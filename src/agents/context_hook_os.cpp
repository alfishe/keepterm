// File side of the statusLine hook (agents/context_hook.h), kt_agents_os.
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "agents/context_hook.h"
#include "agents/json_lite.h"
#include "agents/json_scan.h"
#include "agents/os_access.h"
#include "platform/paths.h"

namespace kt::agents {
namespace {

namespace fs = std::filesystem;

constexpr const char* kShimName = "keepterm-agent-statusline";
constexpr const char* kStateName = "claude.json";
constexpr const char* kBackupSuffix = ".keepterm-bak";

fs::path P(const std::string& s) { return platform::paths::from_utf8(s); }
std::string U(const fs::path& p) { return platform::paths::to_utf8(p); }

std::optional<std::string> read_text(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Temp file in the same folder, then rename over the target; the target's permissions are kept.
Result<void> write_atomic(const fs::path& p, std::string_view data, std::optional<fs::perms> perms = std::nullopt) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".keepterm-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return make_error(Errc::os_error, "cannot write " + U(tmp));
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) return make_error(Errc::os_error, "cannot write " + U(tmp));
    }
    if (!perms) {
        if (auto st = fs::status(p, ec); !ec && fs::exists(st)) perms = st.permissions();
    }
    if (perms) fs::permissions(tmp, *perms, ec);
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return make_error(Errc::os_error, "cannot replace " + U(p) + ": " + ec.message());
    }
    return {};
}

