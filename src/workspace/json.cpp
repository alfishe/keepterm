#include "workspace/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace kt::ws::json {

Value Value::boolean(bool b) {
    Value v;
    v.kind_ = Kind::Bool;
    v.b_ = b;
    return v;
}
Value Value::integer(std::int64_t i) {
    Value v;
    v.kind_ = Kind::Int;
    v.i_ = i;
    return v;
}
Value Value::number(double d) {
    Value v;
    v.kind_ = Kind::Double;
    v.d_ = std::isfinite(d) ? d : 0.0;
    return v;
}
Value Value::string(std::string s) {
    Value v;
    v.kind_ = Kind::String;
    v.s_ = std::move(s);
    return v;
}
Value Value::array() {
    Value v;
    v.kind_ = Kind::Array;
    return v;
}
Value Value::object() {
    Value v;
    v.kind_ = Kind::Object;
    return v;
}

std::optional<bool> Value::as_bool() const {
    if (kind_ != Kind::Bool) return std::nullopt;
    return b_;
}
std::optional<double> Value::as_double() const {
    if (kind_ == Kind::Int) return static_cast<double>(i_);
    if (kind_ == Kind::Double) return d_;
    return std::nullopt;
}
std::optional<std::int64_t> Value::as_int() const {
    if (kind_ == Kind::Int) return i_;
    if (kind_ == Kind::Double && d_ >= -9.2e18 && d_ <= 9.2e18 && std::floor(d_) == d_)
        return static_cast<std::int64_t>(d_);
    return std::nullopt;
}
const std::string* Value::as_string() const { return kind_ == Kind::String ? &s_ : nullptr; }
const std::vector<Value>* Value::as_array() const { return kind_ == Kind::Array ? &items_ : nullptr; }
std::vector<Value>* Value::as_array() { return kind_ == Kind::Array ? &items_ : nullptr; }

namespace {
template <class V>
auto find_member(V& members, std::string_view key) {
    return std::lower_bound(members.begin(), members.end(), key,
                            [](const auto& m, std::string_view k) { return std::string_view(m.first) < k; });
}
}  // namespace

const Value* Value::get(std::string_view key) const {
    if (kind_ != Kind::Object) return nullptr;
    auto it = find_member(members_, key);
    return it != members_.end() && it->first == key ? &it->second : nullptr;
}
Value* Value::get(std::string_view key) {
    if (kind_ != Kind::Object) return nullptr;
    auto it = find_member(members_, key);
    return it != members_.end() && it->first == key ? &it->second : nullptr;
}
Value& Value::set(std::string key, Value v) {
    if (kind_ == Kind::Null) kind_ = Kind::Object;
    auto it = find_member(members_, key);
    if (it != members_.end() && it->first == key) {
        it->second = std::move(v);
        return it->second;
    }
    it = members_.insert(it, {std::move(key), std::move(v)});
    return it->second;
}
bool Value::erase(std::string_view key) {
    if (kind_ != Kind::Object) return false;
    auto it = find_member(members_, key);
    if (it == members_.end() || it->first != key) return false;
    members_.erase(it);
    return true;
}
std::size_t Value::size() const {
    if (kind_ == Kind::Object) return members_.size();
    if (kind_ == Kind::Array) return items_.size();
    return 0;
}
Value& Value::push(Value v) {
    if (kind_ == Kind::Null) kind_ = Kind::Array;
    items_.push_back(std::move(v));
    return items_.back();
}

bool Value::operator==(const Value& o) const {
    if (kind_ != o.kind_) {
        if (is_number() && o.is_number()) return as_double() == o.as_double();
        return false;
    }
    switch (kind_) {
        case Kind::Null: return true;
        case Kind::Bool: return b_ == o.b_;
        case Kind::Int: return i_ == o.i_;
        case Kind::Double: return d_ == o.d_;
        case Kind::String: return s_ == o.s_;
        case Kind::Array: return items_ == o.items_;
        case Kind::Object: return members_ == o.members_;
    }
    return false;
}

void write_string(std::string& out, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    out += "\\u00";
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

std::string format_double(double d) {
    if (!std::isfinite(d)) return "0";
    char buf[40];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s(buf);
    // Keep it a JSON double that reads back as one: "1" -> "1.0" only matters for kind, and
    // as_double() accepts ints, so plain integers are fine. Normalize "-0".
    if (s == "-0") s = "0";
    return s;
}

void Value::dump_to(std::string& out, bool pretty, int indent) const {
    auto newline = [&](int ind) {
        if (!pretty) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(ind) * 2, ' ');
    };
    switch (kind_) {
        case Kind::Null: out += "null"; break;
        case Kind::Bool: out += b_ ? "true" : "false"; break;
        case Kind::Int: out += std::to_string(i_); break;
        case Kind::Double: out += format_double(d_); break;
        case Kind::String: write_string(out, s_); break;
        case Kind::Array:
            if (items_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out.push_back(',');
                newline(indent + 1);
                items_[i].dump_to(out, pretty, indent + 1);
            }
            newline(indent);
            out.push_back(']');
            break;
        case Kind::Object:
            if (members_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i) out.push_back(',');
                newline(indent + 1);
                write_string(out, members_[i].first);
                out += pretty ? ": " : ":";
                members_[i].second.dump_to(out, pretty, indent + 1);
            }
            newline(indent);
            out.push_back('}');
            break;
    }
}

