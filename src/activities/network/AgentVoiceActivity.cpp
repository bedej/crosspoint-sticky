#include "AgentVoiceActivity.h"

#include "activities/RenderLock.h"
#include "activities/reader/ReaderUtils.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_system.h>

#include "WifiCredentialStore.h"
#include "components/UITheme.h"  // VERIFY include path (drawCenteredWrappedText)
#include "fontIds.h"             // VERIFY include path (UI_10_FONT_ID)
#include "platform/MicSelftest.h"

// ---- Endpoint config -------------------------------------------------------
// Read at runtime from /.crosspoint/voice.json on the SD card — NO secret is baked
// into the firmware:  { "token": "<VOICE_TOKEN>", "host": "192.168.1.85", "port": 18092 }
// `token` is required (the voice service's shared secret); host/port default below.
static constexpr char VOICE_CONFIG_PATH[] = "/.crosspoint/voice.json";
static constexpr char VOICE_HOST_DEFAULT[] = "192.168.1.85";
static constexpr uint16_t VOICE_PORT_DEFAULT = 18092;

// WebSocketsClient takes a C callback; bounce it to the live instance.
static AgentVoiceActivity* g_voiceInstance = nullptr;
static void wsTrampoline(WStype_t type, uint8_t* payload, size_t len) {
  if (g_voiceInstance) g_voiceInstance->onWsEvent(type, payload, len);
}

AgentVoiceActivity::AgentVoiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Voice", renderer, mappedInput), view_(renderer, spool_) {}

AgentVoiceActivity::~AgentVoiceActivity() {
  if (g_voiceInstance == this) g_voiceInstance = nullptr;
}

void AgentVoiceActivity::connectWifi() {
  status_ = "Connecting Wi-Fi...";
  requestUpdate();
  // VERIFY: WifiSelectionActivity holds a RenderLock around loadFromFile() because
  // the SD card and the e-paper share the SPI bus. Mirror that if you see SPI races.
  WIFI_STORE.loadFromFile();
  std::string ssid = WIFI_STORE.getLastConnectedSsid();
  auto cred = WIFI_STORE.findCredential(ssid);  // std::optional<WifiCredential>

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  if (cred.has_value()) {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  } else if (ssid.length()) {
    WiFi.begin(ssid.c_str());
  }
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }
}

bool AgentVoiceActivity::loadConfig() {
  host_ = VOICE_HOST_DEFAULT;
  port_ = VOICE_PORT_DEFAULT;
  token_.clear();
  HalFile file;
  if (!Storage.openFileForRead("VOICE", VOICE_CONFIG_PATH, file) || !file) return false;
  const size_t n = file.size();
  if (n == 0 || n > 4096) return false;
  std::string buf;
  buf.resize(n);
  const int got = file.read(&buf[0], n);
  if (got <= 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, buf.c_str(), static_cast<size_t>(got))) return false;
  token_ = static_cast<const char*>(doc["token"] | "");
  host_ = static_cast<const char*>(doc["host"] | VOICE_HOST_DEFAULT);
  port_ = static_cast<uint16_t>(doc["port"] | VOICE_PORT_DEFAULT);
  return !token_.empty();
}

