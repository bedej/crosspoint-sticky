#include "AgentVoiceActivity.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_system.h>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/settings/VoiceMenuActivity.h"
#include "activities/transcript/ConversationPickerActivity.h"
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

void AgentVoiceActivity::startWifi() {
  // Begins the association and returns. Nothing on this screen needs the
  // network — the spool is on the card and the document renders from it — so
  // making the user watch a "Connecting Wi-Fi..." screen for four seconds (and
  // up to fifteen on a bad day) bought nothing.
  WIFI_STORE.loadFromFile();
  std::string ssid = WIFI_STORE.getLastConnectedSsid();
  auto cred = WIFI_STORE.findCredential(ssid);  // std::optional<WifiCredential>

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  if (cred.has_value()) {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  } else if (ssid.length()) {
    WiFi.begin(ssid.c_str());
  } else {
    LOG_ERR("AVA", "no saved wifi credentials");
  }
  wifiStartedAt_ = millis();
  wsStarted_ = false;
  view_.setLink(TranscriptView::Link::Connecting);
}

// Bluetooth first, Wi-Fi fallback — Bede's choice, and the right one: a phone
// in a pocket reaches the backend from places the house network does not.
// "Ready" for BLE means a central has actually SUBSCRIBED, not merely connected;
// frames sent before that are dropped by the stack.
AgentVoiceActivity::Transport AgentVoiceActivity::chooseTransport() const {
  if (VoiceRelayPeripheral::supported() && VoiceRelayPeripheral::instance().isStreaming()) return Transport::Ble;
  if (wsConnected_) return Transport::Wifi;
  return Transport::None;
}

void AgentVoiceActivity::updateTransportIndicator() {
  // While a turn is in flight the indicator shows the LATCHED transport, not
  // live availability: a phone that links mid-utterance must not make the screen
  // claim the turn is going somewhere it is not. At rest it shows what WOULD
  // carry a turn, so a phone linking while idle is visible before you speak.
  Transport shown = turnTransport_;
  if (shown == Transport::None) shown = chooseTransport();
  view_.setTransport(shown == Transport::Ble ? TranscriptView::Transport::Ble : TranscriptView::Transport::Wifi);
}

bool AgentVoiceActivity::sendAudio(const int16_t* pcm, const size_t count) {
  if (turnTransport_ == Transport::Ble) {
    // Compressed 4:1 before it goes out: the link has far less headroom than
    // the socket, and dropping frames is worse for ASR than compressing them.
    const size_t coded = encoder_.encode(pcm, count, coded_);
    if (coded == 0) return false;
    if (!VoiceRelayPeripheral::instance().sendAudioFrame(coded_, coded)) return false;
    bytesSent_ += static_cast<uint32_t>(coded);
    return true;
  }
  if (turnTransport_ == Transport::Wifi && wsConnected_) {
    ws_.sendBIN(reinterpret_cast<const uint8_t*>(pcm), count * sizeof(int16_t));
    bytesSent_ += static_cast<uint32_t>(count) * sizeof(int16_t);
    return true;
  }
  return false;
}

// The phone forwards the backend's own JSON verbatim, so this goes through the
// same handler the WebSocket path uses — one parser, one set of semantics.
void AgentVoiceActivity::pumpBleAnswers() {
  if (!VoiceRelayPeripheral::supported()) return;
  std::string json;
  while (VoiceRelayPeripheral::instance().popAnswer(json)) {
    lastServerMs_ = millis();
    // Logged HERE rather than in handleMessage, which both transports share and
    // whose own line says "ws rx" whichever one delivered the frame. Without
    // this, the log claims the socket carried every answer even when the phone
    // did — the same lie "ws=1" told about the audio direction.
    LOG_DBG("AVA", "ble rx %u bytes", static_cast<unsigned>(json.size()));
    handleMessage(json.c_str(), json.size());
  }
}

