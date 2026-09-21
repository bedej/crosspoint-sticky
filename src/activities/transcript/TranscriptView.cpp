#include "TranscriptView.h"

#include <Epub/blocks/BlockStyle.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {

constexpr int kEdge = 8;      // outer padding
constexpr int kChromeGap = 6; // gap under the header rule / above the footer

CssTextAlign toCssAlign(const uint8_t align) {
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

// The user's turn is the ASR transcript, so it is marked as quoted material
// rather than left to look like something the agent said.
constexpr char kUserMarker[] = ">";

// Query rail geometry. Wide enough that a whole question sits on ONE line and
// is not ellipsised either — wrapping made the rail hard to scan, and truncating
// defeats the point of showing the question at all. Sized so the full 20-byte
// snippet fits beside the ordinal at UI_10. Half the panel is a lot, but the
// rail is a transient overlay and the transcript behind it is only context.
constexpr int kRailWidth = 240;
constexpr int kRailPad = 5;
constexpr int kRailOrdinal = 18;

}  // namespace

// ---------------------------------------------------------------------------
// geometry

void TranscriptView::begin() {
  fontId_ = SETTINGS.getReaderFontId();
  if (fontId_ == 0) {
    LOG_ERR("TVIEW", "no reader font");
    return;
  }
  const float compression = SETTINGS.getReaderLineCompression();
  lineAdvance_ = std::max(1, renderer_.getLineHeight(fontId_, compression));
  uiLineH_ = std::max(1, renderer_.getLineHeight(UI_10_FONT_ID));

  const int screenW = renderer_.getScreenWidth();
  const int screenH = renderer_.getScreenHeight();
  const int margin = std::max<int>(kEdge, SETTINGS.screenMargin);
  textLeft_ = margin;
  textWidth_ = screenW - 2 * margin;

  // One reserved line at the top (live ASR) and one at the bottom (page n/N and
  // the unread-tail indicator). Neither may be eaten by the flowed body.
  bodyTop_ = kEdge + uiLineH_ + kChromeGap + 1 /* rule */ + kChromeGap;
  // The bottom bar is the reader's, so reserve exactly what it occupies rather
  // than a guess at one text line.
  footerTop_ = screenH - UITheme::getStatusBarHeight();
  const int bodyHeight = (footerTop_ - kChromeGap) - bodyTop_;
  linesPerPage_ = static_cast<uint16_t>(std::max(0, bodyHeight / lineAdvance_));

  const ConversationSpool::RenderSpec spec{fontId_, static_cast<uint16_t>(std::max(0, textWidth_)), linesPerPage_,
                                           SETTINGS.fontPointSize};
  if (spool_.applySpec(spec)) {
    // Geometry changed: the page index described a different layout, so it is
    // gone. The spool itself is untouched — pages are rebuilt from raw text.
    LOG_INF("TVIEW", "render spec changed, page index rebuilt");
    lines_.clear();
    docLine_ = 0;
    curPage_ = 0;
    livePage_ = 0;
  }
  LOG_INF("TVIEW", "%dx%d font=%d adv=%d lines/page=%u", screenW, screenH, fontId_, lineAdvance_,
          static_cast<unsigned>(linesPerPage_));
  fullPaint_ = true;
}

void TranscriptView::setHeader(const std::string& text) {
  if (header_ == text) return;
  header_ = text;
  fullPaint_ = true;
}

void TranscriptView::setLink(const Link link) {
  if (link_ == link) return;
  link_ = link;
  fullPaint_ = true;
}

void TranscriptView::setListening(const bool listening) {
  if (listening_ == listening) return;
  listening_ = listening;
  fullPaint_ = true;
}

void TranscriptView::setStatus(const std::string& text) {
  if (status_ == text) return;
  status_ = text;
  fullPaint_ = true;
}

EpdFontFamily::Style TranscriptView::styleFor(const Role role) const {
  // Real faces with their own kerning and ligature tables, not a synthesised
  // slant: bits 0-1 of EpdFontFamily::Style pick the face.
  return role == Role::User ? EpdFontFamily::ITALIC : EpdFontFamily::REGULAR;
}

// ---------------------------------------------------------------------------
// streaming layout

void TranscriptView::startParagraph() {
  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;
  parsed_.reset(new ParsedText(SETTINGS.extraParagraphSpacing != 0, SETTINGS.hyphenationEnabled != 0,
                               SETTINGS.focusReadingEnabled != 0, style));
  paragraphOpen_ = true;
}

