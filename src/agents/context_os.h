// OS side of the context probes (kt_agents_os): the Codex state database through the vendored
// SQLite, opened read-only, and the default paths of the agents' folders.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "agents/context.h"

namespace kt::agents {

// Reads <dir>/state_<n>.sqlite (the highest n), table `threads`, read-only (`mode=ro`, never
// `immutable`: Codex writes it with WAL). Each query opens the database and closes it again, so
// nothing stays open between the probe's rare calls. Any error (no file, schema change, locked)
// gives no rows.
class CodexSqliteIndex final : public ICodexIndex {
public:
    explicit CodexSqliteIndex(std::string dir) : dir_(std::move(dir)) {}
    std::vector<CodexThread> threads(std::string_view id, std::string_view cwd) const override;
    // The database file used, empty when there is none.
    std::string database() const;

private:
    std::string dir_;
};

// The Antigravity CLI's conversation databases, read-only (`mode=ro`: the CLI may be writing).
// Each call opens the database and closes it again; any error gives no rows.
class AgySqliteStore final : public IAgyStore {
public:
    std::vector<std::string> generations(const std::string& db_path, std::size_t max_rows) const override;
};

// <home>, Claude's config folder (agents.claude.config_dir, CLAUDE_CONFIG_DIR, ~/.claude),
// Codex's home (agents.codex.home, CODEX_HOME, ~/.codex), CODEX_SQLITE_HOME, Qoder's home.
ContextPaths default_context_paths(std::string_view configured_claude_dir, std::string_view configured_codex_home);

}  // namespace kt::agents
