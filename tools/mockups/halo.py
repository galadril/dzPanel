#!/usr/bin/env python3
"""Halo UI mockups for dzPanel: stdlib-only rasteriser, writes PNGs.

Usage: python3 -I halo.py OUTDIR [SIZE]
Tokens come from DESIGN.md section 8. These are idea sketches, not the real
LVGL renderer: fonts are a 5x7 bitmap face, so only layout, colour and
geometry are meaningful.
"""
import math
import os
import struct
import sys
import zlib

EMBER = (255, 138, 61)
OK, WARN, ERR, INFO, STALE = (61, 220, 132), (255, 194, 71), (255, 90, 95), (77, 179, 255), (138, 147, 160)
SURFACE, WHITE, DIM = (14, 16, 19), (230, 233, 238), (110, 116, 126)

FONT = {  # 5x7, rows top to bottom, bit 4 = leftmost
    '0': "01110 10001 10011 10101 11001 10001 01110", '1': "00100 01100 00100 00100 00100 00100 01110",
    '2': "01110 10001 00001 00010 00100 01000 11111", '3': "11110 00001 00001 01110 00001 00001 11110",
    '4': "00010 00110 01010 10010 11111 00010 00010", '5': "11111 10000 11110 00001 00001 10001 01110",
    '6': "00110 01000 10000 11110 10001 10001 01110", '7': "11111 00001 00010 00100 01000 01000 01000",
    '8': "01110 10001 10001 01110 10001 10001 01110", '9': "01110 10001 10001 01111 00001 00010 01100",
    ':': "00000 00100 00100 00000 00100 00100 00000", '.': "00000 00000 00000 00000 00000 01100 01100",
    '-': "00000 00000 00000 11111 00000 00000 00000", '%': "11001 11010 00010 00100 01000 01011 10011",
    '°': "01100 10010 01100 00000 00000 00000 00000", '?': "01110 10001 00010 00100 00100 00000 00100",
    '+': "00000 00100 00100 11111 00100 00100 00000", '/': "00001 00010 00010 00100 01000 01000 10000",
    ' ': "00000 00000 00000 00000 00000 00000 00000",
    'A': "01110 10001 10001 11111 10001 10001 10001", 'B': "11110 10001 10001 11110 10001 10001 11110",
    'C': "01110 10001 10000 10000 10000 10001 01110", 'D': "11110 10001 10001 10001 10001 10001 11110",
    'E': "11111 10000 10000 11110 10000 10000 11111", 'F': "11111 10000 10000 11110 10000 10000 10000",
    'G': "01110 10001 10000 10111 10001 10001 01111", 'H': "10001 10001 10001 11111 10001 10001 10001",
    'I': "01110 00100 00100 00100 00100 00100 01110", 'K': "10001 10010 10100 11000 10100 10010 10001",
    'L': "10000 10000 10000 10000 10000 10000 11111", 'M': "10001 11011 10101 10101 10001 10001 10001",
    'N': "10001 11001 10101 10011 10001 10001 10001", 'O': "01110 10001 10001 10001 10001 10001 01110",
    'P': "11110 10001 10001 11110 10000 10000 10000", 'R': "11110 10001 10001 11110 10100 10010 10001",
    'S': "01111 10000 10000 01110 00001 00001 11110", 'T': "11111 00100 00100 00100 00100 00100 00100",
    'U': "10001 10001 10001 10001 10001 10001 01110", 'V': "10001 10001 10001 10001 10001 01010 00100",
    'W': "10001 10001 10001 10101 10101 11011 10001", 'X': "10001 10001 01010 00100 01010 10001 10001",
    'Y': "10001 10001 01010 00100 00100 00100 00100", 'Z': "11111 00001 00010 00100 01000 10000 11111",
    'J': "00111 00010 00010 00010 00010 10010 01100", 'Q': "01110 10001 10001 10001 10101 10010 01101",
}


