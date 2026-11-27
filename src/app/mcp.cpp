#include "app/mcp.h"

#include <algorithm>
#include <atomic>

namespace kt::app::control::mcp {

namespace {

using V = json::Value;

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return o;
}

std::vector<std::string> words_of(std::string_view s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : s) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || static_cast<unsigned char>(c) >= 0x80;
        if (word) {
            w += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        } else if (!w.empty()) {
            out.push_back(std::move(w));
            w.clear();
        }
    }
    if (!w.empty()) out.push_back(std::move(w));
    return out;
}

int score(const CommandInfo& c, const std::vector<std::string>& query) {
    const std::vector<std::string> name = words_of(c.name);
    const std::string text = lower(c.cli + " " + c.help);
    int total = 0;
    for (const std::string& q : query) {
        int best = 0;
        for (const std::string& n : name) {
            if (n == q) best = std::max(best, 10);
            else if (n.starts_with(q) || q.starts_with(n)) best = std::max(best, 6);
            else if (n.find(q) != std::string::npos) best = std::max(best, 4);
        }
        for (const ArgInfo& a : c.args) {
            if (a.name == q) best = std::max(best, 3);
        }
        if (best == 0 && text.find(q) != std::string::npos) best = 2;
        total += best;
    }
    return total;
}

V command_json(const CommandInfo& c) {
    V o = V::object();
    o.set("command", V::string(c.name));
    o.set("help", V::string(c.help));
    o.set("cli", V::string(c.cli));
    V args = V::array();
    for (const ArgInfo& a : c.args) {
        V x = V::object();
        x.set("name", V::string(a.name));
        x.set("type", V::string(a.type));
        if (a.required) x.set("required", V::boolean(true));
        if (!a.help.empty()) x.set("help", V::string(a.help));
        args.push(std::move(x));
    }
    o.set("args", std::move(args));
    return o;
}

V rpc_result(const V& id, V result) {
    V r = V::object();
    r.set("jsonrpc", V::string("2.0"));
    r.set("id", id);
    r.set("result", std::move(result));
    return r;
}

V rpc_error(const V& id, int code, std::string message) {
    V e = V::object();
    e.set("code", V::integer(code));
    e.set("message", V::string(std::move(message)));
    V r = V::object();
    r.set("jsonrpc", V::string("2.0"));
    r.set("id", id);
    r.set("error", std::move(e));
    return r;
}

V text_result(std::string text, bool is_error) {
    V item = V::object();
    item.set("type", V::string("text"));
    item.set("text", V::string(std::move(text)));
    V content = V::array();
    content.push(std::move(item));
    V r = V::object();
    r.set("content", std::move(content));
    r.set("isError", V::boolean(is_error));
    return r;
}

V schema(V properties, std::vector<std::string> required) {
    V s = V::object();
    s.set("type", V::string("object"));
    s.set("properties", std::move(properties));
    V req = V::array();
    for (auto& r : required) req.push(V::string(std::move(r)));
    s.set("required", std::move(req));
    return s;
}

V prop(std::string type, std::string description) {
    V p = V::object();
    p.set("type", V::string(std::move(type)));
    p.set("description", V::string(std::move(description)));
    return p;
}

V tools_list() {
    V tools = V::array();
    {
        V props = V::object();
        props.set("query", prop("string", "Keywords, e.g. \"split pane\", \"type text\", \"screen\", \"layout\", \"session\". Empty lists every command."));
        V lim = prop("integer", "At most this many commands (default 20).");
        lim.set("minimum", V::integer(1));
        lim.set("maximum", V::integer(200));
        props.set("limit", std::move(lim));
        V t = V::object();
        t.set("name", V::string("search"));
        t.set("title", V::string("Find keepterm commands"));
        t.set("description",
              V::string("Search the commands of the running keepterm terminal (projects, tabs, panes, typing into a pane, reading "
                        "its screen, sessions, layouts, agents, windows). Returns each match with its `command` name and its "
                        "`args`; call `invoke` with them."));
        t.set("inputSchema", schema(std::move(props), {}));
        tools.push(std::move(t));
    }
    {
        V props = V::object();
        props.set("command", prop("string", "A command name from `search`, e.g. \"pane.split\"."));
        V a = prop("object", "The command's arguments by name, as `search` lists them. Pane commands act on the agent's own pane "
                             "unless `pane` is given.");
        a.set("additionalProperties", V::boolean(true));
        props.set("args", std::move(a));
        V t = V::object();
        t.set("name", V::string("invoke"));
        t.set("title", V::string("Run a keepterm command"));
        t.set("description",
              V::string("Run one keepterm command and return its result as JSON. Commands that would end running programs "
                        "fail with `busy` unless args.force is true."));
        t.set("inputSchema", schema(std::move(props), {"command"}));
        tools.push(std::move(t));
    }
    V r = V::object();
    r.set("tools", std::move(tools));
    return r;
}

