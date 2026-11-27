// Key contexts: where a command or a binding is active.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kt::commands {

// Built from the focus chain. Bit values are stable (tests and docs use them).
enum class Ctx : std::uint32_t {
    None          = 0,
    Always        = 1u << 0,
    Window        = 1u << 1,   // a main window is key
    Terminal      = 1u << 2,   // a terminal view has focus
    TextInput     = 1u << 3,   // a text field has focus (palette field, rename, find bar, Settings)
    Palette       = 1u << 4,
    FindBar       = 1u << 5,
    CopyMode      = 1u << 6,
    Sidebar       = 1u << 7,   // the sidebar list has focus
    QuickTerminal = 1u << 8,
    HasProject    = 1u << 9,
    HasSplit      = 1u << 10,
    Remote        = 1u << 11,  // later: remote projects
};

constexpr Ctx operator|(Ctx a, Ctx b) { return static_cast<Ctx>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b)); }
constexpr Ctx operator&(Ctx a, Ctx b) { return static_cast<Ctx>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b)); }
constexpr Ctx operator~(Ctx a) { return static_cast<Ctx>(~static_cast<std::uint32_t>(a)); }
constexpr Ctx& operator|=(Ctx& a, Ctx b) { return a = a | b; }
constexpr bool any(Ctx c) { return static_cast<std::uint32_t>(c) != 0; }
constexpr std::uint32_t bits(Ctx c) { return static_cast<std::uint32_t>(c); }

// Focus bits; the rest (HasProject, HasSplit, Remote) are conditions that must all hold.
inline constexpr Ctx kFocusContexts = Ctx::Always | Ctx::Window | Ctx::Terminal | Ctx::TextInput | Ctx::Palette |
                                      Ctx::FindBar | Ctx::CopyMode | Ctx::Sidebar | Ctx::QuickTerminal;
// A binding or command with effective contexts `eff` is active in `ctx`.
constexpr bool context_active(Ctx eff, Ctx ctx) {
    const Ctx have = ctx | Ctx::Always;
    const Ctx focus = eff & kFocusContexts;
    const Ctx cond = eff & ~kFocusContexts;
    if (any(focus) && !any(focus & have)) return false;
    return (cond & have) == cond;
}

// The context bits that say where focus is, most specific first. Condition bits
// (HasProject, HasSplit, Remote) only narrow and never rank.
inline constexpr Ctx kSpecificityOrder[] = {Ctx::Palette,  Ctx::FindBar,       Ctx::CopyMode,
                                            Ctx::TextInput, Ctx::Sidebar,      Ctx::QuickTerminal,
                                            Ctx::Terminal, Ctx::Window,        Ctx::Always};
// 9 for Palette ... 1 for Always, 0 for none of them.
int specificity(Ctx c);
// "Terminal|Window" style, for docs and diagnostics.
std::string format_ctx(Ctx c);
// Parses one context name ("terminal", "find_bar", "palette" ...), case-insensitive.
bool parse_ctx_name(std::string_view name, Ctx& out);

enum class PaletteScope : std::uint8_t { Root, Files, Sessions, Tabs, Folders, MoveTabToProject, Workspaces, History };
std::string_view scope_name(PaletteScope s);   // "files", "sessions", ...

enum class InvokeSource : std::uint8_t { Keymap, Palette, Menu, Cli, Toolbar };

// What the resolver and availability see of the focus state. Filled by the window per key
// event; cheap to copy (views only).
struct KeyContext {
    Ctx bits = Ctx::Always | Ctx::Window;   // Always is implied even when not set
    // Foreground program of the focused pane (session host's report), executable name
    // or path; for `passthrough` bindings. Unknown when the host has not reported yet.
    std::string_view foreground_program;
    bool foreground_known = false;
    std::uint32_t tab_count = 0;   // tabs addressable by tab.select (multi-digit runs)
};

}  // namespace kt::commands
