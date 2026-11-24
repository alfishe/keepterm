#include "session/agent_statusline.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "agents/context_statusline.h"
#include "base/log.h"
#include "config/config.h"
#include "platform/paths.h"
#include "session/agent_context.h"
#include "session/client.h"
#include "session/ids.h"
#include "session/os.h"
#include "session/pane_config.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace kt::session {

namespace {

constexpr std::size_t kMaxInput = 1u << 20;

std::string env(const char* name) {
    auto v = platform::paths::env_utf8(name);
    return v ? *v : std::string();
}

std::string read_stdin() {
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
    std::string data;
    char buf[16384];
    for (;;) {
        const std::size_t n = std::fread(buf, 1, sizeof buf, stdin);
        if (n == 0) break;
        if (data.size() < kMaxInput) data.append(buf, std::min(n, kMaxInput - data.size()));
    }
    return data;
}

config::Config load_config() {
    auto c = config::Config::load(load_options(std::nullopt, {}));
    return c;
}

#ifdef _WIN32
// One argument in the quoting CommandLineToArgvW and the MSVC runtime undo.
std::string quote_arg(std::string_view a) {
    std::string o = "\"";
    std::size_t bs = 0;
    for (char c : a) {
        if (c == '\\') {
            ++bs;
            continue;
        }
        if (c == '"') o.append(bs * 2 + 1, '\\');
        else o.append(bs, '\\');
        bs = 0;
        o += c;
    }
    o.append(bs * 2, '\\');
    return o + "\"";
}
#endif

// Runs `cmd` with `input` on its stdin; its stdout is ours. False when it could not start.
bool run_chained(const std::string& cmd, const std::string& input) {
    std::fflush(stdout);
#ifdef _WIN32
    // Claude Code runs statusLine commands with Git Bash when it has one.
    std::string line;
    const std::string bash = env("CLAUDE_CODE_GIT_BASH_PATH");
    if (!bash.empty()) line = "\"" + quote_arg(bash) + " -c " + quote_arg(cmd) + "\"";
    else line = cmd;
    const std::wstring w = platform::paths::from_utf8(line).wstring();
    FILE* p = ::_wpopen(w.c_str(), L"wb");
    if (!p) return false;
    (void)std::fwrite(input.data(), 1, input.size(), p);
    (void)::_pclose(p);
#else
    std::signal(SIGPIPE, SIG_IGN);   // a command that does not read its stdin
    FILE* p = ::popen(cmd.c_str(), "w");
    if (!p) return false;
    (void)std::fwrite(input.data(), 1, input.size(), p);
    (void)::pclose(p);
#endif
    return true;
}

std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The profile's state folder (os::paths(): KEEPTERM_STATE_DIR, KEEPTERM_PROFILE), else the
// default one when that folder fails its checks.
std::string hook_state_dir() {
    if (auto paths = os::paths()) return paths->state;
    return agents::default_hook_paths({}, {}).state_dir;
}

int hook_usage() {
    std::fprintf(stderr, "usage: keepterm --agent-hook install|uninstall|status [claude]\n");
    return 2;
}

void print_status(const agents::HookStatus& s) {
    std::printf("claude: %s\n", std::string(agents::to_string(s.state)).c_str());
    std::printf("settings: %s\n", s.settings_file.c_str());
    if (!s.command.empty()) std::printf("command: %s\n", s.command.c_str());
    std::printf("chained: %s\n", s.chained ? "yes" : "no");
}

}  // namespace

std::unique_ptr<agents::IStatusLineHook> claude_statusline_hook(std::string_view configured_claude_dir) {
    agents::HookPaths p = agents::default_hook_paths(configured_claude_dir, os::self_exe());
    p.state_dir = hook_state_dir();
    return agents::make_claude_statusline_hook(std::move(p));
}

void statusline_self_check(std::string_view configured_claude_dir) {
    auto hook = claude_statusline_hook(configured_claude_dir);
    auto r = hook->self_check();
    if (!r) {
        log::warn("agents", "Claude Code status line check: " + r.error().what);
        return;
    }
    if (r->state == agents::HookState::Repaired) {
        log::info("agents", "Claude Code status line relay repaired in " + r->settings_file);
    }
}

int agent_statusline_main(int argc, char** argv, int i) {
    if (i < argc && std::string_view(argv[i]) != "claude") return 0;   // other agents: nothing yet
    const std::string input = read_stdin();
    auto report = agents::parse_statusline(input);

    // 1. The pane's host.
    const std::string sid = env("KEEPTERM_SESSION");
    if (report && !sid.empty()) {
        if (auto id = SessionId::parse(sid)) {
            if (auto paths = os::paths()) {
                const auto msg = to_proto(*report, env("KEEPTERM_PANE"));
                (void)send_agent_statusline(os::endpoint_name(*paths, *id), msg, 50);
            }
        }
    }

    // 2. The status line.
    const std::string state_dir = hook_state_dir();
    if (!state_dir.empty()) {
        if (auto cmd = agents::chained_statusline_command(state_dir)) {
            if (run_chained(*cmd, input)) return 0;
        }
    }
    if (!report) return 0;
    const config::Config c = load_config();
    if (c.agents_claude_statusline_default() == "none") return 0;
    report->received_unix_ms = unix_ms();
    const std::string line = agents::compact_statusline(*report);
    if (!line.empty()) std::printf("%s\n", line.c_str());
    return 0;
}

int agent_hook_main(int argc, char** argv, int i) {
    if (i >= argc) return hook_usage();
    const std::string_view action = argv[i++];
    if (i < argc && std::string_view(argv[i]) != "claude") {
        std::fprintf(stderr, "keepterm: --agent-hook supports only claude\n");
        return 2;
    }
    const config::Config c = load_config();
    auto hook = claude_statusline_hook(c.agents_claude_config_dir());
    if (action == "status") {
        print_status(hook->status());
        return 0;
    }
    Result<agents::HookStatus> r = action == "install"     ? hook->install()
                                   : action == "uninstall" ? hook->uninstall()
                                                           : Result<agents::HookStatus>(make_error(Errc::invalid_argument, "bad action"));
    if (action != "install" && action != "uninstall") return hook_usage();
    if (!r) {
        std::fprintf(stderr, "keepterm: %s\n", r.error().what.c_str());
        return 1;
    }
    print_status(*r);
    return 0;
}

}  // namespace kt::session