std::string Value::dump(bool pretty) const {
    std::string out;
    dump_to(out, pretty, 0);
    if (pretty) out.push_back('\n');
    return out;
}

// ----------------------------------------------------------------------------------- parse

namespace {

constexpr int kMaxDepth = 128;
constexpr std::size_t kMaxInput = 16u << 20;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool run(Value& out, ParseError* err) {
        bool ok = value(out, 0);
        if (ok) {
            ws();
            if (p_ != s_.size()) {
                ok = false;
                msg_ = "trailing data";
            }
        }
        if (!ok && err) {
            err->offset = p_;
            err->message = msg_.empty() ? "syntax error" : msg_;
        }
        return ok;
    }

private:
    bool fail(const char* m) {
        if (msg_.empty()) msg_ = m;
        return false;
    }
    void ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
    }
    bool lit(std::string_view w) {
        if (s_.substr(p_, w.size()) != w) return false;
        p_ += w.size();
        return true;
    }
    static void put_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(std::uint32_t& v) {
        if (s_.size() - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (p_ >= s_.size() || s_[p_] != '"') return fail("expected a string");
        ++p_;
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p_ >= s_.size()) return fail("bad escape");
            const char e = s_[p_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t lo = 0;
                        if (!lit("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("bad surrogate");
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Value& v) {
        const std::size_t start = p_;
        if (p_ < s_.size() && s_[p_] == '-') ++p_;
        const std::size_t int_start = p_;
        while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
        if (p_ == int_start) return fail("bad number");
        if (s_[int_start] == '0' && p_ - int_start > 1) return fail("leading zero");
        bool frac = false;
        if (p_ < s_.size() && s_[p_] == '.') {
            frac = true;
            ++p_;
            const std::size_t f = p_;
            while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
            if (p_ == f) return fail("bad number");
        }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
            frac = true;
            ++p_;
            if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
            const std::size_t e = p_;
            while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
            if (p_ == e) return fail("bad number");
        }
        const std::string_view tok = s_.substr(start, p_ - start);
        if (!frac) {
            std::int64_t i = 0;
            const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), i);
            if (r.ec == std::errc{} && r.ptr == tok.data() + tok.size()) {
                v = Value::integer(i);
                return true;
            }
        }
        const std::string copy(tok);
        v = Value::number(std::strtod(copy.c_str(), nullptr));
        return true;
    }
    bool value(Value& v, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        ws();
        if (p_ >= s_.size()) return fail("unexpected end");
        const char c = s_[p_];
        if (c == '{') {
            ++p_;
            v = Value::object();
            ws();
            if (p_ < s_.size() && s_[p_] == '}') {
                ++p_;
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (p_ >= s_.size() || s_[p_] != ':') return fail("expected ':'");
                ++p_;
                Value child;
                if (!value(child, depth + 1)) return false;
                v.set(std::move(key), std::move(child));
                ws();
                if (p_ >= s_.size()) return fail("unexpected end");
                if (s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (s_[p_] == '}') {
                    ++p_;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p_;
            v = Value::array();
            ws();
            if (p_ < s_.size() && s_[p_] == ']') {
                ++p_;
                return true;
            }
            for (;;) {
                Value child;
                if (!value(child, depth + 1)) return false;
                v.push(std::move(child));
                ws();
                if (p_ >= s_.size()) return fail("unexpected end");
                if (s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (s_[p_] == ']') {
                    ++p_;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!string(s)) return false;
            v = Value::string(std::move(s));
            return true;
        }
        if (lit("true")) {
            v = Value::boolean(true);
            return true;
        }
        if (lit("false")) {
            v = Value::boolean(false);
            return true;
        }
        if (lit("null")) {
            v = Value();
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return fail("unexpected character");
    }

    std::string_view s_;
    std::size_t p_ = 0;
    std::string msg_;
};

}  // namespace

bool parse(std::string_view text, Value& out, ParseError* err) {
    if (text.size() > kMaxInput) {
        if (err) *err = ParseError{0, "input too large"};
        return false;
    }
    Value v;
    if (!Parser(text).run(v, err)) return false;
    out = std::move(v);
    return true;
}

}  // namespace kt::ws::json
