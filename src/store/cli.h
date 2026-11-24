// `keepterm --history ...`:
//
//   keepterm --history du [--json]
//   keepterm --history search <query> [--regex] [--case] [--since T] [--until T]
//                         [--session ID]... [--project P] [--kind K]... [--limit N] [--json] [--explain]
//   keepterm --history forget <session>
//   keepterm --history pin <session> | unpin <session>
//   keepterm --history backfill <session> [--journal FILE] [--project P] [--no-raw]
//   keepterm --history janitor            (one janitor pass now, if no other process runs one)
//
// Common options: --root DIR (default <state>/history, KEEPTERM_STATE_DIR honoured).
// T is a duration back from now ("90m", "2h", "7d", "2w"), a date "YYYY-MM-DD[THH:MM[:SS]]" in
// local time, or Unix milliseconds.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace kt::store {

int history_cli(int argc, char** argv, int first);

// Parses T (see above) against `now_ms`; nullopt when malformed.
std::optional<uint64_t> parse_time_arg(std::string_view s, uint64_t now_ms);

}  // namespace kt::store
