# dzPanel UX: the "Halo" design language

Status: research / proposal. Targets: 466x466 (1.75") and 480x480 (2.16") round AMOLED, capacitive touch, LVGL-class renderer over QSPI.
All pixel values are given for 466 and 480 where it matters; the rule is always "design in units, not pixels".

---------------------------------------------------------------------------

## 1. Philosophy: Halo

A round AMOLED in a dark room is a **lit ring of glass floating in black**. Halo leans into that: the bezel disappears because true-black pixels are off, so UI elements appear to float. Content is light; everything else is void.

Principles:
1. **Void first.** Background is `#000000`. Depth comes from glow and thin luminous strokes, never from grey slabs.
2. **Rings, not boxes.** The display is a circle; the primary graphical idiom is the arc. Rectangles are for lists inside the chord-safe middle band only.
3. **One hero per screen.** One number, one ring, one verb. Glanceable in 2 seconds (section 14).
4. **Light is information.** Brightness encodes priority: hero 100%, supporting ~65%, chrome ~35%. Colour is a second channel, never the only one.
5. **Calm motion.** Few, short, purposeful animations that respect QSPI bandwidth; stillness is also battery.
6. **Honest data.** Stale, unknown, and zero are visually different (section 11).
7. **Safe by default.** Dangerous actions need a deliberate hold, with a visible ring filling.

Signature elements (what makes it recognisable):
- The **Halo rim**: a 1u hairline ring at radius 0.985R that carries status (Wi-Fi, battery, notifications) as arc segments.
- The **Bead**: a 2.4u glowing dot that rides the ring (time position, hold progress, loading).
- **Soft-glow arcs**: gauge arcs have a dim 2-3 step "bloom" painted as wider, low-alpha arcs under them (static, pre-rendered, no blur at runtime).

---------------------------------------------------------------------------

## 2. AMOLED rules

### 2.1 Pixels and power
- Background `#000000` everywhere, including dialogs and transitions. Never "near black" (`#0A0A0A` still lights pixels and shows banding/mura at low brightness). Raised surfaces start at `#0E1013` and cover **small areas only**.
- Power is roughly proportional to lit pixels x brightness, with blue the most expensive and the dimmest per-watt. Budget: **APL (average picture level) <= 12% on active screens, <= 3% in always-on (AOD)**.
- Prefer outlines and thin arcs to filled areas. Fill only small hero elements.
- White text at 100% is for the hero only; body text `#F2F4F7` is capped at ~90% via token, secondary at 65%.
- Avoid large saturated blue or white fields (full-screen white = worst case; no light theme ships by default. If ever offered, label it "high power").
- Dimming: brightness curve in perceptual steps (gamma ~2.2 mapped), UI steps 5/15/30/50/75/100 %. Min usable ~5%; auto-dim after 15 s idle to 30%, then AOD or off per setting. Ambient-light sensor if board has one, else time-of-day schedule (night = max 15%).

### 2.2 Burn-in mitigation
- **Pixel shift ("orbit")**: the whole UI layer is offset by a slow Lissajous within +/-2 px (466: +/-2, 480: +/-2), changing 1 px every 60 s. Implemented as a root container translate, not a redraw of children. In AOD, +/-3 px every 30 s.
- **No static high-luminance element persists > 5 min** at full brightness: status rim and clock dim to 60% after 2 min of no touch.
- Static chrome (icons in rim, labels) uses thin strokes and lower luminance (35%) so wear is minimal.
- Rotate accent identity of AOD clock elements by hour (hue shift within palette, same luminance) so the same sub-pixels do not carry the same colour all day.
- Never place a persistent 1-px high-contrast line (cross-hairs, dividers) at a fixed location; hairlines are <= 30% luminance.
- Avoid re-using the same hero position for status-only content: in AOD the clock drifts vertically +/-6 px per hour.

### 2.3 Always-on display (AOD) mode
Goal: sub-1-fps, <3% APL, readable at 5-10% brightness.
```
        ( AOD clock )                 rules
      .-------------.                 - black, no fills
    /       .         \               - time in Thin/Light weight, 100% -> 55% lum
   |      10:42        |              - ring hairline at 25% lum, seconds hidden
   |      Thu 31 Oct   |              - 1 data glyph max (battery or alert dot)
    \     (  * 3 )    /               - redraw once per minute; CPU may sleep
      '-------------'                 - touch wakes (tap anywhere); swipe ignored
```
- Redraw once/minute (partial update of the digits box only, if the panel/driver supports partial windows; else full frame once per minute).
- AOD uses outline digits option ("Contour clock") for lowest APL.
- Unread notification = single 8u dot on the rim at the 12 o'clock position with a count, nothing else.
- Exit: tap or wrist-raise (IMU, if present) -> 150 ms fade up to previous brightness (see motion).

### 2.4 Panel behaviour to design around
- Very low grey levels can show colour tint/steps: avoid dark-grey gradients below `#0E1013`; use dithered or stepped glows (3-4 discrete alpha levels) instead of smooth gradients.
- Avoid pure red `#FF0000` on large area (uneven ageing); use `#FF5A5F`-class coral for errors.

---------------------------------------------------------------------------

## 3. Colour system

All colours are tokens; apps never use raw hex. Contrast ratios are WCAG-style against `#000000` (approximate, verify with `tools/contrast.py` in CI).

### 3.1 Neutrals (dark primary theme "Void")
| Token | Hex | Use | Contrast on #000 |
|---|---|---|---|
| `bg` | `#000000` | all backgrounds | - |
| `surface-1` | `#0E1013` | cards, list rows | 1.1 |
| `surface-2` | `#171A1F` | pressed, selected card | 1.3 |
| `surface-3` | `#22262D` | tracks of rings/gauges, toggles off | 1.5 |
| `hairline` | `#2A2F37` | dividers, rim track | 1.7 (decorative only) |
| `text-1` | `#F2F4F7` | hero / primary | ~18.5 |
| `text-2` | `#A7AFBB` | labels, units | ~9.3 |
| `text-3` | `#6F7885` | hints, timestamps (>= 24px only) | ~4.6 |
| `text-disabled` | `#4A515C` | disabled (paired with icon/strike) | ~2.5 (exempt, never sole info) |

Rule: text under 28px uses `text-1` or `text-2` only. `text-3` is for >= 24px and non-critical.

### 3.2 Accent palettes (user picks one; whole OS re-tints)
Each accent has `base` (strokes, hero ring), `dim` (track at 35%: pre-mixed), `on` (text on filled accent = `#000000`).
| Palette | base | dim (35% over black) | contrast base/#000 |
|---|---|---|---|
| **Ember** (default, warm, low-blue = low power) | `#FF8A3D` | `#592F15` | ~9.0 |
| **Aurora** | `#3DE0C5` | `#15504A` | ~12.5 |
| **Iris** | `#8C7CFF` | `#312B59` | ~6.8 |
| **Rose** | `#FF5C8A` | `#592133` | ~7.0 |
| **Lime** | `#B6F25A` | `#3F5520` | ~15 |
| **Mono** | `#E6E9EE` | `#505254` | ~16 |

