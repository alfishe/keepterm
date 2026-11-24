// The console agent role: `keepterm --console-agent <pipe> <nonce>`.
//
// One agent per session. It owns a hidden console, starts the child attached to it, scrapes
// the screen buffer into VT (scraper.h), turns input bytes into INPUT_RECORDs (input_decoder.h)
// and talks to the host over the pipe (protocol.h). Main thread only: a message loop
// (MsgWaitForMultipleObjectsEx) for the WinEvent hook, the pipe, the child, the host process
// and the adaptive scrape timer.
//
// Scrape schedule: a console WinEvent or input/resize starts a burst (probes at
// +1, +2, +4, +8, +16 ms), changes keep it Active (4 ms), 500 ms without change is Idle (16 ms
// probes, a full scrape every 4th), and 30 s without change while WinEvents arrive is Deep idle
// (100 ms probes, a full scrape every 10th). timeBeginPeriod(1) is held only in Burst/Active.

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "pty/agent/input_decoder.h"
#include "pty/agent/protocol.h"
#include "pty/agent/scraper.h"
#include "pty/pty.h"
#include "pty/util_win.h"

#include <timeapi.h>

namespace kt::pty {
namespace {

using namespace kt::pty::agent;
using win::Handle;

constexpr int16_t kMainBufferRows = 3000;
constexpr int kRecycleMargin = 300;
constexpr std::size_t kMaxPendingRecords = 4096;
constexpr std::size_t kRecordsPerWrite = 512;
constexpr ULONGLONG kEscTimeoutMs = 50;

int g_winevents = 0;
bool g_dirty = false;

void CALLBACK on_console_event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    ++g_winevents;
    g_dirty = true;
}

BOOL WINAPI agent_ctrl_handler(DWORD ev) {
    // Ctrl+C and Ctrl+Break are for the programs; the others end the agent.
    return ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT ? TRUE : FALSE;
}

double now_ms() {
    static const double freq = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / freq;
}

// IConsole over the active screen buffer. CONOUT$ is opened per tick, so a buffer switch
// (SetConsoleActiveScreenBuffer, ?1049h) is followed.
class WinConsole final : public IConsole {
public:
    ~WinConsole() override { out_.reset(); }

    void refresh() {
        out_.reset(::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr));
    }
    HANDLE handle() const { return out_.get(); }

    bool info(ConInfo& o) override {
        CONSOLE_SCREEN_BUFFER_INFO i;
        if (!out_ || !::GetConsoleScreenBufferInfo(out_.get(), &i)) return false;
        CONSOLE_CURSOR_INFO ci{};
        if (!::GetConsoleCursorInfo(out_.get(), &ci)) ci.bVisible = TRUE;
        o.buf_w = i.dwSize.X;
        o.buf_h = i.dwSize.Y;
        o.win_left = i.srWindow.Left;
        o.win_top = i.srWindow.Top;
        o.win_w = static_cast<int16_t>(i.srWindow.Right - i.srWindow.Left + 1);
        o.win_h = static_cast<int16_t>(i.srWindow.Bottom - i.srWindow.Top + 1);
        o.cur_x = i.dwCursorPosition.X;
        o.cur_y = i.dwCursorPosition.Y;
        o.cur_visible = ci.bVisible != FALSE;
        return true;
    }

    bool read(int16_t top, int16_t rows, int16_t left, int16_t cols, std::span<ConCell> out) override {
        buf_.resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols));
        SMALL_RECT r{left, top, static_cast<SHORT>(left + cols - 1), static_cast<SHORT>(top + rows - 1)};
        if (!::ReadConsoleOutputW(out_.get(), buf_.data(), COORD{cols, rows}, COORD{0, 0}, &r)) return false;
        for (std::size_t k = 0; k < buf_.size() && k < out.size(); ++k) {
            out[k].ch = static_cast<char16_t>(buf_[k].Char.UnicodeChar);
            out[k].attr = buf_[k].Attributes;
        }
        return true;
    }

    std::u16string title() override {
        wchar_t t[1024];
        const DWORD n = ::GetConsoleTitleW(t, static_cast<DWORD>(std::size(t)));
        return std::u16string(reinterpret_cast<const char16_t*>(t), n);
    }

