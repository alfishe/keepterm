#include "agents/resume_command.h"

#include <algorithm>

namespace kt::agents {
namespace {

bool has_control(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F;
    });
}

Unexpected<Error> cannot_quote(std::string_view why) {
    return make_error(Errc::invalid_argument, "cannot quote: " + std::string(why));
}

bool posix_plain(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return std::string_view("-_./=:@%+,").find(c) != std::string_view::npos;
}

std::string with_separators(std::string_view path, char from, char to) {
    std::string out(path);
    std::replace(out.begin(), out.end(), from, to);
    return out;
}

Result<std::string> join_quoted(const std::vector<std::string>& words,
                                Result<std::string> (*quote)(std::string_view)) {
    std::string out;
    for (const std::string& w : words) {
        auto q = quote(w);
        if (!q) return Unexpected<Error>(q.error());
        if (!out.empty()) out += ' ';
        out += *q;
    }
    return out;
}

Result<std::string> quote_powershell_arg(std::string_view w) { return quote_powershell(w, true); }

}  // namespace

ShellKind shell_kind(std::string_view image) {
    std::string name = normalize_program_name(image);
    if (!name.empty() && name.front() == '-') name.erase(0, 1);  // login shell
    if (name == "pwsh" || name == "powershell") return ShellKind::PowerShell;
    if (name == "cmd") return ShellKind::Cmd;
    if (name == "bash" || name == "zsh" || name == "fish" || name == "sh" || name == "dash" || name == "ksh") {
        return ShellKind::Posix;
    }
    return ShellKind::Unsupported;
}

Result<std::string> quote_posix(std::string_view word) {
    if (has_control(word)) return cannot_quote("control character");
    if (!word.empty() && std::all_of(word.begin(), word.end(), posix_plain)) return std::string(word);
    std::string out = "'";
    for (char c : word) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

Result<std::string> quote_powershell(std::string_view word, bool native_argument) {
    if (has_control(word)) return cannot_quote("control character");
    if (native_argument) {
        if (word.find('"') != std::string_view::npos) return cannot_quote("double quote in a PowerShell argument");
        if (!word.empty() && word.back() == '\\' && word.find_first_of(" \t") != std::string_view::npos) {
            return cannot_quote("trailing backslash in a PowerShell argument with spaces");
        }
    }
    std::string out = "'";
    for (std::size_t i = 0; i < word.size(); ++i) {
        const char c = word[i];
        if (c == '\'') {
            out += "''";
            continue;
        }
        // U+2018..U+201B (E2 80 98..9B) are single quotes to PowerShell too.
        if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < word.size() &&
            static_cast<unsigned char>(word[i + 1]) == 0x80) {
            const auto third = static_cast<unsigned char>(word[i + 2]);
            if (third >= 0x98 && third <= 0x9B) {
                const std::string_view quote = word.substr(i, 3);
                out += quote;
                out += quote;
                i += 2;
                continue;
            }
        }
        out += c;
    }
    out += '\'';
    return out;
}

Result<std::string> quote_cmd(std::string_view word) {
    if (has_control(word)) return cannot_quote("control character");
    if (word.find_first_of("%!\"") != std::string_view::npos) return cannot_quote("% ! or \" for cmd");
    const bool needs_quotes = word.empty() || word.find_first_of(" &|<>^(),;=") != std::string_view::npos;
    if (!needs_quotes) return std::string(word);
    std::size_t trailing = 0;
    while (trailing < word.size() && word[word.size() - 1 - trailing] == '\\') ++trailing;
    std::string out = "\"";
    out += word;
    out.append(trailing, '\\');
    out += '"';
    return out;
}

std::vector<std::string> resume_words(const AgentResume& record, const AgentDescriptor& d) {
    std::vector<std::string> words;
    words.emplace_back(d.command);
    words.insert(words.end(), record.arguments.begin(), record.arguments.end());
    for (std::string_view a : d.resume_args) {
        words.push_back(a == "{id}" ? record.session_id : std::string(a));
    }
    return words;
}

Result<std::string> resume_command(const AgentResume& record, const AgentDescriptor& d, ShellKind shell,
                                   PathStyle style) {
    if (!d.resumable()) return make_error(Errc::not_supported, "agent " + std::string(d.id) + " cannot resume");
    if (record.directory.empty()) return make_error(Errc::invalid_argument, "empty directory");
    if (record.session_id.empty()) return make_error(Errc::invalid_argument, "empty session id");
    const std::vector<std::string> words = resume_words(record, d);
    const bool windows = style == PathStyle::Windows;

    switch (shell) {
        case ShellKind::Posix: {
            auto dir = quote_posix(windows ? with_separators(record.directory, '\\', '/') : record.directory);
            if (!dir) return dir;
            auto line = join_quoted(words, quote_posix);
            if (!line) return line;
            return "cd " + *dir + " && " + *line;
        }
        case ShellKind::PowerShell: {
            auto dir = quote_powershell(windows ? with_separators(record.directory, '/', '\\') : record.directory,
                                        false);
            if (!dir) return dir;
            auto line = join_quoted(words, quote_powershell_arg);
            if (!line) return line;
            return "if (Set-Location -LiteralPath " + *dir + " -PassThru -ErrorAction SilentlyContinue) { & " +
                   *line + " }";
        }
        case ShellKind::Cmd: {
            const std::string dir = windows ? with_separators(record.directory, '/', '\\') : record.directory;
            if (has_control(dir)) return cannot_quote("control character");
            if (dir.find_first_of("%!\"") != std::string::npos) return cannot_quote("% ! or \" for cmd");
            auto line = join_quoted(words, quote_cmd);
            if (!line) return line;
            return "cd /d \"" + dir + "\" && " + *line;
        }
        case ShellKind::Unsupported:
            break;
    }
    return make_error(Errc::not_supported, "shell is not supported");
}

}  // namespace kt::agents
