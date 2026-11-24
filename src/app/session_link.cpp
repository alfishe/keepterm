#include "app/session_link.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "session/ids.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace kt::app {

namespace {

class LocalPtyLink final : public ISessionLink {
public:
    explicit LocalPtyLink(std::unique_ptr<pty::IPty> p) : pty_(std::move(p)) {}
    ~LocalPtyLink() override { close(std::chrono::milliseconds(500)); }

    Result<void> start(SessionEvents& events) override {
        events_ = &events;
        writer_ = std::thread([this] { write_loop(); });
        reader_ = std::thread([this] { read_loop(); });
        return {};
    }

    void write(std::string_view bytes) override {
        if (bytes.empty()) return;
        {
            std::lock_guard lk(wmu_);
            if (wstop_) return;
            wq_.emplace_back(bytes);
        }
        wcv_.notify_one();
    }

    void resize(pty::GridSize size) override {
        std::lock_guard lk(pty_mu_);
        if (pty_) (void)pty_->resize(size);
    }

    void close(std::chrono::milliseconds grace) override {
        if (closed_.exchange(true)) return;
        stopping_ = true;
        {
            std::lock_guard lk(wmu_);
            wstop_ = true;
        }
        wcv_.notify_one();
        if (writer_.joinable()) writer_.join();
        pty_->wake();
        pty_->close(grace);   // hangs up the session; read() then reports eof
        if (reader_.joinable()) reader_.join();
    }

private:
    void read_loop() {
        std::vector<std::byte> buf(64 * 1024);
        for (;;) {
            auto r = pty_->read(buf);
            if (!r) break;
            if (r->bytes && !stopping_) events_->on_output(std::span(buf.data(), r->bytes));
            if (r->eof || stopping_) break;
        }
        if (stopping_) return;
        // The slave side closed; the shell may need a moment to be reaped.
        std::optional<pty::ExitStatus> st = pty_->exit_status();
        for (int i = 0; !st && i < 200 && !stopping_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            st = pty_->exit_status();
        }
        if (!stopping_) events_->on_exit(st ? st->code : 1);
    }

    void write_loop() {
        std::unique_lock lk(wmu_);
        for (;;) {
            wcv_.wait(lk, [&] { return wstop_ || !wq_.empty(); });
            if (wstop_) return;
            std::string chunk = std::move(wq_.front());
            wq_.pop_front();
            lk.unlock();
            std::size_t off = 0;
            bool failed = false;
            while (off < chunk.size() && !failed) {
                Result<std::size_t> n = [&] {
                    std::lock_guard plk(pty_mu_);
                    return pty_->write(std::as_bytes(std::span(chunk.data() + off, chunk.size() - off)));
                }();
                if (!n) {
                    failed = true;
                } else if (*n == 0) {
                    // The PTY input buffer is full (the program does not read): retry shortly.
                    std::unique_lock wl(wmu_);
                    if (wcv_.wait_for(wl, std::chrono::milliseconds(2), [&] { return wstop_; })) return;
                } else {
                    off += *n;
                }
            }
            lk.lock();
            if (failed) wq_.clear();   // the PTY is gone; the reader reports the exit
        }
    }

    std::unique_ptr<pty::IPty> pty_;
    std::mutex pty_mu_;   // write and resize from different threads
    SessionEvents* events_ = nullptr;
    std::thread reader_, writer_;
    std::atomic<bool> stopping_{false}, closed_{false};

    std::mutex wmu_;
    std::condition_variable wcv_;
    std::deque<std::string> wq_;
    bool wstop_ = false;
};

#ifdef _WIN32
std::string env_utf8(const wchar_t* name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), out.data(), len, nullptr, nullptr);
    return out;
}
#else
std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}
#endif

}  // namespace

Result<std::unique_ptr<ISessionLink>> spawn_local_pty(const pty::SpawnSpec& spec) {
    auto p = pty::spawn(spec);
    if (!p) return Unexpected(p.error());
    return std::unique_ptr<ISessionLink>(std::make_unique<LocalPtyLink>(std::move(*p)));
}

pty::SpawnSpec default_shell_spec() {
    pty::SpawnSpec s;
#ifdef _WIN32
    std::string shell = env_utf8(L"COMSPEC");
    if (shell.empty()) shell = "cmd.exe";
    s.argv.push_back(shell);
    s.cwd = env_utf8(L"USERPROFILE");
#else
    const passwd* pw = ::getpwuid(::getuid());
    std::string shell = env("SHELL");
    if (shell.empty() && pw && pw->pw_shell && *pw->pw_shell) shell = pw->pw_shell;
    if (shell.empty()) shell = "/bin/sh";
    s.argv.push_back(shell);
    s.login_shell = true;
    s.cwd = env("HOME");
    if (s.cwd.empty() && pw && pw->pw_dir) s.cwd = pw->pw_dir;
    s.env_edits.push_back({"TERM", std::string("xterm-256color")});
    s.env_edits.push_back({"COLORTERM", std::string("truecolor")});
    s.env_edits.push_back({"TERM_PROGRAM", std::string("keepterm")});
    s.env_edits.push_back({"TERM_PROGRAM_VERSION", std::string(KT_VERSION)});
#ifdef __APPLE__
    // Started from Finder or launchd there is no locale; the shell would assume ASCII.
    if (env("LANG").empty() && env("LC_ALL").empty() && env("LC_CTYPE").empty())
        s.env_edits.push_back({"LC_CTYPE", std::string("UTF-8")});
#endif
#endif
    if (auto id = session::SessionId::generate()) s.env_edits.push_back({"KEEPTERM_SESSION", id->str()});
    return s;
}

}  // namespace kt::app
