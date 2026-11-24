// Format-preserving TOML edits (see toml_edit.h).
#include "config/toml_edit.h"

#include <sstream>

#include <toml.hpp>

namespace kt::config::toml_edit {
namespace {

struct Stmt {
    enum class Kind : uint8_t { Header, KeyValue } kind = Kind::KeyValue;
    Path path;              // header: the table; key/value: the full path (table + key)
    Path table;             // key/value: the table it is in
    bool array = false;     // header: [[...]]
    bool in_array = false;  // inside an array-of-tables entry
    size_t line_begin = 0, value_begin = 0, value_end = 0, line_end = 0;
};

struct Scanner {
    std::string_view s;
    size_t p = 0;
    std::string error;

    bool eof() const { return p >= s.size(); }
    char at(size_t i) const { return i < s.size() ? s[i] : '\0'; }
    void ws() {
        while (!eof() && (s[p] == ' ' || s[p] == '\t')) ++p;
    }
    // Spaces, newlines and comments (inside arrays).
    void ws_nl() {
        for (;;) {
            ws();
            if (eof()) return;
            if (s[p] == '\n' || s[p] == '\r') {
                ++p;
            } else if (s[p] == '#') {
                while (!eof() && s[p] != '\n') ++p;
            } else {
                return;
            }
        }
    }
    // The rest of the line: spaces, an optional comment, the newline.
    bool eol() {
        ws();
        if (!eof() && s[p] == '#') {
            while (!eof() && s[p] != '\n') ++p;
        }
        if (!eof() && s[p] == '\r') ++p;
        if (eof()) return true;
        if (s[p] != '\n') return fail("unexpected text after a value");
        ++p;
        return true;
    }
    bool fail(std::string m) {
        if (error.empty()) error = std::move(m);
        return false;
    }

