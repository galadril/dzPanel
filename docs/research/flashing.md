# dzPanel: Build, Release, Flash, Update

Status: research and proposal, feeds ADR-006 (firmware update and recovery).

**Evidence caveat.** WebFetch was blocked in this session. Only one web search worked (Waveshare board specs). Everything about ESP Web Tools, esptool-js, ESP-IDF OTA and Improv comes from prior knowledge, not live verification. Items tagged `[VERIFY]` must be checked against current docs before implementation.

## 0. Assumptions

| Item | Value | Confidence |
|---|---|---|
| SoC | ESP32-S3 (`chipFamily: "ESP32-S3"`) | High |
| 1.75" flash/PSRAM | 16 MB flash, 8 MB octal PSRAM (Waveshare wiki and ESPHome devices page) | Medium. Check the board received. |
| 1.75C variant | Aluminium enclosure, **32 MB** flash. This is a different SKU. | Medium, from search result |
| 2.16" flash/PSRAM | **Assumed** 16 MB / 8 MB. Not verified. | Low `[VERIFY]` |
| USB | Native USB-Serial/JTAG on GPIO19/20, no UART bridge chip | Medium `[VERIFY]` per board |
| Framework | ESP-IDF (ADR-001 pending). If Arduino is chosen, the partition and OTA design is the same, but the build workflow changes. | n/a |

Flash size is the main assumption. The partition table is a template. The build generates it per board from a `flash_size` field in the board profile (`boards/<id>/board.yml`). A board with a different flash size gets a different CSV and its own manifest, and the layout logic is not hard-coded.

## 1. Decisions in brief

1. **Distribution:** GitHub Releases hold the artifacts. GitHub Pages hosts the "dzPanel Installer" web flasher.
2. **Flashing:** ESP Web Tools (`<esp-web-install-button>`) with a **single merged binary per board**. It is written at offset 0 and is the least error-prone option.
3. **Provisioning:** **Improv Serial** right after flashing (supported by ESP Web Tools `[VERIFY]`). SoftAP captive portal is the fallback. BLE provisioning is deferred.
4. **OTA:** HTTPS pull from GitHub Releases or the project's Pages site, using `esp_https_ota` with app rollback enabled. Secure Boot v2 and signed images come later as an opt-in.
5. **Recovery:** safe-mode boot (the factory app) plus a hold-button entry. Browser re-flash is always available through the ROM bootloader, because native USB cannot be bricked by app bugs.
6. **Channels:** `stable`, `nightly`, and optional `beta`, as separate manifests.

## 2. Web flashing technology

### 2.1 ESP Web Tools

- A web component, `<esp-web-install-button manifest="...">`, from the ESPHome project. It uses **esptool-js** internally.
- It needs the **Web Serial API**: Chrome, Edge and Opera on desktop. **No Firefox or Safari**, and no iOS. Android Chrome has Web Serial only through USB-OTG, and support is partial/experimental `[VERIFY]`. The installer page must detect `navigator.serial` and show a clear "use Chrome or Edge" message plus a fallback (section 8.5).
- Needs a **secure context**: HTTPS or localhost. GitHub Pages satisfies this.
- The button has slots for browser-unsupported and unsupported-chip messages. The `erase-first` attribute and the manifest's `new_install_prompt_erase` option offer an erase prompt `[VERIFY exact names]`.
- It provides Improv Serial after install if the firmware speaks Improv `[VERIFY]`. It also shows a log/console dialog.
- The user picks the serial port in the browser's port chooser. On native-USB S3 this is the "USB JTAG/serial debug unit" (VID 303A, PID 1001).

### 2.2 esptool-js

- Direct JS port of esptool (npm `esptool-js`). Use it if we outgrow the ESP Web Tools UI, for example to build a custom installer with a board picker, erase-NVS-only, read-flash-id, or custom console. Option: start with ESP Web Tools, then replace with esptool-js plus `Improv` JS SDK if the UI needs it.
- Risk: a custom flasher means owning reset/boot sequencing bugs on USB-JTAG chips. Start with ESP Web Tools.

### 2.3 manifest.json format

```json
{
  "name": "dzPanel (AMOLED 1.75)",
  "version": "0.3.1",
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "improv": true,
      "parts": [
        { "path": "dzpanel-amoled175-v0.3.1-merged.bin", "offset": 0 }
      ]
    }
  ]
}
```