class Canvas:
    def __init__(self, size):
        self.n = size
        self.c = size / 2
        self.px = [[0.0, 0.0, 0.0] for _ in range(size * size)]

    def blend(self, x, y, col, a):
        if a <= 0 or not (0 <= x < self.n and 0 <= y < self.n):
            return
        p = self.px[y * self.n + x]
        for i in range(3):
            p[i] += (col[i] - p[i]) * min(a, 1.0)

    def _scan(self, x0, y0, x1, y1, fn, col, alpha=1.0):
        for y in range(max(0, int(y0)), min(self.n, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(self.n, int(x1) + 1)):
                d = fn(x + .5, y + .5)  # signed distance, <0 inside
                a = max(0.0, min(1.0, 0.5 - d)) * alpha
                if a > 0:
                    self.blend(x, y, col, a)

    def disc(self, cx, cy, r, col, alpha=1.0):
        self._scan(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1,
                   lambda x, y: math.hypot(x - cx, y - cy) - r, col, alpha)

    def ring(self, cx, cy, r, w, col, a0=0.0, a1=360.0, alpha=1.0, cap=True):
        """Arc from a0 to a1 degrees, 0 = 12 o'clock, clockwise."""
        a0, a1 = a0 % 360, a1 if a1 == 360.0 else a1 % 360
        full = (a1 - a0) % 360 == 0 or a1 - a0 >= 360
        ro = r + w / 2

        def fn(x, y):
            d = abs(math.hypot(x - cx, y - cy) - r) - w / 2
            if full:
                return d
            ang = math.degrees(math.atan2(x - cx, -(y - cy))) % 360
            if (ang - a0) % 360 <= (a1 - a0) % 360:
                return d
            if not cap:
                return 99
            best = 99
            for a in (a0, a1):
                ex = cx + r * math.sin(math.radians(a))
                ey = cy - r * math.cos(math.radians(a))
                best = min(best, math.hypot(x - ex, y - ey) - w / 2)
            return best
        self._scan(cx - ro - 1, cy - ro - 1, cx + ro + 1, cy + ro + 1, fn, col, alpha)

    def glow_ring(self, cx, cy, r, w, col, a0, a1, steps=4):
        """Stepped glow: no blur, a few translucent widening strokes (as on device)."""
        for i in range(steps, 0, -1):
            self.ring(cx, cy, r, w + i * w * 0.9, col, a0, a1, alpha=0.07)
        self.ring(cx, cy, r, w, col, a0, a1)

    def rrect(self, x, y, w, h, rad, col, alpha=1.0, stroke=0):
        cx, cy = x + w / 2, y + h / 2

        def fn(px, py):
            qx, qy = abs(px - cx) - (w / 2 - rad), abs(py - cy) - (h / 2 - rad)
            d = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - rad
            return abs(d + stroke / 2) - stroke / 2 if stroke else d
        self._scan(x - 1, y - 1, x + w + 1, y + h + 1, fn, col, alpha)

    def text(self, s, cx, cy, scale, col, alpha=1.0, anchor='c'):
        s = s.upper()
        adv = 6 * scale
        total = len(s) * adv - scale
        x = cx - total / 2 if anchor == 'c' else cx
        y = cy - 3.5 * scale
        for ch in s:
            rows = FONT.get(ch, FONT['?']).split()
            for ry, row in enumerate(rows):
                for rx, bit in enumerate(row):
                    if bit == '1':
                        x0, y0 = x + rx * scale, y + ry * scale
                        self.rrect(x0, y0, scale * .86, scale * .86, scale * .2, col, alpha)
            x += adv

    def mask_circle(self):
        r = self.n / 2
        for y in range(self.n):
            for x in range(self.n):
                d = math.hypot(x + .5 - r, y + .5 - r) - r
                a = 1 - max(0.0, min(1.0, 0.5 - d))
                if a > 0:
                    p = self.px[y * self.n + x]
                    for i in range(3):
                        p[i] *= (1 - a)

    def mask_rounded(self, rad):
        """Rounded-square panel (2.16 inch board)."""
        n = self.n
        for y in range(n):
            for x in range(n):
                qx, qy = abs(x + .5 - n / 2) - (n / 2 - rad), abs(y + .5 - n / 2) - (n / 2 - rad)
                d = math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - rad
                a = 1 - max(0.0, min(1.0, 0.5 - d))
                if a > 0:
                    p = self.px[y * n + x]
                    for i in range(3):
                        p[i] *= (1 - a)

    def save(self, path, bezel=24):
        n = self.n
        w = n + 2 * bezel
        rows = []
        for y in range(w):
            row = bytearray([0])
            for x in range(w):
                ix, iy = x - bezel, y - bezel
                if 0 <= ix < n and 0 <= iy < n:
                    p = self.px[iy * n + ix]
                    row += bytes(int(max(0, min(255, v))) for v in p)
                else:
                    row += bytes((18, 18, 20))
            rows.append(bytes(row))
        raw = b''.join(rows)

        def chunk(t, d):
            c = struct.pack('>I', len(d)) + t + d
            return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, w, 8, 2, 0, 0, 0)) \
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
        with open(path, 'wb') as f:
            f.write(png)


