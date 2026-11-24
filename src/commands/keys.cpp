#include "commands/keys.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace kt::commands {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}

struct NamedEntry {
    NamedKey key;
    std::string_view name;    // canonical config name
    std::string_view words;   // "Page Up"
    std::string_view mac;     // macOS key cap
};

constexpr NamedEntry kNamed[] = {
    {NamedKey::Enter, "enter", "Enter", "\xE2\x86\xA9"},          // ↩
    {NamedKey::Tab, "tab", "Tab", "\xE2\x87\xA5"},                // ⇥
    {NamedKey::Space, "space", "Space", "Space"},
    {NamedKey::Escape, "escape", "Esc", "\xE2\x8E\x8B"},          // ⎋
    {NamedKey::Backspace, "backspace", "Backspace", "\xE2\x8C\xAB"},   // ⌫
    {NamedKey::Delete, "delete", "Delete", "\xE2\x8C\xA6"},       // ⌦
    {NamedKey::Insert, "insert", "Insert", "Insert"},
    {NamedKey::Home, "home", "Home", "\xE2\x86\x96"},             // ↖
    {NamedKey::End, "end", "End", "\xE2\x86\x98"},                // ↘
    {NamedKey::PageUp, "pageup", "PgUp", "\xE2\x87\x9E"},         // ⇞
    {NamedKey::PageDown, "pagedown", "PgDn", "\xE2\x87\x9F"},     // ⇟
    {NamedKey::Up, "up", "Up", "\xE2\x86\x91"},
    {NamedKey::Down, "down", "Down", "\xE2\x86\x93"},
    {NamedKey::Left, "left", "Left", "\xE2\x86\x90"},
    {NamedKey::Right, "right", "Right", "\xE2\x86\x92"},
    {NamedKey::F1, "f1", "F1", "F1"},     {NamedKey::F2, "f2", "F2", "F2"},     {NamedKey::F3, "f3", "F3", "F3"},
    {NamedKey::F4, "f4", "F4", "F4"},     {NamedKey::F5, "f5", "F5", "F5"},     {NamedKey::F6, "f6", "F6", "F6"},
    {NamedKey::F7, "f7", "F7", "F7"},     {NamedKey::F8, "f8", "F8", "F8"},     {NamedKey::F9, "f9", "F9", "F9"},
    {NamedKey::F10, "f10", "F10", "F10"}, {NamedKey::F11, "f11", "F11", "F11"}, {NamedKey::F12, "f12", "F12", "F12"},
    {NamedKey::F13, "f13", "F13", "F13"}, {NamedKey::F14, "f14", "F14", "F14"}, {NamedKey::F15, "f15", "F15", "F15"},
    {NamedKey::F16, "f16", "F16", "F16"}, {NamedKey::F17, "f17", "F17", "F17"}, {NamedKey::F18, "f18", "F18", "F18"},
    {NamedKey::F19, "f19", "F19", "F19"}, {NamedKey::F20, "f20", "F20", "F20"}, {NamedKey::F21, "f21", "F21", "F21"},
    {NamedKey::F22, "f22", "F22", "F22"}, {NamedKey::F23, "f23", "F23", "F23"}, {NamedKey::F24, "f24", "F24", "F24"},
    {NamedKey::Kp0, "kp_0", "Num 0", "Keypad 0"}, {NamedKey::Kp1, "kp_1", "Num 1", "Keypad 1"},
    {NamedKey::Kp2, "kp_2", "Num 2", "Keypad 2"}, {NamedKey::Kp3, "kp_3", "Num 3", "Keypad 3"},
    {NamedKey::Kp4, "kp_4", "Num 4", "Keypad 4"}, {NamedKey::Kp5, "kp_5", "Num 5", "Keypad 5"},
    {NamedKey::Kp6, "kp_6", "Num 6", "Keypad 6"}, {NamedKey::Kp7, "kp_7", "Num 7", "Keypad 7"},
    {NamedKey::Kp8, "kp_8", "Num 8", "Keypad 8"}, {NamedKey::Kp9, "kp_9", "Num 9", "Keypad 9"},
    {NamedKey::KpAdd, "kp_add", "Num +", "Keypad +"},
    {NamedKey::KpSubtract, "kp_subtract", "Num -", "Keypad -"},
    {NamedKey::KpMultiply, "kp_multiply", "Num *", "Keypad *"},
    {NamedKey::KpDivide, "kp_divide", "Num /", "Keypad /"},
    {NamedKey::KpDecimal, "kp_decimal", "Num .", "Keypad ."},
    {NamedKey::KpEqual, "kp_equal", "Num =", "Keypad ="},
    {NamedKey::Menu, "menu", "Menu", "Menu"},
    {NamedKey::PrintScreen, "print_screen", "PrtSc", "PrtSc"},
    {NamedKey::Pause, "pause", "Pause", "Pause"},
    {NamedKey::ScrollLock, "scroll_lock", "ScrLk", "ScrLk"},
};

