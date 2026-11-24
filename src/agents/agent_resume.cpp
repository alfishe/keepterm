#include "agents/agent_resume.h"

#include <algorithm>
#include <array>

namespace kt::agents {
namespace {

using namespace std::string_view_literals;

constexpr std::array kInterpreters{"node"sv, "bun"sv, "deno"sv, "python"sv, "python3"sv, "py"sv,
                                   "pythonw"sv};
constexpr std::array kLauncherExtensions{".exe"sv, ".cmd"sv, ".bat"sv, ".com"sv, ".ps1"sv};
constexpr std::array kScriptExtensions{".js"sv, ".mjs"sv, ".cjs"sv, ".ts"sv, ".py"sv};
// Interpreter options after which no script follows (inline code, a module, stdin).
constexpr std::array kNoScriptOptions{"-e"sv, "--eval"sv, "-p"sv, "--print"sv, "-c"sv, "-m"sv, "-"sv};
// Interpreter options whose value is the next word.
constexpr std::array kInterpreterValueOptions{"-r"sv, "--require"sv, "--import"sv, "--loader"sv,
                                              "--experimental-loader"sv, "--title"sv,
                                              "--env-file"sv, "--inspect-port"sv, "-C"sv,
                                              "--conditions"sv, "-W"sv, "-X"sv};
constexpr std::array kRedactedOptions{"--append-system-prompt"sv, "--system-prompt"sv};

template <class Range>
bool contains(const Range& r, std::string_view w) {
    return std::find(std::begin(r), std::end(r), w) != std::end(r);
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    const std::string_view tail = s.substr(s.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (ascii_lower(tail[i]) != ascii_lower(suffix[i])) return false;
    }
    return true;
}

std::string_view basename(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

template <class Range>
std::string_view strip_extension(std::string_view name, const Range& extensions) {
    for (std::string_view ext : extensions) {
        if (name.size() > ext.size() && ends_with_ci(name, ext)) return name.substr(0, name.size() - ext.size());
    }
    return name;
}

std::string_view program_word(std::string_view argv0) {
    if (!argv0.empty() && argv0.front() == '-') argv0.remove_prefix(1);  // login shell
    return strip_extension(basename(argv0), kLauncherExtensions);
}

bool argv0_is_interpreter(std::span<const std::string> argv) {
    return !argv.empty() && is_interpreter_name(lower(program_word(argv[0])));
}

// Index of the script word of an interpreter argv, skipping interpreter options.
std::optional<std::size_t> script_index(std::span<const std::string> argv) {
    const std::string interpreter = lower(program_word(argv[0]));
    const bool has_run_subcommand = interpreter == "deno" || interpreter == "bun";
    bool run_seen = false;
    std::size_t i = 1;
    while (i < argv.size()) {
        const std::string& w = argv[i];
        if (w == "--") return i + 1 < argv.size() ? std::optional<std::size_t>(i + 1) : std::nullopt;
        if (w.size() > 1 && w.front() == '-') {
            if (contains(kNoScriptOptions, w)) return std::nullopt;
            i += contains(kInterpreterValueOptions, w) ? std::size_t{2} : std::size_t{1};
            continue;
        }
        if (w == "-") return std::nullopt;
        if (has_run_subcommand && !run_seen && w == "run") {
            run_seen = true;
            ++i;
            continue;
        }
        return i;
    }
    return std::nullopt;
}

bool name_matches(std::string_view name, std::span<const std::string_view> names) {
    for (std::string_view n : names) {
        if (name == n) return true;
        if (name.size() > n.size() + 1 && name.substr(0, n.size()) == n && name[n.size()] == '-') return true;
    }
    return false;
}

bool is_option(std::string_view w) { return !w.empty() && w.front() == '-'; }

}  // namespace

bool is_interpreter_name(std::string_view name) { return contains(kInterpreters, lower(name)); }

bool looks_like_version(std::string_view name) {
    if (name.empty() || name.find('.') == std::string_view::npos) return false;
    if (name.front() < '0' || name.front() > '9' || name.back() < '0' || name.back() > '9') return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
}

std::string normalize_program_name(std::string_view name) {
    return lower(strip_extension(basename(name), kLauncherExtensions));
}

std::optional<std::string> invoked_name(std::span<const std::string> argv) {
    if (argv.empty() || argv[0].empty()) return std::nullopt;
    const std::string_view program = program_word(argv[0]);
    if (is_interpreter_name(program)) {
        if (auto idx = script_index(argv)) {
            const std::string_view script = basename(argv[*idx]);
            if (!script.empty()) return std::string(strip_extension(script, kScriptExtensions));
        }
    }
    return std::string(program);
}

std::optional<std::string> script_path(std::span<const std::string> argv) {
    if (!argv0_is_interpreter(argv)) return std::nullopt;
    auto idx = script_index(argv);
    if (!idx) return std::nullopt;
    return argv[*idx];
}

std::vector<std::string> program_arguments(std::span<const std::string> argv) {
    if (argv.empty()) return {};
    if (argv0_is_interpreter(argv)) {
        auto idx = script_index(argv);
        if (!idx) return {};
        return {argv.begin() + static_cast<std::ptrdiff_t>(*idx) + 1, argv.end()};
    }
    return {argv.begin() + 1, argv.end()};
}

Recognition recognize(const ProcessFacts& facts, const AgentRegistry& registry) {
    const bool argv_known = !facts.argv.empty();
    std::vector<std::string> names;
    if (argv_known) {
        if (auto n = invoked_name(facts.argv)) names.push_back(lower(*n));
    }
    if (!facts.image.empty()) names.push_back(normalize_program_name(facts.image));
    if (!facts.comm.empty()) names.push_back(lower(facts.comm));

    for (const AgentDescriptor& d : registry.all()) {
        for (const std::string& name : names) {
            if (!name.empty() && name_matches(name, d.names)) return {&d, Match::Yes};
        }
    }

    const std::string image = normalize_program_name(facts.image);
    const std::string comm = lower(facts.comm);
    const bool interpreter = is_interpreter_name(image) || is_interpreter_name(comm) ||
                             (argv_known && argv0_is_interpreter(facts.argv));
    if (!argv_known) {
        // A native agent whose process title is a version (Claude Code) or an interpreter: only
        // the command line can tell.
        if (interpreter || looks_like_version(image) || looks_like_version(comm)) return {nullptr, Match::NeedsCommandLine};
        return {};
    }
    if (interpreter) {
        if (auto script = script_path(facts.argv)) {
            std::string path = lower(*script);
            std::replace(path.begin(), path.end(), '\\', '/');
            for (const AgentDescriptor& d : registry.all()) {
                for (std::string_view marker : d.package_markers) {
                    if (path.find(marker) != std::string::npos) return {&d, Match::Yes};
                }
            }
        }
    }
    return {};
}

bool is_interactive(const AgentDescriptor& d, std::span<const std::string> program_args) {
    for (const std::string& w : program_args) {
        if (contains(d.one_shot, w)) return false;
    }
    // A first word that is not an option is a subcommand or a prompt. A prompt still opens an
    // interactive session.
    if (!program_args.empty() && !is_option(program_args.front())) return !contains(d.subcommands, program_args.front());
    return true;
}

std::vector<std::string> kept_arguments(const AgentDescriptor& d, std::span<const std::string> argv) {
    const std::vector<std::string> rest = program_arguments(argv);
    std::vector<std::string> kept;
    std::size_t i = 0;
    while (i < rest.size()) {
        const std::string& word = rest[i];
        const std::string_view name = std::string_view(word).substr(0, word.find('='));
        if (contains(d.selectors_with_value, word)) {
            // A bare --resume opens a picker: skip the value only when there is one.
            const bool has_value = i + 1 < rest.size() && !is_option(rest[i + 1]);
            i += has_value ? std::size_t{2} : std::size_t{1};
            continue;
        }
        if ((word.find('=') != std::string::npos && contains(d.selectors_with_value, name)) ||
            contains(d.bare_selectors, word)) {
            ++i;
            continue;
        }
        if (contains(d.options_with_value, word) && i + 1 < rest.size()) {
            kept.push_back(word);
            kept.push_back(rest[i + 1]);
            i += 2;
            continue;
        }
        if (!is_option(word)) {  // a prompt word: a resume must not send it again
            ++i;
            continue;
        }
        kept.push_back(word);
        ++i;
    }
    return kept;
}

std::vector<std::string> redact_for_log(std::span<const std::string> arguments) {
    std::vector<std::string> out;
    out.reserve(arguments.size());
    bool redact_next = false;
    for (const std::string& w : arguments) {
        if (redact_next) {
            out.emplace_back("<redacted>");
            redact_next = false;
            continue;
        }
        const auto eq = w.find('=');
        if (eq != std::string::npos && contains(kRedactedOptions, std::string_view(w).substr(0, eq))) {
            out.push_back(w.substr(0, eq + 1) + "<redacted>");
            continue;
        }
        redact_next = contains(kRedactedOptions, w);
        out.push_back(w);
    }
    return out;
}

}  // namespace kt::agents
