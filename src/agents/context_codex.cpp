// Codex CLI context probe.
//
// The rollout $CODEX_HOME/sessions/YYYY/MM/DD/rollout-<time>-<thread id>.jsonl carries
// event_msg/token_count records: info.last_token_usage (the context, input incl. cached plus
// output, as Codex counts it), info.total_token_usage (cumulative, so exact even from a tail),
// info.model_context_window (already the usable window) and rate_limits. The percent is Codex's
// own "context left" formula, so the pane matches Codex's UI.
//
// The file of a process: `codex resume <id>` from its arguments, else the newest thread of the
// state database with the pane's cwd updated since the process started, else the newest rollout
// of the two newest day folders whose session_meta names the same cwd and started after it.
#include <algorithm>
#include <array>

#include "agents/context.h"
#include "agents/context_internal.h"

namespace kt::agents {
namespace {

using detail::base_name;
using detail::parent_dir;
using jscan::Scalar;

constexpr std::array<std::string_view, 30> kWanted{
    "timestamp", "type", "payload.type", "payload.id", "payload.cwd", "payload.cli_version", "payload.model",
    "payload.timestamp",
    "payload.info.last_token_usage.input_tokens", "payload.info.last_token_usage.cached_input_tokens",
    "payload.info.last_token_usage.output_tokens", "payload.info.last_token_usage.total_tokens",
    "payload.info.total_token_usage.input_tokens", "payload.info.total_token_usage.cached_input_tokens",
    "payload.info.total_token_usage.cache_write_input_tokens", "payload.info.total_token_usage.output_tokens",
    "payload.info.total_token_usage.total_tokens", "payload.info.model_context_window",
    "payload.rate_limits.primary.used_percent", "payload.rate_limits.primary.window_minutes",
    "payload.rate_limits.primary.resets_at", "payload.rate_limits.primary.resets_in_seconds",
    "payload.rate_limits.secondary.used_percent", "payload.rate_limits.secondary.window_minutes",
    "payload.rate_limits.secondary.resets_at", "payload.rate_limits.secondary.resets_in_seconds",
    "payload.info", "payload.rate_limits", "payload.rate_limits.primary", "payload.rate_limits.secondary"};

struct Limit {
    std::optional<double> used;
    std::uint32_t window = 0;
    std::optional<std::int64_t> resets_at, resets_in;
};

class CodexReducer final : public detail::LineReducer {
public:
    CodexReducer() {
        u_.agent = "codex";
        u_.used_source = Source::Data;
    }

private:
    void clear_state() override {
        unknown_ = 0;
        u_.used_source = Source::Data;
    }

