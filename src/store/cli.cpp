#include "store/cli.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "config/config.h"
#include "platform/paths.h"
#include "session/json.h"
#include "store/catalog.h"
#include "store/projection.h"
#include "store/retention.h"
#include "store/search.h"
#include "store/usage.h"

namespace kt::store {

namespace fs = std::filesystem;

namespace {

int fail(const std::string& msg, int code = 1) {
    std::fprintf(stderr, "keepterm: %s\n", msg.c_str());
    return code;
}

int usage() {
    std::fprintf(stderr,
                 "usage: keepterm --history du [--json]\n"
                 "       keepterm --history search <query> [--regex] [--case] [--since T] [--until T]\n"
                 "                [--session ID]... [--project P] [--kind K]... [--limit N] [--json] [--explain]\n"
                 "       keepterm --history forget <session>\n"
                 "       keepterm --history pin <session> | unpin <session>\n"
                 "       keepterm --history backfill <session> [--journal FILE] [--project P] [--no-raw]\n"
                 "       keepterm --history janitor\n"
                 "common: --root DIR (default <state>/history)\n"
                 "T: 90m, 2h, 7d, 2w back from now; YYYY-MM-DD[THH:MM[:SS]] local time; Unix ms\n");
    return 2;
}

fs::path state_dir() {
    if (auto e = platform::paths::env_utf8("KEEPTERM_STATE_DIR"); e && !e->empty()) return platform::paths::from_utf8(*e);
    auto p = platform::paths::get(platform::paths::Kind::state);
    return p ? *p : fs::path();
}

std::string json_str(std::string_view s) {
    std::string o = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (u < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", u);
                    o += b;
                } else {
                    o.push_back(c);
                }
        }
    }
    return o + "\"";
}

