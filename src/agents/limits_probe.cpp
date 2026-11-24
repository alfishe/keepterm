// The probe descriptions and parsers (agents/limits_probe.h); the OS side is limits_probe_os.cpp.
#include "agents/limits_probe.h"

#include <array>
#include <cmath>

#include "agents/json_scan.h"

namespace kt::agents {

namespace {

struct Window {
    std::string_view name;
    std::uint32_t minutes;
};
constexpr std::array<Window, 4> kClaudeWindows{{
    {"five_hour", 300},
    {"seven_day", 7 * 24 * 60},
    {"seven_day_opus", 7 * 24 * 60},
    {"seven_day_sonnet", 7 * 24 * 60},
}};

// "rate_limit_info.unifiedWindows.<name>.utilization" and ".resetsAt" for every window, then the
// event type.
const std::vector<std::string>& claude_paths() {
    static const std::vector<std::string> paths = [] {
        std::vector<std::string> p;
        for (const Window& w : kClaudeWindows) {
            const std::string base = "rate_limit_info.unifiedWindows." + std::string(w.name);
            p.push_back(base + ".utilization");
            p.push_back(base + ".resetsAt");
        }
        p.emplace_back("type");
        return p;
    }();
    return paths;
}

}  // namespace

std::vector<RateLimit> parse_claude_limits(std::string_view output) {
    static const std::vector<std::string_view> wanted = [] {
        std::vector<std::string_view> w;
        for (const std::string& s : claude_paths()) w.emplace_back(s);
        return w;
    }();
    std::vector<RateLimit> out;
    std::size_t pos = 0;
    while (pos < output.size()) {
        std::size_t end = output.find('\n', pos);
        if (end == std::string_view::npos) end = output.size();
        const std::string_view line = output.substr(pos, end - pos);
        pos = end + 1;
        if (line.find("\"rate_limit_event\"") == std::string_view::npos) continue;
        bool event = false;
        std::array<double, kClaudeWindows.size()> used{};
        std::array<std::int64_t, kClaudeWindows.size()> resets{};
        std::array<bool, kClaudeWindows.size()> seen{};
        const bool ok = jscan::scan(line, wanted, [&](std::string_view p, const jscan::Scalar& v) {
            if (p == "type") {
                event = v.kind == jscan::Scalar::Kind::String && v.raw == "rate_limit_event";
                return;
            }
            for (std::size_t i = 0; i < kClaudeWindows.size(); ++i) {
                const std::string& base = claude_paths()[2 * i];
                if (p == base && v.kind == jscan::Scalar::Kind::Number && std::isfinite(v.number)) {
                    used[i] = v.number;
                    seen[i] = true;
                } else if (p == claude_paths()[2 * i + 1] && v.kind == jscan::Scalar::Kind::Number) {
                    resets[i] = static_cast<std::int64_t>(v.number > 1e12 ? v.number / 1000 : v.number);
                }
            }
        });
        if (!ok || !event) continue;
        std::vector<RateLimit> limits;
        for (std::size_t i = 0; i < kClaudeWindows.size(); ++i) {
            if (!seen[i]) continue;
            // A fraction (0.06 = 6 %); values above 1 are an overage.
            limits.push_back(RateLimit{std::string(kClaudeWindows[i].name), used[i] * 100.0, kClaudeWindows[i].minutes, resets[i]});
        }
        if (!limits.empty()) out = std::move(limits);   // the last event wins
    }
    return out;
}

const std::vector<LimitsProbe>& builtin_limits_probes() {
    static const std::vector<LimitsProbe> probes = [] {
        std::vector<LimitsProbe> v;
        LimitsProbe c;
        c.agent = "claude";
#ifdef _WIN32
        c.program_names = {"claude.exe", "claude.cmd"};
        c.known_dirs = {"~/.local/bin", "~/AppData/Roaming/npm", "~/.claude/local"};
#else
        c.program_names = {"claude"};
        c.known_dirs = {"~/.local/bin", "~/.claude/local", "/opt/homebrew/bin", "/usr/local/bin", "~/.npm-global/bin", "~/.bun/bin"};
#endif
        // The smallest request: no tools, MCP servers, settings or session file; Haiku.
        c.args = {"-p", "ok", "--model", "haiku", "--output-format", "stream-json", "--verbose", "--no-session-persistence",
                  "--system-prompt", "Reply ok.", "--tools", "", "--strict-mcp-config", "--setting-sources", "", "--max-turns", "1"};
        c.parse = parse_claude_limits;
        v.push_back(std::move(c));
        return v;
    }();
    return probes;
}

const LimitsProbe* limits_probe_for(std::string_view agent) {
    for (const LimitsProbe& p : builtin_limits_probes())
        if (p.agent == agent) return &p;
    return nullptr;
}

}  // namespace kt::agents