void TranscriptView::beginTurn(const Role role, const uint32_t turnOffset, const uint16_t turnIndex) {
  flushTail();
  liveRole_ = role;
  liveTurnOffset_ = turnOffset;
  liveTurnIndex_ = turnIndex;
  liveTurnLine_ = 0;
  liveTurnRefd_ = false;
  startParagraph();
  // Turn separator: a full line-height gap, the same break the reader gets from
  // BlockStyle::fromBrElement. It is line 0 of the turn so that a replay from
  // the page index reproduces it at exactly the same place.
  if (turnIndex > 0) emitLine(Line());
  if (role == Role::User) parsed_->addWord(kUserMarker, EpdFontFamily::REGULAR);
}

void TranscriptView::feedWords(const char* text, const size_t len, const bool holdTail) {
  if (!parsed_) startParagraph();
  const EpdFontFamily::Style style = styleFor(liveRole_);
  std::string word;
  for (size_t i = 0; i < len; ++i) {
    const char c = text[i];
    if (c == ' ' || c == '\t') {
      if (!word.empty()) {
        parsed_->addWord(word, style);
        word.clear();
      }
      continue;
    }
    word.push_back(c);
  }
  if (word.empty()) return;
  // A delta can end mid-word ("mim" + "ics") or split a word from its full stop
  // ("oil" + "."). Whitespace is the only word boundary the tokenizer has, so a
  // trailing fragment is held back until the next delta proves where the word
  // ends. Without this the live layout also disagrees with a re-layout from the
  // spool, which would put the page index out of step with what was drawn.
  if (holdTail) {
    pendingWord_ = word;
  } else {
    parsed_->addWord(word, style);
  }
}

void TranscriptView::appendText(const char* text) {
  if (text == nullptr || *text == '\0' || !ready()) return;
  if (!parsed_) startParagraph();

  // Re-attach whatever fragment the previous delta ended on.
  std::string work;
  work.reserve(pendingWord_.size() + std::strlen(text));
  work = pendingWord_;
  work += text;
  pendingWord_.clear();

  // A newline ends the paragraph: hard-flush so its short last line settles,
  // then open a fresh one. Everything else is fed as words and soft-flushed, so
  // only SETTLED lines leave the layout engine and the ragged tail is held.
  const char* raw = work.c_str();
  const char* segment = raw;
  for (const char* p = raw;; ++p) {
    if (*p != '\n' && *p != '\0') continue;
    const bool endOfChunk = (*p == '\0');
    feedWords(segment, static_cast<size_t>(p - segment), endOfChunk);
    if (endOfChunk) break;
    flushTail();  // the newline is a real word boundary, so nothing is held back
    startParagraph();
    segment = p + 1;
  }

  if (parsed_ && !parsed_->isEmpty()) {
    parsed_->layoutAndExtractLines(
        renderer_, fontId_, static_cast<uint16_t>(textWidth_),
        [this](std::shared_ptr<TextBlock> line, uint32_t) { emitLine(std::move(line)); },
        /*includeLastLine=*/false);
  }
}

void TranscriptView::flushTail() {
  if (!pendingWord_.empty()) {
    if (!parsed_) startParagraph();
    parsed_->addWord(pendingWord_, styleFor(liveRole_));
    pendingWord_.clear();
  }
  if (!parsed_) return;
  if (!parsed_->isEmpty() && ready()) {
    parsed_->layoutAndExtractLines(
        renderer_, fontId_, static_cast<uint16_t>(textWidth_),
        [this](std::shared_ptr<TextBlock> line, uint32_t) { emitLine(std::move(line)); },
        /*includeLastLine=*/true);
  }
  parsed_.reset();
  paragraphOpen_ = false;
}

void TranscriptView::endTurn() {
  flushTail();
  // Record the turn for the query rail now rather than at beginTurn(): the
  // snippet needs the whole text, which only exists once the turn has settled.
  // Guarded because endTurn() is idempotent by design — finishAnswer(), onExit()
  // and the next beginTurn() may all call it for the same turn.
  if (!liveTurnRefd_ && spool_.isOpen() && spool_.turnCount() > 0) {
    ConversationSpool::Turn turn;
    if (spool_.readTurnAt(liveTurnOffset_, liveTurnIndex_, turn)) {
      spool_.appendTurnRef(turn);
      liveTurnRefd_ = true;
    }
  }
  // A completed turn is a clean boundary, and this is what spares the next boot
  // a full re-layout of the conversation.
  saveIndex();
}

