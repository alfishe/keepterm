// `keepterm --pty-render <out.png> [--size COLSxROWS] [--font-size PT] [--timeout SEC] -- <program> [args]`
//
// Headless smoke path: runs a program in a PTY, feeds its output through the VT terminal, waits
// for it to exit (or the timeout), and renders the final screen with the software renderer
// through the terminal view, using the system's default monospace font. Writes an RGB PNG of
// exactly cols * cell_w by rows * cell_h pixels.
#pragma once

namespace kt::app {

// argv[i] is the first argument after `--pty-render`. Returns the process exit code.
int pty_render(int argc, char** argv, int i);

}  // namespace kt::app
