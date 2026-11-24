// AgentPty: the host side of the console agent.
//
// The host creates a single-instance, user-only named pipe with a 128-bit nonce in its name,
// starts `<own exe> --console-agent <pipe> <nonce>` suspended on the hidden desktop, puts it in
// the session job, resumes it, and checks the client PID and the nonce in HELLO. SPAWN carries
// the command line, cwd, environment and size; SPAWNED returns the child's PID. After that the
// pipe carries OUTPUT frames (VT) to the host and INPUT/RESIZE/SIGNAL/SHUTDOWN to the agent.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "pty/agent/protocol.h"
#include "pty/backends_win.h"
#include "pty/util_win.h"

namespace kt::pty::win {
namespace {

using agent::FrameParser;
using agent::make_frame;
using agent::MsgType;
using agent::PayloadReader;
using agent::PayloadWriter;

constexpr DWORD kConnectTimeoutMs = 5000;
constexpr DWORD kSpawnTimeoutMs = 10000;

std::wstring object_name(HANDLE h) {
    wchar_t buf[256];
    DWORD n = 0;
    if (!::GetUserObjectInformationW(h, UOI_NAME, buf, sizeof buf, &n)) return {};
    return buf;
}

// The current window station and desktop, "WinSta0\Default" in an interactive session.
std::wstring own_desktop() {
    const std::wstring ws = object_name(::GetProcessWindowStation());
    const std::wstring desk = object_name(::GetThreadDesktop(::GetCurrentThreadId()));
    if (ws.empty() || desk.empty()) return {};
    return ws + L"\\" + desk;
}

// The shared hidden desktop, opened or created once per process and kept open, so it
// lives as long as any host uses it. Empty when it cannot be created (policy, desktop heap) or
// after a failed agent start on it: the agent then runs with SW_HIDE on the host's desktop.
struct HiddenDesktop {
    std::mutex m;
    HDESK desk = nullptr;
    std::wstring name;
    bool tried = false;
    bool disabled = false;
};

HiddenDesktop& hidden() {
    static HiddenDesktop h;
    return h;
}

std::wstring hidden_desktop() {
    HiddenDesktop& h = hidden();
    std::lock_guard l(h.m);
    if (h.disabled) return {};
    if (h.tried) return h.name;
    h.tried = true;
    const std::wstring ws = object_name(::GetProcessWindowStation());
    if (ws.empty()) return {};
    const std::wstring dn = L"keepterm-bg-" + widen(sid_hash());
    constexpr ACCESS_MASK access = DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                   DESKTOP_HOOKCONTROL | DESKTOP_ENUMERATE | READ_CONTROL;
    HDESK d = ::OpenDesktopW(dn.c_str(), 0, FALSE, access);
    if (!d) {
        UserOnlySecurity sec(false);
        d = ::CreateDesktopW(dn.c_str(), nullptr, nullptr, 0, access, sec.get());
    }
    if (!d) return {};
    h.desk = d;
    h.name = ws + L"\\" + dn;
    return h.name;
}

void disable_hidden_desktop() {
    HiddenDesktop& h = hidden();
    std::lock_guard l(h.m);
    h.disabled = true;
}

std::wstring own_exe() {
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// Synchronous frame I/O for the handshake, with a timeout and the agent's process handle as
// an early-exit signal.
class SyncPipe {
public:
    SyncPipe(HANDLE pipe, HANDLE agent) : pipe_(pipe), agent_(agent), ev_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    Result<agent::Frame> read_frame(FrameParser& parser, DWORD timeout_ms) {
        const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
        char buf[4096];
        for (;;) {
            if (auto f = parser.next()) return std::move(*f);
            if (parser.bad()) return make_error(Errc::corrupt, "agent sent a bad frame");
            DWORD n = 0;
            if (auto r = io(false, buf, sizeof buf, n, deadline); !r) return Unexpected(r.error());
            parser.append(buf, n);
        }
    }

    Result<void> write_all(const std::string& data, DWORD timeout_ms) {
        const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
        DWORD n = 0;
        return io(true, const_cast<char*>(data.data()), static_cast<DWORD>(data.size()), n, deadline);
    }

    Result<void> connect(DWORD timeout_ms) {
        OVERLAPPED ov{};
        ov.hEvent = ev_.get();
        if (::ConnectNamedPipe(pipe_, &ov)) return {};
        const DWORD e = ::GetLastError();
        if (e == ERROR_PIPE_CONNECTED) return {};
        if (e != ERROR_IO_PENDING) return win_error("ConnectNamedPipe", e);
        DWORD n = 0;
        return wait(ov, n, ::GetTickCount64() + timeout_ms, "agent did not connect");
    }

private:
    Result<void> io(bool write, char* buf, DWORD len, DWORD& n, ULONGLONG deadline) {
        OVERLAPPED ov{};
        ov.hEvent = ev_.get();
        const BOOL ok = write ? ::WriteFile(pipe_, buf, len, nullptr, &ov) : ::ReadFile(pipe_, buf, len, nullptr, &ov);
        if (!ok && ::GetLastError() != ERROR_IO_PENDING) return win_error(write ? "agent pipe write" : "agent pipe read");
        return wait(ov, n, deadline, "agent did not answer");
    }

    Result<void> wait(OVERLAPPED& ov, DWORD& n, ULONGLONG deadline, const char* what) {
        const ULONGLONG now = ::GetTickCount64();
        HANDLE hs[2] = {ev_.get(), agent_};
        const DWORD r = ::WaitForMultipleObjects(2, hs, FALSE, now >= deadline ? 0 : static_cast<DWORD>(deadline - now));
        if (r != WAIT_OBJECT_0) {
            ::CancelIoEx(pipe_, &ov);
            ::GetOverlappedResult(pipe_, &ov, &n, TRUE);
            if (r == WAIT_OBJECT_0 + 1) return make_error(Errc::lost, "console agent exited during startup");
            return make_error(Errc::timeout, what);
        }
        if (!::GetOverlappedResult(pipe_, &ov, &n, FALSE)) return win_error("agent pipe");
        return {};
    }

    HANDLE pipe_, agent_;
    Handle ev_;
};

class AgentPty final : public IPty {
public:
    AgentPty(Handle pipe, Handle agent, Handle child, Job job, ContainmentInfo info, FrameParser parser)
        : pipe_(std::move(pipe)),
          agent_(std::move(agent)),
          child_(std::move(child)),
          job_(std::move(job)),
          info_(std::move(info)),
          parser_(std::move(parser)),
          wake_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)),
          read_ev_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        rov_.hEvent = read_ev_.get();
        writer_.attach(pipe_.get());
        rbuf_.resize(64 * 1024);
        handle_frames();  // frames that arrived together with SPAWNED
    }

