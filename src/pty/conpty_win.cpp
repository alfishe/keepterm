// ConPty backend: Windows 10 1809 / Server 2019 and later.
//
// The pseudo console functions are resolved at run time (nothing newer than
// 14393 is imported). The host ends of the two pipes are overlapped, so read() waits on the
// output, the wake event, the pending write and the child's process handle at once. The
// child exit is noticed on the process handle; output is drained until it is quiet for 100 ms
// (at most 2 s), then a closer thread calls ClosePseudoConsole and the read sees EOF.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "pty/backends_win.h"
#include "pty/util_win.h"

#include <tlhelp32.h>

namespace kt::pty::win {
namespace {

// The SDK declares these only for NTDDI_WIN10_RS5; the baseline headers are RS1.
using Hpcon = void*;
using PfnCreatePseudoConsole = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, Hpcon*);
using PfnResizePseudoConsole = HRESULT(WINAPI*)(Hpcon, COORD);
using PfnClosePseudoConsole = void(WINAPI*)(Hpcon);
constexpr DWORD_PTR kAttrPseudoConsole = 0x00020016;  // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
constexpr DWORD kInheritCursor = 0x1;                 // PSEUDOCONSOLE_INHERIT_CURSOR

struct ConPtyApi {
    PfnCreatePseudoConsole create = nullptr;
    PfnResizePseudoConsole resize = nullptr;
    PfnClosePseudoConsole close = nullptr;
};

const ConPtyApi* api() {
    static const ConPtyApi a = [] {
        ConPtyApi r;
        if (HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll")) {
            r.create = reinterpret_cast<PfnCreatePseudoConsole>(
                reinterpret_cast<void*>(::GetProcAddress(k32, "CreatePseudoConsole")));
            r.resize = reinterpret_cast<PfnResizePseudoConsole>(
                reinterpret_cast<void*>(::GetProcAddress(k32, "ResizePseudoConsole")));
            r.close = reinterpret_cast<PfnClosePseudoConsole>(
                reinterpret_cast<void*>(::GetProcAddress(k32, "ClosePseudoConsole")));
        }
        return r;
    }();
    return a.create && a.resize && a.close ? &a : nullptr;
}

COORD to_coord(GridSize s) {
    return COORD{static_cast<SHORT>(std::clamp<int>(s.cols, 1, 32766)),
                 static_cast<SHORT>(std::clamp<int>(s.rows, 1, 32766))};
}

uint64_t now_filetime() {
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    return (uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime;
}

// The newest conhost.exe child of this process created at or after `since`.
std::optional<Pid> find_conhost(uint64_t since) {
    Handle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return std::nullopt;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    std::optional<Pid> best;
    uint64_t best_t = 0;
    const DWORD self = ::GetCurrentProcessId();
    for (BOOL ok = ::Process32FirstW(snap.get(), &pe); ok; ok = ::Process32NextW(snap.get(), &pe)) {
        if (pe.th32ParentProcessID != self || _wcsicmp(pe.szExeFile, L"conhost.exe") != 0) continue;
        const uint64_t t = creation_time_of(pe.th32ProcessID);
        if (t + 10'000'000 < since || t < best_t) continue;  // 1 s slack for clock granularity
        best = pe.th32ProcessID;
        best_t = t;
    }
    return best;
}

// Console subsystem check from the PE header of the image (the fallback check), cached per path.
bool is_console_image(Pid pid) {
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return false;
    wchar_t path[MAX_PATH * 2];
    DWORD n = static_cast<DWORD>(std::size(path));
    if (!::QueryFullProcessImageNameW(h.get(), 0, path, &n)) return false;
    static std::mutex m;
    static std::map<std::wstring, bool> cache;
    std::lock_guard l(m);
    if (auto it = cache.find(path); it != cache.end()) return it->second;
    bool console = false;
    Handle f(::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr));
    if (f) {
        unsigned char buf[4096];
        DWORD got = 0;
        if (::ReadFile(f.get(), buf, sizeof buf, &got, nullptr) && got >= 0x40) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf);
            const auto off = static_cast<DWORD>(dos->e_lfanew);
            // Signature (4) + file header (20) + Subsystem at offset 68 of the optional header.
            if (dos->e_magic == IMAGE_DOS_SIGNATURE && off + 4 + 20 + 70 <= got) {
                WORD subsystem = 0;
                std::memcpy(&subsystem, buf + off + 4 + 20 + 68, sizeof subsystem);
                console = subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI;
            }
        }
    }
    cache.emplace(path, console);
    return console;
}

