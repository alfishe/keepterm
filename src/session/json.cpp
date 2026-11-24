#include "session/json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace kt::session::json {

Value Value::boolean(bool b) {
    Value v;
    v.kind_ = Kind::Bool;
    v.b_ = b;
    return v;
}

Value Value::number(double d) {
    Value v;
    v.kind_ = Kind::Number;
    v.num_ = d;
    return v;
}

Value Value::integer(int64_t i) {
    Value v;
    v.kind_ = Kind::Number;
    v.is_int_ = true;
    v.int_ = i;
    v.num_ = static_cast<double>(i);
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

std::optional<double> Value::as_number() const {
    if (kind_ != Kind::Number) return std::nullopt;
    return num_;
}

std::optional<int64_t> Value::as_int() const {
    if (kind_ != Kind::Number) return std::nullopt;
    if (is_int_) return int_;
    if (!(num_ >= -9.2e18 && num_ <= 9.2e18) || std::floor(num_) != num_) return std::nullopt;
    return static_cast<int64_t>(num_);
}

std::optional<uint64_t> Value::as_uint() const {
    auto i = as_int();
    if (!i || *i < 0) return std::nullopt;
    return static_cast<uint64_t>(*i);
}

const std::string* Value::as_string() const { return kind_ == Kind::String ? &s_ : nullptr; }
const std::vector<Value>* Value::as_array() const { return kind_ == Kind::Array ? &items_ : nullptr; }

const Value* Value::get(std::string_view key) const {
    if (kind_ != Kind::Object) return nullptr;
    for (const auto& [k, v] : members_) {
        if (k == key) return &v;
    }
    return nullptr;
}

Value& Value::set(std::string key, Value v) {
    for (auto& [k, old] : members_) {
        if (k == key) {
            old = std::move(v);
            return old;
        }
    }
    members_.emplace_back(std::move(key), std::move(v));
    return members_.back().second;
}

Value& Value::push(Value v) {
    items_.push_back(std::move(v));
    return items_.back();
}

std::string Value::str_or(std::string_view key, std::string_view fallback) const {
    const Value* v = get(key);
    if (v && v->as_string()) return *v->as_string();
    return std::string(fallback);
}

int64_t Value::int_or(std::string_view key, int64_t fallback) const {
    const Value* v = get(key);
    if (!v) return fallback;
    return v->as_int().value_or(fallback);
}

uint64_t Value::uint_or(std::string_view key, uint64_t fallback) const {
    const Value* v = get(key);
    if (!v) return fallback;
    return v->as_uint().value_or(fallback);
}

bool Value::bool_or(std::string_view key, bool fallback) const {
    const Value* v = get(key);
    if (!v) return fallback;
    return v->as_bool().value_or(fallback);
}

namespace {

void dump_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void newline(std::string& out, bool pretty, int indent) {
    if (!pretty) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent) * 2, ' ');
}

}  // namespace

void Value::dump_to(std::string& out, bool pretty, int indent) const {
    switch (kind_) {
        case Kind::Null: out += "null"; return;
        case Kind::Bool: out += b_ ? "true" : "false"; return;
        case Kind::Number: {
            if (is_int_) {
                out += std::to_string(int_);
            } else if (std::isfinite(num_)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.17g", num_);
                out += buf;
            } else {
                out += "null";
            }
            return;
        }
        case Kind::String: dump_string(out, s_); return;
        case Kind::Array: {
            out.push_back('[');
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out.push_back(',');
                if (pretty) out.push_back(' ');
                items_[i].dump_to(out, pretty, indent + 1);
            }
            if (pretty && !items_.empty()) out.push_back(' ');
            out.push_back(']');
            return;
        }
        case Kind::Object: {
            out.push_back('{');
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i) out.push_back(',');
                newline(out, pretty, indent + 1);
                dump_string(out, members_[i].first);
                out += pretty ? ": " : ":";
                members_[i].second.dump_to(out, pretty, indent + 1);
            }
            if (!members_.empty()) newline(out, pretty, indent);
            out.push_back('}');
            return;
        }
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

constexpr int kMaxDepth = 32;
constexpr std::size_t kMaxInput = 1u << 20;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Result<Value> run() {
        Value v;
        if (!value(v, 0)) return make_error(Errc::corrupt, "json: syntax error");
        ws();
        if (p_ != s_.size()) return make_error(Errc::corrupt, "json: trailing data");
        return v;
    }

private:
    void ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
    }

    bool lit(std::string_view w) {
        if (s_.substr(p_, w.size()) != w) return false;
        p_ += w.size();
        return true;
    }

    static void put_utf8(std::string& out, uint32_t cp) {
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

    bool hex4(uint32_t& v) {
        if (s_.size() - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        ++p_;
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p_ >= s_.size()) return false;
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
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo = 0;
                        if (!lit("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool number(Value& v) {
        const std::size_t start = p_;
        if (p_ < s_.size() && s_[p_] == '-') ++p_;
        bool frac = false;
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c >= '0' && c <= '9') {
                ++p_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                frac = true;
                ++p_;
            } else {
                break;
            }
        }
        const std::string_view tok = s_.substr(start, p_ - start);
        if (tok.empty() || tok == "-") return false;
        if (!frac) {
            int64_t i = 0;
            const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), i);
            if (r.ec == std::errc{} && r.ptr == tok.data() + tok.size()) {
                v = Value::integer(i);
                return true;
            }
        }
        const std::string copy(tok);
        char* end = nullptr;
        const double d = std::strtod(copy.c_str(), &end);
        if (end != copy.c_str() + copy.size()) return false;
        v = Value::number(d);
        return true;
    }

    bool value(Value& v, int depth) {
        if (depth > kMaxDepth) return false;
        ws();
        if (p_ >= s_.size()) return false;
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
                if (p_ >= s_.size() || s_[p_] != ':') return false;
                ++p_;
                Value child;
                if (!value(child, depth + 1)) return false;
                v.set(std::move(key), std::move(child));
                ws();
                if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (s_[p_] == '}') {
                    ++p_;
                    return true;
                }
                return false;
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
                if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (s_[p_] == ']') {
                    ++p_;
                    return true;
                }
                return false;
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
            v = Value::null();
            return true;
        }
        return number(v);
    }

    std::string_view s_;
    std::size_t p_ = 0;
};

}  // namespace

Result<Value> parse(std::string_view text) {
    if (text.size() > kMaxInput) return make_error(Errc::corrupt, "json: input too large");
    return Parser(text).run();
}

}  // namespace kt::session::json