bool served(const Context& ctx, const CommandInfo& c) {
    if (!c.local.empty()) return static_cast<bool>(ctx.run_local);
    const std::string_view name = c.name;
    return ctx.commands.empty() || std::find(ctx.commands.begin(), ctx.commands.end(), name) != ctx.commands.end();
}

V call_search(const Context& ctx, const V& args) {
    const std::string query = args.str_or("query");
    const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(args.int_or("limit", 20), 1, 200));
    const auto found = search(query, limit, ctx.commands);
    V list = V::array();
    for (const CommandInfo* c : found) list.push(command_json(*c));
    V out = V::object();
    out.set("commands", std::move(list));
    if (found.empty()) out.set("hint", V::string("nothing matched; try other words or an empty query for the full list"));
    return text_result(out.dump(false), false);
}

// A local command: the CLI's own output. With --json the CLI prints {ok, data} or {ok, error}
// as a wire response does; otherwise its text.
V call_local(const Context& ctx, const CommandInfo& info, const V& args) {
    auto argv = local_argv(info, args);
    if (!argv) return text_result(argv.error().what, true);
    auto run = ctx.run_local(*argv, info.timeout_ms);
    if (!run) return text_result("cannot run keepterm: " + run.error().what, true);
    std::string out = run->out, err = run->err;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
    if (run->timed_out) return text_result("timeout: `" + info.cli + "` ran longer than " + std::to_string(info.timeout_ms / 1000) + " s", true);
    if (info.json) {
        if (auto v = json::parse(out); v && v->kind() == V::Kind::Object && v->get("ok")) {
            if (v->bool_or("ok", false)) {
                const V* d = v->get("data");
                return text_result(d ? d->dump(false) : std::string("{}"), false);
            }
            const V* e = v->get("error");
            return text_result(e ? e->str_or("code") + ": " + e->str_or("message") : std::string("failed"), true);
        }
    }
    std::string text = run->exit_code == 0 ? out : (err.empty() ? out : (out.empty() ? err : out + "\n" + err));
    if (run->truncated) text += "\n[output cut]";
    if (run->exit_code != 0) return text_result("exit " + std::to_string(run->exit_code) + (text.empty() ? "" : ": " + text), true);
    if (text.empty()) text = err.empty() ? "ok" : err;   // some commands report on stderr only
    return text_result(std::move(text), false);
}

V call_invoke(const Context& ctx, const V& args) {
    static std::atomic<std::uint64_t> counter{0};
    const std::string command = args.str_or("command");
    if (command.empty()) return text_result("name the command (args.command); `search` lists them", true);
    const CommandInfo* info = nullptr;
    for (const CommandInfo& c : command_catalog())
        if (c.name == command) info = &c;
    if (!info || !served(ctx, *info)) return text_result("unknown command \"" + command + "\"; `search` lists them", true);
    V a = V::object();
    if (const V* given = args.get("args"); given && !given->is_null()) {
        if (given->kind() != V::Kind::Object) return text_result("args must be an object of named arguments", true);
        a = *given;
    }
    if (!info->local.empty()) return call_local(ctx, *info, a);
    if (info->pane_target && !a.get("pane") && !ctx.default_pane.empty()) a.set("pane", V::string(ctx.default_pane));
    V req = V::object();
    req.set("id", V::string("mcp-" + std::to_string(++counter)));
    req.set("command", V::string(command));
    req.set("needs", V::integer(1));
    req.set("args", std::move(a));
    const V resp = ctx.handle ? ctx.handle(req) : error_response(req.str_or("id"), "internal", "no handler");
    if (resp.bool_or("ok", false)) {
        const V* d = resp.get("data");
        return text_result(d ? d->dump(false) : std::string("{}"), false);
    }
    const V* e = resp.get("error");
    std::string text = e ? e->str_or("code") + ": " + e->str_or("message") : std::string("failed");
    if (const V* d = resp.get("data"); d && !d->is_null()) text += "\n" + d->dump(false);
    return text_result(std::move(text), true);
}

}  // namespace

