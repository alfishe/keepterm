// Claude Code and Qoder CLI context probes.
//
// Claude: <config>/sessions/<pid>.json names the session; the transcript is
// <config>/projects/<slug(cwd)>/<sessionId>.jsonl. The context is the prompt size of the newest
// main-chain assistant record (input + cache read + cache creation); records of one API response
// share message.id and count once in the totals; <synthetic>, sidechain and API error records
// are skipped; after a compact_boundary the context is its postTokens until the next call.
//
// Qoder writes the same transcript shape under <home>/.qoder/projects/, with runtime-config
// records carrying the window. Its per call usage is written as zeros (1.1.x), so the fill is an
// estimate: the last compaction's postTokens plus the transcript bytes since, at 4 bytes a token.
#include <algorithm>
#include <array>

#include "agents/context.h"
#include "agents/context_internal.h"

namespace kt::agents {
namespace {

using detail::base_name;
using detail::parent_dir;
using jscan::Scalar;

bool looks_like_uuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

constexpr std::uint64_t k200k = 200'000;
constexpr std::uint64_t k1m = 1'000'000;

constexpr std::array<std::string_view, 19> kWanted{
    "type", "subtype", "isSidechain", "isApiErrorMessage", "version", "timestamp",
    "message.id", "message.model", "message.usage.input_tokens", "message.usage.cache_read_input_tokens",
    "message.usage.cache_creation_input_tokens", "message.usage.output_tokens", "compactMetadata.preTokens",
    "compactMetadata.postTokens", "contextWindow", "model", "totalCostUSD", "cliVersion", "message.role"};

struct Rec {
    std::string type, subtype, version, id, model, top_model;
    std::optional<std::int64_t> time_ms;
    bool sidechain = false, api_error = false;
    bool has_usage = false;
    std::uint64_t in = 0, cr = 0, cc = 0, out = 0;
    std::optional<std::uint64_t> pre, post, context_window;
    std::optional<double> cost;
};

class ClaudeReducer final : public detail::LineReducer {
public:
    explicit ClaudeReducer(ClaudeReducerOptions o) : o_(std::move(o)) {
        u_.agent = o_.qoder ? "qodercli" : "claude";
        clear_state();
    }

private:
    void clear_state() override {
        ids_.clear();
        max_seen_ = 0;
        bytes_since_compaction_ = 0;
        unknown_ = 0;
        post_ = 0;
        context_window_.reset();
        if (o_.qoder) u_.used_source = Source::Estimate;
        update_window();
    }

