#include "session/cwd.h"

#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <system_error>

namespace kt::session {
namespace {

namespace fs = std::filesystem;

fs::path to_path(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string from_path(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_drive_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// [^<>:"|?*] (cmd) or [^<>"|?*] (PowerShell), plus no control characters.
bool path_chars_ok(std::string_view s, bool allow_colon) {
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20) return false;
        switch (c) {
            case '<': case '>': case '"': case '|': case '?': case '*': return false;
            case ':': if (!allow_colon) return false; break;
            default: break;
        }
    }
    return true;
}

// "C:\x\" -> "C:\x"; a drive root keeps its backslash.
std::string trim_backslash(std::string_view p) {
    std::string out(p);
    while (out.size() > 3 && out.back() == '\\') out.pop_back();
    if (out.size() == 2 && out[1] == ':') out.push_back('\\');
    return out;
}

}  // namespace

std::string_view to_string(CwdSource s) {
    switch (s) {
        case CwdSource::None: return "none";
        case CwdSource::Integration: return "integration";
        case CwdSource::OsQuery: return "os_query";
        case CwdSource::ProcessPeb: return "process_peb";
        case CwdSource::PromptParse: return "prompt_parse";
        case CwdSource::LastKnown: return "last_known";
    }
    return "none";
}

std::optional<CwdSource> parse_cwd_source(std::string_view s) {
    for (CwdSource v : {CwdSource::None, CwdSource::Integration, CwdSource::OsQuery, CwdSource::ProcessPeb,
                        CwdSource::PromptParse, CwdSource::LastKnown}) {
        if (to_string(v) == s) return v;
    }
    return std::nullopt;
}

std::optional<std::string> cwd_from_osc7(std::string_view uri, std::string* host) {
    constexpr std::string_view kScheme = "file://";
    if (uri.size() < kScheme.size()) return std::nullopt;
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(uri[i])) != kScheme[i]) return std::nullopt;
    }
    const std::string_view rest = uri.substr(kScheme.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const std::string_view raw_host = rest.substr(0, slash);
    std::string_view raw_path = rest.substr(slash);
    // Query or fragment parts are not part of the path.
    if (const std::size_t q = raw_path.find_first_of("?#"); q != std::string_view::npos) raw_path = raw_path.substr(0, q);

    std::string path;
    path.reserve(raw_path.size());
    for (std::size_t i = 0; i < raw_path.size(); ++i) {
        const char c = raw_path[i];
        if (c == '%') {
            if (i + 2 >= raw_path.size()) return std::nullopt;
            const int hi = hex_value(raw_path[i + 1]), lo = hex_value(raw_path[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            path.push_back(static_cast<char>(hi * 16 + lo));
            i += 2;
        } else {
            path.push_back(c);
        }
    }
    for (char c : path) {
        if (c == '\0') return std::nullopt;
    }
#ifdef _WIN32
    if (path.size() >= 3 && path[0] == '/' && is_drive_letter(path[1]) && path[2] == ':') path.erase(0, 1);
    for (char& c : path) {
        if (c == '/') c = '\\';
    }
    if (path.size() >= 2 && path[1] == ':') path = trim_backslash(path);
#else
    while (path.size() > 1 && path.back() == '/') path.pop_back();
#endif
    if (host) *host = std::string(raw_host);
    return path;
}

std::optional<std::string> cwd_from_osc9_9(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    if (value.empty()) return std::nullopt;
    for (char c : value) {
        if (c == '\0') return std::nullopt;
    }
    if (value.size() >= 2 && value[1] == ':' && is_drive_letter(value[0])) return trim_backslash(value);
    return std::string(value);
}

std::optional<std::string> parse_prompt_cwd(std::string_view t) {
    // PowerShell: ^PS ([A-Za-z]:\\[^<>"|?*]*|\\\\[^<>"|?*]+)> ?$
    if (t.starts_with("PS ")) {
        std::string_view body = t.substr(3);
        if (body.ends_with(' ')) body.remove_suffix(1);
        if (!body.ends_with('>')) return std::nullopt;
        body.remove_suffix(1);
        if (body.size() >= 3 && is_drive_letter(body[0]) && body[1] == ':' && body[2] == '\\') {
            if (!path_chars_ok(body.substr(3), true)) return std::nullopt;
            return trim_backslash(body);
        }
        if (body.size() > 2 && body.starts_with("\\\\")) {
            if (!path_chars_ok(body.substr(2), true)) return std::nullopt;
            return std::string(body);
        }
        return std::nullopt;
    }
    // cmd: ^([A-Za-z]:\\[^<>:"|?*]*)>$
    if (t.size() >= 4 && is_drive_letter(t[0]) && t[1] == ':' && t[2] == '\\' && t.back() == '>') {
        const std::string_view body = t.substr(0, t.size() - 1);
        if (!path_chars_ok(body.substr(3), false)) return std::nullopt;
        return trim_backslash(body);
    }
    return std::nullopt;
}

bool directory_exists(const std::string& utf8_path) {
    if (utf8_path.empty()) return false;
    std::error_code ec;
    return fs::is_directory(to_path(utf8_path), ec);
}

std::string nearest_existing_dir(const std::string& path) {
    if (!path.empty()) {
        std::error_code ec;
        fs::path p = to_path(path).lexically_normal();
        for (int guard = 0; guard < 4096 && !p.empty(); ++guard) {
            if (fs::is_directory(p, ec)) {
                // The bare file system root is a last resort only when it was asked for.
                if (p != p.root_path() || p == to_path(path).lexically_normal()) return from_path(p);
                break;
            }
            fs::path parent = p.parent_path();
            if (parent == p) break;
            p = std::move(parent);
        }
    }
    return home_dir();
}

void CwdTracker::set_initial(std::string dir) {
    if (dir.empty()) return;
    if (source_ == CwdSource::None || source_ == CwdSource::LastKnown) {
        cwd_ = std::move(dir);
        source_ = CwdSource::LastKnown;
    }
}

bool CwdTracker::report(CwdSource source, std::string path) {
    if (path.empty() || source == CwdSource::None) return false;
    switch (source) {
        case CwdSource::Integration:
            integration_ = true;
            break;
        case CwdSource::OsQuery:
        case CwdSource::ProcessPeb:
        case CwdSource::PromptParse:
            if (integration_) return false;
            break;
        case CwdSource::LastKnown:
            if (source_ != CwdSource::None && source_ != CwdSource::LastKnown) return false;
            break;
        case CwdSource::None:
            return false;
    }
    if (path == cwd_ && source == source_) return false;
    cwd_ = std::move(path);
    source_ = source;
    return true;
}

}  // namespace kt::session
