#!/usr/bin/env python3
"""Rasterise Lucide SVG icons into an LVGL 9 bitmap font (4 bpp, uncompressed) plus a C header with UTF-8 macros.

Why this and not lv_font_conv: the build host has no access to the npm registry, and lv_font_conv needs it. Lucide's
SVGs are plain strokes (stroke-width 2, round caps and joins, 24x24 grid) made of paths and circles, so a tiny stroker
is enough: every path is flattened into line segments and a pixel is covered when its sub-samples lie within half the
stroke width of a segment (round caps and joins come for free). Python standard library only.

Usage (from the repository root; this is the exact command the committed files came from):
  python3 -I tools/icons/gen_icons.py --size 24
writes components/ru_ui/fonts/ru_icons_24.c and components/ru_ui/include/ru_icons.h
Inputs: tools/icons/manifest.txt (codepoint + icon name) and tools/icons/lucide/<name>.svg (Lucide 1.70.0, ISC, see
docs/third-party/SOURCES.md). Output is deterministic.
"""
import argparse
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SS = 8  # sub-samples per pixel axis

NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


def tokens(d):
    i, n = 0, len(d)
    while i < n:
        c = d[i]
        if c.isalpha():
            yield c
            i += 1
        elif c in " ,\t\r\n":
            i += 1
        else:
            m = NUM.match(d, i)
            if not m:
                raise ValueError("bad path data near %r" % d[i:i + 10])
            yield float(m.group(0))
            i = m.end()


def arc_points(x1, y1, rx, ry, phi, fa, fs, x2, y2):
    """SVG endpoint arc -> list of points (excluding the start)."""
    if rx == 0 or ry == 0 or (x1 == x2 and y1 == y2):
        return [(x2, y2)]
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2.0, (y1 - y2) / 2.0
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) * (-1 if fa == fs else 1)
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2.0
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2.0

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    th1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dth = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dth > 0:
        dth -= 2 * math.pi
    elif fs and dth < 0:
        dth += 2 * math.pi
    steps = max(4, int(abs(dth) * max(rx, ry) / 0.25) + 1)
    pts = []
    for k in range(1, steps + 1):
        t = th1 + dth * k / steps
        x = cp * rx * math.cos(t) - sp * ry * math.sin(t) + cx
        y = sp * rx * math.cos(t) + cp * ry * math.sin(t) + cy
        pts.append((x, y))
    pts[-1] = (x2, y2)
    return pts


def bez(p0, p1, p2, p3):
    L = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in ((p0, p1), (p1, p2), (p2, p3)))
    steps = max(4, int(L / 0.25))
    pts = []
    for k in range(1, steps + 1):
        t = k / steps
        u = 1 - t
        pts.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
    return pts


def parse_path(d):
    """-> list of (points, closed) sub-paths."""
    toks = list(tokens(d))
    i = 0
    subs = []
    cur = []
    x = y = sx = sy = 0.0
    cmd = None
    lastc = None  # last cubic control point (for S)
    lastq = None
    closed = False

    def num():
        nonlocal i
        v = toks[i]
        if not isinstance(v, float):
            raise ValueError("number expected")
        i += 1
        return v

    def flag():
        # arc flags may be glued to the next number ("01.5"): they are single characters 0 / 1
        nonlocal i
        v = toks[i]
        i += 1
        return int(v)

    while i < len(toks):
        if isinstance(toks[i], str):
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    subs.append((cur, True))
                    cur = []
                x, y = sx, sy
                lastc = lastq = None
                continue
        elif cmd is None:
            raise ValueError("path must start with a command")
        rel = cmd.islower()
        C = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if C == "M":
            if cur:
                subs.append((cur, False))
            x, y = num() + ox, num() + oy
            sx, sy = x, y
            cur = [(x, y)]
            cmd = "l" if rel else "L"  # implicit lineto
            lastc = lastq = None
            continue
        if not cur:
            cur = [(x, y)]
        if C == "L":
            x, y = num() + ox, num() + oy
            cur.append((x, y))
            lastc = lastq = None
        elif C == "H":
            x = num() + ox
            cur.append((x, y))
            lastc = lastq = None
        elif C == "V":
            y = num() + oy
            cur.append((x, y))
            lastc = lastq = None
        elif C == "C":
            c1 = (num() + ox, num() + oy)
            c2 = (num() + ox, num() + oy)
            e = (num() + ox, num() + oy)
            cur += bez((x, y), c1, c2, e)
            lastc, lastq = c2, None
            x, y = e
        elif C == "S":
            c1 = (2 * x - lastc[0], 2 * y - lastc[1]) if lastc else (x, y)
            c2 = (num() + ox, num() + oy)
            e = (num() + ox, num() + oy)
            cur += bez((x, y), c1, c2, e)
            lastc, lastq = c2, None
            x, y = e
        elif C in "QT":
            if C == "Q":
                q = (num() + ox, num() + oy)
            else:
                q = (2 * x - lastq[0], 2 * y - lastq[1]) if lastq else (x, y)
            e = (num() + ox, num() + oy)
            c1 = (x + 2.0 / 3 * (q[0] - x), y + 2.0 / 3 * (q[1] - y))
            c2 = (e[0] + 2.0 / 3 * (q[0] - e[0]), e[1] + 2.0 / 3 * (q[1] - e[1]))
            cur += bez((x, y), c1, c2, e)
            lastq, lastc = q, None
            x, y = e
        elif C == "A":
            rx, ry, rot = num(), num(), num()
            fa, fs = flag(), flag()
            ex, ey = num() + ox, num() + oy
            cur += arc_points(x, y, rx, ry, math.radians(rot), fa, fs, ex, ey)
            x, y = ex, ey
            lastc = lastq = None
        else:
            raise ValueError("unsupported path command " + cmd)
    if cur:
        subs.append((cur, False))
    return subs


