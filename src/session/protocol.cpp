#include "session/protocol.h"

#include <algorithm>
#include <cstring>

namespace kt::session::proto {
namespace {

void put_u16(std::vector<std::byte>& out, uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>(v >> 8));
}

void put_u32(std::vector<std::byte>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

void put_u64(std::vector<std::byte>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
}

uint16_t get_u16(const std::byte* p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

uint32_t get_u32(const std::byte* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<uint32_t>(p[i]);
    return v;
}

uint64_t get_u64(const std::byte* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

Unexpected<Error> bad(const char* what) { return make_error(Errc::corrupt, std::string("protocol: ") + what); }

// Builds a TLV frame: the header is patched after the payload is written.
class FrameBuilder {
public:
    FrameBuilder(Type t, std::vector<std::byte>& out) : out_(out), start_(out.size()), w_(out) {
        encode_header(static_cast<uint16_t>(t), 0, out_);
    }
    ~FrameBuilder() {
        const std::size_t len = out_.size() - start_ - 4;   // type + payload
        for (int i = 0; i < 4; ++i) out_[start_ + static_cast<std::size_t>(i)] = static_cast<std::byte>((len >> (8 * i)) & 0xFFu);
    }
    FrameBuilder(const FrameBuilder&) = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;
    TlvWriter& w() { return w_; }

private:
    std::vector<std::byte>& out_;
    std::size_t start_;
    TlvWriter w_;
};

// Runs `f(field)` for every field; `f` returns false for a malformed known field.
template <class F>
Result<void> for_fields(std::span<const std::byte> payload, F&& f) {
    TlvReader r(payload);
    TlvField fld;
    while (r.next(fld)) {
        if (!f(fld)) return bad("malformed field");
    }
    if (r.error()) return bad("truncated field");
    return {};
}

bool tlv_bool(const TlvField& f, bool& v) {
    uint8_t b = 0;
    if (!tlv_u8(f, b)) return false;
    v = b != 0;
    return true;
}

}  // namespace

bool known_type(uint16_t type) {
    switch (static_cast<Type>(type)) {
        case Type::Hello: case Type::HelloReject: case Type::Ping: case Type::Pong: case Type::Error:
        case Type::Attach: case Type::Detach: case Type::Leader: case Type::ClaimLeader:
        case Type::SnapshotRequest: case Type::Snapshot: case Type::ResumeOk:
        case Type::Output: case Type::Input: case Type::Resize: case Type::Resized: case Type::Resync:
        case Type::Title: case Type::Cwd: case Type::Bell: case Type::Notify: case Type::PromptMark:
        case Type::Attention: case Type::Progress:
        case Type::ProcessInfoRequest: case Type::ProcessInfo: case Type::Kill: case Type::KillResult:
        case Type::ExitStatus: case Type::ExitAck: case Type::SetOption:
        case Type::AgentContextRequest: case Type::AgentContext: case Type::AgentStatusline:
            return true;
    }
    return false;
}

std::string_view type_name(uint16_t type) {
    switch (static_cast<Type>(type)) {
        case Type::Hello: return "HELLO";
        case Type::HelloReject: return "HELLO_REJECT";
        case Type::Ping: return "PING";
        case Type::Pong: return "PONG";
        case Type::Error: return "ERROR";
        case Type::Attach: return "ATTACH";
        case Type::Detach: return "DETACH";
        case Type::Leader: return "LEADER";
        case Type::ClaimLeader: return "CLAIM_LEADER";
        case Type::SnapshotRequest: return "SNAPSHOT_REQUEST";
        case Type::Snapshot: return "SNAPSHOT";
        case Type::ResumeOk: return "RESUME_OK";
        case Type::Output: return "OUTPUT";
        case Type::Input: return "INPUT";
        case Type::Resize: return "RESIZE";
        case Type::Resized: return "RESIZED";
        case Type::Resync: return "RESYNC";
        case Type::Title: return "TITLE";
        case Type::Cwd: return "CWD";
        case Type::Bell: return "BELL";
        case Type::Notify: return "NOTIFY";
        case Type::PromptMark: return "PROMPT_MARK";
        case Type::Attention: return "ATTENTION";
        case Type::Progress: return "PROGRESS";
        case Type::ProcessInfoRequest: return "PROCESS_INFO_REQUEST";
        case Type::ProcessInfo: return "PROCESS_INFO";
        case Type::Kill: return "KILL";
        case Type::KillResult: return "KILL_RESULT";
        case Type::ExitStatus: return "EXIT_STATUS";
        case Type::ExitAck: return "EXIT_ACK";
        case Type::SetOption: return "SET_OPTION";
        case Type::AgentContextRequest: return "AGENT_CONTEXT_REQUEST";
        case Type::AgentContext: return "AGENT_CONTEXT";
        case Type::AgentStatusline: return "AGENT_STATUSLINE";
    }
    return "unknown";
}

// ------------------------------------------------------------------------------- framing

Result<std::size_t> decode_frame(std::span<const std::byte> in, FrameView& out) {
    if (in.size() < 4) return std::size_t{0};
    const uint32_t len = get_u32(in.data());
    if (len < 2) return bad("frame length below 2");
    if (len > kMaxFrame) return bad("frame too large");
    if (in.size() - 4 < len) return std::size_t{0};
    out.type = get_u16(in.data() + 4);
    out.payload = in.subspan(kHeaderSize, len - 2u);
    return std::size_t{4} + len;
}

void encode_header(uint16_t type, std::size_t payload_len, std::vector<std::byte>& out) {
    put_u32(out, static_cast<uint32_t>(payload_len + 2));
    put_u16(out, type);
}

void encode_frame(uint16_t type, std::span<const std::byte> payload, std::vector<std::byte>& out) {
    encode_header(type, payload.size(), out);
    out.insert(out.end(), payload.begin(), payload.end());
}

// ------------------------------------------------------------------------------------- TLV

void TlvWriter::head(uint16_t tag, std::size_t len) {
    put_u16(out_, tag);
    put_u32(out_, static_cast<uint32_t>(len));
}
void TlvWriter::u8(uint16_t tag, uint8_t v) {
    head(tag, 1);
    out_.push_back(static_cast<std::byte>(v));
}
void TlvWriter::u16(uint16_t tag, uint16_t v) {
    head(tag, 2);
    put_u16(out_, v);
}
void TlvWriter::u32(uint16_t tag, uint32_t v) {
    head(tag, 4);
    put_u32(out_, v);
}
void TlvWriter::u64(uint16_t tag, uint64_t v) {
    head(tag, 8);
    put_u64(out_, v);
}
void TlvWriter::i32(uint16_t tag, int32_t v) { u32(tag, static_cast<uint32_t>(v)); }
void TlvWriter::str(uint16_t tag, std::string_view v) {
    bytes(tag, std::as_bytes(std::span(v.data(), v.size())));
}
void TlvWriter::bytes(uint16_t tag, std::span<const std::byte> v) {
    head(tag, v.size());
    out_.insert(out_.end(), v.begin(), v.end());
}

bool TlvReader::next(TlvField& f) {
    if (error_ || pos_ >= in_.size()) return false;
    if (in_.size() - pos_ < 6) {
        error_ = true;
        return false;
    }
    const uint16_t tag = get_u16(in_.data() + pos_);
    const uint32_t len = get_u32(in_.data() + pos_ + 2);
    if (in_.size() - pos_ - 6 < len) {
        error_ = true;
        return false;
    }
    f.tag = tag;
    f.value = in_.subspan(pos_ + 6, len);
    pos_ += 6u + len;
    return true;
}

bool tlv_u8(const TlvField& f, uint8_t& v) {
    if (f.value.size() != 1) return false;
    v = static_cast<uint8_t>(f.value[0]);
    return true;
}
bool tlv_u16(const TlvField& f, uint16_t& v) {
    if (f.value.size() != 2) return false;
    v = get_u16(f.value.data());
    return true;
}
bool tlv_u32(const TlvField& f, uint32_t& v) {
    if (f.value.size() != 4) return false;
    v = get_u32(f.value.data());
    return true;
}
bool tlv_u64(const TlvField& f, uint64_t& v) {
    if (f.value.size() != 8) return false;
    v = get_u64(f.value.data());
    return true;
}
bool tlv_i32(const TlvField& f, int32_t& v) {
    uint32_t u = 0;
    if (!tlv_u32(f, u)) return false;
    v = static_cast<int32_t>(u);
    return true;
}
bool tlv_str(const TlvField& f, std::string& v, std::size_t max) {
    if (f.value.size() > max) return false;
    v.assign(reinterpret_cast<const char*>(f.value.data()), f.value.size());
    return true;
}

// ---------------------------------------------------------------------------------- HELLO

void encode(const Hello& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Hello, out);
    auto& w = b.w();
    w.u32(1, m.magic);
    w.u16(2, m.level_min);
    w.u16(3, m.level_max);
    w.str(4, m.build_key);
    w.str(5, m.version);
    w.u8(6, static_cast<uint8_t>(m.role));
    for (const auto& c : m.caps) w.str(7, c);
    if (m.session) w.str(8, m.session->str());
    w.u32(9, m.pid);
    w.u32(10, m.width_table_version);
    w.u16(11, m.snapshot_major_min);
    w.u16(12, m.snapshot_major_max);
}

Result<void> decode(std::span<const std::byte> payload, Hello& m) {
    m = Hello{};
    m.magic = 0;
    bool session_bad = false;
    auto r = for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_u32(f, m.magic);
            case 2: return tlv_u16(f, m.level_min);
            case 3: return tlv_u16(f, m.level_max);
            case 4: return tlv_str(f, m.build_key, 256);
            case 5: return tlv_str(f, m.version, 256);
            case 6: {
                uint8_t v = 0;
                if (!tlv_u8(f, v) || v > 2) return false;
                m.role = static_cast<Role>(v);
                return true;
            }
            case 7: {
                std::string c;
                if (!tlv_str(f, c, 256) || m.caps.size() >= 256) return false;
                m.caps.push_back(std::move(c));
                return true;
            }
            case 8: {
                std::string s;
                if (!tlv_str(f, s, 64)) return false;
                auto id = SessionId::parse(s);
                if (!id) {
                    session_bad = true;
                    return false;
                }
                m.session = *id;
                return true;
            }
            case 9: return tlv_u32(f, m.pid);
            case 10: return tlv_u32(f, m.width_table_version);
            case 11: return tlv_u16(f, m.snapshot_major_min);
            case 12: return tlv_u16(f, m.snapshot_major_max);
            default: return true;
        }
    });
    if (!r) return r;
    if (m.magic != kMagic) return bad("HELLO magic");
    if (m.level_min > m.level_max) return bad("HELLO level range");
    return {};
}

