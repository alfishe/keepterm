// Windows helpers shared by the ConPTY backend, the agent host side and the agent role.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "base/error.h"
#include "pty/pty.h"

namespace kt::pty::win {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HANDLE get() const { return h_; }
    HANDLE release() {
        HANDLE h = h_;
        h_ = nullptr;
        return h;
    }
    void reset(HANDLE h = nullptr) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) ::CloseHandle(h_);
        h_ = h;
    }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

// Error from a Win32 code; the Errc follows the code (file not found -> unavailable).
Unexpected<Error> win_error(std::string what, DWORD code = ::GetLastError());

// argv -> a command line that CommandLineToArgvW and the CRT split back into argv.
std::wstring build_command_line(const std::vector<std::string>& argv);

// The child's environment block: a fresh user block from CreateEnvironmentBlock
// (the process environment if that fails), KEEPTERM_*/LINES/COLUMNS removed, TERM and
// COLORTERM set for ConPTY and removed for the agent, TERM_PROGRAM, then the caller's edits.
// Sorted, double-NUL terminated.
std::wstring build_environment(const std::vector<EnvEdit>& edits, bool vt_backend);

// Hex of a 64-bit FNV-1a hash of the current user's SID string.
const std::string& sid_hash();
std::string random_hex(std::size_t bytes);

// SECURITY_ATTRIBUTES with a protected DACL: the current user (and SYSTEM if asked), GENERIC_ALL.
class UserOnlySecurity {
public:
    explicit UserOnlySecurity(bool with_system);
    ~UserOnlySecurity();
    UserOnlySecurity(const UserOnlySecurity&) = delete;
    UserOnlySecurity& operator=(const UserOnlySecurity&) = delete;
    SECURITY_ATTRIBUTES* get() { return sd_ ? &sa_ : nullptr; }

private:
    SECURITY_ATTRIBUTES sa_{};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
};

// Session job object: Local\keepterm-<sidhash>-<session>-<nonce>,
// user + SYSTEM DACL, BREAKAWAY_OK, no KILL_ON_JOB_CLOSE.
struct Job {
    Handle handle;
    std::string name;
};
Result<Job> create_job(const std::string& session_id);
Result<std::vector<Pid>> job_process_ids(HANDLE job);

// Session id part of object names: [A-Za-z0-9_-] only, generated when empty.
std::string name_part(const std::string& session_id);

// Single-instance named pipe server end with a user-only DACL, overlapped.
Result<Handle> create_pipe_server(const std::wstring& name, DWORD access);

uint64_t creation_time(HANDLE process);  // FILETIME as 100 ns ticks, 0 on error
uint64_t creation_time_of(Pid pid);

// Foreground rule: drop `exclude`, take the newest by creation time (ties go to
// the larger PID); the root only when nothing else is left. 0 when the list is empty.
Pid pick_foreground(const std::vector<Pid>& pids, const std::vector<Pid>& exclude, Pid root);

// Clears the inherited "ignore Ctrl+C" flag so children get Ctrl+C. Once.
void clear_ctrl_c_ignore();

// One overlapped write at a time from a queue. Thread-safe.
class OverlappedWriter {
public:
    static constexpr std::size_t kMaxInFlight = 64 * 1024;

    OverlappedWriter();
    ~OverlappedWriter();
    OverlappedWriter(const OverlappedWriter&) = delete;
    OverlappedWriter& operator=(const OverlappedWriter&) = delete;

    void attach(HANDLE h) { h_ = h; }
    HANDLE event() const { return ev_.get(); }
    bool pending() const;
    bool broken() const;
    std::size_t queued() const;
    // Queues up to the free space (kMaxInFlight minus queued bytes); `force` takes everything.
    std::size_t enqueue(std::string_view bytes, bool force = false);
    // Finishes a completed write and starts the next. Returns true if a write completed.
    bool pump();
    // Waits up to `ms` for everything queued to be written.
    void flush(DWORD ms);
    // Cancels the pending write and waits for it, before the handle is closed.
    void cancel();

private:
    bool pump_locked();
    mutable std::mutex m_;
    HANDLE h_ = nullptr;
    Handle ev_;
    OVERLAPPED ov_{};
    std::string inflight_, queue_;
    bool pending_ = false, broken_ = false;
};

}  // namespace kt::pty::win