def attrs(tag):
    return dict(re.findall(r'([\w:-]+)\s*=\s*"([^"]*)"', tag))


def load_svg(path):
    text = open(path, encoding="utf-8").read()
    subs = []
    sw = 2.0
    m = re.search(r'stroke-width\s*=\s*"([\d.]+)"', text[:text.find(">")])
    if m:
        sw = float(m.group(1))
    for tag in re.findall(r"<(?:path|circle|line|polyline|polygon|ellipse|rect)\b[^>]*>", text):
        a = attrs(tag)
        name = re.match(r"<(\w+)", tag).group(1)
        if name == "path":
            subs += parse_path(a["d"])
        elif name == "circle" or name == "ellipse":
            cx, cy = float(a["cx"]), float(a["cy"])
            rx = float(a.get("r", a.get("rx", 0)))
            ry = float(a.get("r", a.get("ry", 0)))
            pts = [(cx + rx * math.cos(2 * math.pi * k / 64), cy + ry * math.sin(2 * math.pi * k / 64)) for k in range(65)]
            subs.append((pts, True))
        elif name == "line":
            subs.append(([(float(a["x1"]), float(a["y1"])), (float(a["x2"]), float(a["y2"]))], False))
        elif name in ("polyline", "polygon"):
            v = [float(t) for t in NUM.findall(a["points"])]
            subs.append((list(zip(v[0::2], v[1::2])), name == "polygon"))
        else:
            raise ValueError("%s: unsupported element %s" % (path, name))
    return subs, sw


