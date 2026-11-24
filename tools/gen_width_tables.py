#!/usr/bin/env python3
"""Generate src/vt/width_tables.cpp, the cell width tables of the VT core.

Inputs are three files of the Unicode Character Database for one Unicode version:

    UnicodeData.txt       general categories (Mn, Me, Cf, Cc)
    EastAsianWidth.txt    W, F, A
    emoji-data.txt        Emoji_Presentation

Usage:
    gen_width_tables.py --ucd DIR [--out FILE]
    gen_width_tables.py --fetch 16.0.0 --ucd DIR [--out FILE]   # download first

Stdlib only. The output is a two-level lookup table (2 bits per code point) plus a sorted
range list for Emoji_Presentation. Bump kTableVersion in src/vt/width.h whenever the
output changes.

Classes (match kt::vt::width::W):
    0 Zero       C0/C1 controls, Mn, Me, Cf, Hangul medial vowels and final consonants
    1 Narrow     everything else
    2 Wide       East Asian Wide/Fullwidth, Emoji_Presentation=Yes
    3 Ambiguous  East Asian Ambiguous (width 1, or 2 with ambiguous_is_wide)

keepterm overrides (applied last, in this order):
    U+00AD SOFT HYPHEN                  Narrow (wcwidth tradition; it is Cf)
    U+E000..U+F8FF, planes 15 and 16    Narrow (Private Use Area: icon fonts are drawn narrow)
    U+1F1E6..U+1F1FF regional indicators Narrow outside a flag pair (pairs need mode 2027)
    U+1160..U+11FF, U+D7B0..U+D7FF      Zero (Hangul Jamo that conjoin with the preceding syllable)
"""

import argparse
import os
import sys
import urllib.request

MAX_CP = 0x110000
ZERO, NARROW, WIDE, AMBIGUOUS = 0, 1, 2, 3

UCD_FILES = {
    "UnicodeData.txt": "ucd/UnicodeData.txt",
    "EastAsianWidth.txt": "ucd/EastAsianWidth.txt",
    "emoji-data.txt": "ucd/emoji/emoji-data.txt",
}

# Unassigned code points in these ranges default to W (header of EastAsianWidth.txt).
EAW_DEFAULT_WIDE = [
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xF900, 0xFAFF),
    (0x20000, 0x2FFFD),
    (0x30000, 0x3FFFD),
]


def fetch(version, ucd_dir):
    os.makedirs(ucd_dir, exist_ok=True)
    for name, rel in UCD_FILES.items():
        url = f"https://www.unicode.org/Public/{version}/{rel}"
        print(f"fetching {url}", file=sys.stderr)
        with urllib.request.urlopen(url) as r, open(os.path.join(ucd_dir, name), "wb") as f:
            f.write(r.read())


def parse_range(field):
    field = field.strip()
    if ".." in field:
        a, b = field.split("..")
        return int(a, 16), int(b, 16)
    v = int(field, 16)
    return v, v