private:
    Handle out_;
    std::vector<CHAR_INFO> buf_;
};

enum class State { Burst, Active, Idle, Deep };

class Agent {
public:
    int run(const std::wstring& pipe_name, const std::string& nonce);

private:
    bool send(MsgType t, std::string_view payload);
    std::optional<Frame> read_frame_blocking(DWORD timeout_ms);
    void start_read();
    bool pump_pipe();  // false: the pipe broke
    bool handle_frame(const Frame& f);
    void setup_console();
    bool spawn_child(const Frame& spawn);
    void resize(int cols, int rows);
    void resize_buffer(HANDLE h, int cols, int rows, bool tall);
    void fit_font(int cols, int rows);
    void tick(bool force_full);
    void maybe_recycle(bool changed);
    void inject(const std::vector<InputEvent>& events);
    void write_pending_input();
    void check_foreground();
    void enter(State s);
    DWORD timeout_ms() const;
    void hangup(DWORD grace_ms);
    int finish(bool child_exited);

    Handle pipe_, read_ev_, write_ev_;
    OVERLAPPED rov_{};
    std::vector<char> rbuf_ = std::vector<char>(64 * 1024);
    bool rpending_ = false;
    FrameParser parser_;
    bool pipe_ok_ = true;

    HANDLE conin_ = nullptr, conout_main_ = nullptr;
    HWND window_ = nullptr;
    DWORD conhost_pid_ = 0;
    Handle host_, child_;
    DWORD child_pid_ = 0;
    std::array<HWINEVENTHOOK, 2> hooks_{};

    WinConsole con_;
    Scraper scraper_;
    InputDecoder decoder_;
    std::deque<INPUT_RECORD> input_;
    double last_input_ = 0;

    State state_ = State::Active;
    int burst_step_ = 0;
    double next_due_ = 0, last_change_ = 0;
    int idle_probes_ = 0;
    bool high_res_ = false;
    double next_fg_ = 0;
    std::vector<DWORD> foreground_;
    bool done_ = false;
    int exit_code_ = 0;
};

bool Agent::send(MsgType t, std::string_view payload) {
    if (!pipe_ok_) return false;
    const std::string f = make_frame(t, payload);
    OVERLAPPED ov{};
    ov.hEvent = write_ev_.get();
    if (!::WriteFile(pipe_.get(), f.data(), static_cast<DWORD>(f.size()), nullptr, &ov) &&
        ::GetLastError() != ERROR_IO_PENDING) {
        pipe_ok_ = false;
        return false;
    }
    HANDLE hs[2] = {write_ev_.get(), host_.get()};
    const DWORD n = host_ ? 2 : 1;
    const DWORD r = ::WaitForMultipleObjects(n, hs, FALSE, INFINITE);
    DWORD done = 0;
    if (r != WAIT_OBJECT_0) {
        ::CancelIoEx(pipe_.get(), &ov);
        ::GetOverlappedResult(pipe_.get(), &ov, &done, TRUE);
        pipe_ok_ = false;
        return false;
    }
    if (!::GetOverlappedResult(pipe_.get(), &ov, &done, FALSE)) {
        pipe_ok_ = false;
        return false;
    }
    return true;
}

void Agent::start_read() {
    if (rpending_ || !pipe_ok_) return;
    if (::ReadFile(pipe_.get(), rbuf_.data(), static_cast<DWORD>(rbuf_.size()), nullptr, &rov_) ||
        ::GetLastError() == ERROR_IO_PENDING) {
        rpending_ = true;
    } else {
        pipe_ok_ = false;
    }
}

bool Agent::pump_pipe() {
    while (pipe_ok_) {
        start_read();
        if (!rpending_) break;
        DWORD n = 0;
        if (!::GetOverlappedResult(pipe_.get(), &rov_, &n, FALSE)) {
            if (::GetLastError() == ERROR_IO_INCOMPLETE) break;
            rpending_ = false;
            pipe_ok_ = false;
            break;
        }
        rpending_ = false;
        parser_.append(rbuf_.data(), n);
        while (auto f = parser_.next()) {
            if (!handle_frame(*f)) return false;
        }
        if (parser_.bad()) pipe_ok_ = false;
    }
    return pipe_ok_;
}