void TranscriptView::emitLine(Line line) {
  if (linesPerPage_ == 0) return;
  const size_t page = docLine_ / linesPerPage_;
  // Only the final page can still grow, so a page that has not been indexed yet
  // is by definition the one this line opens.
  if (page >= spool_.pageCount()) {
    spool_.appendPage({liveTurnOffset_, liveTurnIndex_, liveTurnLine_});
  }
  docLine_++;
  liveTurnLine_++;
  livePage_ = page;

  if (page == curPage_ && lines_.size() < linesPerPage_) {
    lines_.push_back(std::move(line));
    // The document just took a line the draft may have been using.
    if (!draft_.empty()) {
      layoutDraft();
      draftDirty_ = true;
    }
  }
  // Lines on a page the reader is not looking at are counted, never retained:
  // only the displayed page's TextBlocks live in RAM.
}

// ---------------------------------------------------------------------------
// replay from the spool

uint16_t TranscriptView::layoutTurnText(const Role role, const uint16_t turnIndex, const std::string& text,
                                       const LineSink& sink) {
  uint16_t produced = 0;
  const auto take = [&](Line line) {
    sink(std::move(line), produced);
    produced++;
  };

  // The separator is line 0 of the turn, so a replay that starts at an arbitrary
  // lineOffset reproduces the gap in exactly the same place the live path put it.
  if (turnIndex > 0) take(Line());

  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;
  const EpdFontFamily::Style wordStyle = styleFor(role);

  const char* raw = text.c_str();
  const char* segment = raw;
  bool first = true;
  for (const char* p = raw;; ++p) {
    if (*p != '\n' && *p != '\0') continue;
    ParsedText parsed(SETTINGS.extraParagraphSpacing != 0, SETTINGS.hyphenationEnabled != 0,
                      SETTINGS.focusReadingEnabled != 0, style);
    if (first && role == Role::User) parsed.addWord(kUserMarker, EpdFontFamily::REGULAR);
    first = false;

    std::string word;
    for (const char* q = segment; q < p; ++q) {
      if (*q == ' ' || *q == '\t') {
        if (!word.empty()) {
          parsed.addWord(word, wordStyle);
          word.clear();
        }
        continue;
      }
      word.push_back(*q);
    }
    if (!word.empty()) parsed.addWord(word, wordStyle);

    if (!parsed.isEmpty()) {
      // Whole-paragraph layout. Greedy line breaking is prefix-deterministic, so
      // this reproduces exactly the settled lines the streaming path produced.
      parsed.layoutAndExtractLines(renderer_, fontId_, static_cast<uint16_t>(textWidth_),
                                   [&](std::shared_ptr<TextBlock> line, uint32_t) { take(std::move(line)); });
    }
    if (*p == '\0') break;
    segment = p + 1;
  }
  return produced;
}

// Rebuild the page index by re-laying out the whole session. This is the
// deep-sleep-wake path: the spool survived, RAM did not, so there is no index
// until we walk the raw text once. No TextBlock is retained while scanning
// (curPage_ is parked out of range), so RAM stays at one page throughout.
void TranscriptView::rebuildIndexFrom(const uint32_t offset, const uint16_t turnIndex, const uint32_t docLine) {
  lines_.clear();
  pendingFrom_ = 0;
  docLine_ = docLine;
  livePage_ = (linesPerPage_ && docLine) ? (docLine - 1) / linesPerPage_ : 0;
  // Nothing is retained while scanning — curPage_ is parked out of range — so
  // RAM stays at one page however long the conversation is.
  const size_t saved = curPage_;
  curPage_ = SIZE_MAX;

  uint32_t cursor = offset;
  uint16_t index = turnIndex;
  while (cursor < spool_.size()) {
    ConversationSpool::Turn turn;
    if (!spool_.readTurnAt(cursor, index, turn)) break;
    liveRole_ = turn.role;
    liveTurnOffset_ = turn.offset;
    liveTurnIndex_ = index;
    liveTurnLine_ = 0;
    spool_.appendTurnRef(turn);
    layoutTurnText(turn.role, index, turn.text, [this](Line line, uint16_t) { emitLine(std::move(line)); });
    if (turn.nextOffset <= cursor) break;
    cursor = turn.nextOffset;
    index++;
  }

  curPage_ = saved;
}

void TranscriptView::saveIndex() const {
  // Only ever called at a turn boundary, so the cursor covers whole turns and
  // resuming from it needs no mid-turn layout state.
  const ConversationSpool::IndexCursor cursor{spool_.size(), spool_.turnCount(), docLine_};
  spool_.saveIndex(cursor);
}