def property_lines(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            data = line.split("#", 1)[0].strip()
            if not data:
                continue
            fields = [x.strip() for x in data.split(";")]
            yield fields


def file_version(path):
    # First line looks like "# EastAsianWidth-16.0.0.txt".
    with open(path, encoding="utf-8") as f:
        first = f.readline()
    name = first.lstrip("# ").strip()
    if "-" in name:
        return name.rsplit("-", 1)[1].removesuffix(".txt")
    return None


def emoji_file_version(path):
    # emoji-data.txt names its version in a comment: "Used with Emoji Version 16.0 ...".
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("#"):
                break
            marker = "Emoji Version "
            if marker in line:
                return line.split(marker, 1)[1].split()[0]
    return None


def load_categories(path):
    """Returns a dict cp -> general category for assigned code points (ranges expanded)."""
    cats = {}
    range_start = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            fields = line.split(";")
            if len(fields) < 3:
                continue
            cp = int(fields[0], 16)
            name, cat = fields[1], fields[2]
            if name.endswith(", First>"):
                range_start = cp
                continue
            if name.endswith(", Last>") and range_start is not None:
                for c in range(range_start, cp + 1):
                    cats[c] = cat
                range_start = None
                continue
            cats[cp] = cat
    return cats


def build(ucd_dir):
    eaw_path = os.path.join(ucd_dir, "EastAsianWidth.txt")
    emoji_path = os.path.join(ucd_dir, "emoji-data.txt")
    version = file_version(eaw_path)
    emoji_version = emoji_file_version(emoji_path)
    if version is None or emoji_version is None or not version.startswith(emoji_version + "."):
        sys.exit("cannot determine a common Unicode version from the input files")

    cls = bytearray([NARROW]) * MAX_CP

    # East Asian Width.
    for a, b in EAW_DEFAULT_WIDE:
        for c in range(a, b + 1):
            cls[c] = WIDE
    for fields in property_lines(eaw_path):
        a, b = parse_range(fields[0])
        v = fields[1]
        val = WIDE if v in ("W", "F") else AMBIGUOUS if v == "A" else NARROW
        for c in range(a, b + 1):
            cls[c] = val

    # Emoji_Presentation=Yes is wide.
    emoji_pres = []
    for fields in property_lines(emoji_path):
        if fields[1] != "Emoji_Presentation":
            continue
        a, b = parse_range(fields[0])
        emoji_pres.append((a, b))
        for c in range(a, b + 1):
            cls[c] = WIDE

    # Zero width: controls, nonspacing and enclosing marks, format characters.
    cats = load_categories(os.path.join(ucd_dir, "UnicodeData.txt"))
    for c, cat in cats.items():
        if cat in ("Mn", "Me", "Cf", "Cc"):
            cls[c] = ZERO

    # keepterm overrides.
    cls[0x00AD] = NARROW
    for a, b in ((0xE000, 0xF8FF), (0xF0000, 0xFFFFD), (0x100000, 0x10FFFD)):
        for c in range(a, b + 1):
            cls[c] = NARROW
    for c in range(0x1F1E6, 0x1F1FF + 1):
        cls[c] = NARROW
    for a, b in ((0x1160, 0x11FF), (0xD7B0, 0xD7FF)):
        for c in range(a, b + 1):
            cls[c] = ZERO
    # Surrogates never reach the table (the UTF-8 decoder rejects them); keep them narrow.
    for c in range(0xD800, 0xE000):
        cls[c] = NARROW

    return version, cls, merge_ranges(emoji_pres)


def merge_ranges(ranges):
    out = []
    for a, b in sorted(ranges):
        if out and a <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return out


def two_level(cls, block_bits):
    block = 1 << block_bits
    stage1, blocks, index = [], [], {}
    for start in range(0, MAX_CP, block):
        packed = bytearray(block // 4)
        for i in range(block):
            packed[i >> 2] |= cls[start + i] << ((i & 3) * 2)
        key = bytes(packed)
        if key not in index:
            index[key] = len(blocks)
            blocks.append(key)
        stage1.append(index[key])
    return stage1, blocks


def fmt_array(values, per_line=24):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    return "\n".join(lines)


def emit(version, cls, emoji_pres):
    best = None
    for bits in range(6, 11):
        s1, blocks = two_level(cls, bits)
        if len(blocks) > 0xFFFF:
            continue
        s1_size = len(s1) * (1 if len(blocks) <= 0x100 else 2)
        total = s1_size + len(blocks) * len(blocks[0])
        if best is None or total < best[0]:
            best = (total, bits, s1, blocks)
    total, bits, s1, blocks = best
    s1_type = "uint8_t" if len(blocks) <= 0x100 else "uint16_t"
    stage2 = [b for blk in blocks for b in blk]

    out = []
    out.append("// Generated by tools/gen_width_tables.py. Do not edit.")
    out.append(f"// Unicode {version}: UnicodeData.txt, EastAsianWidth.txt, emoji-data.txt.")
    out.append(f"// Two-level table, {1 << bits} code points per block, {len(blocks)} unique blocks, "
               f"{total} bytes.")
    out.append("")
    out.append('#include "vt/width_tables.h"')
    out.append("")
    out.append("namespace kt::vt::width::detail {")
    out.append("")
    out.append(f'const char kUnicodeVersion[] = "{version}";')
    out.append("")
    out.append(f"const {s1_type} kStage1[{len(s1)}] = {{")
    out.append(fmt_array(s1))
    out.append("};")
    out.append("")
    out.append(f"const uint8_t kStage2[{len(stage2)}] = {{")
    out.append(fmt_array(stage2))
    out.append("};")
    out.append("")
    out.append(f"const CodeRange kEmojiPresentation[{len(emoji_pres)}] = {{")
    for a, b in emoji_pres:
        out.append(f"    {{0x{a:04X}, 0x{b:04X}}},")
    out.append("};")
    out.append(f"const uint32_t kEmojiPresentationCount = {len(emoji_pres)};")
    out.append("")
    out.append("}  // namespace kt::vt::width::detail")
    out.append("")

    header = []
    header.append("// Generated by tools/gen_width_tables.py. Do not edit.")
    header.append("// Internal to src/vt/width.cpp.")
    header.append("#pragma once")
    header.append("")
    header.append("#include <cstdint>")
    header.append("")
    header.append("namespace kt::vt::width::detail {")
    header.append("")
    header.append(f"inline constexpr uint32_t kBlockBits = {bits};")
    header.append(f"using Stage1 = {s1_type};")
    header.append("struct CodeRange {")
    header.append("    uint32_t first, last;")
    header.append("};")
    header.append("")
    header.append("extern const char kUnicodeVersion[];")
    header.append(f"extern const Stage1 kStage1[{len(s1)}];")
    header.append(f"extern const uint8_t kStage2[{len(stage2)}];")
    header.append(f"extern const CodeRange kEmojiPresentation[{len(emoji_pres)}];")
    header.append("extern const uint32_t kEmojiPresentationCount;")
    header.append("")
    header.append("}  // namespace kt::vt::width::detail")
    header.append("")
    return "\n".join(out), "\n".join(header)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ucd", required=True, help="directory with the UCD input files")
    ap.add_argument("--fetch", metavar="VERSION", help="download the input files for VERSION first")
    ap.add_argument("--out", default=None, help="output .cpp (default: src/vt/width_tables.cpp)")
    args = ap.parse_args()

    if args.fetch:
        fetch(args.fetch, args.ucd)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_cpp = args.out or os.path.join(root, "src", "vt", "width_tables.cpp")
    out_h = os.path.splitext(out_cpp)[0] + ".h"

    version, cls, emoji_pres = build(args.ucd)
    cpp, h = emit(version, cls, emoji_pres)
    with open(out_cpp, "w", encoding="utf-8", newline="\n") as f:
        f.write(cpp)
    with open(out_h, "w", encoding="utf-8", newline="\n") as f:
        f.write(h)
    print(f"wrote {out_cpp} and {out_h} (Unicode {version})", file=sys.stderr)


if __name__ == "__main__":
    main()
