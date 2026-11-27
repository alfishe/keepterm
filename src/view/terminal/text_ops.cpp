#include "view/terminal/text_ops.h"

#include <algorithm>
#include <memory>

#include <pcre2.h>

#include "vt/terminal.h"

namespace kt::view {

namespace {

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

std::u32string decode_utf8(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        char32_t c = 0;
        size_t n = 1;
        if (b < 0x80) c = b;
        else if ((b >> 5) == 6) { c = b & 0x1F; n = 2; }
        else if ((b >> 4) == 14) { c = b & 0x0F; n = 3; }
        else if ((b >> 3) == 30) { c = b & 0x07; n = 4; }
        else { ++i; continue; }
        if (i + n > s.size()) break;
        for (size_t k = 1; k < n; ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out += c;
        i += n;
    }
    return out;
}

// Simple case folding for the scripts most terminals show.
char32_t fold(char32_t c) {
    if (c >= U'A' && c <= U'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;          // Latin-1
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;      // Greek
    if (c >= 0x410 && c <= 0x42F) return c + 32;                    // Cyrillic
    if (c >= 0x400 && c <= 0x40F) return c + 80;                    // Ѐ..Џ
    return c;
}

// The visible code point of a cell (0 for blanks and wide tails; the first of a cluster).
char32_t cell_char(const LineSource& src, const vt::Cell& c) {
    if (c.width() == vt::CellWidth::WideTail || c.width() == vt::CellWidth::Empty) return 0;
    if (c.cluster()) {
        const std::u32string_view cl = src.cluster(c.cp());
        return cl.empty() ? U' ' : cl.front();
    }
    return c.cp() == 0 ? U' ' : c.cp();
}

void append_cell(const LineSource& src, const vt::Cell& c, std::string& out) {
    if (c.width() == vt::CellWidth::WideTail || c.width() == vt::CellWidth::Empty) return;
    if (c.cluster()) {
        for (char32_t cp : src.cluster(c.cp())) append_utf8(out, cp);
        return;
    }
    append_utf8(out, c.cp() == 0 ? U' ' : c.cp());
}

char32_t char_at(const LineSource& src, SelPoint p) {
    uint16_t flags = 0;
    const auto cells = src.cells(p.line, flags);
    if (p.col >= cells.size()) return U' ';
    const vt::Cell* c = &cells[p.col];
    if (c->width() == vt::CellWidth::WideTail && p.col > 0) c = &cells[p.col - 1u];
    const char32_t ch = cell_char(src, *c);
    return ch == 0 ? U' ' : ch;
}

bool wrapped(const LineSource& src, uint64_t line) {
    uint16_t flags = 0;
    (void)src.cells(line, flags);
    return (flags & vt::kRowWrapped) != 0;
}

uint16_t last_col(const LineSource& src) { return static_cast<uint16_t>(std::max<int>(0, src.cols() - 1)); }

// One logical line as code points with the cell of each.
struct Logical {
    std::u32string text;
    std::vector<SelPoint> pos;
};

Logical logical_text(const LineSource& src, uint64_t first, uint64_t last, bool trim_rows) {
    Logical l;
    for (uint64_t line = first; line <= last; ++line) {
        uint16_t flags = 0;
        const auto cells = src.cells(line, flags);
        size_t n = cells.size();
        if (trim_rows || !(flags & vt::kRowWrapped)) {
            while (n > 0 && cells[n - 1].is_blank()) --n;
        }
        for (size_t col = 0; col < n; ++col) {
            const vt::Cell& c = cells[col];
            if (c.width() == vt::CellWidth::WideTail || c.width() == vt::CellWidth::Empty) continue;
            l.text += cell_char(src, c);
            l.pos.push_back(SelPoint{line, static_cast<uint16_t>(col)});
        }
        // A wrapped row that ends in blanks still joins without a newline; pad the soft wrap.
        if ((flags & vt::kRowWrapped) && n < src.cols() && line < last) {
            for (size_t col = n; col < src.cols(); ++col) {
                l.text += U' ';
                l.pos.push_back(SelPoint{line, static_cast<uint16_t>(col)});
            }
        }
    }
    return l;
}

uint16_t end_col_of(const LineSource& src, SelPoint p) {
    uint16_t flags = 0;
    const auto cells = src.cells(p.line, flags);
    if (p.col < cells.size() && cells[p.col].width() == vt::CellWidth::WideLead) return static_cast<uint16_t>(p.col + 1);
    return p.col;
}

}  // namespace

// ---------------------------------------------------------------- TerminalLines

uint64_t TerminalLines::first_line() const { return t_.scrollback().first_abs_line(); }
uint64_t TerminalLines::end_line() const { return t_.first_abs_line() + t_.screen().rows(); }
uint16_t TerminalLines::cols() const { return t_.screen().cols(); }

std::span<const vt::Cell> TerminalLines::cells(uint64_t line, uint16_t& flags) const {
    flags = 0;
    const uint64_t screen0 = t_.first_abs_line();
    if (line >= screen0) {
        const uint64_t r = line - screen0;
        if (r >= t_.screen().rows()) return {};
        flags = t_.screen().flags(static_cast<uint16_t>(r));
        return t_.screen().row(static_cast<uint16_t>(r));
    }
    const vt::Scrollback& sb = t_.scrollback();
    if (line < sb.first_abs_line()) return {};
    const vt::Scrollback::Line& l = sb.line(static_cast<size_t>(line - sb.first_abs_line()));
    flags = l.flags;
    return l.cells;
}

std::u32string_view TerminalLines::cluster(uint32_t index) const {
    if (index >= t_.graphemes().size()) return {};
    return t_.graphemes()[index];
}

std::string TerminalLines::link_uri(uint16_t index) const {
    if (index == 0 || index >= t_.links().size()) return {};
    return t_.links()[index].uri;
}

// ---------------------------------------------------------------- selection

bool is_word_char(char32_t c) {
    if (c >= U'0' && c <= U'9') return true;
    if ((c | 0x20) >= U'a' && (c | 0x20) <= U'z') return true;
    if (c >= 0xC0 && c != 0xD7 && c != 0xF7 && c < 0x2000) return true;   // Latin-1 letters and up
    if (c >= 0x3000 && c != 0x3000) return true;                          // CJK and beyond
    return kWordChars.find(c) != std::u32string_view::npos;
}

uint64_t logical_start(const LineSource& src, uint64_t line) {
    while (line > src.first_line() && wrapped(src, line - 1)) --line;
    return line;
}

uint64_t logical_end(const LineSource& src, uint64_t line) {
    while (line + 1 < src.end_line() && wrapped(src, line)) ++line;
    return line;
}

SelPoint word_start(const LineSource& src, SelPoint p) {
    const bool word = is_word_char(char_at(src, p));
    for (;;) {
        SelPoint prev = p;
        if (p.col > 0) {
            prev.col = static_cast<uint16_t>(p.col - 1);
        } else if (p.line > src.first_line() && wrapped(src, p.line - 1)) {
            prev = SelPoint{p.line - 1, last_col(src)};
        } else {
            return p;
        }
        const char32_t c = char_at(src, prev);
        if (word ? !is_word_char(c) : (c != char_at(src, p) || is_word_char(c))) return p;
        p = prev;
    }
}

SelPoint word_end(const LineSource& src, SelPoint p) {
    const bool word = is_word_char(char_at(src, p));
    for (;;) {
        SelPoint next = p;
        if (p.col < last_col(src)) {
            next.col = static_cast<uint16_t>(p.col + 1);
        } else if (wrapped(src, p.line) && p.line + 1 < src.end_line()) {
            next = SelPoint{p.line + 1, 0};
        } else {
            return p;
        }
        const char32_t c = char_at(src, next);
        if (word ? !is_word_char(c) : (c != char_at(src, p) || is_word_char(c))) return SelPoint{p.line, end_col_of(src, p)};
        p = next;
    }
}

void Selection::range(const LineSource& src, SelPoint& start, SelPoint& end) const {
    start = std::min(anchor, head);
    end = std::max(anchor, head);
    switch (mode) {
        case SelMode::Char: break;
        case SelMode::Word:
            start = word_start(src, start);
            end = word_end(src, end);
            break;
        case SelMode::Line:
            start = SelPoint{logical_start(src, start.line), 0};
            end = SelPoint{logical_end(src, end.line), last_col(src)};
            break;
        case SelMode::Block:
            start = SelPoint{std::min(anchor.line, head.line), std::min(anchor.col, head.col)};
            end = SelPoint{std::max(anchor.line, head.line), std::max(anchor.col, head.col)};
            break;
    }
}

std::string selection_text(const LineSource& src, const Selection& sel) {
    std::string out;
    if (!sel.active) return out;
    SelPoint s, e;
    sel.range(src, s, e);
    s.line = std::max(s.line, src.first_line());
    if (src.end_line() == 0 || e.line >= src.end_line()) e = SelPoint{src.end_line() - 1, last_col(src)};
    if (e < s) return out;
    for (uint64_t line = s.line; line <= e.line; ++line) {
        uint16_t flags = 0;
        const auto cells = src.cells(line, flags);
        uint16_t c0 = 0, c1 = last_col(src);
        if (sel.mode == SelMode::Block) {
            c0 = s.col;
            c1 = e.col;
        } else {
            if (line == s.line) c0 = s.col;
            if (line == e.line) c1 = e.col;
        }
        std::string row;
        for (size_t col = c0; col <= c1 && col < cells.size(); ++col) append_cell(src, cells[col], row);
        const bool joined = sel.mode != SelMode::Block && (flags & vt::kRowWrapped) && line != e.line;
        if (!joined) {
            while (!row.empty() && row.back() == ' ') row.pop_back();
        }
        out += row;
        if (line != e.line && !joined) out += '\n';
    }
    return out;
}

void selection_highlights(const LineSource& src, const Selection& sel, uint32_t background, std::optional<uint32_t> foreground,
                          std::vector<Highlight>& out) {
    if (sel.empty() && sel.mode == SelMode::Char) return;
    if (!sel.active) return;
    SelPoint s, e;
    sel.range(src, s, e);
    const uint64_t first = std::max(s.line, src.first_line());
    const uint64_t last = std::min<uint64_t>(e.line, src.end_line() == 0 ? 0 : src.end_line() - 1);
    for (uint64_t line = first; line <= last && line >= first; ++line) {
        uint16_t c0 = 0, c1 = src.cols();
        if (sel.mode == SelMode::Block) {
            c0 = s.col;
            c1 = static_cast<uint16_t>(e.col + 1);
        } else {
            if (line == s.line) c0 = s.col;
            if (line == e.line) c1 = static_cast<uint16_t>(e.col + 1);
        }
        if (c1 > c0) out.push_back(Highlight{line, c0, c1, background, foreground});
    }
}

// ---------------------------------------------------------------- search

std::vector<Match> find_all(const LineSource& src, std::string_view query_utf8, size_t limit) {
    std::vector<Match> out;
    std::u32string q = decode_utf8(query_utf8);
    if (q.empty() || src.end_line() == 0) return out;
    for (char32_t& c : q) c = fold(c);
    uint64_t line = src.first_line();
    while (line < src.end_line() && out.size() < limit) {
        const uint64_t last = logical_end(src, line);
        Logical l = logical_text(src, line, last, false);
        for (char32_t& c : l.text) c = fold(c);
        for (size_t at = l.text.find(q); at != std::u32string::npos && out.size() < limit; at = l.text.find(q, at + q.size())) {
            const SelPoint a = l.pos[at];
            const SelPoint b = l.pos[at + q.size() - 1];
            out.push_back(Match{a, SelPoint{b.line, end_col_of(src, b)}});
        }
        line = last + 1;
    }
    return out;
}

namespace {

// The search's clock, asked inside a match by the automatic callouts (every pattern item): a
// look at the time every 32 of them. One item scans at most a line (max_line_bytes).
struct RegexClock {
    std::chrono::steady_clock::time_point deadline;
    uint32_t calls = 0;
    bool late = false;
    bool past() {
        if (!late && std::chrono::steady_clock::now() >= deadline) late = true;
        return late;
    }
};

int regex_callout(pcre2_callout_block*, void* data) {
    auto* c = static_cast<RegexClock*>(data);
    if ((++c->calls & 31u) == 0 && c->past()) return PCRE2_ERROR_CALLOUT;
    return 0;
}

template <class T, void (*Free)(T*)>
struct PcreFree {
    void operator()(T* p) const { Free(p); }
};
using CompileContext = std::unique_ptr<pcre2_compile_context, PcreFree<pcre2_compile_context, pcre2_compile_context_free>>;
using MatchContext = std::unique_ptr<pcre2_match_context, PcreFree<pcre2_match_context, pcre2_match_context_free>>;
using Code = std::unique_ptr<pcre2_code, PcreFree<pcre2_code, pcre2_code_free>>;
using MatchData = std::unique_ptr<pcre2_match_data, PcreFree<pcre2_match_data, pcre2_match_data_free>>;

}  // namespace

Result<RegexFound> find_all_regex(const LineSource& src, std::string_view pattern, size_t limit, const RegexLimits& lim) {
    RegexFound out;
    if (pattern.empty()) return out;
    if (pattern.size() > lim.max_pattern_bytes)
        return make_error(Errc::invalid_argument, "the pattern is longer than " + std::to_string(lim.max_pattern_bytes) + " bytes");
    CompileContext cc(pcre2_compile_context_create(nullptr));
    MatchContext mc(pcre2_match_context_create(nullptr));
    if (!cc || !mc) return make_error(Errc::unavailable, "out of memory");
    pcre2_set_max_pattern_length(cc.get(), lim.max_pattern_bytes);
    pcre2_set_parens_nest_limit(cc.get(), lim.max_nesting);
    pcre2_set_max_pattern_compiled_length(cc.get(), lim.max_compiled_bytes);
    int err = 0;
    PCRE2_SIZE off = 0;
    // \C could split a character; the callouts carry the clock.
    const uint32_t opts = PCRE2_UTF | PCRE2_UCP | PCRE2_CASELESS | PCRE2_NEVER_BACKSLASH_C | PCRE2_AUTO_CALLOUT;
    Code code(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), opts, &err, &off, cc.get()));
    if (!code) {
        PCRE2_UCHAR msg[256];
        if (pcre2_get_error_message(err, msg, sizeof msg) < 0) msg[0] = 0;
        return make_error(Errc::invalid_argument, std::string(reinterpret_cast<const char*>(msg)) + " at " + std::to_string(off));
    }
    MatchData md(pcre2_match_data_create_from_pattern(code.get(), nullptr));
    if (!md) return make_error(Errc::unavailable, "out of memory");
    RegexClock clock{std::chrono::steady_clock::now() + lim.budget};
    pcre2_set_match_limit(mc.get(), lim.match_steps);
    pcre2_set_depth_limit(mc.get(), lim.depth);
    pcre2_set_heap_limit(mc.get(), lim.heap_kib);
    pcre2_set_callout(mc.get(), regex_callout, &clock);
    if (src.end_line() == 0) return out;
    std::string bytes;
    std::vector<uint32_t> cp_at;   // byte offset -> index of the code point that starts there
    uint64_t line = src.first_line();
    auto stop = [&](const char* why) {
        if (out.stopped.empty()) out.stopped = why;
    };
    while (line < src.end_line()) {
        if (out.matches.size() >= limit) {
            stop("the first matches only");
            break;
        }
        if (clock.past()) {
            stop("the pattern took too long");
            break;
        }
        const uint64_t last = logical_end(src, line);
        const Logical l = logical_text(src, line, last, false);
        bytes.clear();
        cp_at.clear();
        size_t n = 0;   // the code points searched
        for (; n < l.text.size(); ++n) {
            const size_t before = bytes.size();
            const char32_t c = l.text[n];
            append_utf8(bytes, (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF ? char32_t{0xFFFD} : c);
            if (bytes.size() > lim.max_line_bytes) {
                bytes.resize(before);
                stop("long lines are searched in their first part");
                break;
            }
            cp_at.insert(cp_at.end(), bytes.size() - before, static_cast<uint32_t>(n));
        }
        cp_at.push_back(static_cast<uint32_t>(n));
        PCRE2_SIZE start = 0;
        while (start <= bytes.size() && out.matches.size() < limit) {
            const int rc = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(bytes.data()), bytes.size(), start, 0, md.get(), mc.get());
            if (rc == PCRE2_ERROR_NOMATCH) break;
            if (rc < 0) {   // a limit (the clock, steps, depth, heap) or another failure: the next line
                stop(rc == PCRE2_ERROR_CALLOUT                                    ? "the pattern took too long"
                     : rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ? "the pattern is too complex for some lines"
                     : rc == PCRE2_ERROR_HEAPLIMIT || rc == PCRE2_ERROR_NOMEMORY    ? "the pattern needs too much memory for some lines"
                                                                                    : "the pattern failed on some lines");
                break;
            }
            const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md.get());
            if (ov[0] > bytes.size() || ov[1] > bytes.size() || ov[1] <= ov[0]) {   // empty (or \K past the end): the next code point
                start = std::max<PCRE2_SIZE>(start, std::min<PCRE2_SIZE>(ov[0], bytes.size())) + 1;
                while (start < bytes.size() && (static_cast<unsigned char>(bytes[start]) & 0xC0) == 0x80) ++start;
                continue;
            }
            const SelPoint a = l.pos[cp_at[ov[0]]];
            const SelPoint b = l.pos[cp_at[ov[1]] - 1];
            out.matches.push_back(Match{a, SelPoint{b.line, end_col_of(src, b)}});
            start = ov[1];
        }
        line = last + 1;
    }
    return out;
}