def rim(cv, col=EMBER, wifi=True):
    """Hairline status rim, <=30% luminance."""
    cv.ring(cv.c, cv.c, cv.c - 3, 1.2, col, alpha=0.3)
    if wifi:
        cv.disc(cv.c + (cv.c - 3) * math.sin(math.radians(35)), cv.c - (cv.c - 3) * math.cos(math.radians(35)), 3, OK)


def dots(cv, n, i, y):
    for k in range(n):
        cv.disc(cv.c + (k - (n - 1) / 2) * 14 * cv.n / 466, y, 3 * cv.n / 466, EMBER if k == i else DIM, 1 if k == i else .6)


def screen_home(S):
    cv, u, c = Canvas(S), S / 100, S / 2
    rim(cv)
    cv.text("10:42", c, c - 22 * u, 2.6 * u / 1.0 * 1.0, WHITE)
    cv.text("THU 31 OCT", c, c - 5 * u, 0.9 * u, DIM)
    for i, (v, l, col, frac) in enumerate([("18°", "INSIDE", INFO, .55), ("2.1", "KW NOW", EMBER, .42), ("78%", "BATT", OK, .78)]):
        x = c + (i - 1) * 27 * u
        y = c + 20 * u
        cv.ring(x, y, 11 * u, .9 * u, SURFACE, alpha=1)
        cv.glow_ring(x, y, 11 * u, .9 * u, col, 200, 200 + 320 * frac, steps=3)
        cv.text(v, x, y - u, .75 * u, WHITE)
        cv.text(l, x, y + 6 * u, .35 * u, DIM)
    dots(cv, 4, 0, c + 41 * u)
    return cv


def screen_energy(S):
    cv, u, c = Canvas(S), S / 100, S / 2
    rim(cv)
    # solar / grid / battery segments around a home-load hero
    segs = [(-130, -15, EMBER, "SOLAR 3.4"), (-5, 110, INFO, "GRID 0.2"), (120, 235, OK, "BATT 1.1")]
    for a0, a1, col, _ in segs:
        cv.ring(c, c, 38 * u, 5 * u, SURFACE, a0, a1, cap=True)
        cv.glow_ring(c, c, 38 * u, 3 * u, col, a0, a1 - (a1 - a0) * .15)
    cv.text("2.1", c, c - 6 * u, 1.8 * u, WHITE)
    cv.text("KW HOME", c, c + 6 * u, .75 * u, DIM)
    for (lbl, (a0, a1, col, _)) in zip(("SUN 3.4", "GRID 0.2", "BAT 1.1"), segs):
        m = math.radians((a0 + a1) / 2)
        cv.text(lbl, c + 29 * u * math.sin(m), c - 29 * u * math.cos(m), .42 * u, col, anchor='c')
    cv.text("SELF USE 94%", c, c + 14 * u, .6 * u, WHITE, .8)
    dots(cv, 4, 2, c + 44 * u)
    return cv


