#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"

// The voice screen's swipe-down menu (HomeLab-iv0): Text, Bluetooth and Wi-Fi as
// tabs of the reader's tab-list idiom, so the conversation can be adjusted and a
// phone paired or forgotten without a serial cable.
//
// The Text tab carries the handful of controls that change how a CONVERSATION
// reads — size, line spacing, margin — and hands off to the reader's full text
// settings for the rest. Everything here costs a re-flow of the session on the
// way out, so the short list is deliberate.
//
// The Bluetooth tab is the only way to reach VoiceRelayPeripheral's pairing
// controls. Until it existed, pairing was implicit (the first phone to arrive
// won) and forgetting was CMD:BLEFORGET over USB, which is no use to someone
// holding the device.
//
// Rows are rebuilt on entry and their VALUES refresh every render, because both
// halves of this screen are live: a phone links or drops while the menu is open,
// and Wi-Fi association finishes behind it.
class VoiceMenuActivity final : public UiTabListActivity {
 public:
  enum class Tab : uint8_t { Text, Bluetooth, Wifi, Count };

  VoiceMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Tab initialTab = Tab::Text);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class TextRow : uint8_t { Size, LineSpacing, Margin, AllSettings, Count };
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
  bool handleCustomInput() override;

  void switchTab(int direction);
  void rebuildRowItems();

  const StrId* rowNameIds() const;
  std::string valueTextFor(int row) const;
  std::string textValueText(int row) const;
  std::string btValueText(int row) const;
  std::string wifiValueText(int row) const;

  void confirmTextRow(int row);
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
  // Whether THIS screen started the peripheral, and so owes it an end().
  bool startedPeripheral_ = false;
  // Value pickers, same component the reader's layout rows use.
  OptionPopup optionPopup_;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
};
