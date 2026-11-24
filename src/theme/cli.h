// `keepterm --theme list | show NAME` and `keepterm --font list`.
#pragma once

namespace kt::theme {

// argv[i] is the subcommand. Exit code 0, 1 (not found) or 2 (usage).
int theme_cli(int argc, char** argv, int i);
int font_cli(int argc, char** argv, int i);

// Lines for the usage text of main.cpp.
const char* cli_usage();

}  // namespace kt::theme