    static bool bare(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    bool basic_string(std::string* out) {
        ++p;   // opening quote
        while (!eof() && s[p] != '"') {
            if (s[p] == '\n') return fail("newline in a string");
            if (s[p] == '\\') {
                const char e = at(p + 1);
                p += 2;
                if (!out) continue;
                switch (e) {
                    case 'n': *out += '\n'; break;
                    case 't': *out += '\t'; break;
                    case 'r': *out += '\r'; break;
                    case 'b': *out += '\b'; break;
                    case 'f': *out += '\f'; break;
                    case 'u':
                    case 'U': {
                        const size_t n = e == 'u' ? 4 : 8;
                        uint32_t cp = 0;
                        for (size_t i = 0; i < n && !eof(); ++i, ++p) {
                            const char h = s[p];
                            cp = cp * 16 + static_cast<uint32_t>(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                        }
                        if (cp < 0x80) {
                            *out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            *out += static_cast<char>(0xC0 | (cp >> 6));
                            *out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            *out += static_cast<char>(0xE0 | (cp >> 12));
                            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            *out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            *out += static_cast<char>(0xF0 | (cp >> 18));
                            *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            *out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: *out += e; break;
                }
                continue;
            }
            if (out) *out += s[p];
            ++p;
        }
        if (eof()) return fail("unterminated string");
        ++p;
        return true;
    }

    bool literal_string(std::string* out) {
        ++p;
        while (!eof() && s[p] != '\'') {
            if (s[p] == '\n') return fail("newline in a string");
            if (out) *out += s[p];
            ++p;
        }
        if (eof()) return fail("unterminated string");
        ++p;
        return true;
    }

    bool multiline(char q) {
        p += 3;
        for (;;) {
            if (eof()) return fail("unterminated multi-line string");
            if (q == '"' && s[p] == '\\') {
                p += 2;
                continue;
            }
            if (s[p] == q && at(p + 1) == q && at(p + 2) == q) {
                p += 3;
                // Up to two more quotes belong to the content.
                for (int i = 0; i < 2 && !eof() && s[p] == q; ++i) ++p;
                return true;
            }
            ++p;
        }
    }

    bool key(Path& out) {
        for (;;) {
            ws();
            if (eof()) return fail("missing key");
            std::string seg;
            if (s[p] == '"') {
                if (!basic_string(&seg)) return false;
            } else if (s[p] == '\'') {
                if (!literal_string(&seg)) return false;
            } else {
                const size_t b = p;
                while (!eof() && bare(s[p])) ++p;
                if (p == b) return fail("invalid key");
                seg.assign(s.substr(b, p - b));
            }
            out.push_back(std::move(seg));
            ws();
            if (!eof() && s[p] == '.') {
                ++p;
                continue;
            }
            return true;
        }
    }

    bool value() {
        if (eof()) return fail("missing value");
        const char c = s[p];
        if (c == '"') {
            if (at(p + 1) == '"' && at(p + 2) == '"') return multiline('"');
            return basic_string(nullptr);
        }
        if (c == '\'') {
            if (at(p + 1) == '\'' && at(p + 2) == '\'') return multiline('\'');
            return literal_string(nullptr);
        }
        if (c == '[') {
            ++p;
            for (;;) {
                ws_nl();
                if (eof()) return fail("unterminated array");
                if (s[p] == ']') {
                    ++p;
                    return true;
                }
                if (!value()) return false;
                ws_nl();
                if (!eof() && s[p] == ',') ++p;
            }
        }
        if (c == '{') {
            ++p;
            for (;;) {
                ws_nl();
                if (eof()) return fail("unterminated inline table");
                if (s[p] == '}') {
                    ++p;
                    return true;
                }
                Path k;
                if (!key(k)) return false;
                ws();
                if (eof() || s[p] != '=') return fail("missing '=' in an inline table");
                ++p;
                ws();
                if (!value()) return false;
                ws_nl();
                if (!eof() && s[p] == ',') ++p;
            }
        }
        const size_t b = p;
        while (!eof() && s[p] != ' ' && s[p] != '\t' && s[p] != '\n' && s[p] != '\r' && s[p] != ',' && s[p] != ']' &&
               s[p] != '}' && s[p] != '#')
            ++p;
        if (p == b) return fail("missing value");
        return true;
    }
};

bool starts_with(const Path& p, const Path& prefix) {
    if (prefix.size() > p.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (p[i] != prefix[i]) return false;
    }
    return true;
}

Result<std::vector<Stmt>> scan(std::string_view doc) {
    Scanner sc{doc, 0, {}};
    std::vector<Stmt> out;
    Path table;
    bool in_array = false;
    std::vector<Path> arrays;
    while (!sc.eof()) {
        const size_t line_begin = sc.p;
        sc.ws();
        if (sc.eof()) break;
        const char c = doc[sc.p];
        if (c == '\n' || c == '\r' || c == '#') {
            if (!sc.eol()) break;
            continue;
        }
        Stmt st;
        st.line_begin = line_begin;
        if (c == '[') {
            st.kind = Stmt::Kind::Header;
            st.array = sc.at(sc.p + 1) == '[';
            sc.p += st.array ? 2 : 1;
            if (!sc.key(st.path)) break;
            sc.ws();
            if (sc.at(sc.p) != ']' || (st.array && sc.at(sc.p + 1) != ']')) {
                sc.fail("unterminated table header");
                break;
            }
            sc.p += st.array ? 2 : 1;
            st.value_begin = st.value_end = sc.p;
            if (!sc.eol()) break;
            st.line_end = sc.p;
            table = st.path;
            in_array = st.array;
            if (!in_array) {
                for (const Path& a : arrays) {
                    if (starts_with(table, a)) in_array = true;
                }
            }
            if (st.array) arrays.push_back(st.path);
            st.in_array = in_array;
            out.push_back(std::move(st));
            continue;
        }
        Path k;
        if (!sc.key(k)) break;
        sc.ws();
        if (sc.eof() || doc[sc.p] != '=') {
            sc.fail("missing '='");
            break;
        }
        ++sc.p;
        sc.ws();
        st.value_begin = sc.p;
        if (!sc.value()) break;
        st.value_end = sc.p;
        if (!sc.eol()) break;
        st.line_end = sc.p;
        st.table = table;
        st.path = table;
        st.path.insert(st.path.end(), k.begin(), k.end());
        st.in_array = in_array;
        out.push_back(std::move(st));
    }
    if (!sc.error.empty()) {
        size_t line = 1;
        for (size_t i = 0; i < sc.p && i < doc.size(); ++i) line += doc[i] == '\n';
        return make_error(Errc::invalid_argument, "line " + std::to_string(line) + ": " + sc.error);
    }
    return out;
}

const toml::node* node_at(const toml::table& t, const Path& p) {
    const toml::node* n = &t;
    for (const std::string& seg : p) {
        const toml::table* tt = n ? n->as_table() : nullptr;
        if (!tt) return nullptr;
        n = tt->get(seg);
    }
    return n;
}

std::string render(const toml::node& n) {
    std::ostringstream o;
    n.visit([&](auto&& v) { o << v; });
    return o.str();
}

std::string parse_error_text(const toml::parse_error& e) {
    std::ostringstream o;
    o << "line " << e.source().begin.line << ", column " << e.source().begin.column << ": " << e.description();
    return o.str();
}

std::string newline_of(std::string_view doc) { return doc.find("\r\n") != std::string_view::npos ? "\r\n" : "\n"; }

std::string indent_of(std::string_view doc, const Stmt& st) {
    size_t i = st.line_begin;
    while (i < doc.size() && (doc[i] == ' ' || doc[i] == '\t')) ++i;
    return std::string(doc.substr(st.line_begin, i - st.line_begin));
}

// The insertion text and position for a new assignment.
Result<std::string> insert(std::string_view doc, const std::vector<Stmt>& stmts, const Path& path, std::string_view value) {
    const std::string nl = newline_of(doc);
    const Path table(path.begin(), path.end() - 1);
    std::string out(doc);
    auto put_after = [&](const Stmt& st, const Path& key) {
        size_t at = st.line_end;
        std::string line = indent_of(doc, st) + format_path(key) + " = " + std::string(value) + nl;
        if (at == doc.size() && !doc.empty() && doc.back() != '\n') line = nl + line;
        out.insert(at, line);
        return out;
    };
    // 1. The table's own header: after its last assignment.
    for (size_t i = 0; i < stmts.size(); ++i) {
        const Stmt& h = stmts[i];
        if (h.kind != Stmt::Kind::Header || h.array || h.in_array || h.path != table) continue;
        const Stmt* last = &h;
        for (size_t j = i + 1; j < stmts.size() && stmts[j].kind == Stmt::Kind::KeyValue; ++j) last = &stmts[j];
        if (last == &h) {
            size_t at = h.line_end;
            std::string line = format_key(path.back()) + " = " + std::string(value) + nl;
            if (at == doc.size() && !doc.empty() && doc.back() != '\n') line = nl + line;
            out.insert(at, line);
            return out;
        }
        return put_after(*last, Path(path.begin() + static_cast<long>(last->table.size()), path.end()));
    }
    // 2. A dotted assignment that defines the table implicitly (`font.size = 12` at the top).
    const Stmt* anchor = nullptr;
    for (const Stmt& st : stmts) {
        if (st.kind != Stmt::Kind::KeyValue || st.in_array) continue;
        if (st.table.size() < table.size() && starts_with(table, st.table) && starts_with(st.path, table) &&
            st.path.size() > table.size())
            anchor = &st;
    }
    if (anchor) return put_after(*anchor, Path(path.begin() + static_cast<long>(anchor->table.size()), path.end()));
    // 3. The top level: after the last top-level assignment, else before the first header.
    if (table.empty()) {
        const Stmt* last = nullptr;
        for (const Stmt& st : stmts) {
            if (st.kind == Stmt::Kind::Header) break;
            last = &st;
        }
        if (last) return put_after(*last, path);
        for (const Stmt& st : stmts) {
            if (st.kind == Stmt::Kind::Header) {
                out.insert(st.line_begin, format_key(path.back()) + " = " + std::string(value) + nl + nl);
                return out;
            }
        }
        if (!out.empty() && out.back() != '\n') out += nl;
        out += format_key(path.back()) + " = " + std::string(value) + nl;
        return out;
    }
    // 4. A new table at the end.
    if (!out.empty() && out.back() != '\n') out += nl;
    if (!out.empty()) out += nl;
    out += "[" + format_path(table) + "]" + nl + format_key(path.back()) + " = " + std::string(value) + nl;
    return out;
}

}  // namespace

Path split_path(std::string_view dotted) {
    Path p;
    size_t b = 0;
    for (;;) {
        const size_t e = dotted.find('.', b);
        p.emplace_back(dotted.substr(b, e == std::string_view::npos ? std::string_view::npos : e - b));
        if (e == std::string_view::npos) break;
        b = e + 1;
    }
    return p;
}

std::string format_key(std::string_view k) {
    bool is_bare = !k.empty();
    for (char c : k) is_bare = is_bare && Scanner::bare(c);
    if (is_bare) return std::string(k);
    std::string out = "\"";
    for (char c : k) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string format_path(const Path& p) {
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
        if (i) out += '.';
        out += format_key(p[i]);
    }
    return out;
}

Result<void> check(std::string_view doc) {
    toml::parse_result r = toml::parse(doc);
    if (!r) return make_error(Errc::invalid_argument, parse_error_text(r.error()));
    return {};
}

std::optional<std::string> find(std::string_view doc, const Path& path) {
    auto stmts = scan(doc);
    if (!stmts) return std::nullopt;
    for (const Stmt& st : *stmts) {
        if (st.kind == Stmt::Kind::KeyValue && !st.in_array && st.path == path)
            return std::string(doc.substr(st.value_begin, st.value_end - st.value_begin));
    }
    return std::nullopt;
}

Result<std::string> set(std::string_view doc, const Path& path, std::string_view value) {
    if (path.empty()) return make_error(Errc::invalid_argument, "empty key path");
    toml::parse_result before = toml::parse(doc);
    if (!before) return make_error(Errc::invalid_argument, "the file does not parse: " + parse_error_text(before.error()));
    toml::parse_result want = toml::parse("v = " + std::string(value));
    if (!want) return make_error(Errc::invalid_argument, "invalid value: " + std::string(want.error().description()));
    auto stmts = scan(doc);
    if (!stmts) return make_error(Errc::invalid_argument, stmts.error().what);

    std::string out;
    bool replaced = false;
    for (const Stmt& st : *stmts) {
        if (st.kind != Stmt::Kind::KeyValue || st.in_array) continue;
        if (st.path == path) {
            out.assign(doc.substr(0, st.value_begin));
            out += value;
            out += doc.substr(st.value_end);
            replaced = true;
            break;
        }
        if (st.path.size() < path.size() && starts_with(path, st.path))
            return make_error(Errc::not_supported, format_path(st.path) + " is written as one inline value; edit it in the file");
    }
    if (!replaced) {
        auto ins = insert(doc, *stmts, path, value);
        if (!ins) return ins;
        out = std::move(*ins);
    }
    toml::parse_result after = toml::parse(out);
    if (!after) return make_error(Errc::invalid_argument, "the edit would break the file: " + parse_error_text(after.error()));
    const toml::node* got = node_at(after.table(), path);
    const toml::node* exp = want.table().get("v");
    if (!got || !exp || got->type() != exp->type() || render(*got) != render(*exp))
        return make_error(Errc::invalid_argument, "the edit did not give the intended value for " + format_path(path));
    return out;
}

Result<std::string> remove(std::string_view doc, const Path& path, bool* removed) {
    if (removed) *removed = false;
    auto stmts = scan(doc);
    if (!stmts) return make_error(Errc::invalid_argument, stmts.error().what);
    for (const Stmt& st : *stmts) {
        if (st.kind != Stmt::Kind::KeyValue || st.in_array || st.path != path) continue;
        std::string out(doc.substr(0, st.line_begin));
        out += doc.substr(st.line_end);
        if (auto ok = check(out); !ok) return make_error(Errc::invalid_argument, "the edit would break the file: " + ok.error().what);
        if (removed) *removed = true;
        return out;
    }
    return std::string(doc);
}

}  // namespace kt::config::toml_edit
