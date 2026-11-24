#include "app/control.h"
#include "app/mcp.h"

#include <thread>

#include "session/suspend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "session/os.h"

namespace kt::app::control {

namespace {

void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}
void put_u16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}
std::uint32_t get_u32(std::span<const std::byte> b) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | std::to_integer<std::uint32_t>(b[static_cast<std::size_t>(i)]);
    return v;
}

}  // namespace

std::vector<std::byte> encode_frame(const Frame& f) {
    std::vector<std::byte> out;
    out.reserve(6 + f.payload.size());
    put_u32(out, static_cast<std::uint32_t>(2 + f.payload.size()));
    put_u16(out, static_cast<std::uint16_t>(f.type));
    for (char c : f.payload) out.push_back(static_cast<std::byte>(c));
    return out;
}

Result<std::optional<std::pair<Frame, std::size_t>>> decode_frame(std::span<const std::byte> buf) {
    if (buf.size() < 4) return std::optional<std::pair<Frame, std::size_t>>{};
    const std::uint32_t len = get_u32(buf);
    if (len < 2 || len > kMaxFrame) return make_error(Errc::corrupt, "bad control frame length");
    if (buf.size() < 4u + len) return std::optional<std::pair<Frame, std::size_t>>{};
    Frame f;
    f.type = static_cast<FrameType>(std::to_integer<std::uint16_t>(buf[4]) | (std::to_integer<std::uint16_t>(buf[5]) << 8));
    f.payload.assign(reinterpret_cast<const char*>(buf.data() + 6), len - 2);
    return std::optional<std::pair<Frame, std::size_t>>{std::pair{std::move(f), 4u + len}};
}

Result<void> write_frame(session::ipc::Stream& s, const Frame& f) {
    const auto bytes = encode_frame(f);
    return s.write_all(bytes);
}

Result<Frame> read_frame(session::ipc::Stream& s, std::vector<std::byte>& pending, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::byte buf[16384];
    for (;;) {
        auto d = decode_frame(pending);
        if (!d) return Unexpected<Error>(d.error());
        if (*d) {
            Frame f = std::move((*d)->first);
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>((*d)->second));
            return f;
        }
        int left = -1;
        if (timeout_ms >= 0) {
            left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
            if (left <= 0) return make_error(Errc::timeout, "no answer in time");
        }
        auto n = s.read(buf, left);
        if (!n) return Unexpected<Error>(n.error());
        if (*n == 0) return make_error(Errc::lost, "connection closed");
        pending.insert(pending.end(), buf, buf + *n);
    }
}

Result<std::string> endpoint() {
    auto p = session::os::paths();
    if (!p) return Unexpected<Error>(p.error());
    return session::os::control_endpoint_name(*p);
}

int exit_code_for(std::string_view code) {
    if (code.empty()) return 0;
    if (code == kUsage || code == kTargetRequired) return 2;
    if (code == kNotRunning) return 3;
    if (code == kUnsupportedVersion) return 4;
    if (code == kTimeout) return 5;
    return 1;
}

json::Value ok_response(const std::string& id, json::Value data) {
    json::Value r = json::Value::object();
    r.set("id", json::Value::string(id));
    r.set("ok", json::Value::boolean(true));
    r.set("data", std::move(data));
    return r;
}

json::Value error_response(const std::string& id, std::string_view code, std::string message) {
    json::Value e = json::Value::object();
    e.set("code", json::Value::string(std::string(code)));
    e.set("message", json::Value::string(std::move(message)));
    json::Value r = json::Value::object();
    r.set("id", json::Value::string(id));
    r.set("ok", json::Value::boolean(false));
    r.set("error", std::move(e));
    return r;
}

// ---------------------------------------------------------------- server

Result<std::unique_ptr<Server>> Server::start(const std::string& ep, Info info, Handler handle) {
#ifndef _WIN32
    const auto slash = ep.find_last_of('/');
    if (slash != std::string::npos && slash > 0) (void)session::os::make_private_dir(ep.substr(0, slash));
#endif
    auto l = session::ipc::listen(ep, 16);
    if (!l) return Unexpected<Error>(l.error());
    std::unique_ptr<Server> s(new Server());
    s->listener_ = std::move(*l);
    s->info_ = std::move(info);
    s->handle_ = std::move(handle);
    s->accept_ = std::thread([raw = s.get()] { raw->accept_loop(); });
    return s;
}