void TranscriptView::restoreIndex() {
  if (!ready()) return;

  // Adopting a spool means adopting its line numbering from zero. Without this,
  // switching to a conversation carried the PREVIOUS one's counter: a brand-new
  // session began at line 531 and put its first page 23 pages in, because
  // begin() only resets these when the render spec changes and the early return
  // below skipped them entirely for an empty spool.
  lines_.clear();
  pendingFrom_ = 0;
  docLine_ = 0;
  livePage_ = 0;
  curPage_ = 0;
  liveTurnOffset_ = 0;
  liveTurnIndex_ = 0;
  liveTurnLine_ = 0;
  liveTurnRefd_ = false;
  clearDraft();
  fullPaint_ = true;

  if (spool_.isEmpty()) return;
  const unsigned long started = millis();

  ConversationSpool::IndexCursor cursor;
  bool loaded = spool_.loadIndex(cursor);
  if (loaded && linesPerPage_ > 0) {
    // The line count and the page count have to agree. They disagreed for real:
    // indexes written before the session-switch fix carry the PREVIOUS
    // conversation's line counter, which shows up as a footer promising pages
    // that do not exist. Distrust the cursor and rebuild rather than inherit it.
    const uint32_t implied = static_cast<uint32_t>(spool_.pageCount()) * linesPerPage_;
    if (cursor.docLine > implied) {
      LOG_INF("TVIEW", "index cursor implausible (%u lines vs %u pages), rebuilding",
              static_cast<unsigned>(cursor.docLine), static_cast<unsigned>(spool_.pageCount()));
      loaded = false;
    }
  }
  if (loaded) {
    // Everything before the cursor is already indexed; lay out only what has
    // been appended since. Usually nothing, which is the whole point.
    rebuildIndexFrom(cursor.spoolOffset, cursor.turnIndex, cursor.docLine);
  } else {
    spool_.clearIndex();
    rebuildIndexFrom(0, 0, 0);
  }
  saveIndex();

  LOG_INF("TVIEW", "index ready in %lums: %u lines, %u pages", millis() - started,
          static_cast<unsigned>(docLine_), static_cast<unsigned>(spool_.pageCount()));
}

void TranscriptView::loadPage(const size_t page) {
  lines_.clear();
  pendingFrom_ = 0;
  fullPaint_ = true;
  draftWasDrawn_ = false;
  if (page >= spool_.pageCount() || linesPerPage_ == 0) return;

  const ConversationSpool::PageRef ref = spool_.page(page);
  uint32_t offset = ref.turnOffset;
  uint16_t index = ref.turnIndex;
  uint16_t skip = ref.lineOffset;

  while (lines_.size() < linesPerPage_) {
    ConversationSpool::Turn turn;
    if (!spool_.readTurnAt(offset, index, turn)) break;
    layoutTurnText(turn.role, index, turn.text, [this, skip](Line line, const uint16_t n) {
      if (n >= skip && lines_.size() < linesPerPage_) lines_.push_back(std::move(line));
    });
    if (turn.nextOffset <= offset || turn.nextOffset >= spool_.size()) break;
    offset = turn.nextOffset;
    index++;
    skip = 0;
  }
}

// ---------------------------------------------------------------------------
// live utterance

uint16_t TranscriptView::draftRoom() const {
  if (linesPerPage_ == 0 || lines_.size() >= linesPerPage_) return 0;
  return static_cast<uint16_t>(linesPerPage_ - lines_.size());
}

bool TranscriptView::showsDraft() const {
  // Only ever on the live tail page. If the reader has paged back, the draft
  // would be describing a place they are not looking at.
  return !draftLines_.empty() && curPage_ == livePage_;
}

void TranscriptView::layoutDraft() {
  draftLines_.clear();
  if (draft_.empty() || !ready()) return;
  const uint16_t room = draftRoom();
  if (room == 0) return;

  // Laid out exactly as the committed turn will be — same role, same separator,
  // same wrap width — so the final transcript replaces it without shifting.
  std::vector<Line> all;
  layoutTurnText(Role::User, spool_.turnCount(), draft_,
                 [&all](Line line, uint16_t) { all.push_back(std::move(line)); });

  if (all.size() <= room) {
    draftLines_ = std::move(all);
    return;
  }

  // An utterance can outgrow the space left on the page. Keep the TAIL: the
  // words just spoken are the ones worth seeing, and the draft must not push
  // the page over — a turn would then commit onto a page nobody is looking at.
  //
  // The separator is structural, not content, so it is anchored rather than
  // trimmed. Dropping it would put the draft one line higher than the committed
  // turn, and the text would visibly jump down when the transcript settled —
  // which is the exact thing this draft exists to avoid.
  // With room for a single line, anchoring the separator would leave a blank
  // draft, so the one line goes to text.
  const bool hasSeparator = !all.empty() && !all.front() && room >= 2;
  const size_t keep = hasSeparator ? room - 1 : room;
  if (hasSeparator) draftLines_.push_back(Line());
  for (size_t i = all.size() - keep; i < all.size(); ++i) draftLines_.push_back(std::move(all[i]));
}