void AgentVoiceActivity::onEnter() {
  Activity::onEnter();
  g_voiceInstance = this;
  // Build marker — grep this in serial to confirm which binary is actually running.
  LOG_INF("AVA", "voice build: usb-pin-fix (mic on 19/20, no native USB)");

  if (!loadConfig()) {
    state_ = State::Error;
    status_ = "Add /.crosspoint/voice.json with a \"token\" (see VOICE.md).";
    requestUpdate();
    return;
  }

  LOG_INF("AVA", "config host=%s port=%u tokenLen=%u", host_.c_str(), (unsigned)port_, (unsigned)token_.length());

  connectWifi();
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("AVA", "wifi connect failed (status=%d)", (int)WiFi.status());
    state_ = State::Error;
    status_ = "Wi-Fi failed. Set it up in File Transfer / Wi-Fi Networks.";
    requestUpdate();
    return;
  }
  LOG_INF("AVA", "wifi ok ip=%s rssi=%d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  LOG_INF("AVA", "reset reason=%d", (int)esp_reset_reason());
  logMicPinOwnership("voice-before-begin");
  // The sleep path (PowerManager::powerDownRailsForSleep) holds the mic enable
  // OFF with gpio_hold_en, and the hold survives the wake; Microphone::begin()
  // drives the pin without releasing it, so the mic would stay unpowered.
  if (const int8_t micEn = BoardConfig::ACTIVE.mic.enable; micEn >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(micEn));
  }
  if (!mic_.begin(16000)) {
    LOG_ERR("AVA", "mic begin failed");
    state_ = State::Error;
    status_ = "Mic init failed";
    requestUpdate();
    return;
  }
  LOG_INF("AVA", "mic begin ok @16k, present=%d", (int)mic_.present());
  logMicPinOwnership("voice-after-begin");
  logMicPinActivity("voice-after-begin");

  const std::string path = "/v1/stream?token=" + token_;
  LOG_INF("AVA", "ws connecting %s:%u%s", host_.c_str(), (unsigned)port_, "/v1/stream");
  ws_.begin(host_.c_str(), port_, path.c_str());
  ws_.onEvent(wsTrampoline);
  ws_.setReconnectInterval(3000);

  // The spool is the conversation. Deep sleep wakes through a reset, so the
  // page index is gone even though the text is not — rebuild it from the raw
  // records and land on the latest page.
  view_.begin();
  if (spool_.begin()) {
    view_.rebuildIndex();
    view_.jumpToLatest();
    LOG_INF("AVA", "session %s: %u turns, %u pages", spool_.sessionId().c_str(),
            static_cast<unsigned>(spool_.turnCount()), static_cast<unsigned>(spool_.pageCount()));
  } else {
    LOG_ERR("AVA", "spool unavailable; transcript will not persist");
  }

  state_ = State::Idle;
  status_ = "Press Up to talk  |  Down to exit";
  requestUpdate();
}

void AgentVoiceActivity::onExit() {
  view_.endTurn();
  spool_.endAgentTurn();
  mic_.end();
  ws_.disconnect();
  WiFi.disconnect(false);
  g_voiceInstance = nullptr;
  Activity::onExit();
}

void AgentVoiceActivity::pumpMic() {
  const int n = mic_.read(micBuf_, 320, 20);
  if (n <= 0) return;
  // The PDM stream carries a large DC offset (~1300) and speech peaks only a
  // few hundred LSB above it, which ASR hears as silence. One-pole DC blocker,
  // then a fixed gain with saturation, before anything measures or sends it.
  if (!dcPrimed_) {
    dcState_ = static_cast<int32_t>(micBuf_[0]) << kDcFrac;
    dcPrimed_ = true;
  }
  for (int i = 0; i < n; i++) {
    const int32_t x = micBuf_[i];
    // dcState_ is Q<kDcFrac>: tracking the offset at sub-LSB resolution keeps the
    // integer shift from stalling while still short of it — a leftover offset
    // would be amplified with the signal and eat the headroom.
    dcState_ += ((x << kDcFrac) - dcState_) >> kDcShift;
    const int32_t ac = (x - (dcState_ >> kDcFrac)) * kMicGain;
    micBuf_[i] = static_cast<int16_t>(ac > 32767 ? 32767 : (ac < -32768 ? -32768 : ac));
  }
  for (int i = 0; i < n; i++) {
    const int16_t v = micBuf_[i];
    const int32_t a = v < 0 ? -static_cast<int32_t>(v) : v;
    micAbsSum_ += static_cast<uint32_t>(a);
    if (a > micPeak_) micPeak_ = static_cast<int16_t>(a);
  }
  micCount_ += static_cast<uint32_t>(n);
  if (wsConnected_) {
    ws_.sendBIN(reinterpret_cast<uint8_t*>(micBuf_), static_cast<size_t>(n) * sizeof(int16_t));
    bytesSent_ += static_cast<uint32_t>(n) * sizeof(int16_t);
  } else {
    framesDropped_++;
  }
}

void AgentVoiceActivity::startListening() {
  dcState_ = 0;
  dcPrimed_ = false;
  gotTranscript_ = false;
  transcript_.clear();
  answer_.clear();
  micPeak_ = 0;
  micAbsSum_ = 0;
  micCount_ = 0;
  bytesSent_ = 0;
  framesDropped_ = 0;
  state_ = State::Listening;
  status_ = "Listening... (Up to stop)";
  LOG_INF("AVA", "listen start (ws=%d)", (int)wsConnected_);
  markDirty();
}