def screen_domoticz(S):
    cv, u, c = Canvas(S), S / 100, S / 2
    rim(cv, STALE)
    cv.text("LIVING ROOM", c, c - 38 * u, .7 * u, DIM)
    rows = [("TEMP", "21.4°", OK, "OK"), ("HUMIDITY", "48%", OK, "OK"), ("GARAGE", "?", STALE, "UNKNOWN"), ("HALL LAMP", "OFF", DIM, "")]
    for i, (n, v, col, st) in enumerate(rows):
        y = c - 22 * u + i * 16 * u
        d = abs(y - c)
        w = 2 * math.sqrt(max(1, (0.92 * c) ** 2 - (d + 7 * u) ** 2)) - 6 * u
        cv.rrect(c - w / 2, y - 6.5 * u, w, 13 * u, 6.5 * u, SURFACE)
        cv.text(n, c - w / 2 + 6 * u, y, .5 * u, WHITE, .85, anchor='l')
        cv.text(v, c + w / 2 - 6 * u - len(v) * 6 * .7 * u, y, .7 * u, col, 1, anchor='l')
        if st == "UNKNOWN":
            cv.rrect(c - w / 2, y - 6.5 * u, w, 13 * u, 6.5 * u, STALE, .6, stroke=.5 * u)
    cv.text("UPDATED 42S AGO", c, c + 44 * u, .45 * u, WARN)
    return cv


def screen_confirm(S):
    cv, u, c = Canvas(S), S / 100, S / 2
    rim(cv)
    cv.text("GARAGE DOOR", c, c - 28 * u, .75 * u, DIM)
    cv.text("OPEN?", c, c - 14 * u, 2.2 * u, WHITE)
    cv.ring(c, c + 14 * u, 20 * u, 2.4 * u, SURFACE)
    cv.glow_ring(c, c + 14 * u, 20 * u, 2.4 * u, ERR, 0, 245)
    bx = c + 20 * u * math.sin(math.radians(245))
    by = c + 14 * u - 20 * u * math.cos(math.radians(245))
    cv.disc(bx, by, 3.2 * u, WHITE)
    cv.text("HOLD", c, c + 14 * u, .9 * u, ERR)
    cv.text("SWIPE DOWN TO CANCEL", c, c + 42 * u, .4 * u, DIM)
    return cv


def screen_boot(S):
    cv, u, c = Canvas(S), S / 100, S / 2
    cv.glow_ring(c, c, 14 * u, 1.4 * u, EMBER, 0, 300)
    cv.disc(c + 14 * u * math.sin(math.radians(300)), c - 14 * u * math.cos(math.radians(300)), 2.2 * u, WHITE)
    cv.text("DZPANEL", c, c + 28 * u, .95 * u, WHITE)
    cv.text("V0.1 DEV", c, c + 36 * u, .45 * u, DIM)
    return cv


def screen_launcher(S):
    cv, u, c = Canvas(S), S / 100, S / 2
    rim(cv)
    apps = [("HOME", EMBER), ("CLOCK", INFO), ("DOMOTICZ", OK), ("ENERGY", WARN), ("SETTINGS", STALE)]
    for i, (n, col) in enumerate(apps):
        a = math.radians(-90 + i * 72)
        x, y = c + 29 * u * math.cos(a), c + 29 * u * math.sin(a)
        cv.disc(x, y, 9.5 * u, SURFACE)
        cv.ring(x, y, 9.5 * u, .8 * u, col, alpha=.9 if i == 2 else .45)
        cv.disc(x, y, 3 * u, col, .9)
    cv.glow_ring(c, c, 9.5 * u * 0 + 10 * u, 1.2 * u, EMBER, 0, 216)
    cv.text("DOMOTICZ", c, c, .7 * u, WHITE)
    return cv


