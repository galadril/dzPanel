#!/usr/bin/env python3
"""Measure the dzPanel brand blue from docs/banner.png (stdlib only).

Usage: python3 -I tools/brand/extract_colours.py docs/banner.png

Samples saturated blue pixels in the "Panel" glyph area and in the house icon of the 1672x941 banner and prints
median / top / bottom colours. The median of the Panel glyphs is what ru_theme_t.brand uses."""
import struct
import sys
import zlib

REGIONS = {
    "Panel": (830, 45, 1215, 150),
    "house": (470, 20, 610, 160),
}


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG")
    pos, idat, hdr, plte = 8, b"", None, None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = hdr
    if depth != 8 or interlace or ctype not in (0, 2, 3, 4, 6):
        raise SystemExit("unsupported PNG variant (need 8-bit, non-interlaced)")
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = w * ch
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        cur = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = cur[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1:
                cur[i] = (cur[i] + a) & 255
            elif f == 2:
                cur[i] = (cur[i] + b) & 255
            elif f == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        rows.append(cur)
        prev = cur

    def pixel(x, y):
        r = rows[y]
        if ctype == 3:
            i = r[x] * 3
            return plte[i], plte[i + 1], plte[i + 2]
        if ctype in (0, 4):
            return (r[x * ch],) * 3
        return r[x * ch], r[x * ch + 1], r[x * ch + 2]

    return w, h, pixel


def is_brand_blue(r, g, b):
    # saturated, bright blue: excludes white text, dark background and the glow/anti-aliased edges
    return b >= 200 and b - r >= 140 and r < 90


def med(v):
    v = sorted(v)
    return v[len(v) // 2]


def hexc(c):
    return "#%02X%02X%02X" % c


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    w, h, pixel = read_png(sys.argv[1])
    print("image %dx%d" % (w, h))
    for name, (x0, y0, x1, y1) in REGIONS.items():
        pts = []
        for y in range(y0, min(y1, h)):
            for x in range(x0, min(x1, w)):
                r, g, b = pixel(x, y)
                if is_brand_blue(r, g, b):
                    pts.append((y, (r, g, b)))
        if not pts:
            print("%s: no blue pixels" % name)
            continue
        cols = [c for _, c in pts]
        m = tuple(med([c[i] for c in cols]) for i in range(3))
        ys = sorted(p[0] for p in pts)
        lo, hi = ys[len(ys) // 10], ys[len(ys) * 9 // 10]
        top = [c for y, c in pts if y <= lo + (hi - lo) // 5]
        bot = [c for y, c in pts if y >= hi - (hi - lo) // 5]
        t = tuple(med([c[i] for c in top]) for i in range(3))
        bo = tuple(med([c[i] for c in bot]) for i in range(3))
        print("%s: %d px  median %s  top %s  bottom %s" % (name, len(pts), hexc(m), hexc(t), hexc(bo)))


main()
