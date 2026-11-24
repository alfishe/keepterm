// OSC dispatch: titles, colors, working directory, hyperlinks, clipboard,
// prompt marks and notifications.
#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

#include "vt/terminal.h"
#include "vt/utf8.h"

namespace kt::vt {
namespace {

std::u8string_view as_u8(std::string_view s) {
    return {reinterpret_cast<const char8_t*>(s.data()), s.size()};
}

// Splits "<number>;<rest>". Returns false when the payload does not start with a number.
bool split_command(std::string_view payload, unsigned& cmd, std::string_view& rest) {
    const std::size_t semi = payload.find(';');
    const std::string_view head = payload.substr(0, semi);
    if (head.empty()) return false;
    const auto r = std::from_chars(head.data(), head.data() + head.size(), cmd);
    if (r.ec != std::errc{} || r.ptr != head.data() + head.size()) return false;
    rest = semi == std::string_view::npos ? std::string_view{} : payload.substr(semi + 1);
    return true;
}

// Standard base64 (RFC 4648), padding optional. Returns nullopt on an invalid character.
std::optional<std::vector<std::byte>> base64_decode(std::string_view in, std::size_t max_out) {
    std::array<int8_t, 256> map{};
    map.fill(-1);
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
    std::vector<std::byte> out;
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=') break;
        const int v = map[static_cast<unsigned char>(ch)];
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((acc >> bits) & 0xFFu));
            if (out.size() > max_out) return std::nullopt;
        }
    }
    return out;
}

// xterm's default 256-color palette.
uint32_t default_palette(unsigned i) {
    static constexpr uint32_t kBase[16] = {0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
                                           0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
                                           0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
    if (i < 16) return kBase[i];
    if (i < 232) {
        const unsigned v = i - 16;
        const auto level = [](unsigned c) { return c == 0 ? 0u : 55u + 40u * c; };
        return (level(v / 36) << 16) | (level((v / 6) % 6) << 8) | level(v % 6);
    }
    const unsigned g = 8u + 10u * (i - 232u);
    return (g << 16) | (g << 8) | g;
}

std::string rgb_spec(uint32_t c) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "rgb:";
    for (int shift : {16, 8, 0}) {
        const unsigned v = (c >> shift) & 0xFFu;
        for (int k = 0; k < 2; ++k) {  // 8 bits -> 16 bits: the byte twice
            out.push_back(kHex[v >> 4]);
            out.push_back(kHex[v & 0xFu]);
        }
        if (shift != 0) out.push_back('/');
    }
    return out;
}

constexpr uint32_t kDefaultFg = 0xd0d0d0, kDefaultBg = 0x000000;

// XParseColor forms: rgb:h/h/h with 1 to 4 hex digits per channel, and #rgb, #rrggbb,
// #rrrgggbbb, #rrrrggggbbbb. Color names are not supported (no X11 database here).
std::optional<uint32_t> parse_color_spec(std::string_view spec) {
    const auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    // A channel of n hex digits scaled to 8 bits.
    const auto channel = [&hex](std::string_view d) -> int {
        if (d.empty() || d.size() > 4) return -1;
        unsigned v = 0;
        for (char c : d) {
            const int h = hex(c);
            if (h < 0) return -1;
            v = v * 16u + static_cast<unsigned>(h);
        }
        const unsigned max = (1u << (4u * d.size())) - 1u;
        return static_cast<int>((v * 255u + max / 2u) / max);
    };
    std::array<int, 3> ch{-1, -1, -1};
    if (spec.substr(0, 4) == "rgb:") {
        std::string_view r = spec.substr(4);
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t slash = r.find('/');
            if ((slash == std::string_view::npos) != (i == 2)) return std::nullopt;
            ch[i] = channel(r.substr(0, slash));
            r = slash == std::string_view::npos ? std::string_view{} : r.substr(slash + 1);
        }
    } else if (!spec.empty() && spec[0] == '#') {
        const std::string_view d = spec.substr(1);
        if (d.empty() || d.size() % 3 != 0 || d.size() > 12) return std::nullopt;
        const std::size_t n = d.size() / 3;
        // #rgb means the high digits: #f00 is ff0000, as in XParseColor.
        for (std::size_t i = 0; i < 3; ++i) {
            const std::string_view c = d.substr(i * n, n);
            const std::string_view hi = c.substr(0, std::min<std::size_t>(n, 2));
            ch[i] = channel(hi);
            if (ch[i] >= 0 && hi.size() == 1) ch[i] = ch[i] / 17 * 16;
        }
    } else {
        return std::nullopt;
    }
    for (int v : ch) {
        if (v < 0) return std::nullopt;
    }
    return (static_cast<uint32_t>(ch[0]) << 16) | (static_cast<uint32_t>(ch[1]) << 8) | static_cast<uint32_t>(ch[2]);
}

}  // namespace

