// Golden screen dump: a text form of the screen for tests and for
// `keepterm pane dump` without --styled.
//
//   # keepterm-dump 1
//   size <cols>x<rows> cursor <row>,<col> visible|hidden shape=block|underline|bar alt=0|1 [pending-wrap]
//   row 0 |text of row 0, one column per cell|[W]
//   attr 0 |one legend letter per cell|
//   ...
//   style a = default
//   style b = fg=palette:4 bold
//
// The cursor is 0-based, row first. `W` marks a soft-wrapped row. A wide character shows once,
// followed by `~` in its spacer column. Graphemes are written as they are. A legend letter
// stands for a (style, link) pair; `a` is always the default style without a link. Letters are
// given in order of first appearance.
#pragma once

#include <string>

namespace kt::vt {

class Terminal;

struct DumpOptions {
    bool attrs = true;  // emit `attr` lines and the style legend
};

std::string dump_screen(const Terminal&, const DumpOptions& = {});

// Scrollback rows, oldest first: `sb <abs_line> |text|[W]` (rows keep their stored width).
std::string dump_scrollback(const Terminal&);

}  // namespace kt::vt
