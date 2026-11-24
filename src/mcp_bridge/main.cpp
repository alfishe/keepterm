// keepterm-mcp: the stdio MCP server an agent's client starts. It holds no
// protocol logic: each line of stdin (one JSON-RPC message) goes to the running keepterm as one
// Mcp frame over the control channel, and the answer, when there is one, is printed as one line.
// The MCP server itself (initialize, tools/list, the `search` and `invoke` tools) is in the app,
// src/app/mcp.cpp.
//
//   .mcp.json: { "mcpServers": { "keepterm": { "command": "/path/to/keepterm-mcp" } } }
//
// The endpoint is --endpoint X, else $KEEPTERM_CONTROL (set in every keepterm pane), else the
// default of the user's profile. Each message gets its own connection, so a restarted keepterm
// is found again and a lost answer is never sent twice. When keepterm cannot be reached, a
// request gets a JSON-RPC error (-32603) with its id and the bridge keeps running; notifications
// stay silent. EOF on stdin ends the bridge with 0.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#endif

#include "session/ipc.h"
#include "session/json.h"
#include "session/os.h"

namespace {

using namespace kt;
namespace json = session::json;

constexpr std::uint16_t kHello = 0x0001, kWelcome = 0x0002, kMcp = 0x0020, kBye = 0x00FF;
constexpr std::uint32_t kMaxFrame = 4u << 20;

void put_frame(std::vector<std::byte>& out, std::uint16_t type, const std::string& payload) {
    const auto len = static_cast<std::uint32_t>(2 + payload.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((len >> (8 * i)) & 0xFF));
    out.push_back(static_cast<std::byte>(type & 0xFF));
    out.push_back(static_cast<std::byte>(type >> 8));
    for (char c : payload) out.push_back(static_cast<std::byte>(c));
}

// One frame; nullopt on a transport error or a bad length.
std::optional<std::pair<std::uint16_t, std::string>> get_frame(session::ipc::Stream& s, std::vector<std::byte>& pending, int timeout_ms) {
    std::byte buf[16384];
    for (;;) {
        if (pending.size() >= 6) {
            std::uint32_t len = 0;
            for (int i = 0; i < 4; ++i) len |= std::to_integer<std::uint32_t>(pending[static_cast<std::size_t>(i)]) << (8 * i);
            if (len < 2 || len > kMaxFrame) return std::nullopt;
            if (pending.size() >= 4u + len) {
                const auto type = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(pending[4]) | (std::to_integer<std::uint16_t>(pending[5]) << 8));
                std::string payload(reinterpret_cast<const char*>(pending.data() + 6), len - 2);
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(4u + len));
                return std::pair{type, std::move(payload)};
            }
        }
        auto n = s.read(buf, timeout_ms);
        if (!n || *n == 0) return std::nullopt;
        pending.insert(pending.end(), buf, buf + *n);
    }
}

// A pane id is a UUID; anything else is not passed on.
std::string pane_of_env() {
    std::string p = session::os::getenv_utf8("KEEPTERM_PANE");
    for (char c : p)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-')) return {};
    return p.size() <= 64 ? p : std::string();
}

// The answer to one message, or the reason there is none.
struct Answer {
    std::optional<std::string> text;
    std::string error;
};

Answer forward(const std::string& endpoint, const std::string& hello, const std::string& line) {
    auto c = session::ipc::connect(endpoint, 2000);
    if (!c) return {std::nullopt, c.error().what};
    session::ipc::Stream& s = **c;
    std::vector<std::byte> out;
    put_frame(out, kHello, hello);
    put_frame(out, kMcp, line);
    if (auto w = s.write_all(out); !w) return {std::nullopt, w.error().what};
    std::vector<std::byte> pending;
    auto welcome = get_frame(s, pending, 5000);
    if (!welcome || welcome->first != kWelcome) return {std::nullopt, "no answer to the handshake"};
    // The app answers within its own time limit; a closed window ends the connection.
    auto reply = get_frame(s, pending, -1);
    if (!reply || reply->first != kMcp) return {std::nullopt, "the connection closed before the answer"};
    std::vector<std::byte> bye;
    put_frame(bye, kBye, {});
    (void)s.write_all(bye);
    s.shutdown();
    return {std::move(reply->second), {}};
}

void emit(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::string endpoint;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc) {
            endpoint = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("usage: keepterm-mcp [--endpoint PATH]\n"
                        "The MCP server (stdio) of the running keepterm: tools `search` and `invoke`.\n");
            return 0;
        } else {
            std::fprintf(stderr, "keepterm-mcp: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (endpoint.empty()) endpoint = session::os::getenv_utf8("KEEPTERM_CONTROL");
    if (endpoint.empty()) {
        if (auto p = session::os::paths()) endpoint = session::os::control_endpoint_name(*p);
    }
    json::Value h = json::Value::object();
    h.set("proto_min", json::Value::integer(1));
    h.set("proto_max", json::Value::integer(1));
    h.set("client", json::Value::string("mcp-bridge"));
    if (const std::string pane = pane_of_env(); !pane.empty()) h.set("pane", json::Value::string(pane));
    const std::string hello = h.dump(false);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        Answer a = endpoint.empty() ? Answer{std::nullopt, "no control endpoint"} : forward(endpoint, hello, line);
        if (a.text) {
            if (!a.text->empty()) emit(*a.text);
            continue;
        }
        // Not reachable: a request still gets an answer (with its id, so the client does not
        // wait), a notification none.
        auto m = json::parse(line);
        const json::Value* id = m && m->kind() == json::Value::Kind::Object ? m->get("id") : nullptr;
        if (m && m->kind() == json::Value::Kind::Object && (!id || id->is_null() || !m->get("method"))) continue;
        json::Value e = json::Value::object();
        e.set("code", json::Value::integer(-32603));
        e.set("message", json::Value::string("keepterm is not running or not reachable (" + a.error + ")"));
        json::Value r = json::Value::object();
        r.set("jsonrpc", json::Value::string("2.0"));
        r.set("id", id ? *id : json::Value::null());
        r.set("error", std::move(e));
        emit(r.dump(false));
    }
    return 0;
}
