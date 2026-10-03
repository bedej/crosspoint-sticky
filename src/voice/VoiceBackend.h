#pragma once

#include <stdint.h>

// Which agent answers a turn (HomeLab-cuq): Hermes, the household assistant, or
// Muse. The device only names the backend on the wire — the choice of what that
// means lives in the voice service.
//
// Persisted in NVS rather than in /.crosspoint/voice.json. That file is the
// user's own, carries the shared secret, and this firmware only ever reads it;
// rewriting it to flip a toggle would put the token at risk for no benefit.
//
// Read through voiceBackend(), which caches. NVS and the SD card share timing
// with the e-paper SPI, so a store on a render path can take the display down
// with it — see the connectivity screen crash in 0f74c6a. Load once, write only
// on change.

enum class VoiceBackend : uint8_t { Hermes = 0, Muse = 1 };

// What the server last said about a backend in ready.backends. Unknown is the
// honest default: the server has not implemented `backends` yet, and a device
// that claims "Ready" without being told is guessing.
enum class BackendStatus : uint8_t { Unknown = 0, Ready, Connecting, Unpaired, Error };

// "hermes" | "muse" — the wire spelling, which is also what the menu shows.
const char* voiceBackendWireName(VoiceBackend backend);

// Cached after the first read.
VoiceBackend voiceBackend();

// Writes NVS. Call only when the value actually changes, never from render.
void setVoiceBackend(VoiceBackend backend);

// Last status the server reported for a backend, and the setter the voice
// activity calls when a `ready` frame arrives. Absent `backends` leaves Hermes
// Ready (it is the one the server has always had) and Muse Unknown.
BackendStatus voiceBackendStatus(VoiceBackend backend);
void setVoiceBackendStatus(VoiceBackend backend, BackendStatus status);

// Parses one status word from ready.backends: ready | connecting | unpaired |
// error. Anything else is Unknown rather than a guess.
BackendStatus parseBackendStatus(const char* word);
