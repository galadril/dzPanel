# Privacy and data handling

RoundOS has no cloud service, no account and no telemetry. This page says what stays where. Where something is still a plan, it says so.

## On the device

| Data | Where | Notes |
|---|---|---|
| Wi-Fi name and password | NVS, `secrets` namespace | Set through the installer page (USB `RU+CFG`, password travels over the cable only, never echoed or logged), the install dialog (Improv) or the setup portal |
| Domoticz address and access token | NVS, `secrets` namespace | Set from the installer over USB. The token is never shown (the UI shows a mask), never logged, and wiped from RAM copies after use |
| Setup portal session (AP password, form token) | RAM only | Random per session, shown on the device screen only, wiped when the portal stops. Never written to flash or logged |
| Cached device values | RAM only | Not written to flash |

**Physical access means secret access** until flash encryption is enabled (not enabled yet). Anyone who can hold the device and read its flash can read the token. Use a **Viewer** token if the display should only show things, and delete the token in Domoticz to cut a lost device off at once.

## What crosses the network

- Device to **your Domoticz server only**, using the address you gave it, with your token as a bearer header. Nothing else is contacted at runtime. (Over-the-air updates are planned and will be the one exception; they are not built.)
- Plain `http://` sends the token unencrypted over your network. The device and the installer make you opt in explicitly (`insecure_lan`) and the design has the device show a warning icon while it is on (the screen for it is not built yet). `https://` is verified against the CA bundle with no skip-verify option.
- **Setup portal** (only when you start it, for about 10 minutes at most): the device runs its own Wi-Fi network with a random password and a small web page over plain http. Your phone sends the Wi-Fi password and the Domoticz token to the device over that private network; nothing goes to the internet. It is off at all other times (no always-on web server).
- No telemetry, no crash upload, no update check by default.

## The installer page

- A static page. It runs in your browser and talks to your device over USB (Web Serial).
- The Domoticz address and token you type go **straight to the device over the USB cable**. They are not sent to any server, not stored by the page, and the token field is cleared after sending.
- It fetches firmware and version files from its own origin only (`versions.json` can only name `firmware/<board>/<channel>/<version>/<file>` paths, checked by the page and by the release tooling, so it cannot point it at another host). It loads no analytics, fonts or scripts from a CDN.
- Token rights are read locally in the browser to warn about Admin tokens. The signature is not checked there.

## In this repository

- Wi-Fi credentials, tokens and private hostnames never enter the repository (`secrets*`, `*.local.*` are git-ignored; `config.example.json` ships instead).
- Test fixtures from a real server are scrubbed of names, hostnames, URLs and coordinates before they are committed.
