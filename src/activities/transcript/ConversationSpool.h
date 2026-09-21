#pragma once
// ConversationSpool — append-only transcript persistence for the Sticky's
// conversation view, plus the append-only page index built over it.
//
// WHY A SPOOL AT ALL: the Sticky deep-sleeps with wake=reset, so RAM is gone on
// every wake. The conversation has to live on the microSD or there is nothing to
// page back to. And ParsedText::layoutAndExtractLines CONSUMES its input — a
// ParsedText cannot be laid out twice — so the raw text is the only thing that
// can be re-rendered. Raw text on the card is the source of truth; every page we
// draw is laid out again from it.
//
// FORMAT: /sticky/sessions/<session-id>.jsonl, one record per line:
//   {"r":"u","t":<ms>,"x":"<text>"}    a user turn (complete)
//   {"r":"a","t":<ms>,"x":"<text>"}    the first chunk of an agent turn
//   {"r":"a","c":1,"x":"<text>"}       a continuation of that turn
// Agent replies are appended chunk-by-chunk as they stream, so a crash or a
// sleep mid-reply loses at most the last unflushed delta rather than the turn.
// A "turn" on read is a start record plus every continuation record after it.
//
// PAGE INDEX: one PageRef per laid-out page, appended as pages settle. Appending
// is sound because settled lines never reflow (layoutAndExtractLines with
// includeLastLine=false holds the ragged tail back), so pages 1..N-1 are
// immutable and only the final page can still grow.
//
// A PageRef addresses a page by the TURN it starts in plus the line offset
// within that turn, not by a byte offset into the middle of the text. Re-laying
// out a turn from its start is deterministic; resuming from an arbitrary word is
// not, because the hyphenator can split a word across the page boundary and the
// remainder exists only inside the consumed ParsedText. Callers materialise only
// the lines they need (see TranscriptView), so RAM still holds just one page.

#include <cstdint>
#include <string>
#include <vector>

class ConversationSpool {
 public:
  enum class Role : uint8_t { User = 0, Agent = 1 };

  // One turn's worth of text, reassembled from its start record + continuations.
  struct Turn {
    Role role = Role::Agent;
    uint16_t index = 0;       // 0-based turn ordinal within the session
    uint32_t offset = 0;      // byte offset of the turn's START record
    uint32_t nextOffset = 0;  // byte offset of the next turn's start (== size() at the end)
    uint32_t epoch = 0;       // UTC seconds when the turn opened; 0 = unknown
    std::string text;
  };

  // One row for the query rail and the conversation picker: enough to list and
  // jump to a turn without reading the spool back.
  //
  // Held in RAM alongside the page index rather than windowed off the card. A
  // conversation is bounded now that an idle one rotates after a few hours, so
  // this stays small in practice; kMaxTurnRefs is the backstop for a session
  // that somehow does not.
  struct TurnRef {
    uint32_t offset = 0;  // START record of the turn
    uint32_t epoch = 0;   // 0 when the clock could not be trusted
    uint16_t turnIndex = 0;
    uint8_t role = 0;
    char snippet[21] = {0};  // first words, NUL-terminated
  };
  static constexpr size_t kMaxTurnRefs = 2000;

  // Where a page begins. 8 bytes; 1000 pages = 8 KB.
  struct PageRef {
    uint32_t turnOffset = 0;  // START record of the turn this page opens in
    uint16_t turnIndex = 0;
    uint16_t lineOffset = 0;  // lines of that turn already shown on earlier pages
  };

  // The page index is only valid for the geometry it was built with. A font or
  // margin change invalidates it exactly the way ReaderRenderSpec invalidates
  // Section's cache.
  struct RenderSpec {
    int fontId = 0;
    uint16_t viewportWidth = 0;
    uint16_t linesPerPage = 0;
    uint8_t fontPointSize = 0;
    bool operator==(const RenderSpec& o) const {
      return fontId == o.fontId && viewportWidth == o.viewportWidth && linesPerPage == o.linesPerPage &&
             fontPointSize == o.fontPointSize;
    }
    bool operator!=(const RenderSpec& o) const { return !(*this == o); }
  };

  // Enough to list a conversation without opening it. Read straight out of that
  // session's persisted index, so the picker costs one short read per row
  // rather than a spool scan each.
  struct Summary {
    std::string id;
    uint16_t turnCount = 0;
    uint32_t lastEpoch = 0;   // 0 when no turn carries a trustworthy timestamp
    std::string firstQuestion;  // empty when the session has no index yet
  };
  // Session ids are ordinal, so a lexical sort of these is chronological.
  static std::vector<std::string> listSessionIds();
  static bool readSummary(const std::string& id, Summary& out);
  static bool eraseSession(const std::string& id);
  static const char* sessionDir();