void match_highlights(const LineSource& src, const std::vector<Match>& matches, std::optional<size_t> current, uint32_t background,
                      uint32_t current_background, std::vector<Highlight>& out) {
    for (size_t i = 0; i < matches.size(); ++i) {
        Selection s;
        s.active = true;
        s.anchor = matches[i].start;
        s.head = matches[i].end;
        selection_highlights(src, s, current && *current == i ? current_background : background, std::nullopt, out);
    }
}

// ---------------------------------------------------------------- links

namespace {

bool url_char(char32_t c) {
    if (c <= 0x20 || c == 0x7F) return false;
    switch (c) {
        case U'<': case U'>': case U'"': case U'`': case U'{': case U'}': case U'|': case U'\\': case U'^': return false;
        default: return c < 0x2000 || c > 0x206F;   // general punctuation (quotes, ellipsis) ends a URL
    }
}

bool starts_with_at(std::u32string_view text, size_t at, std::u32string_view prefix) {
    if (at + prefix.size() > text.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (fold(text[at + i]) != prefix[i]) return false;
    }
    return true;
}

// The URLs of a text as [begin, end) code point ranges.
std::vector<std::pair<size_t, size_t>> url_ranges(std::u32string_view text) {
    static constexpr std::u32string_view kSchemes[] = {U"https://", U"http://", U"ftp://", U"file://", U"ssh://", U"git://", U"mailto:",
                                                       U"www."};
    std::vector<std::pair<size_t, size_t>> out;
    size_t i = 0;
    while (i < text.size()) {
        std::u32string_view hit;
        if (i == 0 || !is_word_char(text[i - 1]) || text[i - 1] == U'/' || text[i - 1] == U':') {
            for (std::u32string_view s : kSchemes) {
                if (starts_with_at(text, i, s)) {
                    hit = s;
                    break;
                }
            }
        }
        if (hit.empty()) {
            ++i;
            continue;
        }
        size_t e = i + hit.size();
        while (e < text.size() && url_char(text[e])) ++e;
        // Trailing punctuation; a closing bracket stays only when it is balanced in the URL.
        for (;;) {
            if (e <= i + hit.size()) break;
            const char32_t c = text[e - 1];
            if (std::u32string_view(U".,;:!?'\"*").find(c) != std::u32string_view::npos) {
                --e;
                continue;
            }
            if (c == U')' || c == U']') {
                const char32_t open = c == U')' ? U'(' : U'[';
                const auto opens = std::count(text.begin() + static_cast<std::ptrdiff_t>(i), text.begin() + static_cast<std::ptrdiff_t>(e), open);
                const auto closes = std::count(text.begin() + static_cast<std::ptrdiff_t>(i), text.begin() + static_cast<std::ptrdiff_t>(e), c);
                if (closes > opens) {
                    --e;
                    continue;
                }
            }
            break;
        }
        if (e > i + hit.size()) out.emplace_back(i, e);
        i = std::max(e, i + 1);
    }
    return out;
}

std::string to_utf8(std::u32string_view s) {
    std::string out;
    for (char32_t c : s) append_utf8(out, c);
    return out;
}

}  // namespace

