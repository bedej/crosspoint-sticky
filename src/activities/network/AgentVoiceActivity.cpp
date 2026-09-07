#include "AgentVoiceActivity.h"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "WifiCredentialStore.h"
#include "components/UITheme.h"  // VERIFY include path (drawCenteredWrappedText)
#include "fontIds.h"             // VERIFY include path (UI_10_FONT_ID)

// ---- Endpoint config -------------------------------------------------------
// SET THESE for your homelab. The token authorizes the device on the WS handshake
// (VOICE_TOKEN in /opt/apps/voice/.env on blackbox). DO NOT commit the real token
// to this (public) fork — this is a placeholder; prefer loading it from an SD
// config file (like wifi.json) in a follow-up. See VOICE.md.
static constexpr char VOICE_HOST[] = "192.168.1.85";
static constexpr uint16_t VOICE_PORT = 18092;
static constexpr char VOICE_TOKEN[] = "REPLACE_WITH_VOICE_TOKEN";

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

void AgentVoiceActivity::onEnter() {
  Activity::onEnter();
  g_voiceInstance = this;

  connectWifi();
  if (WiFi.status() != WL_CONNECTED) {
    state_ = State::Error;
    status_ = "Wi-Fi failed. Set it up in File Transfer / Wi-Fi Networks.";
    requestUpdate();
    return;
  }
  if (!mic_.begin(16000)) {
    state_ = State::Error;
    status_ = "Mic init failed";
    requestUpdate();
    return;
  }

  const String path = String("/v1/stream?token=") + VOICE_TOKEN;
  ws_.begin(VOICE_HOST, VOICE_PORT, path.c_str());
  ws_.onEvent(wsTrampoline);
  ws_.setReconnectInterval(3000);

  state_ = State::Idle;
  status_ = "Press AI-Voice to talk";
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
  if (n > 0 && wsConnected_) {
    ws_.sendBIN(reinterpret_cast<uint8_t*>(micBuf_), static_cast<size_t>(n) * sizeof(int16_t));
  }
}

void AgentVoiceActivity::startListening() {
  transcript_.clear();
  answer_.clear();
  state_ = State::Listening;
  status_ = "Listening... (press again to stop)";
  markDirty();
}

void AgentVoiceActivity::stopListening() {
  ws_.sendTXT("{\"type\":\"end\"}");
  state_ = State::Answering;
  status_ = "Thinking...";
  markDirty();
}

void AgentVoiceActivity::loop() {
  ws_.loop();
  if (state_ == State::Listening) pumpMic();

  // AI-Voice = the Power button (short press). A long hold sleeps the device
  // globally (main.cpp) and never reaches here, so there's no power/talk ambiguity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    if (state_ == State::Idle || state_ == State::Answering) {
      startListening();
    } else if (state_ == State::Listening) {
      stopListening();
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
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

void AgentVoiceActivity::onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected_ = true;
      break;
    case WStype_DISCONNECTED:
    case WStype_ERROR:
      wsConnected_ = false;
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
  if (!strcmp(t, "partial") || !strcmp(t, "transcript")) {
    transcript_ = static_cast<const char*>(doc["text"] | "");
    markDirty();
  } else if (!strcmp(t, "answer.delta")) {
    answer_ += static_cast<const char*>(doc["text"] | "");
    markDirty();
  } else if (!strcmp(t, "answer.done")) {
    state_ = State::Idle;
    status_ = "Press AI-Voice to talk";
    dirty_ = false;
    lastRenderMs_ = 0;
    requestUpdate(true);  // one clean full-ish refresh at the end
  } else if (!strcmp(t, "error")) {
    status_ = std::string("error: ") + static_cast<const char*>(doc["message"] | "");
    markDirty();
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