    void apply_line(std::string_view line) override {
        const bool maybe = line.find("\"usage\"") != std::string_view::npos ||
                           line.find("\"system\"") != std::string_view::npos ||
                           line.find("\"runtime-config\"") != std::string_view::npos ||
                           line.find("\"cost-state\"") != std::string_view::npos;
        if (!maybe) {
            bytes_since_compaction_ += line.size();
            if (o_.qoder) update_window();
            return;
        }
        Rec r;
        const bool ok = jscan::scan(line, kWanted, [&](std::string_view path, const Scalar& v) {
            if (path == "type") r.type = v.text();
            else if (path == "subtype") r.subtype = v.text();
            else if (path == "isSidechain") r.sidechain = v.kind == Scalar::Kind::Bool && v.boolean;
            else if (path == "isApiErrorMessage") r.api_error = v.kind == Scalar::Kind::Bool && v.boolean;
            else if (path == "version" || path == "cliVersion") r.version = v.raw.size() <= 64 ? v.text() : std::string();
            else if (path == "timestamp") r.time_ms = parse_iso8601_ms(v.raw);
            else if (path == "message.id") r.id = v.raw.size() <= 256 ? v.text() : std::string();
            else if (path == "message.model") r.model = v.raw.size() <= 128 ? v.text() : std::string();
            else if (path == "model") r.top_model = v.raw.size() <= 128 ? v.text() : std::string();
            else if (path == "message.usage.input_tokens") r.has_usage = true, r.in = v.u64().value_or(0);
            else if (path == "message.usage.cache_read_input_tokens") r.has_usage = true, r.cr = v.u64().value_or(0);
            else if (path == "message.usage.cache_creation_input_tokens") r.has_usage = true, r.cc = v.u64().value_or(0);
            else if (path == "message.usage.output_tokens") r.has_usage = true, r.out = v.u64().value_or(0);
            else if (path == "compactMetadata.preTokens") r.pre = v.u64();
            else if (path == "compactMetadata.postTokens") r.post = v.u64();
            else if (path == "contextWindow") r.context_window = v.u64();
            else if (path == "totalCostUSD" && v.kind == Scalar::Kind::Number) r.cost = v.number;
        });
        if (!ok) return;   // a torn or foreign line
        if (o_.qoder) {
            // Qoder names its models by a server-side id (a UUID) whose display name is not on
            // disk: no name is better than the id.
            if (looks_like_uuid(r.model)) r.model.clear();
            if (looks_like_uuid(r.top_model)) r.top_model.clear();
        }
        if (!r.version.empty()) note_version(r.version, o_.qoder ? kQoderKnownMajor : kClaudeKnownMajor);
        if (r.type == "assistant") {
            apply_assistant(r, line.size());
        } else if (r.type == "system") {
            apply_system(r);
        } else if (r.type == "runtime-config") {
            if (r.context_window && *r.context_window) context_window_ = r.context_window;
            if (!r.top_model.empty()) u_.model = r.top_model;
            saw_usage_ = true;
            if (u_.format == FormatStatus::NoData) u_.format = FormatStatus::Ok;
        } else if (r.type == "cost-state") {
            if (r.cost) u_.cost_usd = r.cost;
        } else {
            bytes_since_compaction_ += line.size();
        }
        if (r.time_ms && (r.type == "assistant" || r.type == "system" || r.type == "runtime-config")) {
            u_.updated_unix_ms = std::max(u_.updated_unix_ms, *r.time_ms);
        }
        update_window();
    }

    void apply_assistant(const Rec& r, std::size_t line_bytes) {
        bytes_since_compaction_ += line_bytes;
        if (r.sidechain || r.api_error) return;   // subagent (own context) or a failed call
        if (r.model == "<synthetic>") return;     // locally generated, zero usage
        if (!r.has_usage) {
            // An assistant record with "usage" somewhere but none of the fields this build reads:
            // a format change. After a few of them without any usage the status says so.
            if (++unknown_ >= 8 && !saw_usage_) u_.format = FormatStatus::UnknownVersion;
            return;
        }
        const std::uint64_t ctx = r.in + r.cr + r.cc;
        if (!r.model.empty()) u_.model = r.model;
        if (r.id.empty() || ids_.insert(r.id)) {
            ++u_.api_calls;
            u_.input_tokens += r.in;
            u_.cache_read_tokens += r.cr;
            u_.cache_write_tokens += r.cc;
            u_.output_tokens += r.out;
        }
        if (o_.qoder && ctx == 0) return;   // Qoder 1.1.x: zeros; keep the estimate
        u_.used_tokens = ctx;
        u_.used_source = Source::Data;
        u_.after_compaction = false;
        max_seen_ = std::max(max_seen_, ctx);
        saw_usage_ = true;
        if (u_.format == FormatStatus::NoData) u_.format = FormatStatus::Ok;
    }

    void apply_system(const Rec& r) {
        if (r.subtype == "turn_duration") {
            ++u_.turns;
        } else if (r.subtype == "compact_boundary") {
            ++u_.compactions;
            if (r.pre) max_seen_ = std::max(max_seen_, *r.pre);
            if (r.post) {
                post_ = *r.post;
                bytes_since_compaction_ = 0;
                if (!o_.qoder || u_.used_source != Source::Data) {
                    u_.used_tokens = *r.post;
                    u_.after_compaction = true;
                }
            }
            saw_usage_ = true;
            if (u_.format == FormatStatus::NoData) u_.format = FormatStatus::Ok;
        }
    }

