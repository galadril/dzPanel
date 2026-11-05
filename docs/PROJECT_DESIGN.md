# RoundOS --- Open-Source Application Platform for Round AMOLED Displays

**Status:** Draft / pre-hardware\
**Project owner:** Mark Heinis (`@galadril`)\
**Initial targets:** Waveshare ESP32-S3-Touch-AMOLED-1.75 and
ESP32-S3-Touch-AMOLED-2.16\
**Primary goal:** Build a reusable, open-source firmware and application
framework for round ESP32-S3 AMOLED devices, with smart-home and
Domoticz applications as an initial use case.

> Working name: **RoundOS**. Treat this as provisional until the
> repository and naming are finalised.

------------------------------------------------------------------------

## 1. Vision

RoundOS aims to make round-screen ESP32-S3 devices feel like small,
extensible products rather than single-purpose demos. It should provide
a consistent user experience, reusable UI components, and a simple way
to add applications.

The same core software should support multiple display sizes and
compatible boards. Applications should adapt to each display instead of
hard-coding coordinates for one device.

The first compelling demonstration will be a round smart-home companion
showing live home status, energy data, notifications, and touch-driven
controls through Domoticz.

### Design principles

-   **Round-screen first:** layouts, typography, and navigation must
    work well on circular displays.
-   **One platform, multiple devices:** separate shared application
    logic from board-specific drivers and display profiles.
-   **Incremental delivery:** establish a reliable boot, display, touch,
    and app lifecycle before adding complex features.
-   **Offline-friendly:** the interface should remain useful when Wi-Fi
    or a server is unavailable.
-   **Open and documented:** publish source, setup instructions,
    hardware notes, and reproducible builds.
-   **Safe by default:** display data and controls explicitly; avoid
    accidental smart-home actions.
-   **No premature rewrite:** evaluate existing Waveshare examples and
    supported libraries before choosing drivers or replacing working
    components.

------------------------------------------------------------------------

## 2. Initial hardware targets

The intended targets are:

1.  **Waveshare ESP32-S3-Touch-AMOLED-1.75** --- 1.75-inch, 466 × 466
    round AMOLED.
2.  **Waveshare ESP32-S3-Touch-AMOLED-2.16** --- 2.16-inch, 480 × 480
    round AMOLED.

The exact board revisions, display controller, touch controller,
peripheral wiring, and available memory must be verified against the
hardware actually received and the official Waveshare documentation.

### Hardware abstraction requirements

Create a board profile for each supported model. Each profile should
describe:

-   Display dimensions and driver configuration.
-   Touch controller and orientation.
-   Display rotation and safe-area assumptions.
-   Available peripherals and their initialisation.
-   Optional battery, microSD, audio, RTC, and IMU capabilities.
-   Board-specific pins and build configuration.

Do not assume every feature is present or wired identically across
variants. Unsupported capabilities must be reported as unavailable
rather than causing startup failure.

------------------------------------------------------------------------

## 3. Scope and non-goals

### MVP scope

-   Boot into a home screen.
-   Show time, date, Wi-Fi status, and device status.
-   Navigate between simple apps using touch.
-   Provide a common rendering and app lifecycle interface.
-   Support configurable display profiles for both target resolutions.
-   Connect to Wi-Fi and expose clear connection/error states.
-   Include a Domoticz dashboard proof of concept.
-   Build and run using documented, repeatable developer instructions.

### Later scope

-   Notification centre and event history.
-   Energy dashboard with solar, consumption, battery, and
    electricity-price cards.
-   User-configurable widgets and favourites.
-   Optional audio, haptics, RTC, IMU, and microSD integrations.
-   App settings and persistent configuration.
-   Over-the-air updates, after recovery and rollback behaviour are
    designed.
-   A documented method for third-party developers to add apps.

### Explicit non-goals for the first milestone

-   Replacing the ESP-IDF or other underlying operating system.
-   Supporting every Waveshare board from day one.
-   Promising a general-purpose app store or sandbox before resource
    limits are understood.
-   Implementing every peripheral before the display and touch pipeline
    is stable.
-   Making the device dependent on cloud services.

**Terminology:** Initially, this is an application platform/firmware
project built on supported ESP32 software, not necessarily a new
operating system.

------------------------------------------------------------------------

## 4. Proposed architecture