void encode(const HelloReject& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::HelloReject, out);
    b.w().str(1, m.reason);
    b.w().u16(2, m.level_min);
    b.w().u16(3, m.level_max);
}

Result<void> decode(std::span<const std::byte> payload, HelloReject& m) {
    m = HelloReject{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_str(f, m.reason);
            case 2: return tlv_u16(f, m.level_min);
            case 3: return tlv_u16(f, m.level_max);
            default: return true;
        }
    });
}

void encode_ping(const Ping& m, bool pong, std::vector<std::byte>& out) {
    FrameBuilder b(pong ? Type::Pong : Type::Ping, out);
    b.w().u64(1, m.nonce);
}

Result<void> decode(std::span<const std::byte> payload, Ping& m) {
    m = Ping{};
    return for_fields(payload, [&](const TlvField& f) { return f.tag == 1 ? tlv_u64(f, m.nonce) : true; });
}

void encode(const ErrorMsg& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Error, out);
    b.w().u16(1, static_cast<uint16_t>(m.code));
    b.w().str(2, m.message);
    b.w().u16(3, m.ref_type);
}

Result<void> decode(std::span<const std::byte> payload, ErrorMsg& m) {
    m = ErrorMsg{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: {
                uint16_t v = 0;
                if (!tlv_u16(f, v)) return false;
                m.code = static_cast<ErrorCode>(v);
                return true;
            }
            case 2: return tlv_str(f, m.message);
            case 3: return tlv_u16(f, m.ref_type);
            default: return true;
        }
    });
}

