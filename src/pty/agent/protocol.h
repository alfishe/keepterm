// Console agent pipe protocol. Portable, header-only.
//
// Frames: u32 payload length, u16 type, payload. Little-endian, over a byte-mode named pipe.
// Strings are a u32 byte count followed by the bytes: UTF-8 for text, UTF-16LE for the
// environment block (kept as the raw block the host built, double-NUL terminated).
//
//   HELLO      agent -> host  u32 version, str nonce, u32 agent pid, u32 console code page,
//                             u8 vt_processing_ok, u8 winevent_hook_ok
//   SPAWN      host -> agent  u16 cols, u16 rows, u32 host pid, u64 host creation time (FILETIME),
//                             str command line (UTF-8), str cwd (UTF-8, empty = inherit),
//                             str desktop (UTF-8, empty = inherit), str environment (UTF-16LE block)
//   SPAWNED    agent -> host  u32 error (GetLastError, 0 = ok), u32 child pid, u64 child creation time,
//                             u32 conhost pid
//   OUTPUT     agent -> host  VT bytes (UTF-8), the whole payload
//   INPUT      host -> agent  input bytes from the encoder, the whole payload
//   RESIZE     host -> agent  u16 cols, u16 rows
//   SIGNAL     host -> agent  u8 PtySignal (Interrupt, Break, Hangup, Terminate)
//   FOREGROUND agent -> host  u32 count, count x u32 pid (GetConsoleProcessList, on change)
//   CHILD_EXIT agent -> host  u32 exit code of the child
//   SHUTDOWN   host -> agent  u32 grace in ms; runs the hangup sequence
//
// Order: the agent sends HELLO first; the host checks the nonce and the client PID, then sends
// SPAWN; the agent answers SPAWNED. After that OUTPUT/FOREGROUND/CHILD_EXIT flow one way and
// INPUT/RESIZE/SIGNAL/SHUTDOWN the other. The agent closes the pipe after CHILD_EXIT.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace kt::pty::agent {

inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 6;
inline constexpr uint32_t kMaxPayload = 16u << 20;

enum class MsgType : uint16_t {
    Hello = 1,
    Spawn = 2,
    Spawned = 3,
    Output = 4,
    Input = 5,
    Resize = 6,
    Signal = 7,
    Foreground = 8,
    Stats = 9,  // reserved
    ChildExit = 10,
    Shutdown = 11,
};

class PayloadWriter {
public:
    PayloadWriter& u8(uint8_t v) {
        buf_.push_back(static_cast<char>(v));
        return *this;
    }
    PayloadWriter& u16(uint16_t v) { return put(v, 2); }
    PayloadWriter& u32(uint32_t v) { return put(v, 4); }
    PayloadWriter& u64(uint64_t v) { return put(v, 8); }
    PayloadWriter& str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        buf_.append(s);
        return *this;
    }
    const std::string& data() const { return buf_; }

private:
    PayloadWriter& put(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        return *this;
    }
    std::string buf_;
};

class PayloadReader {
public:
    explicit PayloadReader(std::string_view p) : p_(p) {}
    uint8_t u8() { return static_cast<uint8_t>(get(1)); }
    uint16_t u16() { return static_cast<uint16_t>(get(2)); }
    uint32_t u32() { return static_cast<uint32_t>(get(4)); }
    uint64_t u64() { return get(8); }
    std::string str() {
        const uint32_t n = u32();
        if (!ok_ || n > p_.size() - pos_) {
            ok_ = false;
            return {};
        }
        std::string s(p_.substr(pos_, n));
        pos_ += n;
        return s;
    }
    bool ok() const { return ok_; }

private:
    uint64_t get(std::size_t n) {
        if (!ok_ || p_.size() - pos_ < n) {
            ok_ = false;
            return 0;
        }
        uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) v |= uint64_t{static_cast<uint8_t>(p_[pos_ + i])} << (8 * i);
        pos_ += n;
        return v;
    }
    std::string_view p_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

inline std::string make_frame(MsgType type, std::string_view payload) {
    std::string f;
    f.reserve(kHeaderSize + payload.size());
    const auto n = static_cast<uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
    const auto t = static_cast<uint16_t>(type);
    f.push_back(static_cast<char>(t & 0xFF));
    f.push_back(static_cast<char>(t >> 8));
    f.append(payload);
    return f;
}

struct Frame {
    MsgType type;
    std::string payload;
};

// Accumulates pipe bytes and splits them into frames.
class FrameParser {
public:
    void append(const void* data, std::size_t n) { buf_.append(static_cast<const char*>(data), n); }

    // The next complete frame, or nullopt. Sets bad() on an oversized frame.
    std::optional<Frame> next() {
        if (bad_ || buf_.size() - pos_ < kHeaderSize) return std::nullopt;
        const auto* b = reinterpret_cast<const unsigned char*>(buf_.data() + pos_);
        const uint32_t n = uint32_t{b[0]} | (uint32_t{b[1]} << 8) | (uint32_t{b[2]} << 16) | (uint32_t{b[3]} << 24);
        const auto t = static_cast<uint16_t>(b[4] | (b[5] << 8));
        if (n > kMaxPayload) {
            bad_ = true;
            return std::nullopt;
        }
        if (buf_.size() - pos_ - kHeaderSize < n) return std::nullopt;
        Frame f{static_cast<MsgType>(t), buf_.substr(pos_ + kHeaderSize, n)};
        pos_ += kHeaderSize + n;
        if (pos_ == buf_.size()) {
            buf_.clear();
            pos_ = 0;
        } else if (pos_ > (64u << 10)) {
            buf_.erase(0, pos_);
            pos_ = 0;
        }
        return f;
    }

    bool bad() const { return bad_; }

private:
    std::string buf_;
    std::size_t pos_ = 0;
    bool bad_ = false;
};

}  // namespace kt::pty::agent