std::vector<std::string> detect_urls(std::u32string_view text) {
    std::vector<std::string> out;
    for (const auto& [b, e] : url_ranges(text)) out.push_back(to_utf8(text.substr(b, e - b)));
    return out;
}

namespace {

bool path_char(char32_t c) {
    if (c <= U' ' || c == 0x7f) return false;
    return std::u32string_view(U"\"'<>|`()[]{}").find(c) == std::u32string_view::npos;
}

bool all_digits(std::u32string_view s) {
    if (s.empty() || s.size() > 9) return false;
    for (char32_t c : s)
        if (c < U'0' || c > U'9') return false;
    return true;
}

uint32_t to_num(std::u32string_view s) {
    uint32_t n = 0;
    for (char32_t c : s) n = n * 10 + static_cast<uint32_t>(c - U'0');
    return n;
}

}  // namespace

std::optional<PathRef> parse_path_token(std::u32string_view t) {
    while (!t.empty() && std::u32string_view(U".,;:!?").find(t.back()) != std::u32string_view::npos) t.remove_suffix(1);
    PathRef r;
    // "file.cpp(12,5)": the bracket ends the token, so it arrives as "file.cpp(12,5)" only when
    // the caller kept it; path_at passes it whole.
    if (const size_t open = t.rfind(U'('); open != std::u32string_view::npos && t.back() == U')') {
        std::u32string_view in = t.substr(open + 1, t.size() - open - 2);
        const size_t comma = in.find(U',');
        const std::u32string_view a = in.substr(0, comma), b = comma == std::u32string_view::npos ? std::u32string_view() : in.substr(comma + 1);
        if (all_digits(a) && (b.empty() || all_digits(b))) {
            r.line = to_num(a);
            r.col = b.empty() ? 0 : to_num(b);
            t = t.substr(0, open);
        }
    }
    if (!r.line) {
        // ":line[:col]" from the end.
        auto take = [&](uint32_t& out) {
            const size_t c = t.rfind(U':');
            if (c == std::u32string_view::npos || !all_digits(t.substr(c + 1))) return false;
            out = to_num(t.substr(c + 1));
            t = t.substr(0, c);
            return true;
        };
        uint32_t last = 0, before = 0;
        if (take(last)) {
            if (take(before)) {
                r.line = before;
                r.col = last;
            } else {
                r.line = last;
            }
        }
    }
    if (t.empty() || t.find(U"://") != std::u32string_view::npos) return std::nullopt;
    bool shaped = t.front() == U'~' || t.find_first_of(U"/\\") != std::u32string_view::npos;
    if (!shaped) {   // "name.ext": a dot with a letter after it, not at the start
        const size_t dot = t.rfind(U'.');
        shaped = dot != std::u32string_view::npos && dot > 0 && dot + 1 < t.size() &&
                 ((t[dot + 1] >= U'a' && t[dot + 1] <= U'z') || (t[dot + 1] >= U'A' && t[dot + 1] <= U'Z'));
    }
    if (!shaped) return std::nullopt;
    r.path = to_utf8(t);
    return r;
}

