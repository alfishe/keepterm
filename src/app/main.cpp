// keepterm entry point: one executable, several roles.
//
//   keepterm [--gui] [--local | --attach ID | --restore ID] [--font-size PT] [--config FILE]
//            [--set KEY=VALUE]... [--renderer NAME] [--allow-software-adapter] [--verbose]
//            [-- <program> [args...]]
//       The terminal window (app/gui_app.h). Its flags may come first; `--config` alone is the
//       config CLI, so a window with --config starts with --gui or another window flag.
//   keepterm --version | --help
//   keepterm --config dump | check | path ...          (config/cli.h)
//   keepterm --session list | kill | new | restore | attach ...   (session/cli.cpp)
//   keepterm --session-host <id> --ready <h> ...      (session/host.h; started by the GUI or the CLI)
//   keepterm --resources [--pid N] [--tree] [--json] ...   (resources/cli.h; without
//       --pid every live session of the user with its totals)
//   keepterm --pty-run [--stats] [--pty=auto|conpty|agent] [--] <program> [args...]
//       Runs a program in a PTY, copies its output to stdout and its exit code out.
//   keepterm --pty-render <out.png> [--size COLSxROWS] [--font-size PT] [--timeout SEC] -- <program> [args...]
//       Runs a program in a PTY and renders its final screen to a PNG (app/pty_render.h).
//   keepterm --probe-renderer auto|d3d11|metal|vulkan|opengl|software ...   (render/probe)
//   keepterm --console-agent <pipe> <nonce>     (Windows; started by the host)
//   keepterm --agent-statusline claude           (Claude Code's statusLine command; session/agent_statusline.h)
//   keepterm --agent-hook install | uninstall | status [claude]   (agents/context_hook.h)
//   keepterm --agent-limits [claude]   runs the agent's limits probe once (agents/limits_probe.h)
//   keepterm status | exit | restart | project | tab | pane | window | quick-terminal | session | agent | layout |
//            completion | help   (app/control.h, app/cli_commands.cpp)

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#include "base/log.h"
#include "config/cli.h"
#include "pty/pty.h"
#include "agents/limits_probe.h"
#include "session/agent_statusline.h"
#include "session/ids.h"
#include "session/host.h"
#include "session/spawn.h"
#include "vt/parser.h"

#ifdef KT_HAVE_RESOURCES
#include "app/resource_sessions.h"
#include "resources/cli.h"
#endif
#ifdef KT_HAVE_STORE
#include <filesystem>

#include "platform/paths.h"
#include "session/history_index.h"  // keepterm --history list | show | restore | ...
#include "session/os.h"
#include "store/cli.h"   // keepterm --history du | search | forget | backfill
#endif
#ifdef KT_HAVE_PTY_RENDER
#include "app/pty_render.h"
#endif
#ifdef KT_HAVE_GUI
#include "app/control.h"   // keepterm status | pane ...
#include "app/gui_app.h"
#endif
#ifdef KT_HAVE_RENDER_PROBE
#include "render/probe/probe_command.h"   // keepterm --probe-renderer <kind>
#endif
#ifdef KT_HAVE_THEME
#include "theme/cli.h"   // keepterm --theme list | show, --font list
#endif
#ifdef KT_HAVE_COMMANDS
#include "commands/cli.h"   // keepterm --command list | run | keymap
#endif

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char* kVersion = KT_VERSION;

struct Stats final : kt::vt::ParserHandler {
    std::size_t printed = 0, controls = 0, csi = 0, esc = 0, osc = 0, dcs = 0;
    void print_ascii(std::string_view run) override { printed += run.size(); }
    void print(char32_t) override { ++printed; }
    void execute(uint8_t) override { ++controls; }
    void esc_dispatch(const kt::vt::Sequence&) override { ++esc; }
    void csi_dispatch(const kt::vt::Sequence&) override { ++csi; }
    void osc_dispatch(std::string_view) override { ++osc; }
    void dcs_hook(const kt::vt::Sequence&) override { ++dcs; }
};

