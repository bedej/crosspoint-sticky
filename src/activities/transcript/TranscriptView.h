#pragma once
// TranscriptView — the conversation rendered as a real flowed e-reader document:
// justified, hyphenated, italic for the user's turns, paged rather than scrolled.
//
// It does NOT implement text layout. ParsedText::layoutAndExtractLines is the
// engine, and its `includeLastLine=false` mode is the streaming primitive:
// settled lines come out, the ragged tail stays inside the ParsedText until more
// text arrives. ChapterHtmlSlimParser uses the same soft flush across XML chunks.
//
// Because settled lines never reflow, pages 1..N-1 are immutable and only the
// last page grows — which is what makes ConversationSpool's page index safe to
// append to while a reply is still streaming, and page turns safe mid-stream.
//
// E-INK DISCIPLINE (the whole reason this class is shaped the way it is):
//   * one FAST_REFRESH per SETTLED LINE, never per token. A refresh is
//     300-500 ms; a screen fills in ~6 s while reading it takes ~55 s.
//   * a page turn is one HALF_REFRESH, which doubles as the periodic FAST-
//     residual cleanup (a screenful is ~14-18 FAST appends).
//   * the page NEVER auto-turns when it fills. Generation outruns reading ~9x
//     and e-ink has no scrollback, so advancing is the reader's call; a filled
//     page shows a "more" affordance and the tail keeps going to the spool.

#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ConversationSpool.h"

class TranscriptView {
 public:
  using Role = ConversationSpool::Role;

  TranscriptView(GfxRenderer& renderer, ConversationSpool& spool) : renderer_(renderer), spool_(spool) {}

  // Measure the panel, derive the page geometry, and invalidate the spool's page
  // index if the render spec changed (font size, margin, viewport).
  void begin();
  bool ready() const { return fontId_ != 0 && linesPerPage_ > 0; }
  uint16_t linesPerPage() const { return linesPerPage_; }

  // Top line: the live ASR transcript. Non-negotiable for a voice UI — ASR
  // misfires, and the reader has to be able to tell a bad answer from a bad
  // transcription.
  void setHeader(const std::string& text);
  void setStatus(const std::string& text);

  // --- streaming -----------------------------------------------------------
  // Open a turn for incremental layout. `turnOffset`/`turnIndex` come from the
  // spool and are what the page index records.
  void beginTurn(Role role, uint32_t turnOffset, uint16_t turnIndex);
  // Feed more of the open turn. Settled lines are emitted immediately; the
  // ragged tail is held until the next call.
  void appendText(const char* text);
  // Flush the held tail (the turn is complete).
  void endTurn();
  bool turnOpen() const { return parsed_ != nullptr; }

  // --- paging --------------------------------------------------------------
  // Each returns true when the displayed page changed and a repaint is owed.
  bool pagePrev();
  bool pageNext();
  bool prevTurn();   // long-press back: a conversation turn is the reader's chapter
  bool jumpToLatest();
  bool atLatest() const { return curPage_ + 1 >= pageCountOrOne(); }
  // True when text exists past the bottom of the displayed page.
  bool hasMore() const { return livePage_ > curPage_; }
  size_t currentPage() const { return curPage_; }

  // --- rendering -----------------------------------------------------------
  // New settled lines are waiting to be appended to the panel.
  // Walk the spool once and rebuild the page index (deep-sleep wake: the file
  // survived, RAM did not). Leaves the live cursor at the end of the session.
  void rebuildIndex();

  bool hasPendingAppend() const { return pendingFrom_ < lines_.size(); }
  void markFullPaint() { fullPaint_ = true; }
  bool needsFullPaint() const { return fullPaint_; }
  // Repaint everything. Page turns pass HALF_REFRESH (it also clears FAST
  // residual); a header/status change passes FAST_REFRESH.
  void renderFull(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH);
  // Draw only the lines settled since the last push, plus the footer, in one
  // FAST_REFRESH. Falls back to a full paint when there is no framebuffer to
  // append into.
  void renderAppend();

 private:
  size_t pageWhereTurnStarts(uint16_t turnIndex) const;
  size_t pageCountOrOne() const { return spool_.pageCount() ? spool_.pageCount() : 1; }
  // A null entry is a turn separator: a full line-height gap, the same thing
  // BlockStyle::fromBrElement injects between blocks in the reader.
  using Line = std::shared_ptr<TextBlock>;

  void startParagraph();
  void flushTail();                 // hard flush: emit the held last line too
  // `holdTail` keeps a trailing word fragment back for the next delta.
  void feedWords(const char* text, size_t len, bool holdTail);
  void emitLine(Line line);         // one settled line of the flowed document
  void loadPage(size_t page);       // re-lay-out `page` from the spool
  // Lay out one whole turn and hand each line to `sink` with its index within
  // the turn. The one place turn text becomes lines on the replay path, shared
  // by page loads and index rebuilds so the two can never drift apart.
  using LineSink = std::function<void(Line, uint16_t)>;
  uint16_t layoutTurnText(Role role, uint16_t turnIndex, const std::string& text, const LineSink& sink);
  EpdFontFamily::Style styleFor(Role role) const;

  void drawHeader() const;
  void drawLine(size_t index) const;
  void drawFooter() const;
  int lineY(size_t index) const { return bodyTop_ + static_cast<int>(index) * lineAdvance_; }

  GfxRenderer& renderer_;
  ConversationSpool& spool_;

  // geometry
  int fontId_ = 0;
  int lineAdvance_ = 0;
  int uiLineH_ = 0;
  int textLeft_ = 0;
  int textWidth_ = 0;
  int bodyTop_ = 0;
  int footerTop_ = 0;
  uint16_t linesPerPage_ = 0;

  // chrome
  std::string header_;
  std::string status_;

  // displayed page
  std::vector<Line> lines_;
  size_t curPage_ = 0;
  size_t pendingFrom_ = 0;  // first line not yet pushed to the panel
  bool fullPaint_ = true;

  // live layout cursor
  std::unique_ptr<ParsedText> parsed_;
  Role liveRole_ = Role::Agent;
  uint32_t liveTurnOffset_ = 0;
  uint16_t liveTurnIndex_ = 0;
  uint16_t liveTurnLine_ = 0;  // lines emitted so far for the open turn
  uint32_t docLine_ = 0;       // settled lines in the whole flowed document
  size_t livePage_ = 0;        // page the tail is currently landing on
  bool paragraphOpen_ = false;
  std::string pendingWord_;  // word fragment carried across delta boundaries

  // scratch for the replay path, so a page turn does not churn the heap
  std::vector<Line> scratch_;
};