    ~AgentPty() override {
        close(std::chrono::milliseconds(500));
        if (rpending_) {
            ::CancelIoEx(pipe_.get(), &rov_);
            DWORD n = 0;
            ::GetOverlappedResult(pipe_.get(), &rov_, &n, TRUE);
        }
        writer_.cancel();
    }

    Result<ReadResult> read(std::span<std::byte> buf) override {
        std::lock_guard io(io_mutex_);
        ReadResult rr;
        for (;;) {
            if (out_off_ < out_.size()) {
                const std::size_t n = std::min(buf.size(), out_.size() - out_off_);
                std::memcpy(buf.data(), out_.data() + out_off_, n);
                out_off_ += n;
                if (out_off_ == out_.size()) {
                    out_.clear();
                    out_off_ = 0;
                }
                rr.bytes = n;
                return rr;
            }
            if (eof_ || closing_) {
                rr.eof = true;
                return rr;
            }
            if (!rpending_) start_read();
            if (eof_) {
                on_agent_gone();
                continue;
            }
            HANDLE hs[3] = {wake_.get(), read_ev_.get(), writer_.event()};
            const DWORD n = writer_.pending() ? 3 : 2;
            const DWORD r = ::WaitForMultipleObjects(n, hs, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) {
                rr.woken = true;
                rr.writable = writer_.pump();
                return rr;
            }
            if (r == WAIT_OBJECT_0 + 1) {
                finish_read();
                if (eof_) on_agent_gone();
                continue;
            }
            if (r == WAIT_OBJECT_0 + 2) {
                if (writer_.pump()) {
                    rr.writable = true;
                    return rr;
                }
                continue;
            }
            return win_error("WaitForMultipleObjects");
        }
    }

