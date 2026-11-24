// Host spawning and the session CLI.
//
//   keepterm --session list [--json]
//   keepterm --session kill <id>
// Helpers for scripts and the integration tests:
//   keepterm --session new [--cwd D] [--size CxR] [--no-history] [--pty=P] [--restore-from ID]
//                          [--host-opt X]... [-- program args...]          prints the id
//   keepterm --session dump <id> [--scrollback]                            screen as text
//   keepterm --session send <id> <text>       \n \r \t \e \\ \xHH escapes
//   keepterm --session wait <id> <text> [timeout-ms]                      until the screen shows text
//   keepterm --session info <id>
//   keepterm --session restore <dead-id> [--cwd D]                         prints the new id
//   keepterm --session attach <id> [--size CxR] [--send TEXT] [--ready H]  a GUI-role client that
//                          stays attached until killed or the shell ends (kill -9 client tests)

#include "session/spawn.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>

#include "session/client.h"
#include "session/history.h"
#include "session/os.h"
#include "session/registry.h"
#include "vt/dump.h"
#include "vt/terminal.h"

namespace kt::session {

Result<SpawnedHost> spawn_host(const HostSpawnOptions& o, std::chrono::milliseconds timeout) {
    auto paths = os::paths();
    if (!paths) return Unexpected(paths.error());
    auto id = SessionId::generate();
    if (!id) return Unexpected(id.error());
    const std::string exe = o.exe.empty() ? os::self_exe() : o.exe;
    if (exe.empty()) return make_error(Errc::unavailable, "cannot find the keepterm executable");
    std::vector<std::string> args = {exe, "--session-host", id->str(), "--ready", "{ready}"};
    if (o.restore_from) {
        args.push_back("--restore-from");
        args.push_back(o.restore_from->str());
    }
    if (!o.cwd.empty()) {
        args.push_back("--cwd");
        args.push_back(o.cwd);
    }
    args.push_back("--size");
    args.push_back(std::to_string(o.size.cols) + "x" + std::to_string(o.size.rows));
    if (!o.history) args.push_back("--no-history");
    if (!o.pty.empty()) args.push_back("--pty=" + o.pty);
    for (const auto& e : o.extra) args.push_back(e);
    if (!o.argv.empty()) {
        args.push_back("--");
        for (const auto& a : o.argv) args.push_back(a);
    }
    auto sp = os::spawn_detached(args, timeout);
    if (!sp) return Unexpected(sp.error());
    if (sp->ready_line.rfind("OK", 0) != 0) {
        return make_error(Errc::unavailable, "session host failed: " + sp->ready_line);
    }
    SpawnedHost h;
    h.id = *id;
    h.endpoint = os::endpoint_name(*paths, *id);
    h.pid = sp->pid;
    h.may_end_with_gui = sp->may_end_with_gui;
    return h;
}

namespace {

int fail(const std::string& msg, int code = 1) {
    std::fprintf(stderr, "keepterm: %s\n", msg.c_str());
    return code;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out.push_back(s[i]);
            continue;
        }
        const char c = s[++i];
        switch (c) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'e': out.push_back('\x1b'); break;
            case '\\': out.push_back('\\'); break;
            case 'x':
                if (i + 2 < s.size()) {
                    const std::string h = s.substr(i + 1, 2);
                    out.push_back(static_cast<char>(std::strtol(h.c_str(), nullptr, 16)));
                    i += 2;
                }
                break;
            default: out.push_back(c);
        }
    }
    return out;
}

Result<std::unique_ptr<vt::Terminal>> fetch_terminal(const std::string& endpoint) {
    auto snap = request_snapshot(endpoint);
    if (!snap) return Unexpected(snap.error());
    return vt::Terminal::restore(snap->first, vt::TerminalOptions{}, nullptr);
}

std::string screen_text(const vt::Terminal& t, bool scrollback) {
    vt::DumpOptions d;
    d.attrs = false;
    std::string s = scrollback ? vt::dump_scrollback(t) : std::string();
    s += vt::dump_screen(t, d);
    return s;
}

Result<std::string> endpoint_of(const os::Paths& p, const std::string& id_text) {
    auto id = SessionId::parse(id_text);
    if (!id) return make_error(Errc::invalid_argument, "bad session id");
    return os::endpoint_name(p, *id);
}