std::optional<PathRef> path_at(const LineSource& src, SelPoint p, Match* where) {
    const uint64_t first = logical_start(src, p.line), last = logical_end(src, p.line);
    const Logical l = logical_text(src, first, last, false);
    size_t at = 0;
    while (at < l.pos.size() && l.pos[at] < p) ++at;
    if (at >= l.text.size() || !(l.pos[at] == p) || !path_char(l.text[at])) return std::nullopt;
    size_t b = at, e = at + 1;
    while (b > 0 && path_char(l.text[b - 1])) --b;
    while (e < l.text.size() && path_char(l.text[e])) ++e;
    // A "(line[,col])" right after the path (MSVC, some linters) belongs to it.
    if (e < l.text.size() && l.text[e] == U'(') {
        size_t k = e + 1;
        while (k < l.text.size() && ((l.text[k] >= U'0' && l.text[k] <= U'9') || l.text[k] == U',')) ++k;
        if (k < l.text.size() && l.text[k] == U')' && k > e + 1) e = k + 1;
    }
    while (e > at + 1 && std::u32string_view(U".,;:!?").find(l.text[e - 1]) != std::u32string_view::npos) --e;
    auto r = parse_path_token(std::u32string_view(l.text).substr(b, e - b));
    if (r && where) *where = Match{l.pos[b], l.pos[e - 1]};
    return r;
}

