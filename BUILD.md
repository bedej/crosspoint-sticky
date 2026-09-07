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

## Status
- **`AgentVoiceActivity` COMPILE-VERIFIED** with the pin above (`pio run -e sticky` →
  `[SUCCESS]`, Flash 80.4% / RAM 20.3%). Every `// VERIFY` renderer/UITheme/font/refresh
  API was correct; the only fix was one Wi-Fi type (`std::string ssid`, not `String`).
- Backend it talks to (`voice` service: ASR + Hermes + LAN+token endpoint): live, tested,
  reproducible from homelab `main`.
- **Remaining (not yet done):** wire the activity into a launcher (the 3-file menu edit in
  VOICE.md — touches core UI files + i18n), then `-t upload` to flash and test on the device.
