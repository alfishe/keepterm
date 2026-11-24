// Session client library: handshake and attach.

#include "session/client.h"

#include <chrono>
#include <algorithm>
#include <cstring>

#include "base/log.h"
#include "session/os.h"

#ifndef KT_VERSION
#define KT_VERSION "0.0.0"
#endif

namespace kt::session {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxQueuedInput = 1u << 20;

int left_ms(Clock::time_point deadline) {
    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return d < 0 ? 0 : static_cast<int>(d);
}

Result<void> check_hello(const proto::Hello& h) {
    if (h.magic != proto::kMagic) return make_error(Errc::corrupt, "not a keepterm session endpoint");
    if (!proto::negotiate_level(proto::kLevelMin, proto::kLevelMax, h.level_min, h.level_max)) {
        return make_error(Errc::not_supported, "no common protocol level");
    }
    return {};
}

}  // namespace

std::string build_key() { return std::string("keepterm/") + KT_VERSION + "/p" + std::to_string(proto::kLevelMax); }

proto::Hello make_hello(proto::Role role, std::optional<SessionId> session) {
    proto::Hello h;
    h.build_key = build_key();
    h.version = KT_VERSION;
    h.role = role;
    h.session = session;
    h.pid = os::current_pid();
    h.width_table_version = kWidthTableVersion;
    h.caps = {"snapshot.ktsn1"};
    return h;
}

// ----------------------------------------------------------------------------------- Conn

Result<std::unique_ptr<Conn>> Conn::connect(const std::string& endpoint, int timeout_ms) {
    auto s = ipc::connect(endpoint, timeout_ms);
    if (!s) return Unexpected(s.error());
    return std::make_unique<Conn>(std::move(*s));
}

Result<void> Conn::send(std::span<const std::byte> frames) {
    std::lock_guard lk(send_mu_);
    return stream_->write_all(frames);
}

Result<std::optional<OwnedFrame>> Conn::recv(int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    std::byte buf[65536];
    for (;;) {
        auto f = fb_.next();
        if (!f) return Unexpected(f.error());
        if (*f) {
            OwnedFrame o;
            o.type = (*f)->type;
            o.payload.assign((*f)->payload.begin(), (*f)->payload.end());
            fb_.compact();
            return std::optional<OwnedFrame>(std::move(o));
        }
        auto n = stream_->read(buf, timeout_ms < 0 ? -1 : left_ms(deadline));
        if (!n) return Unexpected(n.error());
        if (*n == 0) return std::optional<OwnedFrame>();
        fb_.append(std::span<const std::byte>(buf, *n));
    }
}

Result<proto::Hello> Conn::client_handshake(const proto::Hello& mine, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<std::byte> out;
    proto::encode(mine, out);
    if (auto r = send(out); !r) return Unexpected(r.error());
    for (;;) {
        auto f = recv(left_ms(deadline));
        if (!f) return Unexpected(f.error());
        if (!*f) return make_error(Errc::lost, "host closed the connection during HELLO");
        const auto type = static_cast<proto::Type>((*f)->type);
        if (type == proto::Type::HelloReject) {
            proto::HelloReject rej;
            (void)proto::decode((*f)->payload, rej);
            return make_error(Errc::not_supported, "host rejected HELLO: " + rej.reason);
        }
        if (type != proto::Type::Hello) continue;
        proto::Hello h;
        if (auto r = proto::decode((*f)->payload, h); !r) return Unexpected(r.error());
        if (auto r = check_hello(h); !r) return Unexpected(r.error());
        return h;
    }
}

Result<proto::Hello> Conn::server_handshake(const proto::Hello& mine, int timeout_ms) {
    auto f = recv(timeout_ms);
    if (!f) return Unexpected(f.error());
    if (!*f) return make_error(Errc::lost, "client closed the connection during HELLO");
    if ((*f)->type != static_cast<uint16_t>(proto::Type::Hello)) {
        return make_error(Errc::corrupt, "first frame is not HELLO");
    }
    proto::Hello h;
    if (auto r = proto::decode((*f)->payload, h); !r) return Unexpected(r.error());
    std::vector<std::byte> out;
    if (auto r = check_hello(h); !r) {
        proto::HelloReject rej;
        rej.reason = r.error().what;
        proto::encode(rej, out);
        (void)send(out);
        return Unexpected(r.error());
    }
    proto::encode(mine, out);
    if (auto r = send(out); !r) return Unexpected(r.error());
    return h;
}

