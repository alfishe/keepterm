// ISessionLink over the session protocol client (session::Client).

#include <atomic>
#include <mutex>

#include "app/session_link.h"
#include "session/client.h"

namespace kt::app {
namespace {

class SessionClientLink final : public ISessionLink, private session::ClientEvents {
public:
    SessionClientLink(std::string id, std::string endpoint, pty::GridSize size)
        : id_(std::move(id)), endpoint_(std::move(endpoint)), size_(size) {}
    ~SessionClientLink() override { close(std::chrono::milliseconds(0)); }

    Result<void> start(SessionEvents& events) override {
        events_ = &events;
        session::AttachOptions o;
        o.role = session::proto::Role::Gui;
        o.mode = session::proto::AttachMode::Control;
        o.size = session::proto::Resize{size_.cols, size_.rows, size_.px_w, size_.px_h};
        o.client_name = "keepterm";
        o.pane_closed_on_exit = true;   // the window closes when the shell ends
        auto c = session::SessionClient::attach(endpoint_, o, *this);
        if (!c) return Unexpected(c.error());
        std::lock_guard lk(mu_);
        client_ = std::move(*c);
        return {};
    }

    void write(std::string_view bytes) override {
        std::lock_guard lk(mu_);
        if (!client_ || bytes.empty()) return;
        // Over 1 MiB queued: dropped, like keys typed into a hung terminal (backpressure).
        (void)client_->write_input(std::as_bytes(std::span(bytes.data(), bytes.size())));
    }

    void resize(pty::GridSize s) override {
        std::lock_guard lk(mu_);
        if (client_) client_->resize(session::proto::Resize{s.cols, s.rows, s.px_w, s.px_h});
    }

    void close(std::chrono::milliseconds) override {
        std::unique_ptr<session::SessionClient> c;
        {
            std::lock_guard lk(mu_);
            c = std::move(client_);
        }
        closed_ = true;
        c.reset();   // DETACH: the host keeps running
    }

    bool mirror() const override { return true; }
    std::string session_id() const override { return id_; }

private:
    void on_snapshot(std::vector<std::byte> ktsn, uint64_t, bool resync) override {
        events_->on_snapshot(std::move(ktsn), resync);
    }
    void on_output(uint64_t, std::span<const std::byte> bytes) override { events_->on_output(bytes); }
    void on_resized(const session::proto::Resize& r, uint64_t) override {
        events_->on_resized(pty::GridSize{r.cols, r.rows, r.px_w, r.px_h});
    }
    void on_cwd(const session::proto::Cwd& c) override { events_->on_cwd(c.path, c.source); }
    void on_exit(const session::proto::ExitStatus& e) override { events_->on_exit(e.code); }
    void on_disconnected(const Error& e) override {
        if (!closed_) events_->on_host_lost(e.what);
    }

    std::string id_, endpoint_;
    pty::GridSize size_;
    SessionEvents* events_ = nullptr;
    std::mutex mu_;
    std::unique_ptr<session::SessionClient> client_;
    std::atomic<bool> closed_{false};
};

}  // namespace

Result<std::unique_ptr<ISessionLink>> attach_session(const std::string& session_id, const std::string& endpoint,
                                                     pty::GridSize size) {
    return std::unique_ptr<ISessionLink>(std::make_unique<SessionClientLink>(session_id, endpoint, size));
}

}  // namespace kt::app