// Association progress, watched from the loop instead of waited on.
void AgentVoiceActivity::pumpLink() {
  if (state_ == State::Error) return;
  // Cheap: setTransport() only marks a repaint when the value actually changes.
  updateTransportIndicator();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wsStarted_) {
      LOG_INF("AVA", "wifi ok ip=%s rssi=%d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      // client= lets the server tell a BLE turn's two sessions apart: the
      // phone carries the audio as client=phone-relay while this socket sits
      // idle. Without it an idle session logs as samples=0 and reads as the
      // transport having failed.
      const std::string path = "/v1/stream?token=" + token_ + "&client=sticky";
      LOG_INF("AVA", "ws connecting %s:%u/v1/stream", host_.c_str(), (unsigned)port_);
      ws_.begin(host_.c_str(), port_, path.c_str());
      ws_.onEvent(wsTrampoline);
      ws_.setReconnectInterval(3000);
      wsStarted_ = true;
    }
    view_.setLink(wsConnected_ ? TranscriptView::Link::Online : TranscriptView::Link::WifiUp);
    return;
  }

  // Not associated. Retry periodically rather than stranding the activity —
  // the user may well be talking into the buffer while this runs.
  if (millis() - wifiStartedAt_ > kWifiTimeoutMs) {
    view_.setLink(TranscriptView::Link::Failed);
    if (millis() - wifiStartedAt_ > kWifiTimeoutMs + kWifiRetryMs) {
      LOG_ERR("AVA", "wifi still down, retrying");
      startWifi();
    }
    return;
  }
  view_.setLink(TranscriptView::Link::Connecting);
}

// --- pre-connect audio buffer ----------------------------------------------

void AgentVoiceActivity::bufferPcm(const uint8_t* data, const size_t len) {
  size_t written = 0;
  while (written < len) {
    if (segments_.empty() || segmentFill_ == kPcmSegmentBytes) {
      if (segments_.size() >= kPcmMaxSegments) {
        // The ceiling drops the NEWEST audio, never the oldest: a command's verb
        // is at the front of the utterance, so keeping the tail would be worse
        // than useless. The state is surfaced rather than swallowed.
        if (!pcmOverflowed_) {
          pcmOverflowed_ = true;
          LOG_ERR("AVA", "pre-connect buffer full at %u bytes", (unsigned)(segments_.size() * kPcmSegmentBytes));
        }
        framesDropped_++;
        return;
      }
      auto seg = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[kPcmSegmentBytes]);
      if (!seg) {
        pcmOverflowed_ = true;
        framesDropped_++;
        return;
      }
      segments_.push_back(std::move(seg));
      segmentFill_ = 0;
    }
    const size_t room = kPcmSegmentBytes - segmentFill_;
    const size_t take = (len - written) < room ? (len - written) : room;
    memcpy(segments_.back().get() + segmentFill_, data + written, take);
    segmentFill_ += take;
    written += take;
  }
}

void AgentVoiceActivity::drainPcmBuffer() {
  if (segments_.empty()) return;
  if (turnTransport_ == Transport::None) {
    // Nothing to drain TO yet. Claim a transport the moment one is ready — the
    // audio was captured before either existed, so either may carry it.
    turnTransport_ = chooseTransport();
    if (turnTransport_ == Transport::None) return;
    updateTransportIndicator();
  }
  // Sent in the same 20 ms frames the live path uses — the service reads a
  // frame stream, not a blob — and rate-limited so a drain cannot monopolise
  // the loop while the panel still has to render.
  constexpr size_t kFrameBytes = 320 * sizeof(int16_t);
  constexpr int kFramesPerPass = 8;
  for (int sent = 0; sent < kFramesPerPass && drainSegment_ < segments_.size(); ++sent) {
    const bool last = (drainSegment_ + 1 == segments_.size());
    const size_t available = (last ? segmentFill_ : kPcmSegmentBytes) - drainOffset_;
    if (available == 0) {
      drainSegment_++;
      drainOffset_ = 0;
      continue;
    }
    const size_t take = available < kFrameBytes ? available : kFrameBytes;
    // Offsets advance in whole frames, so this stays 2-byte aligned.
    const int16_t* pcm = reinterpret_cast<const int16_t*>(segments_[drainSegment_].get() + drainOffset_);
    if (!sendAudio(pcm, take / sizeof(int16_t))) break;  // retry on the next pass
    drainOffset_ += take;
  }
  if (drainSegment_ >= segments_.size()) {
    LOG_INF("AVA", "pre-connect buffer drained");
    releasePcmBuffer();
    if (pendingEnd_) {
      pendingEnd_ = false;
      sendTurnEnd();
    }
  }
}