def line(cv, x0, y0, x1, y1, w, col, alpha=1.0):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy or 1

    def fn(x, y):
        t = max(0, min(1, ((x - x0) * dx + (y - y0) * dy) / L2))
        return math.hypot(x - (x0 + t * dx), y - (y0 + t * dy)) - w / 2
    cv._scan(min(x0, x1) - w, min(y0, y1) - w, max(x0, x1) + w, max(y0, y1) + w, fn, col, alpha)


def pt(cx, cy, r, ang):
    return cx + r * math.sin(math.radians(ang)), cy - r * math.cos(math.radians(ang))


def hero(S, title, rimcol=EMBER, ty=-40):
    cv, u, c = Canvas(S), S / 100, S / 2
    rim(cv, rimcol)
    cv.text(title, c, c + ty * u, .62 * u, DIM)
    return cv, u, c


def screen_onoff(S):
    cv, u, c = hero(S, "HALL LAMP")
    cv.disc(c, c, 24 * u, SURFACE)
    cv.glow_ring(c, c, 27 * u, 1.6 * u, EMBER, 0, 360)
    # power glyph
    cv.ring(c, c, 8 * u, 1.6 * u, EMBER, 35, 325)
    line(cv, c, c - 12 * u, c, c - 2 * u, 1.6 * u, EMBER)
    cv.text("ON", c, c + 17 * u, 1.0 * u, WHITE)
    cv.text("TAP TO SWITCH OFF", c, c + 42 * u, .4 * u, DIM)
    return cv


def screen_dimmer(S):
    cv, u, c = hero(S, "LIVING ROOM", ty=43)
    cv.ring(c, c, 36 * u, 3.4 * u, SURFACE, 220, 500)
    cv.glow_ring(c, c, 36 * u, 3.4 * u, EMBER, 220, 220 + 280 * .6)
    cv.disc(*pt(c, c, 36 * u, 220 + 280 * .6), 3.2 * u, WHITE)
    cv.text("60%", c, c - 4 * u, 2.6 * u, WHITE)
    cv.text("DIMMER", c, c + 11 * u, .6 * u, DIM)
    return cv


def screen_blinds(S):
    cv, u, c = hero(S, "BEDROOM BLINDS")
    x0, w, top, h = c - 17 * u, 34 * u, c - 33 * u, 52 * u
    cv.rrect(x0, top, w, h, 1.5 * u, SURFACE)
    pos = .65  # fraction closed
    slats = int(h / (3.4 * u) * pos)
    for k in range(slats):
        y = top + 1.2 * u + k * 3.4 * u
        cv.rrect(x0 + 1.2 * u, y, w - 2.4 * u, 2.2 * u, .8 * u, EMBER, .75)
    cv.rrect(x0, top, w, h, 1.5 * u, EMBER, .5, stroke=.5 * u)
    cv.text("65% CLOSED", c, c + 25 * u, .6 * u, WHITE)
    for dx, sym in ((-22, "up"), (0, "stop"), (22, "down")):
        x, y = c + dx * u, c + 36 * u
        cv.disc(x, y, 7 * u, SURFACE)
        cv.ring(x, y, 7 * u, .6 * u, EMBER, alpha=.6)
        if sym == "stop":
            cv.rrect(x - 2.2 * u, y - 2.2 * u, 4.4 * u, 4.4 * u, .6 * u, WHITE)
        else:
            s = -1 if sym == "up" else 1
            line(cv, x - 3 * u, y - s * 1.2 * u, x, y + s * 1.8 * u, 1.1 * u, WHITE)
            line(cv, x + 3 * u, y - s * 1.2 * u, x, y + s * 1.8 * u, 1.1 * u, WHITE)
    return cv


