# Sticky voice mode (AgentVoiceActivity)

Push-to-talk voice for the reTerminal Sticky, talking to the homelab `voice` service.

```
AI-Voice button ─▶ PDM mic (freeink::Microphone, 16 kHz) ─▶ WebSocket (binary frames)
      ▲                                                          │
   e-paper  ◀──── answer text (JSON answer.delta, wrapped) ◀──── voice service
```

The backend (ASR + Hermes + streaming) is live and tested at
`ws://192.168.1.85:18092/v1/stream?token=<VOICE_TOKEN>` (LAN-only, token-authed).

> **Status: COMPILE-VERIFIED + wired into the home menu; not yet flashed.**
> `AgentVoiceActivity.{h,cpp}` builds clean with `pio run -e sticky` (see BUILD.md for the
> required Core pin) and is launched from the **"Voice Assistant"** home-menu item. The
> only code fix needed was one Wi-Fi type (`std::string ssid`). **Remaining:** set the
> endpoint token + provision Wi-Fi, then `-t upload` and test on the device.

## What's already handled
- **WebSocket client**: `links2004/WebSockets @ 2.7.3` is already in `lib_deps`
  (`WebSocketsClient`), no new dependency.
- **Mic**: the SDK ships `freeink::Microphone` (`begin`/`read`/`end`, PDM RX via the
  IDF `i2s_pdm` driver) and `FREEINK_CAP_MIC` is **on for the Sticky**. Added
  `Microphone=symlink://…/Microphone` to `lib_deps`.
- **Pin conflict resolved**: the Sticky's serial is a **WCH bridge on UART0, not
  native USB-CDC** (`freeink-sdk/.../BoardConfig.h:348`), so the mic's GPIO19/20 do
  **not** clash with USB. The mic pins + power rail come from `BoardConfig::ACTIVE.mic`
  — you never hardcode them.
- **Button**: the AI-Voice button is `MappedInputManager::Button::Power`. A long hold
  still sleeps the device globally (main.cpp), so short-press = talk, long-hold = off.

## Before you build — set these
1. **Endpoint token** — in `AgentVoiceActivity.cpp`, `VOICE_TOKEN` is a placeholder.
   Get the real value from blackbox: `grep ^VOICE_TOKEN= /opt/apps/voice/.env`.
   **Don't commit the real token to this (public) fork** — set it locally, or (better,
   follow-up) load it from an SD config file like `wifi.json`.
2. **Host/port** — `VOICE_HOST=192.168.1.85`, `VOICE_PORT=18092` (change if blackbox moves).

## Wire it into a menu (the one edit left — do it by hand, it touches core files)
Launch via **Settings ▸ System** (least invasive; refs are file:line in current tree):
1. `src/CrossPointSettings.h` — add a `SettingAction` enum value, e.g. `VoiceAssistant`.
2. `src/activities/settings/SettingsActivity.cpp`
   - `#include "activities/network/AgentVoiceActivity.h"`
   - in `rebuildSettingsLists()` (~lines 86-95) push the action:
     `systemSettings.push_back(SettingInfo::Action(StrId::STR_VOICE_ASSISTANT, SettingAction::VoiceAssistant));`
   - in the ACTION `switch` (~lines 324-355) add:
     `case SettingAction::VoiceAssistant: startActivityForResult(std::make_unique<AgentVoiceActivity>(renderer, mappedInput), resultHandler); break;`
3. `lib/I18n/translations/english.yaml` — add `STR_VOICE_ASSISTANT: "Voice assistant"`,
   then `python3 scripts/gen_i18n.py` (i18n is generated; all UI strings use `tr(...)`).

## Provision Wi-Fi (once, on the device)
Boot the Sticky → **File Transfer** (or Settings ▸ Wi-Fi Networks) → add your SSID +
password. It persists to `/.crosspoint/wifi.json` (obfuscated with the device MAC), and
the activity auto-connects from the saved credential.

## Build & flash
```bash
pio run -e sticky                 # build
pio run -e sticky -t upload       # flash over the WCH/UART0 USB
pio run -e sticky -t upload -t monitor
```
First build downloads `WebSockets` and rebuilds the Arduino core once (slower, cached after).

## Try it
Open the Voice item → **press AI-Voice** to start, speak, **press again** to stop → the
transcript then the streamed answer appear on the e-paper. **Back** exits.

## Risks to check on hardware (in order)
1. **The `// VERIFY` lines in `render()`** — `renderer.drawText/getLineHeight/displayBuffer`,
   `UITheme::drawCenteredWrappedText`, `UI_10_FONT_ID`, `Rect`, `HalDisplay::FAST_REFRESH`.
   These are the reader's own APIs; confirm exact signatures/includes.
2. **`WifiCredentialStore` SD access** — `WifiSelectionActivity` holds a `RenderLock`
   around `loadFromFile()` (SD + e-paper share SPI). Add that if you see SPI races.
3. **Mic bring-up** — confirm `mic_.begin(16000)` returns true and `read()` yields
   non-silent samples (there may be a mic power-enable in `BoardConfig::ACTIVE.mic`).
4. **Heap** — WiFi + WS + the growing `answer_` string; watch `ESP.getFreeHeap()` (keep
   > 50 KB per CLAUDE.md). Cap `answer_` length if needed.
5. **e-paper cadence** — the 400 ms repaint coalescing (loop) vs ghosting; tune the
   interval / refresh mode.
