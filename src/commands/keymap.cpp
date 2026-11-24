#include "commands/keymap.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace kt::commands {

namespace {

constexpr Ctx kFocusMask = kFocusContexts;

// ---------------------------------------------------------------- default keymap
//
// One row per command and context; chords are ", "-separated alternatives in the grammar of
// keys.h. "=" in the Linux column repeats the Windows column. Every chord that a terminal
// program might want is either avoided or listed in is_documented_exception().
struct DefaultRow {
    std::string_view command;
    std::string_view mac, win, linux_;
    std::string_view when = {};   // context name that narrows the command's contexts
    bool global = false;
    bool hidden = false;          // alias chords: bound, but not the key caps
};

constexpr DefaultRow kDefaults[] = {
    {"palette.toggle", "cmd+p", "ctrl+shift+p", "="},
    {"palette.select_next", "down, ctrl+n", "down, ctrl+n", "="},
    {"palette.select_previous", "up, ctrl+p", "up, ctrl+p", "="},
    {"palette.page_down", "pagedown", "pagedown", "="},
    {"palette.page_up", "pageup", "pageup", "="},
    {"palette.accept", "enter", "enter", "="},
    {"palette.accept_alt", "opt+enter", "alt+enter", "="},
    {"palette.enter_scope", "tab", "tab", "="},
    {"palette.pop_scope", "backspace", "backspace", "="},
    {"palette.close", "escape", "escape", "="},

    {"tab.new", "cmd+t", "ctrl+shift+t", "="},
    {"tab.new_scratch", "cmd+shift+t", "ctrl+shift+alt+t", "="},
    {"tab.rename", "cmd+r", "ctrl+shift+r", "="},
    {"tab.next_in_project", "ctrl+]", "ctrl+pagedown", "="},
    {"tab.previous_in_project", "ctrl+[", "ctrl+pageup", "="},
    {"tab.recent", "ctrl+tab", "ctrl+tab", "="},
    // tab.select {1..9}: generated below (cmd+N / alt+N).
    {"tab.move_up", "ctrl+left", "ctrl+shift+pageup", "="},
    {"tab.move_down", "ctrl+right", "ctrl+shift+pagedown", "="},

    {"pane.close", "cmd+w", "ctrl+shift+w", "="},
    {"pane.split_right", "cmd+d", "ctrl+shift+d", "="},
    {"pane.split_down", "cmd+shift+d", "ctrl+shift+alt+d", "="},
    {"pane.zoom", "cmd+shift+enter", "ctrl+shift+enter", "="},
    {"pane.focus_left", "cmd+ctrl+h", "alt+left", "="},
    {"pane.focus_down", "cmd+ctrl+j", "alt+down", "="},
    {"pane.focus_up", "cmd+ctrl+k", "alt+up", "="},
    {"pane.focus_right", "cmd+ctrl+l", "alt+right", "="},
    {"pane.resize_left", "cmd+shift+h", "alt+shift+left", "="},
    {"pane.resize_down", "cmd+shift+j", "alt+shift+down", "="},
    {"pane.resize_up", "cmd+shift+k", "alt+shift+up", "="},
    {"pane.resize_right", "cmd+shift+l", "alt+shift+right", "="},
    {"pane.move_left", "cmd+ctrl+left", "ctrl+shift+alt+left", "="},
    {"pane.move_down", "cmd+ctrl+down", "ctrl+shift+alt+down", "="},
    {"pane.move_up", "cmd+ctrl+up", "ctrl+shift+alt+up", "="},
    {"pane.move_right", "cmd+ctrl+right", "ctrl+shift+alt+right", "="},

    {"terminal.copy_mode", "cmd+shift+space", "ctrl+shift+space", "="},
    {"terminal.copy", "cmd+c", "ctrl+shift+c, ctrl+insert", "="},
    {"terminal.paste", "cmd+v", "ctrl+shift+v, shift+insert", "="},
    {"terminal.select_all", "cmd+a", "ctrl+shift+a", "="},
    {"terminal.find", "cmd+f", "ctrl+shift+f", "="},
    {"terminal.find_next", "cmd+g", "", ""},
    {"terminal.find_next", "enter", "enter, f3", "=", "find_bar"},
    {"terminal.find_previous", "cmd+shift+g", "", ""},
    {"terminal.find_previous", "shift+enter", "shift+enter, shift+f3", "=", "find_bar"},
    {"terminal.clear", "cmd+k", "ctrl+shift+k", "="},
    {"terminal.font_bigger", "cmd+=", "ctrl+=", "="},
    {"terminal.font_bigger", "cmd+shift+=, cmd+kp_add", "ctrl+shift+=, ctrl+kp_add", "=", {}, false, true},
    {"terminal.font_smaller", "cmd+-", "ctrl+-", "="},
    {"terminal.font_smaller", "cmd+kp_subtract", "ctrl+kp_subtract", "=", {}, false, true},
    {"terminal.font_reset", "cmd+0", "ctrl+0", "="},
    {"terminal.font_reset", "cmd+kp_0", "ctrl+kp_0", "=", {}, false, true},
    {"terminal.scroll_page_up", "cmd+pageup", "shift+pageup", "="},
    {"terminal.scroll_page_down", "cmd+pagedown", "shift+pagedown", "="},
    {"terminal.scroll_top", "cmd+home", "ctrl+shift+home", "="},
    {"terminal.scroll_bottom", "cmd+end", "ctrl+shift+end", "="},
    {"terminal.prompt_previous", "cmd+up", "ctrl+shift+up", "="},
    {"terminal.prompt_next", "cmd+down", "ctrl+shift+down", "="},
    {"terminal.send_next_key", "cmd+opt+k", "ctrl+shift+alt+k", "="},

    {"project.open", "cmd+o", "ctrl+shift+o", "="},
    {"project.next", "cmd+]", "ctrl+shift+]", "="},
    {"project.previous", "cmd+[", "ctrl+shift+[", "="},

    {"view.toggle_sidebar", "cmd+backslash", "ctrl+shift+backslash", "="},
    {"view.toggle_resources", "cmd+i", "ctrl+shift+i", "="},
    {"view.toggle_title_bar", "cmd+opt+t", "", ""},
    {"view.toggle_fullscreen", "cmd+ctrl+f", "f11, alt+enter", "f11"},
    {"window.new", "cmd+n", "ctrl+shift+n", "="},
    {"window.close", "cmd+shift+w", "ctrl+shift+alt+w", "="},
    {"sidebar.select_previous", "up", "up", "="},
    {"sidebar.select_next", "down", "down", "="},
    {"sidebar.collapse", "left", "left", "="},
    {"sidebar.expand", "right", "right", "="},
    {"sidebar.open", "", "enter", "="},
    {"sidebar.rename", "enter", "f2", "="},
    {"sidebar.remove", "delete", "delete", "="},

    {"workspace.save", "cmd+s", "ctrl+shift+s", "="},
    {"app.settings", "cmd+,", "ctrl+,", "="},
    {"app.reload_config", "cmd+shift+,", "ctrl+shift+,", "="},
    {"app.quit", "cmd+q", "", "ctrl+shift+q"},
    {"quick_terminal.toggle", "ctrl+grave", "ctrl+grave", "=", {}, true},
};

std::string_view column(const DefaultRow& r, Os os) {
    switch (os) {
        case Os::MacOS: return r.mac;
        case Os::Windows: return r.win;
        case Os::Linux: return r.linux_ == "=" ? r.win : r.linux_;
    }
    return r.win;
}

template <class F>
void for_each_alternative(std::string_view list, F f) {
    std::size_t i = 0;
    while (i < list.size()) {
        std::size_t j = list.find(',', i);
        // ", " separates alternatives; a bare "," is the comma key ("ctrl+,").
        while (j != std::string_view::npos && (j + 1 >= list.size() || list[j + 1] != ' ')) j = list.find(',', j + 1);
        const std::size_t end = j == std::string_view::npos ? list.size() : j;
        std::string_view item = list.substr(i, end - i);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        if (!item.empty()) f(item);
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
}

Ctx effective_of(Ctx when, const Command& c) {
    const Ctx focus = when & kFocusMask;
    const Ctx cond = when & ~kFocusMask;
    // A command available in the whole window is available in every focus inside it, so a
    // `when` may narrow Window to Terminal, Sidebar, ...; Always covers every focus.
    Ctx allowed = c.contexts;
    if (any(allowed & Ctx::Always)) allowed |= kFocusMask;
    if (any(allowed & Ctx::Window))
        allowed |= Ctx::Terminal | Ctx::TextInput | Ctx::Palette | Ctx::FindBar | Ctx::CopyMode | Ctx::Sidebar;
    Ctx eff = (!any(focus) || any(focus & Ctx::Always)) ? c.contexts : (focus & allowed);
    return eff | cond;
}

config::Diagnostic diag(config::Diagnostic::Level level, const config::KeyBinding& kb, std::string message,
                        std::string hint = {}) {
    config::Diagnostic d;
    d.level = level;
    d.file = kb.origin.file;
    d.line = kb.origin.line;
    d.column = kb.origin.column;
    d.key = "keys." + kb.command;
    d.message = std::move(message);
    d.hint = std::move(hint);
    return d;
}

bool is_letter(const Key& k) { return k.kind == Key::Kind::Char && k.ch >= 'a' && k.ch <= 'z'; }
bool is_char(const Key& k, char c) { return k.kind == Key::Kind::Char && k.ch == static_cast<char32_t>(c); }
bool is_named(const Key& k, NamedKey n) { return k.kind == Key::Kind::Named && k.named == n; }
bool is_fkey(const Key& k) {
    return k.kind == Key::Kind::Named && k.named >= NamedKey::F1 && k.named <= NamedKey::F24;
}
bool is_arrow(const Key& k) {
    return k.kind == Key::Kind::Named && (k.named == NamedKey::Up || k.named == NamedKey::Down ||
                                          k.named == NamedKey::Left || k.named == NamedKey::Right);
}

}  // namespace

// ---------------------------------------------------------------- reserved chords

bool is_reserved_chord(const Chord& c, Os os) {
    (void)os;
    const Key& k = c.key;
    const std::uint8_t m = c.mods;
    if (m == Ctrl) {
        if (is_letter(k)) return true;   // Ctrl+letter is a control character (Ctrl+R, Ctrl+W, Ctrl+D)
        for (char s : {'[', ']', '\\', '^', '_', '@'})
            if (is_char(k, s)) return true;
        if (is_named(k, NamedKey::Space)) return true;
        if (is_named(k, NamedKey::Left) || is_named(k, NamedKey::Right) || is_named(k, NamedKey::Backspace) ||
            is_named(k, NamedKey::Delete))
            return true;   // word editing in readline and PSReadLine
    }
    if ((m == Alt || m == (Alt | Shift)) && is_letter(k)) return true;   // Meta
    if (m == 0 && is_fkey(k)) return true;
    if (m == Shift && (is_named(k, NamedKey::Home) || is_named(k, NamedKey::End))) return true;
    return false;
}

bool is_documented_exception(const Chord& c, Os os) {
    const Key& k = c.key;
    const std::uint8_t m = c.mods;
    if (m == Ctrl && (is_named(k, NamedKey::Tab) || is_named(k, NamedKey::PageUp) || is_named(k, NamedKey::PageDown) ||
                      is_char(k, '=') || is_char(k, '-') || is_char(k, '0') || is_char(k, ',') ||
                      is_named(k, NamedKey::Insert)))
        return true;
    if (m == (Ctrl | Shift) && is_named(k, NamedKey::Tab)) return true;
    if (m == Alt && ((k.kind == Key::Kind::Char && k.ch >= '1' && k.ch <= '9') || is_arrow(k))) return true;
    if (m == Shift && (is_named(k, NamedKey::PageUp) || is_named(k, NamedKey::PageDown) || is_named(k, NamedKey::Insert)))
        return true;
    if (m == 0 && is_named(k, NamedKey::F11)) return true;
    if (os == Os::MacOS && m == Ctrl &&
        (is_char(k, '[') || is_char(k, ']') || is_named(k, NamedKey::Left) || is_named(k, NamedKey::Right)))
        return true;   // Macterm's tab chords (macOS defaults)
    return false;
}

std::string reserved_warning(const Chord& c, Os os) {
    const std::string chord = format_chord(c, os, Format::Words);
    std::string example;
    if (c.mods == Ctrl && is_char(c.key, 'r')) example = " (for example reverse search in shells)";
    else if (c.mods == Ctrl && is_letter(c.key)) example = " (it is a control character)";
    else if (c.mods == Ctrl) example = " (word editing in shells)";
    else if (c.mods & Alt) example = " (Meta in shells and editors)";
    return "Terminal programs use " + chord + example + ". Bind anyway?";
}

bool contexts_conflict(Ctx a, Ctx b) {
    // The focus states that can occur in the UI focus chain, with Always implied.
    static constexpr Ctx kStates[] = {
        Ctx::Window,
        Ctx::Window | Ctx::Terminal,
        Ctx::Window | Ctx::Terminal | Ctx::CopyMode,
        Ctx::Window | Ctx::TextInput,
        Ctx::Window | Ctx::TextInput | Ctx::Palette,
        Ctx::Window | Ctx::TextInput | Ctx::FindBar,
        Ctx::Window | Ctx::Sidebar,
        Ctx::QuickTerminal | Ctx::Terminal,
        Ctx::QuickTerminal | Ctx::Terminal | Ctx::CopyMode,
        Ctx::QuickTerminal | Ctx::TextInput | Ctx::Palette,
        Ctx::QuickTerminal | Ctx::TextInput | Ctx::FindBar,
    };
    constexpr int kBaseRank = 4;   // QuickTerminal and below: Always, Window, Terminal
    for (Ctx s : kStates) {
        const Ctx st = s | Ctx::Always;
        const Ctx x = a & st & kFocusMask, y = b & st & kFocusMask;
        if (!any(x) || !any(y)) continue;
        const int rx = specificity(x), ry = specificity(y);
        if (rx == ry) return true;
        if (rx <= kBaseRank && ry <= kBaseRank) return true;
    }
    return false;
}

bool program_matches(std::string_view fg, std::string_view listed, Os os) {
    auto base = [&](std::string_view p) {
        const auto slash = p.find_last_of(os == Os::Windows ? "/\\" : "/");
        if (slash != std::string_view::npos) p.remove_prefix(slash + 1);
        if (os == Os::Windows) {
            const auto dot = p.rfind('.');
            if (dot != std::string_view::npos && dot > 0) p = p.substr(0, dot);
        }
        return p;
    };
    const std::string_view a = base(fg), b = base(listed);
    if (a.empty() || a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (os == Os::Windows) {
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) return false;
    }
    return true;
}

// ---------------------------------------------------------------- Keymap

void Keymap::add(Binding b) { bindings_.push_back(std::move(b)); }

const std::vector<std::uint32_t>* Keymap::candidates(const Chord& c) const {
    if (index_.empty()) return nullptr;
    const auto it = index_.find(c.code());
    return it == index_.end() ? nullptr : &it->second;
}

void Keymap::finalize() {
    index_.clear();
    conflicts_.clear();
    has_sequences_ = false;
    for (std::uint32_t i = 0; i < bindings_.size(); ++i) {
        Binding& b = bindings_[i];
        if (b.cmd) b.effective = effective_of(b.when, *b.cmd);
        if (b.seq.length > 1) has_sequences_ = true;
        index_[b.seq.front().code()].push_back(i);
    }
    // Resolution order: user first, then the more specific context, then binding order (which
    // is registry order for defaults, file order for user bindings).
    for (auto& [code, list] : index_) {
        std::stable_sort(list.begin(), list.end(), [&](std::uint32_t x, std::uint32_t y) {
            const Binding& a = bindings_[x];
            const Binding& b = bindings_[y];
            if (a.user() != b.user()) return a.user();
            return specificity(a.effective) > specificity(b.effective);
        });
        for (std::size_t i = 0; i < list.size(); ++i) {
            for (std::size_t j = i + 1; j < list.size(); ++j) {
                const Binding& a = bindings_[list[i]];
                const Binding& b = bindings_[list[j]];
                if (!(a.seq.is_prefix_of(b.seq) || b.seq.is_prefix_of(a.seq))) continue;
                if (!contexts_conflict(a.effective, b.effective)) continue;
                if (a.command == b.command && a.args.value == b.args.value) continue;
                Conflict c;
                c.winner = list[i];
                c.loser = list[j];
                c.shadowed_default = a.user() && !b.user();
                const std::string chord = format_sequence(b.seq, os_, Format::Config);
                const std::string at = a.cmd ? std::string(a.cmd->title) : std::string(a.command);
                const std::string bt = b.cmd ? std::string(b.cmd->title) : std::string(b.command);
                c.message = c.shadowed_default ? chord + ": " + at + " shadows the default of " + bt
                                               : chord + ": " + bt + " conflicts with " + at + " (" + at + " wins)";
                conflicts_.push_back(std::move(c));
            }
        }
    }
    std::sort(conflicts_.begin(), conflicts_.end(),
              [](const Conflict& x, const Conflict& y) { return std::pair(x.winner, x.loser) < std::pair(y.winner, y.loser); });
}

std::vector<const Binding*> Keymap::bindings_for(CommandId id) const {
    std::vector<const Binding*> out;
    for (const auto& b : bindings_)
        if (b.command == id && !b.hidden) out.push_back(&b);
    for (const auto& b : bindings_)
        if (b.command == id && b.hidden) out.push_back(&b);
    return out;
}

std::optional<Sequence> Keymap::key_caps(CommandId id) const {
    for (const auto& b : bindings_)
        if (b.command == id && !b.hidden && b.args.empty() && !any(b.when & (Ctx::FindBar | Ctx::Palette | Ctx::Sidebar)))
            return b.seq;
    for (const auto& b : bindings_)
        if (b.command == id && !b.hidden) return b.seq;
    return std::nullopt;
}

namespace {

void add_defaults(Keymap& km, Os os, const CommandRegistry& r) {
    for (const auto& row : kDefaults) {
        const Command* cmd = r.find(row.command);
        if (!cmd) continue;
        Ctx when = Ctx::Always;
        if (!row.when.empty()) parse_ctx_name(row.when, when);
        for_each_alternative(column(row, os), [&](std::string_view text) {
            auto s = parse_sequence(text, os);
            if (!s) return;   // a broken default is caught by the default keymap tests
            Binding b;
            b.seq = *s;
            b.command = cmd->id;
            b.cmd = cmd;
            b.when = when;
            b.global = row.global;
            b.hidden = row.hidden;
            km.add(std::move(b));
        });
    }
    // tab.select {1..9}: cmd+N on macOS; alt+N elsewhere (Ctrl+Alt+digit is AltGr on many layouts).
    if (const Command* cmd = r.find("tab.select")) {
        for (int n = 1; n <= 9; ++n) {
            Binding b;
            b.seq = Sequence::of(Chord{static_cast<std::uint8_t>(os == Os::MacOS ? Super : Alt),
                                       Key::character(U'0' + static_cast<char32_t>(n))});
            b.command = cmd->id;
            b.cmd = cmd;
            b.args.set("index", config::RawValue{static_cast<std::int64_t>(n)});
            km.add(std::move(b));
        }
    }
}

// Places the tab.select bindings where the table says (after tab.recent) so that registry
// order holds for docs; the table above leaves them out for brevity.
void order_by_registry(std::vector<Binding>& v, const CommandRegistry& r) {
    std::stable_sort(v.begin(), v.end(), [&](const Binding& a, const Binding& b) {
        return r.index_of(a.command) < r.index_of(b.command);
    });
}

}  // namespace

Keymap Keymap::defaults_for(Os os, const CommandRegistry& r) {
    Keymap km;
    km.os_ = os;
    add_defaults(km, os, r);
    order_by_registry(km.bindings_, r);
    km.finalize();
    return km;
}

KeymapLoad Keymap::from_config(const config::Config& c, Os os, const CommandRegistry& r) {
    return from_config(c.keys(), c.keys_passthrough_programs(), os, r);
}

KeymapLoad Keymap::from_config(const config::KeysTable& t, std::span<const std::string> programs, Os os,
                               const CommandRegistry& r) {
    using Level = config::Diagnostic::Level;
    KeymapLoad out;
    Keymap& km = out.keymap;
    km.os_ = os;
    add_defaults(km, os, r);
    for (const auto& p : programs) {
        std::string_view s = p;
        while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
        while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
        if (!s.empty()) km.programs_.emplace_back(s);
    }

    std::vector<Binding> user;
    for (const auto& kb : t) {
        const Command* cmd = r.find(kb.command);
        if (!cmd) {
            out.diagnostics.push_back(diag(Level::Warning, kb, "unknown command '" + kb.command + "'; binding ignored",
                                           r.suggest(kb.command)));
            continue;
        }
        if (!cmd->bindable) {
            out.diagnostics.push_back(diag(Level::Warning, kb, "'" + kb.command + "' cannot be bound"));
            continue;
        }
        // Shape: string, list of strings, or { keys, global, passthrough, args, when }.
        std::vector<std::string> texts;
        bool has_keys = true;
        bool global = cmd->id == "quick_terminal.toggle";   // always global
        bool passthrough = false;
        CommandArgs args;
        Ctx when = Ctx::Always;
        bool bad = false;
        auto take_list = [&](const config::RawValue& v) {
            if (const auto* s = std::get_if<std::string>(&v.v)) {
                texts.push_back(*s);
                return true;
            }
            if (const auto* a = std::get_if<config::RawValue::Array>(&v.v)) {
                for (const auto& e : *a) {
                    const auto* s = std::get_if<std::string>(&e.v);
                    if (!s) return false;
                    texts.push_back(*s);
                }
                return true;
            }
            return false;
        };
        if (const auto* tab = kb.value.table()) {
            has_keys = false;
            for (const auto& e : *tab) {
                if (e.key == "keys") {
                    has_keys = true;
                    if (!take_list(e.value)) bad = true;
                } else if (e.key == "global" || e.key == "passthrough") {
                    const auto* b = std::get_if<bool>(&e.value.v);
                    if (!b) {
                        bad = true;
                    } else if (e.key == "global") {
                        global = global || *b;
                    } else {
                        passthrough = *b;
                    }
                } else if (e.key == "args") {
                    if (!e.value.is_table()) bad = true;
                    else args.value = e.value;
                } else if (e.key == "when") {
                    std::vector<std::string> names;
                    if (const auto* s = std::get_if<std::string>(&e.value.v)) names.push_back(*s);
                    else if (const auto* a = std::get_if<config::RawValue::Array>(&e.value.v))
                        for (const auto& x : *a)
                            if (const auto* s2 = std::get_if<std::string>(&x.v)) names.push_back(*s2);
                    when = Ctx::None;
                    for (const auto& n : names) {
                        Ctx c = Ctx::None;
                        if (!parse_ctx_name(n, c)) {
                            out.diagnostics.push_back(diag(Level::Warning, kb, "unknown context '" + n + "' in 'when'",
                                                           "use terminal, window, palette, find_bar, copy_mode, "
                                                           "sidebar, text_input, quick_terminal"));
                            bad = true;
                        }
                        when |= c;
                    }
                    if (!any(when)) when = Ctx::Always;
                } else {
                    out.diagnostics.push_back(diag(Level::Warning, kb, "unknown field '" + e.key + "'; binding ignored"));
                    bad = true;
                }
            }
        } else if (!take_list(kb.value)) {
            bad = true;
        }
        if (bad) {
            out.diagnostics.push_back(diag(Level::Warning, kb, "binding of '" + kb.command +
                                                                   "' has the wrong shape; the default stays",
                                           "use a chord string, a list of chords, \"none\", or { keys = [...], "
                                           "global = bool, passthrough = bool, args = {...} }"));
            continue;
        }
        if (global && passthrough) {
            out.diagnostics.push_back(diag(Level::Warning, kb, "a binding cannot be both global and passthrough; "
                                                               "passthrough ignored"));
            passthrough = false;
        }
        if (auto v = validate_args(cmd->args, args); !v) {
            out.diagnostics.push_back(diag(Level::Warning, kb, v.error().what + "; binding ignored"));
            continue;
        }
        if (!has_keys) {   // flags for the default chords: "tab.new" = { passthrough = true }
            for (auto& b : km.bindings_) {
                if (b.command != cmd->id) continue;
                b.passthrough = passthrough;
                b.global = global;
                if (!args.empty()) b.args = args;
                if (when != Ctx::Always) b.when = when;
            }
            continue;
        }
        std::vector<Binding> parsed;
        bool unbind = false;
        std::size_t failed = 0;
        for (const auto& text : texts) {
            auto s = parse_binding_text(text, os);
            if (!s) {
                ++failed;
                const auto& e = s.error();
                out.diagnostics.push_back(diag(Level::Warning, kb,
                                               "cannot parse \"" + text + "\" at position " + std::to_string(e.pos + 1) +
                                                   ": " + e.message));
                continue;
            }
            if (!s->has_value()) {
                unbind = true;
                continue;
            }
            Binding b;
            b.seq = **s;
            b.command = cmd->id;
            b.cmd = cmd;
            b.args = args;
            b.when = when;
            b.global = global;
            b.passthrough = passthrough;
            b.origin = BindingOrigin{BindingOrigin::Kind::User, kb.origin.file, kb.origin.line, kb.origin.column};
            if (b.seq.length > 1 && global) {
                out.diagnostics.push_back(diag(Level::Warning, kb, "a global binding must be a single chord; \"" +
                                                                       text + "\" ignored"));
                continue;
            }
            if (any(effective_of(when, *cmd) & (Ctx::Terminal | Ctx::Window | Ctx::Always)) &&
                is_reserved_chord(b.seq.front(), os) && !is_documented_exception(b.seq.front(), os))
                out.diagnostics.push_back(diag(Level::Info, kb, reserved_warning(b.seq.front(), os)));
            parsed.push_back(std::move(b));
        }
        if (unbind && !parsed.empty())
            out.diagnostics.push_back(diag(Level::Warning, kb, "\"none\" together with chords; the chords are used"));
        if (failed == texts.size() && !texts.empty()) continue;   // nothing usable: the default stays
        if (when != Ctx::Always && !any(effective_of(when, *cmd) & kFocusMask)) {
            out.diagnostics.push_back(diag(Level::Warning, kb, "'when' never applies to this command; binding ignored"));
            continue;
        }
        // A user entry replaces every default binding of its command.
        std::erase_if(km.bindings_, [&](const Binding& b) { return !b.user() && b.command == cmd->id; });
        std::erase_if(user, [&](const Binding& b) { return b.command == cmd->id; });
        for (auto& b : parsed) user.push_back(std::move(b));
    }
    order_by_registry(km.bindings_, r);
    for (auto& b : user) km.bindings_.push_back(std::move(b));
    km.finalize();
    for (const auto& c : km.conflicts_) {
        const Binding& w = km.bindings_[c.winner];
        const Binding& l = km.bindings_[c.loser];
        const Binding& u = w.user() ? w : l;
        if (!u.user()) continue;   // default against default: the default keymap tests own those
        config::Diagnostic d;
        d.level = Level::Warning;
        d.file = u.origin.file;
        d.line = u.origin.line;
        d.column = u.origin.column;
        d.key = "keys." + std::string(u.command);
        d.message = c.message;
        out.diagnostics.push_back(std::move(d));
    }
    return out;
}

}  // namespace kt::commands
