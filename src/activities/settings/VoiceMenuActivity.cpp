#include "VoiceMenuActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/TextSettingsActivity.h"
#include "ble/VoiceRelayPeripheral.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr StrId TAB_NAME_IDS[] = {StrId::STR_TEXT, StrId::STR_BLUETOOTH, StrId::STR_WIFI};

constexpr StrId TEXT_ROW_NAME_IDS[] = {StrId::STR_SIZE, StrId::STR_LINE_SPACING, StrId::STR_SCREEN_MARGIN,
                                       StrId::STR_ALL_TEXT_SETTINGS};

constexpr StrId LINE_SPACING_IDS[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};

constexpr int MARGIN_MIN = CrossPointSettings::SCREEN_MARGIN_MIN;
constexpr int MARGIN_MAX = CrossPointSettings::SCREEN_MARGIN_MAX;
constexpr int MARGIN_STEP = CrossPointSettings::SCREEN_MARGIN_STEP;

constexpr StrId BT_ROW_NAME_IDS[] = {StrId::STR_PHONE, StrId::STR_LINK_STATE, StrId::STR_PAIR_NEW_PHONE,
                                     StrId::STR_FORGET_PHONE};

constexpr StrId WIFI_ROW_NAME_IDS[] = {StrId::STR_WIFI_NETWORK, StrId::STR_WIFI_SIGNAL, StrId::STR_WIFI_CHOOSE,
                                       StrId::STR_WIFI_RECONNECT};

// How long a deliberately-opened pairing window stays open. Long enough to pick
// the device up and drive the phone, short enough that a device left on a shelf
// is not indefinitely willing to bond with a stranger.
constexpr uint32_t kPairingWindowMs = 120000;

constexpr uint32_t kNoteMs = 4000;
// The forget note is an instruction, not a confirmation, so it outlasts the rest.
constexpr uint32_t kForgetNoteMs = 20000;
constexpr uint32_t kPollMs = 1000;

}  // namespace

VoiceMenuActivity::VoiceMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Tab initialTab)
    : UiTabListActivity("Connectivity", renderer, mappedInput), tab_(initialTab) {}

const char* VoiceMenuActivity::tabLabel(const int index) const { return I18N.get(TAB_NAME_IDS[index]); }

void VoiceMenuActivity::onEnter() {
  UiTabListActivity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;

  // The voice screen stops the peripheral when it exits, so arriving here from
  // anywhere else would show a Bluetooth tab with the radio off: no link to
  // report, and pairing impossible because nothing is advertising. Start it if
  // nobody else has, and hand it back on the way out.
  if (VoiceRelayPeripheral::supported() && !VoiceRelayPeripheral::instance().isRunning()) {
    startedPeripheral_ = VoiceRelayPeripheral::instance().begin("Sticky");
    LOG_INF("CONN", "started the BLE peripheral for this screen (ok=%d)", (int)startedPeripheral_);
  }

  rebuildRowItems();
  requestUpdate(true);
}

void VoiceMenuActivity::onExit() {
  if (startedPeripheral_) {
    VoiceRelayPeripheral::instance().end();
    startedPeripheral_ = false;
  }
  Activity::onExit();
}

int VoiceMenuActivity::listCount() const {
  switch (tab_) {
    case Tab::Text:
      return static_cast<int>(TextRow::Count);
    case Tab::Bluetooth:
      return static_cast<int>(BtRow::Count);
    default:
      return static_cast<int>(WifiRow::Count);
  }
}

const StrId* VoiceMenuActivity::rowNameIds() const {
  switch (tab_) {
    case Tab::Text:
      return TEXT_ROW_NAME_IDS;
    case Tab::Bluetooth:
      return BT_ROW_NAME_IDS;
    default:
      return WIFI_ROW_NAME_IDS;
  }
}

void VoiceMenuActivity::rebuildRowItems() {
  const int count = listCount();
  rowValues_.assign(count, std::string());
  rowItems_.clear();
  rowItems_.reserve(count);
  for (int i = 0; i < count; i++) {
    fui::ListItem item;
    item.label = I18N.get(rowNameIds()[i]);
    item.actionValue = static_cast<int16_t>(i);
    // The first two rows of each tab report state; only the rest do anything.
    // Every Text row does something; the first two rows of the other tabs only
    // report state.
    item.enabled = tab_ == Tab::Text || (tab_ == Tab::Bluetooth ? (i >= static_cast<int>(BtRow::Pair))
                                                                : (i >= static_cast<int>(WifiRow::Choose)));
    // Nothing to forget when no phone has ever paired, and an implicitly-open
    // pairing window is exactly that state. Offering the row invites a press
    // that cannot do anything.
    if (tab_ == Tab::Bluetooth && static_cast<BtRow>(i) == BtRow::Forget) {
      item.enabled = VoiceRelayPeripheral::supported() && !VoiceRelayPeripheral::instance().isPairingWindowOpen();
    }
    rowItems_.push_back(item);
  }
}