  ConversationSpool() = default;

  // Open (creating if needed) the session spool. Passing nullptr resumes the
  // session named in /sticky/sessions/current, or starts a new one if there is
  // none — this is the deep-sleep-wake path.
  bool begin(const char* sessionId = nullptr);
  // Start a fresh session file and point /sticky/sessions/current at it.
  bool startNewSession();
  bool isOpen() const { return open_; }
  const std::string& sessionId() const { return sessionId_; }
  uint32_t size() const { return size_; }
  bool isEmpty() const { return size_ == 0; }

  // --- writing -------------------------------------------------------------
  // A completed user turn (written when ASR finalises).
  bool appendUserTurn(const std::string& text);
  // One streamed agent chunk. The first chunk after a user turn opens the agent
  // turn; later chunks continue it until endAgentTurn().
  bool appendAgentDelta(const std::string& text);
  // Close the open agent turn so the next delta starts a new one.
  void endAgentTurn() { agentTurnOpen_ = false; }
  bool agentTurnOpen() const { return agentTurnOpen_; }

  // --- reading -------------------------------------------------------------
  // Read the turn whose START record is at `offset`. Returns false at EOF or on
  // a malformed record. `index` is carried in by the caller because turn
  // ordinals are positional, not stored.
  bool readTurnAt(uint32_t offset, uint16_t index, Turn& out) const;
  uint32_t firstTurnOffset() const { return 0; }
  uint16_t turnCount() const { return turnCount_; }
  // Offset of the last turn's START record — the resume point after a reset and
  // where "jump to latest" starts laying out.
  uint32_t lastTurnOffset() const { return lastTurnOffset_; }
  uint16_t lastTurnIndex() const { return turnCount_ ? static_cast<uint16_t>(turnCount_ - 1) : 0; }
  // UTC seconds of the most recent turn, or 0 when no turn carries a trustworthy
  // timestamp. Callers must treat 0 as "unknowable" rather than as long ago.
  uint32_t lastTurnEpoch() const { return lastTurnEpoch_; }

  // --- page index ----------------------------------------------------------
  const RenderSpec& spec() const { return spec_; }
  // Drops the index when the geometry changed. Returns true if it was dropped.
  bool applySpec(const RenderSpec& spec);
  void clearIndex() {
    pages_.clear();
    turns_.clear();
  }
  void appendPage(const PageRef& ref) { pages_.push_back(ref); }
  size_t pageCount() const { return pages_.size(); }
  const PageRef& page(size_t i) const { return pages_[i]; }
  bool hasPages() const { return !pages_.empty(); }
  // Index of the first page that opens on or after `turnIndex`, or pageCount().
  size_t firstPageOfTurn(uint16_t turnIndex) const;

  // --- turn table ----------------------------------------------------------
  void appendTurnRef(const Turn& turn);
  size_t turnRefCount() const { return turns_.size(); }
  const TurnRef& turnRef(size_t i) const { return turns_[i]; }
  // Rows the query rail shows: user turns are the questions asked.
  size_t userTurnRefCount() const;

  // Where a saved index stops. It always sits on a TURN BOUNDARY, so resuming
  // needs no mid-turn layout state.
  struct IndexCursor {
    uint32_t spoolOffset = 0;  // START record of the first unindexed turn
    uint16_t turnIndex = 0;    // its index
    uint32_t docLine = 0;      // settled lines in the document before it
  };

  // The page index is derived data written beside the spool. Losing it costs
  // time (a full re-layout), never content.
  bool saveIndex(const IndexCursor& cursor) const;
  // Restores pages_ and `cursor` when the file matches the current spec and the
  // spool it describes. Returns false when the caller must rebuild from scratch.
  bool loadIndex(IndexCursor& cursor);

 private:
  std::string indexPath() const;
  bool scan();  // one pass over the file: count turns, find the last turn start
  bool appendRecord(Role role, bool continuation, const std::string& text);
  static void appendJsonEscaped(std::string& out, const std::string& in);
  // Read one raw line starting at `offset`. Returns the offset just past its
  // newline, or 0 on EOF/error.
  uint32_t readLine(uint32_t offset, std::string& line) const;
  static bool parseRecord(const std::string& line, Role& role, bool& continuation, uint32_t& epoch,
                          std::string& text);

  std::string path_;
  std::string sessionId_;
  bool open_ = false;
  bool agentTurnOpen_ = false;
  uint32_t size_ = 0;
  uint16_t turnCount_ = 0;
  uint32_t lastTurnOffset_ = 0;
  uint32_t lastTurnEpoch_ = 0;

  RenderSpec spec_;
  std::vector<PageRef> pages_;
  std::vector<TurnRef> turns_;
};