void TranscriptView::setDraft(const std::string& text) {
  if (draft_ == text) return;
  draft_ = text;
  layoutDraft();
  draftDirty_ = true;
}

void TranscriptView::clearDraft() {
  if (draft_.empty() && draftLines_.empty()) return;
  draft_.clear();
  draftLines_.clear();
  draftDirty_ = true;
}

void TranscriptView::drawDraft() {
  for (size_t i = 0; i < draftLines_.size(); ++i) {
    const Line& line = draftLines_[i];
    if (line) line->render(renderer_, fontId_, textLeft_, lineY(lines_.size() + i));
  }
  draftWasDrawn_ = true;
}

void TranscriptView::eraseFrom(const size_t lineIndex) const {
  const int top = lineY(lineIndex);
  const int bottom = footerTop_ - kChromeGap;
  if (bottom > top) renderer_.fillRect(0, top, renderer_.getScreenWidth(), bottom - top, false);
}

// ---------------------------------------------------------------------------
// query rail

int TranscriptView::railTopY() const { return kEdge + uiLineH_ + kChromeGap + 1; }
int TranscriptView::railBottomY() const { return footerTop_ - kChromeGap; }

int TranscriptView::railRowHeight() const { return uiLineH_ + kRailPad * 2; }

size_t TranscriptView::railVisibleRows() const {
  const int usable = railBottomY() - railTopY() - railRowHeight();  // less the header row
  if (usable <= 0 || railRowHeight() <= 0) return 0;
  return static_cast<size_t>(usable / railRowHeight());
}

void TranscriptView::buildRailRows() {
  railRows_.clear();
  // The rail lists the QUESTIONS, so agent turns are skipped. The table is built
  // by the index rebuild, so this costs no file reads.
  for (size_t i = 0; i < spool_.turnRefCount(); ++i) {
    if (spool_.turnRef(i).role == static_cast<uint8_t>(Role::User)) {
      railRows_.push_back(static_cast<uint16_t>(i));
    }
  }
  // Open on the most recent questions: that is where the conversation is.
  const size_t visible = railVisibleRows();
  railTop_ = railRows_.size() > visible ? railRows_.size() - visible : 0;
}

void TranscriptView::openRail() {
  if (railOpen_ || !ready()) return;
  buildRailRows();
  // Snapshot the page so closing is a restore rather than a re-render. The
  // reader's toolbar does exactly this; re-rendering here is both slow and
  // visibly wrong (EpubReaderActivity's own comment on the same pattern).
  railStored_ = renderer_.storeBwBuffer();
  railContentChanged_ = false;
  railOpen_ = true;
  drawRail();
  renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
}

