// The main window: sidebar with projects and tabs, the title bar
// with the tab switcher, split panes with their terminals, the status area. One process runs
// every window (AppCore); the workspace model (projects, tabs, splits, sessions) lives in
// ws::WorkspaceRuntime and survives restarts in the state file.
//
// Modes:
//   workspace   the default: the saved workspace, every pane a session host;
//   local       `--local` or `-- program`: an ephemeral workspace whose panes own local PTYs
//               (tests, the fallback when no host can start).
#pragma once

#include <string>
#include <vector>

#include "app/gui_options.h"
#include "platform_gui/platform.h"

namespace kt::app {

struct MainWindowOptions {
    GuiOptions gui;
    bool local = false;                 // panes own local PTYs; nothing is saved
    // --clean: start with a fresh workspace; the saved one becomes the "Last Workspace" preset
    // and its sessions keep running, detached.
    bool clean = false;
    std::vector<std::string> program;   // `-- program args`: the first pane runs it (local mode)
};

// Runs the window until it closes. Returns the process exit code (127 when setup failed).
int run_main_window(platform::IPlatform& p, MainWindowOptions opts);

}  // namespace kt::app
