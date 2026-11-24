// The table-driven part of the CLI: project, tab, pane, window, session, agent
// and layout commands, plus shell completion. Each command maps to one wire command; its
// positional words and options become the request's args. `session *`, `layout show`,
// `layout validate` and `completion` also work without a window.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "app/control.h"
#include "platform/paths.h"
#include "session/client.h"
#include "session/history.h"
#include "session/os.h"
#include "session/spawn.h"
#include "workspace/json.h"
#include "workspace/fs.h"
#include "workspace/presets.h"
#include "workspace/project_file.h"
#include "workspace/runtime.h"

namespace kt::app::control {

namespace {

enum class Kind : std::uint8_t { Bool, Str, Int, Num, On, Off };

struct FlagSpec {
    std::string_view flag;   // "--no-focus"
    std::string_view key;    // "no_focus"
    Kind kind = Kind::Bool;
};

struct CmdSpec {
    std::string_view noun, verb, wire;
    std::vector<std::string_view> pos;   // arg keys; a '?' prefix marks an optional word
    std::vector<FlagSpec> flags;
    bool pane_target = false;            // --pane, default KEEPTERM_PANE
    std::string_view help;
};

const FlagSpec kForce{"--force", "force", Kind::Bool};
const FlagSpec kProject{"--project", "project", Kind::Str};
const FlagSpec kOn{"--on", "on", Kind::On};
const FlagSpec kOff{"--off", "on", Kind::Off};

const std::vector<CmdSpec>& specs() {
    static const std::vector<CmdSpec> s = {
        {"project", "list", "project.list", {}, {}, false, "projects in sidebar order, Scratch last"},
        {"project", "create", "project.create", {"path"}, {{"--name", "name", Kind::Str}, {"--select", "select", Kind::Bool}}, false,
         "add a folder as a new project"},
        {"project", "select", "project.select", {"project"}, {}, false, "show a project in the window"},
        {"project", "rename", "project.rename", {"project", "name"}, {}, false, "rename a project"},
        {"project", "color", "project.color", {"project", "color"}, {}, false,
         "none red orange yellow green teal blue purple pink gray"},
        {"project", "remove", "project.remove", {"project"}, {kForce}, false, "remove a project and end its sessions"},
        {"tab", "list", "tab.list", {}, {kProject, {"--all", "all", Kind::Bool}}, false, "tabs of a project (--all: of every project)"},
        {"tab", "new", "tab.new", {},
         {kProject, {"--cwd", "cwd", Kind::Str}, {"--shell", "shell", Kind::Str}, {"--run", "run", Kind::Str},
          {"--no-focus", "no_focus", Kind::Bool}},
         false, "open a tab"},
        {"tab", "select", "tab.select", {"tab"}, {kProject}, false, "select a tab (id, place or title)"},
        {"tab", "move", "tab.move", {"tab", "slot"}, {kProject}, false, "move a tab to a 1-based place"},
        {"tab", "move-to-project", "tab.move_to_project", {"tab", "to"}, {kProject, {"--slot", "slot", Kind::Int}}, false,
         "move a tab with its sessions to another project"},
        {"tab", "rename", "tab.rename", {"?tab", "?title"}, {kProject, {"--reset", "reset", Kind::Bool}}, false,
         "rename a tab (--reset: back to the automatic title)"},
        {"tab", "close", "tab.close", {"?tab"}, {kProject, kForce}, false, "close a tab"},
        {"pane", "inspect", "pane.inspect", {}, {}, true, "everything about a pane"},
        {"pane", "split", "pane.split", {},
         {{"--direction", "direction", Kind::Str}, {"--cwd", "cwd", Kind::Str}, {"--run", "run", Kind::Str},
          {"--no-focus", "no_focus", Kind::Bool}},
         true, "split a pane: --direction right|left|down|up|auto"},
        {"pane", "focus", "pane.focus", {}, {{"--direction", "direction", Kind::Str}}, true, "focus a pane or its neighbor"},
        {"pane", "close", "pane.close", {}, {kForce}, true, "close a pane"},
        {"pane", "key", "pane.key", {"key"}, {{"--repeat", "repeat", Kind::Int}}, true, "press a key: ctrl+c, enter, up, f5 ..."},
        {"pane", "zoom", "pane.zoom", {}, {kOn, kOff}, true, "zoom a pane (toggle by default)"},
        {"pane", "resize-split", "pane.resize_split", {}, {{"--axis", "axis", Kind::Str}, {"--ratio", "ratio", Kind::Num}}, true,
         "set the ratio of the split around a pane (0.15..0.85)"},
        {"pane", "equalize", "pane.equalize", {}, {}, true, "equal sizes for the panes of the tab"},
        {"pane", "private", "pane.private", {}, {kOn, kOff}, true, "history saving off for a pane (toggle by default)"},
        {"pane", "read-only", "pane.read_only", {}, {kOn, kOff}, true, "ignore typing in a pane (toggle by default)"},
        {"pane", "restart", "pane.restart", {}, {kForce}, true, "new shell in the pane's folder, history kept"},
        {"window", "list", "window.list", {}, {}, false, "windows"},
        {"window", "focus", "window.focus", {"?window"}, {}, false, "bring a window (default: the key one) to front"},
        {"window", "new", "window.new", {}, {kProject}, false, "open a window"},
        {"window", "close", "window.close", {"?window"}, {}, false, "close a window (sessions keep running)"},
        {"quick-terminal", "toggle", "quick_terminal.toggle", {}, {}, false, "show or hide the quick terminal"},
        {"quick-terminal", "show", "quick_terminal.show", {}, {}, false, "show the quick terminal"},
        {"quick-terminal", "hide", "quick_terminal.hide", {}, {}, false, "hide the quick terminal"},
        {"quick-terminal", "status", "quick_terminal.status", {}, {}, false, "shown or hidden, the hotkey"},
        {"agent", "list", "agent.list", {}, {}, false, "agents and their saved sessions"},
        {"agent", "forget", "agent.forget", {}, {{"--all", "all", Kind::Bool}}, true, "forget a pane's saved agent session"},
        {"layout", "save", "layout.save", {}, {kProject}, false, "save the project's tabs and splits to its project file"},
        {"layout", "apply", "layout.apply", {},
         {kProject, kForce, {"--dry-run", "dry_run", Kind::Bool}, {"--keep-sessions", "keep_sessions", Kind::Bool}}, false,
         "reconcile the project's tabs with its project file"},
    };
    return s;
}

std::string_view type_name(Kind k) {
    switch (k) {
        case Kind::Str: return "string";
        case Kind::Int: return "integer";
        case Kind::Num: return "number";
        default: return "boolean";
    }
}

const CmdSpec* find_spec(std::string_view noun, std::string_view verb) {
    for (const CmdSpec& c : specs())
        if (c.noun == noun && c.verb == verb) return &c;
    return nullptr;
}

std::optional<std::string> env_value(const char* name) {
    std::string v = session::os::getenv_utf8(name);
    if (v.empty()) return std::nullopt;
    return v;
}

int usage_error(const std::string& msg) {
    std::fprintf(stderr, "keepterm: %s\n", msg.c_str());
    return 2;
}

std::string pad(std::string s, std::size_t w) {
    if (s.size() < w) s.append(w - s.size(), ' ');
    return s;
}

std::string short_id(const std::string& id) { return id.size() > 8 && id != "scratch" ? id.substr(0, 8) : id; }

const std::vector<json::Value>* array_of(const json::Value& d, std::string_view key) {
    const json::Value* v = d.get(key);
    return v ? v->as_array() : nullptr;
}

std::string scalar_text(const json::Value& v) {
    if (const std::string* s = v.as_string()) return *s;
    if (auto b = v.as_bool()) return *b ? "yes" : "no";
    return v.dump(false);
}

// Human output of a successful answer.
void print_human(std::string_view wire, const json::Value& d) {
    if (wire == "project.list") {
        if (auto a = array_of(d, "projects"))
            for (const json::Value& p : *a)
                std::printf("%s %s  %s  %lld tabs, %lld panes  %s%s\n", p.bool_or("shown", false) ? "*" : " ",
                            pad(short_id(p.str_or("id")), 8).c_str(), pad(p.str_or("name"), 20).c_str(),
                            static_cast<long long>(p.int_or("tabs", 0)), static_cast<long long>(p.int_or("panes", 0)),
                            p.str_or("path").c_str(), p.str_or("color") != "none" ? ("  [" + p.str_or("color") + "]").c_str() : "");
        return;
    }
    if (wire == "tab.list") {
        if (auto a = array_of(d, "tabs"))
            for (const json::Value& t : *a)
                std::printf("%s %2lld  %s  %s  %lld pane%s%s  %s\n", t.bool_or("selected", false) ? "*" : " ",
                            static_cast<long long>(t.int_or("index", 0)), pad(short_id(t.str_or("id")), 8).c_str(),
                            pad(t.str_or("title"), 24).c_str(), static_cast<long long>(t.int_or("panes", 0)),
                            t.int_or("panes", 0) == 1 ? "" : "s", t.bool_or("zoomed", false) ? " (zoomed)" : "", t.str_or("project").c_str());
        return;
    }
    if (wire == "window.list") {
        if (auto a = array_of(d, "windows"))
            for (const json::Value& w : *a)
                std::printf("%s %s  %s  %s\n", w.bool_or("this", false) ? "*" : " ", short_id(w.str_or("id")).c_str(),
                            w.str_or("project").c_str(), w.str_or("tab").c_str());
        return;
    }
    if (wire.starts_with("quick_terminal.")) {
        std::printf("%s", d.bool_or("shown", false) ? "shown" : "hidden");
        if (!d.str_or("hotkey").empty()) std::printf("  hotkey %s", d.str_or("hotkey").c_str());
        else if (!d.str_or("hotkey_error").empty()) std::printf("  no hotkey: %s", d.str_or("hotkey_error").c_str());
        std::printf("\n");
        return;
    }
    if (wire == "agent.list") {
        if (auto a = array_of(d, "agents"))
            for (const json::Value& g : *a)
                std::printf("%s  %-8s %-9s %s  %s\n", short_id(g.str_or("pane")).c_str(), g.str_or("agent").c_str(),
                            !g.str_or("running").empty() ? "running" : g.bool_or("resume", true) ? "resumes" : "no resume",
                            g.str_or("agent_session").empty() ? "-" : g.str_or("agent_session").c_str(), g.str_or("cwd").c_str());
        return;
    }
    if (wire == "pane.inspect") {
        for (const auto& [k, v] : d.members()) std::printf("%-17s %s\n", k.c_str(), scalar_text(v).c_str());
        return;
    }
    if (wire == "layout.apply" && d.get("tabs")) {
        std::printf("layout of %s (%s): ", d.str_or("project").c_str(), d.str_or("file").c_str());
        if (auto a = array_of(d, "tabs")) std::printf("%zu tabs", a->size());
        std::printf(", keeps %lld panes", static_cast<long long>(d.int_or("kept_panes", 0)));
        if (d.int_or("closes_panes", 0) > 0) std::printf(", closes %lld panes", static_cast<long long>(d.int_or("closes_panes", 0)));
        if (d.int_or("detaches_panes", 0) > 0) std::printf(", detaches %lld panes", static_cast<long long>(d.int_or("detaches_panes", 0)));
        std::printf("\n");
        if (auto a = array_of(d, "tabs"))
            for (const json::Value& t : *a) {
                const std::string title = t.str_or("title");
                std::printf("  %-20s %lld panes, %lld kept%s\n", title.empty() ? "(untitled)" : title.c_str(),
                            static_cast<long long>(t.int_or("panes", 0)), static_cast<long long>(t.int_or("kept", 0)),
                            t.bool_or("existing", false) ? "" : ", new tab");
            }
        return;
    }
    if (wire == "layout.save") {
        std::printf("%s\n", d.str_or("file").c_str());
        return;
    }
    if (wire == "project.create" || wire == "tab.new" || wire == "pane.split") {
        std::printf("%s\n", (wire == "project.create" ? d.str_or("id") : wire == "tab.new" ? d.str_or("pane") : d.str_or("pane")).c_str());
        return;
    }
}

// ---------------------------------------------------------------- the GUI-bound commands

int run_spec(const std::string& ep, const CmdSpec& c, std::span<const std::string_view> rest) {
    json::Value a = json::Value::object();
    bool as_json = false;
    std::size_t next_pos = 0;
    for (std::size_t k = 0; k < rest.size(); ++k) {
        const std::string_view w = rest[k];
        if (w == "--json") {
            as_json = true;
            continue;
        }
        if (c.pane_target && w == "--pane") {
            if (k + 1 >= rest.size()) return usage_error("--pane needs a pane id");
            a.set("pane", json::Value::string(std::string(rest[++k])));
            continue;
        }
        if (w.starts_with("--")) {
            const FlagSpec* f = nullptr;
            for (const FlagSpec& fs : c.flags)
                if (fs.flag == w) f = &fs;
            if (!f) return usage_error("unknown option " + std::string(w) + " (keepterm help " + std::string(c.noun) + ")");
            if (f->kind == Kind::Bool || f->kind == Kind::On || f->kind == Kind::Off) {
                a.set(std::string(f->key), json::Value::boolean(f->kind != Kind::Off));
                continue;
            }
            if (k + 1 >= rest.size()) return usage_error(std::string(w) + " needs a value");
            const std::string v(rest[++k]);
            if (f->kind == Kind::Str) {
                a.set(std::string(f->key), json::Value::string(v));
            } else {
                char* end = nullptr;
                const double num = std::strtod(v.c_str(), &end);
                if (end == v.c_str() || *end) return usage_error(std::string(w) + " needs a number");
                a.set(std::string(f->key), f->kind == Kind::Int ? json::Value::integer(static_cast<int64_t>(num)) : json::Value::number(num));
            }
            continue;
        }
        if (next_pos >= c.pos.size()) {
            // The last word takes the rest (titles, names with spaces).
            if (c.pos.empty()) return usage_error("unexpected word " + std::string(w));
            std::string_view key = c.pos.back();
            if (key.starts_with("?")) key.remove_prefix(1);
            a.set(std::string(key), json::Value::string(a.str_or(key) + " " + std::string(w)));
            continue;
        }
        std::string_view key = c.pos[next_pos++];
        if (key.starts_with("?")) key.remove_prefix(1);
        if (key == "slot") {
            char* end = nullptr;
            const long n = std::strtol(std::string(w).c_str(), &end, 10);
            if (n < 1) return usage_error("the place is a number from 1");
            a.set(std::string(key), json::Value::integer(n));
        } else {
            a.set(std::string(key), json::Value::string(std::string(w)));
        }
    }
    for (std::size_t k = next_pos; k < c.pos.size(); ++k)
        if (!c.pos[k].starts_with("?"))
            return usage_error("keepterm " + std::string(c.noun) + " " + std::string(c.verb) + " needs <" + std::string(c.pos[k]) + ">");
    if (c.pane_target && !a.get("pane"))
        if (auto p = env_value("KEEPTERM_PANE")) a.set("pane", json::Value::string(*p));
    const int timeout = c.wire == "layout.apply" ? 120000 : 10000;
    const std::string_view wire = c.wire;
    return finish_response(request(ep, std::string(c.wire), std::move(a), timeout), as_json,
                           [wire](const json::Value& d) { print_human(wire, d); });
}

// ---------------------------------------------------------------- sessions (no GUI needed)

// Session ids the panes claim: the GUI's panes, else the state file. `read_only`: the sessions
// of read-only panes (pane.read_only), which `session send` refuses.
std::optional<std::map<std::string, std::string>> claims(const std::string& ep, const session::os::Paths& p, bool& gui,
                                                         std::set<std::string>* read_only = nullptr) {
    std::map<std::string, std::string> out;
    auto r = request(ep, "pane.list", json::Value::object(), 5000);
    if (r && r->bool_or("ok", false)) {
        gui = true;
        if (const json::Value* d = r->get("data"))
            if (auto a = array_of(*d, "panes"))
                for (const json::Value& pn : *a) {
                    out[pn.str_or("session")] = pn.str_or("id");
                    if (read_only && pn.bool_or("read_only", false)) read_only->insert(pn.str_or("session"));
                }
        return out;
    }
    gui = false;
    std::ifstream f(session::os::join(p.state, "workspace.json"), std::ios::binary);
    if (!f) return out;
    std::stringstream ss;
    ss << f.rdbuf();
    auto v = json::parse(ss.str());
    if (!v) return std::nullopt;
    // Every pane object of the file: {"id": ..., "session": ...}.
    std::function<void(const json::Value&)> walk = [&](const json::Value& x) {
        if (x.kind() == json::Value::Kind::Object) {
            const std::string sid = x.str_or("session"), pid = x.str_or("id");
            if (!sid.empty() && !pid.empty()) {
                out[sid] = pid;
                if (read_only && x.bool_or("read_only", false)) read_only->insert(sid);
            }
            for (const auto& [k, c] : x.members()) walk(c);
        } else if (auto arr = x.as_array()) {
            for (const json::Value& c : *arr) walk(c);
        }
    };
    walk(*v);
    return out;
}

// A session id or a unique prefix of one.
std::optional<session::SessionId> pick_session(const json::Value& list, std::string_view want, std::string& why) {
    std::vector<std::string> hits;
    if (auto a = list.as_array())
        for (const json::Value& s : *a) {
            const std::string id = s.str_or("session");
            if (id == want)
                if (auto r = session::SessionId::parse(id)) return *r;
            if (!want.empty() && id.starts_with(want)) hits.push_back(id);
        }
    if (hits.size() == 1)
        if (auto r = session::SessionId::parse(hits.front())) return *r;
    why = hits.empty() ? "no session " + std::string(want) : "more than one session matches " + std::string(want);
    return std::nullopt;
}

const json::Value* session_entry(const json::Value& list, const std::string& id) {
    if (auto a = list.as_array())
        for (const json::Value& s : *a)
            if (s.str_or("session") == id) return &s;
    return nullptr;
}

// A program other than the shell runs in the session (the host's foreground chain).
std::string session_busy(const session::os::Paths& p, const session::SessionId& id) {
    auto pi = session::request_process_info(session::os::endpoint_name(p, id), 2000);
    if (!pi || pi->foreground_is_shell || !pi->foreground_pid || pi->foreground_pid == pi->root_pid) return {};
    return pi->foreground_chain.empty() || pi->foreground_chain.back().image.empty() ? std::string("a program")
                                                                                     : pi->foreground_chain.back().image;
}

int print_json_or(bool as_json, const json::Value& d, const std::function<void()>& human) {
    if (as_json) std::printf("%s\n", ok_response("1", d).dump(true).c_str());
    else human();
    return 0;
}

int fail_with(bool as_json, std::string_view code, const std::string& msg) {
    if (as_json) std::printf("%s\n", error_response("1", code, msg).dump(false).c_str());
    else std::fprintf(stderr, "keepterm: %s\n", msg.c_str());
    return exit_code_for(code);
}

int cli_session(const std::string& ep, std::span<const std::string_view> args) {
    const std::string_view verb = args.size() > 1 ? args[1] : std::string_view("list");
    bool as_json = false, force = false, scrollback = false, orphans = false, kill = false, submit = true;
    std::vector<std::string> words;
    std::string text;
    bool rest_text = false;
    for (std::size_t k = 2; k < args.size(); ++k) {
        const std::string_view w = args[k];
        if (rest_text) {
            text += (text.empty() ? "" : " ") + std::string(w);
        } else if (w == "--") {
            rest_text = true;
        } else if (w == "--json") {
            as_json = true;
        } else if (w == "--force") {
            force = true;
        } else if (w == "--scrollback") {
            scrollback = true;
        } else if (w == "--orphans") {
            orphans = true;
        } else if (w == "--kill") {
            kill = true;
        } else if (w == "--no-submit") {
            submit = false;
        } else if (w.starts_with("--")) {
            return usage_error("unknown option " + std::string(w));
        } else {
            words.emplace_back(w);
        }
    }
    auto paths = session::os::paths();
    if (!paths) return fail_with(as_json, kInternal, paths.error().what);
    const json::Value list = session::list_sessions(*paths);
    bool gui = false;
    std::set<std::string> read_only;
    auto claimed = claims(ep, *paths, gui, &read_only);
    auto pane_of = [&](const std::string& sid) -> std::string {
        if (!claimed) return {};
        auto it = claimed->find(sid);
        return it == claimed->end() ? std::string() : it->second;
    };

    if (verb == "list" || verb == "orphans") {
        const bool only_orphans = orphans || verb == "orphans";
        json::Value out = json::Value::array();
        if (auto a = list.as_array())
            for (const json::Value& s : *a) {
                json::Value e = s;
                const std::string pane = pane_of(s.str_or("session"));
                if (!pane.empty()) e.set("pane", json::Value::string(pane));
                if (only_orphans && (!pane.empty() || s.str_or("state") != "live")) continue;
                out.push(std::move(e));
            }
        if (verb == "orphans" && kill) {
            json::Value ended = json::Value::array(), skipped = json::Value::array();
            if (auto a = out.as_array())
                for (const json::Value& s : *a) {
                    auto id = session::SessionId::parse(s.str_or("session"));
                    if (!id) continue;
                    if (!force && !session_busy(*paths, *id).empty()) {
                        skipped.push(json::Value::string(id->str()));
                        continue;
                    }
                    if (session::end_session(*paths, *id)) ended.push(json::Value::string(id->str()));
                }
            json::Value d = json::Value::object();
            d.set("ended", std::move(ended));
            d.set("busy", std::move(skipped));
            return print_json_or(as_json, d, [&] {
                std::printf("ended %zu orphan sessions", d.get("ended")->as_array()->size());
                if (!d.get("busy")->as_array()->empty())
                    std::printf("; %zu run a program (--force ends them)", d.get("busy")->as_array()->size());
                std::printf("\n");
            });
        }
        json::Value d = json::Value::object();
        d.set("gui", json::Value::boolean(gui));
        d.set("sessions", out);
        return print_json_or(as_json, d, [&] {
            if (auto a = out.as_array())
                for (const json::Value& s : *a)
                    std::printf("%s  %-6s %7lld  %-8s  %s\n", s.str_or("session").c_str(), s.str_or("state").c_str(),
                                static_cast<long long>(s.int_or("pid", 0)),
                                s.str_or("pane").empty() ? (s.str_or("state") == "live" ? "orphan" : "-") : short_id(s.str_or("pane")).c_str(),
                                s.str_or("cwd").c_str());
        });
    }
    if (words.empty()) return usage_error("keepterm session " + std::string(verb) + " needs <id>");
    std::string why;
    auto id = pick_session(list, words[0], why);
    if (!id) return fail_with(as_json, kNotFound, why);
    const json::Value* entry = session_entry(list, id->str());
    const bool live = entry && entry->str_or("state") == "live";
    const std::string pane = pane_of(id->str());

    if (verb == "info" || verb == "inspect") {
        json::Value d = entry ? *entry : json::Value::object();
        if (!pane.empty()) d.set("pane", json::Value::string(pane));
        if (live) {
            if (auto pi = session::request_process_info(session::os::endpoint_name(*paths, *id), 2000)) {
                d.set("host_pid", json::Value::integer(pi->host_pid));
                d.set("root_pid", json::Value::integer(pi->root_pid));
                d.set("foreground_pid", json::Value::integer(pi->foreground_pid));
                d.set("foreground", json::Value::string(pi->foreground_is_shell || pi->foreground_chain.empty()
                                                            ? std::string()
                                                            : pi->foreground_chain.back().image));
                d.set("cwd", json::Value::string(pi->cwd));
                d.set("cwd_source", json::Value::string(pi->cwd_source));
                d.set("backend", json::Value::string(pi->backend));
                d.set("shell", json::Value::string(pi->shell_kind));
            }
            // The agent's context and limits as the host reads them (what the Agent page shows).
            if (auto c = session::request_agent_context(session::os::endpoint_name(*paths, *id), 2000);
                c && c->status != session::proto::AgentContextStatus::None) {
                using St = session::proto::AgentContextStatus;
                d.set("agent", json::Value::string(c->agent));
                d.set("agent_status", json::Value::string(c->status == St::Ok ? "ok" : c->status == St::NoData ? "no_data" : "other"));
                if (!c->session_id.empty()) d.set("agent_session", json::Value::string(c->session_id));
                if (!c->model.empty()) d.set("model", json::Value::string(c->model));
                if (c->used_tokens) d.set("context_used", json::Value::integer(static_cast<int64_t>(*c->used_tokens)));
                if (c->window_tokens) d.set("context_window", json::Value::integer(static_cast<int64_t>(*c->window_tokens)));
                if (c->percent_x100) {
                    char b[32];
                    std::snprintf(b, sizeof b, "%.2f", *c->percent_x100 / 100.0);
                    d.set("context_percent", json::Value::string(b));
                }
                std::string limits;
                for (const auto& l : c->limits) {
                    char b[96];
                    std::snprintf(b, sizeof b, "%s%s %.1f%%", limits.empty() ? "" : ", ", l.name.c_str(), l.used_percent_x100 / 100.0);
                    limits += b;
                }
                if (!limits.empty()) d.set("limits", json::Value::string(limits));
            }
        }
        if (auto rec = session::read_recovery_record(session::os::join(paths->state_sessions, id->str()))) {
            d.set("history", json::Value::boolean(rec->history));
            d.set("size", json::Value::string(std::to_string(rec->cols) + "x" + std::to_string(rec->rows)));
            if (rec->restored_from) d.set("restored_from", json::Value::string(rec->restored_from->str()));
        }
        return print_json_or(as_json, d, [&] {
            for (const auto& [k, v] : d.members()) std::printf("%-15s %s\n", k.c_str(), scalar_text(v).c_str());
        });
    }
    if (verb == "dump") {
        if (!live) return fail_with(as_json, "not_live", "the session is not running");
        auto t = session::session_text(*paths, *id, scrollback);
        if (!t) return fail_with(as_json, kInternal, t.error().what);
        json::Value d = json::Value::object();
        d.set("session", json::Value::string(id->str()));
        d.set("text", json::Value::string(*t));
        return print_json_or(as_json, d, [&] { std::fwrite(t->data(), 1, t->size(), stdout); });
    }
    if (verb == "send") {
        if (!live) return fail_with(as_json, "not_live", "the session is not running");
        if (read_only.count(id->str()))
            return fail_with(as_json, "read_only", "the session's pane is read-only (keepterm pane read-only --off)");
        // The words after the id, then the text after "--".
        std::string joined;
        for (std::size_t k = 1; k < words.size(); ++k) joined += (k > 1 ? " " : "") + words[k];
        if (!text.empty()) joined += (joined.empty() ? "" : " ") + text;
        text = joined;
        for (char& ch : text)
            if (ch == '\n') ch = '\r';
        if (text.empty() && submit) return usage_error("nothing to type");
        if (submit) text += '\r';
        if (auto r = session::send_session_text(*paths, *id, text); !r) return fail_with(as_json, kInternal, r.error().what);
        json::Value d = json::Value::object();
        d.set("session", json::Value::string(id->str()));
        return print_json_or(as_json, d, [] {});
    }
    if (verb == "kill") {
        if (!pane.empty() && gui) {
            // The pane closes with its session (the GUI checks for a running program).
            json::Value a = json::Value::object();
            a.set("session", json::Value::string(id->str()));
            a.set("force", json::Value::boolean(force));
            return finish_response(request(ep, "session.kill", std::move(a), 10000), as_json, [](const json::Value&) {});
        }
        if (live && !force)
            if (const std::string b = session_busy(*paths, *id); !b.empty())
                return fail_with(as_json, kBusy, b + " runs in the session (--force ends it)");
        if (auto r = session::end_session(*paths, *id); !r) return fail_with(as_json, kInternal, r.error().what);
        json::Value d = json::Value::object();
        d.set("session", json::Value::string(id->str()));
        return print_json_or(as_json, d, [] {});
    }
    if (verb == "restart") {
        if (pane.empty() || !gui) return fail_with(as_json, kNotRunning, "restart needs the window that shows the session");
        json::Value a = json::Value::object();
        a.set("pane", json::Value::string(pane));
        a.set("force", json::Value::boolean(force));
        return finish_response(request(ep, "pane.restart", std::move(a), 10000), as_json, [](const json::Value&) {});
    }
    return usage_error("unknown session command " + std::string(verb));
}

// ---------------------------------------------------------------- layouts (local)

std::string cli_home_dir() {
    std::string s = platform::paths::env_utf8("HOME").value_or("");
    if (s.empty()) s = platform::paths::env_utf8("USERPROFILE").value_or("");
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

// The project files as the GUI sees them; without a window the config is not
// read, so Macterm's folder is always searched on macOS (read only).
ws::ProjectFileStore cli_project_files(ws::IFileSystem& fs) {
    auto d = platform::paths::get(platform::paths::Kind::config, session::os::profile());
    std::vector<std::string> ro;
#if defined(__APPLE__)
    ro.push_back(ws::path_join(cli_home_dir(), ".config/macterm/projects"));
#endif
    return ws::ProjectFileStore(fs, d ? platform::paths::to_utf8(*d / "projects") : std::string(), std::move(ro), cli_home_dir());
}

int cli_layout_local(const std::string& ep, std::string_view verb, std::span<const std::string_view> rest) {
    bool as_json = false;
    std::string file, project, path;
    for (std::size_t k = 0; k < rest.size(); ++k) {
        if (rest[k] == "--json") as_json = true;
        else if (rest[k] == "--file" && k + 1 < rest.size()) file = std::string(rest[++k]);
        else if (rest[k] == "--project" && k + 1 < rest.size()) project = std::string(rest[++k]);
        else if (rest[k] == "--path" && k + 1 < rest.size()) path = std::string(rest[++k]);
        else if (!rest[k].starts_with("--") && file.empty()) file = std::string(rest[k]);
        else return usage_error("unknown option " + std::string(rest[k]));
    }
    if (verb == "validate" && file.empty()) return usage_error("keepterm layout validate <file>");
    auto fs = ws::make_os_file_system();
    std::string text;
    if (file.empty()) {
        // The file that declares the project's folder: the window's project, else --path.
        std::string name;
        if (path.empty()) {
            auto r = request(ep, "project.list", json::Value::object(), 5000);
            if (r && r->bool_or("ok", false))
                if (const json::Value* d = r->get("data"))
                    if (auto a = array_of(*d, "projects"))
                        for (const json::Value& p : *a)
                            if (project.empty() ? p.bool_or("shown", false)
                                                : (p.str_or("id").starts_with(project) || p.str_or("name") == project)) {
                                name = p.str_or("name");
                                path = p.str_or("path");
                            }
            if (path.empty()) return usage_error("name the project (--project, with a window), its folder (--path) or the file (--file)");
        }
        if (name.empty()) name = ws::folder_name(path);
        auto found = cli_project_files(*fs).find(path, name);
        if (!found) return fail_with(as_json, "layout_no_file", "no project file declares " + path);
        file = found->file;
        text = std::move(found->text);
    } else {
        const ws::ReadResult r = fs->read(file);
        if (r.status != ws::ReadStatus::Ok) return fail_with(as_json, "layout_no_file", "cannot read " + file);
        text = r.data;
    }
    auto pf = ws::parse_project_file(text);
    if (!pf) return fail_with(as_json, "layout_parse", file + ": " + pf.error().message);
    json::Value d = json::Value::object();
    d.set("file", json::Value::string(file));
    d.set("path", json::Value::string(pf->path));
    if (pf->name) d.set("name", json::Value::string(*pf->name));
    json::Value tabs = json::Value::array();
    const std::size_t n = pf->tabs ? pf->tabs->size() : 0;
    if (pf->tabs)
        for (const auto& t : *pf->tabs) tabs.push(json::Value::string(t.title.value_or(std::string())));
    d.set("tabs", std::move(tabs));
    if (verb == "validate")
        return print_json_or(as_json, d, [&] {
            if (n) std::printf("%s: ok, %zu tabs\n", file.c_str(), n);
            else std::printf("%s: ok, declares the project only (no tabs)\n", file.c_str());
        });
    return print_json_or(as_json, d, [&] { std::fwrite(text.data(), 1, text.size(), stdout); });
}

// ---------------------------------------------------------------- mcp

// `keepterm mcp`: how an agent's MCP client starts the bridge next to this executable
//; the CLI stays the fallback.
int cli_mcp(std::string_view verb) {
    if (!verb.empty() && verb != "config" && verb != "path") return usage_error("keepterm mcp [config|path]");
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string exe = session::os::self_exe();
    const fs::path self(std::u8string(exe.begin(), exe.end()));
#ifdef _WIN32
    const fs::path bridge = self.parent_path() / "keepterm-mcp.exe";
#else
    const fs::path bridge = self.parent_path() / "keepterm-mcp";
#endif
    if (self.empty() || !fs::is_regular_file(bridge, ec)) {
        std::fprintf(stderr, "keepterm: keepterm-mcp is not next to this keepterm; use the CLI (keepterm help)\n");
        return 1;
    }
    const std::u8string u8 = bridge.u8string();
    const std::string path(u8.begin(), u8.end());
    if (verb == "path") {
        std::printf("%s\n", path.c_str());
        return 0;
    }
    json::Value server = json::Value::object();
    server.set("command", json::Value::string(path));
    json::Value servers = json::Value::object();
    servers.set("keepterm", std::move(server));
    json::Value root = json::Value::object();
    root.set("mcpServers", std::move(servers));
    std::string text = root.dump(true);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    std::printf("%s\n", text.c_str());
    return 0;
}

// ---------------------------------------------------------------- completion

std::vector<std::string> nouns() {
    std::vector<std::string> n = {"status", "exit", "restart", "help", "completion", "session", "layout", "version", "mcp"};
    for (const CmdSpec& c : specs())
        if (std::find(n.begin(), n.end(), std::string(c.noun)) == n.end()) n.emplace_back(c.noun);
    if (std::find(n.begin(), n.end(), "pane") == n.end()) n.emplace_back("pane");
    return n;
}

std::vector<std::string> verbs_of(std::string_view noun) {
    std::vector<std::string> v;
    if (noun == "pane") v = {"list", "run", "dump"};
    if (noun == "session") v = {"list", "info", "inspect", "dump", "send", "kill", "restart", "orphans"};
    if (noun == "layout") v = {"show", "validate"};
    if (noun == "mcp") v = {"config", "path"};
    for (const CmdSpec& c : specs())
        if (c.noun == noun && std::find(v.begin(), v.end(), std::string(c.verb)) == v.end()) v.emplace_back(c.verb);
    return v;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

int cli_completion(std::string_view shell) {
    std::string cases;
    for (const std::string& n : nouns()) {
        const auto v = verbs_of(n);
        if (!v.empty()) cases += "    " + n + ") words=\"" + join(v) + "\" ;;\n";
    }
    if (shell == "bash" || shell == "zsh") {
        std::string s = "# keepterm " + std::string(shell) + " completion: eval \"$(keepterm completion " + std::string(shell) + ")\"\n";
        if (shell == "zsh") s += "autoload -U +X bashcompinit && bashcompinit\n";
        s += "_keepterm() {\n  local cur=${COMP_WORDS[COMP_CWORD]} words\n  if [ $COMP_CWORD -eq 1 ]; then\n    words=\"" + join(nouns()) +
             "\"\n  elif [ $COMP_CWORD -eq 2 ]; then\n    case ${COMP_WORDS[1]} in\n" + cases +
             "    completion) words=\"bash zsh fish powershell\" ;;\n    *) words=\"\" ;;\n    esac\n  else\n    words=\"--json --force --pane --project\"\n  fi\n"
             "  COMPREPLY=($(compgen -W \"$words\" -- \"$cur\"))\n}\ncomplete -F _keepterm keepterm\n";
        std::fputs(s.c_str(), stdout);
        return 0;
    }
    if (shell == "fish") {
        std::string s = "# keepterm fish completion: keepterm completion fish | source\n";
        s += "complete -c keepterm -f -n '__fish_use_subcommand' -a '" + join(nouns()) + "'\n";
        for (const std::string& n : nouns()) {
            const auto v = verbs_of(n);
            if (!v.empty()) s += "complete -c keepterm -f -n '__fish_seen_subcommand_from " + n + "' -a '" + join(v) + "'\n";
        }
        s += "complete -c keepterm -f -n '__fish_seen_subcommand_from completion' -a 'bash zsh fish powershell'\n";
        std::fputs(s.c_str(), stdout);
        return 0;
    }
    if (shell == "powershell" || shell == "pwsh") {
        std::string s = "# keepterm PowerShell completion: keepterm completion powershell | Out-String | Invoke-Expression\n"
                        "Register-ArgumentCompleter -Native -CommandName keepterm, keepterm.exe, keepterm.com -ScriptBlock {\n"
                        "  param($wordToComplete, $commandAst, $cursorPosition)\n"
                        "  $w = @($commandAst.CommandElements | ForEach-Object { $_.ToString() })\n"
                        "  $n = $w.Count; if ($wordToComplete -ne '') { $n-- }\n"
                        "  $verbs = @{\n";
        for (const std::string& n : nouns()) {
            const auto v = verbs_of(n);
            if (!v.empty()) s += "    '" + n + "' = '" + join(v) + "'\n";
        }
        s += "    'completion' = 'bash zsh fish powershell'\n  }\n"
             "  if ($n -le 1) { $c = '" + join(nouns()) + "' } elseif ($n -eq 2) { $c = $verbs[$w[1]] } else { $c = '--json --force --pane --project' }\n"
             "  if ($c) { $c -split ' ' | Where-Object { $_ -like \"$wordToComplete*\" } | ForEach-Object {\n"
             "    [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_) } }\n}\n";
        std::fputs(s.c_str(), stdout);
        return 0;
    }
    return usage_error("keepterm completion bash|zsh|fish|powershell");
}

}  // namespace

std::string table_usage() {
    std::string s;
    for (const CmdSpec& c : specs()) {
        std::string line = "       keepterm " + std::string(c.noun) + " " + std::string(c.verb);
        for (std::string_view p : c.pos) line += p.starts_with("?") ? " [" + std::string(p.substr(1)) + "]" : " <" + std::string(p) + ">";
        if (c.pane_target) line += " [--pane ID]";
        for (const FlagSpec& f : c.flags) line += " [" + std::string(f.flag) + (f.kind == Kind::Str || f.kind == Kind::Int || f.kind == Kind::Num ? " X" : "") + "]";
        s += line + "\n";
    }
    s += "       keepterm session list|orphans [--orphans] [--kill] [--force]\n"
         "       keepterm session info|inspect|dump|kill|restart <id> [--scrollback] [--force]\n"
         "       keepterm session send <id> [words...] [-- text...] [--no-submit]\n"
         "       keepterm layout show [--project P | --path DIR | --file F]\n"
         "       keepterm layout validate <file>\n"
         "       keepterm completion bash|zsh|fish|powershell\n"
         "       keepterm mcp [config|path]   (the MCP server for agents: .mcp.json entry or the bridge path)\n";
    return s;
}

std::optional<int> cli_table(const std::string& ep, std::span<const std::string_view> args) {
    if (args.empty()) return std::nullopt;
    const std::string_view noun = args[0];
    if (noun == "session") return cli_session(ep, args);
    if (noun == "completion") return cli_completion(args.size() > 1 ? args[1] : std::string_view());
    if (noun == "mcp") return cli_mcp(args.size() > 1 ? args[1] : std::string_view());
    if (args.size() < 2) return std::nullopt;
    const std::string_view verb = args[1];
    if (noun == "layout" && (verb == "show" || verb == "validate")) return cli_layout_local(ep, verb, args.subspan(2));
    if (const CmdSpec* c = find_spec(noun, verb)) return run_spec(ep, *c, args.subspan(2));
    return std::nullopt;
}

// ---------------------------------------------------------------- the catalogue for agents

const std::vector<CommandInfo>& command_catalog() {
    static const std::vector<CommandInfo> all = [] {
        std::vector<CommandInfo> out;
        auto arg = [](std::string n, std::string t, std::string h, bool req = false) { return ArgInfo{std::move(n), std::move(t), std::move(h), req}; };
        const ArgInfo pane = arg("pane", "string", "pane id (a unique prefix works); default: the pane the agent runs in");
        for (const CmdSpec& c : specs()) {
            CommandInfo ci;
            ci.name = std::string(c.wire);
            ci.help = std::string(c.help);
            ci.pane_target = c.pane_target;
            ci.cli = "keepterm " + std::string(c.noun) + " " + std::string(c.verb);
            for (std::string_view p : c.pos) {
                const bool opt = p.starts_with("?");
                const std::string key(opt ? p.substr(1) : p);
                ci.args.push_back(arg(key, "string", "", !opt));
                ci.cli += opt ? " [" + key + "]" : " <" + key + ">";
            }
            for (const FlagSpec& f : c.flags) {
                if (std::any_of(ci.args.begin(), ci.args.end(), [&](const ArgInfo& a) { return a.name == f.key; })) continue;
                std::string h = f.kind == Kind::On ? "true: on, false: off; absent: toggle" : "";
                ci.args.push_back(arg(std::string(f.key), std::string(type_name(f.kind)), std::move(h)));
                ci.cli += " [" + std::string(f.flag) + (f.kind == Kind::Str || f.kind == Kind::Int || f.kind == Kind::Num ? " X]" : "]");
            }
            if (c.pane_target) {
                ci.args.push_back(pane);
                ci.cli += " [--pane ID]";
            }
            out.push_back(std::move(ci));
        }
        out.push_back({"status", "keepterm status", "the app: version, pid, counts of projects, tabs and panes", {}, false});
        out.push_back({"pane.list", "keepterm pane list", "every pane: id, project, tab, program, folder, the focused one", {}, false});
        out.push_back({"pane.run", "keepterm pane run [--pane ID] [--no-submit] -- text",
                       "type text into a pane's shell and press Enter (newlines become Enter too)",
                       {arg("text", "string", "the text to type", true), arg("submit", "boolean", "press Enter after the text (default true)"), pane},
                       true});
        out.push_back({"pane.dump", "keepterm pane dump [--pane ID] [--scrollback] [--lines N]", "the text on a pane's screen",
                       {arg("scrollback", "boolean", "include the scrollback"), arg("lines", "integer", "only the last N lines"), pane}, true});
        out.push_back({"session.kill", "keepterm session kill <id>", "end a session host",
                       {arg("session", "string", "session id", true), arg("force", "boolean", "also when a program runs")}, false});
        out.push_back({"command.run", "keepterm command run <id> [args...]", "run an app command by its id (the command palette's ids)",
                       {arg("id", "string", "command id", true), arg("args", "array", "its arguments (strings)"), arg("force", "boolean", ""), pane},
                       true});
        out.push_back({"app.exit", "keepterm exit [--force]", "quit keepterm, ending every session (saved for the next start)",
                       {arg("force", "boolean", "also when programs run")}, false});
        out.push_back({"app.restart", "keepterm restart [--force]", "quit and start keepterm again", {arg("force", "boolean", "also when programs run")},
                       false});
        return out;
    }();
    return all;
}

}  // namespace kt::app::control
