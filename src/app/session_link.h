// The GUI's byte link to one terminal session.
//
// LocalPtyLink: the GUI owns the PTY directly (tests, `--local`, and the fallback when no host
// can start). SessionClientLink: the session host owns the PTY and the authoritative
// terminal, and the GUI's terminal is a mirror fed from the host's snapshot and output. Contract:
//   - output arrives on the link's own I/O thread, in order, through SessionEvents::on_output;
//   - write() and resize() may be called from any thread and never block on the program;
//   - on_exit() runs once on the I/O thread after the last output, when the program is gone.
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "pty/pty.h"

namespace kt::app {

class SessionEvents {
public:
    virtual ~SessionEvents() = default;
    virtual void on_output(std::span<const std::byte> bytes) = 0;   // I/O thread
    virtual void on_exit(int exit_code) = 0;                         // I/O thread, once
    // Session links only (I/O thread). A KTSN snapshot replaces the mirror's state; output that
    // follows continues from it.
    virtual void on_snapshot(std::vector<std::byte> /*ktsn*/, bool /*resync*/) {}
    // The host's terminal changed size (stream order: apply before the next output).
    virtual void on_resized(pty::GridSize /*size*/) {}
    virtual void on_cwd(const std::string& /*path*/, const std::string& /*source*/) {}
    // The host went away without EXIT_STATUS (crash, kill -9). The link is dead.
    virtual void on_host_lost(const std::string& /*why*/) {}
};

class ISessionLink {
public:
    virtual ~ISessionLink() = default;
    // Starts the I/O thread(s). `events` must outlive close().
    virtual Result<void> start(SessionEvents& events) = 0;
    // Queues bytes for the program. Any thread.
    virtual void write(std::string_view bytes) = 0;
    virtual void resize(pty::GridSize size) = 0;
    // Ends the session: hangs up the program (killing it after `grace`), stops and joins the I/O
    // thread. on_exit() is not called after close() returns. Idempotent.
    virtual void close(std::chrono::milliseconds grace) = 0;
    // True for a session link: the host answers terminal queries and owns the size, so the GUI's
    // terminal neither replies nor resizes itself (it follows on_resized).
    virtual bool mirror() const { return false; }
    // The session id of a session link, empty otherwise.
    virtual std::string session_id() const { return {}; }
};

// Spawns `spec` in a local PTY.
Result<std::unique_ptr<ISessionLink>> spawn_local_pty(const pty::SpawnSpec& spec);

// The user's shell in the home directory: $SHELL, else the passwd entry, as a login shell on
// POSIX; %COMSPEC% (else cmd.exe) in %USERPROFILE% on Windows. Adds TERM and friends.
pty::SpawnSpec default_shell_spec();

#ifdef KT_HAVE_SESSIONS
// Attaches to the session host at `endpoint` (session_link_client.cpp). The snapshot arrives
// through SessionEvents::on_snapshot after start().
Result<std::unique_ptr<ISessionLink>> attach_session(const std::string& session_id, const std::string& endpoint,
                                                     pty::GridSize size);
#endif

}  // namespace kt::app
