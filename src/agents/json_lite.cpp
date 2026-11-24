#include "agents/json_lite.h"

#include <cstdlib>

namespace kt::agents::json {

Value::Value() = default;
Value::Value(const Value&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(const Value&) = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;

namespace {

constexpr int kMaxDepth = 32;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    std::optional<Value> run() {
        Value v;
        if (!value(v, 0)) return std::nullopt;
        ws();
        if (pos_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return false;
        ws();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.kind = Value::Kind::String;
            return string(out.text);
        }
        if (c == 't' && literal("true")) {
            out.kind = Value::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (c == 'f' && literal("false")) {
            out.kind = Value::Kind::Bool;
            return true;
        }
        if (c == 'n' && literal("null")) return true;
        return number(out);
    }

    bool object(Value& out, int depth) {
        out.kind = Value::Kind::Object;
        ++pos_;
        ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') return false;
            std::string key;
            if (!string(key)) return false;
            ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            Value v;
            if (!value(v, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(v));
            ws();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool array(Value& out, int depth) {
        out.kind = Value::Kind::Array;
        ++pos_;
        ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            Value v;
            if (!value(v, depth + 1)) return false;
            out.items.push_back(std::move(v));
            ws();
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool hex4(std::uint32_t& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            std::uint32_t d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
            out = out * 16 + d;
        }
        return true;
    }

    static void utf8(std::string& out, std::uint32_t cp) {
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

    bool string(std::string& out) {
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        std::uint32_t lo = 0;
                        if (!literal("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool number(Value& out) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        const std::size_t int_start = pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (pos_ == int_start) return false;
        if (s_[int_start] == '0' && pos_ - int_start > 1) return false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            const std::size_t frac = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == frac) return false;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            const std::size_t exp = pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
            if (pos_ == exp) return false;
        }
        out.kind = Value::Kind::Number;
        const std::string_view text = s_.substr(start, pos_ - start);
        if (text.find_first_of(".eE") == std::string_view::npos) {
            // Integers (pids, counters) without strtod, which depends on the C locale.
            double n = 0;
            for (char d : text.substr(text.front() == '-' ? std::size_t{1} : std::size_t{0})) n = n * 10 + (d - '0');
            out.number = text.front() == '-' ? -n : n;
            return true;
        }
        out.number = std::strtod(std::string(text).c_str(), nullptr);
        return true;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

}  // namespace

const Value* Value::get(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const auto& [k, v] : members) {
        if (k == key) found = &v;
    }
    return found;
}

std::optional<std::string> Value::string_at(std::string_view key) const {
    const Value* v = get(key);
    if (v == nullptr || v->kind != Kind::String) return std::nullopt;
    return v->text;
}

std::optional<Value> parse(std::string_view text) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // BOM
    return Parser(text).run();
}

void write_string(std::string& out, std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (u < 0x20) {
            out += "\\u00";
            out += kHex[u >> 4];
            out += kHex[u & 0xF];
        } else {
            out += c;
        }
    }
    out += '"';
}

}  // namespace kt::agents::json