void VoiceMenuActivity::switchTab(const int direction) {
  const bool onTabBar = ringPos() == 0;
  constexpr int count = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + count) % count);
  rebuildRowItems();
  auto& n = activeNav();
  if (onTabBar) n.selected = 0;
  n.followOnBuild = true;
  requestUpdate();
}

void VoiceMenuActivity::onTabAction(const int index) {
  if (index == static_cast<int>(tab_)) return;
  switchTab(index - static_cast<int>(tab_));
}

std::string VoiceMenuActivity::textValueText(const int row) const {
  switch (static_cast<TextRow>(row)) {
    case TextRow::Size: {
      const std::vector<uint8_t> points = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      char buf[12];
      // "pt" is the typographic unit symbol, written the same way in every
      // language CrossPoint ships — same reasoning as the reader's size list.
      snprintf(buf, sizeof(buf), "%u pt",
               static_cast<unsigned>(snapToNearestPointSize(points, SETTINGS.fontPointSize)));
      return buf;
    }
    case TextRow::LineSpacing: {
      const int idx = std::clamp<int>(SETTINGS.lineSpacing, 0, static_cast<int>(std::size(LINE_SPACING_IDS)) - 1);
      return I18N.get(LINE_SPACING_IDS[idx]);
    }
    case TextRow::Margin:
      return std::to_string(static_cast<int>(SETTINGS.screenMargin));
    default:
      return "";
  }
}

void VoiceMenuActivity::confirmTextRow(const int row) {
  switch (static_cast<TextRow>(row)) {
    case TextRow::Size: {
      const std::vector<uint8_t> points = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      std::vector<std::string> options;
      options.reserve(points.size());
      int cur = 0;
      const uint8_t selected = snapToNearestPointSize(points, SETTINGS.fontPointSize);
      for (const uint8_t pt : points) {
        if (pt == selected) cur = static_cast<int>(options.size());
        options.push_back(std::to_string(pt) + " pt");
      }
      optionPopup_.show(StrId::STR_SIZE, options, cur, [points](int idx) {
        if (idx < 0 || idx >= static_cast<int>(points.size())) return;
        SETTINGS.fontPointSize = points[static_cast<size_t>(idx)];
        SETTINGS.saveToFile();
      });
      requestUpdate();
      break;
    }
    case TextRow::LineSpacing:
      optionPopup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        SETTINGS.lineSpacing, [](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case TextRow::Margin: {
      std::vector<std::string> options;
      options.reserve((MARGIN_MAX - MARGIN_MIN) / MARGIN_STEP + 1);
      for (int m = MARGIN_MIN; m <= MARGIN_MAX; m += MARGIN_STEP) options.push_back(std::to_string(m));
      const int cur = (std::clamp<int>(SETTINGS.screenMargin, MARGIN_MIN, MARGIN_MAX) - MARGIN_MIN) / MARGIN_STEP;
      optionPopup_.show(StrId::STR_SCREEN_MARGIN, options, cur, [](int idx) {
        SETTINGS.screenMargin = static_cast<uint8_t>(MARGIN_MIN + idx * MARGIN_STEP);
        SETTINGS.saveToFile();
      });
      requestUpdate();
      break;
    }
    case TextRow::AllSettings:
      // Font family, alignment, hyphenation and the rest: the reader's own
      // screen rather than a second copy of it here.
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) { requestUpdate(true); });
      break;
    default:
      break;
  }
}

std::string VoiceMenuActivity::btValueText(const int row) const {
  if (!VoiceRelayPeripheral::supported()) return tr(STR_BT_UNAVAILABLE);
  const auto& ble = VoiceRelayPeripheral::instance();
  switch (static_cast<BtRow>(row)) {
    case BtRow::Phone:
      if (ble.isBonded()) return ble.isStreaming() ? tr(STR_CARRYING_TURNS) : tr(STR_PHONE_LINKED);
      // Connected but not bonded is worth distinguishing from absent: it is the
      // state a phone sits in while pairing, and the state it is stuck in when
      // pairing has failed.
      return ble.isConnected() ? tr(STR_PHONE_CONNECTED_UNPAIRED) : tr(STR_PHONE_NOT_LINKED);
    case BtRow::LinkState:
      // Whatever the phone last told us about its own end — the only view the
      // device has of the half of the link it cannot see.
      return ble.linkState();
    case BtRow::Pair:
      // Imperative, not a state word: every other row's value answers "what is
      // this", and "Closed" answers a question this row does not ask.
      // Three states, not two: a window this screen opened, a device that has
      // never paired and is therefore open to its first phone anyway, and the
      // ordinary closed case. Values stay short — a sentence here wraps the
      // label onto two lines.
      if (pairingOpenedMs_ != 0) return tr(STR_PAIRING_OPEN);
      return ble.isPairingWindowOpen() ? tr(STR_PAIRING_READY) : tr(STR_PAIR_TAP_TO_OPEN);
    case BtRow::Forget:
      // Deliberately blank. bondCount() reaches into the NimBLE host, and this
      // runs on the render path with the link live.
      return "";
    default:
      return "";
  }
}