### 3.3 Semantic (fixed, not re-tinted)
| Token | Hex | Pair with (not-colour-alone) |
|---|---|---|
| `ok` | `#3DDC84` | check glyph, "OK" word |
| `warn` | `#FFC247` | triangle glyph, dashed ring |
| `error` | `#FF5A5F` | cross/! glyph, double stroke |
| `info` | `#4DB3FF` | "i" glyph |
| `stale` | `#8A93A0` | dashed stroke + clock-glyph + "12 min ago" |
| `offline` | `#8A93A0` | slashed Wi-Fi glyph, diagonal strike on values |
| `danger-hold` | `#FF5A5F` | ring that fills while holding + label "HOLD" |

### 3.4 Energy data colours (fixed semantic set)
Solar `#FFC247` (sun glyph), Grid import `#4DB3FF` (down-arrow), Grid export `#3DDC84` (up-arrow), Battery `#B58CFF` (battery glyph), Home load `#F2F4F7` (house glyph), Price: below-avg `ok`, above-avg `warn` (with arrows up/down).
Segment encoding in rings always adds separators (2u gaps) and glyph labels, so a colour-blind viewer can decode.

### 3.5 Rules
- Never convey state by hue alone: add glyph, shape (solid vs dashed), position, or text.
- Check palette under deuteranopia/protanopia simulation: ok/error pair is distinguished by glyph, and by luminance (`ok` L~0.55, `error` L~0.30).
- Max 2 accent-coloured elements competing on one screen.
- Glow = same hue at alpha 18% / 9% / 4% in three stepped widths (+2u, +4u, +6u).

---------------------------------------------------------------------------

## 4. Typography

### 4.1 Font recommendations (all open licences)
| Role | Font | Licence | Why |
|---|---|---|---|
| UI text | **Inter** (or **Inter Tight** for condensed numbers) | OFL 1.1 | Tall x-height, great at small px, tabular figures (`tnum`) |
| Hero numerals / clock | **Space Grotesk** or **Manrope** (ExtraLight/Light/Medium) | OFL 1.1 | Characterful digits, round geometry suits ring UI |
| Mono / diagnostics | **JetBrains Mono** or **IBM Plex Mono** | OFL 1.1 | Logs, IPs, serial numbers |
| Icons-in-text | Icon font built from Lucide/Material Symbols (section 13) | ISC / Apache 2.0 | Single glyph run per icon |
Alternates: **DM Sans**, **Outfit** (round, OFL). CJK/Arabic are out of scope for v1; plan for font fallback chain in LVGL (`lv_font_t.fallback`).

### 4.2 Producing LVGL fonts
- Use `lv_font_conv` (offline, CI) to produce **bitmap fonts with 4 bpp** antialiasing (1 bpp for tiny/AOD if flash tight; 4 bpp looks right on AMOLED). `--bpp 4 --no-compress` for hero sizes (fast), compressed for body.
- **Subset aggressively**: numerals and symbols for hero fonts (`0-9 . , : - + % / ° k W h`), Latin-1 + a few symbols for UI sizes. Hero 130 px with 14 glyphs is ~20-30 KB; full Latin at 30 px 4 bpp ~ 25-40 KB.
- Ship weights: Regular (400), Medium (500), Light (300, hero only). Avoid Bold for body: on AMOLED, light-on-black text blooms and looks heavier than dark-on-light, so use **Regular where you would use Medium on paper** and Light for large.
- Alternative: LVGL TinyTTF/FreeType for runtime scaling: more flexible for dual resolution but slower and uses PSRAM; recommended only if build size matters more than speed. Default: **pre-baked bitmap fonts per profile** generated from one unit table (CI step runs `lv_font_conv` twice with size = round(units x D/100)).
- Letter-spacing: +1% for sizes under 28 px; tabular figures for all changing numbers to avoid jitter.

