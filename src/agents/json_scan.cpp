#include "agents/json_scan.h"

namespace kt::agents::jscan {
namespace {

constexpr int kMaxDepth = 32;

class Scanner {
public:
    Scanner(std::string_view doc, std::span<const std::string_view> wanted,
            const std::function<void(std::string_view, const Scalar&)>* on)
        : s_(doc), wanted_(wanted), on_(on) {}

    bool document() {
        ws();
        if (!value(0, true)) return false;
        ws();
        return pos_ == s_.size();
    }

    // Top-level member ranges.
    std::optional<TopLevel> members() {
        TopLevel t;
        ws();
        if (pos_ >= s_.size() || s_[pos_] != '{') return std::nullopt;
        t.open = pos_++;
        ws();
        if (peek('}')) {
            t.close = pos_++;
        } else {
            for (;;) {
                ws();
                Member m;
                m.key_begin = pos_;
                std::string_view key;
                if (!string(key)) return std::nullopt;
                m.key = key;
                ws();
                if (!eat(':')) return std::nullopt;
                ws();
                m.value_begin = pos_;
                if (!value(1, false)) return std::nullopt;
                m.value_end = pos_;
                t.members.push_back(m);
                ws();
                if (eat(',')) continue;
                if (pos_ < s_.size() && s_[pos_] == '}') {
                    t.close = pos_++;
                    break;
                }
                return std::nullopt;
            }
        }
        ws();
        if (pos_ != s_.size()) return std::nullopt;
        return t;
    }

private:
    bool peek(char c) const { return pos_ < s_.size() && s_[pos_] == c; }
    bool eat(char c) {
        if (!peek(c)) return false;
        ++pos_;
        return true;
    }
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    // Whether some wanted path equals `p` (exact) or starts with `p` + '.' (prefix).
    bool interesting(std::string_view p, bool& exact) const {
        exact = false;
        bool prefix = false;
        for (auto w : wanted_) {
            if (w == p) exact = true;
            else if (w.size() > p.size() && w.substr(0, p.size()) == p && w[p.size()] == '.') prefix = true;
        }
        return exact || prefix;
    }

    bool string(std::string_view& raw) {
        if (!eat('"')) return false;
        const std::size_t b = pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '"') {
                raw = s_.substr(b, pos_ - b);
                ++pos_;
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c == '\\') {
                if (pos_ + 1 >= s_.size()) return false;
                const char e = s_[pos_ + 1];
                if (e == 'u') {
                    if (pos_ + 6 > s_.size()) return false;
                    for (std::size_t k = 2; k < 6; ++k) {
                        const char h = s_[pos_ + k];
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
                    }
                    pos_ += 6;
                    continue;
                }
                if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' && e != 'n' && e != 'r' && e != 't') return false;
                pos_ += 2;
                continue;
            }
            ++pos_;
        }
        return false;
    }