int usage(FILE* out = stderr) {
#ifdef KT_HAVE_GUI
    std::fputs(kt::app::gui_usage(), out);
    std::fputs("       (window flags may come first; with --config, start with --gui)\n", out);
#endif
#ifdef KT_HAVE_GUI
    std::fputs("       keepterm --version | --help\n", out);
#else
    std::fputs("usage: keepterm --version | --help\n", out);
#endif
    std::fprintf(out,
                 "%s",
                 ""
                 "       keepterm --config dump [--defaults] [--no-origins] [--profile NAME] [--file PATH]"
                 " [--set KEY=VALUE]...\n"
                 "       keepterm --config check [PATH] [--profile NAME] | --config path [--profile NAME]\n"
                 "       keepterm --session list [--json] | kill <id> | attach <id> ...\n"
                 "       keepterm --session new | restore <id> [--cwd DIR] [--size COLSxROWS] [--no-history]"
                 " [--pty=NAME] [--host-opt OPT]... [-- <program> [args...]]\n"
                 "       keepterm --session-host <id> --ready <handle> [--restore-from ID] [--cwd DIR]"
                 " [--size COLSxROWS]\n"
                 "                [--no-history] [--config FILE] [--set KEY=VALUE]... [--pty=NAME]"
                 " [--scrollback N] [--ring BYTES]\n"
                 "                [--checkpoint-bytes N] [--linger MS] [--journal-fallback]"
                 " [-- <program> [args...]]\n"
#ifdef KT_HAVE_RESOURCES
                 "       keepterm --resources [--pid N [--containment job:<name>|cgroup:<path>]] [--tree]"
                 " [--json] [--no-cpu] [--cmdline] [--stats]\n"
                 "                (without --pid: every live session of this user with its totals)\n"
#endif
                 "       keepterm --pty-run [--stats] [--pty=auto|conpty|agent] [--] <program> [args...]\n"
                 "       keepterm --agent-hook install | uninstall | status [claude]\n"
                 "       keepterm --agent-limits [claude]\n"
                 "       keepterm status | exit | restart | project | tab | pane | window | quick-terminal | session | agent | layout | completion | mcp"
                 "   (keepterm help)\n"
                 "       keepterm --agent-statusline claude   (Claude Code's statusLine command)\n"
#ifdef KT_HAVE_PTY_RENDER
                 "       keepterm --pty-render <out.png> [--size COLSxROWS] [--font-size PT] [--timeout SEC]"
                 " -- <program> [args...]\n"
#endif
#ifdef KT_HAVE_RENDER_PROBE
                 "       keepterm --probe-renderer auto|d3d11|metal|vulkan|opengl|software"
                 " [--allow-software-adapter] [--offscreen] [--show-ms N]\n"
#endif
#ifdef _WIN32
                 "       keepterm --console-agent <pipe> <nonce>   (internal: started by the session host)\n"
#endif
    );
#ifdef KT_HAVE_THEME
    std::fputs(kt::theme::cli_usage(), out);
#endif
#ifdef KT_HAVE_COMMANDS
    std::fputs(kt::commands::command_cli_usage(), out);
#endif
    return out == stderr ? 2 : 0;
}

#ifdef KT_HAVE_GUI
// The GUI role starts with one of its own flags (`--config` alone is the config CLI).
bool is_gui_flag(std::string_view a) {
    for (std::string_view f : {"--gui", "--font-size", "--", "--local", "--clean", "--attach", "--restore", "--set", "--renderer",
                               "--allow-software-adapter", "--verbose", "--after-pid"}) {
        if (a == f) return true;
    }
    return false;
}
#endif