void AgentVoiceActivity::releasePcmBuffer() {
  segments_.clear();
  segments_.shrink_to_fit();
  segmentFill_ = 0;
  drainSegment_ = 0;
  drainOffset_ = 0;
  pcmOverflowed_ = false;
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

  // The spool is the conversation. Deep sleep wakes through a reset, so the
  // page index is gone even though the text is not — rebuild it from the raw
  // records and land on the latest page.
  view_.begin();
  pagesUntilFullRefresh_ = SETTINGS.getRefreshFrequency();
  if (spool_.begin()) {
    maybeRotateSession();
    view_.restoreIndex();
    view_.jumpToLatest();
    LOG_INF("AVA", "session %s: %u turns, %u pages", spool_.sessionId().c_str(),
            static_cast<unsigned>(spool_.turnCount()), static_cast<unsigned>(spool_.pageCount()));
  } else {
    LOG_ERR("AVA", "spool unavailable; transcript will not persist");
  }

  // Advertise before Wi-Fi: a phone that is already paired can pick the link up
  // while the radio is still associating, and then carries the turn instead.
  if (VoiceRelayPeripheral::supported()) {
    bleStarted_ = VoiceRelayPeripheral::instance().begin("Sticky");
    LOG_INF("AVA", "ble peripheral %s", bleStarted_ ? "advertising" : "failed to start");
  }

  // Paint the restored conversation NOW, then bring the link up behind it.
  state_ = State::Idle;
  status_ = "Power to talk";
  requestUpdateAndWait();

  startWifi();
}

void AgentVoiceActivity::onExit() {
  if (bleStarted_) {
    VoiceRelayPeripheral::instance().end();
    bleStarted_ = false;
  }
  view_.endTurn();
  spool_.endAgentTurn();
  mic_.end();
  ws_.disconnect();
  releasePcmBuffer();
  WiFi.disconnect(false);
  g_voiceInstance = nullptr;
  Activity::onExit();
}

void AgentVoiceActivity::pumpMic() {
  const int n = mic_.read(micBuf_, 320, 20);
  if (n <= 0) return;
  // DC block + make-up gain, from the shared implementation so the BLE path
  // cannot silently ship raw PDM the backend hears as noise.
  conditioner_.process(micBuf_, static_cast<size_t>(n));

  for (int i = 0; i < n; i++) {
    const int16_t v = micBuf_[i];
    const int32_t a = v < 0 ? -static_cast<int32_t>(v) : v;
    micAbsSum_ += static_cast<uint32_t>(a);
    if (a > micPeak_) micPeak_ = static_cast<int16_t>(a);
  }
  micCount_ += static_cast<uint32_t>(n);
  // A transport that only became ready mid-utterance still claims the rest of it.
  if (turnTransport_ == Transport::None) {
    turnTransport_ = chooseTransport();
    if (turnTransport_ != Transport::None) updateTransportIndicator();
  }

  if (turnTransport_ != Transport::None && pcmBufferEmpty()) {
    if (!sendAudio(micBuf_, static_cast<size_t>(n))) framesDropped_++;
  } else {
    // No transport yet, or earlier audio is still draining. Live frames queue
    // behind it either way, or the utterance arrives out of order.
    bufferPcm(reinterpret_cast<const uint8_t*>(micBuf_), static_cast<size_t>(n) * sizeof(int16_t));
  }
}