bool parse_size(const std::string& s, proto::Resize& r) {
    const auto x = s.find('x');
    if (x == std::string::npos) return false;
    r.cols = static_cast<uint16_t>(std::strtoul(s.c_str(), nullptr, 10));
    r.rows = static_cast<uint16_t>(std::strtoul(s.c_str() + x + 1, nullptr, 10));
    return r.cols > 1 && r.rows > 0;
}

json::Value list_sessions_impl(const os::Paths& p) {
    Registry reg(p);
    (void)reg.sweep_stale();
    auto live = reg.discover();
    auto out = json::Value::array();
    std::set<std::string> live_ids;
    for (const auto& d : live) {
        live_ids.insert(d.record.id.str());
        auto v = json::Value::object();
        v.set("session", json::Value::string(d.record.id.str()));
        v.set("state", json::Value::string(std::string(to_string(d.state))));
        v.set("pid", json::Value::integer(d.record.host.pid));
        if (auto rec = read_recovery_record(os::join(p.state_sessions, d.record.id.str()))) {
            v.set("cwd", json::Value::string(rec->cwd));
            v.set("cwd_source", json::Value::string(rec->cwd_source));
            v.set("title", json::Value::string(rec->title));
        }
        if (d.hello) v.set("gui_clients", json::Value::integer(d.hello->gui_clients()));
        out.push(std::move(v));
    }
    // Dead sessions with history: restorable.
    for (const std::string& name : os::list_dir(p.state_sessions)) {
        auto id = SessionId::parse(name);
        if (!id || live_ids.count(name)) continue;
        auto rec = read_recovery_record(os::join(p.state_sessions, name));
        auto v = json::Value::object();
        v.set("session", json::Value::string(name));
        v.set("state", json::Value::string(rec && rec->exit_code ? "ended" : "dead"));
        if (rec) {
            v.set("cwd", json::Value::string(rec->cwd));
            v.set("cwd_source", json::Value::string(rec->cwd_source));
            v.set("title", json::Value::string(rec->title));
            if (rec->exit_code) v.set("exit_code", json::Value::integer(*rec->exit_code));
        }
        out.push(std::move(v));
    }
    return out;
}

int cmd_list(const os::Paths& p, bool as_json) {
    auto out = list_sessions(p);
    if (as_json) {
        std::printf("%s\n", out.dump(true).c_str());
        return 0;
    }
    std::printf("%-16s  %-12s  %7s  %s\n", "SESSION", "STATE", "PID", "CWD");
    for (const auto& v : *out.as_array()) {
        std::printf("%-16s  %-12s  %7lld  %s\n", v.str_or("session").c_str(), v.str_or("state").c_str(),
                    static_cast<long long>(v.int_or("pid", 0)), v.str_or("cwd").c_str());
    }
    return 0;
}