const NamedEntry* named_entry(NamedKey k) {
    for (const auto& e : kNamed)
        if (e.key == k) return &e;
    return nullptr;
}

constexpr std::pair<std::string_view, NamedKey> kNamedAliases[] = {
    {"return", NamedKey::Enter},       {"kp_enter", NamedKey::Enter},   {"esc", NamedKey::Escape},
    {"del", NamedKey::Delete},         {"ins", NamedKey::Insert},       {"pgup", NamedKey::PageUp},
    {"page_up", NamedKey::PageUp},     {"pgdn", NamedKey::PageDown},    {"page_down", NamedKey::PageDown},
    {"arrowup", NamedKey::Up},         {"arrowdown", NamedKey::Down},   {"arrowleft", NamedKey::Left},
    {"arrowright", NamedKey::Right},   {"kp_plus", NamedKey::KpAdd},    {"kp_minus", NamedKey::KpSubtract},
    {"kp_period", NamedKey::KpDecimal}, {"contextmenu", NamedKey::Menu}, {"printscreen", NamedKey::PrintScreen},
    {"scrolllock", NamedKey::ScrollLock},
};

constexpr std::pair<std::string_view, char> kSymbols[] = {
    {"grave", '`'},      {"backquote", '`'},  {"minus", '-'},     {"equal", '='},        {"bracketleft", '['},
    {"bracketright", ']'}, {"backslash", '\\'}, {"semicolon", ';'}, {"quote", '\''},     {"apostrophe", '\''},
    {"comma", ','},      {"period", '.'},     {"slash", '/'},     {"plus", '+'},
};

// USB HID usage (page 7) ↔ W3C UI Events code.
struct W3cCode {
    std::uint16_t hid;
    std::string_view name;
};
constexpr W3cCode kW3c[] = {
    {4, "KeyA"},   {5, "KeyB"},   {6, "KeyC"},   {7, "KeyD"},   {8, "KeyE"},   {9, "KeyF"},   {10, "KeyG"},
    {11, "KeyH"},  {12, "KeyI"},  {13, "KeyJ"},  {14, "KeyK"},  {15, "KeyL"},  {16, "KeyM"},  {17, "KeyN"},
    {18, "KeyO"},  {19, "KeyP"},  {20, "KeyQ"},  {21, "KeyR"},  {22, "KeyS"},  {23, "KeyT"},  {24, "KeyU"},
    {25, "KeyV"},  {26, "KeyW"},  {27, "KeyX"},  {28, "KeyY"},  {29, "KeyZ"},
    {30, "Digit1"}, {31, "Digit2"}, {32, "Digit3"}, {33, "Digit4"}, {34, "Digit5"}, {35, "Digit6"},
    {36, "Digit7"}, {37, "Digit8"}, {38, "Digit9"}, {39, "Digit0"},
    {40, "Enter"}, {41, "Escape"}, {42, "Backspace"}, {43, "Tab"}, {44, "Space"}, {45, "Minus"}, {46, "Equal"},
    {47, "BracketLeft"}, {48, "BracketRight"}, {49, "Backslash"}, {50, "IntlHash"}, {51, "Semicolon"},
    {52, "Quote"}, {53, "Backquote"}, {54, "Comma"}, {55, "Period"}, {56, "Slash"}, {57, "CapsLock"},
    {58, "F1"},  {59, "F2"},  {60, "F3"},  {61, "F4"},  {62, "F5"},  {63, "F6"},  {64, "F7"},  {65, "F8"},
    {66, "F9"},  {67, "F10"}, {68, "F11"}, {69, "F12"},
    {70, "PrintScreen"}, {71, "ScrollLock"}, {72, "Pause"}, {73, "Insert"}, {74, "Home"}, {75, "PageUp"},
    {76, "Delete"}, {77, "End"}, {78, "PageDown"}, {79, "ArrowRight"}, {80, "ArrowLeft"}, {81, "ArrowDown"},
    {82, "ArrowUp"}, {83, "NumLock"}, {84, "NumpadDivide"}, {85, "NumpadMultiply"}, {86, "NumpadSubtract"},
    {87, "NumpadAdd"}, {88, "NumpadEnter"}, {89, "Numpad1"}, {90, "Numpad2"}, {91, "Numpad3"}, {92, "Numpad4"},
    {93, "Numpad5"}, {94, "Numpad6"}, {95, "Numpad7"}, {96, "Numpad8"}, {97, "Numpad9"}, {98, "Numpad0"},
    {99, "NumpadDecimal"}, {100, "IntlBackslash"}, {101, "ContextMenu"}, {103, "NumpadEqual"},
    {104, "F13"}, {105, "F14"}, {106, "F15"}, {107, "F16"}, {108, "F17"}, {109, "F18"}, {110, "F19"},
    {111, "F20"}, {112, "F21"}, {113, "F22"}, {114, "F23"}, {115, "F24"},
    {133, "NumpadComma"}, {135, "IntlRo"}, {137, "IntlYen"},
};