    void update_window() {
        if (o_.qoder && u_.used_source == Source::Estimate && saw_usage_) {
            u_.used_tokens = post_ + bytes_since_compaction_ / 4;
        }
        std::optional<std::uint64_t> w;
        Source src = Source::Unknown;
        if (context_window_) {
            w = context_window_;
            src = Source::Data;
        } else if (auto mw = model_window(u_.model, o_.windows)) {
            w = mw->first;
            src = mw->second;
        }
        if (!o_.qoder) {
            if (o_.configured_model.find("[1m]") != std::string::npos && (!w || *w < k1m)) {
                w = k1m;
                src = Source::Config;
            }
            if (max_seen_ > k200k && (!w || *w < k1m) && src != Source::Config) {
                w = k1m;
                src = Source::Inferred;
            }
        }
        u_.window_tokens = w;
        u_.window_source = w ? src : Source::Unknown;
        if (w && *w && u_.used_tokens) {
            u_.percent = 100.0 * static_cast<double>(*u_.used_tokens) / static_cast<double>(*w);
        } else {
            u_.percent.reset();
        }
    }

    ClaudeReducerOptions o_;
    detail::RecentIds ids_;
    std::uint64_t max_seen_ = 0;
    std::uint64_t post_ = 0;
    std::uint64_t bytes_since_compaction_ = 0;
    std::optional<std::uint64_t> context_window_;
    int unknown_ = 0;
};

std::string model_argument(const std::vector<std::string>& argv) {
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (argv[i] == "--model" && i + 1 < argv.size()) return argv[i + 1];
        if (argv[i].starts_with("--model=")) return argv[i].substr(8);
    }
    return {};
}

bool file_exists(const IFileSystem& fs, const std::string& path) { return fs.mtime_ns(path).has_value(); }

// <dir>/projects/<slug(cwd)>/<id>.jsonl, else the same name in any project folder.
std::optional<std::string> transcript_of(const IFileSystem& fs, const std::string& dir, std::string_view cwd,
                                         std::string_view id) {
    const std::string projects = join_path(dir, "projects");
    const std::string name = std::string(id) + ".jsonl";
    if (!cwd.empty()) {
        const std::string p = join_path(join_path(projects, claude_project_dir_name(cwd)), name);
        if (file_exists(fs, p)) return p;
    }
    auto entries = fs.list_dir(projects);
    if (!entries) return std::nullopt;
    for (const auto& e : *entries) {
        if (!e.is_dir) continue;
        const std::string p = join_path(join_path(projects, e.name), name);
        if (file_exists(fs, p)) return p;
    }
    return std::nullopt;
}

class ClaudeProbe final : public IContextProbe {
public:
    ClaudeProbe(const IFileSystem& fs, const IProcessProbe& proc, std::string dir, WindowOverrides w, bool qoder)
        : fs_(fs), proc_(proc), dir_(std::move(dir)), windows_(std::move(w)), qoder_(qoder) {}

    std::string_view agent() const override { return qoder_ ? "qodercli" : "claude"; }

    std::optional<ContextTarget> locate(const ContextQuery& q) const override {
        return qoder_ ? locate_qoder(q) : locate_claude(q);
    }

    std::unique_ptr<IContextReducer> reducer(const ContextTarget&) const override {
        ClaudeReducerOptions o;
        o.qoder = qoder_;
        o.windows = windows_;
        return make_claude_reducer(std::move(o));
    }

    Activity activity(const ContextQuery& q) const override {
        if (qoder_ || q.process.pid == 0) return Activity::Unknown;
        ClaudeSessionFiles files(fs_, proc_, dir_);
        auto pf = files.pid_file(q.process.pid);
        return pf && pf->status ? claude_activity(*pf->status) : Activity::Unknown;
    }

    SessionLabel session_label(const ContextQuery& q) const override {
        if (qoder_ || q.process.pid == 0) return {};
        ClaudeSessionFiles files(fs_, proc_, dir_);
        auto pf = files.pid_file(q.process.pid);
        if (!pf) return {};
        return SessionLabel{pf->name.value_or(""), pf->cwd.value_or("")};
    }

