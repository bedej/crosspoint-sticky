#include "AgentVoiceActivity.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include "WifiCredentialStore.h"
#include "components/UITheme.h"  // VERIFY include path (drawCenteredWrappedText)
#include "fontIds.h"             // VERIFY include path (UI_10_FONT_ID)

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
    : Activity("Voice", renderer, mappedInput) {}

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
  LOG_INF("AVA", "voice build: pdm-pcm-fix + error-handling");

  if (!loadConfig()) {
    state_ = State::Error;
    status_ = "Add /.crosspoint/voice.json with a \"token\" (see VOICE.md).";
    requestUpdate();
    return;
  }

  LOG_INF("AVA", "config host=%s port=%u tokenLen=%u", host_.c_str(), (unsigned)port_,
          (unsigned)token_.length());

  connectWifi();
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("AVA", "wifi connect failed (status=%d)", (int)WiFi.status());
    state_ = State::Error;
    status_ = "Wi-Fi failed. Set it up in File Transfer / Wi-Fi Networks.";
    requestUpdate();
    return;
  }
  LOG_INF("AVA", "wifi ok ip=%s rssi=%d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  if (!mic_.begin(16000)) {
    LOG_ERR("AVA", "mic begin failed");
    state_ = State::Error;
    status_ = "Mic init failed";
    requestUpdate();
    return;
  }
  LOG_INF("AVA", "mic begin ok @16k, present=%d", (int)mic_.present());

  const std::string path = "/v1/stream?token=" + token_;
  LOG_INF("AVA", "ws connecting %s:%u%s", host_.c_str(), (unsigned)port_, "/v1/stream");
  ws_.begin(host_.c_str(), port_, path.c_str());
  ws_.onEvent(wsTrampoline);
  ws_.setReconnectInterval(3000);

  state_ = State::Idle;
  status_ = "Press Up to talk  |  Down to exit";
  requestUpdate();
}

void AgentVoiceActivity::onExit() {
  mic_.end();
  ws_.disconnect();
  WiFi.disconnect(false);
  g_voiceInstance = nullptr;
  Activity::onExit();
}

void AgentVoiceActivity::pumpMic() {
  const int n = mic_.read(micBuf_, 320, 20);
  if (n <= 0) return;
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
  LOG_INF("AVA", "listen end: samples=%u avgAbs=%u peak=%d bytesSent=%u dropped=%u ws=%d",
          micCount_, avgAbs, (int)micPeak_, bytesSent_, framesDropped_, (int)wsConnected_);
  ws_.sendTXT("{\"type\":\"end\"}");
  state_ = State::Answering;
  status_ = "Thinking...";
  lastServerMs_ = millis();  // arm the stall watchdog
  markDirty();
}

void AgentVoiceActivity::loop() {
  ws_.loop();
  if (state_ == State::Listening) pumpMic();

  // The Sticky's AI-Voice button IS CrossPoint's Power button, which main.cpp consumes
  // for sleep before an activity sees it — so push-to-talk uses the Up side button,
  // and Down exits back to the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (state_ == State::Idle || state_ == State::Answering) {
      startListening();
    } else if (state_ == State::Listening) {
      stopListening();
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    finish();
    return;
  }

  // Stall watchdog: if a turn goes quiet (WS dropped, or ASR/Hermes returned
  // nothing) surface an error instead of hanging on "Thinking...".
  if (state_ == State::Answering && millis() - lastServerMs_ > kStallTimeoutMs) {
    LOG_ERR("AVA", "answer stalled >%lums, resetting", kStallTimeoutMs);
    state_ = State::Idle;
    status_ = "No response. Press Up to try again.";
    requestUpdate(true);
    return;
  }

  // E-paper is slow and ghosts — never repaint per answer.delta. Coalesce to ~400 ms.
  if (dirty_ && millis() - lastRenderMs_ > 400) {
    dirty_ = false;
    lastRenderMs_ = millis();
    requestUpdate();
  }
}

void AgentVoiceActivity::markDirty() { dirty_ = true; }

void AgentVoiceActivity::failTurnIfInFlight(const char* msg) {
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
      LOG_INF("AVA", "ws disconnected");
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
  if (deserializeJson(doc, json, len)) return;  // ignore malformed frames
  const char* t = doc["type"] | "";
  lastServerMs_ = millis();  // any server frame keeps the stall watchdog alive
  if (!strcmp(t, "partial") || !strcmp(t, "transcript")) {
    transcript_ = static_cast<const char*>(doc["text"] | "");
    if (!strcmp(t, "transcript")) LOG_INF("AVA", "final transcript: '%s'", transcript_.c_str());
    markDirty();
  } else if (!strcmp(t, "answer.delta")) {
    answer_ += static_cast<const char*>(doc["text"] | "");
    markDirty();
  } else if (!strcmp(t, "answer.done")) {
    LOG_INF("AVA", "answer.done answerLen=%u", (unsigned)answer_.length());
    state_ = State::Idle;
    status_ = "Press Up to talk  |  Down to exit";
    dirty_ = false;
    lastRenderMs_ = 0;
    requestUpdate(true);  // one clean full-ish refresh at the end
  } else if (!strcmp(t, "error")) {
    LOG_ERR("AVA", "server error: %s", static_cast<const char*>(doc["message"] | ""));
    state_ = State::Idle;  // don't strand the user in "Thinking..."
    status_ = std::string("Error: ") + static_cast<const char*>(doc["message"] | "");
    requestUpdate(true);
  }
}

void AgentVoiceActivity::render(RenderLock&&) {
  // VERIFY every renderer/UITheme call + font id + Rect + refresh enum against the
  // local headers (GfxRenderer.h, components/UITheme.h, fontIds.h) on first build.
  renderer.clearScreen();
  int y = 8;
  renderer.drawText(UI_10_FONT_ID, 8, y, status_.c_str());
  y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
  if (!transcript_.empty()) {
    const std::string you = "You: " + transcript_;
    renderer.drawText(UI_10_FONT_ID, 8, y, you.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
  }
  if (!answer_.empty()) {
    Rect area{8, y, 800 - 16, 480 - y - 8};  // Sticky panel is 800x480
    UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, answer_.c_str(), 24);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