// --------------------------------------------------------------------------- SessionClient

SessionClient::SessionClient(std::unique_ptr<Conn> c, AttachOptions o, ClientEvents& ev)
    : conn_(std::move(c)), opts_(std::move(o)), ev_(ev) {}

Result<std::unique_ptr<SessionClient>> SessionClient::attach(const std::string& endpoint, const AttachOptions& o,
                                                             ClientEvents& ev) {
    auto c = Conn::connect(endpoint, o.timeout_ms);
    if (!c) return Unexpected(c.error());
    auto h = (*c)->client_handshake(make_hello(o.role, std::nullopt), o.timeout_ms);
    if (!h) return Unexpected(h.error());
    proto::Attach a;
    a.mode = o.mode;
    a.size = o.size;
    a.client_name = o.client_name;
    std::vector<std::byte> out;
    proto::encode(a, out);
    if (auto r = (*c)->send(out); !r) return Unexpected(r.error());
    std::unique_ptr<SessionClient> sc(new SessionClient(std::move(*c), o, ev));
    sc->host_hello_ = *h;
    sc->writer_ = std::thread([p = sc.get()] { p->writer_loop(); });
    sc->reader_ = std::thread([p = sc.get()] { p->reader_loop(); });
    return sc;
}

SessionClient::~SessionClient() {
    detach();
    if (reader_.joinable()) reader_.join();
    if (writer_.joinable()) writer_.join();
}

void SessionClient::enqueue(std::vector<std::byte> frame, bool counts_as_input) {
    {
        std::lock_guard lk(mu_);
        if (stop_writer_) return;
        if (counts_as_input) q_input_bytes_ += frame.size();
        q_.emplace_back(std::move(frame), counts_as_input);
    }
    cv_.notify_one();
}

Result<void> SessionClient::write_input(std::span<const std::byte> bytes) {
    if (ended_) return make_error(Errc::lost, "session ended");
    {
        std::lock_guard lk(mu_);
        if (q_input_bytes_ > kMaxQueuedInput) return make_error(Errc::busy, "input queue full");
    }
    while (!bytes.empty()) {
        const std::size_t n = std::min<std::size_t>(bytes.size(), proto::kOutputChunk);
        std::vector<std::byte> f;
        proto::encode(proto::Input{bytes.first(n)}, f);
        enqueue(std::move(f), true);
        bytes = bytes.subspan(n);
    }
    return {};
}

void SessionClient::resize(const proto::Resize& s) {
    std::vector<std::byte> f;
    proto::encode(s, f);
    enqueue(std::move(f), false);
}

void SessionClient::claim_leader() {
    std::vector<std::byte> f;
    proto::encode_empty(proto::Type::ClaimLeader, f);
    enqueue(std::move(f), false);
}

void SessionClient::send_option(const std::string& key, const std::string& value) {
    std::vector<std::byte> f;
    proto::encode(proto::SetOption{key, value, 0}, f);
    enqueue(std::move(f), false);
}

void SessionClient::request_agent_context() {
    std::vector<std::byte> f;
    proto::encode_empty(proto::Type::AgentContextRequest, f);
    enqueue(std::move(f), false);
}

void SessionClient::detach() {
    if (detaching_.exchange(true)) return;
    if (!ended_) {
        std::vector<std::byte> f;
        proto::encode_empty(proto::Type::Detach, f);
        enqueue(std::move(f), false);
    }
    {
        std::lock_guard lk(mu_);
        stop_writer_ = true;   // the writer drains the queue, then shuts the stream down
    }
    cv_.notify_one();
}

void SessionClient::writer_loop() {
    for (;;) {
        std::pair<std::vector<std::byte>, bool> item;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return stop_writer_ || !q_.empty(); });
            if (q_.empty()) break;
            item = std::move(q_.front());
            q_.pop_front();
            if (item.second) q_input_bytes_ -= item.first.size();
        }
        if (!conn_->send(item.first)) break;
    }
    if (detaching_) conn_->shutdown();
}

