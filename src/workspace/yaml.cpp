#include "workspace/yaml.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace kt::ws::yaml {

Node::Node() = default;
Node::Node(const Node&) = default;
Node::Node(Node&&) noexcept = default;
Node& Node::operator=(const Node&) = default;
Node& Node::operator=(Node&&) noexcept = default;
Node::~Node() = default;

const Node* Node::get(std::string_view key) const {
    if (kind != Kind::Map) return nullptr;
    for (const auto& [k, v] : map) {
        if (k == key) return &v;
    }
    return nullptr;
}

bool Node::is_null() const {
    if (kind == Kind::Null) return true;
    if (kind != Kind::Scalar || quoted) return false;
    return text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL";
}

namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct Fail {
    ParseError e;
};

class Parser {
public:
    explicit Parser(std::string_view text) {
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
            text.remove_prefix(3);
        std::size_t start = 0;
        int no = 1;
        while (start <= text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            std::string s(text.substr(start, end - start));
            if (!s.empty() && s.back() == '\r') s.pop_back();
            Line l;
            l.s = std::move(s);
            l.no = no++;
            while (l.off < l.s.size() && l.s[l.off] == ' ') ++l.off;
            lines_.push_back(std::move(l));
            if (end == text.size()) break;
            start = end + 1;
        }
    }

    Expected<Node, ParseError> run() {
        if (!ok_) return Unexpected<ParseError>(err_);
        // Directives and the document start.
        skip();
        while (cur_ < lines_.size() && content(cur_).starts_with("%") && indent(cur_) == 0) {
            ++cur_;
            skip();
        }
        if (cur_ < lines_.size() && indent(cur_) == 0 && is_marker(content(cur_), "---")) {
            std::string_view rest = content(cur_).substr(3);
            const std::size_t i = rest.find_first_not_of(' ');
            if (i == std::string_view::npos || rest[i] == '#') ++cur_;
            else lines_[cur_].off += 3 + i;   // "--- value"
            skip();
        }
        Node root;
        if (cur_ < lines_.size() && !at_end_marker()) {
            root = node(-1, 0);
            if (!ok_) return Unexpected<ParseError>(err_);
        }
        skip();
        if (cur_ < lines_.size() && at_end_marker()) {
            ++cur_;
            skip();
        }
        if (ok_ && cur_ < lines_.size()) {
            if (indent(cur_) == 0 && is_marker(content(cur_), "---"))
                fail(cur_, indent(cur_), "several documents in one file are not supported");
            else fail(cur_, indent(cur_), "unexpected content (check the indentation)");
        }
        if (!ok_) return Unexpected<ParseError>(err_);
        return root;
    }

private:
    struct Line {
        std::string s;
        std::size_t off = 0;   // where the content starts (the indent, or later for the rest of a line)
        int no = 0;
    };

    std::vector<Line> lines_;
    std::size_t cur_ = 0;
    bool ok_ = true;
    ParseError err_;

    static bool is_marker(std::string_view c, std::string_view m) {
        return c.starts_with(m) && (c.size() == m.size() || is_space(c[m.size()]));
    }
    bool at_doc_marker() const {
        return indent(cur_) == 0 && (is_marker(content(cur_), "...") || is_marker(content(cur_), "---"));
    }
    bool at_end_marker() const { return indent(cur_) == 0 && (is_marker(content(cur_), "...")); }

