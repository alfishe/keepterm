// `keepterm --history list|show|output|restore|rename|tag|untag|pin|unpin|delete`.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include <ctime>
#include <string>
#include <vector>

#include "session/history_export.h"
#include "session/history_index.h"
#include "session/spawn.h"
#include "store/cli.h"
#include "store/file_io.h"

namespace kt::session {

namespace {

int fail(const std::string& msg, int code = 1) {
    std::fprintf(stderr, "keepterm: %s\n", msg.c_str());
    return code;
}

int usage() {
    std::fprintf(stderr,
                 "usage: keepterm --history list [--since T] [--project P] [--agent A] [--tag T] [--state running|closed]\n"
                 "                [--limit N] [--json]\n"
                 "       keepterm --history show <id> [--json]\n"
                 "       keepterm --history output <id> [--format text|raw|asciicast] [-o FILE]\n"
                 "       keepterm --history restore <id> [--resume-agent [N]] [--size COLSxROWS]\n"
                 "       keepterm --history rename <id> <title>\n"
                 "       keepterm --history tag <id> <tag>... | untag <id> <tag>...\n"
                 "       keepterm --history pin <id> | unpin <id>\n"
                 "       keepterm --history delete <id>... [--force]\n"
                 "<id>: a history id or a unique prefix of it (see list)\n");
    return 2;
}

std::string local_time(uint64_t ms) {
    if (!ms) return "-";
    const auto t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
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

json::Value u64(uint64_t v) { return json::Value::integer(static_cast<int64_t>(v)); }

json::Value to_json(const HistoryEntry& e) {
    auto v = json::Value::object();
    v.set("id", json::Value::string(e.id));
    v.set("state", json::Value::string(std::string(to_string(e.state))));
    v.set("session", e.session.empty() ? json::Value::null() : json::Value::string(e.session));
    v.set("title", json::Value::string(e.display_title()));
    v.set("title_auto", json::Value::string(e.title));
    v.set("project", e.project);
    v.set("cwd_first", json::Value::string(e.cwd_first));
    v.set("cwd_last", json::Value::string(e.cwd_last));
    v.set("shell", json::Value::string(e.shell));
    v.set("created_ms", u64(e.created_ms));
    v.set("last_used_ms", u64(e.last_used_ms));
    v.set("ended_ms", e.ended_ms ? u64(*e.ended_ms) : json::Value::null());
    v.set("exit", e.exit_code ? json::Value::integer(*e.exit_code) : json::Value::null());
    v.set("close_reason", e.close_reason.empty() ? json::Value::null() : json::Value::string(e.close_reason));
    v.set("restores", json::Value::integer(static_cast<int64_t>(e.restores.size())));
    auto tags = json::Value::array();
    for (const auto& t : e.tags) tags.push(json::Value::string(t));
    v.set("tags", std::move(tags));
    v.set("pinned", json::Value::boolean(e.pinned));
    auto as = json::Value::array();
    for (const auto& a : e.agents) as.push(to_json(a));
    v.set("agents", std::move(as));
    auto st = json::Value::object();
    st.set("commands", u64(e.stats.commands));
    st.set("failed_commands", u64(e.stats.failed_commands));
    st.set("raw_bytes", u64(e.stats.raw_bytes));
    st.set("text_lines", u64(e.stats.text_lines));
    st.set("stored_bytes", u64(e.stats.stored_bytes));
    v.set("stats", std::move(st));
    v.set("final_screen", json::Value::boolean(e.final_screen));
    return v;
}

bool has_agent(const HistoryEntry& e, const std::string& agent) {
    return std::any_of(e.agents.begin(), e.agents.end(), [&](const AgentRun& a) { return a.agent == agent; });
}

int cmd_list(const os::Paths& p, int argc, char** argv, int i) {
    std::optional<uint64_t> since;
    std::string project, agent, tag, state;
    std::size_t limit = SIZE_MAX;
    bool js = false;
    const uint64_t now = store::now_unix_ms();
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--json") {
            js = true;
        } else if (a == "--since" && has) {
            since = store::parse_time_arg(argv[++i], now);
            if (!since) return fail("bad --since", 2);
        } else if (a == "--project" && has) {
            project = argv[++i];
        } else if (a == "--agent" && has) {
            agent = argv[++i];
        } else if (a == "--tag" && has) {
            tag = argv[++i];
        } else if (a == "--state" && has) {
            state = argv[++i];
            if (state != "running" && state != "closed") return fail("--state is running or closed", 2);
        } else if (a == "--limit" && has) {
            limit = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else {
            return usage();
        }
    }
    std::vector<HistoryEntry> out;
    for (auto& e : list_history(p)) {
        if (since && e.last_used_ms < *since) continue;
        if (!project.empty() && e.project_name() != project && e.project.str_or("id") != project) continue;
        if (!agent.empty() && !has_agent(e, agent)) continue;
        if (!tag.empty() && std::find(e.tags.begin(), e.tags.end(), tag) == e.tags.end()) continue;
        if (!state.empty() && to_string(e.state) != state) continue;
        if (out.size() >= limit) break;
        out.push_back(std::move(e));
    }
    if (js) {
        auto a = json::Value::array();
        for (const auto& e : out) a.push(to_json(e));
        std::printf("%s\n", a.dump(true).c_str());
        return 0;
    }
    std::printf("%-16s  %-7s  %-16s  %5s  %9s  %-12s  %s\n", "ID", "STATE", "LAST USED", "CMDS", "STORED", "PROJECT", "TITLE");
    for (const auto& e : out) {
        std::string title = e.display_title();
        if (e.pinned) title = "[pinned] " + title;
        for (const auto& t : e.tags) title += " #" + t;
        std::printf("%-16s  %-7s  %-16s  %5llu  %9s  %-12s  %s\n", e.id.c_str(), std::string(to_string(e.state)).c_str(),
                    local_time(e.last_used_ms).c_str(), static_cast<unsigned long long>(e.stats.commands),
                    human(e.stats.stored_bytes).c_str(), e.project_name().c_str(), title.c_str());
    }
    return 0;
}

int cmd_show(const os::Paths& p, int argc, char** argv, int i) {
    if (i >= argc) return usage();
    const std::string id = argv[i++];
    bool js = false;
    for (; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") js = true;
        else return usage();
    }
    auto e = find_history(p, id);
    if (!e) return fail(e.error().what);
    const auto cmds = history_commands(p, e->id);
    if (js) {
        auto v = to_json(*e);
        auto cs = json::Value::array();
        for (const auto& c : cmds) {
            auto o = json::Value::object();
            o.set("started_ms", u64(c.started_ms));
            o.set("ended_ms", c.ended_ms ? u64(*c.ended_ms) : json::Value::null());
            o.set("exit", c.exit_code ? json::Value::integer(*c.exit_code) : json::Value::null());
            o.set("cwd", json::Value::string(c.cwd));
            o.set("command", json::Value::string(c.command));
            cs.push(std::move(o));
        }
        v.set("commands", std::move(cs));
        std::printf("%s\n", v.dump(true).c_str());
        return 0;
    }
    std::printf("%s  %s\n", e->id.c_str(), e->display_title().c_str());
    std::printf("  state      %s%s\n", std::string(to_string(e->state)).c_str(),
                e->session.empty() ? "" : (" (session " + e->session + ")").c_str());
    if (!e->project_name().empty()) std::printf("  project    %s\n", e->project_name().c_str());
    std::printf("  folder     %s%s\n", e->cwd_last.c_str(), e->cwd_first != e->cwd_last ? (" (started in " + e->cwd_first + ")").c_str() : "");
    std::printf("  created    %s\n", local_time(e->created_ms).c_str());
    std::printf("  last used  %s\n", local_time(e->last_used_ms).c_str());
    if (e->ended_ms) {
        std::printf("  ended      %s%s\n", local_time(*e->ended_ms).c_str(),
                    e->close_reason.empty() ? "" : (" (" + e->close_reason + ")").c_str());
    }
    if (!e->restores.empty()) std::printf("  restores   %zu\n", e->restores.size());
    if (!e->tags.empty() || e->pinned) {
        std::string t;
        for (const auto& x : e->tags) t += (t.empty() ? "" : ", ") + x;
        std::printf("  tags       %s%s\n", t.c_str(), e->pinned ? (t.empty() ? "pinned" : "; pinned") : "");
    }
    std::printf("  output     %s raw, %s stored\n", human(e->stats.raw_bytes).c_str(), human(e->stats.stored_bytes).c_str());
    for (std::size_t n = 0; n < e->agents.size(); ++n) {
        const auto& a = e->agents[n];
        std::printf("  agent %zu    %s %s, %s%s%s\n", n, a.agent.c_str(), a.conversation.c_str(), local_time(a.started_ms).c_str(),
                    a.ended_ms ? (" - " + local_time(*a.ended_ms)).c_str() : "", a.cwd.empty() ? "" : (" in " + a.cwd).c_str());
    }
    if (!cmds.empty()) {
        std::printf("  commands   %zu\n", cmds.size());
        for (const auto& c : cmds) {
            std::printf("    %s  %4s  %s\n", local_time(c.started_ms).c_str(),
                        c.exit_code ? std::to_string(*c.exit_code).c_str() : "", c.command.c_str());
        }
    }
    return 0;
}

int cmd_output(const os::Paths& p, int argc, char** argv, int i) {
    if (i >= argc) return usage();
    const std::string id = argv[i++];
    ExportFormat f = ExportFormat::text;
    std::string file;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--format" && i + 1 < argc) {
            auto x = export_format_from_string(argv[++i]);
            if (!x) return fail("bad --format: text, raw or asciicast", 2);
            f = *x;
        } else if ((a == "-o" || a == "--output") && i + 1 < argc) {
            file = argv[++i];
        } else {
            return usage();
        }
    }
    auto e = find_history(p, id);
    if (!e) return fail(e.error().what);
    const ExportInfo info{e->display_title(), e->created_ms};
    Result<ExportStats> st;
    if (!file.empty()) {
        st = export_history_to_file(p, e->id, f, info, file);
    } else {
#ifdef _WIN32
        (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
        st = export_history(p, e->id, f, info, [](std::string_view s) {
            return std::fwrite(s.data(), 1, s.size(), stdout) == s.size();
        });
        std::fflush(stdout);
    }
    if (!st) return fail(st.error().what);
    if (f == ExportFormat::asciicast && !st->sized)
        std::fprintf(stderr, "keepterm: %s has no stored terminal size; the recording says 80x24\n", e->id.c_str());
    return 0;
}

int cmd_restore(const os::Paths& p, int argc, char** argv, int i) {
    if (i >= argc) return usage();
    const std::string id = argv[i++];
    std::optional<std::size_t> run;
    HostSpawnOptions o;
    o.size = proto::Resize{80, 24, 0, 0};
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--resume-agent") {
            run = SIZE_MAX;  // the last run
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') run = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--size" && i + 1 < argc) {
            const char* v = argv[++i];
            char* x = nullptr;
            const unsigned long c = std::strtoul(v, &x, 10);
            char* end = nullptr;
            const unsigned long r = *x == 'x' ? std::strtoul(x + 1, &end, 10) : 0;
            if (!c || !r || c > 1000 || r > 1000 || !end || *end) return fail("bad --size", 2);
            o.size.cols = static_cast<uint16_t>(c);
            o.size.rows = static_cast<uint16_t>(r);
        } else {
            return usage();
        }
    }
    auto e = find_history(p, id);
    if (!e) return fail(e.error().what);
    if (run && *run == SIZE_MAX) {
        if (e->agents.empty()) return fail("session " + e->id + " ran no agent");
        run = e->agents.size() - 1;
    }
    if (run && *run >= e->agents.size()) return fail("session " + e->id + " has " + std::to_string(e->agents.size()) + " agent runs");
    auto plan = plan_restore(*e);
    if (!plan) return fail(plan.error().what);
    o.cwd = plan->cwd;
    o.extra = plan->host_args(run);
    auto h = spawn_host(o);
    if (!h) return fail(h.error().what);
    std::printf("%s\n", h->id.str().c_str());
    return 0;
}

