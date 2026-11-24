#include "store/regex_query.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "store/fold.h"

namespace kt::store {

namespace {

using trigram::Query;
using StrSet = std::vector<std::string>;

constexpr std::size_t kMaxSet = 64;       // larger string sets are dropped (unconstrained)
constexpr std::size_t kMaxClass = 16;     // larger classes are "any character"
constexpr std::size_t kMaxExactLen = 64;  // longer exact strings move into the trigram query

void normalize_set(StrSet& s) {
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
}

std::optional<StrSet> cross(const StrSet& a, const StrSet& b) {
    if (a.size() * b.size() > kMaxSet) return std::nullopt;
    StrSet out;
    out.reserve(a.size() * b.size());
    for (const auto& x : a) {
        for (const auto& y : b) out.push_back(x + y);
    }
    normalize_set(out);
    return out;
}

// OR over the strings of the AND of each string's trigrams.
Query set_query(const StrSet& s) {
    if (s.empty()) return Query::all_blocks();
    Query q = Query::no_blocks();
    for (const auto& str : s) {
        if (str.size() < 3) return Query::all_blocks();
        q = trigram::or_of(std::move(q), trigram::literal_query(str));
    }
    return q;
}

struct Info {
    bool emptyable = false;
    std::optional<StrSet> exact;  // every match is one of these
    StrSet prefix{""}, suffix{""};  // when exact is unknown: every match starts / ends with one
    Query match;                  // required trigrams beyond what the sets say
};

Info empty_info() {
    Info i;
    i.emptyable = true;
    i.exact = StrSet{""};
    return i;
}

Info any_char() { return Info{}; }

Info any_string() {
    Info i;
    i.emptyable = true;
    return i;
}

Info exact_info(StrSet s) {
    Info i;
    normalize_set(s);
    i.emptyable = std::find(s.begin(), s.end(), std::string()) != s.end();
    i.exact = std::move(s);
    return i;
}

// Moves an exact set into the trigram query and the prefix/suffix sets.
void drop_exact(Info& i) {
    if (!i.exact) return;
    i.match = trigram::and_of(std::move(i.match), set_query(*i.exact));
    i.prefix = *i.exact;
    i.suffix = *i.exact;
    i.exact.reset();
}

// Keeps the sets small: their inner trigrams go into the query, only the two bytes that can
// form trigrams across a boundary are kept.
void normalize(Info& i) {
    if (i.exact) {
        bool too_long = false;
        for (const auto& s : *i.exact) too_long = too_long || s.size() > kMaxExactLen;
        if (i.exact->size() > kMaxSet || too_long) drop_exact(i);
        else return;
    }
    auto trim = [&](StrSet& s, bool front) {
        bool all_long = !s.empty();
        for (const auto& x : s) all_long = all_long && x.size() >= 3;
        if (all_long) i.match = trigram::and_of(std::move(i.match), set_query(s));
        for (auto& x : s) {
            if (x.size() > 2) x = front ? x.substr(0, 2) : x.substr(x.size() - 2);
        }
        normalize_set(s);
        if (s.size() > kMaxSet) s = StrSet{""};
    };
    trim(i.prefix, true);
    trim(i.suffix, false);
}

Info concat(Info x, Info y) {
    Info r;
    r.emptyable = x.emptyable && y.emptyable;
    r.match = trigram::and_of(std::move(x.match), std::move(y.match));
    if (x.exact && y.exact) {
        if (auto c = cross(*x.exact, *y.exact)) {
            r.exact = std::move(*c);
            normalize(r);
            return r;
        }
    }
    const StrSet& xs = x.exact ? *x.exact : x.suffix;
    const StrSet& yp = y.exact ? *y.exact : y.prefix;
    if (auto b = cross(xs, yp)) r.match = trigram::and_of(std::move(r.match), set_query(*b));
    if (x.exact) {
        auto c = cross(*x.exact, y.exact ? *y.exact : y.prefix);
        r.prefix = c ? std::move(*c) : *x.exact;
    } else {
        r.prefix = x.prefix;
        if (x.emptyable) {
            // x may match nothing: then the match starts like y.
            const StrSet& yp2 = y.exact ? *y.exact : y.prefix;
            r.prefix.insert(r.prefix.end(), yp2.begin(), yp2.end());
        }
    }
    if (y.exact) {
        auto c = cross(x.exact ? *x.exact : x.suffix, *y.exact);
        r.suffix = c ? std::move(*c) : *y.exact;
    } else {
        r.suffix = y.suffix;
        if (y.emptyable) {
            const StrSet& xs2 = x.exact ? *x.exact : x.suffix;
            r.suffix.insert(r.suffix.end(), xs2.begin(), xs2.end());
        }
    }
    normalize_set(r.prefix);
    normalize_set(r.suffix);
    normalize(r);
    return r;
}

Info alternate(Info x, Info y) {
    Info r;
    r.emptyable = x.emptyable || y.emptyable;
    if (x.exact && y.exact && x.exact->size() + y.exact->size() <= kMaxSet) {
        StrSet u = *x.exact;
        u.insert(u.end(), y.exact->begin(), y.exact->end());
        r.exact = std::move(u);
        normalize_set(*r.exact);
        r.match = trigram::or_of(std::move(x.match), std::move(y.match));
        normalize(r);
        return r;
    }
    drop_exact(x);
    drop_exact(y);
    r.prefix = x.prefix;
    r.prefix.insert(r.prefix.end(), y.prefix.begin(), y.prefix.end());
    r.suffix = x.suffix;
    r.suffix.insert(r.suffix.end(), y.suffix.begin(), y.suffix.end());
    normalize_set(r.prefix);
    normalize_set(r.suffix);
    r.match = trigram::or_of(std::move(x.match), std::move(y.match));
    normalize(r);
    return r;
}

Info quest(Info x) {
    if (x.exact && x.exact->size() < kMaxSet) {
        x.exact->push_back("");
        normalize_set(*x.exact);
        x.emptyable = true;
        return x;
    }
    return any_string();
}

Info plus(Info x) {
    drop_exact(x);
    normalize(x);
    return x;
}

class Parser {
public:
    explicit Parser(std::string_view p) : p_(p) {}