std::string VoiceMenuActivity::valueTextFor(const int row) const {
  switch (tab_) {
    case Tab::Text:
      return textValueText(row);
    case Tab::Bluetooth:
      return btValueText(row);
    default:
      return wifiValueText(row);
  }
}

std::string VoiceMenuActivity::wifiValueText(const int row) const {
  const bool up = WiFi.status() == WL_CONNECTED;
  switch (static_cast<WifiRow>(row)) {
    case WifiRow::Network:
      return up ? std::string(WiFi.SSID().c_str()) : std::string(tr(STR_WIFI_DISCONNECTED));
    case WifiRow::Signal: {
      if (!up) return "";
      char buf[16];
      snprintf(buf, sizeof(buf), "%d dBm", static_cast<int>(WiFi.RSSI()));
      return buf;
    }
    default:
      return "";
  }
}

void VoiceMenuActivity::openPairingWindow() {
  if (!VoiceRelayPeripheral::supported()) return;
  auto& ble = VoiceRelayPeripheral::instance();
  // Toggle what THIS screen opened, not isPairingWindowOpen(): that reports
  // true whenever no phone is linked at all, so a first press would have
  // "closed" a window nobody opened and left the row reading Open regardless.
  const bool open = pairingOpenedMs_ == 0;
  ble.setPairingWindow(open);
  pairingOpenedMs_ = open ? millis() : 0;
  LOG_INF("CONN", "pairing window %s", open ? "opened" : "closed");
  requestUpdate();
}

void VoiceMenuActivity::forgetPhone() {
  if (!VoiceRelayPeripheral::supported()) return;
  VoiceRelayPeripheral::instance().forgetBonds();
  // Say so explicitly: forgetting a phone that is not in the room changes
  // nothing visible, and silence reads as the button having done nothing.
  // Forgetting is two-sided and the phone's half is not ours to clear: iOS has
  // no API for it, so a phone that keeps its keys will try to encrypt with a key
  // this device no longer has, fail silently, and never pair again. Say so, and
  // leave it up long enough to act on.
  note_ = tr(STR_FORGET_PHONE_DONE);
  noteUntilMs_ = millis() + kForgetNoteMs;
  requestUpdate();
}

void VoiceMenuActivity::activateIndex(const int index) {
  if (tab_ == Tab::Text) {
    confirmTextRow(index);
    return;
  }
  if (tab_ == Tab::Bluetooth) {
    switch (static_cast<BtRow>(index)) {
      case BtRow::Pair:
        openPairingWindow();
        break;
      case BtRow::Forget:
        forgetPhone();
        break;
      default:
        break;  // status rows
    }
    return;
  }

  switch (static_cast<WifiRow>(index)) {
    case WifiRow::Choose:
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(true); });
      break;
    case WifiRow::Reconnect:
      WiFi.reconnect();
      requestUpdate();
      break;
    default:
      break;
  }
}

bool VoiceMenuActivity::handleCustomInput() { return optionPopup_.isActive(); }

bool VoiceMenuActivity::handleButtons() {
  if (optionPopup_.isActive()) return false;  // the popup owns input while it is up
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      switchTab(1);
    } else {
      activateIndex(ringPos() - 1);
    }
    return true;
  }
  return false;
}

void VoiceMenuActivity::loop() {
  UiListActivity::loop();

  const uint32_t now = millis();
  if (noteUntilMs_ != 0 && now > noteUntilMs_) {
    noteUntilMs_ = 0;
    note_.clear();
    requestUpdate();
  }
  // Close a pairing window the user opened and then walked away from.
  if (VoiceRelayPeripheral::supported()) {
    auto& ble = VoiceRelayPeripheral::instance();
    if (ble.isPairingWindowOpen() && pairingOpenedMs_ != 0 && now - pairingOpenedMs_ > kPairingWindowMs) {
      pairingOpenedMs_ = 0;
      ble.setPairingWindow(false);
      requestUpdate();
    }
  }
  if (now - lastPollMs_ < kPollMs) return;
  lastPollMs_ = now;
  // Both halves of this screen change underneath it: a phone links or drops,
  // Wi-Fi finishes associating. Repaint only when a value actually differs.
  for (int i = 0; i < listCount(); i++) {
    const std::string next = valueTextFor(i);
    if (next != rowValues_[i]) {
      requestUpdate();
      return;
    }
  }
}

void VoiceMenuActivity::buildScreen(UiScreen& screen) {
  const int noteHeight = note_.empty() ? 0 : renderer.getTextHeight(UI_10_FONT_ID) + metrics_.verticalSpacing;
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(afterHeader), 0, static_cast<int16_t>(bottomReserved + noteHeight), 0});

  buildTabBar(screen);

  const int count = listCount();
  for (int i = 0; i < count; i++) {
    rowValues_[i] = valueTextFor(i);
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

void VoiceMenuActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;  // picker draws over everything

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, renderer.getScreenWidth(), metrics_.headerHeight},
                 tr(STR_VOICE_MENU));

  renderUi();

  if (!note_.empty()) {
    const int y = renderer.getScreenHeight() - bottomReserved;
    renderer.drawText(UI_10_FONT_ID, metrics_.topPadding, y, note_.c_str());
  }
}
