#pragma once
// AgentVoiceActivity — push-to-talk voice for the reTerminal Sticky.
//
// Flow: AI-Voice button starts listening -> capture PDM mic (freeink::Microphone,
// 16 kHz mono PCM16) -> stream 20 ms binary frames over a WebSocket to the homelab
// `voice` service (ws://<host>:18092/v1/stream?token=...) -> receive JSON messages
// (partial / transcript / answer.delta / answer.done) -> render the answer to
// e-paper. No TTS (the Sticky has no speaker); the answer appears as text.
//
// FIRST DRAFT — not yet built/flashed. Verify the APIs marked `// VERIFY` against
// the local headers on the first `pio run -e sticky`. See VOICE.md.

#include <Arduino.h>
#include <WebSocketsClient.h>

#include <string>

#include <Microphone.h>

#include "activities/Activity.h"

class AgentVoiceActivity : public Activity {
 public:
  AgentVoiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~AgentVoiceActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Keep the device awake and the loop hot while a conversation is live.
  bool preventAutoSleep() override { return state_ != State::Idle && state_ != State::Error; }
  bool skipLoopDelay() override { return wsConnected_; }

  // WS event trampoline (WebSocketsClient takes a C callback).
  void onWsEvent(WStype_t type, uint8_t* payload, size_t len);

 private:
  enum class State { Connecting, Idle, Listening, Answering, Error };

  void connectWifi();          // STA from saved creds
  void startListening();
  void stopListening();        // sends {"type":"end"}
  void pumpMic();              // read frames -> ws.sendBIN while Listening
  void handleMessage(const char* json, size_t len);
  void markDirty();            // throttled requestUpdate()

  freeink::Microphone mic_;
  WebSocketsClient ws_;
  bool wsConnected_ = false;

  State state_ = State::Connecting;
  std::string status_;         // one-line status/header
  std::string transcript_;     // latest ASR text (partial/final)
  std::string answer_;         // accumulated answer.delta text
  bool dirty_ = false;
  unsigned long lastRenderMs_ = 0;

  int16_t micBuf_[320];        // 20 ms @ 16 kHz mono
};
