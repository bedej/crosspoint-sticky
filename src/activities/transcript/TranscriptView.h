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
  // Hit band for taps on the top status bar. Deliberately taller than the bar
  // draws: a ~28px strip is a poor finger target, and ActivityManager allows
  // itself the same 44px for its own status-bar taps.
  int topBarHitHeight() const { return bodyTop_ > 44 ? bodyTop_ : 44; }

  // Top line: the live ASR transcript. Non-negotiable for a voice UI — ASR
  // misfires, and the reader has to be able to tell a bad answer from a bad
  // transcription.
  // Connection state, shown as a signal indicator at the right of the status
  // line. The 32x32 icon assets are taller than this one reserved line, so it
  // is drawn from primitives at the line height instead.
  enum class Link : uint8_t { Offline, Connecting, WifiUp, Online, Failed };
  void setLink(Link link);
  // Capture state, shown as a filled dot beside the status. Without it there is
  // no way to tell a live mic from a dead one.
  void setListening(bool listening);

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

  // --- live utterance ------------------------------------------------------
  // The partial ASR text, shown in the position the finished turn will occupy
  // so the words do not jump when it settles. A partial is revised on every
  // frame, which is the exact opposite of the settled-lines-never-reflow
  // invariant the page index rests on — so the draft is laid out separately,
  // drawn over the space below the document's tail, and NEVER spooled or
  // indexed. Committing the real turn through the normal path is what makes the
  // replacement land in the same place: same font, width and origin.
  void setDraft(const std::string& text);
  void clearDraft();
  bool draftNeedsRepaint() const { return draftDirty_; }

  // --- query rail ----------------------------------------------------------
  // A strip of the questions asked, overlaid on the transcript and dismissed by
  // tapping outside it. Deliberately drawn here rather than made an Activity: an
  // overlay has an "outside" to tap and a full-screen activity does not, which
  // is what made the text-settings screen inescapable (HomeLab-07o).
  enum class RailHit : uint8_t { None, Dismiss, Conversations, Turn };

  void openRail();
  void closeRail();
  bool railOpen() const { return railOpen_; }
  // Resolves a tap while the rail is open. `turnIndex` is set for RailHit::Turn.
  RailHit railHitTest(int x, int y, uint16_t& turnIndex) const;
  // Scroll the rail's window by whole rows; returns true when it moved.
  bool railScroll(int rows);
  // The transcript underneath changed while the rail was up, so the stored
  // framebuffer is stale and closing must repaint rather than restore.
  void noteContentChangedUnderRail() { railContentChanged_ = true; }
  // Jump to the page a turn STARTS on. Returns true when the page changed.
  bool jumpToTurn(uint16_t turnIndex);
  // Turn behind visible rail row `row`. Lets a script drive the rail without a
  // parallel lookup that could drift from what a tap resolves to.
  bool railRowTurn(size_t row, uint16_t& turnIndex) const;

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
  // Bring the page index back after a reset. Loads the persisted index and lays
  // out only the turns appended since; falls back to a full rebuild when the
  // file is missing, stale, or built for a different render spec. Leaves the
  // live cursor at the end of the session.
  void restoreIndex();
  // The full re-layout, kept as the correctness backstop.
  void rebuildIndex() { rebuildIndexFrom(0, 0, 0); }

  bool hasPendingAppend() const { return pendingFrom_ < lines_.size(); }
  void markFullPaint() { fullPaint_ = true; }
  bool needsFullPaint() const { return fullPaint_; }
  // Draw everything into the framebuffer WITHOUT pushing it to the panel, so
  // the caller can choose the refresh — page turns go through the reader's
  // FAST-with-periodic-HALF cycle (ReaderUtils::displayWithRefreshCycle), which
  // lives on the activity side to keep this class out of the activity headers.
  void paintFull();
  // paintFull() plus a plain FAST refresh.
  void renderFull();
  // Draw only the lines settled since the last push, plus the footer, in one
  // FAST_REFRESH. Falls back to a full paint when there is no framebuffer to
  // append into.
  void renderAppend();
  // Repaint just the draft region (plus the footer) in one FAST_REFRESH.
  void renderDraft();

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

  void rebuildIndexFrom(uint32_t offset, uint16_t turnIndex, uint32_t docLine);
  void saveIndex() const;
  void layoutDraft();
  // Lines left on this page under the document's tail.
  uint16_t draftRoom() const;
  bool showsDraft() const;
  void drawDraft();
  void eraseFrom(size_t lineIndex) const;

  void buildRailRows();
  void drawRail();
  int railRowHeight() const;
  int railTopY() const;
  int railBottomY() const;
  size_t railVisibleRows() const;

  void drawHeader() const;
  // Returns the width it occupied, so the status text knows where to stop.
  int drawLinkIndicator(int right, int top) const;
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
  Link link_ = Link::Offline;
  bool listening_ = false;
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
  bool liveTurnRefd_ = false;  // this turn is already in the spool's turn table
  uint32_t docLine_ = 0;       // settled lines in the whole flowed document
  size_t livePage_ = 0;        // page the tail is currently landing on
  bool paragraphOpen_ = false;
  std::string pendingWord_;  // word fragment carried across delta boundaries

  // query rail
  bool railOpen_ = false;
  bool railStored_ = false;          // a framebuffer snapshot is held
  bool railContentChanged_ = false;  // the page underneath moved on
  size_t railTop_ = 0;               // first visible row
  std::vector<uint16_t> railRows_;   // indices into the spool's turn table

  // live utterance (provisional, never spooled)
  std::string draft_;
  std::vector<Line> draftLines_;
  bool draftDirty_ = false;
  bool draftWasDrawn_ = false;
};