void AgentVoiceActivity::startListening() {
  view_.clearDraft();
  // Latched for the whole turn: audio captured for one transport must not be
  // drained down another half way through an utterance.
  turnTransport_ = chooseTransport();
  updateTransportIndicator();
  if (turnTransport_ == Transport::Ble) VoiceRelayPeripheral::instance().notifyTurnStart();
  releasePcmBuffer();  // anything left from a capture that never reached the wire
  pendingEnd_ = false;
  conditioner_.reset();
  encoder_.reset();
  gotTranscript_ = false;
  pendingMarker_ = '\0';
  pendingCount_ = 0;
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

void AgentVoiceActivity::sendTurnEnd() {
  if (turnTransport_ == Transport::Ble) {
    VoiceRelayPeripheral::instance().notifyTurnStop();
    return;
  }
  ws_.sendTXT("{\"type\":\"end\"}");
}

void AgentVoiceActivity::stopListening() {
  const uint32_t avgAbs = micCount_ ? static_cast<uint32_t>(micAbsSum_ / micCount_) : 0;
  LOG_INF("AVA", "listen end: transport=%s samples=%u avgAbs=%u peak=%d bytesSent=%u dropped=%u ws=%d",
          turnTransport_ == Transport::Ble ? "ble" : "wifi", micCount_, avgAbs, (int)micPeak_, bytesSent_,
          framesDropped_, (int)wsConnected_);
  // Down the transport that carried the audio. Ending the turn on the socket
  // while BLE carried it ends the WRONG session: the phone's turn never
  // finalises (its recogniser keeps accumulating into the next utterance) and
  // the device's own empty socket session answers "I didn't catch that", which
  // then overwrites the real transcript on screen.
  //
  // ...but not before the audio it terminates has actually gone. Talking before
  // a link is up buffers the utterance, and an end marker sent now would
  // overtake frames still draining — the same out-of-order failure, just on the
  // right transport. loop() releases it once the buffer is empty and a
  // transport is ready.
  if (!pcmBufferEmpty() || turnTransport_ == Transport::None) {
    pendingEnd_ = true;
  } else {
    sendTurnEnd();
  }
  state_ = State::Answering;
  status_ = "Thinking...";
  lastServerMs_ = millis();  // arm the stall watchdog
  markDirty();
}

// Collect a page-turn intent. Priority mirrors the reader: the menu gesture
// (centre third) is claimed first so it can never double as a page turn, then
// long-press variants, then a plain turn.
// Everything TranscriptView::begin() and startParagraph() read out of SETTINGS
// to lay a conversation out. Change any of these and the existing page index
// describes a layout that is no longer true; change none of them and a re-flow
// is pure cost.
uint32_t AgentVoiceActivity::textLayoutSignature() const {
  uint32_t h = 2166136261u;  // FNV-1a, enough to notice a change
  const auto mix = [&h](const uint32_t v) {
    h = (h ^ (v & 0xff)) * 16777619u;
    h = (h ^ ((v >> 8) & 0xff)) * 16777619u;
    h = (h ^ ((v >> 16) & 0xff)) * 16777619u;
    h = (h ^ ((v >> 24) & 0xff)) * 16777619u;
  };
  mix(static_cast<uint32_t>(SETTINGS.getReaderFontId()));
  // The compression is a float; the raw setting it derives from is the integer.
  mix(static_cast<uint32_t>(SETTINGS.lineSpacing));
  mix(static_cast<uint32_t>(SETTINGS.screenMargin));
  mix(static_cast<uint32_t>(SETTINGS.fontPointSize));
  mix(static_cast<uint32_t>(SETTINGS.paragraphAlignment));
  mix(static_cast<uint32_t>(SETTINGS.extraParagraphSpacing));
  mix(static_cast<uint32_t>(SETTINGS.hyphenationEnabled));
  mix(static_cast<uint32_t>(SETTINGS.focusReadingEnabled));
  return h;
}

// Swipe down from the top (or tap the centre third) opens the voice menu:
// Text, Bluetooth and Wi-Fi. Until this was wired up the menu existed but was
// reachable only over USB with CMD:CONN, which is no use to someone holding the
// device — and the gesture still landed on the old text-settings-only screen.
//
// startActivityForResult, not goToVoiceMenu(): the latter REPLACES this
// activity, which would tear the conversation down to change a font size. This
// activity stays on the stack, so it also keeps owning the BLE peripheral and
// the menu's Bluetooth tab finds it already running.
void AgentVoiceActivity::openVoiceMenu() {
  view_.endTurn();  // never leave a half-laid-out turn behind
  spool_.endAgentTurn();
  view_.clearDraft();

  const uint32_t before = textLayoutSignature();
  LOG_INF("AVA", "voice menu: pushing (sig=%08x)", static_cast<unsigned>(before));
  startActivityForResult(std::make_unique<VoiceMenuActivity>(renderer, mappedInput, VoiceMenuActivity::Tab::Text),
                         [this, before](const ActivityResult&) {
                           // RECORD ONLY. This fires while the activity stack is mid-pop, and the
                           // re-flow it used to run here — a blocking render followed by seconds
                           // of SD I/O and layout — panicked the device on exit from Text
                           // Settings. performPendingWork() does it from loop() instead.
                           LOG_INF("AVA", "voice menu: returned (sig=%08x -> %08x)",
                                   static_cast<unsigned>(before), static_cast<unsigned>(textLayoutSignature()));
                           if (textLayoutSignature() == before) {
                             // Nothing that affects the flow moved. Repaint over the menu and
                             // leave the page index, the current page and the draft alone.
                             view_.markFullPaint();
                             requestUpdate();
                             return;
                           }
                           status_ = "Re-flowing the conversation...";
                           view_.setStatus(status_);
                           view_.markFullPaint();
                           pending_ = Pending::Reflow;
                           requestUpdate();
                         });
}

// While the rail is up it owns touch. Returns true when it consumed the input,
// so the caller does not also page.
bool AgentVoiceActivity::handleRailInput() {
  if (!view_.railOpen()) return false;

  // The same edge gesture closes it — symmetry beats having to find the outside.
  if (mappedInput.wasRightEdgeGesture()) {
    view_.closeRail();
    requestUpdate();
    return true;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    // A swipe moves nearly a screenful, keeping one row of overlap for context.
    // Stepping a single row made a long conversation unreachable by swiping, and
    // every step costs an e-ink refresh.
    const int page = view_.railPageRows();
    view_.railScroll(swipe == MappedInputManager::SwipeDir::Up ? page : -page);
    return true;
  }

  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return true;  // swallow everything else

  uint16_t turnIndex = 0;
  switch (view_.railHitTest(x, y, turnIndex)) {
    case TranscriptView::RailHit::Dismiss:
      view_.closeRail();
      requestUpdate();
      break;
    case TranscriptView::RailHit::Conversations:
      view_.noteContentChangedUnderRail();  // the picker will repaint over it anyway
      view_.closeRail();
      openConversations();
      break;
    case TranscriptView::RailHit::Turn:
      // Tell the rail the page is about to move, so closing repaints instead of
      // restoring a snapshot of the page we are leaving.
      view_.noteContentChangedUnderRail();
      view_.closeRail();
      if (view_.jumpToTurn(turnIndex)) nextIsPageTurn_ = true;
      requestUpdate();
      break;
    case TranscriptView::RailHit::None:
      break;
  }
  return true;
}

// The top bar is chrome, so a tap there must be claimed BEFORE the page-turn
// zones — those are Rect{0, 0, zoneWidth, height}, full screen HEIGHT, so
// without this the bar's outer thirds would turn pages instead.
bool AgentVoiceActivity::handleTopBarTap() {
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return false;
  if (y >= view_.topBarHitHeight()) return false;

  const int third = renderer.getScreenWidth() / 3;
  if (x < third) {
    openConversations();
  } else if (x < third * 2) {
    applyConversationChoice(std::string());  // the + starts a new conversation
  }
  // The right third belongs to the connection indicator and does nothing.
  return true;
}

void AgentVoiceActivity::openConversations() {
  view_.endTurn();
  spool_.endAgentTurn();
  view_.clearDraft();

  startActivityForResult(std::make_unique<ConversationPickerActivity>(renderer, mappedInput, spool_.sessionId()),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             view_.markFullPaint();
                             requestUpdate();
                             return;
                           }
                           // No FilePathResult means the picker was dismissed rather than chosen
                           // from — a gesture back, say. Treating that as an empty path would have
                           // read as "start a new conversation", so backing out would have created
                           // one every time.
                           const auto* choice = std::get_if<FilePathResult>(&result.data);
                           if (choice == nullptr) {
                             view_.markFullPaint();
                             requestUpdate();
                             return;
                           }
                           applyConversationChoice(choice->path);
                         });
}

