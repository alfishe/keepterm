// Session client library: framed connections, the HELLO
// handshake, the attached client used by the GUI, and one-shot CLI requests.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/error.h"
#include "session/ids.h"
#include "session/ipc.h"
#include "session/protocol.h"

namespace kt::session {

struct OwnedFrame {
    uint16_t type = 0;
    std::vector<std::byte> payload;
};

// A stream with a frame splitter. send() may be called from any thread; recv() from one thread.
class Conn {
public:
    explicit Conn(std::unique_ptr<ipc::Stream> stream) : stream_(std::move(stream)) {}
    static Result<std::unique_ptr<Conn>> connect(const std::string& endpoint, int timeout_ms);

    Result<void> send(std::span<const std::byte> frames);
    // The next frame; nullopt at the end of the stream. timeout_ms < 0 waits forever.
    Result<std::optional<OwnedFrame>> recv(int timeout_ms = -1);
    void shutdown() { stream_->shutdown(); }
    uint32_t peer_pid() const { return stream_->peer_pid(); }

    // Client side: sends `mine`, expects HELLO (or HELLO_REJECT -> Errc::not_supported).
    Result<proto::Hello> client_handshake(const proto::Hello& mine, int timeout_ms);
    // Host side: expects HELLO, answers with `mine` or HELLO_REJECT. Returns the client's HELLO.
    Result<proto::Hello> server_handshake(const proto::Hello& mine, int timeout_ms);

private:
    std::unique_ptr<ipc::Stream> stream_;
    std::mutex send_mu_;
    proto::FrameBuffer fb_;
};

proto::Hello make_hello(proto::Role role, std::optional<SessionId> session);
std::string build_key();
inline constexpr uint32_t kWidthTableVersion = 1;

// Events of an attached client. They run on the client's reader thread.
class ClientEvents {
public:
    virtual ~ClientEvents() = default;
    // A whole KTSN snapshot; output with seq >= `seq` follows. `resync` after RESYNC.
    virtual void on_snapshot(std::vector<std::byte> ktsn, uint64_t seq, bool resync) = 0;
    virtual void on_output(uint64_t seq, std::span<const std::byte> bytes) = 0;
    virtual void on_resized(const proto::Resize& /*size*/, uint64_t /*seq*/) {}
    virtual void on_leader(bool /*is_leader*/) {}
    virtual void on_title(const proto::Title&) {}
    virtual void on_cwd(const proto::Cwd&) {}
    virtual void on_bell() {}
    // The agent context of the pane's foreground (AGENT_CONTEXT). Sent
    // once after ATTACH when the host has it, then on every change (at most once a second);
    // status None clears it. Hosts without the feature never call it (no "agent-context=1" in
    // host_hello().caps).
    //
    // Binding for the UI (the header indicator's "Context" segment): override this in the
    // pane's ClientEvents, copy the message to the UI thread, and render, for example
    //   status Ok:       "ctx " + short(used_tokens) + "/" + short(window_tokens) + " (" +
    //                    percent_x100/100 + "%)", colored at warn_percent / alert_percent;
    //                    window_source 3 (inferred) dimmed, flags & kCtxAfterCompaction marked;
    //   status NoData:   the agent icon only;  UnknownFormat: "ctx ?" with format_version in
    //                    the tooltip;  None: hide the segment.
    // SessionClient::request_agent_context() asks for the current value again (after a reconnect).
    virtual void on_agent_context(const proto::AgentContext&) {}
    // The shell ended. The client answers EXIT_ACK itself (pane_closed from AttachOptions).
    virtual void on_exit(const proto::ExitStatus&) = 0;
    // The connection ended without EXIT_STATUS (host crashed or was killed).
    virtual void on_disconnected(const Error&) = 0;
};

struct AttachOptions {
    proto::Role role = proto::Role::Gui;
    proto::AttachMode mode = proto::AttachMode::Control;
    proto::Resize size;
    std::string client_name;
    bool pane_closed_on_exit = true;   // EXIT_ACK{pane_closed}: the host deletes its history
    int timeout_ms = 3000;
};

class SessionClient {
public:
    // Connects, runs HELLO and ATTACH, and starts the reader and writer threads. The snapshot
    // arrives through ClientEvents::on_snapshot.
    static Result<std::unique_ptr<SessionClient>> attach(const std::string& endpoint, const AttachOptions&,
                                                         ClientEvents&);
    ~SessionClient();
    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    // Errc::busy when more than 1 MiB is queued (backpressure).
    Result<void> write_input(std::span<const std::byte> bytes);
    void resize(const proto::Resize&);
    void claim_leader();
    void send_option(const std::string& key, const std::string& value);
    // Asks the host for AGENT_CONTEXT; the answer arrives through on_agent_context().
    void request_agent_context();
    // Sends DETACH and closes. The host keeps running. Idempotent.
    void detach();

    const proto::Hello& host_hello() const { return host_hello_; }
    bool connected() const { return !ended_; }

private:
    SessionClient(std::unique_ptr<Conn>, AttachOptions, ClientEvents&);
    void reader_loop();
    void writer_loop();
    void enqueue(std::vector<std::byte> frame, bool counts_as_input);

    std::unique_ptr<Conn> conn_;
    AttachOptions opts_;
    ClientEvents& ev_;
    proto::Hello host_hello_;
    std::thread reader_, writer_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<std::vector<std::byte>, bool>> q_;
    std::size_t q_input_bytes_ = 0;
    bool stop_writer_ = false;
    std::atomic<bool> ended_{false};
    std::atomic<bool> detaching_{false};
    std::vector<std::byte> snap_;
    uint32_t snap_id_ = 0;
};

// One-shot requests of a CLI client (no ATTACH).
Result<proto::ProcessInfo> request_process_info(const std::string& endpoint, int timeout_ms = 2000);
Result<proto::KillResult> request_kill(const std::string& endpoint, const proto::Kill&, int timeout_ms = 3000);
// The current AGENT_CONTEXT of a session (no ATTACH). Errc::timeout from a host without it.
Result<proto::AgentContext> request_agent_context(const std::string& endpoint, int timeout_ms = 2000);
// One SET_OPTION without attaching (the GUI's pane menu: agent.forget, agent.restore,
// agent.resume_now). No answer: the next PROCESS_INFO shows the effect.
Result<void> send_option(const std::string& endpoint, const std::string& key, const std::string& value, int timeout_ms = 2000);
// Sends AGENT_STATUSLINE (the statusLine relay); returns after HELLO and the write. The whole
// call stays within `timeout_ms`.
Result<void> send_agent_statusline(const std::string& endpoint, const proto::AgentStatusline&, int timeout_ms = 50);
// Attaches as a CLI view client, waits for the snapshot and returns it with its seq.
Result<std::pair<std::vector<std::byte>, uint64_t>> request_snapshot(const std::string& endpoint,
                                                                     int timeout_ms = 3000);

}  // namespace kt::session
