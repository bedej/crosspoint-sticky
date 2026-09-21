#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiTabListActivity.h"
#include "components/themes/BaseTheme.h"

// Connectivity for the voice screen (HomeLab-iv0): Bluetooth and Wi-Fi as two
// tabs of the reader's tab-list idiom, so the device can pair and forget a
// phone without a serial cable.
//
// The Bluetooth tab is the only way to reach VoiceRelayPeripheral's pairing
// controls. Until it existed, pairing was implicit (the first phone to arrive
// won) and forgetting was CMD:BLEFORGET over USB, which is no use to someone
// holding the device.
//
// Rows are rebuilt on entry and their VALUES refresh every render, because both
// halves of this screen are live: a phone links or drops while the menu is open,
// and Wi-Fi association finishes behind it.
class ConnectivityActivity final : public UiTabListActivity {
 public:
  enum class Tab : uint8_t { Bluetooth, Wifi, Count };

  ConnectivityActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Tab initialTab = Tab::Bluetooth);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class BtRow : uint8_t { Phone, LinkState, Pair, Forget, Count };
  enum class WifiRow : uint8_t { Network, Signal, Choose, Reconnect, Count };

  // --- UiTabListActivity contract ---
  int listCount() const override;
  int tabCount() const override { return static_cast<int>(Tab::Count); }
  int activeTab() const override { return static_cast<int>(tab_); }
  const char* tabLabel(int index) const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override { switchTab(direction); }
  bool handleButtons() override;

  void switchTab(int direction);
  void rebuildRowItems();

  std::string btValueText(int row) const;
  std::string wifiValueText(int row) const;

  void openPairingWindow();
  void forgetPhone();

  Tab tab_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::vector<std::string> rowValues_;
  // Note shown under the list after an action whose effect is otherwise
  // invisible — forgetting a phone that is not currently connected changes
  // nothing on screen.
  std::string note_;
  uint32_t noteUntilMs_ = 0;
  // Values are re-read on a timer rather than every loop: reading Wi-Fi RSSI
  // and the BLE bond state is cheap but not free, and e-paper cannot show a
  // faster refresh anyway.
  uint32_t lastPollMs_ = 0;
  // When the user opened the pairing window, so it can be closed again if they
  // put the device down and walk off.
  uint32_t pairingOpenedMs_ = 0;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
};