    void apply_line(std::string_view line) override {
        if (line.find("\"event_msg\"") == std::string_view::npos && line.find("\"session_meta\"") == std::string_view::npos &&
            line.find("\"turn_context\"") == std::string_view::npos && line.find("\"compacted\"") == std::string_view::npos) {
            return;
        }
        std::string type, ptype, version, model;
        std::optional<std::int64_t> ts;
        std::optional<std::uint64_t> l_total, l_in, t_in, t_cached, t_write, t_out, window;
        bool info_seen = false, info_null = false, limits_seen = false;
        Limit prim, sec;
        const bool ok = jscan::scan(line, kWanted, [&](std::string_view p, const Scalar& v) {
            if (p == "timestamp") ts = parse_iso8601_ms(v.raw);
            else if (p == "type") type = v.text();
            else if (p == "payload.type") ptype = v.raw.size() <= 64 ? v.text() : std::string();
            else if (p == "payload.cli_version") version = v.raw.size() <= 64 ? v.text() : std::string();
            else if (p == "payload.model") model = v.raw.size() <= 128 ? v.text() : std::string();
            else if (p == "payload.info") info_null = v.kind == Scalar::Kind::Null;
            else if (p == "payload.info.last_token_usage.total_tokens") info_seen = true, l_total = v.u64();
            else if (p == "payload.info.last_token_usage.input_tokens") info_seen = true, l_in = v.u64();
            else if (p == "payload.info.total_token_usage.input_tokens") info_seen = true, t_in = v.u64();
            else if (p == "payload.info.total_token_usage.cached_input_tokens") t_cached = v.u64();
            else if (p == "payload.info.total_token_usage.cache_write_input_tokens") t_write = v.u64();
            else if (p == "payload.info.total_token_usage.output_tokens") t_out = v.u64();
            else if (p == "payload.info.model_context_window") info_seen = true, window = v.u64();
            else if (p.starts_with("payload.rate_limits.")) {
                limits_seen = true;
                Limit& l = p.starts_with("payload.rate_limits.primary") ? prim : sec;
                if (p.ends_with(".used_percent") && v.kind == Scalar::Kind::Number) l.used = v.number;
                else if (p.ends_with(".window_minutes")) l.window = static_cast<std::uint32_t>(v.u64().value_or(0));
                else if (p.ends_with(".resets_at")) l.resets_at = static_cast<std::int64_t>(v.u64().value_or(0));
                else if (p.ends_with(".resets_in_seconds")) l.resets_in = static_cast<std::int64_t>(v.u64().value_or(0));
            }
        });
        if (!ok) return;
        if (ts) u_.updated_unix_ms = std::max(u_.updated_unix_ms, *ts);
        if (type == "session_meta") {
            if (!version.empty()) note_version(version, kCodexKnownMajor);
            return;
        }
        if (type == "turn_context") {
            if (!model.empty()) u_.model = model;
            return;
        }
        if (type == "compacted") {
            compacted();
            return;
        }
        if (type != "event_msg") return;
        if (ptype == "token_count") {
            if (info_seen) {
                const std::uint64_t used = l_total.value_or(l_in.value_or(0));
                u_.used_tokens = used;
                u_.after_compaction = false;
                if (window && *window) {
                    u_.window_tokens = window;
                    u_.window_source = Source::Data;
                    u_.percent = codex_percent_used(used, *window);
                }
                if (t_in) {
                    // total_token_usage is cumulative: the totals are exact even from a tail.
                    u_.input_tokens = *t_in;
                    u_.cache_read_tokens = t_cached.value_or(0);
                    u_.cache_write_tokens = t_write.value_or(0);
                    u_.output_tokens = t_out.value_or(0);
                    u_.totals_since_attach = false;
                }
                ++u_.api_calls;
                saw_usage_ = true;
                if (u_.format == FormatStatus::NoData) u_.format = FormatStatus::Ok;
            } else if (!info_null && !limits_seen) {
                if (++unknown_ >= 4 && !saw_usage_) u_.format = FormatStatus::UnknownVersion;
            }
            if (limits_seen) apply_limits(prim, sec, ts);
        } else if (ptype == "task_started") {
            u_.activity = Activity::Busy;
        } else if (ptype == "task_complete") {
            ++u_.turns;
            u_.activity = Activity::Idle;
        } else if (ptype == "turn_aborted") {
            u_.activity = Activity::Idle;
        } else if (ptype.ends_with("_approval_request") || ptype == "request_user_input") {
            u_.activity = Activity::Waiting;
        } else if (ptype == "context_compacted") {
            compacted();
        } else if (u_.activity == Activity::Waiting) {
            u_.activity = Activity::Busy;
        }
    }

    void compacted() {
        ++u_.compactions;
        u_.used_tokens.reset();
        u_.percent.reset();
        u_.after_compaction = true;
    }

    void apply_limits(const Limit& p, const Limit& s, std::optional<std::int64_t> ts) {
        std::vector<RateLimit> out;
        auto add = [&](const char* name, const Limit& l) {
            if (!l.used) return;
            RateLimit r;
            r.name = name;
            r.used_percent = *l.used;
            r.window_minutes = l.window;
            if (l.resets_at && *l.resets_at) r.resets_at_unix = *l.resets_at;
            else if (l.resets_in && ts) r.resets_at_unix = *ts / 1000 + *l.resets_in;
            out.push_back(std::move(r));
        };
        add("primary", p);
        add("secondary", s);
        if (out.empty()) return;
        u_.limits = std::move(out);
        u_.limits_source = Source::Data;
        if (ts) u_.limits_unix_ms = *ts;   // the record's time: the response the limits came with
    }