// The side channel's handler: only OSC 7, 9;9 and 133 reach the terminal.
struct Terminal::IntegrationHandler final : ParserHandler {
    Terminal& t;
    explicit IntegrationHandler(Terminal& term) : t(term) {}
    void print(char32_t) override {}
    void print_ascii(std::string_view) override {}
    void execute(uint8_t) override {}
    void esc_dispatch(const Sequence&) override {}
    void csi_dispatch(const Sequence&) override {}
    void osc_dispatch(std::string_view payload) override {
        if (!(payload.substr(0, 2) == "7;" || payload.substr(0, 4) == "9;9;" || payload.substr(0, 4) == "133;" ||
              payload.substr(0, 4) == "633;")) {
            return;
        }
        t.integ_ = true;
        t.osc_dispatch(payload);
        t.integ_ = false;
    }
};

void Terminal::set_theme(const ThemeColors& theme) {
    // Colors are 0xRRGGBB; stray high bits would not survive a snapshot round trip.
    const auto rgb = [](std::optional<uint32_t> c) { return c ? std::optional<uint32_t>(*c & 0xFFFFFFu) : c; };
    theme_.fg = rgb(theme.fg);
    theme_.bg = rgb(theme.bg);
    theme_.cursor = rgb(theme.cursor);
    for (std::size_t i = 0; i < theme_.palette.size(); ++i) theme_.palette[i] = rgb(theme.palette[i]);
}

void Terminal::feed_integration(std::span<const std::byte> bytes) {
    constexpr std::size_t kMaxOsc = 4096;
    if (!integ_parser_) integ_parser_ = std::make_unique<Parser>(ParserOptions{kMaxOsc});
    IntegrationHandler h(*this);
    integ_parser_->feed(bytes, h);
    if (publisher_) publish();
}

void Terminal::set_default_cursor(CursorShape shape, bool blink) {
    default_shape_ = shape;
    default_blink_ = blink;
    if (!shape_default_) return;
    if (shape_ == shape && dec(12) == blink) return;
    shape_ = shape;
    dec_.set(static_cast<std::size_t>(dec_slot(12)), blink);
    force_publish_ = true;
}

ThemeColors Terminal::effective_colors() const {
    ThemeColors c = theme_;
    if (color_overrides_.fg) c.fg = color_overrides_.fg;
    if (color_overrides_.bg) c.bg = color_overrides_.bg;
    if (color_overrides_.cursor) c.cursor = color_overrides_.cursor;
    for (std::size_t i = 0; i < c.palette.size(); ++i) {
        if (color_overrides_.palette[i]) c.palette[i] = color_overrides_.palette[i];
    }
    return c;
}