void AgentVoiceActivity::applyConversationChoice(const std::string& sessionId) {
  // Recorded rather than done: this is reached from the picker's result callback
  // as well as from the top bar, and opening a conversation whose index is stale
  // rebuilds it — seconds of work that must not run during activity teardown.
  pendingSessionId_ = sessionId;
  pending_ = Pending::Session;
  status_ = "Opening conversation...";
  view_.setStatus(status_);
  view_.markFullPaint();
  requestUpdate();
}

// Runs from loop(), one iteration after the callback that asked for it — so the
// "Re-flowing..." / "Opening conversation..." message is already on the panel
// before the work starts. That is what requestUpdateAndWait() was reaching for,
// from a place it could not safely be done.
void AgentVoiceActivity::performPendingWork() {
  if (pending_ == Pending::None) return;
  const Pending work = pending_;
  pending_ = Pending::None;

  if (work == Pending::Session) {
    const bool ok = pendingSessionId_.empty() ? spool_.startNewSession() : spool_.begin(pendingSessionId_.c_str());
    if (!ok) LOG_ERR("AVA", "could not open session '%s'", pendingSessionId_.c_str());
    pendingSessionId_.clear();
  }

  // A new RenderSpec drops the page index and rejects the persisted one; the
  // spool is raw text and is untouched either way.
  view_.begin();
  view_.restoreIndex();
  view_.jumpToLatest();
  LOG_INF("AVA", "session %s: %u turns, %u pages", spool_.sessionId().c_str(),
          static_cast<unsigned>(spool_.turnCount()), static_cast<unsigned>(spool_.pageCount()));

  status_ = "Power to talk";
  nextIsPageTurn_ = true;
  requestUpdate();
}