std::string fnv_hex(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

[[maybe_unused]] std::string forward_slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

// The "command" of a statusLine value.
std::string command_of(std::string_view value_raw) {
    static constexpr std::array<std::string_view, 1> kCmd{"command"};
    std::string cmd;
    (void)jscan::scan(value_raw, kCmd, [&](std::string_view, const jscan::Scalar& v) { cmd = v.text(); });
    return cmd;
}

// The first path in a command: "quoted" or up to the first space.
std::string first_path(std::string_view cmd) {
    while (!cmd.empty() && cmd.front() == ' ') cmd.remove_prefix(1);
    if (!cmd.empty() && (cmd.front() == '"' || cmd.front() == '\'')) {
        const char q = cmd.front();
        const auto e = cmd.find(q, 1);
        return std::string(cmd.substr(1, e == std::string_view::npos ? std::string_view::npos : e - 1));
    }
    return std::string(cmd.substr(0, cmd.find(' ')));
}

bool same_file(const std::string& a, const std::string& b) {
    std::error_code ec;
    if (a.empty() || b.empty()) return false;
    return fs::equivalent(P(a), P(b), ec);
}

[[maybe_unused]] std::string sh_quote(std::string_view s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

struct SavedState {
    bool present = false;
    std::optional<std::string> chained_raw;   // the previous statusLine value, byte for byte
    bool existed = true;                      // settings.json existed before the install
    std::string written_hash;                 // of the settings.json keepterm wrote
};

class ClaudeHook final : public IStatusLineHook {
public:
    explicit ClaudeHook(HookPaths p) : p_(std::move(p)) {}

    HookStatus status() const override {
        HookStatus s;
        s.settings_file = U(settings());
        const auto text = read_text(settings());
        const SavedState st = load_state();
        s.chained = st.chained_raw.has_value();
        if (!text) return s;
        const auto raw = json_member_raw(*text, "statusLine");
        if (!raw) return s;
        s.command = command_of(*raw);
        if (!is_keepterm_statusline(s.command)) {
            s.state = HookState::Foreign;
            return s;
        }
        // A value written before the refresh timer existed is repaired to get one; a user's own
        // refreshInterval stays.
        s.state = current(s.command) && json_member_raw(*raw, "refreshInterval") ? HookState::Installed : HookState::Stale;
        return s;
    }

    Result<HookStatus> install() override {
        HookStatus s = status();
        if (s.state == HookState::Installed) {
            if (auto r = write_shim(); !r) return fail(s, r.error());
            return s;
        }
        if (s.state == HookState::Stale) return repair(s);
        const auto old_text = read_text(settings());
        const std::string text = old_text.value_or(std::string());
        SavedState st;
        st.present = true;
        st.existed = old_text.has_value();
        if (auto raw = json_member_raw(text, "statusLine")) st.chained_raw = *raw;   // foreign: chained
        auto next = json_set_member(text, "statusLine", statusline_value(relay_command()));
        if (!next) return fail(s, Error{Errc::invalid_argument, 0, U(settings()) + " is not a JSON object; not changed"});
        if (auto r = write_shim(); !r) return fail(s, r.error());
        if (old_text) {
            fs::path bak = settings();
            bak += kBackupSuffix;
            if (auto r = write_atomic(bak, *old_text); !r) return fail(s, r.error());
        }
        st.written_hash = fnv_hex(*next);
        if (auto r = save_state(st); !r) return fail(s, r.error());
        if (auto r = write_atomic(settings(), *next); !r) return fail(s, r.error());
        return status();
    }

    Result<HookStatus> uninstall() override {
        HookStatus s = status();
        if (s.state != HookState::Installed && s.state != HookState::Stale) {
            if (s.state == HookState::NotInstalled) remove_own_files();
            return s;   // nothing of ours in Claude's file
        }
        const SavedState st = load_state();
        const auto text = read_text(settings()).value_or(std::string());
        fs::path bak = settings();
        bak += kBackupSuffix;
        std::error_code ec;
        if (st.present && !st.written_hash.empty() && fnv_hex(text) == st.written_hash) {
            // Untouched since the install: the original bytes come back exactly.
            if (!st.existed) {
                fs::remove(settings(), ec);
                if (ec) return fail(s, Error{Errc::os_error, 0, "cannot remove " + U(settings())});
            } else if (auto orig = read_text(bak)) {
                if (auto r = write_atomic(settings(), *orig); !r) return fail(s, r.error());
            } else {
                return surgical(s, st, text);
            }
        } else {
            return surgical(s, st, text);
        }
        remove_own_files();
        return status();
    }

    Result<HookStatus> self_check() override {
        HookStatus s = status();
        if (s.state == HookState::Stale) return repair(s);
        if (s.state == HookState::Installed) {
#ifndef _WIN32
            // The shim may name an older keepterm while still existing.
            if (!shim_current()) {
                if (auto r = write_shim(); !r) return fail(s, r.error());
                s.state = HookState::Repaired;
            }
#endif
        }
        return s;
    }

private:
    fs::path settings() const { return P(p_.claude_config_dir) / "settings.json"; }
    fs::path hook_dir() const { return P(p_.state_dir) / "agent-hooks"; }
    fs::path shim() const { return hook_dir() / kShimName; }
    fs::path state_file() const { return hook_dir() / kStateName; }

    std::string relay_command() const {
#ifdef _WIN32
        return "\"" + forward_slashes(p_.exe) + "\" --agent-statusline claude";
#else
        return "\"" + U(shim()) + "\"";
#endif
    }

    // The command runs the current keepterm.
    bool current(const std::string& cmd) const {
        const std::string path = first_path(cmd);
#ifdef _WIN32
        return same_file(path, p_.exe);
#else
        if (!same_file(path, U(shim()))) return false;
        return shim_target_exists();
#endif
    }

    std::string shim_text() const {
        return "#!/bin/sh\n# Claude Code statusLine relay of keepterm (keepterm --agent-hook); rewritten by keepterm.\nexec " +
               sh_quote(p_.exe) + " --agent-statusline claude \"$@\"\n";
    }
    bool shim_target_exists() const {
        auto t = read_text(shim());
        if (!t) return false;
        const auto pos = t->find("\nexec ");
        if (pos == std::string::npos) return false;
        std::string rest = t->substr(pos + 6);
        // 'path' as written by sh_quote (no embedded quotes in practice).
        if (rest.empty() || rest.front() != '\'') return false;
        const auto e = rest.find('\'', 1);
        if (e == std::string::npos) return false;
        std::error_code ec;
        return fs::exists(P(rest.substr(1, e - 1)), ec);
    }
    bool shim_current() const {
        auto t = read_text(shim());
        return t && *t == shim_text();
    }

    Result<void> write_shim() const {
#ifdef _WIN32
        return {};
#else
        if (shim_current()) return {};
        return write_atomic(shim(), shim_text(), fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                                     fs::perms::others_read | fs::perms::others_exec);
#endif
    }

    Result<HookStatus> repair(HookStatus s) {
#ifdef _WIN32
        const auto text = read_text(settings()).value_or(std::string());
        auto next = json_set_member(text, "statusLine", statusline_value(relay_command()));
        if (!next) return fail(s, Error{Errc::invalid_argument, 0, U(settings()) + " is not a JSON object"});
        if (auto r = write_atomic(settings(), *next); !r) return fail(s, r.error());
        SavedState st = load_state();
        st.present = true;
        st.written_hash = fnv_hex(*next);
        (void)save_state(st);
#else
        if (auto r = write_shim(); !r) return fail(s, r.error());
        const auto cur = read_text(settings()).value_or(std::string());
        const auto cur_raw = json_member_raw(cur, "statusLine");
        if (first_path(s.command) != U(shim()) || !cur_raw || !json_member_raw(*cur_raw, "refreshInterval")) {
            // An older shim location: point Claude at the current one.
            const auto text = read_text(settings()).value_or(std::string());
            auto next = json_set_member(text, "statusLine", statusline_value(relay_command()));
            if (!next) return fail(s, Error{Errc::invalid_argument, 0, U(settings()) + " is not a JSON object"});
            if (auto r = write_atomic(settings(), *next); !r) return fail(s, r.error());
            SavedState st = load_state();
            st.present = true;
            st.written_hash = fnv_hex(*next);
            (void)save_state(st);
        }
#endif
        HookStatus out = status();
        if (out.state == HookState::Installed) out.state = HookState::Repaired;
        return out;
    }

    Result<HookStatus> surgical(HookStatus s, const SavedState& st, const std::string& text) {
        std::optional<std::string> next =
            st.chained_raw ? json_set_member(text, "statusLine", *st.chained_raw) : json_remove_member(text, "statusLine");
        if (!next) return fail(s, Error{Errc::invalid_argument, 0, U(settings()) + " is not a JSON object; not changed"});
        if (auto r = write_atomic(settings(), *next); !r) return fail(s, r.error());
        remove_own_files();
        return status();
    }

    void remove_own_files() const {
        std::error_code ec;
        fs::remove(state_file(), ec);
        fs::remove(shim(), ec);
    }

    SavedState load_state() const {
        SavedState st;
        auto t = read_text(state_file());
        if (!t) return st;
        auto v = json::parse(*t);
        if (!v || v->kind != json::Value::Kind::Object) return st;
        st.present = true;
        if (auto c = v->string_at("chained_raw")) st.chained_raw = *c;
        if (const json::Value* e = v->get("existed")) st.existed = e->boolean;
        st.written_hash = v->string_at("written_hash").value_or("");
        return st;
    }

    Result<void> save_state(const SavedState& st) const {
        std::string o = "{\n  \"version\": 1,\n  \"settings\": ";
        json::write_string(o, U(settings()));
        o += ",\n  \"chained_raw\": ";
        if (st.chained_raw) json::write_string(o, *st.chained_raw);
        else o += "null";
        o += ",\n  \"existed\": ";
        o += st.existed ? "true" : "false";
        o += ",\n  \"written_hash\": ";
        json::write_string(o, st.written_hash);
        o += "\n}\n";
        return write_atomic(state_file(), o, fs::perms::owner_read | fs::perms::owner_write);
    }

    static Result<HookStatus> fail(HookStatus s, const Error& e) {
        s.error = e.what;
        return Unexpected(Error{e.code, e.os_code, e.what});
    }

    HookPaths p_;
};

}  // namespace

std::unique_ptr<IStatusLineHook> make_claude_statusline_hook(HookPaths paths) {
    return std::make_unique<ClaudeHook>(std::move(paths));
}

HookPaths default_hook_paths(std::string_view configured_claude_dir, std::string exe) {
    HookPaths p;
    p.claude_config_dir = claude_config_dir(configured_claude_dir, environment_variable("CLAUDE_CONFIG_DIR"), home_directory());
    if (auto s = platform::paths::env_utf8("KEEPTERM_STATE_DIR"); s && !s->empty()) {
        p.state_dir = *s;
    } else if (auto d = platform::paths::get(platform::paths::Kind::state)) {
        p.state_dir = U(*d);
    }
    p.exe = std::move(exe);
    return p;
}

std::optional<std::string> chained_statusline_command(const std::string& state_dir) {
    auto t = read_text(P(state_dir) / "agent-hooks" / kStateName);
    if (!t) return std::nullopt;
    auto v = json::parse(*t);
    if (!v) return std::nullopt;
    auto raw = v->string_at("chained_raw");
    if (!raw) return std::nullopt;
    std::string cmd = command_of(*raw);
    if (cmd.empty() || is_keepterm_statusline(cmd)) return std::nullopt;
    return cmd;
}

}  // namespace kt::agents
