# Third-party material

## Lucide icons (weather icon font)

- What: 25 icons (thermometer, droplet, gauge, sun, sun-dim, moon, cloud-sun, cloud, cloud-rain, cloud-drizzle, cloud-snow,
  snowflake, wind, umbrella, cloud-lightning, cloud-fog, 8 arrows, thermometer-sun). The SVG files are copied unchanged into
  `tools/icons/lucide/`.
- Source: https://github.com/lucide-icons/lucide, tag 1.70.0, path `icons/<name>.svg`.
- License: ISC (Lucide) and MIT (the arrows and the moon derive from Feather, Cole Bemis). Both texts are in
  `docs/third-party/lucide-LICENSE.txt`; the notice must stay with any copy of the icons.
- Why Lucide and not a filled set: the project owner picked Lucide. Lucide has no filled variants (every icon is a 2 px
  stroke), so the icons are outlined. Phosphor "fill" (MIT) would be the filled alternative: add its SVGs and a fill
  rasteriser to `tools/icons/gen_icons.py`.
- How the font is made: `python3 -I tools/icons/gen_icons.py --size 24` (standard library only) rasterises the SVGs to
  4 bpp, uncompressed, 24 px glyphs and writes `components/ru_ui/fonts/ru_icons_24.c` and
  `components/ru_ui/include/ru_icons.h`. Codepoints are in `tools/icons/manifest.txt` (private use area from U+E000; LVGL's
  LV_SYMBOL glyphs live at U+F000 and above, so they never clash). `lv_font_conv` was not used because the npm registry is
  not reachable from the build host; the generated C files are committed, so a normal build needs neither Python nor node.
- Flash cost: 25 glyphs x 288 bytes = 7200 bytes of bitmap plus about 0.5 KB of descriptors, all const (flash, not RAM).