Server::~Server() { stop(); }

void Server::stop() {
    if (stopping_.exchange(true)) return;
    if (listener_) listener_->close();
    if (accept_.joinable()) accept_.join();
    std::unique_lock lk(mu_);
    for (auto& s : streams_) s->shutdown();
    idle_.wait(lk, [this] { return active_ == 0; });
}

void Server::accept_loop() {
    while (!stopping_) {
        auto s = listener_->accept();
        if (!s) {
            if (stopping_ || s.error().code == Errc::lost) return;
            continue;
        }
        std::shared_ptr<session::ipc::Stream> stream(std::move(*s));
        std::lock_guard lk(mu_);
        if (stopping_) {
            stream->shutdown();
            return;
        }
        streams_.push_back(stream);
        ++active_;
        // Detached; stop() shuts the stream down and waits for active_ to reach 0.
        std::thread([this, stream] {
            serve(*stream);
            stream->shutdown();
            std::lock_guard lk2(mu_);
            std::erase(streams_, stream);
            if (--active_ == 0) idle_.notify_all();
        }).detach();
    }
}

void Server::serve(session::ipc::Stream& s) {
    std::vector<std::byte> pending;
    auto hello = read_frame(s, pending, 2000);
    if (!hello || hello->type != FrameType::Hello) return;
    auto hv = json::parse(hello->payload);
    const json::Value h = hv ? *hv : json::Value::object();
    const auto pmin = static_cast<std::uint16_t>(h.uint_or("proto_min", 1));
    const auto pmax = static_cast<std::uint16_t>(h.uint_or("proto_max", 1));
    const std::uint16_t proto = std::min(pmax, kProtoVersion);
    const bool agreed = proto >= pmin && proto >= kProtoMin;
    json::Value w = json::Value::object();
    w.set("proto", json::Value::integer(agreed ? proto : 0));
    w.set("app_version", json::Value::string(info_.app_version));
    w.set("pid", json::Value::integer(session::os::current_pid()));
    json::Value cmds = json::Value::array();
    for (const auto& c : info_.commands) cmds.push(json::Value::string(c));
    w.set("commands", std::move(cmds));
    if (!write_frame(s, Frame{FrameType::Welcome, w.dump(false)}) || !agreed) return;
    // An MCP bridge (src/mcp_bridge) says which pane it runs in; pane commands default to it.
    mcp::Context mc;
    mc.app_version = info_.app_version;
    mc.commands = info_.commands;
    mc.default_pane = h.str_or("pane");
    mc.handle = [this](const json::Value& req) { return handle_(req); };
    while (!stopping_) {
        auto f = read_frame(s, pending, -1);
        if (!f || f->type == FrameType::Bye) return;
        if (f->type == FrameType::Mcp) {
            if (!write_frame(s, Frame{FrameType::Mcp, mcp::handle_message(f->payload, mc)})) return;
            continue;
        }
        if (f->type != FrameType::Request) continue;
        auto rv = json::parse(f->payload);
        json::Value resp;
        if (!rv || rv->kind() != json::Value::Kind::Object) {
            resp = error_response("", "bad_request", "the request is not a JSON object");
        } else if (rv->uint_or("needs", 1) > proto) {
            resp = error_response(rv->str_or("id"), kUnsupportedVersion, "the app is older than this command needs");
        } else {
            resp = handle_(*rv);
        }
        if (!write_frame(s, Frame{FrameType::Response, resp.dump(false)})) return;
    }
}

// ---------------------------------------------------------------- client