    bool number(double& out) {
        const std::size_t b = pos_;
        bool neg = eat('-');
        if (pos_ >= s_.size()) return false;
        double v = 0;
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') v = v * 10 + (s_[pos_++] - '0');
        } else {
            return false;
        }
        if (eat('.')) {
            double scale = 0.1;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                v += (s_[pos_++] - '0') * scale;
                scale /= 10;
            }
        }
        if (peek('e') || peek('E')) {
            ++pos_;
            bool eneg = false;
            if (peek('+') || peek('-')) eneg = s_[pos_++] == '-';
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return false;
            int e = 0;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                if (e < 10000) e = e * 10 + (s_[pos_] - '0');
                ++pos_;
            }
            double m = 1;
            for (int k = 0; k < e && k < 400; ++k) m *= 10;
            v = eneg ? v / m : v * m;
        }
        (void)b;
        out = neg ? -v : v;
        return true;
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    void deliver(const Scalar& sc) {
        if (on_ && report_) (*on_)(path_, sc);
    }

    // `report`: this value's path is wanted exactly (scalars are delivered).
    bool value(int depth, bool report) {
        if (depth > kMaxDepth || pos_ >= s_.size()) return false;
        const bool saved = report_;
        report_ = report;
        Scalar sc;
        sc.begin = pos_;
        bool ok = true;
        switch (s_[pos_]) {
            case '{': ok = object(depth); break;
            case '[': ok = array(depth); break;
            case '"':
                sc.kind = Scalar::Kind::String;
                ok = string(sc.raw);
                break;
            case 't':
                sc.kind = Scalar::Kind::Bool;
                sc.boolean = true;
                ok = literal("true");
                break;
            case 'f':
                sc.kind = Scalar::Kind::Bool;
                ok = literal("false");
                break;
            case 'n': ok = literal("null"); break;
            default:
                sc.kind = Scalar::Kind::Number;
                ok = number(sc.number);
                break;
        }
        sc.end = pos_;
        if (ok && s_[sc.begin] != '{' && s_[sc.begin] != '[') deliver(sc);
        report_ = saved;
        return ok;
    }

    bool object(int depth) {
        ++pos_;
        ws();
        if (eat('}')) return true;
        for (;;) {
            ws();
            std::string_view key;
            if (!string(key)) return false;
            ws();
            if (!eat(':')) return false;
            ws();
            const std::size_t mark = path_.size();
            if (!path_.empty()) path_ += '.';
            path_ += key;
            bool exact = false;
            const bool want = !wanted_.empty() && interesting(path_, exact);
            bool ok;
            if (want) {
                ok = value(depth + 1, exact);
            } else {
                const bool saved = report_;
                report_ = false;
                const auto w = wanted_;
                wanted_ = {};
                ok = value(depth + 1, false);
                wanted_ = w;
                report_ = saved;
            }
            path_.resize(mark);
            if (!ok) return false;
            ws();
            if (eat(',')) continue;
            return eat('}');
        }
    }

    bool array(int depth) {
        ++pos_;
        ws();
        if (eat(']')) return true;
        // Array elements are never reported: scan them with nothing wanted.
        const auto w = wanted_;
        wanted_ = {};
        const bool saved = report_;
        report_ = false;
        bool ok = true;
        for (;;) {
            ws();
            if (!value(depth + 1, false)) {
                ok = false;
                break;
            }
            ws();
            if (eat(',')) continue;
            ok = eat(']');
            break;
        }
        wanted_ = w;
        report_ = saved;
        return ok;
    }

    std::string_view s_;
    std::span<const std::string_view> wanted_;
    const std::function<void(std::string_view, const Scalar&)>* on_;
    std::size_t pos_ = 0;
    std::string path_;
    bool report_ = false;
};

void put_utf8(std::string& out, std::uint32_t cp) {
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

std::uint32_t hex4(std::string_view s) {
    std::uint32_t v = 0;
    for (char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
    }
    return v;
}

}  // namespace

std::optional<std::uint64_t> Scalar::u64() const {
    if (kind != Kind::Number || number < 0 || number > 1.8e19) return std::nullopt;
    return static_cast<std::uint64_t>(number);
}

std::string Scalar::text() const {
    if (kind != Kind::String) return {};
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size()) {
            out += c;
            continue;
        }
        const char e = raw[++i];
        switch (e) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 >= raw.size()) {
                    i = raw.size();
                    break;
                }
                std::uint32_t cp = hex4(raw.substr(i + 1, 4));
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < raw.size() && raw.substr(i + 1, 2) == "\\u") {
                    const std::uint32_t lo = hex4(raw.substr(i + 3, 4));
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                put_utf8(out, cp);
                break;
            }
            default: out += e; break;
        }
    }
    return out;
}

bool scan(std::string_view doc, std::span<const std::string_view> wanted,
          const std::function<void(std::string_view, const Scalar&)>& on) {
    Scanner s(doc, wanted, &on);
    return s.document();
}

std::optional<TopLevel> top_level(std::string_view doc) {
    Scanner s(doc, {}, nullptr);
    return s.members();
}

}  // namespace kt::agents::jscan