// ------------------------------------------------------------------------------ attach, size

namespace {
void put_size(TlvWriter& w, const Resize& s, uint16_t base) {
    w.u16(base, s.cols);
    w.u16(static_cast<uint16_t>(base + 1), s.rows);
    w.u16(static_cast<uint16_t>(base + 2), s.px_w);
    w.u16(static_cast<uint16_t>(base + 3), s.px_h);
}
// Returns 1 if the field was a size field, 0 if not, -1 if malformed.
int get_size(const TlvField& f, Resize& s, uint16_t base) {
    if (f.tag < base || f.tag > base + 3) return 0;
    uint16_t* dst[4] = {&s.cols, &s.rows, &s.px_w, &s.px_h};
    return tlv_u16(f, *dst[f.tag - base]) ? 1 : -1;
}
bool valid_size(const Resize& s) { return s.cols > 0 && s.rows > 0 && s.cols <= 4096 && s.rows <= 4096; }
}  // namespace

void encode(const Attach& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Attach, out);
    b.w().u8(1, static_cast<uint8_t>(m.mode));
    put_size(b.w(), m.size, 2);
    b.w().str(6, m.client_name);
}

Result<void> decode(std::span<const std::byte> payload, Attach& m) {
    m = Attach{};
    auto r = for_fields(payload, [&](const TlvField& f) {
        if (const int s = get_size(f, m.size, 2); s != 0) return s > 0;
        switch (f.tag) {
            case 1: {
                uint8_t v = 0;
                if (!tlv_u8(f, v) || v > 1) return false;
                m.mode = static_cast<AttachMode>(v);
                return true;
            }
            case 6: return tlv_str(f, m.client_name, 256);
            default: return true;
        }
    });
    if (!r) return r;
    if (!valid_size(m.size)) return bad("ATTACH size");
    return {};
}

void encode_empty(Type t, std::vector<std::byte>& out) { encode_header(static_cast<uint16_t>(t), 0, out); }

void encode(const Leader& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Leader, out);
    b.w().u8(1, m.is_leader ? 1 : 0);
    b.w().u32(2, m.leader_client_id);
}

Result<void> decode(std::span<const std::byte> payload, Leader& m) {
    m = Leader{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_bool(f, m.is_leader);
            case 2: return tlv_u32(f, m.leader_client_id);
            default: return true;
        }
    });
}

void encode(const SnapshotRequest& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::SnapshotRequest, out);
    if (m.resume_from) b.w().u64(1, *m.resume_from);
    b.w().u8(2, m.screen_only ? 1 : 0);
    b.w().u8(3, m.scrollback_only ? 1 : 0);
    b.w().u8(4, m.text_only ? 1 : 0);
    b.w().u32(5, m.scrollback_lines);
    b.w().u16(6, m.major_max);
}