    int unknown_ = 0;
};

// ---- Locating the rollout

std::vector<DirEntry> sorted_dirs(const IFileSystem& fs, const std::string& dir, bool dirs) {
    std::vector<DirEntry> out;
    if (auto e = fs.list_dir(dir)) {
        for (auto& x : *e) {
            if (x.is_dir == dirs) out.push_back(std::move(x));
        }
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.name > b.name; });
    return out;
}

// The day folders sessions/YYYY/MM/DD, newest first, at most `max`.
std::vector<std::string> day_dirs(const IFileSystem& fs, const std::string& sessions, std::size_t max) {
    std::vector<std::string> out;
    for (const auto& y : sorted_dirs(fs, sessions, true)) {
        const std::string yd = join_path(sessions, y.name);
        for (const auto& m : sorted_dirs(fs, yd, true)) {
            const std::string md = join_path(yd, m.name);
            for (const auto& d : sorted_dirs(fs, md, true)) {
                out.push_back(join_path(md, d.name));
                if (out.size() >= max) return out;
            }
        }
    }
    return out;
}

bool is_rollout(std::string_view name) { return name.starts_with("rollout-") && name.ends_with(".jsonl"); }

struct Meta {
    std::string cwd, id;
    std::optional<std::int64_t> started_ms;
};

// The session_meta of a rollout (its first line). Long lines (instructions) are read only up to
// the fields that come first; the scanner skips the rest.
std::optional<Meta> read_meta(const IFileSystem& fs, const std::string& path) {
    auto head = fs.read_range(path, 0, 256 * 1024);
    if (!head) return std::nullopt;
    std::string_view line(*head);
    if (auto nl = line.find('\n'); nl != std::string_view::npos) line = line.substr(0, nl);
    static constexpr std::array<std::string_view, 4> kMeta{"type", "payload.cwd", "payload.id", "payload.timestamp"};
    Meta m;
    std::string type;
    (void)jscan::scan(line, kMeta, [&](std::string_view p, const Scalar& v) {
        if (p == "type") type = v.text();
        else if (p == "payload.cwd" && v.raw.size() < 32 * 1024) m.cwd = v.text();
        else if (p == "payload.id" && v.raw.size() < 256) m.id = v.text();
        else if (p == "payload.timestamp") m.started_ms = parse_iso8601_ms(v.raw);
    });
    if (type != "session_meta") return std::nullopt;
    return m;
}

bool same_dir(std::string_view a, std::string_view b) {
    while (a.size() > 1 && (a.back() == '/' || a.back() == '\\')) a.remove_suffix(1);
    while (b.size() > 1 && (b.back() == '/' || b.back() == '\\')) b.remove_suffix(1);
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
#ifdef _WIN32
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
#endif
        if (x != y) return false;
    }
    return true;
}

// `codex resume <id>` / `codex resume --last`: the id, "--last" or empty.
std::string resume_selector(const std::vector<std::string>& argv) {
    for (std::size_t i = 1; i < argv.size(); ++i) {
        if (argv[i] != "resume") continue;
        for (std::size_t k = i + 1; k < argv.size(); ++k) {
            if (argv[k] == "--last") return "--last";
            if (!argv[k].starts_with("-")) return argv[k];
        }
        return "--last";
    }
    return {};
}

class CodexProbe final : public IContextProbe {
public:
    CodexProbe(const IFileSystem& fs, const ICodexIndex* index, std::string home)
        : fs_(fs), index_(index), home_(std::move(home)) {}

    std::string_view agent() const override { return "codex"; }