void TranscriptView::closeRail() {
  if (!railOpen_) return;
  railOpen_ = false;
  railRows_.clear();

  if (railStored_ && !railContentChanged_) {
    railStored_ = false;
    // false is load-bearing: the glass is showing the rail, painted AFTER the
    // store, and resyncing the baseline would treat it as already erased and
    // leave it on the panel (GfxRenderer.h:342-348).
    renderer_.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // The page moved on underneath, so the snapshot is a lie — drop it and repaint.
  if (railStored_) {
    railStored_ = false;
    renderer_.discardStoredBwBuffer();
  }
  railContentChanged_ = false;
  fullPaint_ = true;
}

bool TranscriptView::railScroll(const int rows) {
  if (!railOpen_ || rows == 0) return false;
  const size_t visible = railVisibleRows();
  if (railRows_.size() <= visible) return false;
  const size_t maxTop = railRows_.size() - visible;
  size_t next = railTop_;
  if (rows < 0) {
    const size_t back = static_cast<size_t>(-rows);
    next = back > railTop_ ? 0 : railTop_ - back;
  } else {
    next = railTop_ + static_cast<size_t>(rows);
    if (next > maxTop) next = maxTop;
  }
  if (next == railTop_) return false;
  railTop_ = next;
  drawRail();
  renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
  return true;
}

TranscriptView::RailHit TranscriptView::railHitTest(const int x, const int y, uint16_t& turnIndex) const {
  if (!railOpen_) return RailHit::None;
  const int left = renderer_.getScreenWidth() - kRailWidth;
  if (x < left) return RailHit::Dismiss;  // tapped the conversation, not the rail

  const int headerBottom = railTopY() + railRowHeight();
  if (y < headerBottom) return RailHit::Conversations;

  const int row = (y - headerBottom) / railRowHeight();
  if (row < 0) return RailHit::None;
  const size_t index = railTop_ + static_cast<size_t>(row);
  if (index >= railRows_.size()) return RailHit::None;
  turnIndex = spool_.turnRef(railRows_[index]).turnIndex;
  return RailHit::Turn;
}

void TranscriptView::drawRail() {
  const int screenW = renderer_.getScreenWidth();
  const int left = screenW - kRailWidth;
  const int top = railTopY();
  const int bottom = railBottomY();
  const int rowH = railRowHeight();

  renderer_.fillRect(left, top, kRailWidth, bottom - top, false);  // clear the strip
  renderer_.fillRect(left, top, 1, bottom - top, true);            // and rule it off

  // Header: the way through to the other conversations. Wrapped-and-truncated
  // rather than drawn raw — an unclipped drawText runs off the right edge, the
  // same way the status line did.
  {
    const auto label = renderer_.wrappedText(UI_10_FONT_ID, "Conversations", kRailWidth - kRailPad * 2, 1);
    if (!label.empty()) renderer_.drawText(UI_10_FONT_ID, left + kRailPad, top + kRailPad, label.front().c_str());
  }
  renderer_.fillRect(left, top + rowH - 1, kRailWidth, 1, true);

  const uint16_t currentTurn = spool_.pageCount() ? spool_.page(curPage_).turnIndex : 0;
  const size_t visible = railVisibleRows();
  int y = top + rowH;

  for (size_t i = 0; i < visible; ++i) {
    const size_t index = railTop_ + i;
    if (index >= railRows_.size()) break;
    const ConversationSpool::TurnRef& ref = spool_.turnRef(railRows_[index]);

    // A question is "current" until the next one starts, so the agent's reply to
    // it counts as still being inside it.
    uint16_t nextTurn = 0xFFFF;
    if (index + 1 < railRows_.size()) nextTurn = spool_.turnRef(railRows_[index + 1]).turnIndex;
    const bool active = currentTurn >= ref.turnIndex && currentTurn < nextTurn;

    if (active) renderer_.fillRect(left + 1, y, kRailWidth - 1, rowH, true);
    const bool ink = !active;  // invert the text on the filled row

    char ordinal[8];
    snprintf(ordinal, sizeof(ordinal), "%u", static_cast<unsigned>(index + 1));
    const int ordW = renderer_.getTextAdvanceX(UI_10_FONT_ID, ordinal, EpdFontFamily::REGULAR);
    renderer_.drawRect(left + kRailPad, y + kRailPad, kRailOrdinal, kRailOrdinal, ink);
    renderer_.drawText(UI_10_FONT_ID, left + kRailPad + (kRailOrdinal - ordW) / 2, y + kRailPad + 1, ordinal, ink);

    const int textLeft = left + kRailPad + kRailOrdinal + 4;
    const int textW = screenW - kEdge - textLeft;
    if (textW > 0 && ref.snippet[0] != '\0') {
      // One line per question: truncated with an ellipsis rather than wrapped,
      // so every row is the same height and the rail scans top to bottom.
      const auto lines = renderer_.wrappedText(UI_10_FONT_ID, ref.snippet, textW, 1);
      if (!lines.empty()) renderer_.drawText(UI_10_FONT_ID, textLeft, y + kRailPad, lines.front().c_str(), ink);
    }
    y += rowH;
  }

  // Say so rather than silently showing a window: there is no scrollbar here.
  if (railRows_.size() > visible) {
    char more[24];
    snprintf(more, sizeof(more), "%u earlier", static_cast<unsigned>(railTop_));
    if (railTop_ > 0) renderer_.drawText(UI_10_FONT_ID, left + kRailPad, bottom - uiLineH_, more);
  }
}

bool TranscriptView::railRowTurn(const size_t row, uint16_t& turnIndex) const {
  const size_t index = railTop_ + row;
  if (!railOpen_ || index >= railRows_.size()) return false;
  turnIndex = spool_.turnRef(railRows_[index]).turnIndex;
  return true;
}

bool TranscriptView::jumpToTurn(const uint16_t turnIndex) {
  if (spool_.pageCount() == 0) return false;
  const size_t target = pageWhereTurnStarts(turnIndex);
  if (target == curPage_) return false;
  curPage_ = target;
  loadPage(curPage_);
  return true;
}

// ---------------------------------------------------------------------------
// paging

bool TranscriptView::pagePrev() {
  if (curPage_ == 0) return false;
  curPage_--;
  loadPage(curPage_);
  return true;
}

bool TranscriptView::pageNext() {
  if (curPage_ + 1 >= spool_.pageCount()) return false;
  curPage_++;
  loadPage(curPage_);
  return true;
}

// The page a turn STARTS on. page(i).turnIndex is the turn owning that page's
// FIRST line, which is not the same thing: a turn that begins half way down a
// page is owned by the page before the first one indexed against it. Landing on
// the wrong one of those drops the reader into the middle of a reply.
size_t TranscriptView::pageWhereTurnStarts(const uint16_t turnIndex) const {
  size_t page = 0;
  for (size_t i = 0; i < spool_.pageCount(); ++i) {
    const ConversationSpool::PageRef& ref = spool_.page(i);
    if (ref.turnIndex > turnIndex) break;
    if (ref.turnIndex < turnIndex) {
      page = i;  // the turn may still begin part way down this page
      continue;
    }
    // ref.turnIndex == turnIndex
    if (ref.lineOffset == 0) page = i;  // the page opens exactly on the turn
    break;
  }
  return page;
}

bool TranscriptView::prevTurn() {
  // TURN = CHAPTER, with the reader's skip-back feel: from the middle of a reply
  // the first press goes to the top of that reply, and only from its first page
  // does it step to the turn before it.
  if (spool_.pageCount() == 0) return false;
  const uint16_t here = spool_.page(curPage_).turnIndex;
  size_t target = pageWhereTurnStarts(here);
  if (target == curPage_) {
    if (here == 0) return false;  // already at the top of the first turn
    target = pageWhereTurnStarts(static_cast<uint16_t>(here - 1));
  }
  if (target == curPage_) return false;
  curPage_ = target;
  loadPage(curPage_);
  return true;
}

bool TranscriptView::jumpToLatest() {
  const size_t last = spool_.pageCount() ? spool_.pageCount() - 1 : 0;
  if (last == curPage_ && !lines_.empty()) return false;
  curPage_ = last;
  loadPage(curPage_);
  return true;
}

// ---------------------------------------------------------------------------
// drawing

// Four distinct glyphs, all drawn from primitives at the height of the status
// line. The 32x32 WifiIcon asset is no use here: drawIcon() does not scale and
// its orientation mapping assumes the forced-Portrait UI themes.
//
// Bars only mean SIGNAL, and only once a link exists. Drawing a partially
// filled bar stack while the radio is still associating reads as "connected,
// weak" — the opposite of the truth — so the two states that have no link do
// not use bars at all:
//
//   connecting  three dots, the usual "working on it"
//   no link     hollow bars struck through
//   associated  two filled bars (socket still opening)
//   online      three filled bars
int TranscriptView::drawLinkIndicator(const int right, const int top) const {
  constexpr int kBars = 3;
  constexpr int kBarW = 4;
  constexpr int kGap = 2;
  const int height = std::max(6, uiLineH_ - 4);
  const int width = kBars * kBarW + (kBars - 1) * kGap;
  const int x0 = right - width;

  if (link_ == Link::Connecting) {
    const int dot = std::max(2, height / 5);
    const int y = top + (height - dot) / 2;
    for (int i = 0; i < 3; ++i) {
      renderer_.fillRect(x0 + i * (width - dot) / 2, y, dot, dot, true);
    }
    return width;
  }

  int filled = 0;
  if (link_ == Link::WifiUp) filled = 2;
  if (link_ == Link::Online) filled = 3;

  for (int i = 0; i < kBars; ++i) {
    const int barH = (height * (i + 1)) / kBars;
    const int x = x0 + i * (kBarW + kGap);
    const int y = top + (height - barH);
    if (i < filled) {
      renderer_.fillRect(x, y, kBarW, barH, true);
    } else {
      renderer_.drawRect(x, y, kBarW, barH, true);
    }
  }

  // Offline and Failed both mean "no link"; the stroke is what separates them
  // from a stack that is merely empty so far.
  if (link_ == Link::Offline || link_ == Link::Failed) {
    renderer_.drawLine(x0 - 1, top + height, x0 + width, top - 1, 2, true);
  }
  return width;
}

void TranscriptView::drawHeader() const {
  const int w = renderer_.getScreenWidth();
  std::string top = header_.empty() ? status_ : header_;
  if (!header_.empty() && !status_.empty()) {
    // The status is short ("Listening...") and the transcript is what matters,
    // so the transcript owns the line and the status is dropped once there is
    // something to show.
    top = header_;
  }
  const int indicatorW = drawLinkIndicator(w - kEdge, kEdge);

  // A filled dot while the mic is live. On a panel with no LED this is the only
  // way to tell capture is running.
  int textX = kEdge;
  if (listening_) {
    const int d = std::max(6, uiLineH_ - 6);
    const int y = kEdge + (uiLineH_ - d) / 2;
    renderer_.fillRect(kEdge, y, d, d, true);
    textX = kEdge + d + kChromeGap;
  }

  if (!top.empty()) {
    // The header is ONE reserved line and the status is arbitrarily long, so it
    // has to be wrapped-and-truncated rather than drawn raw — an unclipped
    // drawText runs straight off the right edge of the framebuffer. The text
    // also stops short of the indicator instead of running under it.
    const int textW = w - kEdge - textX - indicatorW - kChromeGap;
    if (textW > 0) {
      const auto wrapped = renderer_.wrappedText(UI_10_FONT_ID, top.c_str(), textW, 1);
      if (!wrapped.empty()) renderer_.drawText(UI_10_FONT_ID, textX, kEdge, wrapped.front().c_str());
    }
  }
  const int ruleY = kEdge + uiLineH_ + kChromeGap;
  renderer_.fillRect(kEdge, ruleY, w - 2 * kEdge, 1, true);
}

void TranscriptView::drawLine(const size_t index) const {
  const Line& line = lines_[index];
  if (!line) return;  // separator
  line->render(renderer_, fontId_, textLeft_, lineY(index));
}

void TranscriptView::drawFooter() const {
  const int w = renderer_.getScreenWidth();
  // Erase first: the footer is redrawn inside somebody else's refresh.
  renderer_.fillRect(0, footerTop_ - kChromeGap, w, renderer_.getScreenHeight() - footerTop_ + kChromeGap, false);

  const size_t total = pageCountOrOne();
  const int current = static_cast<int>(curPage_ + 1);
  const float progress = total > 0 ? (static_cast<float>(current) * 100.0f / static_cast<float>(total)) : 0.0f;

  // The single most confusing state in a streaming transcript on a device with
  // no scrollbar is "has it finished, or is more coming?", so the centre slot —
  // the reader's book title — carries the unread tail instead.
  char more[32] = {0};
  if (hasMore()) {
    snprintf(more, sizeof(more), "%u more", static_cast<unsigned>(livePage_ - curPage_));
  }

  // The reader's own status bar: battery on the left, page index on the right,
  // its fonts and its margins, and it honours the user's status-bar settings
  // (percentage, progress bar, clock) for free.
  BaseTheme::drawStatusBar(renderer_, progress, current, static_cast<int>(total), more);
}

void TranscriptView::paintFull() {
  renderer_.clearScreen();
  drawHeader();
  for (size_t i = 0; i < lines_.size(); ++i) drawLine(i);
  pendingFrom_ = lines_.size();
  draftWasDrawn_ = false;
  if (showsDraft()) drawDraft();
  drawFooter();
  draftDirty_ = false;
  fullPaint_ = false;
}

void TranscriptView::renderFull() {
  paintFull();
  renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
}

void TranscriptView::renderAppend() {
  if (fullPaint_) {
    renderFull();
    return;
  }
  // A settled line lands where the draft was standing, so stale draft text has
  // to go before the line is drawn over it.
  if (draftWasDrawn_ || draftDirty_) {
    eraseFrom(pendingFrom_);
    draftWasDrawn_ = false;
  }
  for (size_t i = pendingFrom_; i < lines_.size(); ++i) drawLine(i);
  pendingFrom_ = lines_.size();
  if (showsDraft()) drawDraft();
  // Coalesced into this same refresh rather than issuing one of its own.
  drawFooter();
  draftDirty_ = false;
  renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
}

void TranscriptView::renderDraft() {
  if (fullPaint_) {
    renderFull();
    return;
  }
  // The draft is revised on every ASR frame, so this repaints a region rather
  // than appending: erase from the document's tail down and redraw.
  eraseFrom(lines_.size());
  draftWasDrawn_ = false;
  if (showsDraft()) drawDraft();
  drawFooter();
  draftDirty_ = false;
  renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
}
