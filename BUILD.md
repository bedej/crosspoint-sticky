# Building the Sticky firmware (`[env:sticky]`)

You do **not** need the hardware to *compile* (only to flash). CrossPoint builds with
the **pioarduino fork of PlatformIO Core, pinned to `v6.1.19`** — mainline
`pip install platformio` is currently broken (see the blocker below), so use the pin.

## Recipe (no hardware; verified on devser)

```bash
# PlatformIO in an isolated venv, pinned to the pioarduino Core v6.1.19 (what CrossPoint CI uses)
python3 -m venv ~/pioenv                    # needs python3-venv/ensurepip installed
~/pioenv/bin/pip install -U "https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip"

# build (the tree must include the freeink-sdk submodule)
~/pioenv/bin/pio run -d <path-to>/sticky-fw -e sticky
# flash (needs the device): ~/pioenv/bin/pio run -d ... -e sticky -t upload
```

First run downloads the pioarduino platform (`55.03.311`) + ESP-IDF toolchain and
rebuilds the Arduino core once (slow, ~15-20 min; cached in `~/.platformio` after).
The build is long — run it detached (`setsid`/`nohup`) if your shell has a runtime cap.

## ⚠ The blocker mainline PlatformIO hits (why the pin matters)

`pip install platformio` (unpinned) fails in ~15 s, **before compiling any source**, with:

```
*** [.pio/build/sticky/firmware.elf] ModuleNotFoundError : No module named 'SCons.Tool.FortranCommon'
  File ".../packages/tool-scons/scons-local-4.11.1/SCons/Tool/linkCommon/__init__.py", line 132, in smart_link
```

**Root cause (dependency drift, not the firmware).** PlatformIO Core **6.2.0** shipped to
PyPI **2026-09-05**; it resolves `platformio/tool-scons@4.41101.0` = **SCons 4.11.1**,
which removed the `SCons.Tool.FortranCommon` module (refactored into
`SCons.Tool.linkCommon` in SCons 4.9). The pioarduino platform pins the *good*
`tool-scons@4.40801.0` = SCons 4.8.1 (still has `FortranCommon`), but Core installs
**both** and the newer one wins → the S3/ESP-IDF builder imports a module that no longer
exists and dies at the `firmware.elf` link step. Clean envs (CI, fresh installs,
throwaway containers) hit it; a machine that already cached the good SCons does not.

**tool-scons → SCons map:** `4.40801.0`=4.8.1 ✅ · `4.41101.0`=4.11.1 ❌ · `<4.409xx`=safe.

**The fix — any of (confirmed against CrossPoint CI + multiple PRs):**
1. **Pin Core to pioarduino v6.1.19** (recipe above) — most robust; matches CrossPoint.
2. **Mainline PyPI:** `pip install "platformio==6.1.19"` (resolves the good tool-scons).
3. **Pin in `platformio.ini`** (works with any Core) — add to `[env:sticky]` (or a base):
   ```ini
   platform_packages = platformio/tool-scons@4.40801.0
   ```
4. If a machine already pulled the bad one, purge it so the pin takes effect:
   `rm -rf ~/.platformio/packages/tool-scons@4.41101.0` (or `rm -rf ~/.platformio`).

