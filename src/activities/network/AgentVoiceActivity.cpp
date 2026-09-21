#include "AgentVoiceActivity.h"

#include "activities/RenderLock.h"
#include "CrossPointSettings.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/settings/TextSettingsActivity.h"
#include "activities/transcript/ConversationPickerActivity.h"
#include <HalClock.h>
#include "SdCardFontSystem.h"

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

// Association progress, watched from the loop instead of waited on.
void AgentVoiceActivity::pumpLink() {
  if (state_ == State::Error) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wsStarted_) {
      LOG_INF("AVA", "wifi ok ip=%s rssi=%d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      const std::string path = "/v1/stream?token=" + token_;
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
  if (!wsConnected_ || segments_.empty()) return;
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
    ws_.sendBIN(segments_[drainSegment_].get() + drainOffset_, take);
    bytesSent_ += static_cast<uint32_t>(take);
    drainOffset_ += take;
  }
  if (drainSegment_ >= segments_.size()) {
    LOG_INF("AVA", "pre-connect buffer drained");
    releasePcmBuffer();
    if (pendingEnd_) {
      pendingEnd_ = false;
      ws_.sendTXT("{\"type\":\"end\"}");
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

  // Paint the restored conversation NOW, then bring the link up behind it.
  state_ = State::Idle;
  status_ = "Power to talk";
  requestUpdateAndWait();

  startWifi();
}

void AgentVoiceActivity::onExit() {
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
  if (wsConnected_ && pcmBufferEmpty()) {
    ws_.sendBIN(reinterpret_cast<uint8_t*>(micBuf_), static_cast<size_t>(n) * sizeof(int16_t));
    bytesSent_ += static_cast<uint32_t>(n) * sizeof(int16_t);
  } else {
    // Either the socket is not up yet, or earlier audio is still draining.
    // Live frames have to queue behind it either way, or the utterance reaches
    // the service out of order.
    bufferPcm(reinterpret_cast<const uint8_t*>(micBuf_), static_cast<size_t>(n) * sizeof(int16_t));
  }
}

void AgentVoiceActivity::startListening() {
  view_.clearDraft();
  releasePcmBuffer();  // anything left from a capture that never reached the wire
  pendingEnd_ = false;
  dcState_ = 0;
  dcPrimed_ = false;
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
// Swipe down from the top (or tap the centre third) opens the reader's own text
// settings, so font size and layout are adjustable from the conversation. The
// gesture, the menu and the re-flow are all existing machinery — this only
// wires them together.
void AgentVoiceActivity::openTextSettings() {
  view_.endTurn();  // never leave a half-laid-out turn behind
  spool_.endAgentTurn();
  view_.clearDraft();

  startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                               TextSettingsActivity::Tab::Size),
                         [this](const ActivityResult&) {
                           // A font, size or margin change is a new RenderSpec, which drops the
                           // page index and rejects the persisted one; the spool is raw text and
                           // is untouched. Re-laying out a long session takes real time (~9 s at
                           // 49 turns), so say so rather than looking hung.
                           status_ = "Re-flowing the conversation...";
                           view_.begin();
                           view_.setStatus(status_);
                           requestUpdateAndWait();

                           view_.restoreIndex();
                           view_.jumpToLatest();
                           status_ = "Power to talk";
                           nextIsPageTurn_ = true;
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
    view_.railScroll(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
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

  startActivityForResult(
      std::make_unique<ConversationPickerActivity>(renderer, mappedInput, spool_.sessionId()),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          view_.markFullPaint();
          requestUpdate();
          return;
        }
        const auto* choice = std::get_if<FilePathResult>(&result.data);
        applyConversationChoice(choice ? choice->path : std::string());
      });
}

void AgentVoiceActivity::applyConversationChoice(const std::string& sessionId) {
  // A conversation without a valid index rebuilds, and that is seconds of real
  // work on a long one — say so rather than looking hung.
  status_ = "Opening conversation...";
  view_.setStatus(status_);
  view_.markFullPaint();
  requestUpdateAndWait();

  const bool ok = sessionId.empty() ? spool_.startNewSession() : spool_.begin(sessionId.c_str());
  if (!ok) LOG_ERR("AVA", "could not open session '%s'", sessionId.c_str());

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

  // Before anything that consumes SwipeDir::Left, since a right-edge swipe is
  // also one of those.
  if (mappedInput.wasRightEdgeGesture()) {
    view_.openRail();
    return;
  }
  if (handleTopBarTap()) return;

  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openTextSettings();
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
  const bool talkTap = mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                       (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
                        mappedInput.getHeldTime() < kPowerSleepHoldMs);
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
  ws_.loop();
  pumpLink();
  drainPcmBuffer();
  if (pendingEnd_ && wsConnected_ && pcmBufferEmpty()) {
    pendingEnd_ = false;
    ws_.sendTXT("{\"type\":\"end\"}");
  }
  if (state_ == State::Listening) pumpMic();

  // The Sticky's AI-Voice button IS CrossPoint's Power button, which main.cpp consumes
  // for sleep before an activity sees it — so push-to-talk uses the Up side button,
  // and Down exits back to the menu.
  if (handleVoiceButtons()) return;  // the activity finished

  pumpInjectedTurns();
  handlePaging();
  applyPendingTurn();

  // Stall watchdog: if a turn goes quiet (WS dropped, or ASR/Hermes returned
  // nothing) surface an error instead of hanging on "Thinking...".
  if (state_ == State::Answering && millis() - lastServerMs_ > kStallTimeoutMs) {
    LOG_ERR("AVA", "answer stalled >%lums, resetting", kStallTimeoutMs);
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
