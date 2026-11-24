// VT byte stream parser.
//
// The DEC ANSI parser of Paul Williams (vt100.net) with these xterm extensions:
// UTF-8 before the state machine, ':' subparameters, private markers as a prefix, OSC ended
// by BEL or ST, DCS hook/put/unhook, SOS/PM/APC consumed. The parser does not interpret
// sequences; it hands them to a ParserHandler.
//
// Not yet here: the constexpr transition table and the SIMD scan of the
// ASCII fast path. The parser state is serialized into KTSN snapshots (vt/snapshot.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace kt::vt {

enum class State : uint8_t {
    ground,
    escape,
    escape_intermediate,
    csi_entry,
    csi_param,
    csi_intermediate,
    csi_ignore,
    dcs_entry,
    dcs_param,
    dcs_intermediate,
    dcs_passthrough,
    dcs_ignore,
    osc_string,
    sos_pm_apc_string,
};

// CSI / DCS parameters. Each value is one parameter or subparameter; `starts` marks which
// values begin a new top-level parameter. Empty parameters are 0 (the caller applies defaults).
struct Params {
    static constexpr std::size_t kMax = 32;

    std::array<uint16_t, kMax> values{};
    uint32_t starts = 0;  // bit i: values[i] starts a top-level parameter
    uint8_t count = 0;

    // `count` is kMax + 1 after an overflow; size() is the number of stored values.
    std::size_t size() const { return count < kMax ? count : kMax; }
    bool is_start(std::size_t i) const { return i < kMax && ((starts >> i) & 1u) != 0; }
    // Number of top-level parameters.
    std::size_t top_count() const;
    // Value of top-level parameter `n` (its first value), or `fallback` when absent or 0.
    uint16_t get(std::size_t n, uint16_t fallback) const;
};

struct Sequence {
    Params params;
    char prefix = 0;                     // private marker '<', '=', '>' or '?', or 0
    std::array<char, 2> intermediates{};  // collected 0x20..0x2F bytes
    uint8_t intermediate_count = 0;
    char final = 0;

    std::string_view intermediate_view() const { return {intermediates.data(), intermediate_count}; }
};

class ParserHandler {
public:
    virtual ~ParserHandler() = default;

    // A run of printable ASCII (0x20..0x7E) from the fast path. Default: print() per byte.
    virtual void print_ascii(std::string_view run) {
        for (char c : run) print(static_cast<unsigned char>(c));
    }
    virtual void print(char32_t cp) = 0;           // a decoded code point (U+FFFD for invalid UTF-8)
    virtual void execute(uint8_t control) = 0;     // C0 control
    virtual void esc_dispatch(const Sequence&) = 0;
    virtual void csi_dispatch(const Sequence&) = 0;
    virtual void osc_dispatch(std::string_view payload) = 0;  // without the terminator
    virtual void dcs_hook(const Sequence&) {}
    virtual void dcs_put(uint8_t) {}
    virtual void dcs_unhook() {}
};

struct ParserOptions {
    std::size_t max_osc_bytes = 1u << 20;  // 1 MiB (OSC 52); longer strings are dropped
};

class Parser {
public:
    explicit Parser(ParserOptions opts = {}) : opts_(opts) {}

    void feed(std::span<const std::byte> bytes, ParserHandler& h);
    void feed(std::string_view bytes, ParserHandler& h) {
        feed(std::as_bytes(std::span(bytes.data(), bytes.size())), h);
    }

    State state() const { return state_; }
    void reset();
    // True while osc_dispatch runs for a string ended by BEL (replies use the same terminator).
    bool osc_ended_with_bel() const { return osc_bel_; }

private:
    friend struct ParserCodec;  // KTSN PARSER section (vt/snapshot.cpp)

    void step(uint8_t b, ParserHandler& h);
    void ground_utf8(uint8_t b, ParserHandler& h);
    void flush_utf8(ParserHandler& h);
    void enter(State s);
    void clear();
    void collect(uint8_t b);
    void param(uint8_t b);
    void enter_escape_from_string(ParserHandler& h);
    void abort_string(ParserHandler& h);

    ParserOptions opts_;
    State state_ = State::ground;
    Sequence seq_{};
    bool seq_overflow_ = false;   // too many intermediates
    bool param_started_ = false;  // a digit or separator was seen
    uint32_t param_acc_ = 0;
    bool after_string_ = false;   // ESC that ended a string; a following '\' is its ST

    std::string osc_;
    bool osc_overflow_ = false;
    bool osc_bel_ = false;

    // UTF-8 decoder state (ground only).
    char32_t u_cp_ = 0;
    uint8_t u_need_ = 0;
    uint8_t u_lo_ = 0x80, u_hi_ = 0xBF;
};

}  // namespace kt::vt