### 4.3 Type scale (unit u = D/100: 4.66 px at 466; 4.80 px at 480)
| Token | Units | 466 px | 480 px | Weight | Use |
|---|---|---|---|---|---|
| `display-xl` | 30u | 140 | 144 | Light | Clock hours/minutes |
| `display` | 20u | 93 | 96 | Light | Hero values (kW, temp) |
| `value` | 13u | 61 | 62 | Medium | Card values |
| `title` | 8u | 37 | 38 | Medium | Screen titles |
| `body` | 6.4u | 30 | 31 | Regular | List rows, buttons |
| `label` | 5.2u | 24 | 25 | Regular | Units, captions |
| `micro` | 4.3u | 20 | 21 | Medium | Timestamps, rim labels (min. size, never essential) |
Minimum legible: 20 px (about 3.5 mm physical on the 1.75" panel at ~ 380 ppi... verify on hardware). Essential info >= 24 px.
- Scaling: `px = round(units * D / 100)`, then snap to the nearest baked font. Two baked sets (466, 480) are almost the same, so a build can ship **one set** (e.g. 466 sizes) and use it on both (+3% on 480 is invisible) and scale layout only. Recommended for flash size.
- Max line length at body: 14 chars in side bands, 22 chars in the central band.

---------------------------------------------------------------------------

## 5. Circular geometry

Let `D` = diameter (466/480), `R = D/2`, `u = D/100`, centre `C=(R,R)`. Safe radius `Rs = 0.92 R` (margin 0.08R, about 19 px) configurable per board profile (bezel tolerances, glass curvature).

### 5.1 Chord width
Usable width of a horizontal band at vertical offset `d` from the centre: `w(d) = 2*sqrt(Rs^2 - d^2)`. A text row occupying y in [d1,d2] must use `d = max(|d1|,|d2|)` (the edge furthest from centre).

Worked (466, Rs = 214):
| d from centre | d/R | chord width |
|---|---|---|
| 0 | 0 | 428 |
| 58 | 0.25 | 412 |
| 116 | 0.50 | 360 |
| 163 | 0.70 | 277 |
| 198 | 0.85 | 162 |
| 210 | 0.90 | 83 |
At 480 (Rs=221): 442 / 426 / 371 / 286 / 167 / 86. Everything scales, so compute in code.

Inscribed rectangle: square side = `Rs*sqrt2` = 303 (466) / 312 (480), i.e. **65% of D**. This is the "fully safe" rect, great for dialogs. Wide rect that fits: width `W`, height `H = 2*sqrt(Rs^2 - (W/2)^2)`; e.g. W=0.8D -> H~0.43D -> pill rows.
```
          ____
       .-'    '-.   <- top band: chord 0.5D at d=0.36D... centre text only
     /  ______    \
    | |  safe  |   |  fully safe square = 0.65D
    | |________|   |
     \            /
       '-.____.-'
```

### 5.2 Helper API (ui/geometry, host-testable)
```
chord_width(y, Rs)            -> int
safe_rect_at(y, h, Rs)        -> {x0, x1}   // worst-edge rule
polar(r, theta_deg) -> (x,y)  // theta 0 = 12 o'clock, clockwise
arc_len(r, deg)               -> px
slot_angles(n, start, sweep)  -> theta[]   // radial layout
fit_text_in_band(text, font, y) -> ok | shrink | ellipsize
```
Unit tests: sample at both D, assert no pixel outside Rs for generated bounding boxes.

### 5.3 Zones (vertical bands, fractions of D from top)
```
0.00-0.10  dead (rim only, glyphs on the rim)
0.10-0.22  status/title band      (chord 0.45-0.75 D)  1 line label
0.22-0.40  upper content          (chord 0.80-0.93 D)
0.40-0.60  hero band              (chord 0.92-0.92 D)  largest type
0.60-0.78  lower content          (chord 0.93-0.80 D)
0.78-0.90  actions / page dots    (chord 0.75-0.45 D)  centred controls
0.90-1.00  dead
```

### 5.4 Ring gauges and arc text
- Ring radii: outer rim `0.985R` (status), primary gauge `0.88R` stroke `5u` (23 px), secondary `0.74R` stroke `3u`, tertiary `0.62R` stroke `2u`. Gap between rings >= 2u. Round caps. Start gap at the bottom (gauge sweeps 270 deg from 135 to 405 deg in LVGL coords) so the bottom keeps room for the label and finger.
- Arcs via `lv_arc` (indicator + track) or pre-rendered 4-bpp masked images when many are animated (cheaper).
- **Arc text** (e.g. "SOLAR" following the rim): LVGL has no native curved text. Render per-glyph at `theta_i = theta0 + cum_advance/r`, each glyph as an `lv_label` rotated by `lv_obj_set_style_transform_angle` (cost: ~1 object per char; limit to <= 12 chars, static only), or **pre-render to an A8 canvas at build time** for fixed labels. Use only for decorative titles at the top (reads left-to-right, centre at 12 o'clock) and bottom (reversed so it stays upright, centred at 6 o'clock). Never for data.
- Radial layout: place n items at `polar(0.68R, theta_i)`; icons 14u (65 px) minimum touch 11u (51 px)... touch target rule below.

### 5.5 Tokens in relative units
| Token | Value | 466 | 480 |
|---|---|---|---|
| `space-1` | 1u | 5 | 5 |
| `space-2` | 2u | 9 | 10 |
| `space-3` | 3u | 14 | 14 |
| `space-4` | 4.5u | 21 | 22 |
| `space-5` | 7u | 33 | 34 |
| `radius-s` | 2.5u | 12 | 12 |
| `radius-m` | 5u | 23 | 24 |
| `radius-l` | 8u | 37 | 38 |
| `radius-pill` | 50% height | - | - |
| `stroke-hair` | 0.3u (min 1px) | 1-2 | 1-2 |
| `stroke-ring` | 1.5u / 3u / 5u | 7 / 14 / 23 | 7 / 14 / 24 |
| `touch-min` | 11u | 51 | 53 |
| `touch-comfort` | 14u | 65 | 67 |
| `elev-0` | bg `#000`, no stroke | | |
| `elev-1` | `surface-1` + hairline 1px inner | | |
| `elev-2` | `surface-2` + 3-step glow in accent at 9/4% | | |
| `elev-3` | modal: `surface-1` fill + accent hairline + 3-step glow + backdrop dims to 0% (already black: instead dim content under to 25% luminance) | | |
Elevation on AMOLED = **luminance step plus glow**, never drop shadows (shadows are invisible on black).

---------------------------------------------------------------------------

## 6. Gesture model

Touch only; no bezel or crown assumed (a boot/PWR button may exist; treat as optional: short press = wake/sleep, long = power menu).

| Gesture | Where | Action |
|---|---|---|
| Tap | anywhere | activate / wake |
| Swipe **left / right** | centre | page between sibling pages (apps in a ring, dots at bottom) |
| Swipe **down** from top edge (start within top 8% of D) | edge | Quick settings (shade slides from top) |
| Swipe **up** from bottom edge (bottom 8%) | edge | Launcher (from home) / Notification stack (from any app: see below) |
| Swipe **right** from left edge (start within left 6%) | edge | **Back** (one level) |
| Swipe **left** from right edge | edge | context panel / history (optional per app) |
| Long press (500 ms) | item | context actions / edit mode |
| Hold (1200 ms) | danger button | confirm dangerous action, ring fills |
| Double tap | clock face | toggle AOD preview / dim |
| Two-finger tap | anywhere | screenshot (dev builds) |
| Circular drag along the rim | rim band (outer 8%) | **virtual crown**: scroll lists, adjust value. Angular delta -> scroll |

Rules:
- Edge swipes start on the **chord**, not the square edge: the edge zone is an annulus `r > 0.90R`, with the sector selecting direction (top sector 315-45 deg = top edge, etc.). Handle in shell, apps never see edge touches that begin in the annulus.
- **Home** = long-press anywhere on the rim for 800 ms, or the "home pill" (a 9u x 1u bar at 6 o'clock on app screens, 35% luminance, tapping it = home, swipe up on it = launcher).
- **Back** always exists: edge-swipe-right and a visible back control on every detail, settings sub and confirm screen (built): an outlined pill with a chevron, "< BACK" ("< DEVICE NAME" at the name position on dial pages, "< CANCEL" on the confirm screen), kept inside the inscribed circle. Square panels also get a 56 px chevron button at the top left corner (hit area 64 px, outside the circle). Every path (pill, corner, shell BACK gesture, confirm hold) goes through `ru_dzapp_back`: confirm returns to its detail or sub screen, detail to its section, sub to Settings, a section to Home. Note the corner lies in the shell's rim zone: a 800 ms hold there still means Home. A swipe that starts on the corner also triggers the shell's BACK/QUICK sector action, and if the finger then lifts inside the chevron LVGL would also click the old screen's chevron; `cb_back` therefore ignores a click whose target is not on the currently shown screen, so one gesture is one step back.
- Rotate-less navigation: the rim-drag virtual crown replaces a rotary crown; every list also supports vertical drag with inertia; value adjusters have +/- buttons as well.
- Gestures are **disambiguated by start location**, then direction; a 10 px dead-zone prevents tap/swipe confusion; tap fires on release if movement < 8 px.
- Accidental-touch protection: after wake from sleep, first touch only wakes. Dangerous actions: hold only. Screen-off while pocket/wet? Provide "lock" (long-press power, unlock by swipe-up arc).
- Navigation is a **two-axis map**:
```
              [Quick settings]  (swipe down)
                     ^
  [Clock] <-> [HOME] <-> [Energy] <-> [Domoticz] <-> [Settings]   (swipe left/right)
                     v
                [Launcher]  (swipe up)    [Notifications] (swipe up from home second time or tap rim dot)
```

---------------------------------------------------------------------------

## 7. Shell patterns

### 7.1 Home: watch-face-like
Home is a **face** with 1 hero (time) and up to 3 "complications" placed on the ring. Faces are skinnable; each = a small config (layout id + data slots + accent).
Variants:
```
A. "Halo Classic"           B. "Orbit"                C. "Data"                  D. "Contour (AOD)"
     .---------.               .---------.              .---------.                .---------.
   /  (* 3)  Wi  \           /   .  o      \          /    21.4°     \            /           \
  |    10:42       |         |  .   10:42  . |        |  10:42   ☀2.1kW|          |  _  _  _   |
  |   Thu 31 Oct   |         |  .  Thu 31   . |       |  Thu 31  ▮ 78% |          | | || | | | |
  |  [18°] [2.1kW] |         |   '  o  '     |        |  18° | 2.1kW | |          |  outline    |
   \  ▮ 78%  ●●●  /           \  ring=sec    /          \  mini chart  /            \           /
     '---------'               '---------'              '---------'                '---------'
```
- Complications: rim arcs (battery on left arc, Wi-Fi top, notification count right), centre trio below the time. Tap a complication = open its app. Max 3.
- Seconds: smooth sweep bead on the rim only in active mode, capped at 10 fps; static bead stepping each second otherwise.

### 7.2 Launcher (choose one, ship carousel first)
```
Carousel (default)                 Honeycomb                      Radial
        ( Energy )                    o  o  o                       o   o
    ( Home )  ( Dom. )               o  o  o  o                   o       o
     \  big icon+label /              o  o  o                    o  (hub)  o
      "Domoticz"  ● ○ ○ ○                                          o   o
```
- **Carousel**: central 22u icon (103 px) with label `title`, neighbours at 60% size, scrolling horizontally with snap; dots below. Best legibility, cheapest to draw (3 icons live).
- **Honeycomb**: hex grid of 15u icons, drag to pan with magnify near centre (fisheye scale 1.0 -> 0.6). Good for > 8 apps; needs per-frame scaling, so use pre-scaled 3 sizes, snap-step not smooth.
- **Radial**: n <= 8 apps on `0.68R` circle, hub shows selected app name, hold hub = edit.
Recommendation: carousel for <= 7 apps, auto-switch to honeycomb beyond.

### 7.3 Quick settings (swipe down)
Shade covers the screen with a 2x2 / 3-tile layout inside the safe square: Brightness arc (drag on rim), Wi-Fi, Do-not-disturb, AOD, Night mode, Settings gear. Slider = rim arc, drag along ring.
```
        .-----------.
      /  Thu 10:42    \
     |  [Wi-Fi] [DND]  |
     |  [ AOD ] [Night]|
     |  brightness ◜━●◝|   <- rim arc
      \   [ ⚙ ]       /
        '-----------'
```

### 7.4 Notification stack
Cards stacked like a deck: top card full, two peeks behind (scaled 0.94/0.88, luminance 60/35%). Swipe card left/right = dismiss, tap = open/detail, swipe up = next. Priority via leading glyph (!, i, check) and rim colour tick; critical alerts pulse the rim 3 times and stay until acknowledged.
Banner: transient toast over the top band (section 10).

---------------------------------------------------------------------------

## 8. Motion principles

QSPI AMOLED links are the bottleneck: a full 466x466 RGB565 frame is ~434 KB; at ~40-80 MB/s effective that is ~6-11 ms, but real systems with DMA + PSRAM often manage 20-40 fps full-screen. Design for **30 fps ceiling and graceful 15 fps**.

Budget rules:
- Animate **small regions or whole-layer translations** (partial refresh), not full-screen alpha blends.
- Prefer **translate and opacity-step** over scale/rotate; avoid rotating large images; pre-rendered ring frames for loading.
- Use LVGL partial render mode with double buffers in internal RAM/PSRAM per ADR-002; set invalidation areas tight (ring arc updates invalidate only the bounding sector).
- Never animate more than one large element at once; no parallax.
- Frame skip tolerance: animations are time-based (not frame-count) so dropped frames still end on time.

Durations and easing:
| Token | ms | Use |
|---|---|---|
| `instant` | 0-50 | press feedback (pressed = surface-2 + 2% scale none) |
| `quick` | 120 | toggles, tick marks, toast fade |
| `base` | 220 | page transitions, shade open |
| `slow` | 360 | launcher snap, dial value change |
| `ambient` | 1000-2400 | breathing bead, loading orbit |
Easing: `standard` = cubic-bezier(0.2, 0, 0, 1) (decelerate-biased, "ease-out-expo-ish") for enter; `exit` = (0.4, 0, 1, 1) for leaving; `spring` = overshoot 6% only for toggles/hold-complete. In LVGL: `lv_anim_path_ease_out` / custom bezier path (`lv_anim_path_custom_bezier3`).
What to animate:
- Value change in rings: tween arc 360 ms (only if delta > 2%).
- Page change: slide 220 ms (translate only, outgoing dims to 40%).
- Dialog: scale 0.92 -> 1.0 + opacity steps 0/50/100 in 120 ms.
- Hold-to-confirm ring fill: linear, exactly the hold duration.
- Loading: orbiting bead (pre-rendered path, 12 fps).
- **Don't animate**: numbers counting (flicker, cost), backgrounds, text blur, clocks seconds in dim mode.
Reduced motion setting: all durations -> 0/instant crossfade; ambient loops off. Honoured automatically in low-power mode (<20% battery).

---------------------------------------------------------------------------

## 9. Component catalogue

(`u` units; sketches are schematic.)

### 9.1 Status ring (rim)
```
      . - * - .         top: Wi-Fi arc (3 notches by RSSI) + glyph
    ·           ·       left: battery arc (fill = %, notch every 25%)
   |             |      right: notification dots (up to 5) + count
    ·           ·       bottom: gap (home pill)
      ' - - - '
```
Track `hairline`, active `text-2`; warning swaps to `warn` AND dashes the arc. Width 1u..1.5u. Bead marks "now".

### 9.2 Value card
```
 ┌────────────────┐
 │ ☀ Solar        │   label (label size, text-2) + icon 6u
 │ 2.14 kW        │   value (value size) + unit (label)
 │ ▂▃▅▆▅▃ +12%    │   optional sparkline + delta glyph
 └────────────────┘
```
`surface-1`, radius-m, padding space-3. On round screens cards placed in chord-safe band; width <= chord at the card's worst edge. Pressed: `surface-2`.

### 9.3 Gauge (ring)
```
      ____
    /  ◜◝  \       270 deg arc, track surface-3, value accent
   |  2.1   |      centre: value + unit; below: label
    \ kW  ◟◞/      ticks every 10% at 25% lum (optional)
      ‾‾‾‾
```
Zones (green/amber/red) are shown as **tick marks and tiny glyph at threshold**, not just colour. Needle bead on arc end. Bidirectional gauge (net grid +/-) has zero at 12 o'clock, left = export, right = import, with arrow glyphs.

### 9.4 Mini chart
```
 kW ┤        ╭╮
    │   ╭─╮ ╭╯╰╮    1 px line = 2 px (stroke 0.5u) accent, area under at 12% alpha stepped
    │ ──╯ ╰─╯   ╰─   last point = bead with value label
    └──────────────  x: "now" at right, ticks 6h
```
Max 96 points (downsample), 1 series (2 for energy: solar fill + load line), no grid except 2 faint hairlines, y-axis min/max labels only. Chart must sit within the chord (use curved baseline option: plot clipped to the circle).

### 9.5 Toggle
```
 OFF: ( o   )  "Off"      ON: (   o ) ✓ "On"
```
Pill 20u x 10u (93 x 47 px), knob 8u, ON = accent fill + check glyph + "On" text; OFF = `surface-3` + dash glyph. Change 120 ms spring. Entire row is the hit area (>= touch-comfort high).

### 9.6 Dialog and hold-to-confirm
```
 Standard confirm              Dangerous (hold)
 .-------------------.        .-------------------.
 |  Turn off garage?  |       |   Open garage door |
 |                    |       |        (🔓)        |  <- ring around button fills
 |  [ Cancel ] [ OK ] |       |   HOLD to confirm  |  1.2 s; release = cancel
 '-------------------'        |      [ Cancel ]    |
                              '-------------------'
```
- Inside the fully-safe square (65% D). Buttons >= 14u high, >= 28u wide; Cancel left or below, default focus on Cancel for danger.
- Danger: ring is `error`, hold progress 1200 ms linear, haptic tick at 0/50/100% if available, glyph change (lock -> unlock), label "HOLD". Completion = spring pop + result toast. Ignore if state changed meanwhile (stale UI: show "State changed, review").
- Which actions are dangerous: locks, doors, heating cut, power circuits, firmware flash, factory reset, delete credentials. Plain toggles for lamps do not need confirmation.

### 9.7 Toast
```
        ┌──────────────────┐   top band chord, pill, surface-2 + accent hairline
        │ ✓ Light on        │  icon + <= 24 chars, 2.4 s (error 5 s, tap to dismiss)
        └──────────────────┘
```
Queue max 3; never covers hold buttons; no sound by default.

### 9.8 State patterns (every data screen implements all)
```
Loading               Stale                  Error                 Offline               Empty
  .-----.               21.4°                  ⚠                    ⌁ (Wi-Fi slashed)      ○
 ( ◌ o  )               ┈┈┈ (dashed ring)     "Can't reach          21.4° (dimmed 50%)    "No devices yet"
  skeleton arcs         ◷ 12 min ago           Domoticz"            "Offline · cached 08:14"  [Add in setup]
  (dim ring pulses)     value 70% lum          [Retry] (cause)       diagonal strike?       
```
- **Loading**: skeleton = track rings only + orbiting bead; show after 300 ms delay (avoid flicker); text "Loading...".
- **Stale** (age > 2x poll interval): value stays but dims to 70%, ring switches solid->dashed, `◷ 12 min` caption appears. >1 h: value replaced by "--".
- **Error**: glyph + one-sentence cause (+ error code in `micro`) + Retry. Never raw JSON.
- **Offline**: rim Wi-Fi slashed glyph, banner "Offline - showing cached 08:14".
- **Zero vs unknown**: zero = "0.0 kW" normal; unknown = "-- kW" with `?` glyph and dashed ring.

---------------------------------------------------------------------------

## 10. Key screens: wireframes (466 canvas; scale by u)

### 10.1 Boot splash
```
            (full black)
              .   .
           .         .
         .      ◯       .       Logo = thin ring (the Halo) draws clockwise 0->360 over 900 ms,
        .     dzPanel     .     bead leaves at 12 o'clock; wordmark fades in (Light weight, 8u)
         .   v0.1.0       .     bottom: tiny version (micro, text-3)
           .         .          Total <= 1.5 s of animation; if boot is slower the bead keeps orbiting
              ' . '             APL ~1%
```
Boot error: ring turns `error` dashed, code and hint "Hold to enter recovery".

### 10.2 Wi-Fi setup (first-run)
```
Step 1: choose              Step 2: join                    Step 3: result
   ┌ top: "Wi-Fi" ┐          "Home-5G"                       ✓ Connected  (ring completes, ok)
 ( 📶 Home-5G  ▂▄▆ )          [ ●●●●●●●  ]  password         IP 192.168.1.42  (mono, label)
 ( 📶 Guest    ▂▄  )          on-screen keyboard?            [ Continue ]
 ( 📶 Neighbour▂   )          OR "Use phone" QR + SoftAP     failure: ⚠ "Wrong password" [Retry]
  [ Rescan ]  [Phone]         ring = progress while joining
```
- Typing on a 466 px circle is painful: **primary path is phone provisioning** (SoftAP "dzPanel-XXXX" captive portal or BLE), shown as a QR code (white-on-black inverted? QR needs dark-on-light: render a light `#E6E9EE` plate of 40u square, small APL cost and temporary).
- Keyboard fallback: 3 arc rows (a-j, k-s, t-z + symbols) along the lower chord band; or a rim-drag letter picker (virtual crown) for short strings. Max 1 screen of 6 keys per row, keys >= 11u.
- Scan list rows: 12 u high, chord-fit, ellipsize SSIDs, signal bars glyph + lock glyph (shape cues).

### 10.3 Home / clock face variants
See 7.1. Detailed A "Halo Classic" (466):
```
y=  0  ─ rim: Wi-Fi arc (top), battery arc (left), notif dots (right)
y= 70      ·     Thu 31 Oct      (label, text-2, chord 0.7D)
y=130          10:42              (display-xl Light, hours text-1, minutes accent)
y=270     ──────────────────      hairline (30% lum, optional)
y=290    [ 18° ]  [ 2.1kW ]  [78%]   3 complications, 14u circles with 1 glyph + 1 value (label)
y=400            ● ○ ○             page dots
y=440            ▁▁▁               home pill
```
Face B "Orbit": seconds bead around rim, time smaller, the day progress as faint arc. Face C "Data": time on top 1/3, big temperature and solar in lower 2/3 with mini chart. Face D "Contour": outline digits, AOD default.

### 10.4 Domoticz overview
```
        ‹  Domoticz  ● live          title band; ● = ok glyph + "live"; stale -> ◷
      ┌──────────────────────┐
      │ 🌡 Living   21.4 °C   │      4-6 rows, 13u high, chord-fit; rows below the hero
      │ 💡 Kitchen  On   (●)  │      hero (top row) is the "pinned" device in display size
      │ 🔒 Front door  Locked │      tap row = toggle (lamps) / open detail (locks)
      │ 💧 Humidity  54 %     │      list scrolls vertically; rim-drag scroll
      └──────────────────────┘
          ● ○ ○   (rooms as pages)   bottom: room dots + last refresh "12 s ago" (micro)
```
- Alternative tile view: 2x2 cards in the safe square (each 30u x 30u), giant icons, for kiosk use.
- Detail screen (dimmer and colour lights): see section 12a (Light control). Earlier idea kept for the record: big arc slider along the rim, centre = % and name, tap centre = on/off.
- Lock action: hold-to-confirm (9.6). Read-only mode (MVP): rows show a lock glyph instead of switch.

### 10.4a Domoticz page model (as built)

The Domoticz app is a row of pages that follows the Domoticz web UI. One dot per visible page (current one larger) sits at the bottom.

```
FAVOURITES  LIGHTS  SCENES  TEMPERATURE  WEATHER  UTILITY  ENERGY  SETTINGS
```

- **Visibility.** Favourites and Settings always. Every other page is hidden while the model has nothing for it. Membership comes from the Domoticz tab of each device (`dz_device_t.tabs`, table in `docs/research/domoticz-api.md`): LIGHTS = light tab (not scenes or groups), SCENES = the scene/group entries, TEMPERATURE / WEATHER / UTILITY = those tabs (a Temp + Humidity + Baro device is on both TEMPERATURE and WEATHER; devices on no tab only show on Favourites when they are favourites). ENERGY = meters with a usage or day total, one at a time with its gauge (unchanged).
- **Order.** The server's order (`dz_model_order`); devices that arrive by delta and are new sort last until the next full refresh.
- **Favourites** = the favourites only, in dashboard order: scenes and groups, lights, temperature, weather, utility, the rest. No hero gauge. "NO FAVOURITES" when empty; old data is told by the rim dot only (no "STALE DATA" subtitle); the connection banner (OFFLINE, TOKEN REJECTED, ...) stays.
- **Layout.** LIGHTS, SCENES, TEMPERATURE, WEATHER and FAVOURITES are a 3 x 2 grid of state tiles (25u x 21u, 1.5u gaps: 78u wide, inside the 79u chord at +-23u), 6 per page. Their tiles show the name (one line, cut with "..."), a state dot or a value, and a small kind line; Temperature and Weather tiles are value tiles with one function per kind (temperature and humidity share a tile: "21.5 / 48%", kind "TEMP / HUM"; wind with direction; rain; UV; barometer), so a richer renderer can replace a kind. UTILITY stays value rows (13u high, name left, value right), 4 per page. LIGHTS is the Domoticz Switches tab: every device of the light filter (all switch types), read-only classes (contacts, motion, smoke, media players) show their state and open the read-only detail. A page indicator "2/4" sits under the list (it is a button: next page, round to the first). Nothing scrolls; there is no cap on the number of devices. At most 6 tiles (or 4 rows) of LVGL objects exist per screen: the index list (8 bytes per entry, one pass over the model) is the only per-device memory.
- **Gestures.** Finger left/right = next/previous page (section), no wrap. Finger up/down = next/previous list page, no wrap. One decision point, `ru_dzapp_swipe`, fed by the shell's recogniser for CONTENT-zone swipes; it refuses starts in the rim (r >= 0.90 R), while a slider is held and on detail, hold-to-confirm and settings sub screens. The rim keeps BACK / HOME. A drag that ends on the tile it started on is not a tap.
- **Return.** A tap opens the existing detail (or the hold screen for scenes and groups). BACK returns to the same page and list page; leaving the page forgets the list page.

### 10.5 Energy dashboard (solar / grid / battery ring)
```
          ☀ 2.1 kW                top (12 o'clock, chord-safe)
      .--═══╗ ╔═══--.             Ring 0.88R, 270 deg segments, 2u gaps:
    / ☀══════╗  ▮▮▮▮ \               top-left = SOLAR (amber, sun glyph)
   | Solar    Home      |           top-right = GRID  (blue ↓ import / green ↑ export arrow)
   |  ⌂  1.4 kW         |           bottom-left = BATTERY (violet, bar fill = SoC)
   |    (home load)     |           centre = HOME LOAD hero (display size)
    \ ▮ 78% ⇄ -0.5kW  /           bottom row: battery SoC and charge/discharge arrow
     '--  ↓ 0.2 kW  --'
          €0.18 ▼ cheap           price chip with arrow + word (not colour-only)
```
Layout:
- Segment length proportional to its share of flow (min 8% so tiny flows stay visible), flow direction by chevrons running along the arc (static glyphs, not animated, or 1 bead moving at 6 fps for the active flow).
- Page 2: 24 h chart (solar fill + load line, "now" bead, price steps as thin bar at the bottom). Page 3: day totals as three numbers (kWh produced / used / exported).
- Units explicit (`kW`, `kWh`, `%`, `EUR/kWh`); unknown source -> `--` with `?` and dashed segment, **never** drawn as zero.
- Colour-blind decode: each segment has its own glyph at its midpoint and the numeric value.

### 10.6 Notifications
```
        Notifications (3)             top: title + count
     ┌─────────────────────┐          top card full (surface-1, radius-l)
     │ ! Front door open    │          priority glyph (!, i, ✓), text <= 2 lines (body)
     │   10:41 · Domoticz   │          meta in label: time · source
     │ [ Dismiss ]  [Open]  │          buttons 14u
     └─╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌┘          2 peeks behind (scaled)
         swipe ← → dismiss   [Clear all] (hold)
Empty state: ○ "All caught up" + faint ring
```
Critical alert: full-screen takeover with `error` ring pulse x3, big glyph, text, [Acknowledge] (hold only if the action itself is dangerous).

### 10.7 Settings
```
        Settings
      ┌──────────────────────┐   single-column list inside safe band, 13u rows, chevrons
      │ 📶 Wi-Fi     Home-5G >│   sections: Network · Display · Apps · Domoticz · System
      │ ☼  Display   AOD On  >│   Display: brightness arc, auto-dim, AOD, accent palette swatches
      │ 🎨 Accent    ● Ember >│        (swatch = circle + name; selected = ring + check)
      │ 🔌 Domoticz  Linked  >│   Domoticz: URL via phone/web portal; never typed on device
      │ ⓘ  About / Update   >│   System: about, storage, reset (hold-to-confirm, danger)
      └──────────────────────┘
```
Value adjusters use the rim-arc slider with +/- buttons. Header scrolls away; rows keep >= 13u height.

### 10.8 Flashing / first-run
Flashing happens on a PC (web flasher: ESP Web Tools), so the device UI covers what follows:
```
Flash screen (device side)      First-run sequence
  ◌ "Installing 62%"             1. Boot splash -> 2. Welcome ("Hello" Light 20u, accent) ->
  ring = progress, bead          3. Wi-Fi via phone (QR) -> 4. Time zone auto (from IP/phone) ->
  "Do not unplug"                5. Pick accent + brightness (live preview) ->
  (keep APL low: ring only)      6. Optional: link Domoticz -> 7. Done: ring completes, home face appears.
OTA update: same ring, "Update 1.2 -> 1.3", rollback note: "Reverting..." on fail (warn dashed ring)
```
Each step <= 1 decision, Skip always visible (label size), progress as 6 beads on the bottom arc. Recovery mode: grey ring, mono text with the serial/URL, hold to reset.

---------------------------------------------------------------------------

## 11. Iconography

- Style: **2 px-equivalent stroke, rounded caps, 24-grid**, outline default and filled for "active/selected". Matches Halo's thin-line identity.
- Sources (open): **Lucide** (ISC, ~1,500 outline icons, consistent), **Phosphor** (MIT, 6 weights incl. thin/light/fill: great for AMOLED), **Material Symbols** (Apache 2.0, variable weight/fill, broadest coverage incl. energy and home). Recommended: Phosphor (Light + Fill) as the base set, Material Symbols for gaps (solar_power, ev_station, ...). Mixed styles are normalised by stroke weight; keep an `icons/SOURCES.md` with licence per icon. Include licence texts in the firmware "About".
- Sizes (units): `icon-s` 5.2u (24 px), `icon-m` 7u (32), `icon-l` 11u (52), `icon-xl` 22u (103, launcher hero). Hit area independent (>= touch-min).
- **Packing for LVGL**:
  1. Preferred for glyph-like icons: build an **icon font** with `lv_font_conv` from SVG-to-TTF (e.g. fantasticon / svgtofont), ranges in the Private Use Area (U+E000+), `--bpp 4`, sizes 24/32/52. Icons then colourise by text colour, can be placed inline in labels, cost 0.2-1 KB each.
  2. For the large launcher icons (103 px), use **A8 (8-bit alpha) images** `LV_IMG_CF_ALPHA_8BIT`/`LV_COLOR_FORMAT_A8` recoloured at runtime with `img_recolor`; ~10 KB each (compress with LVGL's RLE/LZ4 if enabled), or 4-bit alpha (A4, ~5 KB).
  3. Never RGB565 bitmaps for UI icons (can't re-tint with accent, burns colour).
  4. Brand/app icons by third parties: same pipeline, A8 mask only, single colour.
- Build: `tools/icons/build.py` takes a manifest (`name -> source set/name`), emits `icons_24.c`, `icons_32.c`, `icons_52.c`, and a `.h` enum, plus a preview PNG sheet. CI fails if licence field is missing.
- Rules: always label icon-only controls in settings (tooltip on long-press), one metaphor per concept, no emoji.

---------------------------------------------------------------------------

## 12. Accessibility and the 2-second rule

**Glanceable in 2 seconds**: from wake, a user must extract the key fact (time, temperature, power, door state) in <= 2 s at arm's length (~ 50-70 cm).
- One hero per screen, >= 20u tall (93 px) for the most important number when it's the screen's purpose; <= 5 distinct elements total.
- Hierarchy by size first, then luminance, then colour.
- Short words (<= 12 chars) and units always attached; verbs on buttons ("Open", not "OK").
- Text contrast >= 7:1 for essential, >= 4.5:1 minimum; non-text indicators (rings, icons) >= 3:1 against black.
- Not-colour-alone (section 3.5); test with grayscale screenshot: the screen must remain decodable.
- Touch targets >= 11u (51 px), preferred 14u; spacing between targets >= 2u; bottom and corner areas of the circle excluded (use the chord).
- Accidental activation: hold for danger, undo toast (5 s) for reversible actions, ignore touches < 80 ms.
- Reduced motion, larger text option (+15%: scale tokens 1.15 while recalculating chord fits; layouts must handle via ellipsize and 2-line fallbacks), high-contrast mode (`text-3` -> `text-2`, hairlines to 40%).
- Haptics/sound optional and never the sole feedback. Colour-blind safe palette check in CI with a simulation script.
- Language: UTF-8 strings, no text baked into images, locale-aware formats (24h/12h, decimal comma, units).

---------------------------------------------------------------------------

## 13. Implementation notes (for ui/ toolkit)

- All metrics derive from `ui_u()` = `D/100`; `ui_px(units)` rounds half up; board profile gives `D`, `Rs_factor`, `touch_inset`.
- Theme file `ui/theme.h`: colour tokens in RGB565 (pre-mixed glow/dim values: no runtime alpha where avoidable), accent palette chosen at runtime from NVS.
- Layout helpers: `ui_band(y_frac, h_frac)` returns a container clipped to the chord; `ui_ring(r_frac, stroke, start, sweep)`; `ui_polar_place(obj, r_frac, theta)`.
- Golden-image tests: render each screen host-side at 466 and 480 with fake data, diff images, plus automated check: **no non-black pixel outside radius R** and **no essential pixel outside Rs**.
- Screenshot harness also computes APL; CI warns if a screen exceeds 12% (AOD 3%).
- Fonts and icons baked at build; asset budget target < 600 KB flash total for v0.1.

---------------------------------------------------------------------------

## 14. Design review checklist

Round and geometry
- [ ] Everything essential inside Rs (0.92R); worst-edge chord rule applied to each row.
- [ ] Looks right at 466 and 480 (golden images both); no per-device pixel constants.
- [ ] Rings/gaps use tokens; bottom gap kept for hand and home pill.

AMOLED and power
- [ ] Background pure `#000000`; no grey fills > 15% of area; APL <= 12% (AOD <= 3%).
- [ ] Pixel-shift active; no static bright element for > 5 min; hairlines <= 30% luminance.
- [ ] Brightness steps, auto-dim and AOD tested; blue/white fields avoided.

Colour and type
- [ ] Only tokens used; contrast >= 4.5:1 text, >= 3:1 graphics; essential text >= 24 px.
- [ ] State is conveyed by glyph/shape/text as well as colour; grayscale test passes.
- [ ] Numerals tabular; fonts subset; sizes from scale only.

Interaction
- [ ] Touch targets >= 11u, spaced >= 2u; edge gestures start in the annulus.
- [ ] Back and home always reachable; every screen exit is obvious.
- [ ] Dangerous actions use hold-to-confirm; stale UI cannot trigger actions; undo where reversible.
- [ ] Gesture conflicts checked (rim drag vs edge swipe vs list drag).

Data and states
- [ ] Loading, empty, stale, error, offline states designed and demo-able with fake data.
- [ ] Unknown != zero; units explicit; last-updated visible when stale.
- [ ] Error messages are one human sentence plus an action.

Motion and performance
- [ ] Durations from tokens; no full-screen blends; animations time-based; 15 fps floor acceptable.
- [ ] Reduced-motion honoured; ambient loops pause in low power and AOD.
- [ ] Heap/PSRAM and flash cost of new assets listed in the PR.

Glanceability
- [ ] 2-second test: a colleague names the key fact within 2 s of wake.
- [ ] One hero, <= 5 elements, no truncated essential text.
- [ ] Screenshot at 466 and 480 attached; hardware photo once available.

---------------------------------------------------------------------------

## 15. Open questions
- Real QSPI throughput and partial-update support of the 1.75"/2.16" panels (decides pixel-shift cost, AOD redraw approach).
- Is a hardware ambient light sensor, IMU wrist-raise, or haptic motor present on each board revision?
- Hardware-measured bezel offset and glass curvature to calibrate `Rs`.
- Do both boards expose a user button usable as home/power menu?
- Final choice of font set vs flash budget (ship Inter + Space Grotesk subset first).
- LVGL version (v8 vs v9) affects image formats (A8/A4) and bezier anim API names above; verify when ADR-002 is written.

## 11. Weather tiles (Temperature and Weather pages, NSPanel style)

Domoticz data only: no forecast service, no new network client. Every device is judged by ITS OWN readings; there is no "outdoor device" and nothing is combined across devices. Code: `components/ru_apps/ru_wx.c` (pure C: bands, icon choice, tile text), widget `ru_wx_tile_*` in `components/ru_ui/ru_widgets.c`, wiring in `ru_dzapp.c` (`wx_content`, `wx_tile_update`).

### 11.1 Anatomy (centred, 3 x 2 grid, tile 25u x 21u)
```
 +-----------------------+
 |      Device name      |  Montserrat 14, dim, one line, LONG_DOT (name label has its final width from creation)
 |    [icon] 21.5 C^     |  icon 24 px (Lucide font), value Montserrat 32 (24, then 16 when icon+value+unit do not fit,
 |        45%            |  last the icon goes), unit + trend chevron in one dim 16 label on the value baseline
 +-----------------------+  small line: Montserrat 14 dim ("45%", "NW  G 6.1", "FORECAST"); the UV word in its band colour
```
6 child objects per tile (name, icon, value, unit, small line, 6 px stale dot in the bottom right corner), shared `lv_style_t`s, labels clip and never rewrite their text, geometry is a pure function of the strings, every setter compares before touching (a repeat call invalidates nothing; `sim/tests/test_wxtile.c`, `test_dzflicker.c`). Stale or unknown: icon and value grey, warn dot, 1 px border (two channels; colour is never the only signal). The icon alone carries the band colour; the value text stays white.

### 11.2 Colour bands (stepped; every colour is at least 3:1 on the tile surface #0E1013, tested)
| Reading | Bands |
|---|---|
| temperature, deg C (F converted; unknown unit: no colour) | <=0 #A8D8FF, <10 #4DB3FF, <18 #3DE0C5, <24 #3DDC84, <30 #FFC247, <35 #FF8A3D, else #FF5A5F |
| humidity | HumidityStatus first (Dry amber, Comfortable/Normal green, Wet blue), else <30 amber, >70 blue, else green |
| UV (WHO) | <3 green, <6 #FFC247, <8 #FF8A3D, <11 #FF5A5F, else #8C7CFF |
| wind, converted to m/s from the unit Domoticz gave (unknown unit: no colour) | <1.5 dim, <5.5 #4DB3FF, <10.8 #3DE0C5, <17.2 #FFC247, else #FF5A5F; the Beaufort word (`ru_wx_beaufort_word`) is the small line when there is no gust or direction |
| rain RATE (assumed mm/h: Domoticz gives no unit; none is printed) | 0 dim, >0 #4DB3FF, >=2.5 #4D7CFF, >=10 #8C7CFF |
| barometer | text colour; the icon follows ForecastStr |

### 11.3 Icon per device kind (from the device's own readings)
temperature / humidity: thermometer (small line: humidity %); humidity only: droplet; rain gauge: cloud (rate 0 or no rate, dim), drizzle (rate > 0), rain (rate >= 7.6 mm/h); UV: sun-dim below 3, sun from 3 (small line: the WHO word in the band colour); wind: an arrow that points where the wind blows TO (the reading is the bearing it comes FROM), the wind icon when there is no bearing (small line: compass word and `G gust`); barometer: gauge, or sun / cloud-sun / cloud / cloud-rain / cloud-snow / cloud-lightning mapped from the device's own ForecastStr with the small line `FORECAST` (a pressure heuristic, not an observation); air quality: cloud-fog; anything else: no icon.

### 11.4 Icon font and its licence
25 Lucide icons (ISC; the arrows and the moon derive from Feather, MIT), private-use codepoints from U+E000 (`tools/icons/manifest.txt`, macros in `components/ru_ui/include/ru_icons.h`), one 24 px 4 bpp uncompressed LVGL font `ru_icons_24` (7.2 KB of const bitmap, flash only), accessor `ru_font_icon_s()`. Lucide has no filled variants; the set is outlined (Phosphor Fill, MIT, would be the filled alternative). `lv_font_conv` needs the npm registry, which the build host cannot reach, so `python3 -I tools/icons/gen_icons.py --size 24` (standard library only) strokes the SVGs itself (`tools/icons/lucide/`, Lucide 1.70.0). The generated C is committed, so builds need neither Python nor node. Licence text and sources: `docs/third-party/`.

## 12a. Light control (device detail of every dimmer, tunable-white and colour light)

Code: widget `ru_light_*` in `components/ru_ui/ru_widgets.c`, the colour ring image `components/ru_ui/ru_ring_img.c`, colour maths `components/int_domoticz/dz_color.c`, wiring `ru_dzapp.c` (`light_apply`, `cb_light`, `light_tick` path in `ru_dzapp_ui_tick`). Tests: `sim/tests/test_light.c` (ring pixels, layouts, touch bands), `test_dzlight.c` (through the app), screens `detail_ww`, `detail_color`, `detail_color_white`, `detail_color_off`, `detail_color_protected`.

**Square panel (two rings; u = D/100, 4.8 px at 480).** Outer ring = BRIGHTNESS: d 86u, 2.2u wide, glow, coloured with the lamp's colour (accent for a plain dimmer); touch band r 37.5u to 48.3u. Inner ring = COLOUR or TEMPERATURE: d 62u, 5u wide, a conic gradient (an RGB565 image drawn by a plain `lv_arc`: `lv_obj_set_style_arc_image_src`), a 7.5u knob filled with the current colour and a white border; touch band r 23.3u to 37.3u. The press angle must lie inside the 270 degree sweep +- 8 degrees, so the gap at the bottom and the dead zones never start a drag. Centre disc d 38u = the ON/OFF button (power glyph, large percent, caption); a tap fires only when the finger moved less than 8 px. Chips (one segmented pill 34u x 8.5u at y = +27u) only on lights with colour AND white: COLOUR | WHITE. On RGBWW the WHITE chip swaps the inner ring in place to the temperature scale (same arc, regenerated bitmap); on RGBW (one white channel) WHITE is a plain tap that sends "set white", no ring. Pure RGB lights and tunable-white lights have no chips (WW shows the temperature ring directly).

**Round panel (466, small AMOLED).** One ring plus chips BRIGHT | COLOUR | WHITE (only those the light supports) that switch which value the ring edits; a plain dimmer keeps its thin ring and the disc. Same disc.

**Colours.** Hue ring: 270 degrees of arc = hue 0..360 (red at both ends, yellow at the left, green upper left, cyan at the top, blue upper right, magenta at the right). Temperature ring left to right: warm #FF9F45, #FFE3BF, cool #D6E6FF with ordered dithering (RGB565 would band). The caption is a WORD: RED ORANGE YELLOW GREEN CYAN BLUE PURPLE PINK, or WARM SOFT NEUTRAL COOL DAYLIGHT, never a Kelvin number (the tool's kelvin is not real Kelvin on Hue). The disc fill is the colour dimmed (`ru_dim` 60/255), opaque.

**Dragging and commands.** While dragging only the knob, its fill, the disc fill and the large text change (the outer ring and glow re-tint on release). The command is sent on RELEASE only (no live commands: each is an HTTP call plus the 1.5 s verify). The caption shows SENDING (dim), SET (ok, 2 s), NOT CONFIRMED (warn) or the failure (error); the knob stays where it was left until the model moves or the result is in, then snaps to the model value. A colour command carries the lamp's Level as brightness (an Off lamp: its stored Level, 100 when unknown). Touching a colour, white or temperature ring on an Off lamp turns it on: the server switches it on at that level (verified on a Hue bridge, Domoticz 2026.4), so the UI sends no extra On and the verify read requires the lamp to be On. The verify tolerances are 20 degrees of hue and 40 of 255 on the temperature (measured drift of the bridge); beyond them the caption is NOT CONFIRMED. A page update from a poll or a result never rebuilds the screen: the setters compare and touch only what changed (the screen and arc objects stay; a rebuild happens only when the structure changes: capabilities, controllable, protected, name, N/M).

**Off.** The disc is the surface colour with a track-coloured border, a dim power glyph and OFF as the large text; the brightness indicator is hidden with a dim ghost knob at the stored Level; the inner ring stays at 40% opacity with its knob at the last colour (Domoticz keeps it).

**Protected, not controllable, stale.** Rings cannot be touched, knobs lose their border, chips are hidden, the caption says PROTECTED, VIEW ONLY or STALE in warn.

**Swipe.** On any detail page a left swipe steps to the next device of the list the user came from, right to the previous one, stopping at the ends (no wrap); a light page shows N/M above the name pill. A swipe is never taken from a control: a ring's touch band (light), the thermostat or selector dial, the blinds buttons, or while a control is held. The disc, the dead zones and the chips are free. The pill halves are not tappable yet (no room next to the name pill on a round panel).

**Budget.** About 24 objects (the dimmer was 15), shared styles, one 178 KB (480) / 167 KB (466) PSRAM buffer for the ring image, reused for hue and temperature and cached by (diameter, width, kind). The colour pages are a declared exception to the 12% lit-pixel rule of section 2 (a hue wheel is lit by definition; they are interactive pages, never always-on faces); `test_apps.c` allows 25% for them. Not yet checked on hardware: the look on the real AMOLED and the IPS, touch feel, and the behaviour of the lamp for every command.