    Query run() {
        Info i = parse_alt();
        if (bail_ || pos_ != p_.size()) return Query::all_blocks();
        if (i.exact) return trigram::and_of(std::move(i.match), set_query(*i.exact));
        return std::move(i.match);
    }

private:
    bool eof() const { return pos_ >= p_.size(); }
    char peek() const { return eof() ? '\0' : p_[pos_]; }
    bool take(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }

    char32_t next_cp() {
        const auto b = static_cast<unsigned char>(p_[pos_++]);
        if (b < 0x80) return b;
        int len = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
        uint32_t cp = len == 2 ? (b & 0x1Fu) : len == 3 ? (b & 0x0Fu) : len == 4 ? (b & 0x07u) : b;
        for (int i = 1; i < len && !eof(); ++i) cp = (cp << 6) | (static_cast<unsigned char>(p_[pos_++]) & 0x3Fu);
        return static_cast<char32_t>(cp);
    }

    static std::string fold_cp_utf8(char32_t cp) {
        std::string s;
        append_utf8(s, fold_cp(cp));
        return s;
    }

    Info literal(char32_t cp) { return exact_info({fold_cp_utf8(cp)}); }

    Info parse_alt() {
        Info x = parse_concat();
        while (!bail_ && take('|')) x = alternate(std::move(x), parse_concat());
        return x;
    }

    Info parse_concat() {
        Info r = empty_info();
        while (!bail_ && !eof() && peek() != '|' && peek() != ')') r = concat(std::move(r), parse_repeat());
        return r;
    }

    bool parse_count(std::size_t& n) {
        const std::size_t start = pos_;
        n = 0;
        while (!eof() && peek() >= '0' && peek() <= '9') n = n * 10 + static_cast<std::size_t>(p_[pos_++] - '0');
        return pos_ > start;
    }

