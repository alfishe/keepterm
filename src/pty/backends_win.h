// Internal entry points of the Windows backends (pty_win.cpp selects between them).
#pragma once

#include <memory>

#include "pty/pty.h"

namespace kt::pty::win {

bool conpty_available();
Result<std::unique_ptr<IPty>> spawn_conpty(const SpawnSpec&);
Result<std::unique_ptr<IPty>> spawn_agent(const SpawnSpec&);

}  // namespace kt::pty::win