Notes:
- `parts[].path` is relative to the manifest URL, or absolute. `offset` is a decimal number in the JSON.
- `improv` is a build-level flag in some versions `[VERIFY]`. If it is missing, the Improv dialog does not appear.
- **Multi-part alternative** (bootloader at `0x0`, partition table `0x8000`, otadata `0xe000`, app `0x10000`): useful when you want to avoid rewriting NVS or storage. Note that ESP32-S3 bootloader offset is **0x0**, not 0x1000, which is the old ESP32 value. It is easy to get wrong.
- **Merged vs multi-part:**

| | Merged | Multi-part |
|---|---|---|
| Simplicity | One file, one offset | Four to six parts, offsets must match CSV |
| Download size | Padded to the last used offset, so it can be much larger than the app (fill bytes). Typically 1-3 MB unless a storage image is appended. | Smaller |
| Wipes NVS/settings | Zeros between parts are written, so otadata is overwritten and NVS may be too if the merged image spans it | Can leave NVS alone |
| Failure modes | Few | Offset drift between CSV and manifest |

Decision: **merged for fresh install** (per board, per version). Use `esptool.py merge_bin` (or `idf.py merge-bin`) with `--flash_mode dio --flash_freq 80m --flash_size 16MB` matching the sdkconfig `[VERIFY: S3 octal PSRAM boards typically use qio/80m or dio; take the values from build/flasher_args.json]`. Do not hand-type flags. Generate them from `flasher_args.json`.
- OTA updates use only the app `.bin` (not merged). Same release, two artifacts.

### 2.4 Per-board builds

- One firmware image per board, because pins, display drivers and touch drivers are compile-time board profiles (design doc section 2). Targets: `amoled175`, `amoled216`. All use `chipFamily: ESP32-S3`, so ESP Web Tools cannot auto-select by chip. The **user picks the board** in our page, which loads the matching manifest. A "wrong board" flash is not detectable by chip ID, so use a runtime guard: firmware checks a board-ID marker (`esp_app_desc` project_name = `dzpanel-amoled175`) and the OTA code refuses an image whose `project_name` differs from the running one.
- Naming: `dzpanel-<board>-<version>-merged.bin`, `dzpanel-<board>-<version>-app.bin`, `manifest-<board>-<channel>.json`.

## 3. Build and release CI

### 3.1 Tooling

- **Docker image `espressif/idf:v5.x`** (pin the tag, for example `v5.4.x` `[VERIFY the latest]`) is the most reproducible. Pin by tag, and by digest when possible.
- **`espressif/esp-idf-ci-action`** wraps the same image as a GitHub Action (inputs: `esp_idf_version`, `target`, `path`, `command`) `[VERIFY inputs]`. Either works. The sketch below uses the `container:` approach, which is more transparent.
- Matrix over boards. Cache `managed_components` (IDF Component Manager) and ccache.

### 3.2 Versioning and channels

- SemVer tags `vX.Y.Z` drive **stable**. A scheduled build of `main` produces **nightly** (`0.4.0-nightly.20261031+gabc1234`), published as a rolling pre-release tagged `nightly`, with assets replaced each run.
- The version string goes into `esp_app_desc` through `PROJECT_VER` (use `git describe`). The OTA compare uses a numeric build/semver comparison. Nightly vs stable ordering must be defined: a nightly device should not "downgrade" to an older stable unless the user changes channel.
- Channel is a device setting. The device fetches `https://<owner>.github.io/roundos/ota/<board>/<channel>.json`:

```json
{ "version": "0.3.1", "board": "amoled175", "min_version": "0.2.0",
  "url": "https://github.com/OWNER/roundos/releases/download/v0.3.1/dzpanel-amoled175-v0.3.1-app.bin",
  "sha256": "<hex>", "size": 1234567, "notes": "..." }
```

### 3.3 Pages hosting and CORS

