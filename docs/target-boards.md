# Target boards

Candidate boards for roundOS. **All specs below are unverified** (compiled from memory, not checked against vendor pages). Verify the display IC, touch IC, pinout and PSRAM before starting a port.

Each board needs its own profile: `components/boards/<board>/ru_board/` and `firmware/boards/<board>/sdkconfig.defaults`.

## Supported

| Board | Display | Touch | Status |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.75 (`amoled-175`) | CO5300 AMOLED | CST9217 | Working |
| Guition ESP32-S3-4848S040 (`guition-4848s040`) | ST7701 RGB 480x480 | GT911 | Working (nightly only until bring-up is confirmed) |

## Planned

| Board | Display | Touch | Status |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-2.16 (`amoled-216`) | AMOLED (unverified) | unverified | No board profile yet |

## Candidates: close to the Guition port (ST7701 RGB 480x480)

| Board | Shape / size | Likely display | Likely touch | Notes | Dev board requested |
|---|---|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-4 | 4" square 480x480 | ST7701 | unverified | Same vendor as the AMOLED boards | No |
| Waveshare ESP32-S3-Touch-LCD-2.1 | 2.1" round 480x480 | ST7701 | unverified | IO expander for panel init | No |
| Seeed SenseCAP Indicator D1 | 4" square 480x480 | ST7701 | unverified | IO expander for panel init and touch reset | No |
| LilyGO T-RGB | 2.1" round 480x480 | ST7701 | unverified | | No |
| Elecrow CrowPanel 2.1" round | 2.1" round 480x480 | ST7701 | unverified | | No |
| Sunton ESP32-4848S040 / Guition clones | 4" square 480x480 | ST7701 | GT911 | Probably near-identical to `guition-4848s040` | No |
| diymore ESP32-S3 480x480 TFT | 480x480 | unverified | unverified | Get the vendor demo code for the driver IC and pins | No |

## Candidates: round or small displays (different driver)

| Board | Shape / size | Notes | Dev board requested |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.43 / 1.8 / 2.06 | AMOLED | Same family as `amoled-175`, driver port should be easy | No |
| Waveshare ESP32-S3-Touch-LCD-1.28 / 1.46 | Round LCD | Small panel, needs layout work | No |
| M5Stack Dial | 1.28" round | Rotary encoder input | No |
| M5Stack CoreS3 | 2" square | | No |
| LilyGO T-Display-S3 AMOLED | AMOLED | | No |
| LilyGO T-Watch S3 | Square watch | | No |
| Seeed XIAO ESP32-S3 + Round Display | 1.28" round | | No |

## Out of scope for now

| Board | Reason |
|---|---|
| Guition JC4827W543 (480x272), JC8048W550 (800x480), Sunton ESP32-8048S043 | Different resolution and aspect ratio |
| ESP32-S3-BOX-3 (320x240) | Different resolution and aspect ratio |
| Guition JC1060P470, M5Stack Tab5 | ESP32-P4, different target |

## Outreach order

1. Waveshare (already used for the AMOLED boards, covers several targets).
2. Elecrow, Seeed, LilyGO, M5Stack.
3. Guition and diymore (mostly marketplace sellers).

## Checklist per board

- [ ] Confirm display IC, interface and resolution
- [ ] Confirm touch IC and I2C pins
- [ ] Confirm pinout against the schematic
- [ ] Confirm flash size and PSRAM mode (octal or quad)
- [ ] Add the board profile and `sdkconfig.defaults`
- [ ] Add the board to `ci.yml` and `release.yml`
