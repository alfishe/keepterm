#include "commands/cli.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

#include "commands/keymap.h"
#include "commands/keymap_doc.h"
#include "commands/registry.h"

namespace kt::commands {

namespace {

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out + "\"";
}

void put(std::string_view s, FILE* f = stdout) { std::fwrite(s.data(), 1, s.size(), f); }

int fail(const std::string& msg, int code = 2) {
    put("keepterm --command: " + msg + "\n", stderr);
    return code;
}

int list(const CommandRegistry& r, Os os, bool json) {
    const Keymap km = Keymap::defaults_for(os, r);
    ICommandTarget none;
    CommandContext ctx{none, nullptr, {}, InvokeSource::Cli, os, false};
    std::string out;
    if (json) out += "[\n";
    bool first = true;
    for (const Command& c : r.all()) {
        std::vector<std::string> keys;
        for (const Binding* b : km.bindings_for(c.id)) keys.push_back(format_sequence(b->seq, os, Format::Config));
        if (json) {
            const Availability av = r.availability(c, ctx);
            if (!first) out += ",\n";
            first = false;
            out += "  {\"id\": " + json_string(c.id) + ", \"title_en\": " + json_string(c.title) +
                   ", \"category\": " + json_string(category_title(c.category)) + ", \"bindings\": [";
            for (std::size_t i = 0; i < keys.size(); ++i) out += (i ? ", " : "") + json_string(keys[i]);
            out += "], \"enabled\": ";
            out += av.enabled ? "true" : "false";
            out += ", \"reason\": " + json_string(av.reason) + ", \"palette\": ";
            out += c.palette_visible ? "true" : "false";
            out += ", \"destructive\": ";
            out += c.destructive ? "true" : "false";
            out += "}";
        } else {
            std::string line = std::string(c.id);
            if (line.size() < 32) line.resize(32, ' ');
            line += " " + std::string(c.title);
            if (!keys.empty()) {
                if (line.size() < 72) line.resize(72, ' ');
                line += " ";
                for (std::size_t i = 0; i < keys.size() && i < 3; ++i) line += (i ? ", " : "") + keys[i];
                if (keys.size() > 3) line += ", ...";
            }
            out += line + "\n";
        }
    }
    if (json) out += "\n]\n";
    put(out);
    return 0;
}

config::RawValue parse_value(std::string_view v) {
    if (v == "true") return config::RawValue{true};
    if (v == "false") return config::RawValue{false};
    bool digits = !v.empty();
    for (std::size_t i = 0; i < v.size(); ++i)
        if (!(v[i] >= '0' && v[i] <= '9') && !(i == 0 && v[i] == '-' && v.size() > 1)) digits = false;
    if (digits && v.size() < 18) return config::RawValue{static_cast<std::int64_t>(std::stoll(std::string(v)))};
    return config::RawValue{std::string(v)};
}

int run(const CommandRegistry& r, int argc, char** argv, int i, const RunForwarder& forward) {
    if (i >= argc) return fail("run needs a command id");
    RunRequest req;
    req.id = argv[i++];
    const Command* c = r.find(req.id);
    if (!c) {
        const std::string hint = r.suggest(req.id);
        return fail("unknown command '" + req.id + "'" + (hint.empty() ? "" : "; " + hint));
    }
    for (; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--force") {
            req.force = true;
        } else if (a == "--json") {
            req.json = true;
        } else if (a == "--pane") {
            if (i + 1 >= argc) return fail("--pane needs a pane id");
            req.pane = argv[++i];
        } else if (a.starts_with("--pane=")) {
            req.pane = std::string(a.substr(7));
        } else {
            req.words.emplace_back(a);
        }
    }
    if (auto v = parse_run_args(*c, req.words); !v) return fail(v.error().what);
    if (c->destructive && !req.force) return fail("'" + req.id + "' is destructive; add --force");
    if (forward) return forward(req);
    return fail("no running keepterm to run '" + req.id + "' in", 1);
}

}  // namespace

Result<CommandArgs> parse_run_args(const Command& c, std::span<const std::string> words) {
    CommandArgs args;
    for (const std::string& w : words) {
        const std::string_view a = w;
        const auto eq = a.find('=');
        if (eq == std::string_view::npos || eq == 0)
            return make_error(Errc::invalid_argument, "argument '" + w + "' is not name=value");
        args.set(a.substr(0, eq), parse_value(a.substr(eq + 1)));
    }
    if (auto v = validate_args(c.args, args); !v) return make_error(Errc::invalid_argument, v.error().what);
    return args;
}

const char* command_cli_usage() {
    return "       keepterm --command list [--json] [--os macos|windows|linux]\n"
           "       keepterm --command run <id> [name=value]... [--pane ID] [--force] [--json]\n"
           "       keepterm --command keymap   (the default keymaps of every OS, Markdown)\n";
}

int command_cli(int argc, char** argv, int first, const RunForwarder& forward) {
    const CommandRegistry& r = builtin_registry();
    if (first >= argc) return fail("expected list, run or keymap");
    const std::string_view sub = argv[first];
    if (sub == "run") return run(r, argc, argv, first + 1, forward);
    if (sub == "keymap") {
        if (first + 1 != argc) return fail("keymap takes no arguments");
        put(generate_keymap_markdown(r));
        return 0;
    }
    if (sub == "list") {
        Os os = current_os();
        bool json = false;
        for (int i = first + 1; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (a == "--json") {
                json = true;
            } else if (a == "--os" && i + 1 < argc) {
                const auto o = parse_os(argv[++i]);
                if (!o) return fail("unknown OS '" + std::string(argv[i]) + "'");
                os = *o;
            } else {
                return fail("unknown option '" + std::string(a) + "'");
            }
        }
        return list(r, os, json);
    }
    return fail("unknown subcommand '" + std::string(sub) + "'; expected list, run or keymap");
}

}  // namespace kt::commands
