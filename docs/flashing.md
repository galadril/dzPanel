# Flashing and releases

## For users

1. Open the installer page in **Chrome or Edge on a computer** (Web Serial is required; the page falls back to an `esptool` command elsewhere).
2. Pick your board and channel, plug the board in with a USB **data** cable, press Install.
3. Set Wi-Fi in the dialog that follows, then use step 3 on the page to point the device at Domoticz. See `docs/provisioning.md`.

If the board is not found: hold **BOOT** while plugging it in, close other programs using the port (serial monitors), and on Linux add yourself to the `dialout` group.

A reinstall asks before erasing. Say no to keep Wi-Fi and Domoticz settings, yes for a factory-clean device.

## For maintainers

### Publishing a build

```bash
# 1. merged image (bootloader + partition table + app at offset 0), per board
idf.py merge-bin            # verified: writes merged-binary.bin, no extra flags needed (see "What was verified")
# 2. add it to the site tree
python3 -I tools/make_manifest.py --board amoled-175 --channel stable --version 0.1.0 \
        --merged build/merged.bin --out site-out --date "$(date -u +%F)"
# 3. copy site/ next to it and publish the directory (GitHub Pages)
cp site/index.html site/app.js site/lib.js site/style.css site-out/ && cp -r site/vendor site-out/
```

`make_manifest.py` checks the image (size, ESP image magic `0xE9`), writes the ESP Web Tools manifest beside the binary, updates `versions.json` (schema 1, with SHA-256), and prunes to the newest N per board and channel. Failures publish nothing.

Everything is same-origin and relative, so there is no CORS or redirect problem (GitHub release downloads redirect and are not reliable for browser fetches).

### Vendoring the installer component

`tools/vendor_esp_web_tools.sh` builds ESP Web Tools from a pinned commit into `site/vendor/` (git-ignored, built by CI at release) so the page never loads code from a CDN. **It has not been run** (no npm registry access in the authoring sandbox).

### Tests

```bash
node tests/site/test_lib.mjs                       # version ordering, same-origin guard, URL/token/config checks
python3 -I -W ignore tests/tools/test_make_manifest.py
```

## What was verified, and how

From the ESP Web Tools source at commit `384b35b` (read, not run):

- Manifest fields: `name`, `version`, `builds[].chipFamily` (includes `ESP32-S3`), `builds[].parts[{path,offset}]`, optional `serialType`, `new_install_prompt_erase`, `new_install_improv_wait_time`.
- **Without Improv, installing erases the device unless `new_install_prompt_erase` is true.** Our manifests set it.
- Part paths resolve relative to the manifest URL. A build with no `serialType` matches any serial type.
- Works only on HTTPS or localhost. Depends on `improv-wifi-serial-sdk` 2.8.1 and `esptool-js` ^0.7.
- No font or CDN requests in the source. The Home Assistant link appears only if `home_assistant_domain` is set; ours is not.

## Not verified

- **No real browser test**: `site/app.js` (DOM glue, Web Serial send) has never been loaded in a browser. Only `lib.js` and the manifest tool have tests.
- The vendor build script. (`idf.py merge-bin` and the fallback `esptool` syntax are now settled, see below.)
- That the merged image boots. Only compiled and merged on host; no board has run it.
- That the real firmware speaks Improv, and the whole `RU+CFG` device side.
- Content-Security-Policy: not set, because ESP Web Tools' needs (inline styles) are untested. Add one once it can be tried in a browser.
- Flashing an actual board. None has been available.

## Settled by building firmware (2026-11-01, ESP-IDF 5.5.6, compiled and merged, never run on a board)

- `idf.py -B <builddir> merge-bin` (no extra flags needed) writes `<builddir>/merged-binary.bin`. Options: `-o`, `-f raw|uf2|hex`, `-t/--flash-offset`, `--fill-flash-size`. The raw image is the bootloader at 0x0, partition table at 0x8000, `otadata` (all 0xFF, so `ota_0` boots) at 0xf000 and the app at 0x20000. It is **not** padded to 16 MB (914 800 bytes for the skeleton), which is fine for ESP Web Tools writing at offset 0.
- The merged image header says **DIO, 16 MB, 80 MHz** even though `sdkconfig` selects QIO: IDF writes DIO for QIO and lets the second-stage bootloader upgrade. IDF's own printed flash command passes `--flash_mode dio --flash_size 16MB --flash_freq 80m` explicitly; those flags are optional for the fallback, because `esptool` defaults to `keep` (see the last item below) and then uses the same values from the image header.
- `tools/make_manifest.py` accepted that file; `site/lib.js` `validateVersions` returned `ok` and `pickBuild` found it (script in the fw-core notes, `docs/progress/fw-core.md`).
- Minor: `make_manifest.py` writes `manifest.json` and `versions.json` with mode 0600 (via `mkstemp`). CI must `chmod 644` before publishing, or the tool should do it.
- `site/lib.js` `esptoolCommand()` prints `write_flash 0x0 <file>` without flash flags. **Checked in the esptool 4.12 source:** for `write_flash` the flash mode, frequency and size all default to `keep`, so the merged image's own header (DIO, 16 MB, 80 MHz) is used as is. Not yet tried against a real board.