void AgentVoiceActivity::stopListening() {
  const uint32_t avgAbs = micCount_ ? static_cast<uint32_t>(micAbsSum_ / micCount_) : 0;
  LOG_INF("AVA", "listen end: samples=%u avgAbs=%u peak=%d bytesSent=%u dropped=%u ws=%d", micCount_, avgAbs,
          (int)micPeak_, bytesSent_, framesDropped_, (int)wsConnected_);
  ws_.sendTXT("{\"type\":\"end\"}");
  state_ = State::Answering;
  status_ = "Thinking...";
  lastServerMs_ = millis();  // arm the stall watchdog
  markDirty();
}

// Collect a page-turn intent. Priority mirrors the reader: the menu gesture
// (centre third) is claimed first so it can never double as a page turn, then
// long-press variants, then a plain turn.
void AgentVoiceActivity::handlePaging() {
  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) return;

  // Touch only. ReaderUtils::detectPageTurn is deliberately NOT used here:
  // MappedInputManager maps PageBack/PageForward onto BTN_UP/BTN_DOWN, the same
  // two side buttons voice already owns (Up talks, Down goes back), so calling
  // it would turn a page on every push-to-talk. The reader's tap model carries
  // over intact instead — outer horizontal thirds, centre third reserved for the
  // menu — and the side buttons keep the voice semantics the ticket specifies.
  const ReaderUtils::TouchPageTurn touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  bool prev = touch.prev;
  bool next = touch.next;
  const bool held = touch.heldMs >= ReaderUtils::SKIP_HOLD_MS;

  // TURN = CHAPTER. The reader binds long-press to chapter skip, and a
  // conversation turn is the same kind of labelled boundary.
  if (prev && held) {
    pendingTurnSkip_ = true;
    prev = false;
  }
  // Paging back through a six-page reply and then tapping forward six times is
  // miserable, so long-press forward returns to the live tail in one move.
  if (next && held) {
    pendingJumpLatest_ = true;
    next = false;
  }
  if (prev) pendingManualTurn_ = -1;
  if (next) pendingManualTurn_ = 1;
}

void AgentVoiceActivity::applyPendingTurn() {
  if (pendingManualTurn_ == 0 && !pendingTurnSkip_ && !pendingJumpLatest_) return;

  // Copied verbatim from EpubReaderActivity: never turn while the panel is
  // mid-render, and never faster than the panel can settle.
  constexpr unsigned long kMinManualTurnGapMs = 200;
  if (RenderLock::peek() || (millis() - lastPageTurnMs_) < kMinManualTurnGapMs) return;

  bool changed = false;
  if (pendingJumpLatest_) {
    changed = view_.jumpToLatest();
    pendingJumpLatest_ = false;
  } else if (pendingTurnSkip_) {
    changed = view_.prevTurn();
    pendingTurnSkip_ = false;
  } else {
    changed = pendingManualTurn_ > 0 ? view_.pageNext() : view_.pagePrev();
  }
  pendingManualTurn_ = 0;
  if (!changed) return;

  lastPageTurnMs_ = millis();
  // Every page turn is a HALF_REFRESH, which conveniently doubles as the
  // periodic FAST-residual cleanup: a screenful is ~14-18 FAST line appends.
  nextRefreshHalf_ = true;
  requestUpdate();
}

void AgentVoiceActivity::openSpoolTurn(const ConversationSpool::Role role, const char* text) {
  // Order matters: the record has to hit the spool first so the view can record
  // the page index against its real byte offset.
  view_.beginTurn(role, spool_.lastTurnOffset(), spool_.lastTurnIndex());
  view_.appendText(text);
}

// Test-harness injection, deliberately routed through the production paths:
// the spool write, the incremental layout and the per-line refresh are all the
// real ones, so what this verifies is what a real turn does.
void AgentVoiceActivity::pumpInjectedTurns() {
  if (!injectUser_.empty()) {
    const std::string text = injectUser_;
    injectUser_.clear();
    if (spool_.appendUserTurn(text)) {
      openSpoolTurn(ConversationSpool::Role::User, text.c_str());
      view_.endTurn();
    }
  }

  if (!injectAgent_.empty()) {
    // One chunk per loop iteration, sized like a real answer.delta, so settled
    // lines emerge at the same cadence and the page still fills mid-stream.
    constexpr size_t kChunk = 48;
    const size_t remaining = injectAgent_.size() - injectAgentAt_;
    const size_t take = remaining < kChunk ? remaining : kChunk;
    const std::string chunk = injectAgent_.substr(injectAgentAt_, take);
    injectAgentAt_ += take;

    const bool opensTurn = !spool_.agentTurnOpen();
    spool_.appendAgentDelta(chunk);
    if (opensTurn) {
      openSpoolTurn(ConversationSpool::Role::Agent, chunk.c_str());
    } else {
      view_.appendText(chunk.c_str());
    }

    if (injectAgentAt_ >= injectAgent_.size()) {
      injectAgent_.clear();
      injectAgentAt_ = 0;
      view_.endTurn();
      spool_.endAgentTurn();
      nextRefreshHalf_ = true;
      requestUpdate();
    }
  }

  const int8_t move = pageRequest_;
  if (move != 0) {
    pageRequest_ = 0;
    if (move == 2) {
      pendingJumpLatest_ = true;
    } else if (move == -2) {
      pendingTurnSkip_ = true;
    } else {
      pendingManualTurn_ = move;
    }
  }
}