    Result<std::size_t> write(std::span<const std::byte> bytes) override {
        if (closing_) return make_error(Errc::lost, "pty closed");
        if (writer_.broken()) return make_error(Errc::lost, "agent pipe broken");
        const std::size_t used = writer_.queued();
        if (used + agent::kHeaderSize >= OverlappedWriter::kMaxInFlight) return std::size_t{0};
        const std::size_t n = std::min(bytes.size(), OverlappedWriter::kMaxInFlight - used - agent::kHeaderSize);
        if (n == 0) return std::size_t{0};
        writer_.enqueue(make_frame(MsgType::Input, std::string_view(reinterpret_cast<const char*>(bytes.data()), n)),
                        true);
        return n;
    }

    Result<void> resize(GridSize s) override {
        if (closing_) return make_error(Errc::lost, "pty closed");
        PayloadWriter w;
        w.u16(s.cols).u16(s.rows);
        writer_.enqueue(make_frame(MsgType::Resize, w.data()), true);
        return {};
    }

    Result<void> signal(PtySignal sig) override {
        if (sig == PtySignal::Kill) {
            kill_tree();
            return {};
        }
        PayloadWriter w;
        w.u8(static_cast<uint8_t>(sig));
        writer_.enqueue(make_frame(MsgType::Signal, w.data()), true);
        return {};
    }

    void wake() override { ::SetEvent(wake_.get()); }

    void close(std::chrono::milliseconds grace) override {
        std::lock_guard cl(close_mutex_);
        if (closed_) return;
        closing_ = true;
        ::SetEvent(wake_.get());
        std::lock_guard io(io_mutex_);
        PayloadWriter w;
        w.u32(static_cast<uint32_t>(std::clamp<long long>(grace.count(), 0, 60000)));
        writer_.enqueue(make_frame(MsgType::Shutdown, w.data()), true);
        writer_.flush(200);
        // The agent runs the hangup sequence with this grace and exits; keep its pipe drained.
        if (!drain_until(agent_.get(), ::GetTickCount64() + static_cast<ULONGLONG>(grace.count()) + 1000)) {
            kill_tree();
            ::WaitForSingleObject(agent_.get(), 1000);
        }
        if (::WaitForSingleObject(child_.get(), 1000) != WAIT_OBJECT_0) kill_tree();
        // The agent's conhost ends right after its last client; wait for it briefly, so the job
        // is empty when close() returns.
        if (info_.console_host) {
            Handle ch(::OpenProcess(SYNCHRONIZE, FALSE, *info_.console_host));
            if (ch) ::WaitForSingleObject(ch.get(), 1000);
        }
        // Anything still in the job after the hangup (a conhost that lingers, a grandchild that
        // ignored the close) ends with the session.
        if (job_.handle && info_.kind == ContainmentKind::Job) {
            for (int i = 0; i < 40; ++i) {
                auto ids = job_process_ids(job_.handle.get());
                if (!ids || ids->empty()) break;
                if (i == 10) ::TerminateJobObject(job_.handle.get(), 1);
                ::Sleep(50);
            }
        }
        out_.clear();
        out_off_ = 0;
        closed_ = true;
    }