std::optional<Frame> Agent::read_frame_blocking(DWORD timeout_ms) {
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    for (;;) {
        if (auto f = parser_.next()) return f;
        start_read();
        if (!rpending_) return std::nullopt;
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) return std::nullopt;
        if (::WaitForSingleObject(read_ev_.get(), static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD n = 0;
        const BOOL ok = ::GetOverlappedResult(pipe_.get(), &rov_, &n, FALSE);
        rpending_ = false;
        if (!ok) {
            pipe_ok_ = false;
            return std::nullopt;
        }
        parser_.append(rbuf_.data(), n);
    }
}

void Agent::setup_console() {
    // A console of its own: CREATE_NEW_CONSOLE + SW_HIDE from the host, or AllocConsole when
    // started detached.
    if (!::GetConsoleWindow()) ::AllocConsole();
    window_ = ::GetConsoleWindow();
    if (window_ && ::IsWindowVisible(window_)) ::ShowWindow(window_, SW_HIDE);
    ::SetConsoleCtrlHandler(agent_ctrl_handler, TRUE);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};  // inherited by the child only (handle list)
    conin_ = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, nullptr);
    conout_main_ = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);
    DWORD mode = 0;
    if (::GetConsoleMode(conin_, &mode)) {
        ::SetConsoleMode(conin_, (mode | ENABLE_EXTENDED_FLAGS) & ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE));
    }
    ::SetConsoleTextAttribute(conout_main_, 0x07);
    if (window_) ::GetWindowThreadProcessId(window_, &conhost_pid_);
}

void Agent::fit_font(int cols, int rows) {
    COORD lg = ::GetLargestConsoleWindowSize(conout_main_);
    if (lg.X >= cols && lg.Y >= rows) return;
    // The window can never exceed monitor size / font size: shrink the font. The
    // window is invisible, so this has no visual effect.
    for (const int h : {12, 8, 6, 4, 2}) {
        CONSOLE_FONT_INFOEX fi{};
        fi.cbSize = sizeof fi;
        fi.dwFontSize = COORD{0, static_cast<SHORT>(h)};
        fi.FontFamily = FF_DONTCARE;
        fi.FontWeight = FW_NORMAL;
        wcscpy_s(fi.FaceName, L"Consolas");
        if (!::SetCurrentConsoleFontEx(conout_main_, FALSE, &fi)) continue;
        lg = ::GetLargestConsoleWindowSize(conout_main_);
        if (lg.X >= cols && lg.Y >= rows) return;
    }
}

void Agent::resize_buffer(HANDLE h, int cols, int rows, bool tall) {
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (!::GetConsoleScreenBufferInfo(h, &i)) return;
    const int oldw = i.srWindow.Right - i.srWindow.Left + 1, oldh = i.srWindow.Bottom - i.srWindow.Top + 1;
    const int bufh = tall ? std::max<int>(kMainBufferRows, rows) : rows;
    // Shrink the window to fit both the old and the new buffer, set the buffer, set the window.
    const int w1 = std::min(oldw, cols), h1 = std::min(oldh, rows);
    int top = i.srWindow.Top;
    if (top + h1 > bufh) top = std::max(0, bufh - h1);
    SMALL_RECT r1{0, static_cast<SHORT>(top), static_cast<SHORT>(w1 - 1), static_cast<SHORT>(top + h1 - 1)};
    ::SetConsoleWindowInfo(h, TRUE, &r1);
    ::SetConsoleScreenBufferSize(h, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(bufh)});
    if (!::GetConsoleScreenBufferInfo(h, &i)) return;
    top = i.srWindow.Top;
    const int cy = i.dwCursorPosition.Y;
    if (cy >= top + rows) top = cy - rows + 1;
    if (cy < top) top = cy;
    top = std::clamp(top, 0, std::max(0, i.dwSize.Y - rows));
    SMALL_RECT r2{0, static_cast<SHORT>(top), static_cast<SHORT>(cols - 1), static_cast<SHORT>(top + rows - 1)};
    ::SetConsoleWindowInfo(h, TRUE, &r2);
}