Keep the architecture modular, but avoid unnecessary abstraction until
the first board is working.

``` text
┌──────────────────────────────────────────────┐
│                 Applications                 │
│ Home │ Clock │ Domoticz │ Energy │ Settings  │
├──────────────────────────────────────────────┤
│              App Runtime / Shell             │
│ Lifecycle │ Navigation │ Focus │ Notifications│
├──────────────────────────────────────────────┤
│                 UI Toolkit                   │
│ Round layouts │ Cards │ Text │ Icons │ Gauges│
├──────────────────────────────────────────────┤
│               Platform Services              │
│ Config │ Wi-Fi │ Time │ Storage │ Logging    │
│ HTTP/API client │ Update service (later)     │
├──────────────────────────────────────────────┤
│               Hardware Layer                │
│ Display │ Touch │ Board profile │ Peripherals│
├──────────────────────────────────────────────┤
│       ESP-IDF / supported vendor libraries   │
└──────────────────────────────────────────────┘
```

### Suggested modules

  -----------------------------------------------------------------------
  Module                              Responsibility
  ----------------------------------- -----------------------------------
  `boards/`                           Board profiles, pins, capabilities,
                                      display/touch configuration

  `platform/`                         Startup, logging, configuration,
                                      network and storage services

  `ui/`                               Shared visual components and
                                      circular-layout helpers

  `runtime/`                          App registration, lifecycle,
                                      navigation and notifications

  `apps/home/`                        Home screen and launcher

  `apps/clock/`                       Clock and date

  `apps/domoticz/`                    Domoticz connection and dashboard

  `apps/energy/`                      Energy cards, graphs and price
                                      display

  `docs/`                             Setup, architecture, hardware notes
                                      and contributor guide

  `tests/`                            Host-side tests for logic that does
                                      not require hardware
  -----------------------------------------------------------------------

Names are proposals; adapt them to the chosen repository structure.

------------------------------------------------------------------------

## 5. Technology choices

### Evaluate before committing

Use the official Waveshare examples as the baseline. Verify which SDK
and graphics stack they support for both boards before selecting the
long-term foundation.

Candidate considerations:

-   **ESP-IDF:** strong control over ESP32-S3 capabilities and
    production firmware; may require more setup.
-   **Arduino framework on ESP32:** potentially quicker for prototyping
    and broad library availability.
-   **LVGL:** worth evaluating for touch UI, widgets, and display
    abstraction, provided the board's display pipeline is supported and
    performance is adequate.
-   **Waveshare-provided display/touch libraries:** useful for proving
    hardware access; isolate them behind the hardware layer to reduce
    coupling.

Do not mix frameworks casually. Select one supported baseline after
compiling an official example for each target. Record the decision in an
Architecture Decision Record (ADR).

### Initial ADRs to write

-   `ADR-001`: Framework and build system.
-   `ADR-002`: Graphics/UI library and rendering strategy.
-   `ADR-003`: Application registration and lifecycle.
-   `ADR-004`: Configuration and secrets storage.
-   `ADR-005`: Domoticz API integration and authentication.
-   `ADR-006`: Firmware update and recovery strategy.

------------------------------------------------------------------------

## 6. Round-screen UI guidelines

Circular displays need deliberate layout rules. A square interface
clipped to a circle will waste space and make controls difficult to
reach.

-   Keep critical text and controls inside a configurable circular safe
    area.
-   Put primary actions near the centre or in predictable touch zones.
-   Avoid placing small text close to the extreme left/right edges.
-   Prefer glanceable cards, radial indicators, compact lists, and short
    labels.
-   Use a consistent back/home gesture or on-screen navigation
    affordance.
-   Support both 466 × 466 and 480 × 480 without per-app coordinate
    duplication.
-   Make touch targets large enough for a small touchscreen.
-   Provide readable contrast and avoid conveying state through colour
    alone.
-   Include empty, loading, disconnected, stale-data, and error states
    in every data-driven app.

### UI component candidates

-   Circular safe-area and layout helpers.
-   Status bar or status ring.
-   Card and value tile.
-   Icon + label.
-   Gauge and progress ring.
-   Mini line chart.
-   Notification banner.
-   Confirmation dialog for actions.
-   Loading and connection-state indicator.

------------------------------------------------------------------------

## 7. App model

Each app should expose a small, explicit lifecycle rather than owning
the whole device.

