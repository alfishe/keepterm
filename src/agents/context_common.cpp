// Common helpers of the context probes (agents/context.h).
#include <algorithm>
#include <cmath>

#include "agents/context.h"
#include "agents/context_internal.h"

namespace kt::agents {

std::string_view to_string(Source s) {
    switch (s) {
        case Source::Unknown: return "unknown";
        case Source::Data: return "data";
        case Source::Table: return "table";
        case Source::Inferred: return "inferred";
        case Source::Config: return "config";
        case Source::StatusLine: return "statusline";
        case Source::Estimate: return "estimate";
    }
    return "unknown";
}

std::string_view to_string(Activity a) {
    switch (a) {
        case Activity::Unknown: return "unknown";
        case Activity::Busy: return "busy";
        case Activity::Waiting: return "waiting";
        case Activity::Idle: return "idle";
    }
    return "unknown";
}

void ContextProbes::add(std::unique_ptr<IContextProbe> p) {
    if (!p) return;
    const std::string id(p->agent());
    probes_[id] = std::move(p);
}

const IContextProbe* ContextProbes::find(std::string_view agent) const {
    auto it = probes_.find(agent);
    return it == probes_.end() ? nullptr : it->second.get();
}

ContextProbes builtin_context_probes(const IFileSystem& fs, const IProcessProbe& proc, const ICodexIndex* codex_index,
                                     const ContextPaths& paths, WindowOverrides windows, const IAgyStore* agy_store) {
    ContextProbes p;
    if (agy_store) p.add(make_agy_context_probe(fs, proc, agy_store, paths.home, windows));
    p.add(make_claude_context_probe(fs, proc, paths.claude_config_dir, std::move(windows)));
    p.add(make_codex_context_probe(fs, codex_index, paths.codex_home));
    p.add(make_qoder_context_probe(fs, proc, paths.home));
    return p;
}

bool glob_match(std::string_view p, std::string_view t) {
    // Iterative '*' matcher with backtracking to the last star.
    std::size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (pi < p.size() && p[pi] == t[ti]) {
            ++pi;
            ++ti;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

std::optional<std::uint64_t> parse_token_count(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return std::nullopt;
    std::uint64_t mult = 1;
    const char last = s.back();
    if (last == 'k' || last == 'K') mult = 1000;
    else if (last == 'm' || last == 'M') mult = 1'000'000;
    if (mult != 1) s.remove_suffix(1);
    if (s.empty() || s.size() > 12) return std::nullopt;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c == '_' || c == '\'') continue;
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v == 0) return std::nullopt;
    return v * mult;
}

std::optional<std::pair<std::uint64_t, Source>> model_window(std::string_view model, const WindowOverrides& overrides) {
    if (model.empty()) return std::nullopt;
    for (const auto& [glob, tokens] : overrides) {
        if (tokens && glob_match(glob, model)) return std::make_pair(tokens, Source::Config);
    }
    struct Row {
        std::string_view prefix;
        std::uint64_t tokens;
    };
    static constexpr Row kTable[] = {
#define KT_WINDOW(prefix, tokens) {prefix, tokens},
#include "agents/context_windows.inc"
#undef KT_WINDOW
    };
    for (const auto& r : kTable) {
        if (model.starts_with(r.prefix)) return std::make_pair(r.tokens, Source::Table);
    }
    return std::nullopt;
}

double codex_percent_used(std::uint64_t total_tokens, std::uint64_t window) {
    constexpr double kBaseline = 12000;
    const double w = static_cast<double>(window);
    if (w <= kBaseline) return 100.0;
    const double effective = w - kBaseline;
    const double used = std::max(static_cast<double>(total_tokens) - kBaseline, 0.0);
    const double left = std::clamp((effective - used) / effective * 100.0, 0.0, 100.0);
    return 100.0 - std::round(left);
}

std::optional<std::int64_t> parse_iso8601_ms(std::string_view s) {
    auto digits = [&](std::size_t pos, std::size_t n, int& out) {
        if (pos + n > s.size()) return false;
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    };
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!digits(0, 4, y) || s.size() < 19 || s[4] != '-' || !digits(5, 2, mo) || s[7] != '-' || !digits(8, 2, d) ||
        (s[10] != 'T' && s[10] != ' ') || !digits(11, 2, h) || s[13] != ':' || !digits(14, 2, mi) || s[16] != ':' ||
        !digits(17, 2, se)) {
        return std::nullopt;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return std::nullopt;
    std::size_t pos = 19;
    std::int64_t ms = 0;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        int n = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (n < 3) ms = ms * 10 + (s[pos] - '0');
            ++n;
            ++pos;
        }
        for (; n < 3; ++n) ms *= 10;
    }
    std::int64_t offset_min = 0;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        int oh = 0, om = 0;
        const bool neg = s[pos] == '-';
        if (!digits(pos + 1, 2, oh)) return std::nullopt;
        std::size_t mpos = pos + 3;
        if (mpos < s.size() && s[mpos] == ':') ++mpos;
        if (!digits(mpos, 2, om)) om = 0;
        offset_min = (neg ? -1 : 1) * (oh * 60 + om);
    }
    // Days from the civil date (Howard Hinnant's algorithm).
    const int yy = mo <= 2 ? y - 1 : y;
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
    const std::int64_t secs = days * 86400 + h * 3600 + mi * 60 + se - offset_min * 60;
    return secs * 1000 + ms;
}

