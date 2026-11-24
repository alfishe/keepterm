#include "app/gui_input.h"

#include <cctype>
#include <string_view>

namespace kt::app {

using platform::Key;

namespace {

// The layout's base character decides (so that the bindings follow the printed key on other
// layouts); without one, the physical key.
bool is_key(const platform::KeyEvent& k, char32_t ch, Key physical) {
    if (k.logical != 0) return k.logical == ch;
    return k.key == physical;
}

constexpr uint16_t kMain = platform::mod_shift | platform::mod_ctrl | platform::mod_alt | platform::mod_super;

}  // namespace

Shortcut match_shortcut(const platform::KeyEvent& k, bool macos) {
    if (k.action == platform::KeyAction::release || k.composing) return Shortcut::none;
    const uint16_t m = k.mods & kMain;
    const uint16_t primary = macos ? platform::mod_super : platform::mod_ctrl;

    // Font size: primary, Shift allowed (cmd++ is cmd+shift+= on US layouts).
    if ((m & ~platform::mod_shift) == primary) {
        if (is_key(k, U'=', Key::equal) || is_key(k, U'+', Key::equal) || k.key == Key::kp_add)
            return Shortcut::font_bigger;
        if (is_key(k, U'-', Key::minus) || k.key == Key::kp_subtract) return Shortcut::font_smaller;
        if (m == primary && (is_key(k, U'0', Key::d0) || k.key == Key::kp_0)) return Shortcut::font_reset;
    }
    if (macos) {
        if (m == platform::mod_super && is_key(k, U'v', Key::v)) return Shortcut::paste;
    } else {
        if (m == (platform::mod_ctrl | platform::mod_shift) && is_key(k, U'v', Key::v)) return Shortcut::paste;
        if (m == platform::mod_shift && k.key == Key::insert) return Shortcut::paste;
    }
    return Shortcut::none;
}

view::KeyInput to_key_input(const platform::KeyEvent& k) {
    view::KeyInput in;
    in.key = static_cast<vt::Key>(k.key);   // both are USB HID usage page 7
    in.logical = k.logical;
    in.native_code = k.native_code;
    in.mods = k.mods;
    in.action = k.action == platform::KeyAction::press    ? vt::KeyAction::Press
                : k.action == platform::KeyAction::repeat ? vt::KeyAction::Repeat
                                                          : vt::KeyAction::Release;
    in.text = k.text;
    in.composing = k.composing;
    return in;
}

view::PointerInput to_pointer_input(const platform::MouseEvent& m, float scale) {
    view::PointerInput in;
    using T = platform::MouseEvent::Type;
    using VT = view::PointerInput::Type;
    switch (m.type) {
        case T::move: in.type = VT::move; break;
        case T::press: in.type = VT::press; break;
        case T::release: in.type = VT::release; break;
        case T::enter: in.type = VT::enter; break;
        case T::leave: in.type = VT::leave; break;
    }
    in.button = static_cast<view::PointerButton>(m.button);   // same order
    in.click_count = m.click_count;
    in.mods = m.mods;
    in.x = m.pos.x * scale;
    in.y = m.pos.y * scale;
    return in;
}

view::WheelInput to_wheel_input(const platform::ScrollEvent& s, float scale) {
    view::WheelInput in;
    in.precise = s.precise;
    in.dx = s.precise ? s.dx * scale : s.dx;
    in.dy = s.precise ? s.dy * scale : s.dy;
    in.mods = s.mods;
    in.x = s.pos.x * scale;
    in.y = s.pos.y * scale;
    return in;
}

bool key_is_plain_text(const platform::KeyEvent& k, std::string_view encoded) {
    if (k.text.empty()) return false;
    const std::string_view text(reinterpret_cast<const char*>(k.text.data()), k.text.size());
    return encoded == text;
}

std::string quote_paths(const std::vector<std::string>& paths, bool windows) {
    std::string out;
    for (const std::string& p : paths) {
        if (p.empty()) continue;
        bool plain = true;
        for (const char c : p) {
            const auto ch = static_cast<unsigned char>(c);
            const bool ok = std::isalnum(ch) || ch >= 0x80 || std::string_view("_-./:+,@%=").find(c) != std::string_view::npos ||
                            (windows && ch == '\\');
            if (!ok) {
                plain = false;
                break;
            }
        }
        if (plain) {
            out += p;
        } else if (windows) {
            out += '"' + p + '"';   // a path cannot hold '"' on Windows
        } else {
            out += '\'';
            for (char ch : p) {
                if (ch == '\'') out += "'\\''";
                else out += ch;
            }
            out += '\'';
        }
        out += ' ';
    }
    return out;
}

EditorCommand editor_command(std::string_view editor, const std::string& file, uint32_t line, uint32_t col) {
    EditorCommand out;
    size_t k = 0;
    while (k < editor.size()) {
        while (k < editor.size() && (editor[k] == ' ' || editor[k] == '\t')) ++k;
        const size_t b = k;
        while (k < editor.size() && editor[k] != ' ' && editor[k] != '\t') ++k;
        if (k > b) out.argv.emplace_back(editor.substr(b, k - b));
    }
    if (out.argv.empty()) return out;
    std::string base = out.argv[0];
    if (const size_t cut = base.find_last_of("/\\"); cut != std::string::npos) base = base.substr(cut + 1);
    for (char& ch : base) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* ext : {".exe", ".cmd", ".bat"})
        if (base.size() > 4 && base.ends_with(ext)) base.resize(base.size() - 4);
    const std::string l = std::to_string(line), lc = l + (col ? ":" + std::to_string(col) : std::string());
    auto is = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
            if (base == n) return true;
        return false;
    };
    if (is({"code", "code-insiders", "codium", "vscodium", "cursor", "windsurf"})) {
        out.gui = true;
        if (line) {
            out.argv.emplace_back("-g");
            out.argv.push_back(file + ":" + lc);
        } else {
            out.argv.push_back(file);
        }
    } else if (is({"subl", "sublime_text", "zed", "zeditor"})) {
        out.gui = true;
        out.argv.push_back(line ? file + ":" + lc : file);
    } else if (is({"mate"})) {
        out.gui = true;
        if (line) {
            out.argv.emplace_back("-l");
            out.argv.push_back(l);
        }
        out.argv.push_back(file);
    } else if (is({"hx", "helix"})) {
        out.argv.push_back(line ? file + ":" + lc : file);
    } else if (is({"micro", "kak"})) {
        if (line) out.argv.push_back("+" + lc);
        out.argv.push_back(file);
    } else if (is({"nano"})) {
        if (line) out.argv.push_back("+" + l + (col ? "," + std::to_string(col) : std::string()));
        out.argv.push_back(file);
    } else if (is({"vi", "vim", "nvim", "gvim", "mvim", "emacs", "emacsclient", "joe", "ne", "mg", "jed", "pico"})) {
        if (line) out.argv.push_back("+" + l);
        out.argv.push_back(file);
        out.gui = base == "gvim" || base == "mvim";
    } else {
        out.argv.push_back(file);
    }
    return out;
}

}  // namespace kt::app