Result<json::Value> request(const std::string& ep, const std::string& command, json::Value args, int timeout_ms) {
    auto c = session::ipc::connect(ep, 1000);
    if (!c) return Unexpected<Error>(c.error());
    session::ipc::Stream& s = **c;
    json::Value h = json::Value::object();
    h.set("proto_min", json::Value::integer(kProtoMin));
    h.set("proto_max", json::Value::integer(kProtoVersion));
    h.set("client", json::Value::string("cli"));
    if (auto r = write_frame(s, Frame{FrameType::Hello, h.dump(false)}); !r) return Unexpected<Error>(r.error());
    std::vector<std::byte> pending;
    auto w = read_frame(s, pending, 2000);
    if (!w) return Unexpected<Error>(w.error());
    auto wv = json::parse(w->payload);
    if (!wv || wv->int_or("proto", 0) == 0) return make_error(Errc::not_supported, "the app speaks another control protocol version");
    json::Value req = json::Value::object();
    req.set("id", json::Value::string("1"));
    req.set("command", json::Value::string(command));
    req.set("needs", json::Value::integer(1));
    req.set("args", std::move(args));
    if (auto r = write_frame(s, Frame{FrameType::Request, req.dump(false)}); !r) return Unexpected<Error>(r.error());
    auto resp = read_frame(s, pending, timeout_ms);
    if (!resp) return Unexpected<Error>(resp.error());
    (void)write_frame(s, Frame{FrameType::Bye, {}});
    s.shutdown();
    auto rv = json::parse(resp->payload);
    if (!rv) return Unexpected<Error>(rv.error());
    return *rv;
}

Result<RunArgs> parse_run_args(std::span<const std::string_view> args) {
    RunArgs r;
    std::vector<std::string> words;
    bool rest = false;
    std::string tail;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (rest) {
            if (!tail.empty()) tail += ' ';
            tail += a;
            continue;
        }
        if (a == "--") {
            rest = true;
        } else if (a == "--no-submit") {
            r.submit = false;
        } else if (a == "--pane") {
            if (i + 1 >= args.size()) return make_error(Errc::invalid_argument, "--pane needs a pane id");
            r.pane = std::string(args[++i]);
        } else if (a.starts_with("-")) {
            return make_error(Errc::invalid_argument,
                              "\"" + std::string(a) + "\" looks like an option; put the text after -- (keepterm pane run -- " +
                                  std::string(a) + " ...)");
        } else {
            words.emplace_back(a);
        }
    }
    for (const auto& w : words) {
        if (!r.text.empty()) r.text += ' ';
        r.text += w;
    }
    if (!tail.empty()) r.text += (r.text.empty() ? "" : " ") + tail;
    if (r.text.empty() && r.submit) return make_error(Errc::invalid_argument, "nothing to type");
    return r;
}

// ---------------------------------------------------------------- CLI

namespace {

int cli_usage(FILE* out) {
    std::fprintf(out, "usage: keepterm status [--json]\n"
               "       keepterm pane list [--json]\n"
               "       keepterm pane run [--pane ID] [--no-submit] [words...] [-- text...]\n"
               "       keepterm pane dump [--pane ID] [--scrollback] [--lines N] [--json]\n"
               "       keepterm exit [--force] [--json]      end keepterm and every session (saved for the next start)\n"
               "       keepterm restart [--force] [--json]   the same, then start keepterm again\n"
               "%s"
               "Talks to the running keepterm window. Inside a keepterm pane, --pane defaults to that pane.\n",
               table_usage().c_str());
    return out == stderr ? 2 : 0;
}

std::optional<std::string> env_pane() {
    std::string v = session::os::getenv_utf8("KEEPTERM_PANE");
    if (v.empty()) return std::nullopt;
    return v;
}

}  // namespace

// Prints the response; returns the exit code.
int finish_response(const Result<json::Value>& r, bool as_json, const std::function<void(const json::Value&)>& print) {
    if (!r) {
        const bool absent = r.error().code == Errc::unavailable || r.error().code == Errc::os_error || r.error().code == Errc::lost;
        const std::string_view code = r.error().code == Errc::timeout ? kTimeout : absent ? kNotRunning : kInternal;
        if (as_json) {
            std::printf("%s\n", error_response("1", code, r.error().what).dump(false).c_str());
        } else {
            if (code == kNotRunning)
                std::fprintf(stderr, "keepterm: no running keepterm window found\n");
            else
                std::fprintf(stderr, "keepterm: %s\n", r.error().what.c_str());
        }
        return exit_code_for(code);
    }
    const json::Value& v = *r;
    if (as_json) {
        std::printf("%s\n", v.dump(true).c_str());
    } else if (v.bool_or("ok", false)) {
        if (const json::Value* d = v.get("data")) print(*d);
    } else if (const json::Value* e = v.get("error")) {
        std::fprintf(stderr, "keepterm: %s\n", e->str_or("message").c_str());
    }
    if (v.bool_or("ok", false)) return 0;
    const json::Value* e = v.get("error");
    return exit_code_for(e ? e->str_or("code", kInternal) : std::string(kInternal));
}

