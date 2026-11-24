#include "agents/context_statusline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "agents/json_scan.h"

namespace kt::agents {
namespace {

using jscan::Scalar;

constexpr std::array<std::string_view, 21> kWanted{
    "session_id", "transcript_path", "version", "model.id", "model.display_name", "cost.total_cost_usd",
    "context_window.context_window_size", "context_window.used_percentage", "context_window.total_input_tokens",
    "context_window.total_output_tokens", "context_window.current_usage.input_tokens",
    "context_window.current_usage.cache_creation_input_tokens", "context_window.current_usage.cache_read_input_tokens",
    "context_window.current_usage.output_tokens", "rate_limits.five_hour.used_percentage",
    "rate_limits.five_hour.resets_at", "rate_limits.seven_day.used_percentage", "rate_limits.seven_day.resets_at",
    "rate_limits.five_hour.resets_at_unix", "rate_limits.seven_day.resets_at_unix", "exceeds_200k_tokens"};

std::int64_t reset_time(const Scalar& v) {
    if (v.kind == Scalar::Kind::Number) {
        const double t = v.number;
        return static_cast<std::int64_t>(t > 1e12 ? t / 1000 : t);   // seconds or ms
    }
    if (v.kind == Scalar::Kind::String) {
        if (auto ms = parse_iso8601_ms(v.raw)) return *ms / 1000;
    }
    return 0;
}

}  // namespace

std::optional<StatusLineReport> parse_statusline(std::string_view json) {
    StatusLineReport r;
    std::optional<std::uint64_t> in, cc, cr;
    std::optional<double> five, seven;
    std::int64_t five_at = 0, seven_at = 0;
    const bool ok = jscan::scan(json, kWanted, [&](std::string_view p, const Scalar& v) {
        auto text = [&](std::size_t max) { return v.kind == Scalar::Kind::String && v.raw.size() <= max ? v.text() : std::string(); };
        if (p == "session_id") r.session_id = text(256);
        else if (p == "transcript_path") r.transcript_path = text(32 * 1024);
        else if (p == "version") r.version = text(64);
        else if (p == "model.id") r.model_id = text(128);
        else if (p == "model.display_name") r.model_name = text(128);
        else if (p == "cost.total_cost_usd" && v.kind == Scalar::Kind::Number) r.cost_usd = v.number;
        else if (p == "context_window.context_window_size") r.window_tokens = v.u64();
        else if (p == "context_window.used_percentage" && v.kind == Scalar::Kind::Number) r.used_percent = v.number;
        else if (p == "context_window.total_input_tokens") r.total_input = v.u64();
        else if (p == "context_window.total_output_tokens") r.total_output = v.u64();
        else if (p == "context_window.current_usage.input_tokens") in = v.u64();
        else if (p == "context_window.current_usage.cache_creation_input_tokens") cc = v.u64();
        else if (p == "context_window.current_usage.cache_read_input_tokens") cr = v.u64();
        else if (p == "rate_limits.five_hour.used_percentage" && v.kind == Scalar::Kind::Number) five = v.number;
        else if (p == "rate_limits.seven_day.used_percentage" && v.kind == Scalar::Kind::Number) seven = v.number;
        else if (p.starts_with("rate_limits.five_hour.resets_at")) five_at = reset_time(v);
        else if (p.starts_with("rate_limits.seven_day.resets_at")) seven_at = reset_time(v);
    });
    if (!ok || (r.session_id.empty() && r.model_id.empty())) return std::nullopt;
    if (in || cc || cr) r.used_tokens = in.value_or(0) + cc.value_or(0) + cr.value_or(0);
    if (r.window_tokens && *r.window_tokens == 0) r.window_tokens.reset();
    if (five) r.limits.push_back(RateLimit{"five_hour", *five, 300, five_at});
    if (seven) r.limits.push_back(RateLimit{"seven_day", *seven, 7 * 24 * 60, seven_at});
    return r;
}

void merge_statusline(ContextUsage& u, const StatusLineReport& r) {
    if (!u.session_id.empty() && !r.session_id.empty() && u.session_id != r.session_id) return;
    if (u.agent.empty()) u.agent = "claude";
    if (u.session_id.empty()) u.session_id = r.session_id;
    if (!r.model_id.empty()) u.model = r.model_id;
    if (!r.version.empty()) u.agent_version = r.version;
    if (r.window_tokens) {
        u.window_tokens = r.window_tokens;
        u.window_source = Source::StatusLine;
    }
    if (r.used_tokens && u.updated_unix_ms <= r.received_unix_ms) {
        u.used_tokens = r.used_tokens;
        u.used_source = Source::StatusLine;
        u.after_compaction = false;
    }
    if (r.used_percent && u.used_source == Source::StatusLine) {
        u.percent = *r.used_percent;
    } else if (u.used_tokens && u.window_tokens && *u.window_tokens) {
        u.percent = 100.0 * static_cast<double>(*u.used_tokens) / static_cast<double>(*u.window_tokens);
    }
    if (r.cost_usd) u.cost_usd = r.cost_usd;
    if (!r.limits.empty()) {
        // The first report of a session may repeat old values (a resume): it dates from the
        // newest transcript record (the last API response) when that is older.
        u.limits_unix_ms = r.limits_unix_ms ? r.limits_unix_ms : r.received_unix_ms;
        if (r.limits_first && u.updated_unix_ms > 0) u.limits_unix_ms = std::min(u.limits_unix_ms, u.updated_unix_ms);
        u.limits = r.limits;
        u.limits_source = Source::StatusLine;
    }
    if (u.format == FormatStatus::NoData) u.format = FormatStatus::Ok;
    u.updated_unix_ms = std::max(u.updated_unix_ms, r.received_unix_ms);
}

void stamp_limits(StatusLineReport& next, const StatusLineReport* prev) {
    next.limits_first = false;
    next.limits_unix_ms = 0;
    if (next.limits.empty()) return;
    const bool same_session = prev && prev->session_id == next.session_id && !prev->limits.empty();
    if (!same_session) {
        next.limits_unix_ms = next.received_unix_ms;
        next.limits_first = true;
        return;
    }
    const bool repeat = prev->limits == next.limits && prev->total_output == next.total_output;
    next.limits_unix_ms = repeat && prev->limits_unix_ms ? prev->limits_unix_ms : next.received_unix_ms;
    next.limits_first = repeat && prev->limits_first;
}

std::string short_tokens(std::uint64_t n) {
    char b[32];
    if (n >= 1'000'000) {
        const double m = static_cast<double>(n) / 1e6;
        if (n % 1'000'000 == 0) std::snprintf(b, sizeof b, "%lluM", static_cast<unsigned long long>(n / 1'000'000));
        else std::snprintf(b, sizeof b, "%.1fM", m);
    } else if (n >= 10'000) {
        std::snprintf(b, sizeof b, "%lluk", static_cast<unsigned long long>((n + 500) / 1000));
    } else if (n >= 1000) {
        std::snprintf(b, sizeof b, "%.1fk", static_cast<double>(n) / 1000.0);
    } else {
        std::snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(n));
    }
    return b;
}

std::string compact_statusline(const StatusLineReport& r) {
    std::string out = !r.model_name.empty() ? r.model_name : r.model_id;
    auto sep = [&] {
        if (!out.empty()) out += " \xC2\xB7 ";   // middle dot
    };
    if (r.window_tokens) {
        sep();
        out += "ctx ";
        if (r.used_tokens) out += short_tokens(*r.used_tokens) + "/";
        out += short_tokens(*r.window_tokens);
        std::optional<double> pct = r.used_percent;
        if (!pct && r.used_tokens) pct = 100.0 * static_cast<double>(*r.used_tokens) / static_cast<double>(*r.window_tokens);
        if (pct) {
            char b[16];
            std::snprintf(b, sizeof b, " %.0f%%", *pct);
            out += b;
        }
    }
    if (r.cost_usd) {
        sep();
        char b[32];
        std::snprintf(b, sizeof b, "$%.2f", *r.cost_usd);
        out += b;
    }
    return out;
}

}  // namespace kt::agents