// OSC 4 ; n ; spec [; n ; spec ...] and OSC 10/11/12 ; spec [; spec ...]. A spec of "?" asks
// for the color; anything parse_color_spec reads sets a program override.
void Terminal::osc_color(unsigned cmd, std::string_view rest) {
    const std::string_view st = parser_.osc_ended_with_bel() ? "\x07" : "\x1b\\";
    const ThemeColors eff = effective_colors();
    bool changed = false;
    if (cmd == 4) {
        while (!rest.empty()) {
            const std::size_t s1 = rest.find(';');
            if (s1 == std::string_view::npos) break;
            const std::string_view idx_s = rest.substr(0, s1);
            std::string_view tail = rest.substr(s1 + 1);
            const std::size_t s2 = tail.find(';');
            const std::string_view spec = tail.substr(0, s2);
            rest = s2 == std::string_view::npos ? std::string_view{} : tail.substr(s2 + 1);
            unsigned idx = 0;
            const auto r = std::from_chars(idx_s.data(), idx_s.data() + idx_s.size(), idx);
            if (r.ec != std::errc{} || r.ptr != idx_s.data() + idx_s.size() || idx > 255) continue;
            if (spec == "?") {
                const uint32_t c = eff.palette[idx].value_or(default_palette(idx));
                reply("\x1b]4;" + std::to_string(idx) + ";" + rgb_spec(c) + std::string(st));
            } else if (const auto c = parse_color_spec(spec)) {
                if (color_overrides_.palette[idx] != c) changed = true;
                color_overrides_.palette[idx] = c;
            }
        }
    } else {
        // OSC 10 ; a ; b addresses 10 and 11 in turn (xterm).
        for (unsigned c = cmd; c <= 12 && !rest.empty(); ++c) {
            const std::size_t semi = rest.find(';');
            const std::string_view spec = rest.substr(0, semi);
            rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
            std::optional<uint32_t>& slot =
                c == 10 ? color_overrides_.fg : c == 11 ? color_overrides_.bg : color_overrides_.cursor;
            if (spec == "?") {
                const uint32_t fg = eff.fg.value_or(kDefaultFg);
                const uint32_t v = c == 10 ? fg : c == 11 ? eff.bg.value_or(kDefaultBg) : eff.cursor.value_or(fg);
                reply("\x1b]" + std::to_string(c) + ";" + rgb_spec(v) + std::string(st));
            } else if (const auto v = parse_color_spec(spec)) {
                if (slot != v) changed = true;
                slot = v;
            }
        }
    }
    if (changed && sink_) sink_->colors_changed();
}

// OSC 104 [; n ...] resets palette overrides (all without arguments); OSC 110/111/112 reset
// the foreground, background and cursor color.
void Terminal::osc_color_reset(unsigned cmd, std::string_view rest) {
    bool changed = false;
    const auto clear = [&changed](std::optional<uint32_t>& slot) {
        if (slot) changed = true;
        slot.reset();
    };
    if (cmd == 104) {
        if (rest.empty()) {
            for (auto& c : color_overrides_.palette) clear(c);
        }
        while (!rest.empty()) {
            const std::size_t semi = rest.find(';');
            const std::string_view idx_s = rest.substr(0, semi);
            rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
            unsigned idx = 0;
            const auto r = std::from_chars(idx_s.data(), idx_s.data() + idx_s.size(), idx);
            if (r.ec == std::errc{} && r.ptr == idx_s.data() + idx_s.size() && idx <= 255) clear(color_overrides_.palette[idx]);
        }
    } else {
        clear(cmd == 110 ? color_overrides_.fg : cmd == 111 ? color_overrides_.bg : color_overrides_.cursor);
    }
    if (changed && sink_) sink_->colors_changed();
}

// OSC 9 ; text (iTerm2 notification), OSC 9 ; 4 ; state ; percent (ConEmu progress).
void Terminal::osc_notify(std::string_view rest) {
    if (!sink_) return;
    if (rest.substr(0, 2) == "4;" || rest == "4") {
        std::string_view a = rest.size() > 2 ? rest.substr(2) : std::string_view{};
        const std::size_t semi = a.find(';');
        const std::string_view st_s = a.substr(0, semi);
        const std::string_view pct_s = semi == std::string_view::npos ? std::string_view{} : a.substr(semi + 1);
        unsigned st = 0, pct = 0;
        (void)std::from_chars(st_s.data(), st_s.data() + st_s.size(), st);
        (void)std::from_chars(pct_s.data(), pct_s.data() + pct_s.size(), pct);
        if (st > 4) return;
        sink_->progress(static_cast<ProgressState>(st), static_cast<uint8_t>(pct > 100 ? 100 : pct));
        return;
    }
    if (!rest.empty() && rest[0] >= '0' && rest[0] <= '9' && rest.size() > 1 && rest[1] == ';') return;  // other ConEmu
    sink_->notify(std::u8string_view{}, as_u8(rest));
}