std::string url_at(const LineSource& src, SelPoint p, Match* where) {
    uint16_t flags = 0;
    const auto cells = src.cells(p.line, flags);
    if (p.col < cells.size() && cells[p.col].link != 0) {
        std::string uri = src.link_uri(cells[p.col].link);
        if (!uri.empty()) {
            if (where) {
                const uint16_t id = cells[p.col].link;
                uint16_t a = p.col, b = p.col;
                while (a > 0 && cells[a - 1u].link == id) --a;
                while (b + 1u < cells.size() && cells[b + 1u].link == id) ++b;
                *where = Match{SelPoint{p.line, a}, SelPoint{p.line, b}};
            }
            return uri;
        }
    }
    const uint64_t first = logical_start(src, p.line), last = logical_end(src, p.line);
    const Logical l = logical_text(src, first, last, false);
    for (const auto& [b, e] : url_ranges(l.text)) {
        if (l.pos[b] <= p && p <= l.pos[e - 1]) {
            if (where) *where = Match{l.pos[b], l.pos[e - 1]};
            std::string url = to_utf8(std::u32string_view(l.text).substr(b, e - b));
            if (url.starts_with("www.") || url.starts_with("WWW.")) url = "https://" + url;
            return url;
        }
    }
    return {};
}

namespace {

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// The host of "scheme://[user@]host[:port]/..." (lower case), and whether a user part is there.
std::string url_host(std::string_view url, bool* has_user = nullptr) {
    if (has_user) *has_user = false;
    auto p = url.find("://");
    if (p == std::string_view::npos) return {};
    std::string_view rest = url.substr(p + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (auto at = rest.rfind('@'); at != std::string_view::npos) {
        if (has_user) *has_user = true;
        rest = rest.substr(at + 1);
    }
    if (!rest.empty() && rest.front() == '[') rest = rest.substr(0, rest.find(']') + 1);   // IPv6
    else rest = rest.substr(0, rest.find(':'));
    std::string h = lower_ascii(rest);
    while (!h.empty() && h.back() == '.') h.pop_back();
    return h;
}

// The host a label names: "https://a.example/x", "www.a.example", "a.example"; "" when the label
// is not an address (words, a file name, spaces).
std::string label_host(std::string_view shown) {
    while (!shown.empty() && (shown.front() == ' ' || shown.front() == '<' || shown.front() == '(')) shown.remove_prefix(1);
    while (!shown.empty() && (shown.back() == ' ' || shown.back() == '>' || shown.back() == ')' || shown.back() == '.' || shown.back() == ','))
        shown.remove_suffix(1);
    if (shown.empty() || shown.find(' ') != std::string_view::npos) return {};
    if (shown.find("://") != std::string_view::npos) return url_host(shown);
    const std::string s = lower_ascii(shown);
    if (!s.starts_with("www.")) return {};   // "readme.md" is a label, not a host
    return url_host("https://" + s);
}

bool same_site(const std::string& a, const std::string& b) {
    auto bare = [](const std::string& h) { return h.starts_with("www.") ? h.substr(4) : h; };
    return bare(a) == bare(b);
}

}  // namespace

std::string link_risk(std::string_view url, std::string_view shown) {
    const auto colon = url.find(':');
    if (colon == std::string_view::npos) return "not a link";
    const std::string scheme = lower_ascii(url.substr(0, colon));
    if (scheme == "file") {
        std::string path = lower_ascii(url.substr(0, url.find_first_of("?#")));
        static const char* const kPrograms[] = {".exe", ".bat", ".cmd", ".com", ".ps1", ".vbs", ".vbe", ".js",  ".jse", ".wsf", ".msi",
                                                ".lnk", ".scr", ".pif", ".hta", ".cpl", ".sh",  ".command", ".app", ".jar", ".py", ".url"};
        while (!path.empty() && path.back() == '/') path.pop_back();
        for (const char* e : kPrograms)
            if (path.ends_with(e)) return "It opens a program on this computer.";
        return "It opens a local file or folder.";
    }
    if (scheme == "ssh" || scheme == "git") return "It starts another program (" + scheme + ").";
    if (scheme == "mailto") return {};
    bool user = false;
    const std::string host = url_host(url, &user);
    if (user) return "The address has a user name before the host; the site is " + host + ".";
    bool non_ascii = false;
    for (char c : host) non_ascii = non_ascii || (static_cast<unsigned char>(c) & 0x80) != 0;
    if (non_ascii || host.starts_with("xn--") || host.find(".xn--") != std::string::npos)
        return "The host name has characters that can look like others.";
    const std::string named = label_host(shown);
    if (!named.empty() && !host.empty() && !same_site(named, host)) return "The link text shows " + named + ", but it opens " + host + ".";
    return {};
}

// ---------------------------------------------------------------- copy mode

void CopyMode::begin(const LineSource& src, SelPoint cursor, uint16_t rows, uint64_t top_line) {
    active_ = true;
    sel_ = {};
    cur_ = cursor;
    rows_ = std::max<uint16_t>(1, rows);
    top_ = top_line;
    count_ = 0;
    pending_ = 0;
    clamp(src);
}

void CopyMode::clamp(const LineSource& src) {
    if (src.end_line() == 0) return;
    cur_.line = std::clamp(cur_.line, src.first_line(), src.end_line() - 1);
    cur_.col = std::min(cur_.col, last_col(src));
    if (sel_.active) sel_.head = cur_;
    follow();
}

void CopyMode::follow() {
    if (cur_.line < top_) top_ = cur_.line;
    if (cur_.line >= top_ + rows_) top_ = cur_.line - rows_ + 1;
}

void CopyMode::move_to(const LineSource& src, SelPoint p) {
    cur_ = p;
    clamp(src);
}

CopyMode::Action CopyMode::key(const LineSource& src, const KeyInput& k) {
    if (!active_ || k.action == vt::KeyAction::Release) return Action::None;
    const bool ctrl = (k.mods & kPmCtrl) != 0;
    char32_t c = 0;
    switch (k.key) {
        case vt::Key::left: c = U'h'; break;
        case vt::Key::right: c = U'l'; break;
        case vt::Key::up: c = U'k'; break;
        case vt::Key::down: c = U'j'; break;
        case vt::Key::page_up: c = U'b'; break;
        case vt::Key::page_down: c = U'f'; break;
        case vt::Key::home: c = U'0'; break;
        case vt::Key::end: c = U'$'; break;
        case vt::Key::escape: end(); return Action::Exit;
        case vt::Key::enter:
        case vt::Key::kp_enter: return sel_.active ? Action::Copy : Action::None;
        default: break;
    }
    const bool paging = c == U'b' || c == U'f';
    if (c != 0 && paging) return motion(src, c, std::max<uint32_t>(1, count_), true);
    if (c == 0) {
        if (ctrl) {
            c = k.logical;
            if (c >= U'A' && c <= U'Z') c += 32;
        } else if (!k.text.empty()) {
            const std::u32string t = decode_utf8(std::string_view(reinterpret_cast<const char*>(k.text.data()), k.text.size()));
            c = t.empty() ? 0 : t.front();
        } else {
            c = k.logical;
            if ((k.mods & kPmShift) && c >= U'a' && c <= U'z') c -= 32;
        }
    }
    if (c == 0) return Action::None;
    if (!ctrl && c >= U'0' && c <= U'9' && (c != U'0' || count_ > 0)) {
        count_ = std::min<uint32_t>(count_ * 10 + (c - U'0'), 100000);
        return Action::None;
    }
    const uint32_t n = std::max<uint32_t>(1, count_);
    count_ = 0;
    if (!ctrl) {
        switch (c) {
            case U'q': end(); return Action::Exit;
            case U'y': return sel_.active ? Action::Copy : Action::None;
            case U'v':
            case U'V':
                if (sel_.active && sel_.mode == (c == U'v' ? SelMode::Char : SelMode::Line)) {
                    sel_ = {};
                } else {
                    if (!sel_.active) sel_.anchor = cur_;
                    sel_.active = true;
                    sel_.mode = c == U'v' ? SelMode::Char : SelMode::Line;
                    sel_.head = cur_;
                }
                return Action::Moved;
            case U'o':
                if (sel_.active) {
                    std::swap(sel_.anchor, sel_.head);
                    cur_ = sel_.head;
                    follow();
                }
                return Action::Moved;
            case U'/': return Action::Search;
            case U'?': return Action::SearchBack;
            case U'n': return Action::Next;
            case U'N': return Action::Previous;
            default: break;
        }
    } else if (c == U'v') {
        if (!sel_.active) sel_.anchor = cur_;
        sel_.active = true;
        sel_.mode = SelMode::Block;
        sel_.head = cur_;
        return Action::Moved;
    } else if (c == U'c' || c == U'[') {
        end();
        return Action::Exit;
    }
    return motion(src, c, n, ctrl);
}

CopyMode::Action CopyMode::motion(const LineSource& src, char32_t c, uint32_t n, bool ctrl) {
    if (src.end_line() == 0) return Action::None;
    const uint64_t first = src.first_line(), last = src.end_line() - 1;
    const auto up = [&](uint64_t rows) { cur_.line = cur_.line - std::min(rows, cur_.line - first); };
    const auto down = [&](uint64_t rows) { cur_.line = std::min(last, cur_.line + rows); };
    const uint16_t half = static_cast<uint16_t>(std::max(1, rows_ / 2));
    if (c != U'g') pending_ = 0;
    if (ctrl) {
        switch (c) {
            case U'u': up(uint64_t{half} * n); top_ -= std::min<uint64_t>(top_ - std::min(top_, first), uint64_t{half} * n); break;
            case U'd': down(uint64_t{half} * n); top_ = std::min(top_ + uint64_t{half} * n, last + 1 - std::min<uint64_t>(rows_, last + 1)); break;
            case U'b': up(uint64_t{rows_} * n); top_ -= std::min<uint64_t>(top_ - std::min(top_, first), uint64_t{rows_} * n); break;
            case U'f': down(uint64_t{rows_} * n); top_ = std::min(top_ + uint64_t{rows_} * n, last + 1 - std::min<uint64_t>(rows_, last + 1)); break;
            default: return Action::None;
        }
        clamp(src);
        return Action::Moved;
    }
    switch (c) {
        case U'h': cur_.col = static_cast<uint16_t>(cur_.col - std::min<uint32_t>(n, cur_.col)); break;
        case U'l': cur_.col = static_cast<uint16_t>(std::min<uint32_t>(cur_.col + n, last_col(src))); break;
        case U'k': up(n); break;
        case U'j': down(n); break;
        case U'0': cur_.col = 0; break;
        case U'$': {
            uint16_t flags = 0;
            auto cells = src.cells(cur_.line, flags);
            size_t e = cells.size();
            while (e > 0 && cells[e - 1].is_blank()) --e;
            cur_.col = static_cast<uint16_t>(e == 0 ? 0 : e - 1);
            break;
        }
        case U'^': {
            uint16_t flags = 0;
            auto cells = src.cells(cur_.line, flags);
            size_t b = 0;
            while (b < cells.size() && cells[b].is_blank()) ++b;
            cur_.col = static_cast<uint16_t>(b < cells.size() ? b : 0);
            break;
        }
        case U'g':
            if (pending_ != U'g') {
                pending_ = U'g';
                return Action::None;
            }
            pending_ = 0;
            cur_ = SelPoint{first, 0};
            break;
        case U'G': cur_ = SelPoint{last, 0}; break;
        case U'H': cur_.line = std::max(first, top_); break;
        case U'M': cur_.line = std::min(last, top_ + rows_ / 2); break;
        case U'L': cur_.line = std::min(last, top_ + rows_ - 1); break;
        case U'w':
        case U'W':
            for (uint32_t i = 0; i < n; ++i) {
                // To the start of the next word.
                SelPoint p = cur_;
                const SelPoint e = word_end(src, p);
                p = e;
                for (int guard = 0; guard < 4096; ++guard) {
                    if (p.col < last_col(src)) p.col = static_cast<uint16_t>(p.col + 1);
                    else if (p.line < last) p = SelPoint{p.line + 1, 0};
                    else break;
                    if (char_at(src, p) != U' ') break;
                }
                cur_ = p;
            }
            break;
        case U'e':
        case U'E':
            for (uint32_t i = 0; i < n; ++i) {
                SelPoint p = cur_;
                for (int guard = 0; guard < 4096; ++guard) {
                    if (p.col < last_col(src)) p.col = static_cast<uint16_t>(p.col + 1);
                    else if (p.line < last) p = SelPoint{p.line + 1, 0};
                    else break;
                    if (char_at(src, p) != U' ') break;
                }
                cur_ = word_end(src, p);
            }
            break;
        case U'b':
        case U'B':
            for (uint32_t i = 0; i < n; ++i) {
                SelPoint p = cur_;
                for (int guard = 0; guard < 4096; ++guard) {
                    if (p.col > 0) p.col = static_cast<uint16_t>(p.col - 1);
                    else if (p.line > first) p = SelPoint{p.line - 1, last_col(src)};
                    else break;
                    if (char_at(src, p) != U' ') break;
                }
                cur_ = word_start(src, p);
            }
            break;
        default: return Action::None;
    }
    clamp(src);
    return Action::Moved;
}

}  // namespace kt::view