    // The model the agent was started with ("[1m]" selects the large window).
    std::string configured_model(const ContextQuery& q) const {
        std::string m = model_argument(q.argv);
        if (!m.empty()) return m;
        if (auto text = fs_.read_file(join_path(dir_, "settings.json"), kMaxSessionFileBytes)) {
            static constexpr std::array<std::string_view, 1> kModel{"model"};
            (void)jscan::scan(*text, kModel, [&](std::string_view, const Scalar& v) {
                if (v.raw.size() <= 128) m = v.text();
            });
        }
        return m;
    }

private:
    std::optional<ContextTarget> locate_claude(const ContextQuery& q) const {
        // The agent's own report (statusLine relay) names the transcript exactly.
        if (!q.reported_transcript.empty() && !q.reported_session.empty() &&
            base_name(q.reported_transcript) == q.reported_session + ".jsonl" && file_exists(fs_, q.reported_transcript)) {
            return target(q.reported_transcript, q.reported_session);
        }
        std::optional<AgentSessionInfo> s = q.session;
        if (!s) {
            ClaudeSessionFiles files(fs_, proc_, dir_);
            s = files.session_of(q.process);
        }
        if (!s || s->session_id.empty()) return std::nullopt;
        std::string cwd = s->cwd.value_or(q.process.cwd.empty() ? q.pane_cwd : q.process.cwd);
        if (auto p = transcript_of(fs_, dir_, cwd, s->session_id)) return target(*p, s->session_id);
        return std::nullopt;
    }

    std::optional<ContextTarget> locate_qoder(const ContextQuery& q) const {
        std::optional<AgentSessionInfo> s = q.session;
        if (!s) {
            QoderSessionFiles files(fs_, proc_, dir_);
            s = files.session_of(q.process);
        }
        if (!s || s->session_id.empty()) return std::nullopt;
        const std::string qdir = join_path(dir_, ".qoder");
        std::string cwd = s->cwd.value_or(q.process.cwd.empty() ? q.pane_cwd : q.process.cwd);
        if (auto p = transcript_of(fs_, qdir, cwd, s->session_id)) return target(*p, s->session_id);
        return std::nullopt;
    }

    static ContextTarget target(const std::string& path, const std::string& id) {
        ContextTarget t;
        t.path = path;
        t.session_id = id;
        t.watch_dir = parent_dir(path);
        return t;
    }

    const IFileSystem& fs_;
    const IProcessProbe& proc_;
    std::string dir_;   // Claude: the config folder; Qoder: the home folder
    WindowOverrides windows_;
    bool qoder_;
};

// The Claude probe hands the configured model to its reducers.
class ClaudeProbeWithModel final : public IContextProbe {
public:
    explicit ClaudeProbeWithModel(std::unique_ptr<ClaudeProbe> p, WindowOverrides w)
        : p_(std::move(p)), windows_(std::move(w)) {}
    std::string_view agent() const override { return p_->agent(); }
    std::optional<ContextTarget> locate(const ContextQuery& q) const override {
        auto t = p_->locate(q);
        if (t) model_ = p_->configured_model(q);
        return t;
    }
    std::unique_ptr<IContextReducer> reducer(const ContextTarget&) const override {
        ClaudeReducerOptions o;
        o.windows = windows_;
        o.configured_model = model_;
        return make_claude_reducer(std::move(o));
    }
    Activity activity(const ContextQuery& q) const override { return p_->activity(q); }
    SessionLabel session_label(const ContextQuery& q) const override { return p_->session_label(q); }

private:
    std::unique_ptr<ClaudeProbe> p_;
    WindowOverrides windows_;
    mutable std::string model_;   // of the last locate(); the keeper calls locate then reducer
};

}  // namespace

std::unique_ptr<IContextReducer> make_claude_reducer(ClaudeReducerOptions o) {
    return std::make_unique<ClaudeReducer>(std::move(o));
}

std::unique_ptr<IContextProbe> make_claude_context_probe(const IFileSystem& fs, const IProcessProbe& proc,
                                                         std::string config_dir, WindowOverrides windows) {
    auto p = std::make_unique<ClaudeProbe>(fs, proc, std::move(config_dir), windows, false);
    return std::make_unique<ClaudeProbeWithModel>(std::move(p), std::move(windows));
}

std::unique_ptr<IContextProbe> make_qoder_context_probe(const IFileSystem& fs, const IProcessProbe& proc,
                                                        std::string home) {
    return std::make_unique<ClaudeProbe>(fs, proc, std::move(home), WindowOverrides{}, true);
}

}  // namespace kt::agents