Result<void> decode(std::span<const std::byte> payload, SnapshotRequest& m) {
    m = SnapshotRequest{};
    auto r = for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: {
                uint64_t v = 0;
                if (!tlv_u64(f, v)) return false;
                m.resume_from = v;
                return true;
            }
            case 2: return tlv_bool(f, m.screen_only);
            case 3: return tlv_bool(f, m.scrollback_only);
            case 4: return tlv_bool(f, m.text_only);
            case 5: return tlv_u32(f, m.scrollback_lines);
            case 6: return tlv_u16(f, m.major_max);
            default: return true;
        }
    });
    if (!r) return r;
    if (m.screen_only && m.scrollback_only) return bad("SNAPSHOT_REQUEST options");
    return {};
}

// ------------------------------------------------------------------------- fixed layouts

namespace {
constexpr std::size_t kSnapshotFixed = 4 + 4 + 1 + 8 + 2 + 2;
}

void encode(const SnapshotChunk& m, std::vector<std::byte>& out) {
    encode_header(static_cast<uint16_t>(Type::Snapshot), kSnapshotFixed + m.data.size(), out);
    put_u32(out, m.snapshot_id);
    put_u32(out, m.index);
    out.push_back(static_cast<std::byte>(m.flags));
    put_u64(out, m.seq);
    put_u16(out, m.major);
    put_u16(out, m.minor);
    out.insert(out.end(), m.data.begin(), m.data.end());
}

Result<void> decode(std::span<const std::byte> p, SnapshotChunk& m) {
    if (p.size() < kSnapshotFixed) return bad("SNAPSHOT too short");
    m.snapshot_id = get_u32(p.data());
    m.index = get_u32(p.data() + 4);
    m.flags = static_cast<uint8_t>(p[8]);
    m.seq = get_u64(p.data() + 9);
    m.major = get_u16(p.data() + 17);
    m.minor = get_u16(p.data() + 19);
    m.data = p.subspan(kSnapshotFixed);
    return {};
}

void encode(const ResumeOk& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::ResumeOk, out);
    b.w().u64(1, m.from_seq);
}

Result<void> decode(std::span<const std::byte> payload, ResumeOk& m) {
    m = ResumeOk{};
    return for_fields(payload, [&](const TlvField& f) { return f.tag == 1 ? tlv_u64(f, m.from_seq) : true; });
}

void encode(const Output& m, std::vector<std::byte>& out) {
    encode_header(static_cast<uint16_t>(Type::Output), 8 + m.bytes.size(), out);
    put_u64(out, m.seq);
    out.insert(out.end(), m.bytes.begin(), m.bytes.end());
}

Result<void> decode(std::span<const std::byte> p, Output& m) {
    if (p.size() < 8) return bad("OUTPUT too short");
    m.seq = get_u64(p.data());
    m.bytes = p.subspan(8);
    return {};
}

void encode(const Input& m, std::vector<std::byte>& out) {
    encode_frame(static_cast<uint16_t>(Type::Input), m.bytes, out);
}

Result<void> decode(std::span<const std::byte> p, Input& m) {
    if (p.size() > kOutputChunk) return bad("INPUT over 64 KiB");
    m.bytes = p;
    return {};
}

void encode(const Resize& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Resize, out);
    put_size(b.w(), m, 1);
}

Result<void> decode(std::span<const std::byte> payload, Resize& m) {
    m = Resize{};
    auto r = for_fields(payload, [&](const TlvField& f) { return get_size(f, m, 1) >= 0; });
    if (!r) return r;
    if (!valid_size(m)) return bad("RESIZE size");
    return {};
}

void encode(const Resized& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Resized, out);
    put_size(b.w(), m.size, 1);
    b.w().u64(5, m.seq);
}

Result<void> decode(std::span<const std::byte> payload, Resized& m) {
    m = Resized{};
    auto r = for_fields(payload, [&](const TlvField& f) {
        if (const int s = get_size(f, m.size, 1); s != 0) return s > 0;
        return f.tag == 5 ? tlv_u64(f, m.seq) : true;
    });
    if (!r) return r;
    if (!valid_size(m.size)) return bad("RESIZED size");
    return {};
}

void encode(const Resync& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Resync, out);
    b.w().u8(1, static_cast<uint8_t>(m.reason));
}

Result<void> decode(std::span<const std::byte> payload, Resync& m) {
    m = Resync{};
    return for_fields(payload, [&](const TlvField& f) {
        if (f.tag != 1) return true;
        uint8_t v = 0;
        if (!tlv_u8(f, v)) return false;
        m.reason = static_cast<ResyncReason>(v);
        return true;
    });
}

// --------------------------------------------------------------------------- notifications

void encode(const Title& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Title, out);
    b.w().str(1, m.text);
    b.w().u8(2, static_cast<uint8_t>(m.source));
    b.w().u64(3, m.seq);
}