void AgentVoiceActivity::loop() {
  ws_.loop();
  if (state_ == State::Listening) pumpMic();

  // The Sticky's AI-Voice button IS CrossPoint's Power button, which main.cpp consumes
  // for sleep before an activity sees it — so push-to-talk uses the Up side button,
  // and Down exits back to the menu.
  const bool pttSerial = pttRequested_;
  pttRequested_ = false;
  if (pttSerial || mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    // Talking is always about the live tail: snap forward before capturing so a
    // reply never streams onto a page the reader has paged away from.
    pendingJumpLatest_ = true;
    if (state_ == State::Idle || state_ == State::Answering) {
      startListening();
    } else if (state_ == State::Listening) {
      stopListening();
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    // Back returns to the live tail if the reader is behind it; only an
    // already-current view exits.
    if (!view_.atLatest()) {
      pendingJumpLatest_ = true;
    } else {
      finish();
      return;
    }
  }

  pumpInjectedTurns();
  handlePaging();
  applyPendingTurn();

  // Stall watchdog: if a turn goes quiet (WS dropped, or ASR/Hermes returned
  // nothing) surface an error instead of hanging on "Thinking...".
  if (state_ == State::Answering && millis() - lastServerMs_ > kStallTimeoutMs) {
    LOG_ERR("AVA", "answer stalled >%lums, resetting", kStallTimeoutMs);
    state_ = State::Idle;
    status_ = "No response. Press Up to try again.";
    requestUpdate(true);
    return;
  }

  // One refresh per SETTLED LINE, never per token. A FAST refresh is 300-500 ms,
  // so a screen fills in ~6 s while reading it takes ~55 s; the layout engine
  // holds the ragged tail back and only completed lines reach the panel.
  const bool linesWaiting = view_.hasPendingAppend() || view_.needsFullPaint();
  if ((dirty_ || linesWaiting) && millis() - lastRenderMs_ > 250) {
    dirty_ = false;
    lastRenderMs_ = millis();
    requestUpdate();
  }
}

void AgentVoiceActivity::markDirty() { dirty_ = true; }

void AgentVoiceActivity::appendAnswerText(const char* text) {
  // Hermes answers in Markdown; e-paper has one face and no styling, so the
  // emphasis/code runs would render as literal **asterisks**. Strip the inline
  // markers as deltas arrive (they never split mid-marker in practice, and a
  // stray single char is harmless).
  for (const char* p = text; *p; ++p) {
    if (*p == '`') continue;
    if (*p == '*' && (p[1] == '*' || (p != text && p[-1] == '*'))) continue;
    if (*p == '_' && p[1] == '_') {
      ++p;
      continue;
    }
    answer_ += *p;
  }
}

void AgentVoiceActivity::finishAnswer() {
  // Flush the ragged tail the layout engine was holding, and close the turn so
  // the next reply starts a fresh one in the spool.
  view_.endTurn();
  spool_.endAgentTurn();
  nextRefreshHalf_ = true;
  state_ = State::Idle;
  status_ = "Press Up to talk  |  Down to exit";
  dirty_ = false;
  lastRenderMs_ = 0;
  requestUpdate(true);  // one clean full-ish refresh at the end
}

void AgentVoiceActivity::failTurnIfInFlight(const char* msg) {
  // The server closes right after answer.done, and the close can overtake it;
  // an answer already streamed in means the turn completed.
  if (state_ == State::Answering && answer_.empty() && gotTranscript_ && transcript_.empty()) {
    LOG_INF("AVA", "closed after empty transcript");
    answer_ = "I didn't catch that - please try again.";
    finishAnswer();
    return;
  }
  if (state_ == State::Answering && !answer_.empty()) {
    LOG_INF("AVA", "closed after answer (answerLen=%u), treating as done", (unsigned)answer_.length());
    finishAnswer();
    return;
  }
  if (state_ == State::Listening || state_ == State::Answering) {
    state_ = State::Idle;
    status_ = msg;
    requestUpdate(true);
  }
}

void AgentVoiceActivity::onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected_ = true;
      LOG_INF("AVA", "ws connected");
      break;
    case WStype_DISCONNECTED:
      wsConnected_ = false;
      LOG_INF("AVA", "ws disconnected (state=%d answerLen=%u)", (int)state_, (unsigned)answer_.length());
      failTurnIfInFlight("Connection lost. Press Up to try again.");
      break;
    case WStype_ERROR:
      wsConnected_ = false;
      LOG_ERR("AVA", "ws error");
      failTurnIfInFlight("Connection error. Press Up to try again.");
      break;
    case WStype_TEXT:
      handleMessage(reinterpret_cast<const char*>(payload), len);
      break;
    default:
      break;
  }
}