Proposed lifecycle:

``` text
register → initialise → enter → update/event handling → leave → release
```

An app should be able to:

-   Declare its name, icon, and required capabilities.
-   Create and update its UI.
-   Receive touch/navigation events.
-   Request platform services through documented interfaces.
-   Persist only its own settings.
-   Handle missing network data and unavailable peripherals gracefully.

For the MVP, apps can be compiled into the firmware and registered at
build time. Dynamic loading or third-party binary apps should be
considered only after memory, security, and update constraints are
understood.

------------------------------------------------------------------------

## 8. First-party apps

### 8.1 Home / Launcher

-   Clock and date.
-   Wi-Fi and device status.
-   Shortcuts to installed apps.
-   A clear indication when data is stale or a service is disconnected.

### 8.2 Domoticz

Initial read-only functionality:

-   Connection status.
-   Selected temperature and environmental sensors.
-   Selected switch/device states.
-   Last successful refresh and stale-data indication.

Follow-up functionality:

-   Switch controls with confirmation for consequential actions.
-   Configurable favourites.
-   Notifications for selected Domoticz events.

Use a configurable Domoticz base URL and authentication mechanism. Do
not commit credentials, tokens, private hostnames, or personal
configuration to source control. Prefer local-network access initially;
document the risks before supporting remote access.

### 8.3 Energy

-   Current household power.
-   Solar production.
-   Home-battery state of charge and power, where available.
-   Current electricity price, where available.
-   A simple trend or recent history.

Energy data should be sourced through Domoticz initially rather than
building multiple integrations at once. Make units explicit and
distinguish unavailable data from a genuine zero.

### 8.4 Notifications

-   Short, glanceable messages.
-   Priority and timestamp.
-   Dismiss action.
-   Optional notification history when storage is available.

------------------------------------------------------------------------

## 9. Domoticz integration approach

Keep Domoticz-specific logic separate from rendering.

1.  Configure the Domoticz endpoint and credentials locally.
2.  Implement a small client for the required JSON/API calls.
3.  Map responses into typed internal models.
4.  Make the UI consume those models, not raw API responses.
5.  Add timeouts, retry/backoff, and stale-data handling.
6.  Poll conservatively; determine whether event-driven updates are
    practical later.
7.  Test API parsing on a development machine with saved, sanitised
    fixtures before hardware is available.

The MVP should support a small configurable set of devices. Avoid
hard-coding one household's device indexes.

------------------------------------------------------------------------

## 10. Development plan while waiting for hardware

### Phase 0 --- Repository and design

-   [ ] Create repository and README.
-   [ ] Add this design document.
-   [ ] Add `CONTRIBUTING.md`, licence, issue templates, and a basic
    roadmap.
-   [ ] Review official Waveshare examples for both target boards.
-   [ ] Record supported framework versions and links to official docs.
-   [ ] Create initial ADRs for the framework and graphics stack.

### Phase 1 --- Host-side UI prototype

-   [ ] Create mock screens at 466 × 466 and 480 × 480.
-   [ ] Define a circular safe-area helper.
-   [ ] Prototype the launcher, clock, Domoticz cards, and energy view.
-   [ ] Use fake data so UI work does not depend on hardware or a live
    home.
-   [ ] Check text clipping and layout scaling at both resolutions.

A desktop mock-up is a design aid, not proof that the real display
driver or touch controller works.

### Phase 2 --- Build environment

-   [ ] Install the chosen SDK and toolchain.
-   [ ] Build an unmodified official example for each board target where
    possible.
-   [ ] Capture compiler versions and setup commands.
-   [ ] Create a minimal project skeleton and automated build checks.
-   [ ] Keep board-specific code behind a small interface.

### Phase 3 --- First hardware bring-up

Once the boards arrive:

-   [ ] Confirm exact model/revision and power requirements.
-   [ ] Bring up display and verify orientation/resolution.
-   [ ] Test touch coordinates and rotation.
-   [ ] Measure available heap/PSRAM and monitor task stack usage.
-   [ ] Test Wi-Fi reconnect and basic time synchronisation.
-   [ ] Verify optional peripherals individually.
-   [ ] Save a known-good recovery/build procedure.

### Phase 4 --- MVP