struct ModName {
    std::string_view name;
    std::uint8_t bit;   // 0 = primary
};
constexpr ModName kMods[] = {
    {"ctrl", Ctrl}, {"control", Ctrl}, {"shift", Shift}, {"alt", Alt},     {"opt", Alt},
    {"option", Alt}, {"super", Super}, {"cmd", Super},   {"command", Super}, {"win", Super},
    {"meta", Super}, {"primary", 0},
};

std::optional<std::uint8_t> mod_from_name(std::string_view s) {
    for (const auto& m : kMods)
        if (iequal(m.name, s)) return m.bit;
    return std::nullopt;
}

Unexpected<ParseError> err(std::size_t pos, std::string msg) { return Unexpected(ParseError{pos, std::move(msg)}); }

bool is_space(char c) { return c == ' ' || c == '\t'; }

Expected<Chord, ParseError> parse_chord(std::string_view text, std::size_t base, Os os) {
    struct Part {
        std::string_view s;
        std::size_t pos;
    };
    std::vector<Part> parts;
    std::size_t pos = 0;
    const std::size_t n = text.size();
    while (pos < n) {
        if (text[pos] == '+' && pos == n - 1) {   // "ctrl++": the key is '+'
            parts.push_back({text.substr(pos, 1), pos});
            pos = n;
            break;
        }
        const std::size_t plus = text.find('+', pos);
        if (plus == pos) return err(base + pos, "empty modifier before '+'");
        const std::size_t end = plus == std::string_view::npos ? n : plus;
        parts.push_back({text.substr(pos, end - pos), pos});
        if (plus == std::string_view::npos) break;
        if (plus + 1 == n) return err(base + n, "missing key after '+'");
        pos = plus + 1;
    }
    if (parts.empty()) return err(base, "empty chord");

    Chord c;
    bool primary = false;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        const auto m = mod_from_name(parts[i].s);
        if (!m) {
            if (key_from_name(parts[i].s))
                return err(base + parts[i].pos, "'" + std::string(parts[i].s) + "' is a key, not a modifier; the key comes last");
            return err(base + parts[i].pos, "unknown modifier '" + std::string(parts[i].s) + "'");
        }
        if (*m == 0) {
            if (primary) return err(base + parts[i].pos, "duplicate modifier 'primary'");
            primary = true;
            continue;
        }
        if (c.mods & *m) return err(base + parts[i].pos, "duplicate modifier '" + std::string(parts[i].s) + "'");
        c.mods = static_cast<std::uint8_t>(c.mods | *m);
    }
    const Part& last = parts.back();
    if (mod_from_name(last.s)) return err(base + last.pos, "missing key after the modifiers");
    const auto k = key_from_name(last.s);
    if (!k) {
        const auto u = static_cast<unsigned char>(last.s.empty() ? 0 : last.s[0]);
        if (u >= 0x80)
            return err(base + last.pos, "non-Latin keys cannot be bound; name the Latin key at the same position "
                                        "(bindings follow the key, not the layout)");
        return err(base + last.pos, "unknown key '" + std::string(last.s) + "'");
    }
    c.key = *k;
    if (primary) {
        if (os == Os::MacOS) {
            c.mods = static_cast<std::uint8_t>(c.mods | Super);
        } else {
            // primary = ctrl+shift; an added shift becomes alt (primary+shift+d = ctrl+shift+alt+d).
            if (c.mods & Shift) c.mods = static_cast<std::uint8_t>(c.mods | Alt);
            c.mods = static_cast<std::uint8_t>(c.mods | Ctrl | Shift);
        }
    }
    return c;
}

}  // namespace