void Terminal::osc_dispatch(std::string_view payload) {
    unsigned cmd = 0;
    std::string_view rest;
    if (!split_command(payload, cmd, rest)) return;
    switch (cmd) {
        case 0:
        case 1:
        case 2:
            if (cmd != 2) icon_name_ = as_u8(rest);
            if (cmd != 1) {
                title_ = as_u8(rest);
                if (sink_) sink_->title_changed(title_);
            }
            return;
        case 4:
        case 10:
        case 11:
        case 12:
            if (!integ_) osc_color(cmd, rest);
            return;
        case 104:
        case 110:
        case 111:
        case 112:
            if (!integ_) osc_color_reset(cmd, rest);
            return;
        case 7:
            cwd_ = as_u8(rest);
            if (sink_) sink_->cwd_changed(CwdReport{CwdReport::Form::Osc7, cwd_, integ_});
            return;
        case 9:
            if (rest.substr(0, 2) == "9;") {  // ConEmu: current directory, the path as sent
                cwd_ = as_u8(rest.substr(2));
                if (sink_) sink_->cwd_changed(CwdReport{CwdReport::Form::Osc9_9, cwd_, integ_});
                return;
            }
            if (!integ_) osc_notify(rest);
            return;
        case 8: osc_hyperlink(rest); return;
        case 52: osc_clipboard(rest); return;
        case 133: osc_prompt(rest); return;
        case 633: osc_vscode(rest); return;
        case 777:  // OSC 777 ; notify ; title ; body (rxvt-unicode, also VTE)
            if (!integ_ && sink_ && rest.substr(0, 7) == "notify;") {
                const std::string_view tb = rest.substr(7);
                const std::size_t semi = tb.find(';');
                const std::string_view title = tb.substr(0, semi);
                const std::string_view body = semi == std::string_view::npos ? std::string_view{} : tb.substr(semi + 1);
                sink_->notify(as_u8(title), as_u8(body));
            }
            return;
        default: return;  // unknown: ignored
    }
}

// OSC 8 ; params ; URI. An empty URI closes the link.
void Terminal::osc_hyperlink(std::string_view rest) {
    const std::size_t semi = rest.find(';');
    if (semi == std::string_view::npos) return;
    const std::string_view params = rest.substr(0, semi);
    const std::string_view uri = rest.substr(semi + 1);
    if (uri.empty()) {
        link_ = 0;
        return;
    }
    std::string_view id;
    std::string_view ps = params;
    while (!ps.empty()) {
        const std::size_t colon = ps.find(':');
        const std::string_view kv = ps.substr(0, colon);
        if (kv.substr(0, 3) == "id=") id = kv.substr(3);
        if (colon == std::string_view::npos) break;
        ps = ps.substr(colon + 1);
    }
    if (links_.needs_collect() && collect_due()) collect_tables();
    link_ = links_.intern(id, uri);  // 0 (no link) when the URI is too long or the table is full
}

// OSC 52 ; targets ; base64 | ?
void Terminal::osc_clipboard(std::string_view rest) {
    if (opts_.osc52 == Osc52Policy::Off || !sink_) return;
    const std::size_t semi = rest.find(';');
    if (semi == std::string_view::npos) return;
    const std::string_view targets = rest.substr(0, semi);
    const std::string_view data = rest.substr(semi + 1);
    ClipboardSel sel = ClipboardSel::Clipboard;
    for (char t : targets) {
        if (t == 'c') { sel = ClipboardSel::Clipboard; break; }
        if (t == 'p') { sel = ClipboardSel::Primary; break; }
        if (t == 's') { sel = ClipboardSel::Selection; break; }
    }
    if (data == "?") {
        if (opts_.osc52 == Osc52Policy::ReadWrite) sink_->clipboard_read_request(sel);
        return;
    }
    constexpr std::size_t kMaxDecoded = 1u << 20;
    auto bytes = base64_decode(data, kMaxDecoded);
    if (!bytes) return;
    sink_->clipboard_write(sel, *bytes);
}

// OSC 633 (VS Code shell integration): A/B/C/D are the OSC 133 marks; P;Cwd=<path> reports
// the directory. E (the command line) and the other properties are not used.
void Terminal::osc_vscode(std::string_view rest) {
    if (rest.empty()) return;
    if (rest[0] == 'A' || rest[0] == 'B' || rest[0] == 'C' || rest[0] == 'D') {
        if (rest.size() == 1 || rest[1] == ';') osc_prompt(rest);
        return;
    }
    if (rest.substr(0, 6) == "P;Cwd=") {
        cwd_ = as_u8(rest.substr(6));
        if (sink_) sink_->cwd_changed(CwdReport{CwdReport::Form::Osc633, cwd_, integ_});
    }
}

