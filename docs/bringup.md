# First bring-up on a real board

**Status: nothing in dzPanel has ever run on hardware.** Every "expect" below is a hypothesis to confirm, not a known result. This is `BUILD_PLAN.md` track C0 to C3 as a runbook for the first hour.

The authoring sandbox cannot see USB devices, so flashing and watching the serial console happen on your computer. Send back what the last section asks for and the next iteration can start from facts.

## What is in `dist/` (git-ignored; verify with `SHA256SUMS`)

| File | What it is | Use |
|---|---|---|
| `waveshare-factory-1.75-16MB.bin` | Waveshare's own full 16 MiB factory image (their `FactoryOnly-260805`) | **Recovery.** Write once at `0x0` to restore the board to how it shipped |
| `waveshare-demo-1.75-merged.bin` | The **unmodified** Waveshare LVGL demo, built here with our toolchain | Proves our toolchain and config produce an image that works, before blaming our code |
| `dzpanel-1.75-merged.bin` | dzPanel firmware (ESP-IDF 5.5.6, LVGL 9.4.0) | The real test |
| `dzpanel-1.75.elf` | Symbols for the dzPanel image | Decode crash backtraces (below) |

All images are written at offset `0x0`. Do not mix files from different packages (Waveshare's own advice).

## Step 0: first look (before flashing anything)

1. Photograph the board front and back. Note the markings and whether it is the `-G` (GNSS) variant.
2. Power it as shipped and note what the factory firmware does: does the display light, is the circle fully visible, does touch respond?
3. Plug it into the computer. Note how it enumerates (Windows Device Manager: a COM port, or an "ESP32-S3" device).

## Step 1: tooling and a read-only probe

```bash
python -m pip install esptool
python -m esptool --chip esp32s3 -p COM5 flash_id        # use your port; Linux: /dev/ttyACM0
```

Record the flash size and chip features. Expectation (unconfirmed): 16 MB flash, ESP32-S3 with octal PSRAM. This command does not write anything.

**Download mode.** Waveshare lists `BOOT` as the GPIO0 download-mode strap. If the port is missing or `flash_id` fails, hold `BOOT` while plugging the USB cable in, then retry. Whether the native USB resets into download mode automatically on this board is **not confirmed**: note which of the two you needed.

## Step 2: our toolchain on their demo

```bash
python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x0 waveshare-demo-1.75-merged.bin
```

Expect Waveshare's LVGL demo to run. If it does not, the problem is in the toolchain or sdkconfig, **not** in dzPanel code. Stop and report.

## Step 3: dzPanel

```bash
python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x0 dzpanel-1.75-merged.bin
python -m serial.tools.miniterm COM5 115200               # the serial log (pyserial comes with esptool)
```

Expectations (hypotheses):

- The boot log prints free internal and PSRAM heap at three points.
- A device with no Wi-Fi and no Domoticz settings shows a **NOT SET UP** screen (text only).
- Nothing connects to anything until it is provisioned.

Check by eye and record:

1. Is anything clipped at the rim of the circle?
2. **Touch orientation.** The Waveshare driver defaults to mirror on both axes. Tap near the top, bottom, left, right edges and the centre. Does the response land where you touched? If mirrored or swapped, say which.
3. Colours, flicker, tearing, brightness.
4. Does it reboot by itself? (Boot-loop detection is a simple counter: three failed boots lead to a "safe mode".)

## Step 4: connecting it to Wi-Fi and Domoticz (option B chosen)

The device needs Wi-Fi credentials and a Domoticz address plus token. The paths in the design, and where each stands:

| Path | State |
|---|---|
| Improv Serial from the installer's install dialog (Wi-Fi) | Firmware side built and host-tested. The installer needs the ESP Web Tools component, which **has not been built** (`tools/vendor_esp_web_tools.sh` needs npm and has never run) |
| **`RU+CFG` over USB from the installer page's step 3: Wi-Fi name and password plus Domoticz address and token, in one line (option B, chosen)** | Firmware side built and host-tested (Wi-Fi is joined first with 'save only on got-IP, else revert', then the Domoticz test); the Wi-Fi join and the console glue are **compile-checked, not run**. The page has **never been loaded in a browser**. It needs Chrome or Edge and a local server (`python3 -m http.server` in `site/`; `localhost` is allowed) |
| SoftAP setup portal (phone) | Written and host-tested, but **not wired into the firmware** |

**Decision: B.** A first setup now works from the installer page alone, with no Improv dialog: fill in the Wi-Fi name and password and the Domoticz address and token, press *Send to device* (the page waits up to 45 s). The options considered, kept as notes:

- **A. Build the installer component** on a machine with Node and npm (`tools/vendor_esp_web_tools.sh`, unverified), then use the page's install dialog for Wi-Fi and its step 3 for Domoticz. No new code, but it depends on an unrun script and an untested page.
- **B (chosen, built). Add Wi-Fi to the USB setup line** (`RU+CFG` gets an optional `wifi` field and the page gets two inputs). Small and testable on the host, and it works from one page without Improv. Needs a firmware rebuild, and the Wi-Fi password travels over the USB cable like the token does.
- **C. Wire the SoftAP portal into the firmware.** The friendliest path (a phone and a QR code, no computer needed) but the largest change: the portal glue is only compile-checked and needs the screen and the supervisor wired in.

B for the first week on hardware (smallest, host-testable), C afterwards as the path for ordinary users. The page, the Wi-Fi password and the token: see `docs/provisioning.md` (the password travels over the USB cable to the device only). Replies to expect: `wifi_failed` (with `no_ap`, `auth` or `timeout`) means the join failed and nothing changed; a Domoticz error with `wifi_saved` means the Wi-Fi is saved and only Domoticz needs fixing, so send the Domoticz part again without the Wi-Fi.

When you do connect it to your own Domoticz: **use a Viewer token first** (a display that cannot switch anything). Never paste the token into a chat, a log or a file.

## Recovery

```bash
python -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x0 waveshare-factory-1.75-16MB.bin
```

Restores the board to its shipped state. `erase_flash` is only needed for a clean slate and is not part of the normal flow.

## Decode a crash

If the serial log shows a backtrace with addresses like `0x42xxxxxx`:

```bash
xtensa-esp32s3-elf-addr2line -pfiaC -e dzpanel-1.75.elf 0x42001234 0x42005678
```

The tool is in the ESP-IDF toolchain (`/opt/esp/tools/.../xtensa-esp32s3-elf-addr2line` in the authoring environment). The `.elf` must match the flashed image exactly: do not rebuild between flashing and decoding.

## What to send back

- Photos and the markings from step 0; what the factory firmware did.
- `flash_id` output (flash size, PSRAM, chip revision).
- Whether you needed to hold `BOOT`, and what Windows showed when it enumerated.
- Result of step 2 (their demo) and step 3 (ours), with the **first 60 lines of the serial log** after a reset. Remove any Wi-Fi name or address you do not want to share. The firmware is designed never to log the token, but check before you paste.
- The touch result per edge and centre, and anything clipped at the rim.

## Known unknowns (not tested on hardware)

Display orientation and offsets; touch mirroring; the 14 px safe inset (a guess); draw buffers (they live in PSRAM, where DESIGN section 9 wanted internal RAM); frame rate; free memory; idle current and heat; the IRAM trade-offs made to fit memory (speed unmeasured); boot from empty OTA data; the whole network path; the NVS config store on real flash; USB-Serial/JTAG sharing the console between logs, Improv and `RU+CFG`.