void Agent::resize(int cols, int rows) {
    cols = std::clamp(cols, 2, 1000);
    rows = std::clamp(rows, 1, 500);
    fit_font(cols, rows);
    resize_buffer(conout_main_, cols, rows, true);
    if (scraper_.alternate()) {
        con_.refresh();
        if (con_.handle()) resize_buffer(con_.handle(), cols, rows, false);
    }
    scraper_.resync();  // conhost reflows on a width change: repaint everything
    enter(State::Burst);
}

bool Agent::spawn_child(const Frame& spawn) {
    PayloadReader r(spawn.payload);
    const uint16_t cols = r.u16(), rows = r.u16();
    const uint32_t host_pid = r.u32();
    const uint64_t host_time = r.u64();
    const std::wstring cmd_s = win::widen(r.str());
    const std::wstring cwd = win::widen(r.str());
    std::wstring desktop = win::widen(r.str());
    const std::string env_bytes = r.str();
    PayloadWriter reply;
    if (!r.ok()) {
        reply.u32(ERROR_INVALID_DATA).u32(0).u64(0).u32(0);
        send(MsgType::Spawned, reply.data());
        return false;
    }
    // The host's process: its exit is a hangup. The creation time guards against PID reuse.
    host_.reset(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, host_pid));
    if (host_ && win::creation_time(host_.get()) != host_time) host_.reset();

    // Grid size, a clean 3,000-line main buffer, the cursor at the top.
    resize_buffer(conout_main_, cols, rows, true);
    fit_font(cols, rows);
    resize_buffer(conout_main_, cols, rows, true);
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (::GetConsoleScreenBufferInfo(conout_main_, &i)) {
        DWORD n = 0;
        const DWORD cells = static_cast<DWORD>(i.dwSize.X) * static_cast<DWORD>(i.dwSize.Y);
        ::FillConsoleOutputCharacterW(conout_main_, L' ', cells, COORD{0, 0}, &n);
        ::FillConsoleOutputAttribute(conout_main_, 0x07, cells, COORD{0, 0}, &n);
        ::SetConsoleCursorPosition(conout_main_, COORD{0, 0});
    }

    std::wstring env(env_bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(env.data(), env_bytes.data(), env.size() * sizeof(wchar_t));
    std::wstring cmd = cmd_s;

    // Only the two console handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
    HANDLE inherit[2] = {conin_, conout_main_};
    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    ::InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr,
                                nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = conin_;
    si.StartupInfo.hStdOutput = conout_main_;
    si.StartupInfo.hStdError = conout_main_;
    // GUI programs started by the shell appear on the user's desktop, not the hidden one.
    if (!desktop.empty()) si.StartupInfo.lpDesktop = desktop.data();
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    // No CREATE_NEW_PROCESS_GROUP: that would disable Ctrl+C for the child.
    // Suspended until the console title is set: the console was created for the agent, so its
    // title would be keepterm's own path; a console created for the child is titled with the
    // child's image path, as ConPTY reports it.
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                     env.size() > 1 ? env.data() : nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                                     &si.StartupInfo, &pi);
    const DWORD err = ::GetLastError();
    ::DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        reply.u32(err ? err : ERROR_GEN_FAILURE).u32(0).u64(0).u32(0);
        send(MsgType::Spawned, reply.data());
        return false;
    }
    wchar_t image[MAX_PATH];
    DWORD image_len = MAX_PATH;
    if (::QueryFullProcessImageNameW(pi.hProcess, 0, image, &image_len)) ::SetConsoleTitleW(image);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    child_.reset(pi.hProcess);
    child_pid_ = pi.dwProcessId;
    reply.u32(0).u32(pi.dwProcessId).u64(win::creation_time(pi.hProcess)).u32(conhost_pid_);
    return send(MsgType::Spawned, reply.data());
}