Activity claude_activity(std::string_view status) {
    if (status == "busy" || status == "running" || status == "working") return Activity::Busy;
    if (status == "idle") return Activity::Idle;
    if (status.find("wait") != std::string_view::npos || status.find("input") != std::string_view::npos ||
        status.find("permission") != std::string_view::npos || status == "blocked") {
        return Activity::Waiting;
    }
    return Activity::Unknown;
}

std::optional<int> version_major(std::string_view v) {
    std::size_t i = 0;
    while (i < v.size() && (v[i] < '0' || v[i] > '9')) ++i;
    if (i == v.size()) return std::nullopt;
    int n = 0;
    for (; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i) {
        if (n > 100000) return std::nullopt;
        n = n * 10 + (v[i] - '0');
    }
    return n;
}

namespace detail {

void LineReducer::feed(std::string_view bytes) {
    while (!bytes.empty()) {
        const auto nl = bytes.find('\n');
        if (nl == std::string_view::npos) {
            if (!overlong_) {
                pending_.append(bytes);
                if (pending_.size() > kMaxLine) {
                    pending_.clear();
                    overlong_ = true;
                }
            }
            return;
        }
        if (!overlong_) pending_.append(bytes.substr(0, nl));
        bytes.remove_prefix(nl + 1);
        if (skip_first_) {
            skip_first_ = false;
        } else if (!overlong_) {
            std::string_view line = pending_;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.empty()) apply_line(line);
        }
        pending_.clear();
        overlong_ = false;
    }
}

void LineReducer::reset() {
    const std::string agent = u_.agent;
    u_ = ContextUsage{};
    u_.agent = agent;
    saw_usage_ = false;
    pending_.clear();
    skip_first_ = false;
    overlong_ = false;
    clear_state();
}

void LineReducer::reset_from_tail(std::string_view tail) {
    reset();
    skip_first_ = true;
    u_.totals_since_attach = true;
    feed(tail);
}

void LineReducer::note_version(std::string_view v, int known_major) {
    if (v.empty() || v.size() > 64) return;
    if (u_.agent_version != v) u_.agent_version = std::string(v);
    const auto major = version_major(v);
    if (major && *major > known_major) {
        u_.format = FormatStatus::UnknownVersion;
        u_.format_version = std::string(v);
    }
}

bool RecentIds::insert(std::string_view id) {
    if (set_.find(id) != set_.end()) return false;
    set_.emplace(id);
    order_.emplace_back(id);
    if (order_.size() > cap_) {
        set_.erase(set_.find(order_.front()));
        order_.pop_front();
    }
    return true;
}

std::string_view base_name(std::string_view path) {
    const auto p = path.find_last_of("/\\");
    return p == std::string_view::npos ? path : path.substr(p + 1);
}

std::string parent_dir(std::string_view path) {
    const auto p = path.find_last_of("/\\");
    return p == std::string_view::npos ? std::string() : std::string(path.substr(0, p));
}

}  // namespace detail
}  // namespace kt::agents