void SessionClient::reader_loop() {
    bool exited = false;
    Error why{Errc::lost, 0, "host closed the connection"};
    for (;;) {
        auto f = conn_->recv(-1);
        if (!f) {
            why = f.error();
            break;
        }
        if (!*f) break;
        const auto& p = (*f)->payload;
        switch (static_cast<proto::Type>((*f)->type)) {
            case proto::Type::Snapshot: {
                proto::SnapshotChunk c;
                if (!proto::decode(p, c)) break;
                if (c.index == 0) {
                    snap_.clear();
                    snap_id_ = c.snapshot_id;
                }
                if (c.snapshot_id != snap_id_) break;
                snap_.insert(snap_.end(), c.data.begin(), c.data.end());
                if (c.flags & proto::kSnapLast) {
                    ev_.on_snapshot(std::move(snap_), c.seq, (c.flags & proto::kSnapResync) != 0);
                    snap_ = {};
                }
                break;
            }
            case proto::Type::Output: {
                proto::Output o;
                if (proto::decode(p, o)) ev_.on_output(o.seq, o.bytes);
                break;
            }
            case proto::Type::Resized: {
                proto::Resized r;
                if (proto::decode(p, r)) ev_.on_resized(r.size, r.seq);
                break;
            }
            case proto::Type::Leader: {
                proto::Leader l;
                if (proto::decode(p, l)) ev_.on_leader(l.is_leader);
                break;
            }
            case proto::Type::Title: {
                proto::Title t;
                if (proto::decode(p, t)) ev_.on_title(t);
                break;
            }
            case proto::Type::Cwd: {
                proto::Cwd c;
                if (proto::decode(p, c)) ev_.on_cwd(c);
                break;
            }
            case proto::Type::Bell: ev_.on_bell(); break;
            case proto::Type::AgentContext: {
                proto::AgentContext a;
                if (proto::decode(p, a)) ev_.on_agent_context(a);
                break;
            }
            case proto::Type::Ping: {
                proto::Ping ping;
                if (proto::decode(p, ping)) {
                    std::vector<std::byte> out;
                    proto::encode_ping(ping, true, out);
                    enqueue(std::move(out), false);
                }
                break;
            }
            case proto::Type::ExitStatus: {
                proto::ExitStatus e;
                if (!proto::decode(p, e)) break;
                std::vector<std::byte> out;
                proto::encode(proto::ExitAck{opts_.pane_closed_on_exit}, out);
                (void)conn_->send(out);
                exited = true;
                ended_ = true;
                ev_.on_exit(e);
                break;
            }
            default: break;   // RESYNC is followed by a snapshot; the rest is not used yet
        }
        if (exited) break;
    }
    ended_ = true;
    {
        std::lock_guard lk(mu_);
        stop_writer_ = true;
    }
    cv_.notify_one();
    if (!exited && !detaching_) ev_.on_disconnected(why);
}

// ------------------------------------------------------------------------- CLI requests

namespace {

Result<std::unique_ptr<Conn>> cli_connect(const std::string& endpoint, int timeout_ms) {
    auto c = Conn::connect(endpoint, timeout_ms);
    if (!c) return Unexpected(c.error());
    auto h = (*c)->client_handshake(make_hello(proto::Role::Cli, std::nullopt), timeout_ms);
    if (!h) return Unexpected(h.error());
    return c;
}

template <class T>
Result<T> await_reply(Conn& c, proto::Type type, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        auto f = c.recv(left_ms(deadline));
        if (!f) return Unexpected(f.error());
        if (!*f) return make_error(Errc::lost, "host closed the connection");
        if ((*f)->type == static_cast<uint16_t>(proto::Type::Error)) {
            proto::ErrorMsg e;
            (void)proto::decode((*f)->payload, e);
            return make_error(Errc::unavailable, "host error: " + e.message);
        }
        if ((*f)->type != static_cast<uint16_t>(type)) continue;
        T v;
        if (auto r = proto::decode((*f)->payload, v); !r) return Unexpected(r.error());
        return v;
    }
}

}  // namespace