    Info parse_repeat() {
        Info a = parse_atom();
        for (;;) {
            if (take('*')) {
                a = any_string();
            } else if (take('+')) {
                a = plus(std::move(a));
            } else if (take('?')) {
                a = quest(std::move(a));
            } else if (peek() == '{') {
                const std::size_t save = pos_++;
                std::size_t lo = 0, hi = 0;
                const bool has_lo = parse_count(lo);
                bool has_hi = false, comma = false;
                if (take(',')) {
                    comma = true;
                    has_hi = parse_count(hi);
                }
                if (!take('}') || (!has_lo && !has_hi)) {
                    pos_ = save + 1;  // a literal '{'
                    a = concat(std::move(a), literal('{'));
                    continue;
                }
                if (lo == 0) a = (comma && !has_hi) || hi > 1 ? any_string() : quest(std::move(a));
                else if (!(lo == 1 && !comma)) a = plus(std::move(a));
            } else {
                break;
            }
            // Lazy and possessive quantifier suffixes.
            if (!take('?')) take('+');
        }
        return a;
    }

    Info parse_group_body() {
        Info i = parse_alt();
        if (!take(')')) bail_ = true;
        return i;
    }

    Info parse_atom() {
        if (eof()) return empty_info();
        const char c = peek();
        if (c == '(') {
            ++pos_;
            if (take('?')) {
                if (take(':') || take('>') || take('|')) return parse_group_body();
                if (take('=') || take('!')) {
                    (void)parse_group_body();
                    return empty_info();
                }
                if (take('<')) {
                    if (take('=') || take('!')) {
                        (void)parse_group_body();
                        return empty_info();
                    }
                    while (!eof() && peek() != '>') ++pos_;  // (?<name>...)
                    if (!take('>')) bail_ = true;
                    return parse_group_body();
                }
                if (take('P')) {
                    if (!take('<')) {
                        bail_ = true;
                        return any_string();
                    }
                    while (!eof() && peek() != '>') ++pos_;
                    if (!take('>')) bail_ = true;
                    return parse_group_body();
                }
                // Inline flags: (?i) (?-i) (?i:...). Extended mode changes the syntax: give up.
                while (!eof() && (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '-' || peek() == '^')) {
                    if (peek() == 'x') bail_ = true;
                    ++pos_;
                }
                if (take(')')) return empty_info();
                if (take(':')) return parse_group_body();
                bail_ = true;
                return any_string();
            }
            return parse_group_body();
        }
        if (c == '[') {
            ++pos_;
            return parse_class();
        }
        if (c == '.') {
            ++pos_;
            return any_char();
        }
        if (c == '^' || c == '$') {
            ++pos_;
            return empty_info();
        }
        if (c == '\\') {
            ++pos_;
            return parse_escape();
        }
        if (c == '*' || c == '+' || c == '?') {
            bail_ = true;  // a quantifier with nothing before it: PCRE2 rejects it
            ++pos_;
            return any_string();
        }
        return literal(next_cp());
    }