Result<void> decode(std::span<const std::byte> payload, Title& m) {
    m = Title{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_str(f, m.text);
            case 2: {
                uint8_t v = 0;
                if (!tlv_u8(f, v)) return false;
                m.source = static_cast<TitleSource>(v);
                return true;
            }
            case 3: return tlv_u64(f, m.seq);
            default: return true;
        }
    });
}

void encode(const Cwd& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Cwd, out);
    b.w().str(1, m.path);
    b.w().str(2, m.host);
    b.w().str(3, m.source);
    b.w().u64(4, m.seq);
}

Result<void> decode(std::span<const std::byte> payload, Cwd& m) {
    m = Cwd{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_str(f, m.path, kMaxPath);
            case 2: return tlv_str(f, m.host, 1024);
            case 3: return tlv_str(f, m.source, 64);
            case 4: return tlv_u64(f, m.seq);
            default: return true;
        }
    });
}

void encode(const Bell& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Bell, out);
    b.w().u64(1, m.seq);
}

Result<void> decode(std::span<const std::byte> payload, Bell& m) {
    m = Bell{};
    return for_fields(payload, [&](const TlvField& f) { return f.tag == 1 ? tlv_u64(f, m.seq) : true; });
}

void encode(const Notify& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Notify, out);
    b.w().u8(1, static_cast<uint8_t>(m.kind));
    b.w().str(2, m.title);
    b.w().str(3, m.body);
    b.w().u64(4, m.seq);
}

Result<void> decode(std::span<const std::byte> payload, Notify& m) {
    m = Notify{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: {
                uint8_t v = 0;
                if (!tlv_u8(f, v)) return false;
                m.kind = static_cast<NotifyKind>(v);
                return true;
            }
            case 2: return tlv_str(f, m.title);
            case 3: return tlv_str(f, m.body);
            case 4: return tlv_u64(f, m.seq);
            default: return true;
        }
    });
}

void encode(const PromptMarkMsg& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::PromptMark, out);
    b.w().u8(1, m.kind);
    if (m.exit_code) b.w().i32(2, *m.exit_code);
    b.w().u64(3, m.seq);
}

Result<void> decode(std::span<const std::byte> payload, PromptMarkMsg& m) {
    m = PromptMarkMsg{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_u8(f, m.kind);
            case 2: {
                int32_t v = 0;
                if (!tlv_i32(f, v)) return false;
                m.exit_code = v;
                return true;
            }
            case 3: return tlv_u64(f, m.seq);
            default: return true;
        }
    });
}

void encode(const Attention& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Attention, out);
    b.w().u8(1, m.flags);
}

Result<void> decode(std::span<const std::byte> payload, Attention& m) {
    m = Attention{};
    return for_fields(payload, [&](const TlvField& f) { return f.tag == 1 ? tlv_u8(f, m.flags) : true; });
}

void encode(const Progress& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Progress, out);
    b.w().u8(1, m.state);
    b.w().u8(2, m.percent);
}

Result<void> decode(std::span<const std::byte> payload, Progress& m) {
    m = Progress{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_u8(f, m.state);
            case 2: return tlv_u8(f, m.percent);
            default: return true;
        }
    });
}

// ------------------------------------------------------------------------------ processes

void encode(const ProcessInfo& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::ProcessInfo, out);
    auto& w = b.w();
    w.u32(1, m.host_pid);
    w.u64(2, m.host_start);
    w.u32(3, m.root_pid);
    w.u64(4, m.root_start);
    w.u32(5, m.foreground_pid);
    w.u8(6, m.foreground_is_shell ? 1 : 0);
    w.str(7, m.cwd);
    w.str(8, m.cwd_source);
    w.str(9, m.backend);
    w.u8(10, m.containment_kind);
    w.str(11, m.containment_handle);
    if (m.agent_pid) w.u32(12, *m.agent_pid);
    if (m.posix_sid) w.u32(13, *m.posix_sid);
    if (m.tty_dev) w.u64(14, *m.tty_dev);
    for (uint32_t c : m.children) w.u32(15, c);
    w.u64(16, m.last_input_ns);
    w.str(17, m.shell_kind);
    for (const ChainEntry& e : m.foreground_chain) {
        std::vector<std::byte> sub;
        TlvWriter cw(sub);
        cw.u32(1, e.pid);
        cw.u32(2, e.ppid);
        cw.u64(3, e.start);
        cw.u64(4, e.start_unix_ns);
        cw.str(5, e.image);
        cw.str(6, e.comm);
        for (const auto& a : e.argv) cw.str(7, a);
        cw.str(8, e.cwd);
        w.bytes(18, sub);
    }
    w.str(19, m.shell_image);
    if (!m.saved_agent.empty()) {
        w.str(20, m.saved_agent);
        w.str(21, m.saved_agent_session);
        w.str(22, m.saved_agent_dir);
    }
    if (!m.agent_restore) w.u8(23, 0);
}

