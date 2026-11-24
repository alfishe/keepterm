// `keepterm` / `keepterm --gui`: one terminal window running the user's shell.
//
// Threads: the session link's I/O thread feeds the Terminal and publishes frames;
// the UI thread wakes on publish, acquires the frame, builds the draw list with the TerminalView
// and renders it with the backend chosen by render/render_select.h (renderer key, probe cache,
// device-loss fallback), damage only, at most once per frame interval of the
// window. Input goes the other way: keys, text, paste, mouse and focus are
// encoded by the view and queued on the link.
//
// Here the Terminal lives in the GUI process and the I/O thread is the PTY reader. The UI
// thread touches the Terminal only under a mutex (resize, scrollback reads while scrolled back,
// publish retries); a scrollback view and the session host remove that.
#pragma once

namespace kt::app {

// argv[i] is the first argument after `--gui` (or 1 without it). Returns the process exit code:
// the shell's exit code when the shell ended the session, 0 when the window was closed.
int gui_main(int argc, char** argv, int i);

// The usage lines of the GUI role (also part of `keepterm --help`).
const char* gui_usage();

}  // namespace kt::app
