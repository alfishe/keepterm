#!/usr/bin/env python3
"""Draws the keepterm application icon and writes every form the builds use.

The icon is described by a few shapes (a rounded tile with a diagonal dusk-blue gradient, a light
prompt chevron and a steel-blue block cursor) and rasterized here with signed distances, one render per size, so small sizes are drawn
for their grid instead of scaled down. Pure Python (zlib only): the output does not depend on
the machine that runs it.

Outputs (committed; --check compares instead of writing):

  assets/icon/keepterm.svg                  the same drawing as a vector (Linux scalable icon)
  assets/icon/png/keepterm-<n>.png          16 ... 1024 (Linux hicolor, documentation)
  assets/icon/keepterm.ico                  Windows: 16 20 24 32 40 48 64 BMP entries, 256 PNG
  assets/icon/keepterm.icns                 macOS: PNG entries 16 ... 1024 (bundles)
  src/platform_gui/app_icon_rgba.inc        X11 _NET_WM_ICON: RGBA 16 32 48 64 128
  src/platform_gui/app_icon_png.inc         macOS Dock icon: the 1024 PNG

Usage:
  python3 tools/gen_icon.py [--check] [--preview DIR]
"""
import argparse
import math
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --------------------------------------------------------------------------------- the drawing

TOP = (0x3E, 0x55, 0x73)        # tile gradient, top left
BOTTOM = (0x1D, 0x2A, 0x3D)     # tile gradient, bottom right
RIM = (255, 255, 255, 0.10)     # inner edge light
CHEVRON = (0xE4, 0xEA, 0xF2)    # prompt
CURSOR = (0x9F, 0xB7, 0xD1)     # cursor


def layout(size):
    """The tile box (x0, y0, side) and its corner radius, in pixels of a `size` canvas."""
    if size >= 128:
        m = size * 100 / 1024     # the macOS icon grid: an 824 tile on 1024, room for the shadow
    else:
        m = max(0.0, size / 32)   # small sizes use the whole square
    side = size - 2 * m
    return m, m, side, side * 0.225


def shapes(size):
    """Everything to draw, back to front, in pixel coordinates."""
    x0, y0, side, radius = layout(size)

    def P(u, v):
        return (x0 + u * side, y0 + v * side)

    out = []
    if size >= 128:
        out.append(("shadow", (x0, y0 + side * 0.012, side, radius), side * 0.022, 0.40))
    out.append(("tile", (x0, y0, side, radius)))
    if size >= 32:
        out.append(("rim", (x0, y0, side, radius), max(1.0, side * 0.008)))
    if size >= 48:
        w = side * 0.109
        a, b, c = P(0.194, 0.340), P(0.388, 0.500), P(0.194, 0.642)
        # The cursor: a block with slightly rounded corners (x0, y0, x1, y1, r).
        cur = (*P(0.473, 0.607), *P(0.801, 0.714), side * 0.022)
    else:
        w = side * 0.11
        a, b, c = P(0.20, 0.30), P(0.44, 0.50), P(0.20, 0.70)
        cur = (*P(0.52, 0.70), *P(0.86, 0.70), 0.0)
        # Hinting for the small grids: whole-pixel strokes, the cursor's edges on pixel edges.
        w = max(2.0, float(round(w)))
        y = round(cur[1] - w / 2)
        cur = (float(round(cur[0])), float(y), float(round(cur[2])), float(y + w), 0.0)
    out.append(("bar", a, b, w / 2, CHEVRON, 1.0))
    out.append(("bar", b, c, w / 2, CHEVRON, 1.0))
    out.append(("block", cur, CURSOR))
    return out


def sd_round_box(px, py, box):
    x0, y0, side, r = box
    cx, cy, h = x0 + side / 2, y0 + side / 2, side / 2
    qx, qy = abs(px - cx) - h + r, abs(py - cy) - h + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def sd_block(px, py, blk):
    x0, y0, x1, y1, r = blk
    cx, cy, hx, hy = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    qx, qy = abs(px - cx) - hx + r, abs(py - cy) - hy + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def sd_segment(px, py, a, b, hw):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = min(1.0, max(0.0, t))
    return math.hypot(px - ax - t * dx, py - ay - t * dy) - hw


def cover(d):
    return min(1.0, max(0.0, 0.5 - d))


def over(dst, rgb, a):
    """Source-over of a straight-alpha colour onto a straight-alpha pixel."""
    if a <= 0.0:
        return dst
    r, g, b, da = dst
    oa = a + da * (1 - a)
    if oa <= 0.0:
        return (0.0, 0.0, 0.0, 0.0)
    k = da * (1 - a)
    return ((rgb[0] * a + r * k) / oa, (rgb[1] * a + g * k) / oa, (rgb[2] * a + b * k) / oa, oa)