Os current_os() {
#if defined(_WIN32)
    return Os::Windows;
#elif defined(__APPLE__)
    return Os::MacOS;
#else
    return Os::Linux;
#endif
}

std::string_view os_name(Os os) {
    switch (os) {
        case Os::MacOS: return "macos";
        case Os::Windows: return "windows";
        case Os::Linux: return "linux";
    }
    return "linux";
}

std::optional<Os> parse_os(std::string_view name) {
    if (iequal(name, "macos") || iequal(name, "mac") || iequal(name, "darwin")) return Os::MacOS;
    if (iequal(name, "windows") || iequal(name, "win")) return Os::Windows;
    if (iequal(name, "linux")) return Os::Linux;
    return std::nullopt;
}

Key Key::character(char32_t c) {
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    return Key{Kind::Char, c, NamedKey::None, 0};
}

std::uint32_t Key::code() const {
    const auto k = static_cast<std::uint32_t>(kind) << 24;
    switch (kind) {
        case Kind::None: return 0;
        case Kind::Char: return k | static_cast<std::uint32_t>(ch);
        case Kind::Named: return k | static_cast<std::uint32_t>(named);
        case Kind::Physical: return k | physical;
    }
    return 0;
}

bool Sequence::is_prefix_of(const Sequence& other) const {
    if (length > other.length) return false;
    for (std::size_t i = 0; i < length; ++i)
        if (!(chords[i] == other.chords[i])) return false;
    return true;
}

bool operator==(const Sequence& a, const Sequence& b) { return a.length == b.length && a.is_prefix_of(b); }

std::optional<Key> key_from_name(std::string_view name) {
    if (name.empty()) return std::nullopt;
    if (name.size() == 1) {
        const auto c = static_cast<unsigned char>(name[0]);
        if (c > 0x20 && c < 0x7F) return Key::character(c);
        return std::nullopt;
    }
    if (name.size() > 5 && iequal(name.substr(0, 5), "phys:")) {
        const auto hid = hid_from_w3c_code(name.substr(5));
        if (!hid) return std::nullopt;
        return Key::phys(*hid);
    }
    const std::string l = to_lower(name);
    for (const auto& e : kNamed)
        if (e.name == l) return Key::named_key(e.key);
    for (const auto& [alias, k] : kNamedAliases)
        if (alias == l) return Key::named_key(k);
    for (const auto& [sym, c] : kSymbols)
        if (sym == l) return Key::character(static_cast<unsigned char>(c));
    return std::nullopt;
}

std::string_view named_key_name(NamedKey n) {
    const auto* e = named_entry(n);
    return e ? e->name : std::string_view{};
}

std::string_view w3c_code_name(std::uint16_t hid) {
    for (const auto& [h, name] : kW3c)
        if (h == hid) return name;
    return {};
}

std::optional<std::uint16_t> hid_from_w3c_code(std::string_view code) {
    for (const auto& [h, name] : kW3c)
        if (iequal(name, code)) return h;
    return std::nullopt;
}