bool Agent::handle_frame(const Frame& f) {
    switch (f.type) {
        case MsgType::Input: {
            std::vector<InputEvent> ev;
            decoder_.feed(f.payload, ev);
            inject(ev);
            last_input_ = now_ms();
            enter(State::Burst);
            return true;
        }
        case MsgType::Resize: {
            PayloadReader r(f.payload);
            const int cols = r.u16(), rows = r.u16();
            if (r.ok()) resize(cols, rows);
            return true;
        }
        case MsgType::Signal: {
            PayloadReader r(f.payload);
            const auto sig = static_cast<PtySignal>(r.u8());
            switch (sig) {
                case PtySignal::Interrupt: ::GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0); break;
                case PtySignal::Break: ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0); break;
                case PtySignal::Hangup: hangup(2000); return false;
                case PtySignal::Terminate:
                case PtySignal::Kill: {
                    std::vector<DWORD> list(256);
                    const DWORD n = ::GetConsoleProcessList(list.data(), static_cast<DWORD>(list.size()));
                    for (DWORD k = 0; k < n && k < list.size(); ++k) {
                        if (list[k] == ::GetCurrentProcessId()) continue;
                        Handle p(::OpenProcess(PROCESS_TERMINATE, FALSE, list[k]));
                        if (p) ::TerminateProcess(p.get(), 1);
                    }
                    break;
                }
            }
            return true;
        }
        case MsgType::Shutdown: {
            PayloadReader r(f.payload);
            const uint32_t grace = r.u32();
            hangup(r.ok() ? grace : 3000);
            return false;
        }
        default: return true;
    }
}

void Agent::inject(const std::vector<InputEvent>& events) {
    for (const InputEvent& e : events) {
        INPUT_RECORD rec{};
        if (e.kind == InputEvent::Kind::Focus) {
            rec.EventType = FOCUS_EVENT;
            rec.Event.FocusEvent.bSetFocus = e.focus ? TRUE : FALSE;
            input_.push_back(rec);
            continue;
        }
        const KeyInput& k = e.key;
        const bool ctrl = (k.mods & (kLeftCtrl | kRightCtrl)) != 0;
        // Ctrl+C: a key event does not raise CTRL_C_EVENT. In processed mode the agent raises
        // it; raw-mode programs read the key. Ctrl+Break is always the event.
        if (k.ch == 0x03 || (k.vk == 'C' && ctrl && !(k.mods & (kLeftAlt | kRightAlt)))) {
            DWORD mode = 0;
            if (::GetConsoleMode(conin_, &mode) && (mode & ENABLE_PROCESSED_INPUT)) {
                if (k.down) {
                    write_pending_input();
                    ::GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
                }
                continue;
            }
        }
        if (k.vk == vk::Cancel && ctrl) {
            if (k.down) {
                write_pending_input();
                ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
            }
            continue;
        }
        uint16_t vkey = k.vk;
        uint32_t mods = k.mods;
        if (vkey == 0) {
            const SHORT r = ::VkKeyScanW(static_cast<wchar_t>(k.ch));
            if (r == -1) {
                vkey = VK_PACKET;
            } else {
                vkey = LOBYTE(r);
                const unsigned hi = HIBYTE(r);
                if ((hi & 6) == 6) {
                    mods |= RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED;  // AltGr
                } else {
                    if (hi & 2) mods |= LEFT_CTRL_PRESSED;
                    if (hi & 4) mods |= LEFT_ALT_PRESSED;
                }
                if (hi & 1) mods |= SHIFT_PRESSED;
            }
        }
        rec.EventType = KEY_EVENT;
        rec.Event.KeyEvent.bKeyDown = k.down ? TRUE : FALSE;
        rec.Event.KeyEvent.wRepeatCount = k.repeat ? k.repeat : 1;
        rec.Event.KeyEvent.wVirtualKeyCode = vkey;
        rec.Event.KeyEvent.wVirtualScanCode =
            k.scan ? k.scan : static_cast<WORD>(vkey == VK_PACKET ? 0 : ::MapVirtualKeyW(vkey, MAPVK_VK_TO_VSC));
        rec.Event.KeyEvent.uChar.UnicodeChar = static_cast<WCHAR>(k.ch);
        rec.Event.KeyEvent.dwControlKeyState = mods;
        input_.push_back(rec);
    }
    write_pending_input();
}