BOOL WINAPI ignore_all_ctrl(DWORD) { return TRUE; }

// The only code that attaches this process to another console.
std::mutex& attach_mutex() {
    static std::mutex m;
    return m;
}

class ConPty final : public IPty {
public:
    ConPty(const ConPtyApi* a, Hpcon hpc, Handle in_w, Handle out_r, Handle proc, Job job, ContainmentInfo info)
        : api_(a),
          hpc_(hpc),
          in_w_(std::move(in_w)),
          out_r_(std::move(out_r)),
          proc_(std::move(proc)),
          job_(std::move(job)),
          info_(std::move(info)),
          wake_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)),
          read_ev_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          closer_done_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        rov_.hEvent = read_ev_.get();
        writer_.attach(in_w_.get());
        rbuf_.resize(64 * 1024);
    }

    ~ConPty() override {
        close(std::chrono::milliseconds(500));
        if (rpending_) {
            ::CancelIoEx(out_r_.get(), &rov_);
            DWORD n = 0;
            ::GetOverlappedResult(out_r_.get(), &rov_, &n, TRUE);
        }
        writer_.cancel();
    }

    Result<ReadResult> read(std::span<std::byte> buf) override {
        std::lock_guard io(io_mutex_);
        ReadResult rr;
        for (;;) {
            if (data_off_ < data_.size()) {
                const std::size_t n = std::min(buf.size(), data_.size() - data_off_);
                std::memcpy(buf.data(), data_.data() + data_off_, n);
                data_off_ += n;
                if (data_off_ == data_.size()) {
                    data_.clear();
                    data_off_ = 0;
                }
                rr.bytes = n;
                return rr;
            }
            if (eof_ || closing_) {
                rr.eof = true;
                return rr;
            }
            if (!rpending_) start_read();
            if (eof_) continue;

            HANDLE hs[4];
            DWORD n = 0;
            hs[n++] = wake_.get();
            const DWORD i_read = n;
            hs[n++] = read_ev_.get();
            DWORD i_write = MAXDWORD, i_proc = MAXDWORD;
            if (writer_.pending()) {
                i_write = n;
                hs[n++] = writer_.event();
            }
            if (!exit_seen_) {
                i_proc = n;
                hs[n++] = proc_.get();
            }
            DWORD timeout = INFINITE;
            if (exit_seen_ && !closer_started_) {
                const ULONGLONG since = ::GetTickCount64() - exit_tick_;
                if (since >= 2000) {
                    start_closer();
                    continue;
                }
                timeout = static_cast<DWORD>(std::min<ULONGLONG>(100, 2000 - since));
            }
            const DWORD r = ::WaitForMultipleObjects(n, hs, FALSE, timeout);
            if (r == WAIT_TIMEOUT) {
                start_closer();  // the child exited and the output is quiet
                continue;
            }
            if (r >= WAIT_OBJECT_0 + n) return win_error("WaitForMultipleObjects");
            const DWORD idx = r - WAIT_OBJECT_0;
            if (idx == 0) {
                rr.woken = true;
                rr.writable = writer_.pump();
                return rr;
            }
            if (idx == i_read) {
                finish_read();
                continue;
            }
            if (idx == i_write) {
                if (writer_.pump()) {
                    rr.writable = true;
                    return rr;
                }
                continue;
            }
            if (idx == i_proc) {
                exit_seen_ = true;
                exit_tick_ = ::GetTickCount64();
            }
        }
    }

    Result<std::size_t> write(std::span<const std::byte> bytes) override {
        if (closing_) return make_error(Errc::lost, "pty closed");
        if (writer_.broken()) return make_error(Errc::lost, "input pipe broken");
        return writer_.enqueue(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }

    Result<void> resize(GridSize s) override {
        if (closing_ || closer_started_) return make_error(Errc::lost, "pty closed");
        std::lock_guard l(size_mutex_);
        const COORD c = to_coord(s);
        const HRESULT hr = api_->resize(hpc_, c);
        if (FAILED(hr)) return make_error(Errc::os_error, "ResizePseudoConsole", hr);
        if (!connected_) early_size_ = c;
        return {};
    }

    Result<void> signal(PtySignal sig) override {
        switch (sig) {
            case PtySignal::Interrupt:
                writer_.enqueue("\x03", true);  // conhost raises CTRL_C_EVENT in processed mode
                return {};
            case PtySignal::Break: return send_break();
            case PtySignal::Hangup:
                start_closer();  // ClosePseudoConsole sends CTRL_CLOSE_EVENT
                return {};
            case PtySignal::Terminate:
            case PtySignal::Kill: terminate_tree(); return {};
        }
        return {};
    }

    void wake() override { ::SetEvent(wake_.get()); }

    void close(std::chrono::milliseconds grace) override {
        std::lock_guard cl(close_mutex_);
        if (closed_) return;
        closing_ = true;
        ::SetEvent(wake_.get());
        std::lock_guard io(io_mutex_);  // read() returns eof now and leaves the pipe to us
        writer_.flush(100);
        start_closer();
        // ClosePseudoConsole may block until its output is read (1809): drain while waiting.
        const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(grace.count());
        if (!drain_until(proc_.get(), deadline)) {
            terminate_tree();
            drain_until(proc_.get(), ::GetTickCount64() + 1000);
        }
        if (!drain_until(closer_done_.get(), ::GetTickCount64() + 5000)) {
            // ClosePseudoConsole hangs: end the console host too and leave the closer thread.
            if (info_.console_host) {
                Handle ch(::OpenProcess(PROCESS_TERMINATE, FALSE, *info_.console_host));
                if (ch) ::TerminateProcess(ch.get(), 1);
            }
        }
        data_.clear();
        data_off_ = 0;
        closed_ = true;
    }

    std::optional<ExitStatus> exit_status() const override {
        if (::WaitForSingleObject(proc_.get(), 0) != WAIT_OBJECT_0) return std::nullopt;
        DWORD code = 0;
        if (!::GetExitCodeProcess(proc_.get(), &code)) return std::nullopt;
        return ExitStatus{static_cast<int>(code), std::nullopt};
    }

    NativeWaitable exit_waitable() const override { return proc_.get(); }
    BackendKind kind() const override { return BackendKind::ConPty; }
    BackendCaps caps() const override { return expected_caps(BackendKind::ConPty); }
    const ContainmentInfo& containment() const override { return info_; }

    Result<std::vector<Pid>> process_ids() const override {
        if (job_.handle && info_.kind == ContainmentKind::Job) return job_process_ids(job_.handle.get());
        if (::WaitForSingleObject(proc_.get(), 0) == WAIT_OBJECT_0) return std::vector<Pid>{};
        return std::vector<Pid>{info_.root_pid};
    }

    Result<Pid> foreground_pid() const override {
        std::vector<Pid> exclude{static_cast<Pid>(::GetCurrentProcessId())};
        if (info_.console_host) exclude.push_back(*info_.console_host);
        // Primary: the console's own process list. Only possible while this process has no
        // console of its own (a detached session host).
        if (::GetConsoleWindow() == nullptr && !closer_started_) {
            std::lock_guard l(attach_mutex());
            if (::AttachConsole(info_.root_pid)) {
                ::SetConsoleCtrlHandler(ignore_all_ctrl, TRUE);
                std::vector<DWORD> list(64);
                DWORD got = ::GetConsoleProcessList(list.data(), static_cast<DWORD>(list.size()));
                if (got > list.size()) {
                    list.resize(got + 16);
                    got = ::GetConsoleProcessList(list.data(), static_cast<DWORD>(list.size()));
                }
                ::FreeConsole();
                ::SetConsoleCtrlHandler(ignore_all_ctrl, FALSE);
                if (got > 0 && got <= list.size()) {
                    const std::vector<Pid> pids(list.begin(), list.begin() + got);
                    if (Pid p = pick_foreground(pids, exclude, info_.root_pid)) return p;
                }
            }
        }
        // Fallback (best effort): the newest console program in the job.
        auto ids = process_ids();
        if (!ids) return Unexpected(ids.error());
        std::vector<Pid> console_ids;
        for (Pid p : *ids) {
            if (is_console_image(p)) console_ids.push_back(p);
        }
        if (Pid p = pick_foreground(console_ids, exclude, info_.root_pid)) return p;
        return make_error(Errc::lost, "no process in the session");
    }

private:
    void start_read() {
        if (::ReadFile(out_r_.get(), rbuf_.data(), static_cast<DWORD>(rbuf_.size()), nullptr, &rov_) ||
            ::GetLastError() == ERROR_IO_PENDING) {
            rpending_ = true;
        } else {
            eof_ = true;  // ERROR_BROKEN_PIPE: the pseudo console is closed
        }
    }

    void finish_read() {
        DWORD n = 0;
        const BOOL ok = ::GetOverlappedResult(out_r_.get(), &rov_, &n, FALSE);
        if (!ok && ::GetLastError() == ERROR_IO_INCOMPLETE) return;
        rpending_ = false;
        if (!ok) {
            eof_ = true;
            return;
        }
        if (n == 0) return;
        if (!connected_) note_early_output(std::string_view(reinterpret_cast<const char*>(rbuf_.data()), n));
        data_.append(reinterpret_cast<const char*>(rbuf_.data()), n);
        track_modes(std::string_view(data_).substr(data_.size() - n));
    }

    // A resize that reaches conhost before the child has connected to it can be lost for good
    // (Windows 10 22H2 on a loaded machine: the child kept the CreatePseudoConsole size, with
    // output, for as long as it ran). Output means a client is connected, and
    // conhost's paint with the client's title (OSC 0) comes after the connection, so the last
    // size set before that title is sent again on the first output (with inherit_cursor that can
    // be the DSR request alone) and on the first title. Each resend makes conhost repaint, even
    // for the same size, so there is no periodic retry.
    void note_early_output(std::string_view chunk) {
        const std::string s = tail_ + std::string(chunk);
        const bool title = s.find("\x1b]0;") != std::string::npos || s.find("\x1b]2;") != std::string::npos;
        std::lock_guard l(size_mutex_);
        if (early_size_ && !closer_started_ && (!early_resent_ || title)) (void)api_->resize(hpc_, *early_size_);
        early_resent_ = true;
        if (title) {
            connected_ = true;
            early_size_.reset();
        }
    }

    // Follows win32-input-mode so Break can be sent as a key.
    void track_modes(std::string_view chunk) {
        std::string s = tail_ + std::string(chunk);
        for (std::size_t p = s.find("\x1b[?9001"); p != std::string::npos; p = s.find("\x1b[?9001", p + 1)) {
            if (p + 7 < s.size()) win32_input_ = s[p + 7] == 'h' ? true : (s[p + 7] == 'l' ? false : win32_input_.load());
        }
        tail_ = s.size() > 8 ? s.substr(s.size() - 8) : s;
    }

    // Reads and discards output until `h` is signaled or the deadline passes. True if signaled.
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
                data_.clear();
                data_off_ = 0;
            }
            if (r == WAIT_TIMEOUT) return false;
        }
    }

    void start_closer() {
        std::lock_guard l(closer_mutex_);
        if (closer_started_) return;
        closer_started_ = true;
        HANDLE done = nullptr;
        ::DuplicateHandle(::GetCurrentProcess(), closer_done_.get(), ::GetCurrentProcess(), &done, 0, FALSE,
                          DUPLICATE_SAME_ACCESS);
        // Detached: if ClosePseudoConsole never returns, the object can still go away.
        std::thread([fn = api_->close, hpc = hpc_, done] {
            fn(hpc);
            if (done) {
                ::SetEvent(done);
                ::CloseHandle(done);
            }
        }).detach();
    }

    void terminate_tree() {
        if (job_.handle && info_.kind == ContainmentKind::Job) {
            ::TerminateJobObject(job_.handle.get(), 1);
        } else {
            ::TerminateProcess(proc_.get(), 1);
        }
    }

    Result<void> send_break() {
        if (win32_input_) {
            // Ctrl+Break as win32-input-mode key events: VK_CANCEL, scan 0x46, Ctrl, enhanced.
            writer_.enqueue("\x1b[3;70;0;1;264;1_\x1b[3;70;0;0;264;1_", true);
            return {};
        }
        if (::GetConsoleWindow() != nullptr) {
            return make_error(Errc::not_supported, "Ctrl+Break needs win32-input-mode or a detached host");
        }
        std::lock_guard l(attach_mutex());
        if (!::AttachConsole(info_.root_pid)) return win_error("AttachConsole");
        ::SetConsoleCtrlHandler(ignore_all_ctrl, TRUE);
        const BOOL ok = ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
        const DWORD err = ::GetLastError();
        ::FreeConsole();
        ::SetConsoleCtrlHandler(ignore_all_ctrl, FALSE);
        if (!ok) return win_error("GenerateConsoleCtrlEvent", err);
        return {};
    }

    const ConPtyApi* api_;
    Hpcon hpc_;
    Handle in_w_, out_r_, proc_;
    Job job_;
    ContainmentInfo info_;
    Handle wake_, read_ev_, closer_done_;
    OVERLAPPED rov_{};
    std::vector<std::byte> rbuf_;
    bool rpending_ = false;
    std::string data_;
    std::size_t data_off_ = 0;
    OverlappedWriter writer_;
    std::string tail_;
    std::atomic<bool> win32_input_{false};
    std::mutex size_mutex_;
    std::optional<COORD> early_size_;     // under size_mutex_
    bool early_resent_ = false;           // under size_mutex_
    std::atomic<bool> connected_{false};  // conhost painted the client's title
    bool exit_seen_ = false;
    ULONGLONG exit_tick_ = 0;
    bool eof_ = false;
    std::atomic<bool> closing_{false}, closer_started_{false};
    bool closed_ = false;
    std::mutex io_mutex_, close_mutex_, closer_mutex_;
};

}  // namespace