    int indent(std::size_t i) const { return static_cast<int>(lines_[i].off); }
    std::string_view content(std::size_t i) const {
        std::string_view v(lines_[i].s);
        v.remove_prefix(std::min(lines_[i].off, v.size()));
        while (!v.empty() && is_space(v.back())) v.remove_suffix(1);
        return v;
    }
    bool blank(std::size_t i) const {
        const std::string_view c = content(i);
        return c.empty() || c.front() == '#';
    }
    // To the next line with content; a tab in its indentation is an error.
    void skip() {
        while (cur_ < lines_.size() && blank(cur_)) ++cur_;
        if (cur_ < lines_.size() && ok_) {
            const Line& l = lines_[cur_];
            if (l.off < l.s.size() && l.s[l.off] == '\t' && content(cur_).find_first_not_of(" \t") != std::string_view::npos) {
                std::string_view rest(l.s);
                rest.remove_prefix(l.off);
                const std::size_t k = rest.find_first_not_of(" \t");
                if (k != std::string_view::npos && rest[k] != '#') fail(cur_, indent(cur_), "a tab in the indentation (use spaces)");
            }
        }
    }

    void fail(std::size_t line, int col0, std::string msg) {
        if (!ok_) return;
        ok_ = false;
        err_.message = std::move(msg);
        err_.line = line < lines_.size() ? lines_[line].no : (lines_.empty() ? 1 : lines_.back().no);
        err_.col = col0 + 1;
    }

    static bool is_seq_item(std::string_view c) { return c == "-" || (c.size() >= 2 && c[0] == '-' && is_space(c[1])); }