    std::optional<ContextTarget> locate(const ContextQuery& q) const override {
        const std::string cwd = q.process.cwd.empty() ? q.pane_cwd : q.process.cwd;
        const std::int64_t start_ms = q.process.start_time ? static_cast<std::int64_t>(q.process.start_time / 1'000'000) : 0;
        const std::int64_t tol_ms = kTimeToleranceNs / 1'000'000;
        const std::string sel = resume_selector(q.argv);
        // 1. An explicit thread id.
        if (!sel.empty() && sel != "--last") {
            if (index_) {
                for (const auto& t : index_->threads(sel, {})) {
                    if (exists(t.rollout_path)) return target(t.rollout_path, t.id);
                }
            }
            for (const auto& d : day_dirs(fs_, sessions(), 400)) {
                for (const auto& f : sorted_dirs(fs_, d, false)) {
                    if (is_rollout(f.name) && f.name.ends_with("-" + sel + ".jsonl")) return target(join_path(d, f.name), sel);
                }
            }
            return std::nullopt;
        }
        if (cwd.empty()) return std::nullopt;
        // 2. The state database: the newest thread of this cwd updated since the process started
        // (resume --last and a resumed thread update it too).
        if (index_) {
            for (const auto& t : index_->threads({}, cwd)) {
                if (t.archived) continue;
                if (start_ms && t.updated_at_ms + tol_ms < start_ms) break;   // newest first
                if (exists(t.rollout_path)) return target(t.rollout_path, t.id);
            }
        }
        // 3. The newest rollouts: session_meta with this cwd, started after the process. The
        // earliest such start is this process's own thread (a later codex starts later).
        std::optional<std::pair<std::int64_t, ContextTarget>> best;
        for (const auto& d : day_dirs(fs_, sessions(), 2)) {
            for (const auto& f : sorted_dirs(fs_, d, false)) {
                if (!is_rollout(f.name)) continue;
                if (start_ms && f.mtime_ns / 1'000'000 + tol_ms < start_ms) continue;
                const std::string path = join_path(d, f.name);
                auto m = read_meta(fs_, path);
                if (!m || !same_dir(m->cwd, cwd)) continue;
                const std::int64_t started = m->started_ms.value_or(f.mtime_ns / 1'000'000);
                if (start_ms && started + tol_ms < start_ms && sel != "--last") continue;
                const std::int64_t key = sel == "--last" ? -f.mtime_ns : started;
                if (!best || key < best->first) best = std::make_pair(key, target(path, m->id));
            }
        }
        if (best) return best->second;
        return std::nullopt;
    }

    std::unique_ptr<IContextReducer> reducer(const ContextTarget&) const override { return make_codex_reducer(); }

private:
    std::string sessions() const { return join_path(home_, "sessions"); }
    bool exists(const std::string& p) const { return !p.empty() && fs_.mtime_ns(p).has_value(); }
    static ContextTarget target(const std::string& path, const std::string& id) {
        ContextTarget t;
        t.path = path;
        t.session_id = id;
        if (t.session_id.empty()) {
            // rollout-<time>-<uuid>.jsonl: the last 36 characters before the extension.
            std::string_view n = base_name(path);
            if (n.size() > 6 + 36) t.session_id = std::string(n.substr(n.size() - 6 - 36, 36));
        }
        t.watch_dir = parent_dir(path);
        return t;
    }

    const IFileSystem& fs_;
    const ICodexIndex* index_;
    std::string home_;
};

class CodexSessionSource final : public ISessionSource {
public:
    CodexSessionSource(const IFileSystem& fs, const ICodexIndex* index, std::string home)
        : probe_(fs, index, std::move(home)) {}

    std::optional<AgentSessionInfo> session_of(const SessionQuery& q) const override {
        ContextQuery cq;
        cq.process = q;
        cq.argv = q.argv;
        cq.pane_cwd = q.cwd;
        auto t = probe_.locate(cq);
        if (!t || t->session_id.empty()) return std::nullopt;
        AgentSessionInfo info;
        info.session_id = t->session_id;
        return info;
    }
    bool session_is_running(std::string_view) const override { return false; }
    bool is_ready(const SessionQuery&) const override { return true; }

private:
    CodexProbe probe_;
};

}  // namespace

std::unique_ptr<IContextReducer> make_codex_reducer() { return std::make_unique<CodexReducer>(); }

std::unique_ptr<ISessionSource> make_codex_session_source(const IFileSystem& fs, const ICodexIndex* index,
                                                          std::string codex_home) {
    return std::make_unique<CodexSessionSource>(fs, index, std::move(codex_home));
}

std::unique_ptr<IContextProbe> make_codex_context_probe(const IFileSystem& fs, const ICodexIndex* index,
                                                        std::string codex_home) {
    return std::make_unique<CodexProbe>(fs, index, std::move(codex_home));
}

}  // namespace kt::agents