- **Important:** GitHub Release asset downloads **redirect** (to `objects.githubusercontent.com` / release-assets), and I recall they do **not** reliably send permissive CORS headers for browser `fetch` `[VERIFY]`. ESP Web Tools fetches the manifest and the parts with `fetch()`, so cross-origin release URLs may fail in the browser.
- **Decision:** the workflow **copies** the `*-merged.bin`, the manifests and the OTA JSON into the `gh-pages` branch (or the Pages artifact) so the installer fetches **same-origin** files. GitHub Pages serves `access-control-allow-origin: *` for its static files `[VERIFY]`, but same-origin avoids the question. Releases keep the canonical copies and checksums.
- Size limits: Pages sites are soft-limited to about 1 GB and repo files should stay under 100 MB. Merged images are a few MB, so keep a history of **only the last N versions** on Pages (prune in the workflow).
- The ESP32 does OTA fetches without CORS (not a browser), but a redirect from github.com to the CDN needs `esp_https_ota` to follow redirects (`.max_redirection_count`, `HTTP_EVENT_REDIRECT`) and a certificate bundle (`esp_crt_bundle_attach`). Serving OTA from Pages (direct, no redirect, Fastly CA) is simpler. Use the Pages URL for OTA, and put the `sha256` in the OTA JSON.

### 3.4 Workflow sketch

```yaml
name: release
on:
  push: { tags: ['v*'] }
  schedule: [{ cron: '0 3 * * *' }]     # nightly
  workflow_dispatch:
permissions: { contents: write, pages: write, id-token: write }
jobs:
  build:
    runs-on: ubuntu-latest
    strategy:
      matrix: { board: [amoled175, amoled216] }
    container: espressif/idf:v5.4.1     # pin; VERIFY version
    steps:
      - uses: actions/checkout@v4
        with: { fetch-depth: 0, submodules: recursive }
      - name: Compute version/channel
        run: |
          git config --global --add safe.directory "$GITHUB_WORKSPACE"
          if [[ "$GITHUB_REF" == refs/tags/v* ]]; then
            V=${GITHUB_REF_NAME#v}; CH=stable
          else
            V="$(git describe --tags --abbrev=7 --always)-nightly"; CH=nightly
          fi
          echo "V=$V" >> $GITHUB_ENV; echo "CH=$CH" >> $GITHUB_ENV
      - name: Build
        run: |
          . $IDF_PATH/export.sh
          idf.py -B build -DBOARD=${{ matrix.board }} \
            -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/${{ matrix.board }}/sdkconfig.board" \
            -DPROJECT_VER="$V" set-target esp32s3 build
          idf.py -B build merge-bin -o out-merged.bin   # or esptool.py merge_bin from flasher_args.json
      - name: Package
        run: |
          mkdir dist; N=dzpanel-${{ matrix.board }}-$V
          cp out-merged.bin dist/$N-merged.bin
          cp build/dzpanel.bin dist/$N-app.bin
          (cd dist && sha256sum * > $N.sha256)
          python3 tools/make_manifest.py --board ${{ matrix.board }} --version $V \
            --channel $CH --out dist
      - uses: actions/upload-artifact@v4
        with: { name: fw-${{ matrix.board }}, path: dist }
  publish:
    needs: build
    runs-on: ubuntu-latest
    steps:
      - uses: actions/download-artifact@v4
        with: { path: dist, merge-multiple: true }
      - name: GitHub Release (stable tag, or rolling nightly)
        uses: softprops/action-gh-release@v2
        with:
          tag_name: ${{ github.ref_type == 'tag' && github.ref_name || 'nightly' }}
          prerelease: ${{ github.ref_type != 'tag' }}
          files: dist/*
      - name: Assemble Pages site (installer + manifests + bins)
        run: ./tools/assemble_pages.sh dist site   # merges with previous site, prunes old
      - uses: actions/upload-pages-artifact@v3
        with: { path: site }
      - uses: actions/deploy-pages@v4
```

Gaps: `idf.py merge-bin` flags and Pages merge with prior content (deploy-pages replaces the whole site, so `assemble_pages.sh` must fetch the previous site or keep it in a `gh-pages` branch). Pick the branch approach for simplicity. Build-time checks should also run on PRs without publishing. Signing step is added in section 6.

## 4. USB bootloader entry on native-USB ESP32-S3

