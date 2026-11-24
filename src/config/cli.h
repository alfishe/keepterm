// `keepterm --config ...` (`config check` / `config path`):
//   keepterm --config dump [--defaults] [--no-origins] [--profile NAME] [--file PATH] [--set key=value]...
//   keepterm --config check [PATH] [--profile NAME]
//   keepterm --config path [--profile NAME]
#pragma once

namespace kt::config {

// argv[first] is the word after --config. Returns the process exit code:
// 0 ok, 1 errors in the checked config, 2 usage.
int run_cli(int argc, char** argv, int first);

}  // namespace kt::config