def render(size):
    """RGBA bytes, straight alpha, rows top to bottom."""
    layers = shapes(size)
    x0, y0, side, _ = layout(size)
    px = bytearray(size * size * 4)
    for y in range(size):
        cy = y + 0.5
        for x in range(size):
            cx = x + 0.5
            # Diagonal gradient, top left to bottom right (the SVG's x1=0 y1=0 x2=1 y2=1).
            t = min(1.0, max(0.0, ((cx - x0) + (cy - y0)) / (2 * side)))
            grad = tuple(TOP[i] + (BOTTOM[i] - TOP[i]) * t for i in range(3))
            pix = (0.0, 0.0, 0.0, 0.0)
            for s in layers:
                kind = s[0]
                if kind == "shadow":
                    d = sd_round_box(cx, cy, s[1])
                    if d < s[2] * 3:
                        f = math.exp(-(max(0.0, d) / s[2]) ** 2)   # a soft, gaussian-like edge
                        pix = over(pix, (0, 0, 0), s[3] * f)
                elif kind == "tile":
                    pix = over(pix, grad, cover(sd_round_box(cx, cy, s[1])))
                elif kind == "rim":
                    d = sd_round_box(cx, cy, s[1])
                    if -s[2] - 1 < d < 1:
                        ring = cover(d) * cover(-d - s[2])
                        pix = over(pix, RIM[:3], RIM[3] * ring)
                elif kind == "block":
                    pix = over(pix, s[2], cover(sd_block(cx, cy, s[1])))
                else:
                    _, a, b, hw, rgb, alpha = s
                    lo_x, hi_x = min(a[0], b[0]) - hw - 1, max(a[0], b[0]) + hw + 1
                    lo_y, hi_y = min(a[1], b[1]) - hw - 1, max(a[1], b[1]) + hw + 1
                    if lo_x <= cx <= hi_x and lo_y <= cy <= hi_y:
                        pix = over(pix, rgb, alpha * cover(sd_segment(cx, cy, a, b, hw)))
            i = (y * size + x) * 4
            px[i] = int(round(pix[0])) if pix[3] > 0 else 0
            px[i + 1] = int(round(pix[1])) if pix[3] > 0 else 0
            px[i + 2] = int(round(pix[2])) if pix[3] > 0 else 0
            px[i + 3] = int(round(pix[3] * 255))
    return bytes(px)


def svg():
    """The drawing at 1024 as SVG (same shapes; the shadow is a blur filter)."""
    x0, y0, side, r = layout(1024)
    f = lambda v: f"{v:.2f}".rstrip("0").rstrip(".")
    lines = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">',
        "  <defs>",
        '    <linearGradient id="tile" x1="0" y1="0" x2="1" y2="1">',
        f'      <stop offset="0" stop-color="#{TOP[0]:02x}{TOP[1]:02x}{TOP[2]:02x}"/>',
        f'      <stop offset="1" stop-color="#{BOTTOM[0]:02x}{BOTTOM[1]:02x}{BOTTOM[2]:02x}"/>',
        "    </linearGradient>",
        f'    <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="{f(side * 0.011)}"/></filter>',
        "  </defs>",
        f'  <rect x="{f(x0)}" y="{f(y0 + side * 0.012)}" width="{f(side)}" height="{f(side)}" rx="{f(r)}" fill="#000" opacity="0.45" filter="url(#shadow)"/>',
        f'  <rect x="{f(x0)}" y="{f(y0)}" width="{f(side)}" height="{f(side)}" rx="{f(r)}" fill="url(#tile)"/>',
    ]
    rim = side * 0.008
    lines.append(
        f'  <rect x="{f(x0 + rim / 2)}" y="{f(y0 + rim / 2)}" width="{f(side - rim)}" height="{f(side - rim)}" rx="{f(r - rim / 2)}" '
        f'fill="none" stroke="#fff" stroke-opacity="{RIM[3]}" stroke-width="{f(rim)}"/>')
    hexc = lambda c: f"#{c[0]:02x}{c[1]:02x}{c[2]:02x}"
    for s in shapes(1024):
        if s[0] == "block":
            bx0, by0, bx1, by1, br = s[1]
            lines.append(f'  <rect x="{f(bx0)}" y="{f(by0)}" width="{f(bx1 - bx0)}" height="{f(by1 - by0)}" rx="{f(br)}" '
                         f'fill="{hexc(s[2])}"/>')
            continue
        if s[0] != "bar":
            continue
        _, a, b, hw, rgb, alpha = s
        op = "" if alpha >= 1 else f' stroke-opacity="{alpha}"'
        lines.append(f'  <line x1="{f(a[0])}" y1="{f(a[1])}" x2="{f(b[0])}" y2="{f(b[1])}" stroke="{hexc(rgb)}"{op} '
                     f'stroke-width="{f(hw * 2)}" stroke-linecap="round"/>')
    lines.append("</svg>")
    return "\n".join(lines) + "\n"

