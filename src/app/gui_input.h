// Platform input to terminal view input, and the built-in default keymap.
//
// The command registry and the configurable keymap replace match_shortcut() later.
// Pure functions, no OS calls.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform_gui/input.h"
#include "view/terminal/terminal_view.h"

namespace kt::app {

enum class Shortcut : uint8_t { none, paste, font_bigger, font_smaller, font_reset };

// macOS: cmd+v, cmd+= (also cmd++), cmd+-, cmd+0. Elsewhere: ctrl+shift+v, shift+insert,
// ctrl+= (also ctrl++), ctrl+-, ctrl+0. Keypad + and - work as = and -. Releases never match.
Shortcut match_shortcut(const platform::KeyEvent&, bool macos);

view::KeyInput to_key_input(const platform::KeyEvent&);
// Positions go from DIPs to physical pixels.
view::PointerInput to_pointer_input(const platform::MouseEvent&, float scale);
view::WheelInput to_wheel_input(const platform::ScrollEvent&, float scale);

// True when the bytes the encoder produced for a key are just the key's text: the platform
// then delivers the same text through on_text (and an IME may take the key), so the key is
// not consumed.
bool key_is_plain_text(const platform::KeyEvent&, std::string_view encoded);

// Dropped (or pasted image) file paths as shell words, space-separated, with a trailing space
// so the next word can follow. `windows`: double quotes around a path with a space or a
// special character (cmd, PowerShell); else POSIX: single quotes, ' as '\''.
std::string quote_paths(const std::vector<std::string>& paths, bool windows);

// How $VISUAL / $EDITOR opens `file` at `line`:`col` (0 = not given; Cmd/Ctrl+click on a path).
// `gui`: a windowed editor (code, cursor, subl, zed, ...) started detached; else a terminal
// editor (vim, nvim, nano, hx, emacs, micro, ...) run in a new tab. Unknown editors get the file
// alone. Empty argv when `editor` is blank.
struct EditorCommand {
    std::vector<std::string> argv;
    bool gui = false;
};
EditorCommand editor_command(std::string_view editor, const std::string& file, uint32_t line, uint32_t col);

}  // namespace kt::app
