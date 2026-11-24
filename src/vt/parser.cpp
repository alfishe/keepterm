#include "vt/parser.h"

namespace kt::vt {
namespace {

constexpr uint8_t kEsc = 0x1B;
constexpr uint8_t kCan = 0x18;
constexpr uint8_t kSub = 0x1A;
constexpr uint8_t kBel = 0x07;
constexpr uint8_t kDel = 0x7F;
constexpr char32_t kReplacement = 0xFFFD;

bool is_c0(uint8_t b) { return b < 0x20; }

}  // namespace

std::size_t Params::top_count() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < size(); ++i) n += is_start(i) ? 1u : 0u;
    return n;
}

uint16_t Params::get(std::size_t n, uint16_t fallback) const {
    std::size_t top = 0;
    for (std::size_t i = 0; i < size(); ++i) {
        if (!is_start(i)) continue;
        if (top == n) return values[i] != 0 ? values[i] : fallback;
        ++top;
    }
    return fallback;
}

void Parser::reset() {
    state_ = State::ground;
    clear();
    osc_.clear();
    osc_overflow_ = false;
    u_need_ = 0;
    after_string_ = false;
}

void Parser::clear() {
    seq_ = Sequence{};
    seq_overflow_ = false;
    param_started_ = false;
    param_acc_ = 0;
}

void Parser::enter(State s) {
    state_ = s;
    switch (s) {
        case State::escape:
        case State::csi_entry:
        case State::dcs_entry:
            clear();
            break;
        case State::osc_string:
            osc_.clear();
            osc_overflow_ = false;
            break;
        default:
            break;
    }
}

void Parser::collect(uint8_t b) {
    if (seq_.intermediate_count < seq_.intermediates.size()) {
        seq_.intermediates[seq_.intermediate_count++] = static_cast<char>(b);
    } else {
        seq_overflow_ = true;
    }
}

void Parser::param(uint8_t b) {
    auto& p = seq_.params;
    if (!param_started_) {
        param_started_ = true;
        p.count = 1;
        p.values[0] = 0;
        p.starts = 1;
    }
    if (b >= '0' && b <= '9') {
        param_acc_ = param_acc_ * 10 + (b - '0');
        if (param_acc_ > 0xFFFF) param_acc_ = 0xFFFF;  // saturate
        if (p.count <= Params::kMax) p.values[p.count - 1u] = static_cast<uint16_t>(param_acc_);
        return;
    }
    // ';' starts a top-level parameter, ':' a subparameter. Extra parameters are ignored.
    param_acc_ = 0;
    if (p.count < Params::kMax) {
        p.values[p.count] = 0;
        if (b == ';') p.starts |= (1u << p.count);
        ++p.count;
    } else {
        p.count = Params::kMax + 1;  // marks overflow; values stay at kMax entries
    }
}

void Parser::flush_utf8(ParserHandler& h) {
    if (u_need_ != 0) {
        u_need_ = 0;
        h.print(kReplacement);
    }
}

// Unicode §3.9 well-formed UTF-8, one U+FFFD per maximal invalid subpart.
void Parser::ground_utf8(uint8_t b, ParserHandler& h) {
    if (u_need_ != 0) {
        if (b >= u_lo_ && b <= u_hi_) {
            u_cp_ = (u_cp_ << 6) | (b & 0x3Fu);
            u_lo_ = 0x80;
            u_hi_ = 0xBF;
            if (--u_need_ == 0) h.print(u_cp_);
            return;
        }
        u_need_ = 0;
        h.print(kReplacement);
        // fall through: b starts anew
    }
    if (b < 0x80) {
        step(b, h);
        return;
    }
    u_lo_ = 0x80;
    u_hi_ = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        u_need_ = 1;
        u_cp_ = b & 0x1Fu;
    } else if (b >= 0xE0 && b <= 0xEF) {
        u_need_ = 2;
        u_cp_ = b & 0x0Fu;
        if (b == 0xE0) u_lo_ = 0xA0;
        if (b == 0xED) u_hi_ = 0x9F;  // no surrogates
    } else if (b >= 0xF0 && b <= 0xF4) {
        u_need_ = 3;
        u_cp_ = b & 0x07u;
        if (b == 0xF0) u_lo_ = 0x90;
        if (b == 0xF4) u_hi_ = 0x8F;  // <= U+10FFFF
    } else {
        h.print(kReplacement);  // 0x80..0xC1, 0xF5..0xFF
    }
}

void Parser::enter_escape_from_string(ParserHandler& h) {
    if (state_ == State::osc_string) {
        osc_bel_ = false;
        if (!osc_overflow_) h.osc_dispatch(osc_);
        osc_.clear();
    } else if (state_ == State::dcs_passthrough) {
        h.dcs_unhook();
    }
    enter(State::escape);
    after_string_ = true;
}

void Parser::abort_string(ParserHandler& h) {
    if (state_ == State::dcs_passthrough) h.dcs_unhook();
    osc_.clear();
    enter(State::ground);
}

void Parser::feed(std::span<const std::byte> bytes, ParserHandler& h) {
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
    const auto* end = p + bytes.size();
    while (p < end) {
        if (state_ == State::ground && u_need_ == 0) {
            // Fast path: a run of printable ASCII.
            const auto* q = p;
            while (q < end && *q >= 0x20 && *q < 0x7F) ++q;
            if (q != p) {
                h.print_ascii(std::string_view(reinterpret_cast<const char*>(p), static_cast<std::size_t>(q - p)));
                p = q;
                continue;
            }
        }
        if (state_ == State::ground) {
            ground_utf8(*p++, h);
        } else {
            step(*p++, h);
        }
    }
}