- Native USB-Serial/JTAG: esptool and esptool-js can reset into the ROM bootloader automatically through the USB-JTAG reset sequence (they toggle DTR/RTS, which the USB-Serial/JTAG peripheral maps onto EN/GPIO0). This usually works without button presses `[VERIFY in the browser for these boards]`.
- When the firmware is **running a user app that reconfigures USB** (for example TinyUSB CDC/HID/MSC instead of the built-in USB-Serial/JTAG), auto-reset no longer works, and the port can have a different VID/PID. **Design rule: dzPanel must keep the USB-Serial/JTAG console enabled** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), and not use TinyUSB on that port, at least in MVP.
- Manual entry: hold **BOOT (GPIO0)**, tap **RESET/EN** (or plug USB in while holding BOOT), release BOOT. The device appears as "ESP32-S3 (download mode)". The Waveshare boards have BOOT and PWR/reset buttons `[VERIFY the layout and labels per board]`.
- Crash loop case: a looping firmware that resets USB rapidly can make the port flicker. Holding BOOT during the plug-in is the reliable cure. The installer page documents this with a photo per board.
- Windows: the S3 ROM download-mode device needs a driver on some older Windows versions (WinUSB/usbser `[VERIFY]`). Chrome on Linux needs udev rules for `/dev/ttyACM*` (user in the `dialout` group).
- After flashing, the chip does a hard reset through USB-JTAG. On some hosts the port re-enumerates, and the Web Serial handle is lost, so the installer must re-prompt for the port (see Improv flow, section 5).

## 5. WiFi and device provisioning

| Option | Pros | Cons |
|---|---|---|
| **Improv Serial** (over USB, Web Serial) | Works directly after flashing, the same browser session, no phone. Wi-Fi credentials never hit the air. ESP Web Tools has it built in. | Needs Web Serial (Chromium desktop). Credentials only; Domoticz URL needs an extension. |
| Improv WiFi (BLE) | Phone-friendly | Needs BLE stack (RAM/flash), Web Bluetooth (Chrome/Android only), more complexity |
| SoftAP + captive portal | Works with any phone/browser, no USB | Clunky UX, iOS captive quirks, the board must show the AP name/QR on screen. Needs an HTTP server and a DNS catch-all. |
| ESP-IDF `wifi_provisioning` (SoftAP/BLE + official phone apps) | Official, security1/2 handshake | Requires the Espressif phone app or custom client |
| On-device touch entry | Fully offline | Typing a long password on a 1.75" round screen is awkward. Fallback only. |

**Decision:**
1. **Primary: Improv Serial.** Firmware implements the Improv serial protocol on the USB-Serial/JTAG console: RPC commands for set-wifi, get-state, get-device-info, and scan networks (`Wi-Fi scan` is an optional RPC `[VERIFY]`). Run Improv only when unprovisioned or in setup mode, and keep it multiplexed with the log output (Improv packets are framed with the `IMPROV` header, so it coexists with text logs).
2. **Fallback: SoftAP captive portal** (`dzPanel-XXXX`, QR code on screen). It starts automatically when there are no credentials, or after the 3-failure join limit, or via the long-press setup gesture. The portal page also holds the Domoticz URL and credentials.
3. **Domoticz and other app settings** are not part of Improv. Options: (a) use the **device's own web UI over LAN** (`http://dzpanel-xxxx.local`) once WiFi is up. The installer shows the link from Improv's "redirect URL" (Improv supports returning a URL after provisioning). (b) A custom serial JSON-RPC (`dzpanel.config.set`) used by the installer's console tab as an advanced option. Pick (a) for MVP. It avoids inventing a protocol and works with the secrets rules in the design doc. Secrets are stored in NVS only (consider NVS encryption later, see section 6).
4. BLE provisioning: **later**, only if a phone-only flow is a demanded feature. BLE+WiFi coexistence also costs RAM.

## 6. Partition table, OTA, rollback, security

### 6.1 Partition CSV (16 MB flash assumption)

App slot sizing: LVGL plus fonts plus image assets plus TLS/HTTP/Domoticz and an HTTP server may reach 2-3 MB. 3 MB slots leave headroom. Assets go in a separate storage partition, so app images stay smaller and asset updates are independent.

```csv
# Name,    Type, SubType,  Offset,    Size,      Flags
nvs,       data, nvs,      0x9000,    0x6000,
otadata,   data, ota,      0xf000,    0x2000,
phy_init,  data, phy,      0x11000,   0x1000,
nvs_keys,  data, nvs_keys, 0x12000,   0x1000,   encrypted
factory,   app,  factory,  0x20000,   0x200000,           # 2 MB: recovery/safe-mode app
ota_0,     app,  ota_0,    0x220000,  0x400000,           # 4 MB
ota_1,     app,  ota_1,    0x620000,  0x400000,           # 4 MB
coredump,  data, coredump, 0xA20000,  0x10000,
storage,   data, littlefs, 0xA30000,  0x5C0000,           # ~5.75 MB assets/logs/user data
# ends 0xFF0000; 0x10000 spare at end of 16 MB
```