namespace {
bool decode_chain_entry(std::span<const std::byte> v, ChainEntry& e) {
    std::size_t argv_bytes = 0;
    auto r = for_fields(v, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_u32(f, e.pid);
            case 2: return tlv_u32(f, e.ppid);
            case 3: return tlv_u64(f, e.start);
            case 4: return tlv_u64(f, e.start_unix_ns);
            case 5: return tlv_str(f, e.image, 1024);
            case 6: return tlv_str(f, e.comm, 256);
            case 7: {
                std::string a;
                if (!tlv_str(f, a, kMaxString) || e.argv.size() >= 4096) return false;
                argv_bytes += a.size();
                if (argv_bytes > kMaxPath * 4) return false;
                e.argv.push_back(std::move(a));
                return true;
            }
            case 8: return tlv_str(f, e.cwd, kMaxPath);
            default: return true;
        }
    });
    return static_cast<bool>(r);
}
}  // namespace

Result<void> decode(std::span<const std::byte> payload, ProcessInfo& m) {
    m = ProcessInfo{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_u32(f, m.host_pid);
            case 2: return tlv_u64(f, m.host_start);
            case 3: return tlv_u32(f, m.root_pid);
            case 4: return tlv_u64(f, m.root_start);
            case 5: return tlv_u32(f, m.foreground_pid);
            case 6: return tlv_bool(f, m.foreground_is_shell);
            case 7: return tlv_str(f, m.cwd, kMaxPath);
            case 8: return tlv_str(f, m.cwd_source, 64);
            case 9: return tlv_str(f, m.backend, 256);
            case 10: return tlv_u8(f, m.containment_kind);
            case 11: return tlv_str(f, m.containment_handle, 1024);
            case 12: {
                uint32_t v = 0;
                if (!tlv_u32(f, v)) return false;
                m.agent_pid = v;
                return true;
            }
            case 13: {
                uint32_t v = 0;
                if (!tlv_u32(f, v)) return false;
                m.posix_sid = v;
                return true;
            }
            case 14: {
                uint64_t v = 0;
                if (!tlv_u64(f, v)) return false;
                m.tty_dev = v;
                return true;
            }
            case 15: {
                uint32_t v = 0;
                if (!tlv_u32(f, v) || m.children.size() >= 65536) return false;
                m.children.push_back(v);
                return true;
            }
            case 16: return tlv_u64(f, m.last_input_ns);
            case 17: return tlv_str(f, m.shell_kind, 64);
            case 18: {
                ChainEntry e;
                if (m.foreground_chain.size() >= kMaxChain || !decode_chain_entry(f.value, e)) return false;
                m.foreground_chain.push_back(std::move(e));
                return true;
            }
            case 19: return tlv_str(f, m.shell_image, 1024);
            case 20: return tlv_str(f, m.saved_agent, 64);
            case 21: return tlv_str(f, m.saved_agent_session, 256);
            case 22: return tlv_str(f, m.saved_agent_dir, kMaxPath);
            case 23: return tlv_bool(f, m.agent_restore);
            default: return true;
        }
    });
}

void encode(const Kill& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::Kill, out);
    b.w().u8(1, static_cast<uint8_t>(m.mode));
    b.w().u32(2, m.grace_ms);
    for (uint32_t t : m.targets) b.w().u32(3, t);
}

Result<void> decode(std::span<const std::byte> payload, Kill& m) {
    m = Kill{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: {
                uint8_t v = 0;
                if (!tlv_u8(f, v) || v > 5) return false;
                m.mode = static_cast<KillMode>(v);
                return true;
            }
            case 2: return tlv_u32(f, m.grace_ms);
            case 3: {
                uint32_t v = 0;
                if (!tlv_u32(f, v) || m.targets.size() >= 4096) return false;
                m.targets.push_back(v);
                return true;
            }
            default: return true;
        }
    });
}

void encode(const KillResult& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::KillResult, out);
    b.w().u8(1, m.ok ? 1 : 0);
    b.w().str(2, m.detail);
}

Result<void> decode(std::span<const std::byte> payload, KillResult& m) {
    m = KillResult{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_bool(f, m.ok);
            case 2: return tlv_str(f, m.detail);
            default: return true;
        }
    });
}

void encode(const ExitStatus& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::ExitStatus, out);
    b.w().i32(1, m.code);
    b.w().i32(2, m.signal);
    b.w().u8(3, m.core ? 1 : 0);
    b.w().u64(4, m.final_seq);
    b.w().u64(5, m.ended_unix_ms);
}

Result<void> decode(std::span<const std::byte> payload, ExitStatus& m) {
    m = ExitStatus{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_i32(f, m.code);
            case 2: return tlv_i32(f, m.signal);
            case 3: return tlv_bool(f, m.core);
            case 4: return tlv_u64(f, m.final_seq);
            case 5: return tlv_u64(f, m.ended_unix_ms);
            default: return true;
        }
    });
}

void encode(const ExitAck& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::ExitAck, out);
    b.w().u8(1, m.pane_closed ? 1 : 0);
}