    std::optional<ExitStatus> exit_status() const override {
        if (::WaitForSingleObject(child_.get(), 0) != WAIT_OBJECT_0) return std::nullopt;
        DWORD code = 0;
        if (!::GetExitCodeProcess(child_.get(), &code)) return std::nullopt;
        return ExitStatus{static_cast<int>(code), std::nullopt};
    }

    NativeWaitable exit_waitable() const override { return child_.get(); }
    BackendKind kind() const override { return BackendKind::ConsoleAgent; }
    BackendCaps caps() const override { return expected_caps(BackendKind::ConsoleAgent); }
    const ContainmentInfo& containment() const override { return info_; }

    Result<std::vector<Pid>> process_ids() const override {
        if (job_.handle && info_.kind == ContainmentKind::Job) return job_process_ids(job_.handle.get());
        std::vector<Pid> ids;
        if (::WaitForSingleObject(agent_.get(), 0) != WAIT_OBJECT_0 && info_.agent_pid) ids.push_back(*info_.agent_pid);
        if (::WaitForSingleObject(child_.get(), 0) != WAIT_OBJECT_0) ids.push_back(info_.root_pid);
        return ids;
    }

    Result<Pid> foreground_pid() const override {
        std::vector<Pid> list;
        {
            std::lock_guard l(fg_mutex_);
            list = foreground_;
        }
        std::vector<Pid> exclude{static_cast<Pid>(::GetCurrentProcessId())};
        if (info_.agent_pid) exclude.push_back(*info_.agent_pid);
        if (info_.console_host) exclude.push_back(*info_.console_host);
        if (Pid p = pick_foreground(list, exclude, info_.root_pid)) return p;
        // The list lags the console by up to 250 ms plus the host's read loop.
        if (::WaitForSingleObject(child_.get(), 0) != WAIT_OBJECT_0) return info_.root_pid;
        return make_error(Errc::lost, "no process in the session");
    }

private:
    void start_read() {
        if (::ReadFile(pipe_.get(), rbuf_.data(), static_cast<DWORD>(rbuf_.size()), nullptr, &rov_) ||
            ::GetLastError() == ERROR_IO_PENDING) {
            rpending_ = true;
        } else {
            eof_ = true;
        }
    }

    void finish_read() {
        DWORD n = 0;
        const BOOL ok = ::GetOverlappedResult(pipe_.get(), &rov_, &n, FALSE);
        if (!ok && ::GetLastError() == ERROR_IO_INCOMPLETE) return;
        rpending_ = false;
        if (n) parser_.append(rbuf_.data(), n);
        handle_frames();
        if (!ok || parser_.bad()) eof_ = true;
    }

    void handle_frames() {
        while (auto f = parser_.next()) {
            switch (f->type) {
                case MsgType::Output: out_ += f->payload; break;
                case MsgType::ChildExit: child_exit_seen_ = true; break;
                case MsgType::Foreground: {
                    PayloadReader r(f->payload);
                    const uint32_t n = r.u32();
                    std::vector<Pid> pids;
                    for (uint32_t i = 0; i < n && r.ok() && i < 4096; ++i) pids.push_back(r.u32());
                    std::lock_guard l(fg_mutex_);
                    foreground_ = std::move(pids);
                    break;
                }
                default: break;
            }
        }
    }

    // The pipe broke. Without CHILD_EXIT the agent crashed: end the child it served. (The
    // full outside hangup sequence needs a host without a console; not yet.)
    void on_agent_gone() {
        if (child_exit_seen_) return;
        if (::WaitForSingleObject(agent_.get(), 2000) == WAIT_OBJECT_0 &&
            ::WaitForSingleObject(child_.get(), 0) != WAIT_OBJECT_0) {
            ::TerminateProcess(child_.get(), 1);
        }
    }