Notes:
- Offsets are 64 KB aligned for app partitions (required). The bootloader is at 0x0 and the partition table at 0x8000 on S3. The default bootloader size is under 0x8000 and no secure boot bloats it. **With Secure Boot v2 or flash encryption the bootloader grows**. Reserve room (move nvs to 0x10000) before enabling `[VERIFY]`.
- Boot order: otadata decides between ota_0/ota_1. If otadata is empty/invalid, the bootloader falls back to the **factory** app. That gives us the safe-mode path for free.
- **Is a factory app needed?** Option A (shown): factory is a small recovery app (WiFi, Improv, OTA pull, a minimal on-screen status, no LVGL apps) and ota_0/ota_1 hold the full dzPanel. Option B: drop the factory partition, use ota_0/ota_1 only (saves 2 MB, simpler) and rely on rollback plus browser re-flash. Recommendation: **A for 16 MB boards** because the recovery story needs an on-device way out for users without a browser flasher at hand. Revisit if the recovery app is too much work in MVP. In that case ship B and keep the layout compatible, but remember that **changing the partition table later is a destructive reflash** for all users. Decide before the first public release.
- The factory slot is also the initial flash target of the merged image. The first boot goes to factory unless otadata is pre-set. For a normal install, the merged image should place the **full app in ota_0 with otadata initialised** (use `idf.py` `flasher_args` with `--flash_files` for the ota_0 offset, or have factory immediately switch). Simplest: the merged image includes the full app at the **factory** offset on first install... Resolve this: **open question**. Common practice is to flash the main app in `factory` and OTA into ota_0/ota_1, but then the factory is full-sized and needs 4 MB. Alternative is Option B with no factory. Note it in ADR-006.
- `storage` as LittleFS rather than SPIFFS (SPIFFS is deprecated and slow when full). Use `joltwallet/littlefs` component `[VERIFY it is current with the chosen IDF version]`. Do not put secrets in `storage`; use NVS.
- Boards with 32 MB flash (1.75C) use a second CSV with bigger `storage`. The manifest for the C variant is a separate board ID.
- Flash size must also be set in `sdkconfig` (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`). A mismatch makes the bootloader reject the partition table.

### 6.2 OTA with rollback

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A new image boots in `ESP_OTA_IMG_PENDING_VERIFY`. The app calls `esp_ota_mark_app_valid_cancel_rollback()` only after health checks pass (display init OK, UI loop running, optionally WiFi up and update server reachable, within a 60 s window). If it crashes or resets before then, the bootloader rolls back to the previous slot. Optionally call `esp_ota_mark_app_invalid_rollback_and_reboot()` for explicit failure.
- Use `esp_https_ota` (or `esp_https_ota_begin` with progress callbacks to draw a progress ring), with `esp_crt_bundle_attach` and TLS validation always on. No `skip_cert_common_name_check` in release builds.
- Checks before writing: `project_name` matches the board, version greater than or equal to `min_version`, image size less than slot size, and a SHA-256 of the whole file compared with the signed OTA JSON. `esp_https_ota` also validates the image header and checksum itself. Also set `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` only if secure boot is on (it needs `secure_version` handling `[VERIFY]`).
- Never OTA while on battery below a threshold or while in a critical UI action. Require an explicit user confirmation in the UI (consistent with "safe by default").
- Memory: TLS plus OTA while LVGL holds PSRAM buffers needs about 40-60 KB internal RAM `[VERIFY]`. Consider doing OTA from the recovery/factory app, or pausing the UI.

### 6.3 Signing and Secure Boot (staged)

| Stage | What | Notes |
|---|---|---|
| MVP | No signing. HTTPS with the CA bundle, plus the SHA-256 in the OTA JSON from the same origin. | The sha256 and the binary come from the same origin, so this protects against corruption only, not a compromised host. Be honest in the docs. |
| Beta | **Signed OTA images without secure boot**: `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, ECDSA/RSA signature checked at OTA (`esp_secure_boot_verify` style) `[VERIFY the exact Kconfig]`. The private key lives in a GitHub Actions secret (or better, an offline signing step). | Prevents a rogue host from installing code through OTA. Does not protect against physical flashing. |
| Later | **Secure Boot v2** (RSA-3072 or ECDSA) plus optional flash encryption. | **Irreversible eFuse burns** can brick boards if the key is lost. It also breaks "re-flash from browser" for anybody not holding the key, which conflicts with the open-source goal that users build their own firmware. Recommendation: do not enable by default. Offer it as a documented opt-in for people who build and sign their own firmware. |

### 6.4 Config survival across updates

NVS and storage partitions are untouched by OTA. A merged re-flash overwrites them (unless the merged image stops short of those offsets, which it cannot, because NVS sits before the app). So the installer offers **"Keep settings"** (multi-part install that omits NVS) vs **"Clean install"**. This is an argument to ship **both** artifacts per board: merged, and multi-part parts in a second manifest `manifest-<board>-<channel>-keep.json`. Mark as nice-to-have. Document "export settings" over the web UI as the safer path.

## 7. Recovery story

Layered, from least to most drastic:

1. **Automatic rollback** (6.2) handles a bad OTA.
2. **Boot-loop detection:** keep an RTC/NVS counter `boot_attempts`, incremented at boot and cleared after the app reaches a healthy state (~30 s). After 3 failed boots the firmware boots **safe mode**: skip apps, skip the UI shell where possible, bring up only the display with a status screen, WiFi/Improv, and OTA. (With the factory partition, this is the factory app. Without it, it is a `safe_mode` flag in the full app.)
3. **Hold-button at boot:** holding BOOT (GPIO0, usable as an input after boot) or a designated PWR/side button for about 5 s at power-up forces safe mode. A longer hold (about 15 s) offers "factory reset (wipe NVS)" with an on-screen confirmation. Note that holding GPIO0 low at **reset** enters the ROM download mode instead, which is also a recovery path, and the two must not be confused in the docs. `[VERIFY the board's button wiring and whether the PWR key goes through the PMIC]`
4. **Browser re-flash:** download mode (section 4) plus the Installer page. Always works for a native-USB S3 unless eFuses were burned (secure boot, JTAG/USB disable). **Rule: never burn the `DIS_USB_JTAG`/`DIS_USB_SERIAL_JTAG` eFuses.**
5. **Offline flash fallback:** `esptool.py --chip esp32s3 write_flash 0x0 dzpanel-...-merged.bin` documented in the README.

## 8. "dzPanel Installer" page design

Static site on GitHub Pages: plain HTML plus ES modules, no build step if possible. Dependencies pinned and vendored (ESP Web Tools, optionally esptool-js and the improv SDK) to avoid CDN drift.

### 8.1 Layout (single page, 5 steps)

```
[ dzPanel Installer ]                         [ channel: stable v | nightly ]
1. Board        ( ) AMOLED 1.75" 466x466   ( ) AMOLED 2.16" 480x480
                (photo + "check your board's silkscreen"; link to flash-size note)
2. Version      [ 0.3.1 (stable, 2026-10-20) v ]   [x] show release notes
                [ ] Erase all settings (clean install)
3. Connect &    [ Install dzPanel ]  <- <esp-web-install-button manifest=...>
   Install      progress bar, erase/write/verify states
4. Configure    Improv dialog: choose WiFi (scan list) + password
                -> after success: "Open device page" link (http://<ip>/)
                -> Domoticz URL/credentials entered on the device page (not stored by installer)
5. Console      [ Connect console ] 115200, live log, Copy / Save .txt, Clear,
                filter text, "Reset device" button (toggle RTS/DTR), "Enter bootloader"
```

### 8.2 Behaviour

- Board and channel choose the manifest URL: `manifests/<board>/<channel>.json`. A version picker reads `versions.json` (generated by CI: `[{"version","channel","date","notes","manifest"}]`). Per-version manifests are kept for the last N releases.
- `navigator.serial` missing: replace step 3 with an unsupported-browser notice (Chrome/Edge desktop), and show the esptool.py command and the download link for the merged bin. The page remains useful for downloading.
- Board mismatch protection: manifest is per board. Add the optional **"Detect"** button that connects with esptool-js, reads the chip and the flash size (`flash_id`), and warns when the flash is less than the board profile requires. It can also read the MAC and (if the factory app exposes it) a board ID over Improv's device-info RPC. `[VERIFY whether the Improv device-info can carry the board name; it returns firmware name, version, chip and device name]`.
- Console: use a separate Web Serial connection with `TextDecoderStream` and line splitting. **Do not hold the port while ESP Web Tools is flashing.** Only one owner of the port at a time, so the console tab disconnects before install.
- Console secrets: firmware must not log Wi-Fi passwords or tokens (design doc section 12). The page shows a notice before "Save log".
- Privacy: no analytics, no server. All processing is in the browser. State this on the page.
- Accessibility and text: troubleshooting accordion (boot-loop, port missing, driver, udev, hold BOOT).
- Later: "Update via browser" tab (same flow with `Keep settings`), "Backup NVS" (esptool-js read-flash of the nvs region), "Check device firmware version" via Improv device-info to recommend an update.

### 8.3 Files

```
site/
  index.html            # installer UI
  app.js, style.css
  vendor/esp-web-tools/ # pinned
  versions.json
  manifests/amoled175/{stable,nightly}.json
  manifests/amoled216/{stable,nightly}.json
  fw/<board>/<version>/dzpanel-<board>-<version>-merged.bin  (+ .sha256)
  ota/<board>/{stable,nightly}.json                          # device-facing
```

## 9. Open questions and risks

1. **Partition layout:** factory + two 4 MB slots versus two slots only. This affects all future users, so decide before public release. How does the merged image boot on first install, and which partition holds the main app (6.1)?
2. 2.16" board flash and PSRAM sizes, USB mode, and button wiring are all unverified. 1.75 vs 1.75C differ in flash (16 vs 32 MB).
3. CORS and redirect behaviour of GitHub Release assets: the plan avoids it by hosting on Pages. Verify Pages headers.
4. ESP Web Tools details to verify: `improv` flag placement, erase options, the current version and manifest schema, Android Web Serial status.
5. Improv Serial coexisting with the IDF log output on USB-Serial/JTAG, and the board-ID passthrough.
6. Signing key custody (GitHub secret vs offline) and when to stop at "signed OTA" versus Secure Boot v2 (irreversible, conflicts with open re-flash).
7. Memory headroom for TLS OTA under LVGL (internal RAM). Possibly do OTA from safe mode.
8. IDF version pin and the `esp-idf-ci-action` inputs. Reproducible builds need a pinned image digest and a lockfile for managed components (`dependencies.lock`).
9. Whether nightly users can go back to stable (downgrade policy and rollback with anti-rollback eFuse).
10. If the framework ends up being Arduino/PlatformIO (ADR-001), the CI changes (`pio run`), but the partition, manifest and installer design is unchanged.

## 12. Resolved by reading the ESP Web Tools source (2026-10-31, commit 384b35b)

Read from a shallow clone; **not run**. See `docs/flashing.md` for the user-facing summary.

| Earlier `[VERIFY]` | Answer |
|---|---|
| Improv supported by ESP Web Tools? | Yes, via `improv-wifi-serial-sdk` 2.8.1. After an install it waits 10 s for Improv by default (`new_install_improv_wait_time`, 0 disables) |
| Erase prompt option name | `new_install_prompt_erase` (`new_install_skip_erase` is deprecated). **Without Improv the default is to erase**, so we must set it |
| Per-chip builds | `builds[].chipFamily`, optional `serialType` (`cdc` or `uart`). A build without `serialType` matches any |
| Part path resolution | Relative to the manifest URL (`flash.ts`) |
| Secure context | HTTPS or localhost only; slots `activate`, `unsupported`, `not-allowed` customise the messages |
| Browser support | Detection is `"serial" in navigator`; the default message even names Firefox, so do not hard-code a browser list |
| Runtime third-party requests | None found in source. The rollup config lists an unused external `improv-wifi.com` entry; the source never imports it |

`idf.py merge-bin` (IDF 5.5.6) is settled: see `docs/flashing.md`, last section (output `merged-binary.bin`, flags `-o -f -t --fill-flash-size`, header is DIO for a QIO build, otadata included and blank). Still open: `esp_https_ota` details, driver notes for Windows, and every hardware claim.