void Parser::step(uint8_t b, ParserHandler& h) {
    const bool string_state = state_ == State::osc_string || state_ == State::dcs_passthrough ||
                              state_ == State::dcs_ignore || state_ == State::sos_pm_apc_string;

    // "Anywhere" transitions.
    if (b == kEsc) {
        if (string_state) {
            enter_escape_from_string(h);
        } else {
            flush_utf8(h);
            enter(State::escape);
            after_string_ = false;
        }
        return;
    }
    if (b == kCan || b == kSub) {
        if (string_state) {
            abort_string(h);
        } else {
            enter(State::ground);
        }
        h.execute(b);
        return;
    }
    // Outside strings, a non-ASCII byte abandons a sequence and is reprocessed in ground.
    if (b >= 0x80 && !string_state && state_ != State::ground) {
        enter(State::ground);
        ground_utf8(b, h);
        return;
    }

    switch (state_) {
        case State::ground:
            if (is_c0(b)) {
                h.execute(b);
            } else if (b != kDel) {
                h.print(b);
            }
            return;

        case State::escape:
            if (after_string_) {
                after_string_ = false;
                if (b == '\\') {  // ST of the string that just ended
                    enter(State::ground);
                    return;
                }
            }
            if (is_c0(b)) {
                h.execute(b);
            } else if (b >= 0x20 && b <= 0x2F) {
                collect(b);
                state_ = State::escape_intermediate;
            } else if (b == '[') {
                enter(State::csi_entry);
            } else if (b == ']') {
                enter(State::osc_string);
            } else if (b == 'P') {
                enter(State::dcs_entry);
            } else if (b == 'X' || b == '^' || b == '_') {
                enter(State::sos_pm_apc_string);
            } else if (b != kDel) {
                seq_.final = static_cast<char>(b);
                h.esc_dispatch(seq_);
                enter(State::ground);
            }
            return;

        case State::escape_intermediate:
            if (is_c0(b)) {
                h.execute(b);
            } else if (b >= 0x20 && b <= 0x2F) {
                collect(b);
            } else if (b != kDel) {
                seq_.final = static_cast<char>(b);
                if (!seq_overflow_) h.esc_dispatch(seq_);
                enter(State::ground);
            }
            return;

        case State::csi_entry:
        case State::csi_param:
        case State::csi_intermediate:
            if (is_c0(b)) {
                h.execute(b);
                return;
            }
            if (b == kDel) return;
            if (b >= 0x40) {  // final byte
                seq_.final = static_cast<char>(b);
                if (!seq_overflow_) h.csi_dispatch(seq_);
                enter(State::ground);
                return;
            }
            if (b >= 0x20 && b <= 0x2F) {
                collect(b);
                state_ = State::csi_intermediate;
                return;
            }
            // 0x30..0x3F
            if (state_ == State::csi_intermediate) {
                state_ = State::csi_ignore;
            } else if (b >= 0x3C) {  // private marker
                if (state_ == State::csi_entry) {
                    seq_.prefix = static_cast<char>(b);
                    state_ = State::csi_param;
                } else {
                    state_ = State::csi_ignore;
                }
            } else {
                param(b);
                state_ = State::csi_param;
            }
            return;

        case State::csi_ignore:
            if (is_c0(b)) {
                h.execute(b);
            } else if (b >= 0x40 && b != kDel) {
                enter(State::ground);
            }
            return;

        case State::dcs_entry:
        case State::dcs_param:
        case State::dcs_intermediate:
            if (is_c0(b) || b == kDel) return;
            if (b >= 0x40) {
                seq_.final = static_cast<char>(b);
                if (seq_overflow_) {
                    state_ = State::dcs_ignore;
                } else {
                    h.dcs_hook(seq_);
                    state_ = State::dcs_passthrough;
                }
                return;
            }
            if (b >= 0x20 && b <= 0x2F) {
                collect(b);
                state_ = State::dcs_intermediate;
                return;
            }
            if (state_ == State::dcs_intermediate) {
                state_ = State::dcs_ignore;
            } else if (b >= 0x3C) {
                if (state_ == State::dcs_entry) {
                    seq_.prefix = static_cast<char>(b);
                    state_ = State::dcs_param;
                } else {
                    state_ = State::dcs_ignore;
                }
            } else {
                param(b);
                state_ = State::dcs_param;
            }
            return;

        case State::dcs_passthrough:
            if (b != kDel) h.dcs_put(b);
            return;

        case State::dcs_ignore:
        case State::sos_pm_apc_string:
            return;

        case State::osc_string:
            if (b == kBel) {
                osc_bel_ = true;
                if (!osc_overflow_) h.osc_dispatch(osc_);
                osc_.clear();
                enter(State::ground);
                return;
            }
            if (is_c0(b)) return;
            if (osc_overflow_) return;
            if (osc_.size() >= opts_.max_osc_bytes) {
                osc_overflow_ = true;
                osc_.clear();
                osc_.shrink_to_fit();
                return;
            }
            osc_.push_back(static_cast<char>(b));
            return;
    }
}

}  // namespace kt::vt