// Checked on ENTRY only, never on a timer: rotating under someone who has just
// started speaking would lose the turn they are in the middle of, and a
// conversation that ran long because they kept talking is not a problem.
void AgentVoiceActivity::maybeRotateSession() {
  const uint32_t last = spool_.lastTurnEpoch();
  if (last == 0) return;  // no trustworthy timestamp — never guess
  uint32_t now = 0;
  if (!halClock.nowEpoch(now)) return;  // no clock, or the RTC lost time
  if (now <= last || (now - last) < kIdleNewSessionSecs) return;

  LOG_INF("AVA", "last turn %us ago, starting a new conversation", static_cast<unsigned>(now - last));
  spool_.startNewSession();
}

void AgentVoiceActivity::handlePaging() {
  if (handleRailInput()) return;

  // Both edge gestures are checked before ReaderUtils sees the swipe: an edge
  // swipe is ALSO a plain SwipeDir, and in swipe-paging mode the page turn would
  // otherwise swallow it.
  if (mappedInput.wasRightEdgeGesture()) {
    view_.openRail();
    return;
  }
  // Left edge opens the conversations. It used to turn a page back, which the
  // Up button already does — a duplicated binding on the one edge gesture a
  // person is likely to try.
  if (mappedInput.wasBackGesture()) {
    openConversations();
    return;
  }
  if (handleTopBarTap()) return;

  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openVoiceMenu();
    return;
  }

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

  if (view_.railOpen()) {
    view_.noteContentChangedUnderRail();
    view_.closeRail();
  }

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
  nextIsPageTurn_ = true;
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
  if (injectPartialPending_) {
    injectPartialPending_ = false;
    view_.setDraft(injectPartial_);
  }

  if (!injectUser_.empty()) {
    const std::string text = injectUser_;
    injectUser_.clear();
    view_.clearDraft();
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
      nextIsPageTurn_ = true;
      requestUpdate();
    }
  }

  if (sessionListRequested_) {
    sessionListRequested_ = false;
    const auto ids = ConversationSpool::listSessionIds();
    LOG_INF("AVA", "sessions: %u (current %s)", static_cast<unsigned>(ids.size()), spool_.sessionId().c_str());
    for (const std::string& id : ids) {
      ConversationSpool::Summary sum;
      ConversationSpool::readSummary(id, sum);
      LOG_INF("AVA", "  %s: %u turns, epoch %lu, '%s'", id.c_str(), static_cast<unsigned>(sum.turnCount),
              static_cast<unsigned long>(sum.lastEpoch), sum.firstQuestion.c_str());
    }
  }

  if (pickerRequested_) {
    pickerRequested_ = false;
    openConversations();
    return;  // the picker owns the screen now
  }

  if (newConversationRequested_) {
    newConversationRequested_ = false;
    applyConversationChoice(std::string());
  }

  if (railToggleRequested_) {
    railToggleRequested_ = false;
    if (view_.railOpen()) {
      view_.closeRail();
      requestUpdate();
    } else {
      view_.openRail();
      LOG_INF("AVA", "rail open");
    }
  }

  const int railRow = railRowRequested_;
  if (railRow >= 0) {
    railRowRequested_ = -1;
    // Resolve through the rail's own hit test so the script exercises the same
    // path a finger does, rather than a parallel one that could drift from it.
    uint16_t turnIndex = 0;
    if (view_.railRowTurn(static_cast<size_t>(railRow), turnIndex)) {
      view_.noteContentChangedUnderRail();
      view_.closeRail();
      LOG_INF("AVA", "rail row %d -> turn %u", railRow, static_cast<unsigned>(turnIndex));
      if (view_.jumpToTurn(turnIndex)) nextIsPageTurn_ = true;
      requestUpdate();
    } else {
      LOG_ERR("AVA", "rail row %d out of range", railRow);
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

// Button model, set by Bede after using the device:
//   Up     short = previous page    hold = previous TURN
//   Down   short = next page        hold = leave the activity
//   Power  short = start/stop listening   (hold still sleeps, in main.cpp)
//
// Power is usable here despite this activity's original claim that main.cpp
// eats it: main.cpp only consumes a HELD power button, and it sleeps the device
// while the button is still down, so a release reaching this point is always a
// short press. The Power+Down screenshot combo is likewise consumed in main.cpp
// before the activity loop runs.
//
// Returns true when the activity has finished and the caller must stop touching
// it.
bool AgentVoiceActivity::handleVoiceButtons() {
  const bool pttSerial = pttRequested_;
  pttRequested_ = false;
  // The Sticky wires OK/confirm and power/wake to the SAME GPIO4, and
  // InputManager splits them by duration: a short click is delivered as
  // CONFIRM, and only a hold past CONFIRM_POWER_HOLD_MS (400 ms) is delivered
  // as POWER. Binding talk to Power alone therefore never fired for a tap,
  // which is why the button appeared dead. A press emits exactly one of the
  // two, so accepting both cannot double-toggle: tap -> Confirm, medium hold ->
  // Power, and a hold long enough to mean sleep never reaches a release at all.
  const bool talkTap =
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      (mappedInput.wasReleased(MappedInputManager::Button::Power) && mappedInput.getHeldTime() < kPowerSleepHoldMs);
  if (pttSerial || talkTap) {
    // Talking is always about the live tail: snap forward before capturing so a
    // reply never streams onto a page the reader has paged away from.
    pendingJumpLatest_ = true;
    LOG_INF("AVA", "talk toggle (state=%d ws=%d)", (int)state_, (int)wsConnected_);
    if (state_ == State::Listening) {
      stopListening();
    } else if (state_ == State::Idle || state_ == State::Answering) {
      startListening();
    }
  }

  // wasLongPressed fires while the button is still down and suppresses the
  // release that follows, so the short-press branch cannot fire for the same
  // press and a hold never also turns a page.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Up, ReaderUtils::SKIP_HOLD_MS)) {
    pendingTurnSkip_ = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    pendingManualTurn_ = -1;
  }

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Down, ReaderUtils::GO_BACK_OR_HOME_MS)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    pendingManualTurn_ = 1;
  }
  return false;
}

