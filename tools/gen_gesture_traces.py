#!/usr/bin/env python3
"""Generate synthetic touch traces (CSV) for the ru_gesture tests.

usage: gen_gesture_traces.py OUTDIR
Each file: '# diameter: N', '# expect: EVENT EVENT ...' then 't_ms,x,y,down,contacts,size'.
Expectations are written from the intent of each scenario, not from recogniser output.
Event names: TAP LONG_PRESS SWIPE:<dir> EDGE_SWIPE:<action> DRAG_START DRAG_MOVE DRAG_END CANCEL,
rim TAP/DRAG_* get '@RIM'; consecutive DRAG_MOVE collapse to one. Seeded, deterministic.
"""
import math, os, random, sys

def make(d, seed):
    rnd = random.Random(seed)
    c = d / 2.0
    R = c
    def jit(p, a):
        return (p[0] + rnd.uniform(-a, a), p[1] + rnd.uniform(-a, a)) if a else p
    def line(p0, p1, t0, dur, step=8, a=0.0, contacts=1, size=10):
        n = max(2, int(dur / step))
        out = []
        for i in range(n + 1):
            f = i / n
            x, y = jit((p0[0] + (p1[0] - p0[0]) * f, p0[1] + (p1[1] - p0[1]) * f), a)
            out.append((t0 + int(dur * f), x, y, 1, contacts, size))
        return out
    def up(t, x=None, y=None):
        return [(t, c if x is None else x, c if y is None else y, 0, 0, 0)]
    def hold(p, t0, dur, step=16, a=1.0):
        return [(t0 + i * step, *jit(p, a), 1, 1, 10) for i in range(dur // step + 1)]
    T = {}
    def add(name, expect, rows):
        T[name] = (expect, rows)
    k = lambda xs: xs
    # ---- content zone
    add("tap_center", "TAP", hold((c, c), 100, 60, a=0) + up(170))
    add("tap_jitter", "TAP", hold((c + 30, c - 20), 100, 120, a=2.5) + up(240))
    add("long_press", "LONG_PRESS", hold((c, c), 100, 700, a=1.5) + up(820))
    r = line((c + 70, c), (c - 70, c), 100, 120, a=1.0)
    add("swipe_left", "DRAG_START DRAG_MOVE DRAG_END SWIPE:LEFT", r + up(r[-1][0] + 8))
    r = line((c - 20, c + 80), (c - 20, c - 60), 100, 160, a=1.0)
    add("swipe_up", "DRAG_START DRAG_MOVE DRAG_END SWIPE:UP", r + up(r[-1][0] + 8))
    r = line((c - 70, c), (c + 70, c), 100, 3000, step=30, a=0.5)
    add("slow_drag_no_swipe", "DRAG_START DRAG_MOVE DRAG_END", r + up(r[-1][0] + 30))
    r = line((c - 70, c), (c + 70, c), 100, 150, a=0.5)
    add("drag_pause_then_lift", "DRAG_START DRAG_MOVE DRAG_END", r + hold(r[-1][1:3], r[-1][0] + 20, 300, a=0.5) + up(r[-1][0] + 340))
    r = line((c - 50, c - 50), (c + 50, c + 50), 100, 150, a=0.5)
    add("diagonal_no_swipe", "DRAG_START DRAG_MOVE DRAG_END", r + up(r[-1][0] + 8))
    r = line((c + 70, c), (c - 70, c), 100, 160, step=8, a=0.5)
    r = [p for i, p in enumerate(r) if i % 2 == 0 or i == len(r) - 1]
    add("swipe_lost_frames", "DRAG_START DRAG_MOVE DRAG_END SWIPE:LEFT", r + up(r[-1][0] + 16))
    add("up_garbage_coords", "TAP", hold((c, c + 40), 100, 60, a=0) + up(170, 0, 0))
    r = line((c, c), (c + 60, c), 100, 100, a=0)
    r += line((c + 400, c + 400), (c + 400, c + 400), 120, 8)
    add("outside_circle_start", "", line((6, 6), (80, 80), 100, 100) + up(220))
    # ---- palm / multi-touch / glitches
    add("palm_from_start", "", [(100 + i * 8, c, c, 1, 2, 10) for i in range(10)] + up(200))
    add("palm_big_contact", "", [(100 + i * 8, c, c, 1, 1, 200) for i in range(10)] + up(200))
    r = line((c + 60, c), (c - 60, c), 100, 160, a=0.5)
    r2 = [(t, x, y, dn, (2 if i >= len(r) // 2 else 1), s) for i, (t, x, y, dn, _, s) in enumerate(r)]
    add("second_finger_mid_drag", "DRAG_START DRAG_MOVE CANCEL", r2 + up(r[-1][0] + 8))
    r = line((c - 60, c), (c - 20, c), 100, 80, a=0.3) + [(190, c + 150, c + 100, 1, 1, 10), (198, c + 152, c + 101, 1, 1, 10)]
    add("teleport_mid_drag", "DRAG_START DRAG_MOVE CANCEL", r + up(210))
    # ---- rim
    L = 0.97 * R
    add("edge_back_left", "EDGE_SWIPE:BACK", line((c - L, c), (c - L + 90, c), 100, 120, a=1.0) + up(230))
    add("edge_quick_top", "EDGE_SWIPE:QUICK", line((c, c - L), (c, c - L + 90), 100, 120, a=1.0) + up(230))
    add("edge_home_bottom", "EDGE_SWIPE:HOME", line((c, c + L), (c, c + L - 90), 100, 120, a=1.0) + up(230))
    add("edge_context_right", "EDGE_SWIPE:CONTEXT", line((c + L, c), (c + L - 90, c), 100, 120, a=1.0) + up(230))
    p0 = (c + L * math.sin(math.radians(-45)), c - L * math.cos(math.radians(-45)))
    # diagonal in from the top-left corner region, bearing 315 = top sector boundary: inward diagonal
    add("edge_back_slow", "EDGE_SWIPE:BACK", line((c - L, c), (c - L + 60, c), 100, 1500, step=20, a=0.7) + up(1620))
    add("edge_too_short", "", line((c - L, c), (c - L + 20, c), 100, 100, a=0.3) + up(210))
    add("edge_outward_ignored", "", line((c - 0.92 * R, c), (c - 0.92 * R - 15, c), 100, 100) + up(210))
    pts = []
    for i in range(41):
        ang = math.radians(300 + i)
        pts.append((100 + i * 10, c + 0.95 * R * math.sin(ang), c - 0.95 * R * math.cos(ang), 1, 1, 10))
    add("rim_crown_drag", "DRAG_START@RIM DRAG_MOVE@RIM DRAG_END@RIM", pts + up(520))
    add("rim_long_press_home", "LONG_PRESS:HOME", hold((c, c + L), 100, 1000, a=1.0) + up(1120))
    add("rim_tap", "TAP@RIM", hold((c + L, c), 100, 60, a=0) + up(170))
    # ---- two taps in a row (state must reset)
    add("two_taps", "TAP TAP", hold((c, c), 100, 50, a=0) + up(160) + hold((c + 40, c), 400, 50, a=0) + up(510))
    return T

def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for d in (466, 480):
        for name, (expect, rows) in make(d, 1000 + d).items():
            with open(os.path.join(out, f"d{d}_{name}.csv"), "w") as f:
                f.write(f"# diameter: {d}\n# expect: {expect}\n# t_ms,x,y,down,contacts,size\n")
                for t, x, y, dn, ct, sz in rows:
                    f.write(f"{t},{x:.1f},{y:.1f},{dn},{ct},{sz}\n")

if __name__ == "__main__":
    main()