def screen_thermostat(S):
    cv, u, c = hero(S, "THERMOSTAT", INFO, ty=43)
    cv.ring(c, c, 36 * u, 3 * u, SURFACE, 225, 495)
    cv.glow_ring(c, c, 36 * u, 3 * u, EMBER, 225, 225 + 270 * (21.5 - 10) / 20)  # setpoint arc
    cv.disc(*pt(c, c, 36 * u, 225 + 270 * (21.5 - 10) / 20), 3 * u, WHITE)
    cv.disc(*pt(c, c, 36 * u, 225 + 270 * (19.8 - 10) / 20), 1.4 * u, INFO)  # current-temp tick
    cv.text("21.5°", c, c - 5 * u, 2.3 * u, WHITE)
    cv.text("NOW 19.8°  HEATING", c, c + 9 * u, .5 * u, EMBER)
    for dx, s in ((-14, "-"), (14, "+")):
        x, y = c + dx * u, c + 24 * u
        cv.disc(x, y, 6.5 * u, SURFACE)
        cv.ring(x, y, 6.5 * u, .6 * u, WHITE, alpha=.5)
        cv.text(s, x, y, 1.1 * u, WHITE)
    return cv


def screen_fan(S):
    cv, u, c = hero(S, "BEDROOM FAN", INFO, ty=43)
    names = ["OFF", "LOW", "MED", "HIGH"]
    sel = 2
    for i, n in enumerate(names):
        a0 = -130 + i * 65
        cv.ring(c, c, 36 * u, 4 * u, SURFACE, a0, a0 + 60)
        if i <= sel and i > 0:
            cv.glow_ring(c, c, 36 * u, 4 * u, INFO, a0, a0 + 60)
        x, y = pt(c, c, 26 * u, a0 + 30)
        cv.text(n, x, y, .45 * u, WHITE if i == sel else DIM)
    # simple rotor glyph
    for k in range(3):
        a = 120 * k + 20
        x, y = pt(c, c, 8 * u, a)
        cv.ring(x, y, 4.5 * u, 1.2 * u, INFO, a0=a + 200, a1=a + 330, alpha=.9)
    cv.disc(c, c, 1.6 * u, WHITE)
    cv.text("MEDIUM", c, c + 22 * u, .9 * u, WHITE)
    return cv


def screen_tempview(S):
    cv, u, c = hero(S, "OUTSIDE", INFO)
    cv.text("12.4°", c, c - 24 * u, 2.8 * u, WHITE)
    cv.text("HUM 81%   DEW 9.4°", c, c - 8 * u, .5 * u, DIM)
    vals = [10.1, 9.4, 9.0, 9.6, 11.0, 12.8, 14.2, 14.9, 14.0, 12.9, 12.4]
    lo, hi = min(vals), max(vals)
    pts = [(c - 30 * u + i * 6 * u, c + 22 * u - (v - lo) / (hi - lo) * 20 * u) for i, v in enumerate(vals)]
    for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
        line(cv, xa, ya, xb, yb, .9 * u, INFO)
    cv.disc(*pts[-1], 1.8 * u, WHITE)
    cv.text("24H  9.0-14.9", c, c + 30 * u, .45 * u, DIM)
    cv.text("UPDATED 2M AGO", c, c + 41 * u, .4 * u, DIM)
    return cv


def screen_push(S):
    cv, u, c = hero(S, "DOORBELL SOUND")
    for i, r in enumerate((30, 25)):
        cv.ring(c, c, r * u, .5 * u, EMBER, alpha=.18 - i * .06)
    cv.disc(c, c, 20 * u, EMBER, .95)
    cv.text("PUSH", c, c, 1.4 * u, (0, 0, 0))
    cv.text("SENT 10:41", c, c + 36 * u, .45 * u, OK)
    return cv