void AgentVoiceActivity::loop() {
  if (menuRequested_) {
    menuRequested_ = false;
    LOG_INF("AVA", "serial: opening the voice menu");
    openVoiceMenu();
    return;
  }
  ws_.loop();
  pumpBleAnswers();
  pumpLink();
  drainPcmBuffer();
  if (pendingEnd_ && pcmBufferEmpty() && (turnTransport_ == Transport::Ble || wsConnected_)) {
    pendingEnd_ = false;
    sendTurnEnd();
  }
  if (state_ == State::Listening) pumpMic();

  // The Sticky's AI-Voice button IS CrossPoint's Power button, which main.cpp consumes
  // for sleep before an activity sees it — so push-to-talk uses the Up side button,
  // and Down exits back to the menu.
  if (handleVoiceButtons()) return;  // the activity finished

  performPendingWork();
  pumpInjectedTurns();
  handlePaging();
  applyPendingTurn();

  // Stall watchdog: if a turn goes quiet (WS dropped, or ASR/Hermes returned
  // nothing) surface an error instead of hanging on "Thinking...".
  if (state_ == State::Answering && millis() - lastServerMs_ > kStallTimeoutMs) {
    LOG_ERR("AVA", "answer stalled >%lums, resetting", kStallTimeoutMs);
    turnTransport_ = Transport::None;
    view_.clearDraft();
    state_ = State::Idle;
    status_ = "No response. Press Up to try again.";
    requestUpdate(true);
    return;
  }

  // One refresh per SETTLED LINE, never per token. A FAST refresh is 300-500 ms,
  // so a screen fills in ~6 s while reading it takes ~55 s; the layout engine
  // holds the ragged tail back and only completed lines reach the panel.
  const bool linesWaiting = view_.hasPendingAppend() || view_.needsFullPaint();
  // A settled line is final and paces itself. A draft is revised on every ASR
  // frame, far faster than the panel can settle, so it gets a slower coalesce
  // and is accepted as approximate until the transcript finalises.
  const unsigned long quiet = linesWaiting ? 250 : 400;
  if (view_.railOpen()) {
    // Repainting the page under the rail would both overdraw it and invalidate
    // the snapshot that closing restores. Keep laying out; note that the page
    // moved on so closing repaints instead.
    if (linesWaiting || view_.draftNeedsRepaint()) view_.noteContentChangedUnderRail();
  } else if ((dirty_ || linesWaiting || view_.draftNeedsRepaint()) && millis() - lastRenderMs_ > quiet) {
    dirty_ = false;
    lastRenderMs_ = millis();
    requestUpdate();
  }
}

