#pragma once
// ConversationPickerActivity — choose a conversation, start a new one, or
// delete one.
//
// A full-screen list rather than an overlay, unlike the query rail: this is a
// destination you go to, not a strip you glance at. Rides UiListActivity for
// scrolling, touch routing, subtitles and long-press, and escapes by gesture on
// boards with no Back button (see UiListActivity::wasDismissRequested).

#include <string>
#include <vector>

#include "ConversationSpool.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class ConversationPickerActivity final : public UiListActivity {
 public:
  // The empty string is returned when the user asks for a NEW conversation; any
  // other value is a session id. Cancelling returns isCancelled instead.
  ConversationPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             std::string currentSessionId);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  // Row 0 is always "New conversation"; sessions follow, newest first.
  static constexpr int kNewRow = 0;

  int listCount() const override { return static_cast<int>(rowItems_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;

  void reload();
  void rebuildRows();
  void showDeleteConfirmation(int index);
  void deleteSelected();

  std::string currentSessionId_;
  std::vector<ConversationSpool::Summary> sessions_;  // index 0 == row 1
  // ListItem holds const char*, so the strings must outlive it.
  std::vector<std::string> labels_;
  std::vector<std::string> subtitles_;
  std::vector<freeink::ui::ListItem> rowItems_;

  bool confirmingDelete_ = false;
  int pendingDeleteRow_ = -1;
  OptionPopup confirmPopup_;
};
