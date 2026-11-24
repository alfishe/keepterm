// The resume line typed at an idle shell prompt, quoted per shell. Core code.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agents/agent_resume.h"
#include "base/error.h"

namespace kt::agents {

enum class ShellKind : std::uint8_t { Posix, PowerShell, Cmd, Unsupported };
// The path convention of the machine the pane runs on: Windows normalizes the directory to '\'
// for PowerShell and cmd and to '/' for a POSIX shell (Git Bash, MSYS2).
enum class PathStyle : std::uint8_t { Posix, Windows };

// From the idle shell's image name: pwsh, powershell -> PowerShell; cmd -> Cmd; bash, zsh, fish,
// sh, dash, ksh -> Posix; anything else (wsl, nu, elvish, xonsh, ...) -> Unsupported.
ShellKind shell_kind(std::string_view image);

// Every quoting function refuses C0 control characters and DEL: typed into a terminal they act
// as keys (^C, ^U, Enter) instead of text.
//
// POSIX sh: unchanged if non-empty and every character is in [A-Za-z0-9-_./=:@%+,]; otherwise
// '...' with each ' written as '\''.
Result<std::string> quote_posix(std::string_view word);
// PowerShell: '...', with ' and U+2018..U+201B doubled. `native_argument` also refuses a '"' and
// a trailing '\' in a word with whitespace, which Windows PowerShell 5.1 passes to native programs
// unescaped.
Result<std::string> quote_powershell(std::string_view word, bool native_argument = true);
// cmd.exe: "..." when the word is empty or has a space or one of &|<>^(),;= (inside quotes these
// are literal, so no caret is needed); trailing backslashes are doubled before the closing quote
// (CommandLineToArgvW). Refuses %, !, " and control characters: cmd expands %VAR% even inside
// quotes, has no reliable escape for it on an interactive line, and ! expands with delayed
// expansion.
Result<std::string> quote_cmd(std::string_view word);

// [descriptor.command] + record.arguments + descriptor.resume_args with "{id}" replaced.
std::vector<std::string> resume_words(const AgentResume& record, const AgentDescriptor& d);

// POSIX:      cd <dir> && claude <args> --resume <id>
// PowerShell: if (Set-Location -LiteralPath '<dir>' -PassThru -ErrorAction SilentlyContinue) { & 'claude' ... }
// cmd:        cd /d "<dir>" && claude <args> --resume <id>
// Errors: Unsupported shell, an agent without resume arguments, an empty directory or session id,
// or a word that cannot be quoted for that shell. The restore then skips the pane.
Result<std::string> resume_command(const AgentResume& record, const AgentDescriptor& d, ShellKind shell,
                                   PathStyle style);

}  // namespace kt::agents