void AgentVoiceActivity::markDirty() { dirty_ = true; }

// Resolve a run of emphasis markers that has ended. Two or more in a row is a
// Markdown marker and disappears; a lone one is a real character ("2 * 3",
// "snake_case") and survives.
void AgentVoiceActivity::flushPendingMarker() {
  if (pendingCount_ == 1 && pendingMarker_ != '\0') answer_ += pendingMarker_;
  pendingCount_ = 0;
  pendingMarker_ = '\0';
}

void AgentVoiceActivity::appendAnswerText(const char* text) {
  // Hermes answers in Markdown; e-paper has one face and no styling, so the
  // emphasis runs would otherwise render as literal **asterisks**.
  //
  // A delta can end anywhere, INCLUDING between the two characters of a '**'.
  // Judging a marker by what happens to be in the current delta therefore gets
  // it wrong at chunk boundaries and leaves both asterisks on the panel, so an
  // unfinished run is carried in pendingMarker_/pendingCount_ and resolved
  // against whatever arrives next instead.
  for (const char* p = text; *p; ++p) {
    if (*p == '`') continue;
    if (*p == '*' || *p == '_') {
      if (pendingMarker_ != '\0' && pendingMarker_ != *p) flushPendingMarker();
      pendingMarker_ = *p;
      if (pendingCount_ < 0xFF) pendingCount_++;
      continue;
    }
    flushPendingMarker();
    answer_ += *p;
  }
}

void AgentVoiceActivity::finishAnswer() {
  turnTransport_ = Transport::None;  // the next turn chooses afresh
  // A marker run still open at the end resolves now; route it through the spool
  // and the view like any other delta rather than letting it sit in answer_.
  const size_t before = answer_.length();
  flushPendingMarker();
  if (answer_.length() > before) {
    const std::string tail = answer_.substr(before);
    spool_.appendAgentDelta(tail);
    view_.appendText(tail.c_str());
  }
  // Flush the ragged tail the layout engine was holding, and close the turn so
  // the next reply starts a fresh one in the spool.
  view_.endTurn();
  spool_.endAgentTurn();
  nextIsPageTurn_ = true;
  state_ = State::Idle;
  status_ = "Power to talk";
  dirty_ = false;
  lastRenderMs_ = 0;
  requestUpdate(true);  // one clean full-ish refresh at the end
}

void AgentVoiceActivity::failTurnIfInFlight(const char* msg) {
  // A socket that closes while BLE is carrying the turn says nothing about the
  // turn: the backend closes the device's idle session on its own schedule, and
  // treating that as a failure would abandon an utterance that is still in
  // flight over Bluetooth.
  if (turnTransport_ == Transport::Ble) {
    LOG_INF("AVA", "ignoring socket close: this turn is on BLE");
    return;
  }
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
      view_.clearDraft();
      if (!transcript_.empty() && spool_.appendUserTurn(transcript_)) {
        openSpoolTurn(ConversationSpool::Role::User, transcript_.c_str());
        view_.endTurn();
      }
    }
    // The words go where they are about to live, not into a status line: the
    // partial renders as a draft in the position the finished turn will occupy,
    // and committing the real turn above replaced it in place.
    if (gotTranscript_) {
      view_.clearDraft();
    } else {
      view_.setDraft(transcript_);
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
    turnTransport_ = Transport::None;
    view_.clearDraft();
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
  view_.setListening(state_ == State::Listening);
  const bool pageTurn = nextIsPageTurn_;
  nextIsPageTurn_ = false;
  if (view_.needsFullPaint()) {
    view_.paintFull();
    if (pageTurn) {
      // The book reader's cadence, reused rather than reinvented: a page turn is
      // a FAST refresh, and every SETTINGS.getRefreshFrequency() turns one is
      // promoted to a HALF refresh to clear the residue FAST leaves behind.
      // Paying 1720 ms on every single turn, as this used to, buys nothing.
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh_);
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  } else if (view_.hasPendingAppend()) {
    view_.renderAppend();  // also picks up any draft change
  } else {
    view_.renderDraft();
  }
}