bool conpty_available() { return api() != nullptr; }

Result<std::unique_ptr<IPty>> spawn_conpty(const SpawnSpec& spec) {
    const ConPtyApi* a = api();
    if (!a) return make_error(Errc::not_supported, "ConPTY is not available on this Windows build");
    if (spec.argv.empty()) return make_error(Errc::invalid_argument, "empty argv");
    clear_ctrl_c_ignore();

    auto job = create_job(spec.containment.session_id);
    const std::wstring base = L"\\\\.\\pipe\\keepterm-" + widen(sid_hash()) + L"-conpty-" +
                              widen(name_part(spec.containment.session_id)) + L"-" + widen(random_hex(16));
    auto in_w = create_pipe_server(base + L"-in", PIPE_ACCESS_OUTBOUND);
    if (!in_w) return Unexpected(in_w.error());
    auto out_r = create_pipe_server(base + L"-out", PIPE_ACCESS_INBOUND);
    if (!out_r) return Unexpected(out_r.error());
    // ConPTY's ends are synchronous handles.
    Handle in_r(::CreateFileW((base + L"-in").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!in_r) return win_error("open ConPTY input pipe");
    Handle out_w(::CreateFileW((base + L"-out").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!out_w) return win_error("open ConPTY output pipe");

    const uint64_t t0 = now_filetime();
    Hpcon hpc = nullptr;
    const HRESULT hr =
        a->create(to_coord(spec.size), in_r.get(), out_w.get(), spec.inherit_cursor ? kInheritCursor : 0, &hpc);
    if (FAILED(hr)) return make_error(Errc::os_error, "CreatePseudoConsole", hr);
    in_r.reset();  // ConPTY holds its own duplicates; ours would prevent EOF
    out_w.reset();

    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !::UpdateProcThreadAttribute(attrs, 0, kAttrPseudoConsole, hpc, sizeof(Hpcon), nullptr, nullptr)) {
        auto e = win_error("UpdateProcThreadAttribute");
        a->close(hpc);
        return e;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;  // all three null: never the host's handles
    si.lpAttributeList = attrs;
    std::wstring cmd = build_command_line(spec.argv);
    std::wstring env = build_environment(spec.env_edits, true);
    const std::wstring cwd = widen(spec.cwd);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                     EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                     env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    const DWORD err = ::GetLastError();
    ::DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        a->close(hpc);
        return win_error("CreateProcessW", err);
    }
    Handle proc(pi.hProcess), thread(pi.hThread);

    ContainmentInfo info;
    info.root_pid = pi.dwProcessId;
    info.root_start_time = creation_time(proc.get());
    Job j;
    if (job && ::AssignProcessToJobObject(job->handle.get(), proc.get())) {
        info.kind = ContainmentKind::Job;
        info.accuracy = Accuracy::Exact;
        info.handle = job->name;
        j = std::move(*job);
    }
    ::ResumeThread(thread.get());
    info.console_host = find_conhost(t0);

    return std::unique_ptr<IPty>(
        new ConPty(a, hpc, std::move(*in_w), std::move(*out_r), std::move(proc), std::move(j), std::move(info)));
}

}  // namespace kt::pty::win
