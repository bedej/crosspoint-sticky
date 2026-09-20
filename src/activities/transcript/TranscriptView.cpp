#include "TranscriptView.h"

#include <Epub/blocks/BlockStyle.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
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
  footerTop_ = screenH - kEdge - uiLineH_;
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

void TranscriptView::endTurn() { flushTail(); }

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
void TranscriptView::rebuildIndex() {
  if (!ready() || spool_.isEmpty()) return;
  spool_.clearIndex();
  lines_.clear();
  pendingFrom_ = 0;
  docLine_ = 0;
  livePage_ = 0;
  const size_t saved = curPage_;
  curPage_ = SIZE_MAX;

  uint32_t offset = spool_.firstTurnOffset();
  uint16_t index = 0;
  while (offset < spool_.size()) {
    ConversationSpool::Turn turn;
    if (!spool_.readTurnAt(offset, index, turn)) break;
    liveRole_ = turn.role;
    liveTurnOffset_ = turn.offset;
    liveTurnIndex_ = index;
    liveTurnLine_ = 0;
    layoutTurnText(turn.role, index, turn.text, [this](Line line, uint16_t) { emitLine(std::move(line)); });
    if (turn.nextOffset <= offset) break;
    offset = turn.nextOffset;
    index++;
  }

  curPage_ = saved;
  LOG_INF("TVIEW", "index rebuilt: %u lines, %u pages", static_cast<unsigned>(docLine_),
          static_cast<unsigned>(spool_.pageCount()));
}

void TranscriptView::loadPage(const size_t page) {
  lines_.clear();
  pendingFrom_ = 0;
  fullPaint_ = true;
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

void TranscriptView::drawHeader() const {
  const int w = renderer_.getScreenWidth();
  std::string top = header_.empty() ? status_ : header_;
  if (!header_.empty() && !status_.empty()) {
    // The status is short ("Listening...") and the transcript is what matters,
    // so the transcript owns the line and the status is dropped once there is
    // something to show.
    top = header_;
  }
  if (!top.empty()) {
    // The header is ONE reserved line and an ASR transcript is arbitrarily long,
    // so it has to be wrapped-and-truncated rather than drawn raw — an unclipped
    // drawText runs straight off the right edge of the framebuffer.
    const auto wrapped = renderer_.wrappedText(UI_10_FONT_ID, top.c_str(), w - 2 * kEdge, 1);
    if (!wrapped.empty()) renderer_.drawText(UI_10_FONT_ID, kEdge, kEdge, wrapped.front().c_str());
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
  renderer_.fillRect(0, footerTop_ - kChromeGap, w, uiLineH_ + kChromeGap + kEdge, false);

  char buf[64];
  const size_t total = pageCountOrOne();
  snprintf(buf, sizeof(buf), "%u/%u", static_cast<unsigned>(curPage_ + 1), static_cast<unsigned>(total));
  renderer_.drawText(UI_10_FONT_ID, kEdge, footerTop_, buf);

  // The single most confusing state in a streaming transcript on a device with
  // no scrollbar is "has it finished, or is more coming?". Say so explicitly.
  if (hasMore()) {
    const size_t behind = livePage_ - curPage_;
    snprintf(buf, sizeof(buf), "v %u more", static_cast<unsigned>(behind));
    const int tw = renderer_.getTextAdvanceX(UI_10_FONT_ID, buf, EpdFontFamily::REGULAR);
    renderer_.drawText(UI_10_FONT_ID, w - kEdge - tw, footerTop_, buf);
  }
}

void TranscriptView::renderFull(const HalDisplay::RefreshMode mode) {
  renderer_.clearScreen();
  drawHeader();
  for (size_t i = 0; i < lines_.size(); ++i) drawLine(i);
  drawFooter();
  pendingFrom_ = lines_.size();
  fullPaint_ = false;
  renderer_.displayBuffer(mode);
}

void TranscriptView::renderAppend() {
  if (fullPaint_) {
    renderFull();
    return;
  }
  for (size_t i = pendingFrom_; i < lines_.size(); ++i) drawLine(i);
  pendingFrom_ = lines_.size();
  // Coalesced into this same refresh rather than issuing one of its own.
  drawFooter();
  renderer_.displayBuffer(HalDisplay::FAST_REFRESH);
}
