// The keymap: bindings from the per-OS defaults overlaid with `[keys]`,
// conflict detection and the reserved-chord rules.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "commands/context.h"
#include "commands/keys.h"
#include "commands/registry.h"
#include "config/config.h"

namespace kt::commands {

struct BindingOrigin {
    enum class Kind : std::uint8_t { Default, User } kind = Kind::Default;
    std::string file;   // display form (User)
    int line = 0, column = 0;
};

struct Binding {
    Sequence seq;
    CommandId command;                 // points into the registry (static ids)
    const Command* cmd = nullptr;      // resolved command
    CommandArgs args;
    Ctx when = Ctx::Always;            // narrows the command's contexts
    Ctx effective = Ctx::None;         // when & command.contexts (Always → the command's)
    bool global = false;               // OS-wide hotkey
    bool passthrough = false;          // yields to keys.passthrough_programs
    bool hidden = false;               // an alias chord left out of key caps and docs columns
    BindingOrigin origin;
    bool user() const { return origin.kind == BindingOrigin::Kind::User; }
};

struct Conflict {
    std::size_t winner = 0, loser = 0;   // binding indices; the winner fires where both apply
    bool shadowed_default = false;       // a user binding took a default's chord (Info, not Warning)
    std::string message;                 // English: "ctrl+shift+d: Split Right conflicts with ..."
};

struct KeymapLoad;

class Keymap {
public:
    Keymap() = default;

    static Keymap defaults_for(Os os, const CommandRegistry& r = builtin_registry());
    // Defaults overlaid with user bindings. A user entry replaces every default binding of its
    // command ("none" unbinds). Problems are Warning diagnostics with file and line; a binding
    // that does not parse is dropped and that command keeps its defaults.
    static KeymapLoad from_config(const config::KeysTable& t, std::span<const std::string> passthrough_programs,
                                  Os os, const CommandRegistry& r = builtin_registry());
    static KeymapLoad from_config(const config::Config& c, Os os, const CommandRegistry& r = builtin_registry());

    Os os() const { return os_; }
    std::span<const Binding> bindings() const { return bindings_; }
    std::span<const Conflict> conflicts() const { return conflicts_; }
    std::span<const std::string> passthrough_programs() const { return programs_; }
    // For key caps in menus and the palette, the Keymaps list and the CLI. Registry order of
    // the defaults; hidden alias chords last.
    std::vector<const Binding*> bindings_for(CommandId id) const;
    // First visible binding without args (or with `args` when given): the key caps of a row.
    std::optional<Sequence> key_caps(CommandId id) const;

    // Binding indices whose first chord is `c`, in resolution order (user first, then the
    // more specific context, then registry order). Null when none. No allocation.
    const std::vector<std::uint32_t>* candidates(const Chord& c) const;
    bool has_sequences() const { return has_sequences_; }

    // Building (tests and the loader).
    void add(Binding b);
    void set_passthrough_programs(std::vector<std::string> p) { programs_ = std::move(p); }
    void finalize();   // sorts, indexes, computes conflicts

private:
    Os os_ = current_os();
    std::vector<Binding> bindings_;
    std::vector<Conflict> conflicts_;
    std::vector<std::string> programs_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> index_;
    bool has_sequences_ = false;
};

struct KeymapLoad {
    Keymap keymap;
    std::vector<config::Diagnostic> diagnostics;
};

// The reserved-chord rules: chords that terminal programs need. Settings → Keymaps warns
// before binding one in a terminal context.
bool is_reserved_chord(const Chord& c, Os os);
// The documented exceptions that defaults may use anyway.
bool is_documented_exception(const Chord& c, Os os);
// "Terminal programs use Ctrl+R (for example reverse search in shells). Bind anyway?"
std::string reserved_warning(const Chord& c, Os os);

// Two effective contexts can be active at once and neither is a modal override of the other
// (Palette, FindBar, CopyMode, TextInput and Sidebar bindings override the base contexts
// Always, Window, Terminal and QuickTerminal by design).
bool contexts_conflict(Ctx a, Ctx b);

// Foreground program matching for passthrough bindings: executable name without directory;
// on Windows without extension and case-insensitive.
bool program_matches(std::string_view foreground, std::string_view listed, Os os);

}  // namespace kt::commands