// OSC 133 ; A|B|C|D [; options]
void Terminal::osc_prompt(std::string_view rest) {
    if (rest.empty()) return;
    const char kind = rest[0];
    std::string_view opts = rest.size() > 1 && rest[1] == ';' ? rest.substr(2) : std::string_view{};
    PromptMark mark;
    uint16_t flag = 0;
    switch (kind) {
        case 'A': mark = PromptMark::PromptStart; flag = kRowPromptStart; break;
        case 'B': mark = PromptMark::InputStart; flag = kRowInputStart; break;
        case 'C': mark = PromptMark::OutputStart; flag = kRowOutputStart; break;
        case 'D': mark = PromptMark::CommandFinished; break;
        default: return;
    }
    if (flag != 0) active_->set_flag(y_, flag, true);
    active_->mark_dirty(y_);
    if (mark == PromptMark::InputStart || (mark == PromptMark::PromptStart && !input_from_b_)) {
        input_abs_ = cursor_abs_line();
        input_col_ = x_;
        input_from_b_ = mark == PromptMark::InputStart;
    }
    if (!sink_) return;
    sink_->prompt_mark(mark, cursor_abs_line());
    if (mark == PromptMark::OutputStart) {
        sink_->command_started(input_text());
        input_abs_ = UINT64_MAX;
        input_from_b_ = false;
    }
    if (mark == PromptMark::CommandFinished) {
        std::optional<int> code;
        const std::string_view first = opts.substr(0, opts.find(';'));
        int v = 0;
        const auto r = std::from_chars(first.data(), first.data() + first.size(), v);
        if (!first.empty() && r.ec == std::errc{} && r.ptr == first.data() + first.size()) code = v;
        sink_->command_finished(code);
    }
}

std::u8string Terminal::input_text() const {
    constexpr std::size_t kMax = 4096;
    std::u8string out;
    if (input_abs_ == UINT64_MAX || alt_active()) return out;
    const uint64_t top = scrollback_.next_abs_line();
    const uint64_t cur = cursor_abs_line();
    if (input_abs_ > cur) return out;
    // The rows from the mark to the cursor; a cursor at column 0 below the mark ends the text
    // on the row above (the shell echoed the newline of Enter).
    uint64_t last = cur;
    if (x_ == 0 && !pending_wrap_ && cur > input_abs_) --last;
    for (uint64_t abs = std::max(input_abs_, top); abs <= last; ++abs) {
        const auto y = static_cast<uint16_t>(abs - top);
        const auto r = active_->row(y);
        uint16_t from = abs == input_abs_ ? input_col_ : 0;
        uint16_t to = cols();
        if (abs == cur) to = pending_wrap_ ? static_cast<uint16_t>(x_ + 1u) : x_;
        if (abs > std::max(input_abs_, top) && !(active_->flags(static_cast<uint16_t>(y - 1)) & kRowWrapped)) {
            out.push_back(u8' ');
        }
        const std::size_t row_start = out.size();
        for (uint16_t x = from; x < to && x < cols(); ++x) {
            const Cell& c = r[x];
            if (c.width() == CellWidth::WideTail || c.width() == CellWidth::Empty) continue;
            if (c.cluster()) {
                for (char32_t cp : graphemes_[c.cp()]) append_utf8(out, cp);
            } else {
                append_utf8(out, c.cp() == 0 ? U' ' : c.cp());
            }
            if (out.size() > kMax) break;
        }
        // Trailing blanks of a row that does not wrap.
        if (!(active_->flags(y) & kRowWrapped)) {
            while (out.size() > row_start && out.back() == u8' ') out.pop_back();
        }
        if (out.size() > kMax) break;
    }
    if (out.size() > kMax) {
        // Cut before the sequence that crosses the limit.
        std::size_t n = kMax;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0u) == 0x80u) --n;
        out.resize(n);
    }
    std::size_t b = 0;
    while (b < out.size() && out[b] == u8' ') ++b;
    out.erase(0, b);
    while (!out.empty() && out.back() == u8' ') out.pop_back();
    return out;
}

}  // namespace kt::vt