def screen_scenes(S):
    cv, u, c = hero(S, "SCENES")
    items = [("MOVIE", EMBER, "SCENE"), ("NIGHT", INFO, "SCENE"), ("AWAY", WARN, "GROUP"), ("ALL OFF", STALE, "GROUP")]
    for i, (n, col, kind) in enumerate(items):
        x, y = c + (i % 2 * 2 - 1) * 17 * u, c - 12 * u + (i // 2) * 29 * u
        cv.rrect(x - 15 * u, y - 12 * u, 30 * u, 24 * u, 8 * u, SURFACE)
        cv.disc(x, y - 4 * u, 3 * u, col)
        cv.text(n, x, y + 4 * u, .55 * u, WHITE)
        cv.text(kind, x, y + 9 * u, .33 * u, DIM)
    return cv


def screen_lock(S):
    cv, u, c = hero(S, "FRONT DOOR", OK)
    cv.ring(c, c, 22 * u, 2 * u, SURFACE)
    cv.glow_ring(c, c, 22 * u, 2 * u, OK, 0, 360)
    cv.rrect(c - 8 * u, c - 3 * u, 16 * u, 12 * u, 2 * u, OK)
    cv.ring(c, c - 4 * u, 5 * u, 1.6 * u, OK, 270, 450)
    cv.text("LOCKED", c, c + 30 * u, .9 * u, WHITE)
    cv.text("HOLD TO UNLOCK", c, c + 41 * u, .4 * u, DIM)
    return cv


def sun(cv, x, y, r, col=WARN):
    cv.disc(x, y, r, col)
    for k in range(8):
        a = k * 45
        line(cv, *pt(x, y, r * 1.45, a), *pt(x, y, r * 1.9, a), r * .22, col)


def cloud(cv, x, y, r, col=(170, 180, 195)):
    for dx, dy, rr in ((-1, .3, .7), (-.1, -.2, .95), (.9, .25, .65)):
        cv.disc(x + dx * r, y + dy * r, rr * r, col)
    cv.rrect(x - 1.6 * r, y + .3 * r, 3.2 * r, .65 * r, .3 * r, col)


def screen_weather(S):
    cv, u, c = hero(S, "AMSTERDAM", INFO)
    sun(cv, c - 18 * u, c - 20 * u, 5 * u)
    cloud(cv, c - 13 * u, c - 17 * u, 5 * u)
    cv.text("12°", c + 14 * u, c - 20 * u, 2.4 * u, WHITE)
    cv.text("PARTLY CLOUDY", c, c - 6 * u, .5 * u, DIM)
    cells = [("WIND", "14 KM/H", "SW"), ("RAIN", "0.0 MM", "TODAY"), ("PRESS", "1014", "HPA")]
    for i, (k, v, s) in enumerate(cells):
        x = c + (i - 1) * 27 * u
        cv.text(k, x, c + 8 * u, .4 * u, DIM)
        cv.text(v.split()[0], x, c + 16 * u, .8 * u, WHITE)
        cv.text(s if i else "SW " + v.split()[1], x, c + 23 * u, .33 * u, DIM)
    cv.text("UV 2  FEELS 10°", c, c + 35 * u, .45 * u, WARN)
    return cv


SCREENS = [("boot", screen_boot), ("home", screen_home), ("energy", screen_energy),
           ("domoticz_stale", screen_domoticz), ("confirm_hold", screen_confirm), ("launcher", screen_launcher),
           ("onoff", screen_onoff), ("dimmer", screen_dimmer), ("blinds", screen_blinds),
           ("thermostat", screen_thermostat), ("fan_selector", screen_fan), ("temp_view", screen_tempview),
           ("push_button", screen_push), ("scenes", screen_scenes), ("lock", screen_lock),
           ("weather", screen_weather)]


def main():
    out = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 466
    square = size == 480  # 2.16 inch board is a rounded square, see docs/hardware/verified.md
    os.makedirs(out, exist_ok=True)
    for name, fn in SCREENS:
        cv = fn(size)
        cv.mask_rounded(size * .09) if square else cv.mask_circle()
        cv.save(os.path.join(out, f"{name}_{size}.png"))
        print("wrote", name)


if __name__ == "__main__":
    main()