    // The position of the ':' that ends a block mapping key, npos when the line is no key.
    static std::size_t key_colon(std::string_view c) {
        if (c.empty() || c[0] == '[' || c[0] == '{' || c[0] == '#') return std::string_view::npos;
        std::size_t i = 0;
        if (c[0] == '"' || c[0] == '\'') {
            const char q = c[0];
            i = 1;
            while (i < c.size()) {
                if (q == '"' && c[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (c[i] == q) {
                    if (q == '\'' && i + 1 < c.size() && c[i + 1] == '\'') {
                        i += 2;
                        continue;
                    }
                    break;
                }
                ++i;
            }
            if (i >= c.size()) return std::string_view::npos;
            ++i;
            while (i < c.size() && is_space(c[i])) ++i;
            if (i < c.size() && c[i] == ':' && (i + 1 == c.size() || is_space(c[i + 1]))) return i;
            return std::string_view::npos;
        }
        for (; i < c.size(); ++i) {
            if (c[i] == '#' && i > 0 && is_space(c[i - 1])) return std::string_view::npos;
            if (c[i] == ':' && (i + 1 == c.size() || is_space(c[i + 1]))) return i;
        }
        return std::string_view::npos;
    }

    // ---------------------------------------------------------------- block structure

    // The node at the current line (its indent > parent, or == parent for a compact sequence).
    Node node(int parent, int depth) {
        Node n;
        if (depth > kMaxDepth) {
            fail(cur_, indent(cur_), "nested too deep");
            return n;
        }
        skip();
        if (!ok_ || cur_ >= lines_.size()) return n;
        const int ind = indent(cur_);
        const std::string_view c = content(cur_);
        if (is_seq_item(c)) return seq(ind, depth);
        if (key_colon(c) != std::string_view::npos) return map(ind, depth);
        return inline_value(parent, depth);
    }

    Node seq(int ind, int depth) {
        Node n;
        n.kind = Node::Kind::Seq;
        n.line = lines_[cur_].no;
        n.col = ind + 1;
        while (ok_) {
            skip();
            if (cur_ >= lines_.size() || indent(cur_) != ind || at_doc_marker() || !is_seq_item(content(cur_))) break;
            const std::string_view c = content(cur_);
            std::size_t r = 1;
            while (r < c.size() && is_space(c[r])) ++r;
            if (r >= c.size() || c[r] == '#') {
                ++cur_;
                skip();
                if (ok_ && cur_ < lines_.size() && indent(cur_) > ind) n.seq.push_back(node(ind, depth + 1));
                else n.seq.emplace_back();
            } else {
                lines_[cur_].off += r;   // the rest of the line is the item, at its own column
                n.seq.push_back(node(ind, depth + 1));
            }
        }
        if (ok_ && cur_ < lines_.size() && indent(cur_) > ind) fail(cur_, indent(cur_), "unexpected indentation in a sequence");
        return n;
    }

    Node map(int ind, int depth) {
        Node n;
        n.kind = Node::Kind::Map;
        n.line = lines_[cur_].no;
        n.col = ind + 1;
        while (ok_) {
            skip();
            if (cur_ >= lines_.size() || indent(cur_) != ind || at_doc_marker()) break;
            const std::string_view c = content(cur_);
            if (is_seq_item(c)) break;
            const std::size_t k = key_colon(c);
            if (k == std::string_view::npos) {
                fail(cur_, ind, "expected \"key: value\"");
                break;
            }
            std::string_view kt = c.substr(0, k);
            while (!kt.empty() && is_space(kt.back())) kt.remove_suffix(1);
            std::string key;
            if (!kt.empty() && (kt[0] == '"' || kt[0] == '\'')) {
                std::size_t pos = 0;
                auto s = quoted_text(kt, pos, cur_, ind);
                if (!ok_) break;
                key = std::move(s);
            } else {
                if (!kt.empty() && (kt[0] == '?' || kt[0] == '&' || kt[0] == '*' || kt[0] == '!')) {
                    fail(cur_, ind, kt[0] == '?' ? "complex keys (?) are not supported" : "anchors, aliases and tags are not supported");
                    break;
                }
                key = std::string(kt);
            }
            for (const auto& [ek, ev] : n.map) {
                if (ek == key) fail(cur_, ind, "duplicate key \"" + key + "\"");
            }
            if (!ok_) break;
            const std::size_t line = cur_;
            std::size_t v = k + 1;
            while (v < c.size() && is_space(c[v])) ++v;
            Node value;
            if (v >= c.size() || c[v] == '#') {
                ++cur_;
                skip();
                if (ok_ && cur_ < lines_.size() && (indent(cur_) > ind || (indent(cur_) == ind && is_seq_item(content(cur_))))) {
                    value = indent(cur_) == ind ? seq(ind, depth + 1) : node(ind, depth + 1);
                } else {
                    value.line = lines_[line].no;
                    value.col = static_cast<int>(lines_[line].off + k) + 2;
                }
            } else {
                lines_[cur_].off += v;
                value = inline_value(ind, depth + 1);
            }
            n.map.emplace_back(std::move(key), std::move(value));
        }
        if (ok_ && cur_ < lines_.size() && indent(cur_) > ind) fail(cur_, indent(cur_), "unexpected indentation in a mapping");
        return n;
    }

    // A value that starts at the current line's content: a flow collection, a quoted or plain
    // scalar (plain ones continue on more indented lines), or a block scalar.
    Node inline_value(int parent, int depth) {
        Node n;
        const std::size_t first = cur_;
        const int col0 = indent(cur_);
        n.line = lines_[cur_].no;
        n.col = col0 + 1;
        const std::string_view c = content(cur_);
        const char f = c.empty() ? '\0' : c[0];
        if (f == '&' || f == '*' || f == '!') {
            fail(cur_, col0, "anchors, aliases and tags are not supported");
            return n;
        }
        if (f == '?' && (c.size() == 1 || is_space(c[1]))) {
            fail(cur_, col0, "complex keys (?) are not supported");
            return n;
        }
        if (f == '|' || f == '>') return block_scalar(parent, n);
        if (f == '[' || f == '{' || f == '"' || f == '\'') {
            // Gather lines until the collection or string closes.
            std::string text(c);
            std::vector<std::pair<std::size_t, std::size_t>> starts{{0, first}};   // text offset -> line
            ++cur_;
            while (!balanced(text) && cur_ < lines_.size()) {
                text += '\n';
                starts.emplace_back(text.size(), cur_);
                std::string_view raw(lines_[cur_].s);
                text += raw;
                ++cur_;
            }
            Flow fl{*this, text, starts, col0};
            std::size_t pos = 0;
            Node v = fl.value(pos, depth);
            if (!ok_) return n;
            fl.ws(pos);
            if (pos < text.size()) fl.fail_at(pos, "unexpected text after the value");
            v.line = n.line;
            v.col = n.col;
            skip();
            return v;
        }
        // Plain scalar, folded over continuation lines.
        n.kind = Node::Kind::Scalar;
        std::string text(strip_comment(c));
        ++cur_;
        while (ok_ && cur_ < lines_.size()) {
            if (blank(cur_)) {
                // A blank line inside a multi-line plain scalar is a line break; a comment ends it.
                std::size_t j = cur_;
                while (j < lines_.size() && content(j).empty()) ++j;
                if (j < lines_.size() && !blank(j) && indent(j) > parent && j > cur_ && !is_seq_item(content(j)) &&
                    key_colon(content(j)) == std::string_view::npos) {
                    text += '\n';
                    cur_ = j;
                    continue;
                }
                break;
            }
            if (indent(cur_) <= parent) break;
            const std::string_view cc = content(cur_);
            if (key_colon(cc) != std::string_view::npos || is_seq_item(cc)) break;
            if (!text.empty() && text.back() != '\n') text += ' ';
            text += strip_comment(cc);
            ++cur_;
        }
        n.text = std::move(text);
        skip();
        return n;
    }

    static std::string_view strip_comment(std::string_view c) {
        for (std::size_t i = 1; i < c.size(); ++i) {
            if (c[i] == '#' && is_space(c[i - 1])) {
                c = c.substr(0, i);
                break;
            }
        }
        while (!c.empty() && is_space(c.back())) c.remove_suffix(1);
        return c;
    }

    // Whether the quotes and brackets that `t` opens are closed.
    static bool balanced(std::string_view t) {
        int depth = 0;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const char ch = t[i];
            if (ch == '"') {
                ++i;
                while (i < t.size() && t[i] != '"') i += t[i] == '\\' ? 2u : 1u;
                if (i >= t.size()) return false;
                if (depth == 0) return true;
            } else if (ch == '\'') {
                ++i;
                while (i < t.size()) {
                    if (t[i] == '\'') {
                        if (i + 1 < t.size() && t[i + 1] == '\'') {
                            i += 2;
                            continue;
                        }
                        break;
                    }
                    ++i;
                }
                if (i >= t.size()) return false;
                if (depth == 0) return true;
            } else if (ch == '#' && (i == 0 || is_space(t[i - 1]) || t[i - 1] == '\n')) {
                while (i < t.size() && t[i] != '\n') ++i;
            } else if (ch == '[' || ch == '{') {
                ++depth;
            } else if (ch == ']' || ch == '}') {
                if (--depth <= 0) return true;
            }
        }
        return depth <= 0;
    }

    Node block_scalar(int parent, Node n) {
        const std::size_t head = cur_;
        const std::string_view h = strip_comment(content(cur_));
        const bool folded = h[0] == '>';
        char chomp = 'c';
        int explicit_indent = 0;
        for (std::size_t i = 1; i < h.size(); ++i) {
            if (h[i] == '-' || h[i] == '+') chomp = h[i];
            else if (h[i] >= '1' && h[i] <= '9') explicit_indent = h[i] - '0';
            else {
                fail(head, indent(head) + static_cast<int>(i), "bad block scalar header");
                return n;
            }
        }
        ++cur_;
        // Content indent: given, else the first line with text.
        int ci = 0;
        if (explicit_indent) {
            ci = std::max(parent, 0) + explicit_indent;
            if (parent < 0) ci = explicit_indent;
        } else {
            for (std::size_t j = cur_; j < lines_.size(); ++j) {
                const std::string& s = lines_[j].s;
                const std::size_t k = s.find_first_not_of(' ');
                if (k == std::string::npos) continue;
                ci = static_cast<int>(k);
                break;
            }
            if (ci <= parent) ci = parent + 1;
        }
        std::vector<std::string> body;
        while (cur_ < lines_.size()) {
            const std::string& s = lines_[cur_].s;
            const std::size_t k = s.find_first_not_of(' ');
            if (k == std::string::npos) {
                body.emplace_back();
                ++cur_;
                continue;
            }
            if (static_cast<int>(k) < ci) break;
            body.push_back(s.substr(static_cast<std::size_t>(ci)));
            ++cur_;
        }
        // Trailing blank lines belong to the chomping.
        std::size_t trailing = 0;
        while (!body.empty() && body.back().empty()) {
            body.pop_back();
            ++trailing;
        }
        std::string text;
        for (std::size_t i = 0; i < body.size(); ++i) {
            if (i) {
                const bool fold = folded && !body[i - 1].empty() && !body[i].empty() && body[i][0] != ' ' && body[i - 1][0] != ' ';
                text += fold ? ' ' : '\n';
            }
            text += body[i];
        }
        if (!body.empty()) {
            if (chomp == 'c') text += '\n';
            else if (chomp == '+') text += std::string(trailing + 1, '\n');
        }
        n.kind = Node::Kind::Scalar;
        n.quoted = true;
        n.text = std::move(text);
        skip();
        return n;
    }

    // A quoted string at t[pos] (pos at the quote); pos ends after the closing quote. Line
    // breaks inside fold to a space (an empty line to a line break).
    std::string quoted_text(std::string_view t, std::size_t& pos, std::size_t line, int col0) {
        const char q = t[pos];
        std::string out;
        std::size_t i = pos + 1;
        auto fold = [&](std::size_t& j) {
            // At a '\n': trailing spaces of out go, leading spaces of the next line go.
            while (!out.empty() && is_space(out.back())) out.pop_back();
            int breaks = 0;
            while (j < t.size() && (t[j] == '\n' || is_space(t[j]))) {
                if (t[j] == '\n') ++breaks;
                ++j;
            }
            if (breaks <= 1) out += ' ';
            else out.append(static_cast<std::size_t>(breaks - 1), '\n');
        };
        while (i < t.size()) {
            const char ch = t[i];
            if (ch == '\n') {
                fold(i);
                continue;
            }
            if (ch == q) {
                if (q == '\'' && i + 1 < t.size() && t[i + 1] == '\'') {
                    out += '\'';
                    i += 2;
                    continue;
                }
                pos = i + 1;
                return out;
            }
            if (q == '"' && ch == '\\') {
                if (i + 1 >= t.size()) break;
                const char e = t[i + 1];
                i += 2;
                switch (e) {
                    case '0': out += '\0'; break;
                    case 'a': out += '\a'; break;
                    case 'b': out += '\b'; break;
                    case 't': case '\t': out += '\t'; break;
                    case 'n': out += '\n'; break;
                    case 'v': out += '\v'; break;
                    case 'f': out += '\f'; break;
                    case 'r': out += '\r'; break;
                    case 'e': out += '\x1b'; break;
                    case ' ': out += ' '; break;
                    case '"': out += '"'; break;
                    case '/': out += '/'; break;
                    case '\\': out += '\\'; break;
                    case 'N': append_utf8(out, 0x85); break;
                    case '_': append_utf8(out, 0xA0); break;
                    case 'L': append_utf8(out, 0x2028); break;
                    case 'P': append_utf8(out, 0x2029); break;
                    case '\n': {   // an escaped line break joins the lines
                        while (i < t.size() && is_space(t[i])) ++i;
                        break;
                    }
                    case 'x': case 'u': case 'U': {
                        const std::size_t len = e == 'x' ? 2 : (e == 'u' ? 4 : 8);
                        std::uint32_t cp = 0;
                        for (std::size_t k = 0; k < len; ++k) {
                            const int hv = i + k < t.size() ? hex_value(t[i + k]) : -1;
                            if (hv < 0) {
                                fail(line, col0 + static_cast<int>(i), "bad escape in a double-quoted string");
                                return out;
                            }
                            cp = cp * 16 + static_cast<std::uint32_t>(hv);
                        }
                        i += len;
                        append_utf8(out, cp);
                        break;
                    }
                    default: fail(line, col0 + static_cast<int>(i) - 2, std::string("unknown escape \\") + e); return out;
                }
                continue;
            }
            out += ch;
            ++i;
        }
        fail(line, col0 + static_cast<int>(pos), "unterminated quoted string");
        return out;
    }

    // ---------------------------------------------------------------- flow collections

    struct Flow {
        Parser& p;
        std::string_view t;
        const std::vector<std::pair<std::size_t, std::size_t>>& starts;   // offset -> line index
        int col0;

        // The line index and the 0-based column of offset `pos`.
        std::pair<std::size_t, int> where(std::size_t pos) const {
            std::size_t li = starts.front().second, base = 0;
            bool first = true;
            for (const auto& [off, line] : starts) {
                if (off > pos) break;
                li = line;
                base = off;
                first = off == 0;
            }
            return {li, static_cast<int>(pos - base) + (first ? col0 : 0)};
        }
        void fail_at(std::size_t pos, std::string msg) {
            auto [li, c] = where(pos);
            p.fail(li, c, std::move(msg));
        }
        void ws(std::size_t& pos) const {
            while (pos < t.size()) {
                if (is_space(t[pos]) || t[pos] == '\n') {
                    ++pos;
                } else if (t[pos] == '#' && (pos == 0 || is_space(t[pos - 1]) || t[pos - 1] == '\n')) {
                    while (pos < t.size() && t[pos] != '\n') ++pos;
                } else {
                    break;
                }
            }
        }

        Node value(std::size_t& pos, int depth) {
            Node n;
            if (depth > kMaxDepth) {
                fail_at(pos, "nested too deep");
                return n;
            }
            ws(pos);
            if (pos >= t.size()) return n;
            auto [li, c] = where(pos);
            n.line = p.lines_[li].no;
            n.col = c + 1;
            const char f = t[pos];
            if (f == '{' || f == '[') {
                const bool is_map = f == '{';
                const char close = is_map ? '}' : ']';
                n.kind = is_map ? Node::Kind::Map : Node::Kind::Seq;
                ++pos;
                while (p.ok_) {
                    ws(pos);
                    if (pos >= t.size()) {
                        fail_at(pos, std::string("missing '") + close + "'");
                        break;
                    }
                    if (t[pos] == close) {
                        ++pos;
                        break;
                    }
                    if (is_map) {
                        Node k = scalar_at(pos, true);
                        if (!p.ok_) break;
                        ws(pos);
                        Node v;
                        if (pos < t.size() && t[pos] == ':') {
                            ++pos;
                            ws(pos);
                            if (pos < t.size() && t[pos] != ',' && t[pos] != '}') v = value(pos, depth + 1);
                        }
                        for (const auto& [ek, ev] : n.map) {
                            if (ek == k.text) fail_at(pos, "duplicate key \"" + k.text + "\"");
                        }
                        n.map.emplace_back(std::move(k.text), std::move(v));
                    } else {
                        n.seq.push_back(value(pos, depth + 1));
                    }
                    if (!p.ok_) break;
                    ws(pos);
                    if (pos < t.size() && t[pos] == ',') {
                        ++pos;
                    } else if (pos < t.size() && t[pos] == close) {
                        ++pos;
                        break;
                    } else if (pos >= t.size()) {
                        fail_at(pos, std::string("missing '") + close + "'");
                        break;
                    } else {
                        fail_at(pos, std::string("expected ',' or '") + close + "'");
                        break;
                    }
                }
                return n;
            }
            Node s = scalar_at(pos, false);
            s.line = n.line;
            s.col = n.col;
            return s;
        }

        Node scalar_at(std::size_t& pos, bool key) {
            Node n;
            n.kind = Node::Kind::Scalar;
            if (pos >= t.size()) return n;
            const char f = t[pos];
            if (f == '"' || f == '\'') {
                auto [li, c] = where(pos);
                std::size_t q = pos;
                n.text = p.quoted_text(t, q, li, c - static_cast<int>(pos));
                n.quoted = true;
                pos = q;
                return n;
            }
            if (f == '&' || f == '*' || f == '!') {
                fail_at(pos, "anchors, aliases and tags are not supported");
                return n;
            }
            std::size_t e = pos;
            while (e < t.size()) {
                const char ch = t[e];
                if (ch == ',' || ch == '[' || ch == ']' || ch == '{' || ch == '}') break;
                if (ch == ':' && (e + 1 == t.size() || is_space(t[e + 1]) || t[e + 1] == '\n' || t[e + 1] == ',' ||
                                  t[e + 1] == '}' || t[e + 1] == ']' || key))
                    break;
                if (ch == '#' && e > pos && (is_space(t[e - 1]) || t[e - 1] == '\n')) break;
                ++e;
            }
            std::string_view v = t.substr(pos, e - pos);
            pos = e;
            std::string out;
            // Line breaks fold to spaces.
            bool sp = false;
            for (char ch : v) {
                if (ch == '\n' || is_space(ch)) {
                    sp = true;
                    continue;
                }
                if (sp && !out.empty()) out += ' ';
                sp = false;
                out += ch;
            }
            n.text = std::move(out);
            return n;
        }
    };
};

bool needs_quotes(std::string_view s) {
    if (s.empty()) return true;
    if (is_space(s.front()) || is_space(s.back())) return true;
    static constexpr std::string_view kSpecialStart = "-?:,[]{}#&*!|>'\"%@`";
    if (kSpecialStart.find(s.front()) != std::string_view::npos) {
        // "-x", ":x", "?x" are plain in YAML 1.2, but quoting keeps every reader on the safe side.
        return true;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto ch = static_cast<unsigned char>(s[i]);
        if (ch < 0x20 || ch == 0x7F) return true;
        if (s[i] == ':' && (i + 1 == s.size() || s[i + 1] == ' ')) return true;
        if (s[i] == '#' && i > 0 && s[i - 1] == ' ') return true;
    }
    // Words other readers type (YAML 1.1 booleans, null) and numbers.
    std::string low;
    for (char ch : s) low += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    for (std::string_view w : {"~", "null", "true", "false", "yes", "no", "on", "off", "y", "n", ".inf", "-.inf", ".nan"}) {
        if (low == w) return true;
    }
    bool numeric = true, digit = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch >= '0' && ch <= '9') digit = true;
        else if (!(ch == '.' || ch == '_' || ch == 'e' || ch == 'E' || ((ch == '+' || ch == '-') && (i == 0 || s[i - 1] == 'e' || s[i - 1] == 'E')) ||
                   (i == 1 && (ch == 'x' || ch == 'o' || ch == 'b') && s[0] == '0') || ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))))
            numeric = false;
    }
    return numeric && digit;
}

}  // namespace

Expected<Node, ParseError> parse(std::string_view text) {
    if (text.size() > kMaxText) return Unexpected<ParseError>(ParseError{"the file is too large", 1, 1});
    Parser p(text);
    return p.run();
}

std::string scalar(std::string_view s) {
    if (!needs_quotes(s)) return std::string(s);
    std::string out = "\"";
    for (char ch : s) {
        const auto u = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (u < 0x20 || u == 0x7F) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\x%02x", u);
                    out += b;
                } else {
                    out += ch;
                }
        }
    }
    return out + "\"";
}

}  // namespace kt::ws::yaml
