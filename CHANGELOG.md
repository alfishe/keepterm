# Changelog

Every release has a section here; its text becomes the body of the GitHub release
(`tools/release_notes.py`). Add changes under `[Unreleased]` as they land. To release, rename
that heading to `## [X.Y.Z] - YYYY-MM-DD`, set `project(VERSION X.Y.Z)` in `CMakeLists.txt`,
start a new empty `[Unreleased]`, and push the tag `vX.Y.Z`.

## [Unreleased]

- **History is stored by default:** every pane that is not private writes its output, text and commands to the history
  store; closing a pane archives its history (final screen and manifest) instead of deleting it. `--no-store` on the
  session host or `session.history = false` turns it off. Retention deletes nothing unless a `storage.*` limit is set.
- **History survives crashes:** a session host killed at any moment, restored and killed again keeps every line and
  command once and in order; a dead session's folder is archived when its lock frees.
- **Closed sessions come back:** `keepterm --history list | show | restore` lists the sessions of the history, running and
  closed, with their title, project, folders, agent runs and commands; `restore` brings a closed one back in a new session
  host with its last screen, folder and title, under the same history entry, and `--resume-agent` resumes its agent.
  `rename`, `tag`, `pin` and `delete` edit the history; all of them are in MCP too.
- **Settings → History:** delete closed sessions after a period (never by default; pinned ones stay) with a preview of
  what a shorter period deletes before anything goes, compression age, store figures and Delete History…. The period can
  differ per project (`[[projects]]` `storage.keep_closed`); the janitor now applies project overrides and picks up config
  changes without a restart.
- **Session History window:** Tools → Session History, Cmd+Y (Ctrl+Shift+Y on Windows and Linux), also from the
  palette, the Sessions dialog, Settings → History and "Show in Session History" in the pane and tab menus. It lists
  every session, live, running without a tab and closed, grouped by day, project, agent or tag, with filters by state,
  date, project, tag and agent ("Any agent", each agent by name, "No agent", "Match all selected"). The search looks in
  names, projects, folders, agents, tags, commands and the stored output; every found row says what matched, the match
  marked, and the details list all matches. Show, Attach, Restore, Resume, rename, tag, pin and delete act on the
  selected session.
- **Search History in the palette:** sessions with their project, agents, tags and state; from three characters lines
  of their stored output, grouped by session, each under its session's name and context, the match marked. Enter on a
  line opens the pane with Find on it. A magnifier button in the sidebar's header (in the title bar while the sidebar is
  hidden) opens it.
- **Output viewer:** the stored output of a closed session in its own read-only window, at the line that was clicked:
  an output match or a command in Session History, or an output line in the palette (also a running session's line its
  pane no longer has). A find bar with the match count, previous and next; Jump to Previous / Next Prompt goes from
  command to command; selection, copy and the font size keys work as in a pane. Long sessions show up to 100,000 lines
  around the line, the rest counted.