Result<void> decode(std::span<const std::byte> payload, ExitAck& m) {
    m = ExitAck{};
    return for_fields(payload, [&](const TlvField& f) { return f.tag == 1 ? tlv_bool(f, m.pane_closed) : true; });
}

void encode(const SetOption& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::SetOption, out);
    b.w().str(1, m.key);
    b.w().str(2, m.value);
    b.w().u64(3, m.seq);
}

Result<void> decode(std::span<const std::byte> payload, SetOption& m) {
    m = SetOption{};
    auto r = for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_str(f, m.key, 256);
            case 2: return tlv_str(f, m.value);
            case 3: return tlv_u64(f, m.seq);
            default: return true;
        }
    });
    if (!r) return r;
    if (m.key.empty()) return bad("SET_OPTION without key");
    return {};
}

// --------------------------------------------------------------------------- agent context

namespace {

void encode_limit(TlvWriter& w, uint16_t tag, const AgentLimit& l) {
    std::vector<std::byte> sub;
    TlvWriter lw(sub);
    lw.str(1, l.name);
    lw.u32(2, l.used_percent_x100);
    lw.u32(3, l.window_minutes);
    lw.u64(4, l.resets_at_unix);
    w.bytes(tag, sub);
}

bool decode_limit(const TlvField& f, std::vector<AgentLimit>& out) {
    if (out.size() >= kMaxAgentLimits) return true;   // extra limits are dropped
    AgentLimit l;
    auto r = for_fields(f.value, [&](const TlvField& g) {
        switch (g.tag) {
            case 1: return tlv_str(g, l.name, 64);
            case 2: return tlv_u32(g, l.used_percent_x100);
            case 3: return tlv_u32(g, l.window_minutes);
            case 4: return tlv_u64(g, l.resets_at_unix);
            default: return true;
        }
    });
    if (!r) return false;
    out.push_back(std::move(l));
    return true;
}

template <class T>
bool tlv_opt_u64(const TlvField& f, std::optional<T>& v) {
    uint64_t x = 0;
    if (!tlv_u64(f, x)) return false;
    v = static_cast<T>(x);
    return true;
}

bool tlv_opt_u32(const TlvField& f, std::optional<uint32_t>& v) {
    uint32_t x = 0;
    if (!tlv_u32(f, x)) return false;
    v = x;
    return true;
}

}  // namespace

void encode(const AgentContext& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::AgentContext, out);
    auto& w = b.w();
    w.u16(1, m.version);
    w.u8(2, static_cast<uint8_t>(m.status));
    if (!m.agent.empty()) w.str(3, m.agent);
    if (!m.session_id.empty()) w.str(4, m.session_id);
    if (!m.model.empty()) w.str(5, m.model);
    if (!m.agent_version.empty()) w.str(6, m.agent_version);
    if (m.used_tokens) w.u64(7, *m.used_tokens);
    if (m.window_tokens) w.u64(8, *m.window_tokens);
    w.u8(9, m.window_source);
    if (m.percent_x100) w.u32(10, *m.percent_x100);
    w.u64(11, m.input_tokens);
    w.u64(12, m.output_tokens);
    w.u64(13, m.cache_read_tokens);
    w.u64(14, m.cache_write_tokens);
    w.u32(15, m.turns);
    w.u32(16, m.compactions);
    w.u32(17, m.api_calls);
    w.u8(18, m.flags);
    w.u8(19, m.activity);
    w.u64(20, m.updated_unix_ms);
    for (std::size_t i = 0; i < m.limits.size() && i < kMaxAgentLimits; ++i) encode_limit(w, 21, m.limits[i]);
    w.u8(22, m.used_source);
    w.u8(23, m.limits_source);
    if (m.cost_micro_usd) w.u64(24, *m.cost_micro_usd);
    if (!m.format_version.empty()) w.str(25, m.format_version);
    w.u8(26, m.warn_percent);
    w.u8(27, m.alert_percent);
    w.u64(28, m.seq);
    if (!m.session_name.empty()) w.str(29, m.session_name);
    if (!m.session_cwd.empty()) w.str(30, m.session_cwd);
    if (m.limits_unix_ms) w.u64(31, m.limits_unix_ms);
}