# ------------------------------------------------------------------------------------ formats


def png(size, rgba):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico_bmp(size, rgba):
    """A 32-bit DIB entry: BITMAPINFOHEADER (double height), BGRA rows bottom up, the AND mask."""
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in range(size - 1, -1, -1):
        row = bytearray()
        for x in range(size):
            i = (y * size + x) * 4
            row += bytes((rgba[i + 2], rgba[i + 1], rgba[i], rgba[i + 3]))
        rows.append(bytes(row))
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(size):
            if rgba[(y * size + x) * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return hdr + b"".join(rows) + bytes(mask)


def ico(images):
    """images: [(size, rgba)]; 256 is stored as PNG, the rest as DIBs."""
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for size, rgba in images:
        blob = png(size, rgba) if size >= 256 else ico_bmp(size, rgba)
        dim = 0 if size >= 256 else size
        entries.append(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    return struct.pack("<HHH", 0, 1, len(images)) + b"".join(entries) + b"".join(blobs)


ICNS_TYPES = [("icp4", 16), ("icp5", 32), ("ic07", 128), ("ic08", 256), ("ic09", 512),
              ("ic10", 1024), ("ic11", 32), ("ic12", 64), ("ic13", 256), ("ic14", 512)]


def icns(pngs):
    body = b""
    for tag, size in ICNS_TYPES:
        data = pngs[size]
        body += tag.encode() + struct.pack(">I", 8 + len(data)) + data
    return b"icns" + struct.pack(">I", 8 + len(body)) + body


def c_array(name, data, per_line=24):
    out = [f"// {len(data)} bytes"]
    out.append(f"alignas(4) static const unsigned char {name}[] = {{")
    for i in range(0, len(data), per_line):
        out.append("    " + ",".join(str(b) for b in data[i:i + per_line]) + ",")
    out.append("};")
    return "\n".join(out) + "\n"


HEADER = "// Generated by tools/gen_icon.py; do not edit. The drawing is described in that script.\n"

# --------------------------------------------------------------------------------------- main

PNG_SIZES = [16, 32, 48, 64, 128, 256, 512, 1024]
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
RGBA_SIZES = [16, 32, 48, 64, 128]


def outputs():
    sizes = sorted(set(PNG_SIZES) | set(ICO_SIZES) | set(RGBA_SIZES))
    rgba = {}
    for s in sizes:
        rgba[s] = render(s)
    pngs = {s: png(s, rgba[s]) for s in sizes}
    files = {}
    files["assets/icon/keepterm.svg"] = svg().encode()
    for s in PNG_SIZES:
        files[f"assets/icon/png/keepterm-{s}.png"] = pngs[s]
    files["assets/icon/keepterm.ico"] = ico([(s, rgba[s]) for s in ICO_SIZES])
    files["assets/icon/keepterm.icns"] = icns(pngs)
    inc = [HEADER, "// The application icon for X11 (_NET_WM_ICON): straight-alpha RGBA, rows top to bottom.\n"]
    for s in RGBA_SIZES:
        inc.append(c_array(f"kIconRgba{s}", rgba[s]))
    inc.append("struct IconImage { int size; const unsigned char* rgba; };\n")
    inc.append("static const IconImage kIconImages[] = {" +
               ", ".join(f"{{{s}, kIconRgba{s}}}" for s in RGBA_SIZES) + "};\n")
    files["src/platform_gui/app_icon_rgba.inc"] = "".join(inc).encode()
    files["src/platform_gui/app_icon_png.inc"] = (
        HEADER + "// The application icon for the macOS Dock: a 1024x1024 PNG.\n" +
        c_array("kIconPng1024", pngs[1024])).encode()
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="compare with the committed files; exit 1 on a difference")
    ap.add_argument("--preview", help="write only the PNGs to this folder")
    a = ap.parse_args()
    if a.preview:
        os.makedirs(a.preview, exist_ok=True)
        for s in [16, 32, 48, 64, 128, 256, 1024]:
            with open(os.path.join(a.preview, f"keepterm-{s}.png"), "wb") as f:
                f.write(png(s, render(s)))
        return 0
    bad = 0
    for rel, data in outputs().items():
        path = os.path.join(ROOT, rel)
        if a.check:
            try:
                with open(path, "rb") as f:
                    same = f.read() == data
            except OSError:
                same = False
            if not same:
                print(f"differs: {rel}")
                bad += 1
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        print(f"wrote {rel} ({len(data)} bytes)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