Expected<Sequence, ParseError> parse_sequence(std::string_view text, Os os) {
    Sequence s;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n && is_space(text[i])) ++i;
    if (i == n) return err(0, "empty binding");
    while (i < n) {
        std::size_t j = i;
        while (j < n && !is_space(text[j])) ++j;
        if (s.length == kMaxChords) return err(i, "a sequence has at most 4 chords");
        const std::string_view tok = text.substr(i, j - i);
        if (iequal(tok, "none")) return err(i, "'none' unbinds and cannot be part of a sequence");
        auto c = parse_chord(tok, i, os);
        if (!c) return Unexpected(c.error());
        s.chords[s.length++] = *c;
        i = j;
        while (i < n && is_space(text[i])) ++i;
    }
    return s;
}

Expected<std::optional<Sequence>, ParseError> parse_binding_text(std::string_view text, Os os) {
    std::string_view t = text;
    while (!t.empty() && is_space(t.front())) t.remove_prefix(1);
    while (!t.empty() && is_space(t.back())) t.remove_suffix(1);
    if (iequal(t, "none")) return std::optional<Sequence>{};
    auto s = parse_sequence(text, os);
    if (!s) return Unexpected(s.error());
    return std::optional<Sequence>{*s};
}

namespace {

std::string key_text(const Key& k, Os os, Format f) {
    switch (k.kind) {
        case Key::Kind::None: return "?";
        case Key::Kind::Char: {
            const char c = static_cast<char>(k.ch);
            if (f == Format::Config) {
                if (c == '`') return "grave";
                if (c == '\\') return "backslash";
                if (c == '+') return "plus";
                return std::string(1, c);
            }
            if (c >= 'a' && c <= 'z') return std::string(1, static_cast<char>(c - 'a' + 'A'));
            return std::string(1, c);
        }
        case Key::Kind::Named: {
            const auto* e = named_entry(k.named);
            if (!e) return "?";
            if (f == Format::Config) return std::string(e->name);
            if (f == Format::Text && os == Os::MacOS) return std::string(e->mac);
            return std::string(e->words);
        }
        case Key::Kind::Physical: {
            const auto name = w3c_code_name(k.physical);
            return "phys:" + std::string(name.empty() ? "?" : name);
        }
    }
    return "?";
}

}  // namespace

std::string format_chord(const Chord& c, Os os, Format f) {
    std::string out;
    const bool mac = os == Os::MacOS;
    if (f == Format::Text && mac) {
        // Apple's order: ⌃ ⌥ ⇧ ⌘, then the key, no separators.
        if (c.mods & Ctrl) out += "\xE2\x8C\x83";
        if (c.mods & Alt) out += "\xE2\x8C\xA5";
        if (c.mods & Shift) out += "\xE2\x87\xA7";
        if (c.mods & Super) out += "\xE2\x8C\x98";
        out += key_text(c.key, os, f);
        return out;
    }
    auto add = [&](std::string_view m) {
        out += m;
        out += '+';
    };
    if (f == Format::Config) {
        if (mac) {   // the Macterm spelling of the defaults: cmd+ctrl+opt+shift
            if (c.mods & Super) add("cmd");
            if (c.mods & Ctrl) add("ctrl");
            if (c.mods & Alt) add("opt");
            if (c.mods & Shift) add("shift");
        } else {     // canonical ctrl+shift+alt+super
            if (c.mods & Ctrl) add("ctrl");
            if (c.mods & Shift) add("shift");
            if (c.mods & Alt) add("alt");
            if (c.mods & Super) add("super");
        }
    } else if (mac) {
        if (c.mods & Ctrl) add("Ctrl");
        if (c.mods & Alt) add("Opt");
        if (c.mods & Shift) add("Shift");
        if (c.mods & Super) add("Cmd");
    } else {
        if (c.mods & Ctrl) add("Ctrl");
        if (c.mods & Shift) add("Shift");
        if (c.mods & Alt) add("Alt");
        if (c.mods & Super) add(os == Os::Windows ? "Win" : "Super");
    }
    out += key_text(c.key, os, f);
    return out;
}

std::string format_sequence(const Sequence& s, Os os, Format f) {
    std::string out;
    for (std::size_t i = 0; i < s.length; ++i) {
        if (i) out += ' ';
        out += format_chord(s.chords[i], os, f);
    }
    return out;
}

}  // namespace kt::commands