int cmd_edit(const os::Paths& p, const std::string& cmd, int argc, char** argv, int i) {
    if (i >= argc) return usage();
    auto e = find_history(p, argv[i++]);
    if (!e) return fail(e.error().what);
    std::vector<std::string> words(argv + i, argv + argc);
    Result<void> r;
    if (cmd == "rename") {
        std::string title;
        for (const auto& w : words) title += (title.empty() ? "" : " ") + w;
        r = set_history_title(p, e->id, title);
    } else if (cmd == "tag" || cmd == "untag") {
        if (words.empty()) return usage();
        r = cmd == "tag" ? tag_history(p, e->id, words, {}) : tag_history(p, e->id, {}, words);
    } else {
        if (!words.empty()) return usage();
        r = pin_history(p, e->id, cmd == "pin");
    }
    if (!r) return fail(r.error().what);
    return 0;
}

int cmd_delete(const os::Paths& p, int argc, char** argv, int i) {
    std::vector<std::string> ids;
    bool force = false;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--force") force = true;
        else ids.push_back(a);
    }
    if (ids.empty()) return usage();
    std::vector<HistoryEntry> doomed;
    for (const auto& id : ids) {
        auto e = find_history(p, id);
        if (!e) return fail(e.error().what);
        if (e->state == HistoryState::running) return fail("session " + e->id + " is running; close it first");
        doomed.push_back(std::move(*e));
    }
    if (!force) {
        uint64_t bytes = 0;
        for (const auto& e : doomed) bytes += e.stats.stored_bytes;
        std::printf("would delete %zu session%s (%s stored); run again with --force\n", doomed.size(),
                    doomed.size() == 1 ? "" : "s", human(bytes).c_str());
        return 1;
    }
    for (const auto& e : doomed) {
        auto r = delete_history(p, e.id);
        if (!r) return fail(e.id + ": " + r.error().what);
        std::printf("deleted %s (%s)\n", e.id.c_str(), human(*r).c_str());
    }
    return 0;
}

}  // namespace

bool is_history_index_command(std::string_view cmd) {
    return cmd == "list" || cmd == "show" || cmd == "output" || cmd == "restore" || cmd == "rename" || cmd == "tag" || cmd == "untag" ||
           cmd == "pin" || cmd == "unpin" || cmd == "delete";
}

int history_index_cli(int argc, char** argv, int first) {
    if (first >= argc) return usage();
    const std::string cmd = argv[first];
    auto paths = os::paths();
    if (!paths) return fail(paths.error().what);
    const int i = first + 1;
    if (cmd == "list") return cmd_list(*paths, argc, argv, i);
    if (cmd == "show") return cmd_show(*paths, argc, argv, i);
    if (cmd == "output") return cmd_output(*paths, argc, argv, i);
    if (cmd == "restore") return cmd_restore(*paths, argc, argv, i);
    if (cmd == "delete") return cmd_delete(*paths, argc, argv, i);
    if (is_history_index_command(cmd)) return cmd_edit(*paths, cmd, argc, argv, i);
    return usage();
}

}  // namespace kt::session
