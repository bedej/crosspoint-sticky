# Sticky voice mode (AgentVoiceActivity)

Push-to-talk voice for the reTerminal Sticky, talking to the homelab `voice` service.

```
AI-Voice button ─▶ PDM mic (freeink::Microphone, 16 kHz) ─▶ WebSocket (binary frames)
      ▲                                                          │
   e-paper  ◀──── answer text (JSON answer.delta, wrapped) ◀──── voice service
```

> **Status: compile-verified + on the home menu; not yet flashed/tested on hardware.**
> Builds clean with `pio run -e sticky` (see BUILD.md for the required PlatformIO Core
> pin). Launches from the **"Voice Assistant"** home-menu item. No secret is baked into
> the firmware — the endpoint token is read from the SD card at runtime.

## Configure the device (`/.crosspoint/voice.json`)

Create this file on the SD card (same `/.crosspoint/` dir CrossPoint uses for its config):

```json
{ "token": "<your VOICE_TOKEN>", "host": "192.168.1.85", "port": 18092 }
```

- `token` (**required**) — the voice service's shared secret. Get it from blackbox:
  `grep ^VOICE_TOKEN= /opt/apps/voice/.env`.
- `host`/`port` — default to `192.168.1.85:18092` if omitted.

Without a valid `token` the activity shows an error instead of connecting.

## Provision Wi-Fi (once, on the device)
Boot the Sticky → **File Transfer** (or Settings ▸ Wi-Fi Networks) → add your SSID +
password. It persists to `/.crosspoint/wifi.json`; the activity auto-connects from it.

## Flashing

**Option A — CrossPoint web flasher (WebSerial, easiest).** Plug the Sticky in via USB
(WCH bridge), open the flasher in a Chromium browser, and upload the **merged factory
image** `firmware.factory.bin` (flashed at `0x0`). Grab it from the fork's Releases, or
build it (below) — it's at `.pio/build/sticky/firmware.factory.bin`.

**Option B — USB via PlatformIO.** `pio run -e sticky -t upload` (see BUILD.md for the
Core pin). Flashes the app to `app0`.

**Option C — OTA (no USB), keeps the A/B slots.** Put the **app-only** `firmware.bin` on
the SD card and use CrossPoint's SD firmware-update (`SdFirmwareUpdateActivity`) — it
writes the *inactive* app slot and reboots, with rollback if the image fails. Or point
CrossPoint's `OtaUpdater` at a URL hosting `firmware.bin`.

## Use it
Home menu → **Voice Assistant** → press **Up** to start, speak, press **Up** again to
stop → the transcript then the streamed answer appear on the e-paper. **Down** exits.

> The Sticky's **AI-Voice button is CrossPoint's Power button**, which the main loop
> consumes for deep sleep before an activity can see it (default `shortPwrBtn=IGNORE`,
> `main.cpp`). So push-to-talk is the **Up** side button, not AI-Voice.

## Building
See **BUILD.md** — the key point is you must pin the pioarduino PlatformIO Core `v6.1.19`
(mainline PlatformIO 6.2.0 pulls a broken SCons). Then `pio run -e sticky` produces both
`firmware.bin` (app) and `firmware.factory.bin` (merged).

## Risks to check on hardware (in order)
1. **Mic bring-up** — confirm `mic_.begin(16000)` returns true and `read()` yields
   non-silent samples (there may be a mic power-enable in `BoardConfig::ACTIVE.mic`).
2. **`WifiCredentialStore` / SD access under the render task** — `WifiSelectionActivity`
   holds a `RenderLock` around `loadFromFile()` (SD + e-paper share SPI). Add that if you
   see SPI races.
3. **Heap** — WiFi + WS + the growing `answer_` string; watch `ESP.getFreeHeap()` (keep
   > 50 KB per CLAUDE.md). Cap `answer_` length if needed. PSRAM (8 MB) is available for
   spillover.
4. **e-paper cadence** — the 400 ms repaint coalescing vs ghosting; tune the interval /
   refresh mode.
