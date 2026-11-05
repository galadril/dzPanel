# RoundOS (working name)

A glanceable Domoticz companion for round AMOLED screens (Waveshare ESP32-S3-Touch-AMOLED 1.75" and 2.16"). It shows live device values, scenes, groups and energy, and offers safe touch controls. It works with any Domoticz setup: screens are chosen from what a device can do, never from its name.

> **Status: pre-hardware.** Nothing has run on a real board yet. The host code, the simulator and the installer page are tested; the firmware compiles (see below) but is unproven on a device. The name and the licence are not decided.

## What exists

| Part | State |
|---|---|
| Domoticz parser, client, device model, command layer | Built and tested on the host (including against a real Domoticz 2026.4 server, read-only, plus one switch test) |
| Scene and Group handling | Scene = push only. Group = On / Off / **Mixed**. Both confirmed by re-reading the device, not by trusting the reply |
| Round UI toolkit, simulator (LVGL 9.4.0 + SDL2) | Built; screenshots at 466 and 480 px |
| Event bus, app runtime, gesture recogniser, navigation | Built and tested on the host |
| Config store, Wi-Fi manager, Improv Serial, USB setup protocol | Host-tested; ESP glue compile-checked only |
| Web installer page and release tooling | Page logic and manifest tool tested; **never loaded in a browser** |
| ESP-IDF firmware project | In progress; see `docs/progress/` |
| Anything on real hardware | **Not done** |

## Try it without hardware

```bash
tools/fetch_vendor.sh                              # pinned third-party sources (LVGL for the simulator)
cmake -S . -B /tmp/vb -DRU_BUILD_SIM=OFF && cmake --build /tmp/vb -j8 && (cd /tmp/vb && ctest)
node sim/mock/domoticz_mock.js --port 18080        # a fake Domoticz to point the code at
```

Firmware (needs ESP-IDF 5.5, see the setup doc):

```bash
tools/build_fw.sh amoled-175           # Waveshare 1.75" AMOLED, build dir firmware/build-amoled-175
tools/build_fw.sh guition-4848s040     # Guition 4" 480x480 square, build dir firmware/build-guition-4848s040
```

Full instructions, including the ESP-IDF setup: [`docs/development-setup.md`](docs/development-setup.md).

## Documents

| | |
|---|---|
| [`DESIGN.md`](DESIGN.md), [`BUILD_PLAN.md`](BUILD_PLAN.md) | What and why; in what order |
| [`docs/development-setup.md`](docs/development-setup.md) | Build, test, toolchain, secrets |
| [`docs/flashing.md`](docs/flashing.md), [`docs/provisioning.md`](docs/provisioning.md), [`docs/bringup.md`](docs/bringup.md) | Installing and configuring a device; the first day on a real board |
| [`docs/releasing.md`](docs/releasing.md) | Cutting a release, GitHub setup, pulling a bad build |
| [`docs/privacy.md`](docs/privacy.md) | What stays on the device and what crosses the network |
| [`docs/research/domoticz-api.md`](docs/research/domoticz-api.md) | Domoticz API notes, verified against a live server |
| [`docs/AGENT_CONTRACT.md`](docs/AGENT_CONTRACT.md) | Rules used when several agents build in parallel |

## Using your own Domoticz

Create an **access token** in Domoticz (Setup, More Options, Access Tokens). Choose **Viewer** for a read-only display or **User** if it may switch things. Avoid Admin. See `docs/provisioning.md`.

## Not decided

- **Licence** (ADR-007 is open). Third-party code in use: LVGL (MIT), Waveshare examples and BSP (Apache-2.0). The Domoticz source (GPL-3.0) is read for reference only and no code is copied from it.
- **Name.** "RoundOS" implies a platform; the scope is a Domoticz remote. See `docs/research/domoticz-api.md` section 6.