void AgentVoiceActivity::handleMessage(const char* json, size_t len) {
  JsonDocument doc;
  if (const auto err = deserializeJson(doc, json, len)) {
    LOG_ERR("AVA", "bad ws frame (%s) len=%u", err.c_str(), (unsigned)len);
    return;
  }
  const char* t = doc["type"] | "";
  LOG_DBG("AVA", "ws rx type=%s len=%u state=%d", t, (unsigned)len, (int)state_);
  lastServerMs_ = millis();  // any server frame keeps the stall watchdog alive
  if (!strcmp(t, "partial") || !strcmp(t, "transcript")) {
    transcript_ = static_cast<const char*>(doc["text"] | "");
    if (!strcmp(t, "transcript")) {
      gotTranscript_ = true;
      LOG_INF("AVA", "final transcript: '%s'", transcript_.c_str());
      // The finalised ASR text is the user's turn: persist it and flow it into
      // the document in italic, so it can be paged back to like any other turn.
      if (!transcript_.empty() && spool_.appendUserTurn(transcript_)) {
        openSpoolTurn(ConversationSpool::Role::User, transcript_.c_str());
        view_.endTurn();
      }
    }
    // The header is LIVE feedback only: it shows what ASR is hearing while the
    // words have nowhere else to be. The moment the transcript is final it
    // becomes a turn in the document below, so the header hands off rather than
    // showing the same sentence twice.
    if (gotTranscript_) {
      view_.setHeader(std::string());
    } else {
      view_.setHeader(transcript_.empty() ? std::string() : ("You: " + transcript_));
    }
    markDirty();
  } else if (!strcmp(t, "answer.delta")) {
    const size_t before = answer_.length();
    appendAnswerText(static_cast<const char*>(doc["text"] | ""));
    const std::string chunk = answer_.substr(before);
    if (!chunk.empty()) {
      // Every delta reaches the card as it arrives, so a crash or a sleep
      // mid-reply costs at most this chunk rather than the whole turn.
      const bool opensTurn = !spool_.agentTurnOpen();
      spool_.appendAgentDelta(chunk);
      if (opensTurn) {
        openSpoolTurn(ConversationSpool::Role::Agent, chunk.c_str());
      } else {
        view_.appendText(chunk.c_str());
      }
    }
    markDirty();
  } else if (!strcmp(t, "answer.done")) {
    LOG_INF("AVA", "answer.done answerLen=%u", (unsigned)answer_.length());
    finishAnswer();
  } else if (!strcmp(t, "error")) {
    LOG_ERR("AVA", "server error: %s", static_cast<const char*>(doc["message"] | ""));
    state_ = State::Idle;  // don't strand the user in "Thinking..."
    status_ = std::string("Error: ") + static_cast<const char*>(doc["message"] | "");
    requestUpdate(true);
  }
}

void AgentVoiceActivity::render(RenderLock&&) {
  if (!view_.ready()) {
    // No reader font (no SD card, or fonts missing): there is no flowed document
    // to draw, so fall back to the plain status line rather than a blank panel.
    renderer.clearScreen();
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    int y = 8;
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, status_.c_str(), renderer.getScreenWidth() - 16, 3)) {
      renderer.drawText(UI_10_FONT_ID, 8, y, line.c_str());
      y += lineH;
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  view_.setStatus(status_);
  const auto mode = nextRefreshHalf_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  nextRefreshHalf_ = false;
  if (view_.needsFullPaint()) {
    view_.renderFull(mode);
  } else {
    view_.renderAppend();
  }
}