- **Export a session's output:** Export… in Session History and in the output viewer, and `keepterm --history output
  <id> [--format text|raw|asciicast] [-o FILE]`: the text lines, the raw stream with its escape sequences, or an
  asciicast v2 recording for `asciinema play`, at the sizes the terminal had (sessions now store their size and every
  resize).
- **Find in terminal: regular expressions and scrollbar ticks.** The find bar's `.*` toggle searches with a regular
  expression (PCRE2, case-insensitive, within each logical line; an invalid pattern says why); the matches show as ticks
  on the scrollbar, the current one taller, while the bar is open. No pattern can hang or crash the app: its length,
  nesting and compiled size are limited, and so are the steps, depth, memory and text per line and the time of the whole
  search (checked inside a match); a search a limit stopped keeps what it found and shows "4 of 6+" with the reason.
- **Several sessions at once in Session History:** Cmd/Ctrl+click, Shift+click or Shift+arrows and Cmd/Ctrl+A select
  several; the details column then pins or unpins them, adds and removes tags on all of them, and deletes the closed
  ones after one confirmation. Esc goes back to one.
- **Fixed:** a restore started from Session History that failed said so in the main window, behind the history
  window; its progress and failure now show in the history window.
- **Fixed:** after a pane was resized with Find open, the match highlights stayed on the cells of the old line wrap.
- **Fixed:** on Windows, ending a session that had just started could time out: the host asked the console for its
  foreground process under the PTY lock, which takes seconds while the console starts on a busy machine, and the kill
  waited for that lock.
- **A Tools menu:** Session History, Search History, Sessions… and Reload Config moved out of the Window menu.
- **Fixed:** commands in the history carried the shell prompt in front ("user@host dir % git status"): zsh, bash and
  fish now mark the end of the prompt (OSC 133 B).
- **Fixed:** the history did not know a live session's project or its tab's name (only after a restore), so the window
  showed the folder for every session; the GUI now hands both to the session host.
- **MCP covers the whole CLI:** besides the window commands, an agent can list, inspect, read and type into sessions, check
  project files, list the app commands, search the history, and read resources, config, themes and fonts.
- **Fixed:** the lines of a session's last screen were stored with the time 0 when the session closed right after a
  checkpoint (`--history search` showed 1970).
- **Fixed:** with no `storage.max_total` the status bar read "2 KB of 17179869184.0 GB"; it shows the size alone.
- **Fixed:** `keepterm --history`, `--config` and `--theme` now use the profile of `KEEPTERM_PROFILE`, as the app and the
  panes do.
- **Fixed:** a crash when clicking in the Session History window (or anywhere a click rebuilds the widget it lands on):
  the window kept the removed widget as the press target. Pointer, wheel, key, text and hover dispatch now survive any
  handler that removes widgets, its own included.
- **Fixed:** a search mark in the Session History window or the palette could start or end inside a character when the
  store's match offset did not fit the line; marks cover whole characters.
- **Fixed:** Esc did not close the Settings window (only the Settings sheet); it does, after clearing a search.
- **Session History window zoom:** Increase / Decrease / Reset Font Size (Cmd/Ctrl + `=`, `-`, `0`) zoom its text and
  controls.
- **Text kept its old size after a scale change:** labels, buttons, badges and form controls keep their shaped text;
  when the window's scale changed (the zoom, a move to another display) only the root was laid out again, so text not
  laid out anew (the Session History facet headings) stayed at the old size. Every widget now drops its shaped text.
- **A session host given up by its spawner could live on unseen:** when the spawner was slow to look (a loaded
  machine), the host's ready line went into the still-open channel, the spawner closed it unread on its timeout, and
  the host took the write for a yes and kept its shell running with nobody attached. The spawner now acknowledges the
  line it takes (POSIX) or ends the child it writes off (Windows), and the timeout covers the whole start.
- **A forged snapshot could leave the terminal unable to snapshot itself:** the restored parser took any UTF-8
  decoder state, so a byte range that admits ESC as a continuation printed code point 0x1F401B, and the next snapshot
  of that terminal was rejected on restore. A sequence in progress must now be one the decoder could be in.
- **MSVC AddressSanitizer builds** (`KT_SANITIZE=address`) get `/bigobj` and the ASan runtime DLL next to the
  executables: the session host, started apart from the developer prompt, failed to load it.

## [0.1.0] - 2026-11-24

First public release.

- **Agent-aware panes:** Claude Code, Codex, Qoder CLI, Antigravity, Gemini CLI, Copilot CLI,
  Cursor Agent, OpenCode and Grok are recognized from the process tree; custom agents go into
  `[[agents.custom]]`.
- **Context gauge:** how full the context window is, for Claude Code, Codex, Qoder CLI and
  Antigravity; `keepterm --agent-limits` for the Claude usage limits.
- **Agents drive the terminal:** MCP server (`keepterm mcp config`) and the control CLI for
  projects, tabs, panes, windows and layouts, with `--json` output and shell completion.
- **State survives:** every pane is its own session host process; the window reattaches after a
  restart, and after a reboot panes are rebuilt in their folders and agent conversations resumed.
- **Workspace:** projects, tabs, splits, several windows per project, YAML layouts, quick terminal,
  private and read-only panes, command palette, rebindable keymaps, resource monitor.
- **Themes:** built-in light, dark and high-contrast, iTerm2-Color-Schemes and Ghostty theme files,
  follows the OS light/dark mode.
- **Platforms:** Windows 10, 11 and Windows Server 2016+ (own console agent on 1607, ConPTY on 1809+), macOS 13+
  (arm64, x86_64), Linux x86_64 (glibc 2.28+); Direct3D 11, Metal, Vulkan and CPU renderers.

Known gaps: the history store is off by default, no native Wayland backend, no terminal images,
no remote projects, unsigned builds.
