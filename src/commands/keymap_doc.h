// The keymap reference (Markdown), generated from the registry and the default keymaps.
// `keepterm --command keymap` prints it; a unit test keeps the committed copy current.
#pragma once

#include <string>

#include "commands/registry.h"

namespace kt::commands {

std::string generate_keymap_markdown(const CommandRegistry& r);

}  // namespace kt::commands