void Agent::write_pending_input() {
    while (!input_.empty()) {
        DWORD queued = 0;
        // A big paste waits while the console input buffer is full.
        if (::GetNumberOfConsoleInputEvents(conin_, &queued) && queued > kMaxPendingRecords) return;
        const std::size_t n = std::min(input_.size(), kRecordsPerWrite);
        std::vector<INPUT_RECORD> chunk(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(n));
        DWORD written = 0;
        if (!::WriteConsoleInputW(conin_, chunk.data(), static_cast<DWORD>(n), &written) || written == 0) return;
        input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(written));
    }
}

void Agent::enter(State s) {
    const double now = now_ms();
    if (s == State::Burst || s == State::Active) {
        if (!high_res_) {
            ::timeBeginPeriod(1);
            high_res_ = true;
        }
    } else if (high_res_) {
        ::timeEndPeriod(1);
        high_res_ = false;
    }
    if (s == State::Burst) {
        burst_step_ = 0;
        next_due_ = now + 1;
    } else if (s == State::Active) {
        next_due_ = now + 4;
    } else if (s == State::Idle) {
        next_due_ = now + 16;
        idle_probes_ = 0;
    } else {
        next_due_ = now + 100;
        idle_probes_ = 0;
    }
    state_ = s;
}

DWORD Agent::timeout_ms() const {
    double t = next_due_ - now_ms();
    if (decoder_.pending()) t = std::min(t, last_input_ + kEscTimeoutMs - now_ms());
    if (!input_.empty()) t = std::min(t, 10.0);
    t = std::min(t, next_fg_ - now_ms());
    return t <= 0 ? 0 : static_cast<DWORD>(t + 0.5);
}

void Agent::maybe_recycle(bool changed) {
    if (changed || scraper_.alternate()) return;
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (!con_.handle() || !::GetConsoleScreenBufferInfo(con_.handle(), &i)) return;
    if (i.srWindow.Bottom < i.dwSize.Y - kRecycleMargin || i.srWindow.Top == 0) return;
    // Move the window rows to the top of the buffer before conhost starts shifting content
    //. The host already shows these rows, so nothing is emitted.
    const SHORT top = i.srWindow.Top, h = static_cast<SHORT>(i.srWindow.Bottom - i.srWindow.Top + 1);
    SMALL_RECT src{0, top, static_cast<SHORT>(i.dwSize.X - 1), static_cast<SHORT>(i.dwSize.Y - 1)};
    CHAR_INFO fill{};
    fill.Char.UnicodeChar = L' ';
    fill.Attributes = 0x07;
    if (!::ScrollConsoleScreenBufferW(con_.handle(), &src, nullptr, COORD{0, 0}, &fill)) return;
    SMALL_RECT win{0, 0, static_cast<SHORT>(i.dwSize.X - 1), static_cast<SHORT>(h - 1)};
    ::SetConsoleWindowInfo(con_.handle(), TRUE, &win);
    const COORD cur{i.dwCursorPosition.X, static_cast<SHORT>(std::max(0, i.dwCursorPosition.Y - top))};
    ::SetConsoleCursorPosition(con_.handle(), cur);
    scraper_.rebase(0);
    CONSOLE_SCREEN_BUFFER_INFO j;
    // A program wrote during the move: its text may sit in the wrong rows. Repaint.
    if (!::GetConsoleScreenBufferInfo(con_.handle(), &j) || j.dwCursorPosition.X != cur.X ||
        j.dwCursorPosition.Y != cur.Y || j.srWindow.Top != 0) {
        scraper_.resync();
    }
}