Result<void> end_session_impl(const os::Paths& p, const SessionId& sid) {
    const SessionId* id = &sid;
    Registry reg(p);
    const std::string dir = os::join(p.state_sessions, id->str());
    auto d = reg.find(*id);
    bool done = false;
    if (d && d->state == HostState::Live) {
        proto::Kill k;
        k.mode = proto::KillMode::KillTree;
        auto r = request_kill(d->record.endpoint, k);
        if (r && r->ok) {
            // The host deletes its folder and record when the shell is gone.
            for (int i = 0; i < 60; ++i) {
                if (!os::process_matches(d->record.host.pid, d->record.host.start_time)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            done = true;
        }
    }
    if (d && !done && d->record.host.pid && os::process_matches(d->record.host.pid, d->record.host.start_time)) {
        if (auto r = os::kill_process(d->record.host.pid, d->record.host.start_time); !r) {
            return make_error(Errc::os_error, "cannot end the session host: " + r.error().what);
        }
        done = true;
    }
    reg.remove(*id);
#ifndef _WIN32
    (void)os::remove_file(os::endpoint_name(p, *id));
#endif
    const bool had_dir = os::is_dir(dir);
    (void)os::remove_tree(dir);
    if (!done && !had_dir && !d) return make_error(Errc::invalid_argument, "no such session: " + id->str());
    return {};
}

int cmd_kill(const os::Paths& p, const std::string& id_text) {
    auto id = SessionId::parse(id_text);
    if (!id) return fail("bad session id", 2);
    if (auto r = end_session(p, *id); !r) return fail(r.error().what);
    return 0;
}

}  // namespace

json::Value list_sessions(const os::Paths& p) { return list_sessions_impl(p); }
Result<void> end_session(const os::Paths& p, const SessionId& id) { return end_session_impl(p, id); }

Result<std::string> session_text(const os::Paths& p, const SessionId& id, bool scrollback) {
    auto t = fetch_terminal(os::endpoint_name(p, id));
    if (!t) return Unexpected(t.error());
    return screen_text(**t, scrollback);
}

Result<void> send_session_text(const os::Paths& p, const SessionId& id, std::string_view text) {
    auto c = Conn::connect(os::endpoint_name(p, id), 3000);
    if (!c) return Unexpected(c.error());
    if (auto h = (*c)->client_handshake(make_hello(proto::Role::Cli, std::nullopt), 3000); !h) return Unexpected(h.error());
    std::vector<std::byte> out;
    proto::Attach a;
    a.mode = proto::AttachMode::Control;
    a.client_name = "keepterm-cli";
    proto::encode(a, out);
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    for (std::size_t off = 0; off < bytes.size(); off += proto::kOutputChunk)
        proto::encode(proto::Input{bytes.subspan(off, std::min<std::size_t>(proto::kOutputChunk, bytes.size() - off))}, out);
    proto::encode_empty(proto::Type::Detach, out);
    if (auto r = (*c)->send(out); !r) return Unexpected(r.error());
    // The host closes after DETACH, once the input is queued.
    for (;;) {
        auto f = (*c)->recv(3000);
        if (!f || !*f) break;
    }
    return {};
}

int cli_main(int argc, char** argv, int i) {
    if (i + 1 >= argc) return fail("usage: keepterm --session list|kill <id>", 2);
    const std::string cmd = argv[i + 1];
    i += 2;
    auto paths = os::paths();
    if (!paths) return fail(paths.error().what);
    const os::Paths& p = *paths;

    if (cmd == "list") {
        const bool js = i < argc && std::string(argv[i]) == "--json";
        return cmd_list(p, js);
    }
    if (cmd == "kill") {
        if (i >= argc) return fail("usage: keepterm --session kill <id>", 2);
        return cmd_kill(p, argv[i]);
    }
    if (cmd == "new" || cmd == "restore") {
        HostSpawnOptions o;
        o.size = proto::Resize{80, 24, 0, 0};
        if (cmd == "restore") {
            if (i >= argc) return fail("usage: keepterm --session restore <id>", 2);
            auto old = SessionId::parse(argv[i++]);
            if (!old) return fail("bad session id", 2);
            o.restore_from = *old;
        }
        for (; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--cwd" && i + 1 < argc) {
                o.cwd = argv[++i];
            } else if (a == "--size" && i + 1 < argc) {
                if (!parse_size(argv[++i], o.size)) return fail("bad --size", 2);
            } else if (a == "--no-history") {
                o.history = false;
            } else if (a.rfind("--pty=", 0) == 0) {
                o.pty = a.substr(6);
            } else if (a == "--restore-from" && i + 1 < argc) {
                auto old = SessionId::parse(argv[++i]);
                if (!old) return fail("bad session id", 2);
                o.restore_from = *old;
            } else if (a == "--host-opt" && i + 1 < argc) {
                o.extra.emplace_back(argv[++i]);
            } else if (a == "--") {
                for (++i; i < argc; ++i) o.argv.emplace_back(argv[i]);
                break;
            } else {
                return fail("unknown option " + a, 2);
            }
        }
        auto h = spawn_host(o);
        if (!h) return fail(h.error().what);
        std::printf("%s\n", h->id.str().c_str());
        return 0;
    }
    if (i >= argc) return fail("usage: keepterm --session " + cmd + " <id> ...", 2);
    auto ep = endpoint_of(p, argv[i]);
    if (!ep) return fail(ep.error().what, 2);
    ++i;
    if (cmd == "dump") {
        const bool sb = i < argc && std::string(argv[i]) == "--scrollback";
        auto t = fetch_terminal(*ep);
        if (!t) return fail(t.error().what);
        const std::string s = screen_text(**t, sb);
        std::fwrite(s.data(), 1, s.size(), stdout);
        return 0;
    }
    if (cmd == "send") {
        if (i >= argc) return fail("usage: keepterm --session send <id> <text>", 2);
        auto id = SessionId::parse(argv[i - 1]);
        if (!id) return fail("bad session id", 2);
        if (auto r = send_session_text(p, *id, unescape(argv[i])); !r) return fail(r.error().what);
        return 0;
    }
    if (cmd == "wait") {
        if (i >= argc) return fail("usage: keepterm --session wait <id> <text> [timeout-ms]", 2);
        const std::string needle = unescape(argv[i]);
        const long ms = i + 1 < argc ? std::strtol(argv[i + 1], nullptr, 10) : 10000;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::string last;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto t = fetch_terminal(*ep)) {
                last = screen_text(**t, true);
                if (last.find(needle) != std::string::npos) return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fwrite(last.data(), 1, last.size(), stderr);
        return fail("timeout waiting for the text");
    }
    if (cmd == "attach") {
        struct Events final : ClientEvents {
            std::mutex mu;
            std::condition_variable cv;
            bool snap = false, done = false;
            int code = 0;
            void on_snapshot(std::vector<std::byte>, uint64_t, bool) override {
                std::lock_guard lk(mu);
                snap = true;
                cv.notify_all();
            }
            void on_output(uint64_t, std::span<const std::byte>) override {}
            void on_exit(const proto::ExitStatus& e) override {
                std::lock_guard lk(mu);
                code = e.code;
                done = true;
                cv.notify_all();
            }
            void on_disconnected(const Error&) override {
                std::lock_guard lk(mu);
                code = 1;
                done = true;
                cv.notify_all();
            }
        } ev;
        AttachOptions o;
        o.role = proto::Role::Gui;
        o.client_name = "keepterm-cli-attach";
        o.pane_closed_on_exit = false;
        int64_t ready = -1;
        std::string text;
        for (; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--size" && i + 1 < argc) {
                if (!parse_size(argv[++i], o.size)) return fail("bad --size", 2);
            } else if (a == "--send" && i + 1 < argc) {
                text = unescape(argv[++i]);
            } else if (a == "--ready" && i + 1 < argc) {
                ready = std::strtoll(argv[++i], nullptr, 10);
            } else {
                return fail("unknown option " + a, 2);
            }
        }
        auto c = SessionClient::attach(*ep, o, ev);
        if (!c) {
            os::signal_ready(ready, "ERR " + c.error().what);
            return fail(c.error().what);
        }
        {
            std::unique_lock lk(ev.mu);
            ev.cv.wait_for(lk, std::chrono::seconds(5), [&] { return ev.snap || ev.done; });
        }
        if (!text.empty()) (void)(*c)->write_input(std::as_bytes(std::span(text.data(), text.size())));
        os::signal_ready(ready, "OK " + std::to_string(os::current_pid()));
        std::unique_lock lk(ev.mu);
        ev.cv.wait(lk, [&] { return ev.done; });
        return ev.code;
    }
    if (cmd == "info") {
        auto pi = request_process_info(*ep);
        if (!pi) return fail(pi.error().what);
        std::printf("host_pid %u\nroot_pid %u\nforeground_pid %u\ncwd %s\ncwd_source %s\nbackend %s\nshell %s\n",
                    pi->host_pid, pi->root_pid, pi->foreground_pid, pi->cwd.c_str(), pi->cwd_source.c_str(),
                    pi->backend.c_str(), pi->shell_kind.c_str());
        return 0;
    }
    if (cmd == "context") {
        auto c = request_agent_context(*ep);
        if (!c) return fail(c.error().what);
        auto opt = [](const std::optional<uint64_t>& v) { return v ? std::to_string(*v) : std::string("-"); };
        std::printf("status %u\nagent %s\nsession %s\nmodel %s\nused %s\nwindow %s\npercent_x100 %s\n"
                    "sources used=%u window=%u limits=%u\ninput %llu output %llu cache_read %llu cache_write %llu\n"
                    "turns %u compactions %u api_calls %u activity %u flags %u\n",
                    static_cast<unsigned>(c->status), c->agent.c_str(), c->session_id.c_str(), c->model.c_str(),
                    opt(c->used_tokens).c_str(), opt(c->window_tokens).c_str(),
                    c->percent_x100 ? std::to_string(*c->percent_x100).c_str() : "-", c->used_source, c->window_source,
                    c->limits_source, static_cast<unsigned long long>(c->input_tokens),
                    static_cast<unsigned long long>(c->output_tokens), static_cast<unsigned long long>(c->cache_read_tokens),
                    static_cast<unsigned long long>(c->cache_write_tokens), c->turns, c->compactions, c->api_calls, c->activity,
                    c->flags);
        for (const auto& l : c->limits)
            std::printf("limit %s %.2f%% window=%um resets=%llu\n", l.name.c_str(), l.used_percent_x100 / 100.0, l.window_minutes,
                        static_cast<unsigned long long>(l.resets_at_unix));
        return 0;
    }
    return fail("unknown session command " + cmd, 2);
}

}  // namespace kt::session
