// `keepterm --history ...`:
//
//   keepterm --history du [--json]
//   keepterm --history search <query> [--regex] [--case] [--since T] [--until T]
//                         [--session ID]... [--project P] [--kind K]... [--limit N] [--json] [--explain]
//   keepterm --history forget <session>
//   keepterm --history backfill <session> [--journal FILE] [--project P] [--no-raw]
//   keepterm --history janitor            (one janitor pass now, if no other process runs one)
//
// The sessions of the history (list, show, restore, rename, tag, untag, pin, unpin, delete) are
// session/history_index.h's.
//
// Common options: --root DIR (default <state>/history, KEEPTERM_STATE_DIR honoured).
// T is a duration back from now ("90m", "2h", "7d", "2w"), a date "YYYY-MM-DD[THH:MM[:SS]]" in
// local time, or Unix milliseconds.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include "config/config.h"
#include "store/retention.h"

namespace kt::store {

// `default_root`: the history folder when --root is not given (the caller knows the profile's
// state folder); empty: <state>/history of the default profile.
// `per_session`: the policy per session from the config (per-project overrides, which the
// session layer resolves); null: the config's policy for all.
int history_cli(int argc, char** argv, int first, const std::filesystem::path& default_root = {},
                const std::function<Janitor::PolicyFor(const config::Config&)>& per_session = {});

// Parses T (see above) against `now_ms`; nullopt when malformed.
std::optional<uint64_t> parse_time_arg(std::string_view s, uint64_t now_ms);

}  // namespace kt::store
