// The session host role: `keepterm --session-host <id> ...`.
//
// The host owns the PTY, the authoritative vt::Terminal, the history journal and the endpoint.
// It answers terminal queries itself, serves several clients, and ends when the shell ends.
//
// Command line (built by session/spawn.h, never typed by users):
//   --session-host <id>        the new session's id
//   --ready <fd|handle>        ready channel inherited from spawn_detached
//   --restore-from <old-id>    recover the history of a dead session first
//   --cwd <dir>                start folder (nearest existing parent, then home)
//   --size <cols>x<rows>
//   --no-history               no journal, no checkpoints
//   --journal-fallback         buffered journal writer instead of the mapping (tests)
//   --pty=<auto|posix|conpty|agent>
//   --scrollback <lines>
//   --ring <bytes>             journal ring size (default 8 MiB)
//   --checkpoint-bytes <n>     output between checkpoints (default 4 MiB)
//   --linger <ms>              wait for EXIT_ACK after the shell ended (default 3000)
//   -- <argv...>               program to run instead of the user's shell
#pragma once

namespace kt::session {

// argv[i] is "--session-host". Returns the process exit code.
int host_main(int argc, char** argv, int i);

}  // namespace kt::session