int pty_run(int argc, char** argv, int i) {
    bool stats = false;
    kt::pty::BackendChoice choice = kt::pty::BackendChoice::Auto;
    for (; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--stats") {
            stats = true;
        } else if (a.starts_with("--pty=")) {
            const std::string_view v = a.substr(6);
            if (v == "auto") {
                choice = kt::pty::BackendChoice::Auto;
            } else if (v == "conpty") {
                choice = kt::pty::BackendChoice::ConPty;
            } else if (v == "agent") {
                choice = kt::pty::BackendChoice::Agent;
            } else {
                return usage();
            }
        } else if (a == "--") {
            ++i;
            break;
        } else {
            break;
        }
    }
    if (i >= argc) return usage();

    kt::pty::SpawnSpec spec;
    for (; i < argc; ++i) spec.argv.emplace_back(argv[i]);
    auto id = kt::session::SessionId::generate();
    if (id) {
        spec.env_edits.push_back({"KEEPTERM_SESSION", id->str()});
        spec.containment.session_id = id->str();
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // the VT stream must not get CR LF translation
#endif

    auto pty = kt::pty::spawn(spec, choice);
    if (!pty) {
        std::fprintf(stderr, "keepterm: %s\n", pty.error().what.c_str());
        return 127;
    }

    kt::vt::Parser parser;
    Stats counts;
    std::vector<std::byte> buf(64 * 1024);
    for (;;) {
        auto r = (*pty)->read(buf);
        if (!r) {
            std::fprintf(stderr, "keepterm: %s\n", r.error().what.c_str());
            return 1;
        }
        if (r->bytes) {
            std::fwrite(buf.data(), 1, r->bytes, stdout);
            std::fflush(stdout);
            if (stats) parser.feed(std::span(buf.data(), r->bytes), counts);
        }
        if (r->eof) break;
    }
    (*pty)->close();
    const auto st = (*pty)->exit_status();
    if (stats) {
        std::fprintf(stderr, "stats: printed=%zu controls=%zu csi=%zu esc=%zu osc=%zu dcs=%zu\n", counts.printed,
                     counts.controls, counts.csi, counts.esc, counts.osc, counts.dcs);
    }
    return st ? st->code : 1;
}

#ifdef _WIN32
// The narrow argv of the CRT is in the ANSI code page (the manifest's UTF-8 code page needs 1903
// and is not relied on), so non-ASCII arguments would turn into '?'. Rebuild it as UTF-8 from the
// UTF-16 command line; the strings live for the whole process.
char** utf8_argv(int& argc, char** fallback) {
    int n = 0;
    LPWSTR* w = ::CommandLineToArgvW(::GetCommandLineW(), &n);
    if (!w) return fallback;
    static std::vector<std::string> store;
    static std::vector<char*> ptrs;
    store.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int len = static_cast<int>(std::wcslen(w[i]));
        std::string& s = store[static_cast<std::size_t>(i)];
        const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, w[i], len, nullptr, 0, nullptr, nullptr);
        s.resize(static_cast<std::size_t>(bytes > 0 ? bytes : 0));
        if (bytes > 0) ::WideCharToMultiByte(CP_UTF8, 0, w[i], len, s.data(), bytes, nullptr, nullptr);
    }
    ::LocalFree(w);
    for (std::string& s : store) ptrs.push_back(s.data());
    ptrs.push_back(nullptr);
    argc = n;
    return ptrs.data();
}
#endif

}  // namespace