**Dead ends (don't repeat):** patching/pinning the *pip/venv* SCons does nothing — the
build uses PlatformIO's **vendored** `tool-scons`; and patching that vendored copy gets
re-materialized away on the next `pio run`. Fix the *version*, per above.

## Recurring build failures on macOS, and what each one actually is

Every one of these cost real time at least once. They share a shape: the message
names something unrelated to the actual cause.

### `uv` cannot reach PyPI — "Socket is not connected (os error 57)"

```
× Failed to build `esptool @ file:///…/packages/tool-esptoolpy`
├─▶ Failed to resolve requirements from `build-system.requires`
╰─▶ Socket is not connected (os error 57)
```

pioarduino installs its Python dependencies with `uv`. On this network `uv` (and
`curl`) get their TLS handshake reset by `files.pythonhosted.org`, while `pip`
succeeds against the same host — so it is not connectivity, it is whatever the
edge dislikes about those clients' handshakes. `pypi.org` itself answers fine,
which makes it look like a partial outage.

Fix, per machine (pip works, so pre-install what uv would fetch):

```bash
P=~/.platformio/penv/bin/python
$P -m ensurepip                                    # penv is created without pip
$P -m pip install -U setuptools wheel
$P -m pip install -e ~/.platformio/packages/tool-esptoolpy   # editable: resolves
                                                             # to the path the
                                                             # platform checks
# ESP-IDF's own venv, for the CMake step:
E=~/.platformio/penv/.espidf-5.5.5/bin/python
$E -m ensurepip && $E -m pip install "cryptography~=44.0.0" "pyparsing>=3.1.0,<4" \
    "idf-component-manager~=2.4.11" "esp-idf-kconfig~=3.7.0"
```

Symptom if the IDF venv is missing them: `ModuleNotFoundError: No module named
'idf_component_manager'` from `build.cmake`, which reads like a broken toolchain
and is not.

### PlatformIO needs Python ≤ 3.12

`pip install platformio==6.1.19` under Python 3.14 installs, then fails at
`Error: Failed to install Python dependencies into penv` with no further detail.
Use a 3.12 venv (`~/pioenv` here). Unrelated to the SCons blocker below.

### Deleting `managed_components/` without clearing the env build dir

Removing `managed_components/` and `dependencies.lock` to force re-resolution
leaves `.pio/build/<env>/` full of build edges pointing at sources that no longer
exist:

```
*** [.../espressif__esp-dl/vision/detect/dl_detect_postprocessor.cpp.o]
    Source '…' not found, needed by target '…'
```

Delete `.pio/build/<env>/` at the same time. The related failure —
`Failed to resolve component 'espressif__cbor' required by component …` — is the
same cache in the opposite state, and clearing both fixes it.

### Bluetooth on the S3: "esp_bt.h: No such file or directory"

NimBLE-Arduino fails to compile under a `custom_sdkconfig` (hybrid) build even
though `libbt.a` and `libbtdm_app.a` for the S3 are present. The S3 belongs to
IDF's **c3 family** for Bluetooth, so its controller headers live under
`bt/include/esp32c3/`, and the hybrid build does not add that path. See
`[env:sticky-ble]`, which adds it explicitly — and note the NimBLE sdkconfig
entries in `firmware_tuned_c3` are C3-only, so an S3 env must enable
`CONFIG_BT_*` itself.

### BLE pairing that silently never encrypts

Three traps, all of which look identical from the outside — the phone connects,
nothing streams, and no error appears on either side.

**Erasing bonds on every boot breaks pairing.** It reads as hygiene and is the
opposite: the phone keeps its half of the bond while the device throws its half
away at each reflash. On the next connection iOS tries to encrypt with a key
this device can no longer produce, the attempt fails, and because the pairing is
Just Works (no passkey) iOS shows the user nothing at all. Symptom in the log:

```
[BLE] central 46:e9:… connected (mtu=23 bonded=0 encrypted=0, 1 stored bond(s))
[BLE] requested encryption (ok=1 rc=0)      <- the REQUEST succeeded
[BLE] pairing complete (bonded=0 encrypted=0)
```

`ok=1 rc=0` only means the request was accepted, never that the link encrypted.
Bonds are meant to survive reboots; drop one only when pairing against it has
actually failed.

**Refusing a subscribe before the link is secure is a dead end.** A central
subscribes as soon as it has discovered the service, which is a few hundred ms
*before* pairing finishes. Rejecting that write does not make it try again —
nothing prompts a retry — so the link sits connected and permanently silent.
Record the subscription and gate the audio on `onAuthenticationComplete`
instead; the security property is identical and the happy path actually
completes.

**`deleteAllBonds()` does not always delete all bonds.** It can leave records
behind, and it returns nothing to say so. Check `getNumBonds()` afterwards
rather than logging success on faith — a bond you believe is gone but the phone
still holds is the exact state that produces the silent failure above. A peer
connecting under a resolvable private address may also not match the identity
its bond was stored against, so `deleteBond(address)` can be a no-op for a bond
that is genuinely there.

Also: `NimBLEDevice::deleteAllBonds()` reaches into the NimBLE host, so calling
it before `NimBLEDevice::init()` panics the device into a boot loop with no
useful log line. Guard it with `isInitialized()`.

### A turn that ends on the wrong transport

Symptom: streaming partials look right on screen, then the moment you stop
talking the query disappears and the answer is "I didn't catch that", while the
phone's throughput counter clearly showed audio arriving.

`stopListening()` sent `{"type":"end"}` straight down the WebSocket instead of
calling `sendTurnEnd()`, the function that knows which transport is carrying.
Over Bluetooth that ended two sessions wrongly at once: the device's own socket
had received no audio, so it finalised an empty utterance and answered "I didn't
catch that" — which then overwrote the good transcript — and the phone's session
never finalised at all, so its recogniser kept accumulating and every later turn
replayed the previous utterances. Two symptoms, one line.

Two log labels make this hard to see, so read them carefully:

- `ws=1` in `listen start` / `listen end` means a WebSocket is CONNECTED, not
  that it is carrying the turn. The line now prints `transport=ble|wifi`.
- `ws rx type=...` is logged for answers arriving over BLE too, because both
  transports go through the same handler.

The number that actually tells you which transport carried the audio is
`bytesSent` against `samples`: ADPCM is 4:1 (129920 samples -> ~66 KB), raw
PCM16 over the socket is 2 bytes per sample (~260 KB).

Confirm against the backend rather than the device alone. `docker logs voice-app-1`
prints `turn complete ... samples=N` per session; a session with `samples=0` and
`ended_explicitly=true` is something ending a turn it never fed any audio.

## Flashing the Sticky

- **`upload_speed = 460800`, not 921600.** The CH343 bridge fails to sync at
  921600 more often than not. 460800 writes the 5.5 MB image in ~90 s.
- **GPIO0 is both the boot strap and the sensor I2C clock.** While the app runs it
  drives that line, so auto-reset into the bootloader usually fails. Serial
  `CMD:DOWNLOAD` forces a download-mode reboot in software; then flash with
  `--before no-reset`, or esptool's own reset will knock it straight back out:

  ```bash
  # after sending CMD:DOWNLOAD over the serial port
  esptool --port <port> --baud 460800 --before no-reset --after hard-reset \
      write-flash 0x10000 .pio/build/sticky/firmware.bin
  ```

- **`CMD:DOWNLOAD` does not work in a selftest env.** `runMicSelftest()` and
  `runBleVoiceSelftest()` never return from `setup()`, so `loop()` — and the whole
  serial command handler — never runs. Those envs also skip the sensor init, which
  leaves GPIO0 free, so a plain auto-reset flash works there instead.
- **A wake from deep sleep freezes every pad** (`gpio_deep_sleep_hold_en`) and the
  freeze survives the wake. `setup()` calls `gpio_deep_sleep_hold_dis()` first
  thing; anything that power-gates a peripheral needs its own `gpio_hold_dis`.

## Status
- **`AgentVoiceActivity` COMPILE-VERIFIED** with the pin above (`pio run -e sticky` →
  `[SUCCESS]`, Flash 80.4% / RAM 20.3%). Every `// VERIFY` renderer/UITheme/font/refresh
  API was correct; the only fix was one Wi-Fi type (`std::string ssid`, not `String`).
- Backend it talks to (`voice` service: ASR + Hermes + LAN+token endpoint): live, tested,
  reproducible from homelab `main`.
- **Remaining (not yet done):** wire the activity into a launcher (the 3-file menu edit in
  VOICE.md — touches core UI files + i18n), then `-t upload` to flash and test on the device.