def rasterise(subs, sw, size):
    """-> size*size coverage values 0..255, row-major."""
    s = size / 24.0
    r = sw * s / 2.0
    n = size * SS
    hit = bytearray(n * n)
    segs = []
    for pts, closed in subs:
        p = [(x * s, y * s) for x, y in pts]
        if closed and p[0] != p[-1]:
            p.append(p[0])
        if len(p) == 1:
            p.append(p[0])
        segs += list(zip(p[:-1], p[1:]))
    step = 1.0 / SS
    for (ax, ay), (bx, by) in segs:
        x0 = max(0, int((min(ax, bx) - r) * SS) - 1)
        x1 = min(n - 1, int((max(ax, bx) + r) * SS) + 1)
        y0 = max(0, int((min(ay, by) - r) * SS) - 1)
        y1 = min(n - 1, int((max(ay, by) + r) * SS) + 1)
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        for j in range(y0, y1 + 1):
            py = (j + 0.5) * step
            row = j * n
            for i in range(x0, x1 + 1):
                px = (i + 0.5) * step
                if L2 > 0:
                    t = ((px - ax) * dx + (py - ay) * dy) / L2
                    t = 0.0 if t < 0 else 1.0 if t > 1 else t
                else:
                    t = 0.0
                qx, qy = ax + t * dx - px, ay + t * dy - py
                if qx * qx + qy * qy <= r * r:
                    hit[row + i] = 1
    out = []
    for py in range(size):
        for px in range(size):
            c = 0
            for j in range(SS):
                base = (py * SS + j) * n + px * SS
                c += sum(hit[base:base + SS])
            out.append((c * 255 + SS * SS // 2) // (SS * SS))
    return out


def utf8_c(cp):
    return "".join("\\x%02X" % b for b in chr(cp).encode("utf-8"))


def read_manifest(path):
    rows = []
    for ln in open(path, encoding="utf-8"):
        ln = ln.split("#", 1)[0].strip()
        if ln:
            cp, name = ln.split()
            rows.append((int(cp, 16), name))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=24)
    ap.add_argument("--manifest", default=os.path.join(HERE, "manifest.txt"))
    ap.add_argument("--svg-dir", default=os.path.join(HERE, "lucide"))
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "components", "ru_ui"))
    a = ap.parse_args()
    size = a.size
    rows = read_manifest(a.manifest)
    cps = [c for c, _ in rows]
    if cps != list(range(cps[0], cps[0] + len(cps))) or cps[0] < 0xE000 or cps[-1] >= 0xF000:
        sys.exit("manifest: codepoints must be contiguous inside U+E000..U+EFFF")
    bitmaps = []
    for cp, name in rows:
        subs, sw = load_svg(os.path.join(a.svg_dir, name + ".svg"))
        cov = rasterise(subs, sw, size)
        data = bytearray()
        for k in range(0, len(cov), 2):
            hi = (cov[k] * 15 + 127) // 255
            lo = (cov[k + 1] * 15 + 127) // 255 if k + 1 < len(cov) else 0
            data.append(hi << 4 | lo)
        bitmaps.append(bytes(data))
    per = len(bitmaps[0])
    fname = "ru_icons_%d" % size
    L = []
    L.append("/* GENERATED by tools/icons/gen_icons.py --size %d from Lucide 1.70.0 icons (ISC, see docs/third-party). Do not edit. */" % size)
    L.append('#include "lvgl.h"')
    L.append("")
    L.append("static const uint8_t glyph_bitmap[] = {")
    for (cp, name), bm in zip(rows, bitmaps):
        L.append("    /* U+%04X %s */" % (cp, name))
        for k in range(0, len(bm), 16):
            L.append("    " + ", ".join("0x%02x" % b for b in bm[k:k + 16]) + ",")
    L.append("};")
    L.append("")
    L.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    L.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
    for k, (cp, name) in enumerate(rows):
        L.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = 0, .ofs_y = 0}, /* U+%04X */"
                 % (k * per, size * 16, size, size, cp))
    L.append("};")
    L.append("")
    L.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    L.append("    {")
    L.append("        .range_start = 0x%X, .range_length = %d, .glyph_id_start = 1," % (cps[0], len(cps)))
    L.append("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
    L.append("    }")
    L.append("};")
    L.append("")
    L.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    L.append("    .glyph_bitmap = glyph_bitmap,")
    L.append("    .glyph_dsc = glyph_dsc,")
    L.append("    .cmaps = cmaps,")
    L.append("    .kern_dsc = NULL,")
    L.append("    .kern_scale = 0,")
    L.append("    .cmap_num = 1,")
    L.append("    .bpp = 4,")
    L.append("    .kern_classes = 0,")
    L.append("    .bitmap_format = 0,")
    L.append("};")
    L.append("")
    L.append("const lv_font_t %s = {" % fname)
    L.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    L.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    L.append("    .line_height = %d," % size)
    L.append("    .base_line = 0,")
    L.append("    .subpx = LV_FONT_SUBPX_NONE,")
    L.append("    .underline_position = 0,")
    L.append("    .underline_thickness = 0,")
    L.append("    .dsc = &font_dsc,")
    L.append("    .fallback = NULL,")
    L.append("    .user_data = NULL,")
    L.append("};")
    os.makedirs(os.path.join(a.out_dir, "fonts"), exist_ok=True)
    with open(os.path.join(a.out_dir, "fonts", fname + ".c"), "w", newline="\n") as f:
        f.write("\n".join(L) + "\n")

    H = []
    H.append("/* GENERATED by tools/icons/gen_icons.py (manifest tools/icons/manifest.txt). Lucide icons, ISC license (docs/third-party).")
    H.append(" * Each macro is the UTF-8 text of one private-use codepoint; show it in a label whose font is ru_font_icon_s() / ru_icons_24. */")
    H.append("#pragma once")
    H.append('#include "lvgl.h"')
    H.append("")
    H.append("extern const lv_font_t ru_icons_%d;" % size)
    H.append("#define RU_ICON_FIRST 0x%X" % cps[0])
    H.append("#define RU_ICON_COUNT %d" % len(cps))
    H.append("")
    for cp, name in rows:
        H.append('#define RU_ICON_%-18s "%s" /* U+%04X */' % (name.upper().replace("-", "_"), utf8_c(cp), cp))
    with open(os.path.join(a.out_dir, "include", "ru_icons.h"), "w", newline="\n") as f:
        f.write("\n".join(H) + "\n")
    print("wrote %s.c (%d glyphs, %d bytes of bitmap)" % (fname, len(rows), per * len(rows)))


if __name__ == "__main__":
    main()
