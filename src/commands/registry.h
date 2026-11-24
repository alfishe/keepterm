// The command registry: every action a user can invoke.
//
// The palette, the menus, Settings → Keymaps and the CLI all render from it; adding an action
// means adding one Command (catalog.cpp). UI thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/error.h"
#include "commands/context.h"
#include "commands/target.h"
#include "config/value.h"

namespace kt::config {
class Config;
}

namespace kt::commands {

using CommandId = std::string_view;   // "pane.split_right"; stable, English (config, CLI, layouts)

enum class Category : std::uint8_t { Palettes, Tabs, Panes, Terminal, Projects, Window, App };
std::string_view category_title(Category c);   // English, "Palettes"

struct Availability {
    bool enabled = true;
    std::string reason;                 // palette subtitle, menu tooltip, toast
    bool notify_when_invoked = false;   // a chord on a disabled command shows a toast
};

// Arguments of parameterized commands: a table of TOML-like values ({ index = 3 }), as the
// config file writes them in `[keys]` (`args = {...}`) and the CLI passes them (`key=value`).
struct CommandArgs {
    config::RawValue value{config::RawValue::Table{}};

    bool empty() const;
    const config::RawValue* get(std::string_view name) const;
    std::optional<std::int64_t> get_int(std::string_view name) const;
    std::optional<std::string_view> get_string(std::string_view name) const;
    void set(std::string_view name, config::RawValue v);
};

enum class ArgType : std::uint8_t { Int, String, Chord, Id };
struct ArgField {
    std::string_view name;
    ArgType type = ArgType::String;
    bool required = true;
    std::int64_t min = 0, max = 0;   // Int only; max 0 = unbounded
    std::string_view doc;
};
using ArgSpec = std::vector<ArgField>;

struct CommandContext {
    explicit CommandContext(ICommandTarget& t) : target(t) {}
    CommandContext(ICommandTarget& t, const config::Config* c, KeyContext k, InvokeSource s, Os o = current_os(),
                   bool f = false)
        : target(t), config(c), keys(k), source(s), os(o), force(f) {}

    ICommandTarget& target;
    const config::Config* config = nullptr;
    KeyContext keys;
    InvokeSource source = InvokeSource::Keymap;
    Os os = current_os();
    bool force = false;   // CLI --force: destructive commands skip their confirmation
};

struct Command {
    CommandId id;
    std::string_view title;             // English, Title Case; localized through set_translator
    std::string_view description;       // one line, palette preview and Keymaps tooltip
    Category category = Category::App;
    Ctx contexts = Ctx::Window;         // where its bindings fire
    std::string_view icon;              // icon id
    bool bindable = true;
    bool palette_visible = true;
    bool repeatable = false;            // auto-repeat runs it again (resize, font size, focus, scroll)
    // An unavailable command bound to a chord lets the chord through to the widget instead of
    // swallowing it (Backspace pops a palette scope only when the field is empty).
    bool yields_when_unavailable = false;
    bool destructive = false;           // CLI needs --force
    std::optional<PaletteScope> opens_scope;
    std::function<Availability(const CommandContext&)> availability;   // empty = always enabled
    std::function<Status(CommandContext&, const CommandArgs&)> handler;
    ArgSpec args;
};

using Translator = std::function<std::string(std::string_view english)>;

class CommandRegistry {
public:
    // At startup; ids must be unique (a duplicate is a programming error: asserted, ignored).
    void add(Command c);
    const Command* find(CommandId id) const;
    std::span<const Command> all() const { return commands_; }   // registry order
    std::size_t index_of(CommandId id) const;                    // registry order; npos if absent

    Availability availability(const Command& c, const CommandContext& ctx) const;
    // Validates the arguments, checks availability, runs the handler. A disabled command does
    // nothing and returns Errc::unavailable with the reason; a throwing handler is caught and
    // reported as "Command failed".
    Status invoke(CommandId id, CommandContext& ctx, const CommandArgs& a = {}) const;

    // Localized title; the English title stays a search alias.
    std::string title(const Command& c) const;
    void set_translator(Translator t) { translate_ = std::move(t); }

    // "did you mean 'pane.split_right'?" for an unknown id, or "".
    std::string suggest(std::string_view unknown) const;

private:
    std::vector<Command> commands_;
    std::unordered_map<std::string_view, std::size_t> by_id_;
    Translator translate_;
};

// Checks `a` against `spec`: unknown names, missing required fields, wrong types, ranges.
Status validate_args(const ArgSpec& spec, const CommandArgs& a);

// The built-in catalog, handlers bound to ICommandTarget. Registry order is palette
// and Keymaps order: Palettes first.
void add_builtin_commands(CommandRegistry& r);
const CommandRegistry& builtin_registry();   // a shared instance for the CLI, docs and tests

}  // namespace kt::commands
