// The control channel to the running app: `keepterm status`, `pane`, `project`,
// `tab`, `window`, `session`, `agent`, `layout`, `exit`, `restart` talk to the GUI over a
// per-user endpoint (a 0600 unix socket in the runtime folder, a named pipe with the user's
// DACL on Windows; the session transports with their peer-user check).
//
// Wire: u32 length (LE, counts type + payload), u16 type (LE), UTF-8 JSON.
// HELLO {proto_min, proto_max, client} -> WELCOME {proto, app_version, pid, commands[]}, then
// REQUEST {id, command, needs, args} -> RESPONSE {id, ok, data | error {code, message}}.
//
// The command table and the GUI-less commands (session *, layout show/validate, completion) are
// in cli_commands.cpp. Not here yet: events, `__complete`, resources, config.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/error.h"
#include "session/ipc.h"
#include "session/json.h"

namespace kt::app::control {

namespace json = session::json;

inline constexpr std::uint16_t kProtoVersion = 1;
inline constexpr std::uint16_t kProtoMin = 1;
inline constexpr std::uint32_t kMaxFrame = 4u << 20;   // the JSON parser reads up to 1 MiB of text

// Mcp: one JSON-RPC 2.0 message of the Model Context Protocol each way (src/app/mcp.h); the
// answer to a notification is an empty Mcp frame.
enum class FrameType : std::uint16_t {
    Hello = 0x0001,
    Welcome = 0x0002,
    Request = 0x0010,
    Response = 0x0011,
    Mcp = 0x0020,
    Bye = 0x00FF
};

struct Frame {
    FrameType type = FrameType::Bye;
    std::string payload;
};

std::vector<std::byte> encode_frame(const Frame&);
// Decodes one frame from the front of `buf`: the frame and the bytes it used, nullopt when more
// bytes are needed. A length above kMaxFrame or below 2 is an error.
Result<std::optional<std::pair<Frame, std::size_t>>> decode_frame(std::span<const std::byte> buf);

Result<void> write_frame(session::ipc::Stream&, const Frame&);
Result<Frame> read_frame(session::ipc::Stream&, std::vector<std::byte>& pending, int timeout_ms);

// Prints a response (human text through `print`, or the JSON); returns the exit code.
int finish_response(const Result<json::Value>& r, bool as_json, const std::function<void(const json::Value&)>& print);
// The table-driven commands (cli_commands.cpp): project, tab, pane, window, session, agent,
// layout, completion. nullopt: not one of them.
std::optional<int> cli_table(const std::string& ep, std::span<const std::string_view> args);
std::string table_usage();

// The control endpoint of this user and profile.
Result<std::string> endpoint();

// Control protocol error codes used here and their exit codes.
inline constexpr std::string_view kNotFound = "not_found", kTargetRequired = "target_required", kUsage = "usage",
                                  kUnknownCommand = "unknown_command", kUnsupportedVersion = "unsupported_version",
                                  kInternal = "internal", kNotRunning = "not_running", kTimeout = "timeout",
                                  kBusy = "busy", kUnavailable = "unavailable";
int exit_code_for(std::string_view code);

json::Value ok_response(const std::string& id, json::Value data);
json::Value error_response(const std::string& id, std::string_view code, std::string message);

// A new GUI of this executable with `args` (after the program), detached from the caller (its
// own session on POSIX, out of the caller's job on Windows when allowed). Its pid.
Result<uint32_t> launch_gui(const std::vector<std::string>& args);

// The GUI side. `handle` runs on a connection thread with the request object and returns the
// response object (the GUI posts to its UI thread and waits).
class Server {
public:
    using Handler = std::function<json::Value(const json::Value& request)>;
    struct Info {
        std::string app_version;
        std::vector<std::string> commands;
    };
    static Result<std::unique_ptr<Server>> start(const std::string& endpoint, Info info, Handler handle);
    ~Server();
    void stop();   // closes the endpoint and every connection, joins the threads; idempotent

private:
    Server() = default;
    void accept_loop();
    void serve(session::ipc::Stream& s);

    std::unique_ptr<session::ipc::Listener> listener_;
    Info info_;
    Handler handle_;
    std::thread accept_;
    std::mutex mu_;
    std::condition_variable idle_;
    std::vector<std::shared_ptr<session::ipc::Stream>> streams_;   // the open connections
    int active_ = 0;                                                // their (detached) threads
    std::atomic<bool> stopping_{false};
};

// The commands agents can find and call (MCP `search` / `invoke`): every wire
// command with the keys of its request `args`. The CLI table's commands plus the hand-parsed ones.
struct ArgInfo {
    std::string name;
    std::string type;   // "string", "boolean", "integer", "number", "array"
    std::string help;
    bool required = false;
};
struct CommandInfo {
    std::string name;   // the wire command ("pane.split")
    std::string cli;    // "keepterm pane split [--direction X] ..."
    std::string help;
    std::vector<ArgInfo> args;
    bool pane_target = false;   // `pane` defaults to the caller's pane
};
const std::vector<CommandInfo>& command_catalog();

// The CLI side: one request, the response object or a transport error.
Result<json::Value> request(const std::string& endpoint, const std::string& command, json::Value args, int timeout_ms);

// `keepterm pane run` argument rules: flags parse anywhere before "--"; plain
// words are typed joined by spaces; any other word starting with '-' is a usage error; after
// "--" everything is text.
struct RunArgs {
    std::optional<std::string> pane;   // --pane ID (else KEEPTERM_PANE)
    bool submit = true;                // --no-submit: no Enter after the text
    std::string text;
};
Result<RunArgs> parse_run_args(std::span<const std::string_view> args);

// `keepterm command run <id>` after the CLI checked it: sends command.run. An empty
// pane falls back to KEEPTERM_PANE, then to the window's focused pane.
int cli_command_run(const std::string& id, const std::vector<std::string>& words, std::string pane, bool force,
                    bool as_json);

// `keepterm status | pane list | pane run | pane dump` (argv[i] is the command word).
int cli_main(int argc, char** argv, int i);

}  // namespace kt::app::control