// keepterm --agent-limits [agent]: the account's current limits from the agent's own probe.
static int agent_limits(const char* agent) {
    const kt::agents::LimitsProbe* p = kt::agents::limits_probe_for(agent);
    if (!p) {
        std::fprintf(stderr, "keepterm: %s has no limits probe\n", agent);
        return 2;
    }
    auto r = kt::agents::run_limits_probe(*p, std::chrono::seconds(45));
    if (!r) {
        std::fprintf(stderr, "keepterm: %s\n", r.error().what.c_str());
        return 1;
    }
    for (const auto& l : *r)
        std::printf("%s %.1f%% window=%um resets=%lld\n", l.name.c_str(), l.used_percent, l.window_minutes, static_cast<long long>(l.resets_at_unix));
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    argv = utf8_argv(argc, argv);
#endif
#ifdef KT_HAVE_GUI
    if (argc < 2 || is_gui_flag(argv[1])) {
#ifdef _WIN32
        // keepterm.exe is a console-subsystem image (B1). Started from Explorer it gets a
        // console of its own: detach from it so no empty console window stays next to the GUI.
        // A console shared with a parent shell (more than one process attached) is kept.
        DWORD procs[2];
        if (::GetConsoleProcessList(procs, 2) == 1) ::FreeConsole();
#endif
        return kt::app::gui_main(argc, argv, argc >= 2 && std::string_view(argv[1]) == "--gui" ? 2 : 1);
    }
#endif
    if (argc < 2) return usage();
    const std::string_view cmd = argv[1];
    // First: Claude Code runs the relay on every status update.
    if (cmd == "--agent-statusline") return kt::session::agent_statusline_main(argc, argv, 2);
    if (cmd == "--agent-hook") return kt::session::agent_hook_main(argc, argv, 2);
    if (cmd == "--agent-limits") return agent_limits(argc >= 3 ? argv[2] : "claude");
    if (cmd == "--help" || cmd == "-h") return usage(stdout);
    if (cmd == "--version") {
        std::printf("keepterm %s\n", kVersion);
        return 0;
    }
    if (cmd == "--pty-run") return pty_run(argc, argv, 2);
    if (cmd == "--config") return kt::config::run_cli(argc, argv, 2);  // dump | check | path
    if (cmd == "--session-host") return kt::session::host_main(argc, argv, 1);
    if (cmd == "--session") return kt::session::cli_main(argc, argv, 1);
#ifdef KT_HAVE_RESOURCES
    if (cmd == "--resources") return kt::resources::resources_cli(argc, argv, 2, kt::app::live_session_processes);
#endif
#ifdef KT_HAVE_STORE
    if (cmd == "--history") {
        // The sessions of the history; the rest works on the store itself.
        if (argc > 2 && kt::session::is_history_index_command(argv[2])) return kt::session::history_index_cli(argc, argv, 2);
        // The store the hosts of this profile write (KEEPTERM_STATE_DIR, KEEPTERM_PROFILE).
        std::filesystem::path root;
        if (auto p = kt::session::os::paths()) root = kt::platform::paths::from_utf8(p->state) / "history";
        // Retention per project needs the manifests (the session layer); --root runs without.
        auto per_session = [](const kt::config::Config& cfg) -> kt::store::Janitor::PolicyFor {
            auto p = kt::session::os::paths();
            return p ? kt::session::history_policy_for(cfg, *p) : kt::store::Janitor::PolicyFor{};
        };
        return kt::store::history_cli(argc, argv, 2, root, per_session);
    }
#endif
#ifdef _WIN32
    if (cmd == "--console-agent") {
        if (argc != 4) return usage();
        // The pipe name and the nonce are ASCII (generated by the host).
        const std::string_view pipe = argv[2], nonce = argv[3];
        return kt::pty::run_console_agent(std::u16string(pipe.begin(), pipe.end()),
                                          std::u16string(nonce.begin(), nonce.end()));
    }
#endif
#ifdef KT_HAVE_PTY_RENDER
    if (cmd == "--pty-render") return kt::app::pty_render(argc, argv, 2);
#endif
#ifdef KT_HAVE_RENDER_PROBE
    if (cmd == "--probe-renderer") return kt::render::run_probe_command(argc, argv);
#endif
#ifdef KT_HAVE_THEME
    if (cmd == "--theme") return kt::theme::theme_cli(argc, argv, 2);
    if (cmd == "--font") return kt::theme::font_cli(argc, argv, 2);
#endif
#ifdef KT_HAVE_COMMANDS
    // `command run` goes to the running window; list and keymap need no app.
    if (cmd == "--command" || cmd == "command") {
        kt::commands::RunForwarder forward;
#ifdef KT_HAVE_GUI
        forward = [](const kt::commands::RunRequest& r) {
            return kt::app::control::cli_command_run(r.id, r.words, r.pane, r.force, r.json);
        };
#endif
        return kt::commands::command_cli(argc, argv, 2, forward);
    }
#endif
#ifdef KT_HAVE_GUI
    // The control CLI to the running window (app/control.h).
    for (const char* noun : {"status", "pane", "exit", "restart", "project", "tab", "window", "quick-terminal", "session", "agent", "layout",
                             "completion", "mcp", "help"})
        if (cmd == noun) return kt::app::control::cli_main(argc, argv, 1);
#endif
    return usage();
}
