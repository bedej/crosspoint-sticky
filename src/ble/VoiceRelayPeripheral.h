#pragma once

// BLE voice-relay peripheral (HomeLab-9k7): the device side of the transport the
// iPhone gateway already speaks. Wire contract — service/characteristic UUIDs,
// frame layout, control JSON, answer-down fragmentation — is
// homelab/projects/assistant/phone-relay/VoiceRelayProtocol.md, and is already
// proven end to end against a macOS peripheral standing in for this code.
//
// Roles: the Sticky ADVERTISES (peripheral); the phone connects (central). That is
// not arbitrary — only the central can be relaunched by iOS to service a connection
// it initiated, which is what lets a locked phone in a pocket carry a turn.
//
// Threading: NimBLE callbacks run on the host task. Nothing here calls into the
// renderer or the activity directly; inbound data lands in fixed buffers guarded by
// a mutex and the activity drains them from its own loop, like BleKeyboardHost.
//
// Capability-gated (FREEINK_CAP_BLE_VOICE_RELAY): disabled builds link stub bodies
// and pull in no BLE code, so the flash cost is opt-in.

#include <stddef.h>
#include <stdint.h>

// Default off. Defined here rather than in the SDK's BoardConfig so this feature
// needs no change to the upstream freeink-sdk submodule.
#ifndef FREEINK_CAP_BLE_VOICE_RELAY
#define FREEINK_CAP_BLE_VOICE_RELAY 0
#endif

#include <functional>
#include <string>

class VoiceRelayPeripheral {
 public:
  static VoiceRelayPeripheral& instance();

  // True when this build has the capability compiled in.
  static bool supported();

  // Start advertising. `deviceName` shows in the phone's scan results.
  bool begin(const char* deviceName = "Sticky");
  void end();

  // True once begin() has started the stack. The connectivity menu uses this to
  // decide whether it has to start the peripheral itself.
  bool isRunning() const;
  bool isConnected() const;
  // A central has subscribed to audio-up: frames sent before this are dropped.
  bool isStreaming() const;

  // Tell the phone a turn started/ended (it opens and closes the backend socket).
  void notifyTurnStart();
  void notifyTurnStop();

  // Send one encoded audio frame. Adds the ver+seq header; returns false when no
  // central is subscribed or the stack refuses (queue full) — audio is dropped
  // rather than queued, because late audio is worse than missing audio for ASR.
  bool sendAudioFrame(const uint8_t* coded, size_t len);

  // Drain one complete answer-down JSON object written by the phone, if any.
  // Returns false when nothing is pending. Reassembles fragments.
  bool popAnswer(std::string& out);

  // Pairing (HomeLab-tdg/jhe). Relaying requires a bonded, encrypted central;
  // a new phone can only bond while the window is open, which the UI time-boxes.
  void setPairingWindow(bool open);
  bool isPairingWindowOpen() const;
  bool isBonded() const;
  int bondCount() const;
  void forgetBonds();

  // Latest link state reported by the phone ("connecting" | "ready" | "relaying" |
  // "error"), for the device's own screen.
  std::string linkState() const;

 private:
  VoiceRelayPeripheral() = default;
};