    // After the backslash. Returns the literal code point through `cp` when there is one.
    enum class Esc { literal, any, empty, backref, unknown };
    Esc escape(char32_t& cp) {
        if (eof()) return Esc::unknown;
        const char c = p_[pos_++];
        switch (c) {
            case 'd': case 'D': case 'w': case 'W': case 's': case 'S': case 'h': case 'H':
            case 'v': case 'V': case 'N': case 'R': case 'X': case 'C': return Esc::any;
            case 'p': case 'P':
                if (take('{')) {
                    while (!eof() && peek() != '}') ++pos_;
                    take('}');
                } else if (!eof()) {
                    ++pos_;
                }
                return Esc::any;
            case 'b': case 'B': case 'A': case 'z': case 'Z': case 'G': case 'K': return Esc::empty;
            case 'n': cp = '\n'; return Esc::literal;
            case 't': cp = '\t'; return Esc::literal;
            case 'r': cp = '\r'; return Esc::literal;
            case 'f': cp = '\f'; return Esc::literal;
            case 'e': cp = 0x1B; return Esc::literal;
            case 'a': cp = 0x07; return Esc::literal;
            case 'x': {
                uint32_t v = 0;
                auto hex = [&](char h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    return -1;
                };
                if (take('{')) {
                    while (!eof() && peek() != '}') {
                        const int d = hex(p_[pos_++]);
                        if (d < 0) return Esc::unknown;
                        v = v * 16 + static_cast<uint32_t>(d);
                    }
                    if (!take('}')) return Esc::unknown;
                } else {
                    for (int i = 0; i < 2 && !eof() && hex(peek()) >= 0; ++i) v = v * 16 + static_cast<uint32_t>(hex(p_[pos_++]));
                }
                cp = static_cast<char32_t>(v);
                return Esc::literal;
            }
            case 'g': case 'k':
                if (!eof() && (peek() == '{' || peek() == '<' || peek() == '\'')) {
                    const char close = peek() == '{' ? '}' : peek() == '<' ? '>' : '\'';
                    while (!eof() && peek() != close) ++pos_;
                    take(close);
                } else {
                    while (!eof() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '-')) ++pos_;
                }
                return Esc::backref;
            default: break;
        }
        if (c >= '1' && c <= '9') {
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
            return Esc::backref;
        }
        if (c == '0') {
            cp = 0;
            return Esc::literal;
        }
        if (std::isalnum(static_cast<unsigned char>(c))) return Esc::unknown;
        --pos_;
        cp = next_cp();
        return Esc::literal;
    }

    Info parse_escape() {
        if (!eof() && peek() == 'Q') {
            ++pos_;
            Info r = empty_info();
            while (!eof()) {
                if (p_.substr(pos_, 2) == "\\E") {
                    pos_ += 2;
                    break;
                }
                r = concat(std::move(r), literal(next_cp()));
            }
            return r;
        }
        if (!eof() && peek() == 'E') {
            ++pos_;
            return empty_info();
        }
        char32_t cp = 0;
        switch (escape(cp)) {
            case Esc::literal: return literal(cp);
            case Esc::any: return any_char();
            case Esc::empty: return empty_info();
            case Esc::backref: return any_string();
            case Esc::unknown: bail_ = true; return any_string();
        }
        return any_string();
    }

    Info parse_class() {
        const bool negated = take('^');
        StrSet members;
        bool big = negated;
        bool first = true;
        while (!eof()) {
            if (peek() == ']' && !first) break;
            first = false;
            char32_t lo = 0;
            if (p_.substr(pos_, 2) == "[:") {
                const auto end = p_.find(":]", pos_ + 2);
                if (end == std::string_view::npos) {
                    bail_ = true;
                    return any_string();
                }
                pos_ = end + 2;
                big = true;
                continue;
            }
            if (peek() == '\\') {
                ++pos_;
                const Esc e = escape(lo);
                if (e == Esc::unknown) {
                    bail_ = true;
                    return any_string();
                }
                if (e != Esc::literal) {
                    big = true;
                    continue;
                }
            } else {
                lo = next_cp();
            }
            char32_t hi = lo;
            if (peek() == '-' && pos_ + 1 < p_.size() && p_[pos_ + 1] != ']') {
                ++pos_;
                if (peek() == '\\') {
                    ++pos_;
                    if (escape(hi) != Esc::literal) {
                        big = true;
                        continue;
                    }
                } else {
                    hi = next_cp();
                }
            }
            if (hi < lo) {
                bail_ = true;
                return any_string();
            }
            if (static_cast<std::size_t>(hi - lo) >= kMaxClass) {
                big = true;
                continue;
            }
            for (char32_t x = lo; x <= hi; ++x) members.push_back(fold_cp_utf8(x));
        }
        if (!take(']')) {
            bail_ = true;
            return any_string();
        }
        normalize_set(members);
        if (big || members.empty() || members.size() > kMaxClass) return any_char();
        return exact_info(std::move(members));
    }

    std::string_view p_;
    std::size_t pos_ = 0;
    bool bail_ = false;
};

}  // namespace

trigram::Query regex_trigram_query(std::string_view pattern) { return Parser(pattern).run(); }

}  // namespace kt::store
