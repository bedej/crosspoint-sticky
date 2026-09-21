#include "ConversationPickerActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>

#include <algorithm>
#include <cstdio>

#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

// "just now" / "2 hours ago" / "yesterday". Only ever called with a real epoch;
// a session whose turns predate trustworthy timestamps shows its turn count
// alone rather than a made-up age.
void formatAge(const uint32_t epoch, const uint32_t now, char* buf, const size_t len) {
  if (epoch == 0 || now == 0 || now < epoch) {
    buf[0] = '\0';
    return;
  }
  const uint32_t secs = now - epoch;
  if (secs < 90) {
    snprintf(buf, len, "just now");
  } else if (secs < 3600) {
    snprintf(buf, len, "%u min ago", static_cast<unsigned>(secs / 60));
  } else if (secs < 86400) {
    const unsigned h = secs / 3600;
    snprintf(buf, len, h == 1 ? "1 hour ago" : "%u hours ago", h);
  } else if (secs < 172800) {
    snprintf(buf, len, "yesterday");
  } else {
    snprintf(buf, len, "%u days ago", static_cast<unsigned>(secs / 86400));
  }
}
}  // namespace

ConversationPickerActivity::ConversationPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string currentSessionId)
    : UiListActivity("Conversations", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      currentSessionId_(std::move(currentSessionId)) {}

void ConversationPickerActivity::onEnter() {
  UiListActivity::onEnter();
  reload();
  // Open on the conversation being read, not on "New".
  for (size_t i = 0; i < sessions_.size(); ++i) {
    if (sessions_[i].id == currentSessionId_) {
      activeNav().selected = static_cast<int>(i) + 1;
      break;
    }
  }
}

void ConversationPickerActivity::reload() {
  sessions_.clear();
  std::vector<std::string> ids = ConversationSpool::listSessionIds();
  // Newest first: ids are allocated in order, so this is reverse-chronological.
  std::reverse(ids.begin(), ids.end());
  for (const std::string& id : ids) {
    ConversationSpool::Summary summary;
    if (!ConversationSpool::readSummary(id, summary)) {
      // No index yet (or one written by an older build). Still offer it — opening
      // it will build the index — but say nothing we cannot back up.
      summary = ConversationSpool::Summary{};
      summary.id = id;
    }
    sessions_.push_back(std::move(summary));
  }
  rebuildRows();
}

void ConversationPickerActivity::rebuildRows() {
  labels_.clear();
  subtitles_.clear();
  rowItems_.clear();
  labels_.reserve(sessions_.size() + 1);
  subtitles_.reserve(sessions_.size() + 1);

  labels_.emplace_back("+  New conversation");
  subtitles_.emplace_back();

  uint32_t now = 0;
  const bool haveClock = halClock.nowEpoch(now);

  for (const ConversationSpool::Summary& s : sessions_) {
    labels_.push_back(s.firstQuestion.empty() ? std::string("(no questions yet)") : s.firstQuestion);

    char age[24] = {0};
    if (haveClock) formatAge(s.lastEpoch, now, age, sizeof(age));

    char line[64];
    const char* turnWord = s.turnCount == 1 ? "turn" : "turns";
    if (age[0] != '\0') {
      snprintf(line, sizeof(line), "%u %s \xc2\xb7 %s", static_cast<unsigned>(s.turnCount), turnWord, age);
    } else {
      snprintf(line, sizeof(line), "%u %s", static_cast<unsigned>(s.turnCount), turnWord);
    }
    subtitles_.emplace_back(line);
  }

  // Built only after both string vectors have stopped growing: ListItem holds
  // raw pointers, and a reallocation part way through would dangle them.
  for (size_t i = 0; i < labels_.size(); ++i) {
    fui::ListItem item{};
    item.label = labels_[i].c_str();
    item.subtitle = subtitles_[i].empty() ? nullptr : subtitles_[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

void ConversationPickerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void ConversationPickerActivity::activateIndex(const int index) {
  ActivityResult result;
  if (index == kNewRow) {
    // The empty path is the agreed signal for "start a new one".
    result.data = FilePathResult{std::string()};
  } else {
    const size_t i = static_cast<size_t>(index - 1);
    if (i >= sessions_.size()) return;
    result.data = FilePathResult{sessions_[i].id};
  }
  setResult(std::move(result));
  finish();
}

bool ConversationPickerActivity::handleHomeGesture() {
  onBackButton();  // finish() with no result — the caller reads that as dismissed
  return true;
}

void ConversationPickerActivity::onRowLongPress(const int index) { showDeleteConfirmation(index); }

void ConversationPickerActivity::showDeleteConfirmation(const int index) {
  if (index == kNewRow || confirmPopup_.isActive()) return;
  const size_t i = static_cast<size_t>(index - 1);
  if (i >= sessions_.size()) return;
  // Deleting the conversation currently on screen would leave the voice view
  // reading a file that no longer exists.
  if (sessions_[i].id == currentSessionId_) return;

  confirmingDelete_ = true;
  pendingDeleteRow_ = index;
  static const char* options[] = {"Cancel", "Delete"};
  confirmPopup_.show("Delete this conversation?", options, 2, 0, [this](const int choice) {
    confirmingDelete_ = false;
    if (choice == 1) deleteSelected();
    requestUpdate();
  });
  requestUpdate();
}

void ConversationPickerActivity::deleteSelected() {
  const int index = pendingDeleteRow_;
  pendingDeleteRow_ = -1;
  if (index <= kNewRow) return;
  const size_t i = static_cast<size_t>(index - 1);
  if (i >= sessions_.size()) return;

  ConversationSpool::eraseSession(sessions_[i].id);
  reload();
  auto& nav = activeNav();
  if (nav.selected >= static_cast<int>(rowItems_.size())) {
    nav.selected = static_cast<int>(rowItems_.size()) - 1;
  }
}

bool ConversationPickerActivity::handleCustomInput() {
  if (confirmPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete_) {
    // Dismissed without choosing (gesture or tap outside): treat as cancel.
    confirmingDelete_ = false;
    pendingDeleteRow_ = -1;
    requestUpdate();
    return true;
  }
  return false;
}

void ConversationPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int titleX =
      (renderer.getScreenWidth() - renderer.getTextWidth(UI_12_FONT_ID, "Conversations", EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15, "Conversations", true, EpdFontFamily::BOLD);

  renderUi();
  if (confirmPopup_.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}