Result<std::vector<std::string>> local_argv(const CommandInfo& c, const V& args) {
    std::vector<std::string> out = c.local, tail;
    for (const auto& [k, v] : args.members()) {
        if (std::none_of(c.args.begin(), c.args.end(), [&](const ArgInfo& a) { return a.name == k; }))
            return make_error(Errc::invalid_argument, "`" + c.name + "` has no argument \"" + k + "\"; `search` lists its args");
    }
    for (const ArgInfo& a : c.args) {
        const V* v = args.get(a.name);
        if (!v || v->is_null()) {
            if (a.required) return make_error(Errc::invalid_argument, "`" + c.name + "` needs \"" + a.name + "\"");
            continue;
        }
        std::vector<std::string> values;
        if (a.type == "boolean") {
            if (v->kind() != V::Kind::Bool) return make_error(Errc::invalid_argument, "\"" + a.name + "\" is a boolean");
            const bool on = *v->as_bool();
            if (on != a.negate) out.push_back(a.flag);
            continue;
        }
        auto scalar = [&](const V& x) -> std::optional<std::string> {
            if (const std::string* s = x.as_string()) return *s;
            if (x.kind() != V::Kind::Number) return std::nullopt;
            if (auto i = x.as_int()) return std::to_string(*i);
            return x.dump(false);
        };
        if (a.type == "array") {
            const auto* items = v->as_array();
            if (!items) return make_error(Errc::invalid_argument, "\"" + a.name + "\" is an array");
            for (const V& x : *items) {
                auto s = scalar(x);
                if (!s) return make_error(Errc::invalid_argument, "\"" + a.name + "\" holds strings or numbers");
                values.push_back(*s);
            }
        } else {
            auto s = scalar(*v);
            if (!s) return make_error(Errc::invalid_argument, "\"" + a.name + "\" is a " + a.type);
            if (a.type == "integer" && !v->as_int())
                return make_error(Errc::invalid_argument, "\"" + a.name + "\" is an integer");
            values.push_back(*s);
        }
        for (std::string& x : values) {
            if (a.flag.empty()) {
                // A positional word must not read as an option.
                if (x.starts_with("-")) return make_error(Errc::invalid_argument, "\"" + a.name + "\" cannot start with '-'");
                out.push_back(std::move(x));
            } else if (a.flag == "--") {
                tail.push_back(std::move(x));
            } else {
                out.push_back(a.flag);
                out.push_back(std::move(x));
            }
        }
    }
    if (c.json) out.emplace_back("--json");
    if (!tail.empty()) {
        out.emplace_back("--");
        for (std::string& x : tail) out.push_back(std::move(x));
    }
    return out;
}

std::vector<const CommandInfo*> search(std::string_view query, std::size_t limit, const std::vector<std::string>& available) {
    const std::vector<std::string> q = words_of(query);
    std::vector<std::pair<int, const CommandInfo*>> hits;
    for (const CommandInfo& c : command_catalog()) {
        // Local commands run without the app, so they are there whatever the app serves.
        if (!available.empty() && c.local.empty() && std::find(available.begin(), available.end(), c.name) == available.end()) continue;
        const int s = q.empty() ? 1 : score(c, q);
        if (s > 0) hits.emplace_back(s, &c);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const CommandInfo*> out;
    for (const auto& h : hits) {
        if (out.size() >= limit) break;
        out.push_back(h.second);
    }
    return out;
}

std::string handle_message(std::string_view line, const Context& ctx) {
    auto parsed = json::parse(line);
    if (!parsed) return rpc_error(V::null(), -32700, "parse error").dump(false);
    const V& m = *parsed;
    if (m.kind() == V::Kind::Array) return rpc_error(V::null(), -32600, "batches are not supported").dump(false);
    if (m.kind() != V::Kind::Object) return rpc_error(V::null(), -32600, "not a JSON-RPC message").dump(false);
    const V* idp = m.get("id");
    const std::string* method = m.get("method") ? m.get("method")->as_string() : nullptr;
    if (!method) return {};                       // a response from the client: nothing to answer
    if (!idp || idp->is_null()) return {};        // a notification (initialized, cancelled, ...)
    const V id = *idp;
    const V* pp = m.get("params");
    const V params = pp && pp->kind() == V::Kind::Object ? *pp : V::object();

    if (*method == "initialize") {
        const std::string want = params.str_or("protocolVersion");
        std::string version(kProtocolVersions[0]);
        for (std::string_view v : kProtocolVersions)
            if (v == want) version = want;
        V caps = V::object();
        V tools = V::object();
        tools.set("listChanged", V::boolean(false));
        caps.set("tools", std::move(tools));
        V info = V::object();
        info.set("name", V::string("keepterm"));
        info.set("title", V::string("keepterm terminal"));
        info.set("version", V::string(ctx.app_version));
        V r = V::object();
        r.set("protocolVersion", V::string(version));
        r.set("capabilities", std::move(caps));
        r.set("serverInfo", std::move(info));
        r.set("instructions",
              V::string("Controls the keepterm terminal this agent runs in. Use `search` to find a command (empty query: all of "
                        "them), then `invoke` it with its args. Pane commands default to the agent's own pane."));
        return rpc_result(id, std::move(r)).dump(false);
    }
    if (*method == "ping") return rpc_result(id, V::object()).dump(false);
    if (*method == "tools/list") return rpc_result(id, tools_list()).dump(false);
    if (*method == "tools/call") {
        const std::string name = params.str_or("name");
        const V* ap = params.get("arguments");
        const V args = ap && ap->kind() == V::Kind::Object ? *ap : V::object();
        if (name == "search") return rpc_result(id, call_search(ctx, args)).dump(false);
        if (name == "invoke") return rpc_result(id, call_invoke(ctx, args)).dump(false);
        return rpc_error(id, -32602, "unknown tool \"" + name + "\" (the tools are search and invoke)").dump(false);
    }
    return rpc_error(id, -32601, "method not found: " + *method).dump(false);
}

}  // namespace kt::app::control::mcp