std::string human(uint64_t b) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string local_time(uint64_t ms) {
    const auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

struct Common {
    fs::path root;
    bool json = false;
    std::vector<std::string> rest;
};

Result<std::unique_ptr<Catalog>> open_catalog(const fs::path& root) {
    if (auto r = platform::paths::make_private_dir(root); !r) return Unexpected(r.error());
    return Catalog::open(root / kCatalogFile);
}

std::string pct(double p) {
    char b[16];
    std::snprintf(b, sizeof b, "%.1f%%", p);
    return b;
}

std::string json_limit(const LimitUsage& l) {
    char b[160];
    std::snprintf(b, sizeof b, "{\"used\":%llu,\"limit\":%llu,\"remaining\":%llu,\"percent\":%.2f}",
                  static_cast<unsigned long long>(l.used), static_cast<unsigned long long>(l.limit),
                  static_cast<unsigned long long>(l.remaining), l.percent);
    return b;
}

std::string json_eviction(const Eviction& e) {
    return "{\"session\":" + json_str(e.session) + ",\"segment\":" + std::to_string(e.segment_id) + ",\"kind\":" +
           json_str(to_string(e.kind)) + ",\"bytes\":" + std::to_string(e.bytes) + ",\"at_ms\":" +
           std::to_string(e.at_ms) + ",\"reason\":" + json_str(e.reason) + "}";
}

std::string json_kinds(const std::array<uint64_t, kKindCount>& k, uint64_t index) {
    std::string o = "{";
    for (std::size_t i = 0; i < kKindCount; ++i) {
        o += json_str(to_string(static_cast<Kind>(i))) + ":" + std::to_string(k[i]) + ",";
    }
    return o + "\"index\":" + std::to_string(index) + "}";
}

std::string when(uint64_t at, uint64_t now) {
    if (at <= now) return "now (next janitor pass)";
    const uint64_t d = at - now;
    char b[48];
    if (d < kHourMs) std::snprintf(b, sizeof b, "in %llu min", static_cast<unsigned long long>(d / 60'000));
    else if (d < 2 * kDayMs) std::snprintf(b, sizeof b, "in %.1f h", static_cast<double>(d) / kHourMs);
    else std::snprintf(b, sizeof b, "in %.1f days", static_cast<double>(d) / kDayMs);
    return std::string(b) + " (" + local_time(at) + ")";
}

int cmd_du(Common& c) {
    auto cat = open_catalog(c.root);
    if (!cat) return fail(cat.error().what);
    const Policy pol = policy_from_config(config::Config::load({}));
    auto u = usage(**cat, c.root, pol, now_unix_ms());
    if (!u) return fail(u.error().what);
    if (c.json) {
        std::string o = "{\"root\":" + json_str(platform::paths::to_utf8(c.root)) + ",\"now_ms\":" + std::to_string(u->now_ms);
        o += ",\"limits\":{\"max_total\":" + json_limit(u->total);
        o += ",\"min_free_disk\":{\"capacity\":" + std::to_string(u->disk.capacity) + ",\"available\":" +
             std::to_string(u->disk.available) + ",\"threshold\":" + std::to_string(u->disk.threshold) +
             ",\"writes_stopped\":" + (u->disk.below ? "true" : "false") + ",\"fill\":" + json_limit(u->disk.fill) + "}";
        o += ",\"max_per_session\":" + std::to_string(pol.max_per_session) + "}";
        o += ",\"bytes\":" + json_kinds(u->kind_bytes, u->index_bytes);
        o += ",\"catalog\":" + std::to_string(u->catalog_bytes) + ",\"pinned\":" + std::to_string(u->pinned_bytes);
        o += ",\"payload\":{\"raw\":" + std::to_string(u->raw_payload_bytes) + ",\"text\":" +
             std::to_string(u->text_payload_bytes) + "}";
        o += ",\"next_expiry\":" + (u->next_expiry ? json_eviction(*u->next_expiry) : std::string("null"));
        o += ",\"over_cap\":[";
        for (std::size_t i = 0; i < u->over_cap.size(); ++i) o += (i ? "," : "") + json_eviction(u->over_cap[i]);
        o += "],\"sessions\":[";
        bool first = true;
        for (const auto& s : u->sessions) {
            o += std::string(first ? "" : ",") + "{\"id\":" + json_str(s.id) + ",\"project\":" + json_str(s.project) +
                 ",\"live\":" + (s.live ? "true" : "false") + ",\"pinned\":" + (s.pinned ? "true" : "false") +
                 ",\"segments\":" + std::to_string(s.segments) + ",\"max_per_session\":" + json_limit(s.cap) +
                 ",\"bytes\":" + json_kinds(s.kind_bytes, s.index_bytes) + "}";
            first = false;
        }
        o += "]}";
        std::printf("%s\n", o.c_str());
        return 0;
    }
    std::printf("store  %s\n\n", platform::paths::to_utf8(c.root).c_str());
    std::printf("%-26s %12s %12s %12s %8s\n", "limit", "used", "limit", "remaining", "percent");
    auto row = [](const std::string& name, const LimitUsage& l) {
        std::printf("%-26s %12s %12s %12s %8s\n", name.c_str(), human(l.used).c_str(), human(l.limit).c_str(),
                    human(l.remaining).c_str(), pct(l.percent).c_str());
    };
    row("max_total", u->total);
    const SessionUsage* biggest = nullptr;
    for (const auto& s : u->sessions) {
        if (!biggest || s.cap.percent > biggest->cap.percent) biggest = &s;
    }
    if (biggest) row("max_per_session (fullest)", biggest->cap);
    if (u->disk.capacity) {
        row("disk until min_free_disk", u->disk.fill);
        std::printf("  volume: %s free of %s, writes stop below %s free%s\n", human(u->disk.available).c_str(),
                    human(u->disk.capacity).c_str(), human(u->disk.threshold).c_str(),
                    u->disk.below ? "  -- WRITES STOPPED" : "");
    }
    std::printf("\n%-10s %12s\n", "part", "on disk");
    for (std::size_t i = 0; i < kKindCount; ++i) {
        std::printf("%-10s %12s\n", std::string(to_string(static_cast<Kind>(i))).c_str(), human(u->kind_bytes[i]).c_str());
    }
    std::printf("%-10s %12s\n%-10s %12s\n%-10s %12s (counted in the parts above)\n", "index", human(u->index_bytes).c_str(),
                "catalog", human(u->catalog_bytes).c_str(), "pinned", human(u->pinned_bytes).c_str());
    if (u->raw_payload_bytes || u->text_payload_bytes) {
        std::printf("uncompressed: raw %s, text %s\n", human(u->raw_payload_bytes).c_str(),
                    human(u->text_payload_bytes).c_str());
    }
    std::printf("\n");
    if (u->next_expiry) {
        const auto& e = *u->next_expiry;
        std::printf("next expiry: %s of %s (%s, %s) %s\n", e.segment_id ? std::string(to_string(e.kind)).c_str() : "all",
                    e.session.c_str(), e.reason.c_str(), human(e.bytes).c_str(), when(e.at_ms, u->now_ms).c_str());
    } else {
        std::printf("next expiry: none\n");
    }
    for (const auto& e : u->over_cap) {
        std::printf("over cap:    %s segment %lld of %s (%s, %s) now\n", std::string(to_string(e.kind)).c_str(),
                    static_cast<long long>(e.segment_id), e.session.c_str(), e.reason.c_str(), human(e.bytes).c_str());
    }
    std::printf("\n%-16s %-12s %-6s %10s %10s %10s %7s %10s %10s %10s\n", "session", "project", "state", "used", "limit",
                "remaining", "percent", "raw", "text", "index");
    for (const auto& s : u->sessions) {
        std::printf("%-16s %-12s %-6s %10s %10s %10s %7s %10s %10s %10s%s\n", s.id.c_str(), s.project.c_str(),
                    s.live ? "live" : "closed", human(s.cap.used).c_str(), human(s.cap.limit).c_str(),
                    human(s.cap.remaining).c_str(), pct(s.cap.percent).c_str(),
                    human(s.kind_bytes[static_cast<std::size_t>(Kind::raw)]).c_str(),
                    human(s.kind_bytes[static_cast<std::size_t>(Kind::text)]).c_str(), human(s.index_bytes).c_str(),
                    s.pinned ? "  pinned" : "");
    }
    return 0;
}

int cmd_search(Common& c) {
    SearchQuery q;
    bool explain_only = false;
    std::vector<Kind> kinds;
    const uint64_t now = now_unix_ms();
    for (std::size_t i = 0; i < c.rest.size(); ++i) {
        const std::string& a = c.rest[i];
        auto value = [&]() -> const std::string* { return i + 1 < c.rest.size() ? &c.rest[++i] : nullptr; };
        if (a == "--regex") {
            q.regex = true;
        } else if (a == "--case") {
            q.case_sensitive = true;
        } else if (a == "--explain") {
            explain_only = true;
        } else if (a == "--since" || a == "--until") {
            const auto* v = value();
            const auto t = v ? parse_time_arg(*v, now) : std::nullopt;
            if (!t) return fail("bad time for " + a);
            (a == "--since" ? q.since_ms : q.until_ms) = *t;
        } else if (a == "--session") {
            const auto* v = value();
            if (!v) return usage();
            q.sessions.push_back(*v);
        } else if (a == "--project") {
            const auto* v = value();
            if (!v) return usage();
            q.project = *v;
        } else if (a == "--kind") {
            const auto* v = value();
            const auto k = v ? kind_from_string(*v) : std::nullopt;
            if (!k) return fail("bad kind (raw, text, commands, events)");
            kinds.push_back(*k);
        } else if (a == "--limit") {
            const auto* v = value();
            if (!v) return usage();
            q.limit = static_cast<std::size_t>(std::strtoull(v->c_str(), nullptr, 10));
            if (q.limit == 0) return fail("bad --limit");
        } else if (a.starts_with("--")) {
            return usage();
        } else if (q.pattern.empty()) {
            q.pattern = a;
        } else {
            return usage();
        }
    }
    if (q.pattern.empty()) return usage();
    if (!kinds.empty()) q.kinds = kinds;
    if (explain_only) {
        auto e = explain(q);
        if (!e) return fail(e.error().what);
        std::printf("%s\n", e->c_str());
        return 0;
    }
    auto cat = open_catalog(c.root);
    if (!cat) return fail(cat.error().what);
    auto st = search(**cat, c.root, q, [&](const Hit& h) {
        if (c.json) {
            std::printf("{\"session\":%s,\"project\":%s,\"cwd\":%s,\"kind\":\"%s\",\"ts_ms\":%llu,\"seq\":%llu,"
                        "\"file\":%s,\"block\":%u,\"record\":%u,\"match\":[%zu,%zu],\"text\":%s}\n",
                        json_str(h.session).c_str(), json_str(h.project).c_str(), json_str(h.cwd).c_str(),
                        std::string(to_string(h.kind)).c_str(), static_cast<unsigned long long>(h.ts_ms),
                        static_cast<unsigned long long>(h.seq), json_str(h.file).c_str(), h.block, h.record,
                        h.match_offset, h.match_len, json_str(h.text).c_str());
        } else {
            std::printf("%s  %.8s  %s\n", local_time(h.ts_ms).c_str(), h.session.c_str(), h.text.c_str());
        }
        return true;
    });
    if (!st) return fail(st.error().what);
    std::fprintf(stderr,
                 "%llu hits%s, %u segments (%u scanned in full), %llu/%llu blocks read, first %.1f ms, total %.1f ms\n",
                 static_cast<unsigned long long>(st->hits), st->limit_reached ? " (limit)" : "", st->segments,
                 st->segments_full_scan, static_cast<unsigned long long>(st->blocks_read),
                 static_cast<unsigned long long>(st->blocks_total), st->first_hit_ms, st->total_ms);
    if (st->blocks_corrupt) {
        std::fprintf(stderr, "warning: %llu damaged blocks skipped\n", static_cast<unsigned long long>(st->blocks_corrupt));
    }
    return st->hits ? 0 : 1;
}

int cmd_forget(Common& c) {
    if (c.rest.size() != 1) return usage();
    auto cat = open_catalog(c.root);
    if (!cat) return fail(cat.error().what);
    auto s = (*cat)->session(c.rest[0]);
    if (!s) return fail(s.error().what);
    if (!*s) return fail("no such session in the store: " + c.rest[0]);
    auto r = forget_session(**cat, c.root, c.rest[0]);
    if (!r) return fail(r.error().what);
    std::printf("forgot %s (%s)\n", c.rest[0].c_str(), human(*r).c_str());
    return 0;
}

int cmd_pin(Common& c, bool pin) {
    if (c.rest.size() != 1) return usage();
    auto cat = open_catalog(c.root);
    if (!cat) return fail(cat.error().what);
    if (auto r = (*cat)->set_pinned(c.rest[0], pin); !r) return fail(r.error().what);
    return 0;
}

int cmd_backfill(Common& c) {
    std::string id, project;
    fs::path journal;
    BackfillOptions bo;
    for (std::size_t i = 0; i < c.rest.size(); ++i) {
        const std::string& a = c.rest[i];
        if (a == "--journal" && i + 1 < c.rest.size()) {
            journal = platform::paths::from_utf8(c.rest[++i]);
        } else if (a == "--project" && i + 1 < c.rest.size()) {
            project = c.rest[++i];
        } else if (a == "--no-raw") {
            bo.raw = false;
        } else if (!a.starts_with("--") && id.empty()) {
            id = a;
        } else {
            return usage();
        }
    }
    if (id.empty()) return usage();
    const fs::path sdir = state_dir() / "sessions" / platform::paths::from_utf8(id);
    if (journal.empty()) journal = sdir / "journal.ktj";
    std::ifstream in(journal, std::ios::binary);
    if (!in) return fail("cannot read " + platform::paths::to_utf8(journal));
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();

    SessionRow row;
    row.id = id;
    row.project = project;
    row.started_ms = now_unix_ms();
    std::ifstream rec(sdir / "record.json", std::ios::binary);
    if (rec) {
        std::stringstream rs;
        rs << rec.rdbuf();
        if (auto v = session::json::parse(rs.str())) {
            if (!v->bool_or("history", true)) return fail("the session has history off; nothing is stored");
            row.cwd = v->str_or("cwd");
            row.shell = v->str_or("shell");
            if (const auto t = v->uint_or("created_unix_ms", 0)) row.started_ms = t;
        }
    }
    auto cat = open_catalog(c.root);
    if (!cat) return fail(cat.error().what);
    auto store = SessionStore::open(c.root, **cat, row);
    if (!store) return fail(store.error().what);
    auto st = backfill_journal(std::as_bytes(std::span(bytes.data(), bytes.size())), **store, bo);
    if (!st) return fail(st.error().what);
    if (auto r = (*store)->close(now_unix_ms()); !r) return fail(r.error().what);
    std::printf("backfilled %s: %llu records, %s output, %llu lines%s\n", id.c_str(),
                static_cast<unsigned long long>(st->records), human(st->output_bytes).c_str(),
                static_cast<unsigned long long>(st->lines), st->gap ? " (the ring had lost older output)" : "");
    return 0;
}

int cmd_janitor(Common& c) {
    if (auto r = platform::paths::make_private_dir(c.root); !r) return fail(r.error().what);
    auto lock = LockFile::try_acquire(c.root / kJanitorLock);
    if (!lock) return fail(lock.error().what);
    if (!*lock) return fail("another process runs the janitor", 3);
    auto cat = open_catalog(c.root);
    if (!cat) return fail(cat.error().what);
    Janitor j(**cat, c.root, policy_from_config(config::Config::load({})));
    auto rep = j.run_pass(now_unix_ms());
    if (!rep) return fail(rep.error().what);
    std::printf("orphans %u files, %u rows, %u temp; expired %u; closed sessions %u; caps %u+%u; merged %u->%u; "
                "repacked %u%s; trimmed %u; freed %s; total %s%s\n",
                rep->orphan_files, rep->orphan_rows, rep->temp_files, rep->expired, rep->closed_sessions,
                rep->over_session_cap, rep->over_total_cap, rep->merged_inputs, rep->merged_outputs, rep->recompressed,
                rep->repack_pending ? " (more pending)" : "", rep->trimmed, human(rep->freed_bytes).c_str(), human(rep->total_bytes).c_str(),
                rep->pins_over_cap ? "; pinned data exceeds storage.max_total" : "");
    return 0;
}

}  // namespace

std::optional<uint64_t> parse_time_arg(std::string_view s, uint64_t now_ms) {
    if (s.empty()) return std::nullopt;
    bool all_digits = true;
    for (const char ch : s) all_digits = all_digits && ch >= '0' && ch <= '9';
    if (all_digits) {
        const uint64_t v = std::strtoull(std::string(s).c_str(), nullptr, 10);
        return v;
    }
    // A duration back from now.
    {
        std::size_t i = 0;
        uint64_t n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') n = n * 10 + static_cast<uint64_t>(s[i++] - '0');
        if (i > 0 && i + 1 == s.size()) {
            uint64_t mul = 0;
            switch (s[i]) {
                case 's': mul = 1000; break;
                case 'm': mul = 60'000; break;
                case 'h': mul = kHourMs; break;
                case 'd': mul = kDayMs; break;
                case 'w': mul = 7 * kDayMs; break;
                default: return std::nullopt;
            }
            const uint64_t d = n * mul;
            return d > now_ms ? 0 : now_ms - d;
        }
    }
    // YYYY-MM-DD[THH:MM[:SS]] in local time.
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    std::size_t at = 0;
    // A field of exactly `digits` digits, then an optional separator from `seps`.
    auto field = [&](int digits, int& out) {
        if (at + static_cast<std::size_t>(digits) > s.size()) return false;
        out = 0;
        for (int i = 0; i < digits; ++i) {
            const char ch = s[at++];
            if (ch < '0' || ch > '9') return false;
            out = out * 10 + (ch - '0');
        }
        return true;
    };
    auto sep = [&](std::string_view seps) {
        if (at < s.size() && seps.find(s[at]) != std::string_view::npos) {
            ++at;
            return true;
        }
        return false;
    };
    if (!field(4, y) || !sep("-") || !field(2, mo) || !sep("-") || !field(2, d)) return std::nullopt;
    if (at < s.size()) {
        if (!sep("T ") || !field(2, h) || !sep(":") || !field(2, mi)) return std::nullopt;
        if (at < s.size() && (!sep(":") || !field(2, sec))) return std::nullopt;
        if (at != s.size()) return std::nullopt;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return std::nullopt;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t < 0) return std::nullopt;
    return static_cast<uint64_t>(t) * 1000;
}

int history_cli(int argc, char** argv, int first) {
    if (first >= argc) return usage();
    const std::string_view cmd = argv[first];
    Common c;
    for (int i = first + 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--root" && i + 1 < argc) {
            c.root = platform::paths::from_utf8(argv[++i]);
        } else if (a == "--json") {
            c.json = true;
        } else {
            c.rest.emplace_back(a);
        }
    }
    if (c.root.empty()) {
        const fs::path s = state_dir();
        if (s.empty()) return fail("no state folder");
        c.root = s / "history";
    }
    if (cmd == "du") return c.rest.empty() ? cmd_du(c) : usage();
    if (cmd == "search") return cmd_search(c);
    if (cmd == "forget") return cmd_forget(c);
    if (cmd == "pin") return cmd_pin(c, true);
    if (cmd == "unpin") return cmd_pin(c, false);
    if (cmd == "backfill") return cmd_backfill(c);
    if (cmd == "janitor") return c.rest.empty() ? cmd_janitor(c) : usage();
    return usage();
}

}  // namespace kt::store
