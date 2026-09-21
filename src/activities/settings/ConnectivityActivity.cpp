#include "ConnectivityActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "ble/VoiceRelayPeripheral.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr StrId TAB_NAME_IDS[] = {StrId::STR_BLUETOOTH, StrId::STR_WIFI};

constexpr StrId BT_ROW_NAME_IDS[] = {StrId::STR_PHONE, StrId::STR_LINK_STATE, StrId::STR_PAIR_NEW_PHONE,
                                     StrId::STR_FORGET_PHONE};

constexpr StrId WIFI_ROW_NAME_IDS[] = {StrId::STR_WIFI_NETWORK, StrId::STR_WIFI_SIGNAL, StrId::STR_WIFI_CHOOSE,
                                       StrId::STR_WIFI_RECONNECT};

// How long a deliberately-opened pairing window stays open. Long enough to pick
// the device up and drive the phone, short enough that a device left on a shelf
// is not indefinitely willing to bond with a stranger.
constexpr uint32_t kPairingWindowMs = 120000;

constexpr uint32_t kNoteMs = 4000;
constexpr uint32_t kPollMs = 1000;

}  // namespace

ConnectivityActivity::ConnectivityActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Tab initialTab)
    : UiTabListActivity("Connectivity", renderer, mappedInput), tab_(initialTab) {}

const char* ConnectivityActivity::tabLabel(const int index) const { return I18N.get(TAB_NAME_IDS[index]); }

void ConnectivityActivity::onEnter() {
  UiTabListActivity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;

  rebuildRowItems();
  requestUpdate(true);
}

int ConnectivityActivity::listCount() const {
  return tab_ == Tab::Bluetooth ? static_cast<int>(BtRow::Count) : static_cast<int>(WifiRow::Count);
}

void ConnectivityActivity::rebuildRowItems() {
  const int count = listCount();
  rowValues_.assign(count, std::string());
  rowItems_.clear();
  rowItems_.reserve(count);
  for (int i = 0; i < count; i++) {
    fui::ListItem item;
    item.label = I18N.get(tab_ == Tab::Bluetooth ? BT_ROW_NAME_IDS[i] : WIFI_ROW_NAME_IDS[i]);
    item.actionValue = static_cast<int16_t>(i);
    // The first two rows of each tab report state; only the rest do anything.
    item.enabled = tab_ == Tab::Bluetooth ? (i >= static_cast<int>(BtRow::Pair))
                                          : (i >= static_cast<int>(WifiRow::Choose));
    rowItems_.push_back(item);
  }
}

void ConnectivityActivity::switchTab(const int direction) {
  const bool onTabBar = ringPos() == 0;
  constexpr int count = static_cast<int>(Tab::Count);
  tab_ = static_cast<Tab>((static_cast<int>(tab_) + direction + count) % count);
  rebuildRowItems();
  auto& n = activeNav();
  if (onTabBar) n.selected = 0;
  n.followOnBuild = true;
  requestUpdate();
}

void ConnectivityActivity::onTabAction(const int index) {
  if (index == static_cast<int>(tab_)) return;
  switchTab(index - static_cast<int>(tab_));
}

std::string ConnectivityActivity::btValueText(const int row) const {
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
      return ble.isPairingWindowOpen() ? tr(STR_PAIRING_OPEN) : tr(STR_PAIRING_CLOSED);
    case BtRow::Forget:
      return ble.isBonded() || ble.bondCount() > 0 ? "" : tr(STR_PHONE_NOT_LINKED);
    default:
      return "";
  }
}

std::string ConnectivityActivity::wifiValueText(const int row) const {
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

void ConnectivityActivity::openPairingWindow() {
  if (!VoiceRelayPeripheral::supported()) return;
  auto& ble = VoiceRelayPeripheral::instance();
  const bool open = !ble.isPairingWindowOpen();
  ble.setPairingWindow(open);
  pairingOpenedMs_ = open ? millis() : 0;
  LOG_INF("CONN", "pairing window %s", open ? "opened" : "closed");
  requestUpdate();
}

void ConnectivityActivity::forgetPhone() {
  if (!VoiceRelayPeripheral::supported()) return;
  VoiceRelayPeripheral::instance().forgetBonds();
  // Say so explicitly: forgetting a phone that is not in the room changes
  // nothing visible, and silence reads as the button having done nothing.
  note_ = tr(STR_FORGET_PHONE_DONE);
  noteUntilMs_ = millis() + kNoteMs;
  requestUpdate();
}

void ConnectivityActivity::activateIndex(const int index) {
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

bool ConnectivityActivity::handleButtons() {
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

void ConnectivityActivity::loop() {
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
    const std::string next = tab_ == Tab::Bluetooth ? btValueText(i) : wifiValueText(i);
    if (next != rowValues_[i]) {
      requestUpdate();
      return;
    }
  }
}

void ConnectivityActivity::buildScreen(UiScreen& screen) {
  const int noteHeight = note_.empty() ? 0 : renderer.getTextHeight(UI_10_FONT_ID) + metrics_.verticalSpacing;
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(afterHeader), 0,
                                               static_cast<int16_t>(bottomReserved + noteHeight), 0});

  buildTabBar(screen);

  const int count = listCount();
  for (int i = 0; i < count; i++) {
    rowValues_[i] = tab_ == Tab::Bluetooth ? btValueText(i) : wifiValueText(i);
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

void ConnectivityActivity::render(RenderLock&&) {
  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, renderer.getScreenWidth(), metrics_.headerHeight},
                 tr(STR_CONNECTIVITY));

  renderUi();

  if (!note_.empty()) {
    const int y = renderer.getScreenHeight() - bottomReserved;
    renderer.drawText(UI_10_FONT_ID, metrics_.topPadding, y, note_.c_str());
  }
}
