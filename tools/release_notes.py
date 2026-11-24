#!/usr/bin/env python3
"""Prints the CHANGELOG.md section of one version, the body of its GitHub release.

    tools/release_notes.py 0.2.0        # section "## [0.2.0] - <date>"
    tools/release_notes.py Unreleased   # what the next release will say

Exits 1 when the section is missing or empty, so a release tag without notes fails its build.
"""

import pathlib
import re
import sys


def section(text: str, version: str) -> str:
    head = re.compile(r"^## \[" + re.escape(version) + r"\](\s|$)")
    out, inside = [], False
    for line in text.splitlines():
        if line.startswith("## "):
            if inside:
                break
            inside = bool(head.match(line))
            continue
        if inside:
            out.append(line)
    return "\n".join(unwrap(out)).strip()


BLOCK_START = re.compile(r"^(#|\||```|[-*+] |\d+\. |> )")


def unwrap(lines):
    """Joins hard-wrapped lines: GitHub shows every newline of a release body as a line break."""
    out, fence = [], False
    for line in lines:
        text = line.strip()
        if text.startswith("```"):
            fence = not fence
            out.append(line)
            continue
        prev = out[-1].strip() if out else ""
        joinable = (not fence and text and prev and not BLOCK_START.match(text)
                    and not prev.startswith(("#", "|", "```")))
        if joinable:
            out[-1] = out[-1].rstrip() + " " + text
        else:
            out.append(line)
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    version = sys.argv[1].removeprefix("v")
    path = pathlib.Path(__file__).resolve().parent.parent / "CHANGELOG.md"
    body = section(path.read_text(encoding="utf-8"), version)
    if not body:
        print(f"CHANGELOG.md has no notes for [{version}]", file=sys.stderr)
        return 1
    print(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
