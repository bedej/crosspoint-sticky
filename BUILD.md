# Building the Sticky firmware (`[env:sticky]`) — recipe + the SCons blocker

You do **not** need the hardware to *compile* (only to flash). This documents how to
build in a container, and the one toolchain bug that currently blocks a clean compile.

## Container build recipe (no hardware)

On a machine with Docker + internet (e.g. blackbox):

```bash
# 1. get the source (this fork, recursively — freeink-sdk is a nested submodule)
git clone --rectree ... # or rsync an already-checked-out tree, incl. freeink-sdk/
#    rsync -a --exclude .git <checkout>/ host:/tmp/sticky-build/

# 2. build in a PlatformIO container, caching the toolchain in a named volume
docker volume create pio_home
docker run --rm -v /tmp/sticky-build:/proj -v pio_home:/root/.platformio -w /proj \
  python:3.12-slim bash -c '
    apt-get update -qq && apt-get install -y -qq git >/dev/null
    pip install -q platformio
    pio run -e sticky'
```

First run downloads the pioarduino platform + ESP-IDF toolchain + rebuilds the Arduino
core (slow, ~10-20 min; cached in `pio_home` after). `AgentVoiceActivity.cpp` compiles
as part of this once the blocker below is resolved.

## ⚠ The blocker: SCons 4.11 removed `SCons.Tool.FortranCommon`

Right now `pio run -e sticky` fails in ~11-20 s, **before compiling any source**, with:

```
*** [.pio/build/sticky/firmware.elf] ModuleNotFoundError : No module named 'SCons.Tool.FortranCommon'
  File ".../packages/tool-scons/scons-local-4.11.1/SCons/Tool/linkCommon/__init__.py", line 132, in smart_link
```

**Root cause.** PlatformIO builds with its **own vendored SCons**, downloaded to
`~/.platformio/packages/tool-scons/scons-local-4.11.1/` — a 4.11-era SCons that
**dropped the `FortranCommon` module**, while that same SCons's `linkCommon`/`fortran`
still `import`s it. So SCons dies during build-environment setup. **This is a
PlatformIO + SCons-4.11 packaging incompatibility, not a firmware bug** — no source is
ever compiled.

**Dead ends (documented so nobody repeats them):**
- Pinning/patching SCons in the container or in `~/.platformio/penv` — *irrelevant*; the
  build uses the **vendored** `tool-scons`, not those.
- Copying `FortranCommon.py` into `.../tool-scons/scons-local-4.11.1/SCons/Tool/` — the
  patch lands, but `pio run` **re-materializes tool-scons** each run and clobbers it.

**The fix (apply in a real pioarduino dev env, then compile-verify):** force a
`tool-scons` whose bundled SCons is **< 4.9** (which still has `FortranCommon`). Options,
in order of preference:
1. Pin it in the `[env:sticky]` (or a base env) `platform_packages`, e.g.
   `platform_packages = platformio/tool-scons@<version bundling scons<4.9>` — find the
   version from `pio pkg show platformio/tool-scons` / the registry.
2. Use a PlatformIO **core** version old enough that its `tool-scons` predates SCons 4.9.
3. If patching the vendored SCons, make it survive re-extraction (e.g. pin the package so
   `pio` treats it as satisfied and won't re-fetch), then drop `FortranCommon.py` in.

Once `pio run -e sticky` gets past SCons, `AgentVoiceActivity.cpp` will compile and any
real errors (the `// VERIFY` renderer/UITheme signatures noted in VOICE.md) surface —
fix those against the local headers, then `-t upload` to flash.

## Status
- Backend the firmware talks to (`voice` service, ASR, Hermes, LAN+token endpoint): live,
  tested, reproducible from homelab `main`.
- Firmware (`AgentVoiceActivity`): written against the SDK APIs, **not yet compile-verified**
  because of the SCons blocker above (environmental, pre-compilation).