namespace {

// keepterm exit / restart. With a window: it checks, saves and ends everything; we wait for it
// to go. Without one: the session hosts are ended from here (and a restart starts a window).
int cli_exit(const std::string& ep, bool restart, const std::vector<std::string_view>& args) {
    bool force = false, as_json = false;
    for (std::size_t k = 1; k < args.size(); ++k) {
        if (args[k] == "--force") force = true;
        else if (args[k] == "--json") as_json = true;
        else return cli_usage(stderr);
    }
    json::Value a = json::Value::object();
    a.set("force", json::Value::boolean(force));
    auto r = request(ep, restart ? "app.restart" : "app.exit", std::move(a), 15000);
    const bool absent = !r && (r.error().code == Errc::unavailable || r.error().code == Errc::os_error || r.error().code == Errc::lost);
    if (absent) {
        auto paths = session::os::paths();
        if (!paths) {
            std::fprintf(stderr, "keepterm: %s\n", paths.error().what.c_str());
            return 1;
        }
        const session::SuspendReport rep = session::suspend_all_hosts(*paths, session::os::getenv_utf8("KEEPTERM_SESSION"));
        std::optional<uint32_t> started;
        std::string launch_error;
        if (restart) {
            if (auto l = launch_gui({"--gui"})) started = *l;
            else launch_error = l.error().what;
        }
        if (as_json) {
            json::Value d = json::Value::object();
            d.set("window", json::Value::boolean(false));
            d.set("saved", json::Value::integer(static_cast<int64_t>(rep.saved)));
            d.set("ended", json::Value::integer(static_cast<int64_t>(rep.killed)));
            d.set("still_running", json::Value::integer(static_cast<int64_t>(rep.failed)));
            if (started) d.set("pid", json::Value::integer(*started));
            std::printf("%s\n", (launch_error.empty() ? ok_response("1", std::move(d)) : error_response("1", kInternal, launch_error))
                                    .dump(true)
                                    .c_str());
        } else {
            std::printf("keepterm: no window; %zu sessions saved and ended", rep.saved + rep.killed);
            if (rep.failed) std::printf(", %zu still running", rep.failed);
            std::printf("\n");
            if (!launch_error.empty()) std::fprintf(stderr, "keepterm: %s\n", launch_error.c_str());
        }
        return !launch_error.empty() || rep.failed ? 1 : 0;
    }
    if (!r || !r->bool_or("ok", false)) return finish_response(r, as_json, [](const json::Value&) {});
    const json::Value* d = r->get("data");
    const auto pid = static_cast<uint32_t>(d ? d->int_or("pid", 0) : 0);
    const uint64_t start = pid ? session::os::process_start_time(pid) : 0;
    if (!as_json) std::printf("keepterm: %s\n", restart ? "restarting" : "exiting");
    std::fflush(stdout);
    // Inside a pane this process ends with its session before the window does.
    bool gone = !pid || !start;
    for (int waited = 0; !gone && waited < 600; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        gone = !session::os::process_matches(pid, start);
    }
    if (as_json) std::printf("%s\n", r->dump(true).c_str());
    if (!gone) {
        std::fprintf(stderr, "keepterm: the window did not end in 30 s\n");
        return exit_code_for(kTimeout);
    }
    return 0;
}

}  // namespace

int cli_command_run(const std::string& id, const std::vector<std::string>& words, std::string pane, bool force,
                    bool as_json) {
    auto ep = endpoint();
    if (!ep) {
        std::fprintf(stderr, "keepterm: %s\n", ep.error().what.c_str());
        return 1;
    }
    if (pane.empty()) pane = session::os::getenv_utf8("KEEPTERM_PANE");
    json::Value a = json::Value::object();
    a.set("id", json::Value::string(id));
    json::Value w = json::Value::array();
    for (const std::string& s : words) w.push(json::Value::string(s));
    a.set("args", std::move(w));
    if (!pane.empty()) a.set("pane", json::Value::string(pane));
    if (force) a.set("force", json::Value::boolean(true));
    return finish_response(request(*ep, "command.run", std::move(a), 10000), as_json, [](const json::Value&) {});
}

