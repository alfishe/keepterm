#include "session/history_export.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>

#include "platform/paths.h"
#include "store/catalog.h"
#include "store/session_store.h"

namespace kt::session {

namespace fs = std::filesystem;

namespace {

// The length of the UTF-8 sequence at s (n bytes left): 0 when invalid; `incomplete` when the
// bytes end inside a sequence that is valid so far.
std::size_t utf8_len(const unsigned char* s, std::size_t n, bool& incomplete) {
    incomplete = false;
    const unsigned char c = s[0];
    if (c < 0x80) return 1;
    std::size_t len = 0;
    unsigned char lo = 0x80, hi = 0xBF;  // the second byte's range
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c == 0xE0) len = 3, lo = 0xA0;
    else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) len = 3;
    else if (c == 0xED) len = 3, hi = 0x9F;
    else if (c == 0xF0) len = 4, lo = 0x90;
    else if (c >= 0xF1 && c <= 0xF3) len = 4;
    else if (c == 0xF4) len = 4, hi = 0x8F;
    else return 0;
    for (std::size_t i = 1; i < len; ++i) {
        if (i >= n) {
            incomplete = true;
            return 0;
        }
        const unsigned char b = s[i];
        if (i == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xBF)) return 0;
    }
    return len;
}

Result<fs::path> history_root(const os::Paths& p, const std::string& id) {
    if (id.empty() || id.size() > 64 || id.find_first_of("/\\.:") != std::string::npos)
        return make_error(Errc::invalid_argument, "not a session history id: " + id);
    const fs::path root = platform::paths::from_utf8(os::join(p.state, "history"));
    if (std::error_code ec; !fs::exists(root / store::kCatalogFile, ec)) return make_error(Errc::unavailable, "nothing is stored");
    return root;
}

std::string seconds(uint64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%llu.%03u", static_cast<unsigned long long>(ms / 1000), static_cast<unsigned>(ms % 1000));
    return buf;
}

struct Event {
    uint64_t seq = 0, ts = 0;
    TermSize size;
};

}  // namespace

void append_json_string(std::string& out, std::string_view bytes, std::string* carry) {
    std::string joined;
    if (carry && !carry->empty()) {
        joined = std::move(*carry) + std::string(bytes);
        carry->clear();
        bytes = joined;
    }
    const auto* s = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    out += '"';
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
            ++i;
            continue;
        }
        bool incomplete = false;
        const std::size_t len = utf8_len(s + i, n - i, incomplete);
        if (len) {
            out.append(reinterpret_cast<const char*>(s + i), len);
            i += len;
        } else if (incomplete && carry) {
            carry->assign(reinterpret_cast<const char*>(s + i), n - i);
            break;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    out += '"';
}

std::optional<ExportFormat> export_format_from_string(std::string_view s) {
    if (s == "text") return ExportFormat::text;
    if (s == "raw") return ExportFormat::raw;
    if (s == "asciicast" || s == "cast") return ExportFormat::asciicast;
    return std::nullopt;
}

std::string_view to_string(ExportFormat f) {
    switch (f) {
        case ExportFormat::text: return "text";
        case ExportFormat::raw: return "raw";
        case ExportFormat::asciicast: return "asciicast";
    }
    return "text";
}

std::string_view export_extension(ExportFormat f) {
    switch (f) {
        case ExportFormat::text: return ".txt";
        case ExportFormat::raw: return ".log";
        case ExportFormat::asciicast: return ".cast";
    }
    return ".txt";
}

std::string export_file_name(std::string_view name, uint64_t created_ms, ExportFormat f) {
    std::string out;
    for (std::size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i]);
        bool incomplete = false;
        const std::size_t len = utf8_len(reinterpret_cast<const unsigned char*>(name.data()) + i, name.size() - i, incomplete);
        if (!len || c < 0x20 || c == 0x7F || std::string_view("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos) {
            if (!out.empty() && out.back() != '-') out += '-';
            i += len ? len : 1;
            continue;
        }
        if (out.size() + len > 80) break;
        if (c != ' ' || !out.empty()) out.append(name.data() + i, len);
        i += len;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == '.') out.erase(0, 1);
    if (out.empty()) out = "session";
    if (created_ms) {
        const auto t = static_cast<std::time_t>(created_ms / 1000);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[24];
        std::strftime(buf, sizeof buf, "-%Y-%m-%d-%H%M", &tm);
        out += buf;
    }
    out += export_extension(f);
    return out;
}

