// Core part of the statusLine hook (agents/context_hook.h): states and the in-place JSON edits.
#include "agents/context_hook.h"

#include "agents/json_lite.h"
#include "agents/json_scan.h"

namespace kt::agents {
namespace {

bool blank(std::string_view s) {
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

const jscan::Member* find_member(const jscan::TopLevel& t, std::string_view key) {
    const jscan::Member* found = nullptr;
    for (const auto& m : t.members) {
        if (m.key == key) found = &m;   // the last duplicate wins, as in JSON.parse
    }
    return found;
}

// The whitespace before the first member's key (its line's indentation), else two spaces.
std::string indent_of(std::string_view text, const jscan::TopLevel& t) {
    if (t.members.empty()) return "  ";
    const std::size_t k = t.members.front().key_begin;
    std::size_t b = k;
    while (b > 0 && (text[b - 1] == ' ' || text[b - 1] == '\t')) --b;
    if (b == 0 || text[b - 1] != '\n') return "  ";
    return std::string(text.substr(b, k - b));
}

}  // namespace

std::string_view to_string(HookState s) {
    switch (s) {
        case HookState::NotInstalled: return "not installed";
        case HookState::Installed: return "installed";
        case HookState::Stale: return "stale path";
        case HookState::Foreign: return "foreign";
        case HookState::Repaired: return "repaired";
    }
    return "not installed";
}

std::string describe(const HookStatus& s) {
    std::string out;
    switch (s.state) {
        case HookState::NotInstalled: out = "Not installed"; break;
        case HookState::Installed: out = "Installed"; break;
        case HookState::Stale: out = "Installed, but the keepterm it runs has moved"; break;
        case HookState::Foreign: out = "Not installed; Claude Code uses another status line"; break;
        case HookState::Repaired: out = "Installed (repaired)"; break;
    }
    if (s.chained && (s.state == HookState::Installed || s.state == HookState::Repaired || s.state == HookState::Stale)) {
        out += "; the previous status line is chained";
    }
    return out;
}

bool is_keepterm_statusline(std::string_view command) {
    return command.find("--agent-statusline") != std::string_view::npos ||
           command.find("keepterm-agent-statusline") != std::string_view::npos;
}

std::string statusline_value(std::string_view command) {
    std::string v = "{\"type\": \"command\", \"command\": ";
    json::write_string(v, command);
    // Claude Code runs the command only on its own events (a reply, a mode change ...); the
    // timer keeps the rate limits and the session's figures fresh while the session is idle.
    v += ", \"refreshInterval\": " + std::to_string(kStatusLineRefreshSeconds) + "}";
    return v;
}

std::optional<std::string> json_member_raw(std::string_view text, std::string_view key) {
    if (blank(text)) return std::nullopt;
    auto t = jscan::top_level(text);
    if (!t) return std::nullopt;
    const jscan::Member* m = find_member(*t, key);
    if (!m) return std::nullopt;
    return std::string(text.substr(m->value_begin, m->value_end - m->value_begin));
}

std::optional<std::string> json_set_member(std::string_view text, std::string_view key, std::string_view value_json) {
    std::string name;
    json::write_string(name, key);
    if (blank(text)) return "{\n  " + name + ": " + std::string(value_json) + "\n}\n";
    auto t = jscan::top_level(text);
    if (!t) return std::nullopt;
    std::string out(text);
    if (const jscan::Member* m = find_member(*t, key)) {
        out.replace(m->value_begin, m->value_end - m->value_begin, value_json);
        return out;
    }
    const std::string ind = indent_of(text, *t);
    if (t->members.empty()) {
        out.replace(t->open + 1, t->close - t->open - 1, "\n" + ind + name + ": " + std::string(value_json) + "\n");
        return out;
    }
    out.insert(t->members.back().value_end, ",\n" + ind + name + ": " + std::string(value_json));
    return out;
}

std::optional<std::string> json_remove_member(std::string_view text, std::string_view key) {
    if (blank(text)) return std::string(text);
    auto t = jscan::top_level(text);
    if (!t) return std::nullopt;
    std::size_t idx = t->members.size();
    for (std::size_t i = 0; i < t->members.size(); ++i) {
        if (t->members[i].key == key) idx = i;
    }
    if (idx == t->members.size()) return std::string(text);
    std::string out(text);
    const auto& m = t->members[idx];
    if (idx > 0) {
        // From the end of the previous value: the comma, the whitespace, the member.
        const std::size_t from = t->members[idx - 1].value_end;
        out.erase(from, m.value_end - from);
    } else if (t->members.size() > 1) {
        // The first member: up to the next key.
        out.erase(m.key_begin, t->members[1].key_begin - m.key_begin);
    } else {
        out.erase(t->open + 1, t->close - t->open - 1);
    }
    return out;
}

}  // namespace kt::agents