void Agent::tick(bool force_full) {
    con_.refresh();
    bool full = force_full || g_dirty || state_ == State::Burst || state_ == State::Active;
    if (!full) {
        ++idle_probes_;
        full = idle_probes_ % (state_ == State::Deep ? 10 : 4) == 0 || scraper_.probe(con_);
    }
    g_dirty = false;
    bool changed = false;
    if (full) {
        const std::string vt = scraper_.scrape(con_);
        if (!vt.empty()) {
            changed = true;
            send(MsgType::Output, vt);
        }
    }
    maybe_recycle(changed);
    const double now = now_ms();
    if (changed) last_change_ = now;
    switch (state_) {
        case State::Burst:
            if (++burst_step_ >= 5) {
                enter(State::Active);
            } else {
                next_due_ = now + static_cast<double>(1 << burst_step_);
            }
            break;
        case State::Active:
            if (now - last_change_ > 500) {
                enter(State::Idle);
            } else {
                next_due_ = now + 4;
            }
            break;
        case State::Idle:
        case State::Deep:
            if (changed) {
                enter(State::Active);
            } else if (state_ == State::Idle && g_winevents > 0 && now - last_change_ > 30000) {
                enter(State::Deep);
            } else {
                next_due_ = now + (state_ == State::Deep ? 100 : 16);
            }
            break;
    }
}

void Agent::check_foreground() {
    const double now = now_ms();
    if (now < next_fg_) return;
    next_fg_ = now + 250;
    std::vector<DWORD> list(64);
    DWORD n = ::GetConsoleProcessList(list.data(), static_cast<DWORD>(list.size()));
    if (n > list.size()) {
        list.resize(n + 16);
        n = ::GetConsoleProcessList(list.data(), static_cast<DWORD>(list.size()));
    }
    if (n == 0 || n > list.size()) return;
    list.resize(n);
    std::sort(list.begin(), list.end());
    if (list == foreground_) return;
    foreground_ = list;
    PayloadWriter w;
    w.u32(n);
    for (DWORD p : list) w.u32(p);
    send(MsgType::Foreground, w.data());
}

// Hangup sequence: CTRL_CLOSE to the console's processes through WM_CLOSE on the
// console window, wait up to the grace, then TerminateProcess on those still alive whose
// creation time still matches. Detached processes are not in the list and stay in the job.
void Agent::hangup(DWORD grace_ms) {
    std::vector<DWORD> list(256);
    DWORD n = ::GetConsoleProcessList(list.data(), static_cast<DWORD>(list.size()));
    if (n > list.size()) n = 0;
    struct Rec {
        Handle h;
        uint64_t t;
    };
    std::vector<Rec> procs;
    for (DWORD k = 0; k < n; ++k) {
        if (list[k] == ::GetCurrentProcessId()) continue;
        Handle h(::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, list[k]));
        if (h) {
            const uint64_t t = win::creation_time(h.get());
            procs.push_back({std::move(h), t});
        }
    }
    if (!window_) ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
    // Leave the console first, so the agent itself does not get CTRL_CLOSE_EVENT.
    ::FreeConsole();
    if (window_) ::PostMessageW(window_, WM_CLOSE, 0, 0);
    const ULONGLONG deadline = ::GetTickCount64() + grace_ms;
    for (auto& p : procs) {
        const ULONGLONG now = ::GetTickCount64();
        ::WaitForSingleObject(p.h.get(), now >= deadline ? 0 : static_cast<DWORD>(deadline - now));
    }
    for (auto& p : procs) {
        if (::WaitForSingleObject(p.h.get(), 0) != WAIT_OBJECT_0 && win::creation_time(p.h.get()) == p.t) {
            ::TerminateProcess(p.h.get(), 1);
        }
    }
    done_ = true;
}

