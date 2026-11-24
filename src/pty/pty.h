// PTY backends.
//
// Status: PosixPty, ConPty (Windows 1809+) and the console agent (Windows 1607+, or forced with
// BackendChoice::Agent) are implemented; OpenConsole is not. Strings are UTF-8 in std::string.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "resources/containment.h"

namespace kt::pty {

using Pid = uint32_t;
using kt::resources::Accuracy;
using kt::resources::ContainmentKind;

enum class BackendKind : uint8_t { Posix, ConPty, ConsoleAgent, OpenConsole };  // OpenConsole: extended, opt-in
enum class BackendChoice : uint8_t { Auto, Posix, ConPty, Agent, OpenConsole };

struct ExtendedOptions {
    bool openconsole = false;  // extended.openconsole
};

struct GridSize {
    uint16_t cols = 80, rows = 24, px_w = 0, px_h = 0;
};

struct EnvEdit {
    std::string name;
    std::optional<std::string> value;  // nullopt = unset
};

struct ContainmentRequest {
    std::string session_id;
    // POSIX: what the host set up; reported by containment(). None = the backend's
    // default (Tracked on POSIX; Windows always creates its job).
    ContainmentKind kind = ContainmentKind::None;
    int cgroup_procs_fd = -1;   // Linux: the child writes itself into this cgroup.procs before exec
    std::string handle;         // cgroup path relative to the cgroup2 mount
};

struct SpawnSpec {
    std::vector<std::string> argv;  // argv[0] = program, looked up in PATH when it has no '/'
    std::string cwd;                // empty = inherit
    std::vector<EnvEdit> env_edits;
    GridSize size;
    bool login_shell = false;       // POSIX: argv[0] becomes "-<basename>"
    bool inherit_cursor = false;    // ConPTY
    ContainmentRequest containment;
};

enum class PtySignal : uint8_t { Interrupt, Break, Hangup, Terminate, Kill };

struct ExitStatus {
    int code = 0;                // exit code, or 128 + signal on POSIX
    std::optional<int> signal;   // POSIX only
};

struct ContainmentInfo {
    ContainmentKind kind = ContainmentKind::None;
    Accuracy accuracy = Accuracy::BestEffort;
    std::string handle;
    Pid root_pid = 0;
    uint64_t root_start_time = 0;
    std::optional<Pid> agent_pid;
    std::optional<Pid> console_host;
    std::optional<Pid> posix_sid;
    std::optional<uint64_t> tty_dev;
};

struct BackendCaps {
    uint8_t colors = 0;  // 16, 256 or 0 (= true color)
    bool osc_passthrough = true;
    bool mouse_input = true;
    bool exact_wrap = true;
};

struct ReadResult {
    std::size_t bytes = 0;
    bool eof = false;
    bool woken = false;
    bool writable = false;
};

#ifdef _WIN32
using NativeWaitable = void*;  // HANDLE
#else
using NativeWaitable = int;    // fd, -1 = none
#endif

class IPty {
public:
    virtual ~IPty() = default;

    virtual Result<ReadResult> read(std::span<std::byte> buf) = 0;
    virtual Result<std::size_t> write(std::span<const std::byte> bytes) = 0;
    virtual Result<void> resize(GridSize) = 0;
    virtual Result<void> signal(PtySignal) = 0;
    virtual void wake() = 0;
    // Not concurrently with read(): stop the reader (wake() and join) first.
    virtual void close(std::chrono::milliseconds grace = std::chrono::milliseconds(3000)) = 0;

    virtual std::optional<ExitStatus> exit_status() const = 0;
    virtual NativeWaitable exit_waitable() const = 0;

    virtual BackendKind kind() const = 0;
    virtual BackendCaps caps() const = 0;
    virtual const ContainmentInfo& containment() const = 0;
    virtual Result<std::vector<Pid>> process_ids() const = 0;
    virtual Result<Pid> foreground_pid() const = 0;
};

BackendKind resolve_backend(BackendChoice, const ExtendedOptions& = {});
BackendCaps expected_caps(BackendKind);
Result<std::unique_ptr<IPty>> spawn(const SpawnSpec&, BackendChoice = BackendChoice::Auto,
                                    const ExtendedOptions& = {});

#ifdef _WIN32
// Entry point of the console agent role, `keepterm --console-agent <pipe> <nonce>`.
// Started only by AgentPty; returns the child's exit code, or 2-4 for a failed start.
int run_console_agent(std::u16string_view pipe_name, std::u16string_view nonce);
#endif

}  // namespace kt::pty