Result<void> decode(std::span<const std::byte> payload, AgentContext& m) {
    m = AgentContext{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_u16(f, m.version);
            case 2: {
                uint8_t v = 0;
                if (!tlv_u8(f, v)) return false;
                // A newer status this build does not know reads as "no data".
                m.status = v <= 3 ? static_cast<AgentContextStatus>(v) : AgentContextStatus::NoData;
                return true;
            }
            case 3: return tlv_str(f, m.agent, 64);
            case 4: return tlv_str(f, m.session_id, 256);
            case 5: return tlv_str(f, m.model, 256);
            case 6: return tlv_str(f, m.agent_version, 64);
            case 7: return tlv_opt_u64(f, m.used_tokens);
            case 8: return tlv_opt_u64(f, m.window_tokens);
            case 9: return tlv_u8(f, m.window_source);
            case 10: return tlv_opt_u32(f, m.percent_x100);
            case 11: return tlv_u64(f, m.input_tokens);
            case 12: return tlv_u64(f, m.output_tokens);
            case 13: return tlv_u64(f, m.cache_read_tokens);
            case 14: return tlv_u64(f, m.cache_write_tokens);
            case 15: return tlv_u32(f, m.turns);
            case 16: return tlv_u32(f, m.compactions);
            case 17: return tlv_u32(f, m.api_calls);
            case 18: return tlv_u8(f, m.flags);
            case 19: return tlv_u8(f, m.activity);
            case 20: return tlv_u64(f, m.updated_unix_ms);
            case 21: return decode_limit(f, m.limits);
            case 22: return tlv_u8(f, m.used_source);
            case 23: return tlv_u8(f, m.limits_source);
            case 24: return tlv_opt_u64(f, m.cost_micro_usd);
            case 25: return tlv_str(f, m.format_version, 64);
            case 26: return tlv_u8(f, m.warn_percent);
            case 27: return tlv_u8(f, m.alert_percent);
            case 28: return tlv_u64(f, m.seq);
            case 29: return tlv_str(f, m.session_name, 256);
            case 30: return tlv_str(f, m.session_cwd, 32 * 1024);
            case 31: return tlv_u64(f, m.limits_unix_ms);
            default: return true;
        }
    });
}

void encode(const AgentStatusline& m, std::vector<std::byte>& out) {
    FrameBuilder b(Type::AgentStatusline, out);
    auto& w = b.w();
    if (!m.session_id.empty()) w.str(1, m.session_id);
    if (!m.transcript_path.empty()) w.str(2, m.transcript_path);
    if (!m.model_id.empty()) w.str(3, m.model_id);
    if (!m.model_name.empty()) w.str(4, m.model_name);
    if (!m.version.empty()) w.str(5, m.version);
    if (m.window_tokens) w.u64(6, *m.window_tokens);
    if (m.used_tokens) w.u64(7, *m.used_tokens);
    if (m.used_percent_x100) w.u32(8, *m.used_percent_x100);
    if (m.cost_micro_usd) w.u64(9, *m.cost_micro_usd);
    if (m.total_input) w.u64(10, *m.total_input);
    if (m.total_output) w.u64(11, *m.total_output);
    for (std::size_t i = 0; i < m.limits.size() && i < kMaxAgentLimits; ++i) encode_limit(w, 12, m.limits[i]);
    if (!m.pane.empty()) w.str(13, m.pane);
}

Result<void> decode(std::span<const std::byte> payload, AgentStatusline& m) {
    m = AgentStatusline{};
    return for_fields(payload, [&](const TlvField& f) {
        switch (f.tag) {
            case 1: return tlv_str(f, m.session_id, 256);
            case 2: return tlv_str(f, m.transcript_path, kMaxPath);
            case 3: return tlv_str(f, m.model_id, 256);
            case 4: return tlv_str(f, m.model_name, 256);
            case 5: return tlv_str(f, m.version, 64);
            case 6: return tlv_opt_u64(f, m.window_tokens);
            case 7: return tlv_opt_u64(f, m.used_tokens);
            case 8: return tlv_opt_u32(f, m.used_percent_x100);
            case 9: return tlv_opt_u64(f, m.cost_micro_usd);
            case 10: return tlv_opt_u64(f, m.total_input);
            case 11: return tlv_opt_u64(f, m.total_output);
            case 12: return decode_limit(f, m.limits);
            case 13: return tlv_str(f, m.pane, 256);
            default: return true;
        }
    });
}

// ----------------------------------------------------------------------------- negotiation

std::optional<uint16_t> negotiate_level(uint16_t a_min, uint16_t a_max, uint16_t b_min, uint16_t b_max) {
    if (a_min > a_max || b_min > b_max) return std::nullopt;
    const uint16_t level = std::min(a_max, b_max);
    if (level < std::max(a_min, b_min)) return std::nullopt;
    return level;
}

std::optional<uint16_t> pick_snapshot_major(uint16_t host_min, uint16_t host_max, uint16_t client_min,
                                            uint16_t client_max) {
    return negotiate_level(host_min, host_max, client_min, client_max);
}

// ----------------------------------------------------------------------------- FrameBuffer

void FrameBuffer::append(std::span<const std::byte> bytes) { buf_.insert(buf_.end(), bytes.begin(), bytes.end()); }

Result<std::optional<FrameView>> FrameBuffer::next() {
    FrameView f;
    auto n = decode_frame(std::span(buf_).subspan(pos_), f);
    if (!n) return Unexpected(n.error());
    if (*n == 0) return std::optional<FrameView>{};
    pos_ += *n;
    return std::optional<FrameView>(f);
}

void FrameBuffer::compact() {
    if (pos_ == 0) return;
    if (pos_ >= buf_.size()) {
        buf_.clear();
    } else {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    }
    pos_ = 0;
}

}  // namespace kt::session::proto