int Agent::finish(bool child_exited) {
    if (child_exited) {
        // Final scrape: the last output before the exit reaches the host.
        tick(true);
    }
    if (child_) {
        ::WaitForSingleObject(child_.get(), 2000);
        DWORD code = 1;
        ::GetExitCodeProcess(child_.get(), &code);
        exit_code_ = static_cast<int>(code);
        PayloadWriter w;
        w.u32(code);
        send(MsgType::ChildExit, w.data());
    }
    if (!done_) {
        // Other console processes still attached (a `start /b` job): like a hangup.
        std::vector<DWORD> list(8);
        const DWORD n = ::GetConsoleProcessList(list.data(), static_cast<DWORD>(list.size()));
        if (n > 1) hangup(2000);
    }
    if (high_res_) ::timeEndPeriod(1);
    for (auto h : hooks_) {
        if (h) ::UnhookWinEvent(h);
    }
    ::FlushFileBuffers(pipe_.get());
    pipe_.reset();
    return exit_code_;
}

int Agent::run(const std::wstring& pipe_name, const std::string& nonce) {
    pipe_.reset(::CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr));
    if (!pipe_) return 3;
    read_ev_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    write_ev_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    rov_.hEvent = read_ev_.get();

    // The "ignore Ctrl+C" flag inherited from the host must not reach the child.
    ::SetConsoleCtrlHandler(nullptr, FALSE);
    setup_console();
    DWORD mode = 0;
    const bool vt_ok = ::GetConsoleMode(conout_main_, &mode) &&
                       ::SetConsoleMode(conout_main_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    if (conhost_pid_) {
        hooks_[0] = ::SetWinEventHook(EVENT_CONSOLE_CARET, EVENT_CONSOLE_END_APPLICATION, nullptr, on_console_event,
                                      conhost_pid_, 0, WINEVENT_OUTOFCONTEXT);
        hooks_[1] = ::SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, on_console_event,
                                      conhost_pid_, 0, WINEVENT_OUTOFCONTEXT);
    }

    PayloadWriter hello;
    hello.u32(kProtocolVersion).str(nonce).u32(::GetCurrentProcessId()).u32(::GetConsoleOutputCP());
    hello.u8(vt_ok ? 1 : 0).u8(hooks_[0] ? 1 : 0);
    if (!send(MsgType::Hello, hello.data())) return 3;

    auto spawn = read_frame_blocking(10000);
    if (!spawn || spawn->type != MsgType::Spawn) return 3;
    if (!spawn_child(*spawn)) return 4;
    send(MsgType::Output, Scraper::start());
    enter(State::Burst);

    // Frames that arrived together with SPAWN.
    while (auto f = parser_.next()) {
        if (!handle_frame(*f)) return finish(false);
    }

    for (;;) {
        HANDLE hs[3];
        DWORD n = 0;
        start_read();
        if (rpending_) hs[n++] = read_ev_.get();
        hs[n++] = child_.get();
        if (host_) hs[n++] = host_.get();
        ::MsgWaitForMultipleObjectsEx(n, hs, timeout_ms(), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) ::DispatchMessageW(&msg);

        if (!pump_pipe()) {
            if (!done_) hangup(2000);  // host gone, or SHUTDOWN/Hangup handled
            return finish(false);
        }
        if (host_ && ::WaitForSingleObject(host_.get(), 0) == WAIT_OBJECT_0) {
            pipe_ok_ = false;
            hangup(2000);
            return finish(false);
        }
        if (::WaitForSingleObject(child_.get(), 0) == WAIT_OBJECT_0) return finish(true);

        if (decoder_.pending() && now_ms() - last_input_ >= kEscTimeoutMs) {
            std::vector<InputEvent> ev;
            decoder_.flush(ev);
            inject(ev);
        }
        if (!input_.empty()) write_pending_input();
        if (g_dirty && state_ != State::Burst && state_ != State::Active) enter(State::Active);
        if (g_dirty || now_ms() >= next_due_) tick(false);
        check_foreground();
    }
}

}  // namespace

int run_console_agent(std::u16string_view pipe_name, std::u16string_view nonce) {
    if (pipe_name.empty() || nonce.empty()) return 2;
    const std::wstring pipe(reinterpret_cast<const wchar_t*>(pipe_name.data()), pipe_name.size());
    const std::string n = win::narrow(std::wstring_view(reinterpret_cast<const wchar_t*>(nonce.data()), nonce.size()));
    Agent agent;
    return agent.run(pipe, n);
}

}  // namespace kt::pty