int cli_main(int argc, char** argv, int i) {
    if (i >= argc) return cli_usage(stderr);
    std::vector<std::string_view> args(argv + i, argv + argc);
    const std::string_view noun = args[0];
    auto ep = endpoint();
    if (!ep) {
        std::fprintf(stderr, "keepterm: %s\n", ep.error().what.c_str());
        return 1;
    }
    auto has = [&](std::string_view flag) { return std::find(args.begin(), args.end(), flag) != args.end(); };
    const bool as_json = has("--json");
    if (noun == "--help" || noun == "help") return cli_usage(stdout);
    if (noun == "status") {
        return finish_response(request(*ep, "status", json::Value::object(), 10000), as_json, [](const json::Value& d) {
            std::printf("keepterm %s (pid %lld): %lld projects, %lld tabs, %lld panes\n", d.str_or("app_version").c_str(),
                        static_cast<long long>(d.int_or("pid", 0)), static_cast<long long>(d.int_or("projects", 0)),
                        static_cast<long long>(d.int_or("tabs", 0)), static_cast<long long>(d.int_or("panes", 0)));
        });
    }
    if (noun == "exit" || noun == "restart") return cli_exit(*ep, noun == "restart", args);
    if (noun == "help" || noun == "--help") return cli_usage(stdout);
    if (!(noun == "pane" && args.size() >= 2 && (args[1] == "list" || args[1] == "run" || args[1] == "dump")))
        if (auto r = cli_table(*ep, args)) return *r;
    if (noun != "pane" || args.size() < 2) return cli_usage(stderr);
    const std::string_view verb = args[1];
    const std::span<const std::string_view> rest(args.data() + 2, args.size() - 2);
    if (verb == "list") {
        return finish_response(request(*ep, "pane.list", json::Value::object(), 10000), as_json, [](const json::Value& d) {
            const json::Value* panes = d.get("panes");
            if (!panes || !panes->as_array()) return;
            for (const json::Value& p : *panes->as_array()) {
                std::printf("%s%s  %s  %s  %s  %s\n", p.bool_or("focused", false) ? "*" : " ", p.str_or("id").c_str(),
                            p.str_or("project").c_str(), p.str_or("title").c_str(), p.str_or("program").c_str(),
                            p.str_or("cwd").c_str());
            }
        });
    }
    if (verb == "run") {
        auto ra = parse_run_args(rest);
        if (!ra) {
            std::fprintf(stderr, "keepterm: %s\n", ra.error().what.c_str());
            return 2;
        }
        if (!ra->pane) ra->pane = env_pane();
        if (!ra->pane) {
            std::fprintf(stderr, "keepterm: name the pane with --pane ID (keepterm pane list shows them)\n");
            return 2;
        }
        json::Value a = json::Value::object();
        a.set("pane", json::Value::string(*ra->pane));
        a.set("text", json::Value::string(ra->text));
        a.set("submit", json::Value::boolean(ra->submit));
        return finish_response(request(*ep, "pane.run", std::move(a), 10000), as_json, [](const json::Value&) {});
    }
    if (verb == "dump") {
        json::Value a = json::Value::object();
        std::optional<std::string> pane;
        for (std::size_t k = 0; k < rest.size(); ++k) {
            if (rest[k] == "--pane" && k + 1 < rest.size()) {
                pane = std::string(rest[++k]);
            } else if (rest[k] == "--scrollback") {
                a.set("scrollback", json::Value::boolean(true));
            } else if (rest[k] == "--lines" && k + 1 < rest.size()) {
                a.set("lines", json::Value::integer(std::strtoll(std::string(rest[++k]).c_str(), nullptr, 10)));
            } else if (rest[k] != "--json") {
                return cli_usage(stderr);
            }
        }
        if (!pane) pane = env_pane();
        if (!pane) {
            std::fprintf(stderr, "keepterm: name the pane with --pane ID (keepterm pane list shows them)\n");
            return 2;
        }
        a.set("pane", json::Value::string(*pane));
        return finish_response(request(*ep, "pane.dump", std::move(a), 10000), as_json,
                      [](const json::Value& d) { std::fputs((d.str_or("text") + "\n").c_str(), stdout); });
    }
    return cli_usage(stderr);
}

}  // namespace kt::app::control
