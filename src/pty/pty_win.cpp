// Windows PTY backend selection. ConPty: conpty_win.cpp; the console agent:
// agent_pty_win.cpp (host side) and agent/console_agent_win.cpp (the role).

#include "pty/backends_win.h"
#include "pty/pty.h"

namespace kt::pty {

BackendKind resolve_backend(BackendChoice choice, const ExtendedOptions&) {
    switch (choice) {
        case BackendChoice::Agent: return BackendKind::ConsoleAgent;
        case BackendChoice::ConPty: return BackendKind::ConPty;
        default:
            // Feature detection is the primary check: all three functions must resolve.
            // OpenConsole is not built yet, so `openconsole` acts as `auto`.
            return win::conpty_available() ? BackendKind::ConPty : BackendKind::ConsoleAgent;
    }
}

BackendCaps expected_caps(BackendKind k) {
    if (k == BackendKind::ConsoleAgent) return BackendCaps{16, false, false, false};
    return BackendCaps{0, true, true, true};
}

Result<std::unique_ptr<IPty>> spawn(const SpawnSpec& spec, BackendChoice choice, const ExtendedOptions& ext) {
    if (choice == BackendChoice::Posix) return make_error(Errc::not_supported, "POSIX pty on Windows");
    if (spec.argv.empty()) return make_error(Errc::invalid_argument, "empty argv");
    const BackendKind kind = resolve_backend(choice, ext);
    if (kind == BackendKind::ConPty) {
        auto r = win::spawn_conpty(spec);
        // CreatePseudoConsole failing under `auto` falls back to the agent once.
        if (!r && choice != BackendChoice::ConPty && r.error().what.rfind("CreatePseudoConsole", 0) == 0) {
            return win::spawn_agent(spec);
        }
        return r;
    }
    return win::spawn_agent(spec);
}

}  // namespace kt::pty
