# Changelog

Every release has a section here; its text becomes the body of the GitHub release
(`tools/release_notes.py`). Add changes under `[Unreleased]` as they land. To release, rename
that heading to `## [X.Y.Z] - YYYY-MM-DD`, set `project(VERSION X.Y.Z)` in `CMakeLists.txt`,
start a new empty `[Unreleased]`, and push the tag `vX.Y.Z`.

## [Unreleased]

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