    bool drain_until(HANDLE h, ULONGLONG deadline) {
        for (;;) {
            if (::WaitForSingleObject(h, 0) == WAIT_OBJECT_0) return true;
            const ULONGLONG now = ::GetTickCount64();
            if (now >= deadline) return false;
            if (!rpending_ && !eof_) start_read();
            HANDLE hs[2] = {h, read_ev_.get()};
            const DWORD n = (rpending_ && !eof_) ? 2 : 1;
            const DWORD r = ::WaitForMultipleObjects(n, hs, FALSE, static_cast<DWORD>(deadline - now));
            if (r == WAIT_OBJECT_0) return true;
            if (r == WAIT_OBJECT_0 + 1) {
                finish_read();
                out_.clear();
                out_off_ = 0;
            }
            if (r == WAIT_TIMEOUT) return false;
        }
    }

    void kill_tree() {
        if (job_.handle && info_.kind == ContainmentKind::Job) {
            ::TerminateJobObject(job_.handle.get(), 1);
        } else {
            ::TerminateProcess(child_.get(), 1);
            ::TerminateProcess(agent_.get(), 1);
        }
    }

    Handle pipe_, agent_, child_;
    Job job_;
    ContainmentInfo info_;
    FrameParser parser_;
    Handle wake_, read_ev_;
    OVERLAPPED rov_{};
    std::vector<char> rbuf_;
    bool rpending_ = false;
    std::string out_;
    std::size_t out_off_ = 0;
    OverlappedWriter writer_;
    bool eof_ = false;
    bool child_exit_seen_ = false;
    std::atomic<bool> closing_{false};
    bool closed_ = false;
    mutable std::mutex fg_mutex_;
    std::vector<Pid> foreground_;
    std::mutex io_mutex_, close_mutex_;
};

struct Started {
    Handle pipe, agent;
    Pid agent_pid = 0;
    FrameParser parser;
};

// Agent start: pipe, agent process, job, connection, HELLO.
Result<Started> start_agent(const SpawnSpec& spec, const Job* job, const std::wstring& desktop) {
    const std::string nonce = random_hex(16);
    const std::wstring pipe_name = L"\\\\.\\pipe\\keepterm-" + widen(sid_hash()) + L"-agent-" +
                                   widen(name_part(spec.containment.session_id)) + L"-" + widen(nonce);
    auto pipe = create_pipe_server(pipe_name, PIPE_ACCESS_DUPLEX);
    if (!pipe) return Unexpected(pipe.error());

    const std::wstring exe = own_exe();
    if (exe.empty()) return win_error("GetModuleFileNameW");
    std::wstring cmd = L"\"" + exe + L"\" --console-agent " + pipe_name + L" " + widen(nonce);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::wstring desk = desktop;
    if (!desk.empty()) si.lpDesktop = desk.data();
    PROCESS_INFORMATION pi{};
    // A new console of its own, hidden; on the hidden desktop it is never visible.
    if (!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NEW_CONSOLE | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si,
                          &pi)) {
        return win_error("start console agent");
    }
    Started s;
    s.pipe = std::move(*pipe);
    s.agent.reset(pi.hProcess);
    Handle thread(pi.hThread);
    s.agent_pid = pi.dwProcessId;
    if (job && job->handle) ::AssignProcessToJobObject(job->handle.get(), s.agent.get());
    ::ResumeThread(thread.get());

    auto fail = [&](Unexpected<Error> e) -> Result<Started> {
        ::TerminateProcess(s.agent.get(), 1);
        return e;
    };
    SyncPipe sp(s.pipe.get(), s.agent.get());
    if (auto r = sp.connect(kConnectTimeoutMs); !r) return fail(Unexpected(r.error()));
    ULONG client = 0;
    if (!::GetNamedPipeClientProcessId(s.pipe.get(), &client) || client != s.agent_pid) {
        return fail(make_error(Errc::os_error, "agent pipe: unexpected client"));
    }
    auto hello = sp.read_frame(s.parser, kConnectTimeoutMs);
    if (!hello) return fail(Unexpected(hello.error()));
    PayloadReader r(hello->payload);
    const uint32_t version = r.u32();
    const std::string their_nonce = r.str();
    if (hello->type != MsgType::Hello || !r.ok() || their_nonce != nonce) {
        return fail(make_error(Errc::corrupt, "agent handshake failed"));
    }
    if (version != agent::kProtocolVersion) return fail(make_error(Errc::not_supported, "agent protocol version"));
    return s;
}

}  // namespace

