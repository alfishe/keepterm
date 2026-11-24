// OS services of the session module (system layer): folders, files, processes, detached start.
// Paths are UTF-8. The Windows implementation is os_win.cpp, the POSIX one os_posix.cpp.
//
// The platform layer is the home of spawn_detached and the folder layout; until kt_platform_core
// grows them, the session module carries the small subset it needs here.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"
#include "session/ids.h"
#include "session/journal.h"

namespace kt::session::os {

// ------------------------------------------------------------------------------ folders

struct Paths {
    std::string runtime;          // sockets and registry
    std::string run_sessions;     // <runtime>/sessions: <id>.json, <id>.sock
    std::string state;            // survives a reboot
    std::string state_sessions;   // <state>/sessions/<id>/: journal, checkpoints, record.json
    std::string logs;
};

// The per-user folders, created and checked (0700, owned by the user, not a symlink). Test
// overrides: KEEPTERM_RUNTIME_DIR and KEEPTERM_STATE_DIR. KEEPTERM_PROFILE=<name> selects
// separate folders and endpoints (--profile).
Result<Paths> paths();
std::string profile();

// Session endpoint: a socket path on POSIX, a pipe name on Windows.
std::string endpoint_name(const Paths&, const SessionId&, bool integration = false);
// The running GUI's control endpoint: <runtime>/control.sock, or the user's pipe
// prefix + "control" on Windows.
std::string control_endpoint_name(const Paths&);
// Session ids that have an endpoint (socket file or pipe), integration endpoints skipped.
std::vector<SessionId> list_endpoints(const Paths&);

// ------------------------------------------------------------------------------ files

std::string join(std::string_view a, std::string_view b);
bool exists(const std::string& path);
bool is_dir(const std::string& path);
Result<std::string> read_file(const std::string& path, std::size_t max_bytes = 64u << 20);
// Temp file + fsync + rename; user-only permissions.
Result<void> write_file_atomic(const std::string& path, std::string_view data);
Result<void> make_private_dir(const std::string& path);   // mkdir -p, 0700 / protected DACL
std::vector<std::string> list_dir(const std::string& path);   // names, no "." / ".."
Result<void> remove_file(const std::string& path);
Result<void> rename_file(const std::string& from, const std::string& to);
Result<void> remove_tree(const std::string& path);
Result<void> fsync_dir(const std::string& path);           // POSIX; no-op on Windows
uint64_t unix_ms();
std::string home_dir();

// Journal storage: the file is allocated to `total` bytes up front. The mapped
// store is used unless `force_fallback`, the folder is on a network or FUSE file system, or the
// mapping fails; then the buffered fallback.
Result<std::unique_ptr<journal::JournalStore>> open_journal_store(const std::string& path, std::size_t total,
                                                                  bool force_fallback, std::string* why_fallback);

// ------------------------------------------------------------------------------ processes

uint32_t current_pid();
// Windows: creation FILETIME; Linux: starttime ticks since boot; macOS: start in microseconds
// since the epoch. 0 when the process does not exist or cannot be queried.
uint64_t process_start_time(uint32_t pid);
bool process_matches(uint32_t pid, uint64_t start_time);
// Start-time-checked SIGKILL / TerminateProcess.
Result<void> kill_process(uint32_t pid, uint64_t start_time);

struct BootId {
    std::array<uint8_t, 16> bytes{};
    bool exact = false;   // Linux boot_id: a mismatch is final
};
BootId boot_id();
std::string hex(std::span<const uint8_t> bytes);

std::string self_exe();

// spawn_detached for the session host: `args` is the full argv (args[0] = the
// program); "{ready}" in it is replaced by the inherited ready fd or handle value. Waits up to
// `timeout` for the child's ready line and returns it ("OK <pid>" or "ERR ..."). Without
// "{ready}" in `args` it starts a plain background process and returns at once.
struct Spawned {
    uint32_t pid = 0;
    std::string ready_line;
    bool may_end_with_gui = false;   // Windows: inside a job that forbids breakaway
};
Result<Spawned> spawn_detached(const std::vector<std::string>& args, std::chrono::milliseconds timeout);

// Host side of the ready channel: writes `line` + "\n" and closes the handle. Idempotent.
// False when the line could not be written: the spawner gave up waiting (timeout) and nobody
// will use this host.
bool signal_ready(int64_t handle, std::string_view line);

// Host process setup: no console, stdio to the null device (or to KEEPTERM_HOST_LOG), umask
// 077, SIGPIPE ignored, Linux subreaper (basic containment). Returns true when the
// subreaper is set.
bool host_process_setup();

// Gives the free memory the allocator keeps back to the OS (macOS malloc pressure relief, glibc
// malloc_trim, the Windows CRT heap); a no-op where there is no such call. The host calls it once
// after a flood has ended, so its footprint returns to what it holds.
void release_free_memory();

// Calls `cb` on its own thread when the host should save its history: SIGTERM, SIGHUP, SIGINT
// and SIGQUIT on POSIX (`final` = true; the host then ends), WM_QUERYENDSESSION (`final` =
// false) and WM_ENDSESSION (`final` = true) on Windows. POSIX: call it before any
// other thread starts, it blocks those signals in the calling thread.
void watch_shutdown(std::function<void(bool final)> cb);

std::string getenv_utf8(const char* name);

}  // namespace kt::session::os