-   [ ] Launcher and clock.
-   [ ] Basic app lifecycle and navigation.
-   [ ] Wi-Fi/configuration UI.
-   [ ] Domoticz read-only dashboard.
-   [ ] One energy view.
-   [ ] Documentation and a short demo video/GIF.
-   [ ] Test on both screen sizes and document any hardware-specific
    differences.

### Phase 5 --- Community release

-   [ ] Publish a tagged release and build instructions.
-   [ ] Include screenshots/mock-ups and actual hardware photos.
-   [ ] Document known limitations and supported boards.
-   [ ] Share the project with Waveshare and the Domoticz community.
-   [ ] Invite issues and contributions with a clear scope.

------------------------------------------------------------------------

## 11. Testing strategy

### Before hardware arrives

-   Test JSON parsing and model conversion with sanitised fixtures.
-   Unit-test layout calculations and circular safe-area logic.
-   Use mock services for time, network state, and Domoticz responses.
-   Verify both screen profiles in a desktop simulator or
    image-rendering harness where supported.
-   Add formatting, linting, and build checks to CI.

### With hardware

-   Verify rendering, touch mapping, rotation, and screen edges.
-   Test reconnects, API timeouts, invalid credentials, and stale data.
-   Check memory use during screen transitions and graph updates.
-   Test prolonged operation and repeated navigation.
-   Confirm that controls cannot be triggered by accidental touch or
    stale UI state.

------------------------------------------------------------------------

## 12. Security and privacy

-   Keep Wi-Fi credentials, API passwords, tokens, and personal
    endpoints out of Git.
-   Provide a local configuration mechanism and a safe example
    configuration.
-   Avoid logging secrets or complete authenticated URLs.
-   Use timeouts and validate API responses.
-   Treat remote access as a separate design decision; do not expose the
    device or Domoticz API directly to the internet by default.
-   Require explicit confirmation for important smart-home actions.
-   Document what data stays on the device and what is sent over the
    network.

------------------------------------------------------------------------

## 13. Repository and contribution conventions

Suggested repository layout:

``` text
roundos/
├── README.md
├── LICENSE
├── CONTRIBUTING.md
├── docs/
│   ├── PROJECT_DESIGN.md
│   ├── hardware/
│   ├── decisions/
│   └── development-setup.md
├── boards/
├── components/
├── platform/
├── ui/
├── runtime/
├── apps/
│   ├── home/
│   ├── clock/
│   ├── domoticz/
│   └── energy/
├── tests/
└── tools/
```

Use the repository's chosen licence consistently, including for bundled
dependencies. Check the licences of Waveshare examples and third-party
libraries before copying or redistributing code.

------------------------------------------------------------------------

## 14. Definition of done for the first milestone

The MVP is ready for an initial public release when:

-   It boots reliably on at least one supported board.
-   The launcher, clock, and one data-driven app work.
-   Touch navigation is usable and predictable.
-   The same application code can target both screen profiles, with
    hardware differences isolated in board configuration.
-   Domoticz connection failures and stale data are clearly represented.
-   Secrets are configured locally and are not committed.
-   A new developer can follow the documentation to build the firmware.
-   Known limitations and supported hardware revisions are documented.

------------------------------------------------------------------------

## 15. Open questions to resolve

-   Which framework and graphics stack provide the best supported path
    for both boards?
-   Are the display and touch drivers shared between the target models?
-   What are the real memory and frame-rate constraints?
-   Which peripherals differ between board revisions?
-   Should the initial Domoticz client use polling, and at what
    interval?
-   What is the simplest maintainable app registration model?
-   How should settings be backed up and reset?
-   What update/recovery mechanism is safe enough for early releases?
-   Which licence best fits the project's goals and dependencies?

------------------------------------------------------------------------

## 16. Immediate next actions

1.  Create the repository and add this document as
    `docs/PROJECT_DESIGN.md`.
2.  Read the official documentation and clone the official example
    repositories for both target models.
3.  Choose a framework only after successfully building the vendor
    examples.
4.  Sketch three screens: launcher, Domoticz overview, and energy
    dashboard.
5.  Build a mock-data UI prototype at both resolutions.
6.  Keep the first milestone deliberately small: **boot → render → touch
    → launcher → one Domoticz screen**.

**Success criterion:** a polished, reproducible open-source foundation
for round-screen ESP32-S3 devices that can grow into a useful app
platform without locking every app to one particular display.