Result<ExportStats> export_history(const os::Paths& p, const std::string& id, ExportFormat f, const ExportInfo& info,
                                   const ExportSink& sink) {
    auto root = history_root(p, id);
    if (!root) return Unexpected(root.error());
    auto cat = store::Catalog::open(*root / store::kCatalogFile);
    if (!cat) return Unexpected(cat.error());
    auto row = (*cat)->session(id);
    if (!row) return Unexpected(row.error());
    if (!*row) return make_error(Errc::unavailable, "no output is stored for " + id);
    ExportStats st;
    bool sink_failed = false;
    std::string buf;
    auto put = [&](bool force) {
        if (buf.empty() || (!force && buf.size() < (256u << 10))) return true;
        if (!sink(buf)) {
            sink_failed = true;
            return false;
        }
        st.bytes += buf.size();
        buf.clear();
        return true;
    };

    if (f == ExportFormat::text || f == ExportFormat::raw) {
        const auto kind = f == ExportFormat::text ? store::Kind::text : store::Kind::raw;
        auto r = store::for_each_stored_record(**cat, *root, id, kind, 0, [&](const store::RecordView& v) {
            buf += v.payload;
            if (f == ExportFormat::text) buf += '\n';
            ++st.records;
            return put(false);
        });
        if (!r) return Unexpected(r.error());
        if (!sink_failed) put(true);
        if (sink_failed) return make_error(Errc::os_error, "could not write the export");
        return st;
    }

    // asciicast v2: the sizes first (few), then the raw records with the resizes between them.
    std::vector<Event> events;
    auto er = store::for_each_stored_record(**cat, *root, id, store::Kind::events, 0, [&](const store::RecordView& v) {
        if (auto sz = parse_resize_event(v.payload)) events.push_back({v.seq, v.ts, *sz});
        return true;
    });
    if (!er) return Unexpected(er.error());
    std::size_t next = 0;
    bool header = false;
    uint64_t t0 = 0, last_ms = 0;
    TermSize cur{0, 0};
    std::string carry;
    auto write_header = [&](uint64_t first_seq, uint64_t first_ts) {
        // The size the output starts at: the last event at or before its first byte, else the first.
        while (next < events.size() && events[next].seq <= first_seq) cur = events[next++].size;
        if (!cur.cols && next < events.size()) cur = events[next++].size;
        st.sized = cur.cols != 0;
        if (cur.cols) st.size = cur;
        cur = st.size;
        t0 = first_ts;
        buf += "{\"version\":2,\"width\":" + std::to_string(st.size.cols) + ",\"height\":" + std::to_string(st.size.rows);
        const uint64_t stamp = info.created_ms ? info.created_ms : first_ts;
        if (stamp) buf += ",\"timestamp\":" + std::to_string(stamp / 1000);
        if (!info.title.empty()) {
            buf += ",\"title\":";
            append_json_string(buf, info.title, nullptr);
        }
        buf += ",\"env\":{\"TERM\":\"xterm-256color\"}}\n";
        header = true;
    };
    auto resize = [&](const Event& e) {
        if (e.size == cur) return;
        cur = e.size;
        last_ms = std::max(last_ms, e.ts >= t0 ? e.ts - t0 : 0);
        buf += "[" + seconds(last_ms) + ",\"r\",\"" + std::to_string(e.size.cols) + "x" + std::to_string(e.size.rows) + "\"]\n";
        ++st.resizes;
    };
    auto r = store::for_each_stored_record(**cat, *root, id, store::Kind::raw, 0, [&](const store::RecordView& v) {
        if (!header) write_header(v.seq, v.ts);
        while (next < events.size() && events[next].seq <= v.seq) resize(events[next++]);
        last_ms = std::max(last_ms, v.ts >= t0 ? v.ts - t0 : 0);
        buf += "[" + seconds(last_ms) + ",\"o\",";
        append_json_string(buf, v.payload, &carry);
        buf += "]\n";
        ++st.records;
        return put(false);
    });
    if (!r) return Unexpected(r.error());
    if (!sink_failed) {
        if (!header) write_header(0, info.created_ms);
        while (next < events.size()) resize(events[next++]);
        if (!carry.empty()) {
            buf += "[" + seconds(last_ms) + ",\"o\",";
            append_json_string(buf, carry, nullptr);
            buf += "]\n";
        }
        put(true);
    }
    if (sink_failed) return make_error(Errc::os_error, "could not write the export");
    return st;
}

Result<ExportStats> export_history_to_file(const os::Paths& p, const std::string& id, ExportFormat f,
                                           const ExportInfo& info, const std::string& path) {
    const fs::path dst = platform::paths::from_utf8(path);
    fs::path tmp = dst;
    tmp += ".part";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return make_error(Errc::os_error, "cannot write " + path);
    auto st = export_history(p, id, f, info, [&](std::string_view s) {
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
        return static_cast<bool>(out);
    });
    out.close();
    std::error_code ec;
    if (st && !out) st = make_error(Errc::os_error, "cannot write " + path);
    if (!st) {
        fs::remove(tmp, ec);
        return st;
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return make_error(Errc::os_error, "cannot write " + path + ": " + ec.message());
    }
    return st;
}

}  // namespace kt::session