Result<proto::ProcessInfo> request_process_info(const std::string& endpoint, int timeout_ms) {
    auto c = cli_connect(endpoint, timeout_ms);
    if (!c) return Unexpected(c.error());
    std::vector<std::byte> out;
    proto::encode_empty(proto::Type::ProcessInfoRequest, out);
    if (auto r = (*c)->send(out); !r) return Unexpected(r.error());
    return await_reply<proto::ProcessInfo>(**c, proto::Type::ProcessInfo, timeout_ms);
}

Result<proto::AgentContext> request_agent_context(const std::string& endpoint, int timeout_ms) {
    auto c = cli_connect(endpoint, timeout_ms);
    if (!c) return Unexpected(c.error());
    std::vector<std::byte> out;
    proto::encode_empty(proto::Type::AgentContextRequest, out);
    if (auto r = (*c)->send(out); !r) return Unexpected(r.error());
    return await_reply<proto::AgentContext>(**c, proto::Type::AgentContext, timeout_ms);
}

Result<void> send_option(const std::string& endpoint, const std::string& key, const std::string& value, int timeout_ms) {
    auto c = cli_connect(endpoint, timeout_ms);
    if (!c) return Unexpected(c.error());
    std::vector<std::byte> out;
    proto::encode(proto::SetOption{key, value, 0}, out);
    auto r = (*c)->send(out);
    // A PING round trip: the host has read the option before the connection goes.
    std::vector<std::byte> ping;
    proto::encode_ping(proto::Ping{1}, false, ping);
    if (r && (*c)->send(ping)) (void)await_reply<proto::Ping>(**c, proto::Type::Pong, timeout_ms);
    (*c)->shutdown();
    return r;
}

Result<void> send_agent_statusline(const std::string& endpoint, const proto::AgentStatusline& m, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    auto c = Conn::connect(endpoint, timeout_ms);
    if (!c) return Unexpected(c.error());
    auto h = (*c)->client_handshake(make_hello(proto::Role::Cli, std::nullopt), std::max(1, left_ms(deadline)));
    if (!h) return Unexpected(h.error());
    std::vector<std::byte> out;
    proto::encode(m, out);
    auto r = (*c)->send(out);
    (*c)->shutdown();
    return r;
}

Result<proto::KillResult> request_kill(const std::string& endpoint, const proto::Kill& k, int timeout_ms) {
    auto c = cli_connect(endpoint, timeout_ms);
    if (!c) return Unexpected(c.error());
    std::vector<std::byte> out;
    proto::encode(k, out);
    if (auto r = (*c)->send(out); !r) return Unexpected(r.error());
    return await_reply<proto::KillResult>(**c, proto::Type::KillResult, timeout_ms);
}

Result<std::pair<std::vector<std::byte>, uint64_t>> request_snapshot(const std::string& endpoint, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    auto c = cli_connect(endpoint, timeout_ms);
    if (!c) return Unexpected(c.error());
    proto::Attach a;
    a.mode = proto::AttachMode::View;
    a.client_name = "keepterm-cli";
    std::vector<std::byte> out;
    proto::encode(a, out);
    if (auto r = (*c)->send(out); !r) return Unexpected(r.error());
    std::vector<std::byte> snap;
    uint32_t id = 0;
    for (;;) {
        auto f = (*c)->recv(left_ms(deadline));
        if (!f) return Unexpected(f.error());
        if (!*f) return make_error(Errc::lost, "host closed the connection");
        if ((*f)->type != static_cast<uint16_t>(proto::Type::Snapshot)) continue;
        proto::SnapshotChunk ch;
        if (auto r = proto::decode((*f)->payload, ch); !r) return Unexpected(r.error());
        if (ch.index == 0) {
            snap.clear();
            id = ch.snapshot_id;
        }
        if (ch.snapshot_id != id) continue;
        snap.insert(snap.end(), ch.data.begin(), ch.data.end());
        if (ch.flags & proto::kSnapLast) {
            out.clear();
            proto::encode_empty(proto::Type::Detach, out);
            (void)(*c)->send(out);
            return std::make_pair(std::move(snap), ch.seq);
        }
    }
}

}  // namespace kt::session