Result<std::unique_ptr<IPty>> spawn_agent(const SpawnSpec& spec) {
    if (spec.argv.empty()) return make_error(Errc::invalid_argument, "empty argv");
    clear_ctrl_c_ignore();
    auto job = create_job(spec.containment.session_id);
    const Job* jp = job ? &*job : nullptr;

    const std::wstring desktop = hidden_desktop();
    auto started = start_agent(spec, jp, desktop);
    if (!started && !desktop.empty()) {
        // retry once on the host's own desktop (SW_HIDE) and stop using the hidden one.
        disable_hidden_desktop();
        started = start_agent(spec, jp, {});
    }
    if (!started) return Unexpected(started.error());
    Started& s = *started;
    SyncPipe sp(s.pipe.get(), s.agent.get());

    const std::wstring env = build_environment(spec.env_edits, false);
    PayloadWriter w;
    w.u16(spec.size.cols).u16(spec.size.rows);
    w.u32(::GetCurrentProcessId()).u64(creation_time(::GetCurrentProcess()));
    w.str(narrow(build_command_line(spec.argv)));
    w.str(spec.cwd);
    w.str(narrow(own_desktop()));
    w.str(std::string_view(reinterpret_cast<const char*>(env.data()), env.size() * sizeof(wchar_t)));
    auto fail = [&](Unexpected<Error> e) -> Result<std::unique_ptr<IPty>> {
        if (jp && jp->handle) ::TerminateJobObject(jp->handle.get(), 1);
        ::TerminateProcess(s.agent.get(), 1);
        return e;
    };
    if (auto r = sp.write_all(make_frame(MsgType::Spawn, w.data()), kConnectTimeoutMs); !r) {
        return fail(Unexpected(r.error()));
    }
    auto spawned = sp.read_frame(s.parser, kSpawnTimeoutMs);
    if (!spawned) return fail(Unexpected(spawned.error()));
    PayloadReader r(spawned->payload);
    const uint32_t err = r.u32();
    const uint32_t child_pid = r.u32();
    const uint64_t child_time = r.u64();
    const uint32_t conhost = r.u32();
    if (spawned->type != MsgType::Spawned || !r.ok()) return fail(make_error(Errc::corrupt, "agent: bad SPAWNED"));
    if (err != 0) {
        ::WaitForSingleObject(s.agent.get(), 2000);
        return fail(win_error("CreateProcessW (agent)", err));
    }
    Handle child(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, child_pid));
    if (!child) child.reset(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child_pid));
    if (!child || creation_time(child.get()) != child_time) {
        return fail(make_error(Errc::lost, "agent child is gone"));
    }

    ContainmentInfo info;
    info.root_pid = child_pid;
    info.root_start_time = child_time;
    info.agent_pid = s.agent_pid;
    if (conhost) info.console_host = conhost;
    Job j;
    if (job) {
        BOOL in_job = FALSE;
        if (::IsProcessInJob(s.agent.get(), job->handle.get(), &in_job) && in_job) {
            info.kind = ContainmentKind::Job;
            info.accuracy = Accuracy::Exact;
            info.handle = job->name;
            j = std::move(*job);
        }
    }
    return std::unique_ptr<IPty>(new AgentPty(std::move(s.pipe), std::move(s.agent), std::move(child), std::move(j),
                                              std::move(info), std::move(s.parser)));
}

}  // namespace kt::pty::win
