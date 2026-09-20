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
#include <Microphone.h>
#include <WebSocketsClient.h>

#include <string>

#include "activities/Activity.h"
#include "activities/transcript/ConversationSpool.h"
#include "activities/transcript/TranscriptView.h"

class AgentVoiceActivity : public Activity {
 public:
  AgentVoiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~AgentVoiceActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;

  // Serial "CMD:PTT" (dev/test harness): toggle talk as if Up were pressed.
  static void requestPushToTalk() { pttRequested_ = true; }
  // Serial test hooks. Touch cannot be automated and ASR needs a room with
  // sound in it, so these are the only way to exercise the transcript view's
  // streaming, paging and power-cycle persistence from the harness. They drive
  // exactly the same spool + layout path a real turn does; injected agent text
  // is dripped in chunks so the streaming cadence is the real one.
  static void injectUserTurn(const char* text) { injectUser_ = text; }
  static void injectAgentTurn(const char* text) {
    injectAgent_ = text;
    injectAgentAt_ = 0;
  }
  // -1 prev page, +1 next page, -2 previous turn, +2 jump to latest.
  static void requestPageMove(int8_t move) { pageRequest_ = move; }
  void render(RenderLock&&) override;

  // Keep the device awake and the loop hot while a conversation is live.
  bool preventAutoSleep() override { return state_ != State::Idle && state_ != State::Error; }
  bool skipLoopDelay() override { return wsConnected_; }

  // WS event trampoline (WebSocketsClient takes a C callback).
  void onWsEvent(WStype_t type, uint8_t* payload, size_t len);

 private:
  static inline volatile bool pttRequested_ = false;
  static inline std::string injectUser_;
  static inline std::string injectAgent_;
  static inline size_t injectAgentAt_ = 0;
  static inline volatile int8_t pageRequest_ = 0;
  void pumpInjectedTurns();
  enum class State { Connecting, Idle, Listening, Answering, Error };

  bool loadConfig();   // token/host/port from /.crosspoint/voice.json (SD)
  void connectWifi();  // STA from saved creds
  void startListening();
  void stopListening();  // sends {"type":"end"}
  void pumpMic();        // read frames -> ws.sendBIN while Listening
  void handleMessage(const char* json, size_t len);
  void markDirty();  // throttled requestUpdate()
  // Paging. INHERITED from the reader rather than invented: the tap zones, the
  // button mapping and the turn guard all come from ReaderUtils/EpubReader so a
  // conversation feels identical to a book.
  void handlePaging();
  void applyPendingTurn();
  void openSpoolTurn(ConversationSpool::Role role, const char* text);
  void appendAnswerText(const char* text);  // Markdown-stripped append
  void finishAnswer();
  void failTurnIfInFlight(const char* msg);  // Listening/Answering -> Idle + error

  std::string token_;  // shared secret, from SD config — never baked in
  std::string host_;
  uint16_t port_ = 18092;

  freeink::Microphone mic_;
  WebSocketsClient ws_;
  bool wsConnected_ = false;

  State state_ = State::Connecting;
  ConversationSpool spool_;
  TranscriptView view_;
  // Turn-safety, copied from EpubReaderActivity: a page turn that lands mid-
  // render tears the panel. It matters more here than in a book because the
  // reader taps while text is actively streaming in.
  int8_t pendingManualTurn_ = 0;  // -1 prev, +1 next
  bool pendingTurnSkip_ = false;  // long-press back: previous TURN
  bool pendingJumpLatest_ = false;
  unsigned long lastPageTurnMs_ = 0;
  bool nextRefreshHalf_ = false;  // page turns get a HALF_REFRESH

  std::string status_;      // one-line status/header
  std::string transcript_;  // latest ASR text (partial/final)
  std::string answer_;      // accumulated answer.delta text
  bool dirty_ = false;
  unsigned long lastRenderMs_ = 0;
  // Stall watchdog: last time any server frame arrived while a turn is in flight.
  // If Thinking/Answering goes quiet past kStallTimeoutMs, surface an error instead
  // of hanging. Reset by every partial/delta so a slow streaming answer isn't cut off.
  unsigned long lastServerMs_ = 0;
  static constexpr unsigned long kStallTimeoutMs = 15000;

  int16_t micBuf_[320];  // 20 ms @ 16 kHz mono
  // Mic conditioning: DC blocker state (1/2^kDcShift per sample ~= 8 Hz corner
  // at 16 kHz) and the fixed make-up gain applied before streaming.
  static constexpr int kDcShift = 9;
  static constexpr int kDcFrac = 8;  // fixed-point bits below the LSB in dcState_
  static constexpr int32_t kMicGain = 32;
  int32_t dcState_ = 0;
  bool dcPrimed_ = false;
  bool gotTranscript_ = false;

  // Per-listen mic level stats (logged on stop) — near-zero => mic sent silence.
  int16_t micPeak_ = 0;
  uint64_t micAbsSum_ = 0;
  uint32_t micCount_ = 0;
  uint32_t bytesSent_ = 0;
  uint32_t framesDropped_ = 0;  // mic frames read while WS not connected
};
