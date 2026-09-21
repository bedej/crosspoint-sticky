#include "ConversationSpool.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

namespace {
constexpr char kSessionDir[] = "/sticky/sessions";
constexpr char kCurrentPath[] = "/sticky/sessions/current";
// Records are short (one streamed delta); anything longer is a corrupt line and
// gets skipped rather than allowed to eat the heap.
constexpr size_t kMaxRecordBytes = 4096;
}  // namespace

// ---------------------------------------------------------------------------
// session catalogue

const char* ConversationSpool::sessionDir() { return kSessionDir; }

std::vector<std::string> ConversationSpool::listSessionIds() {
  std::vector<std::string> ids;
  for (const String& name : Storage.listFiles(kSessionDir, 200)) {
    const std::string n(name.c_str());
    if (n.size() < 7) continue;
    if (n.compare(n.size() - 6, 6, ".jsonl") != 0) continue;
    ids.push_back(n.substr(0, n.size() - 6));
  }
  // Ids are allocated in order (s0001, s0002, ...), so lexical IS chronological
  // — which is just as well, because HalFile exposes no modification time and
  // SdFat only records one if a date-time callback is installed, which this
  // firmware never does.
  std::sort(ids.begin(), ids.end());
  return ids;
}

bool ConversationSpool::eraseSession(const std::string& id) {
  const std::string base = std::string(kSessionDir) + "/" + id;
  const bool spool = Storage.remove((base + ".jsonl").c_str());
  // The index is derived data; losing it alone would only cost a rebuild, but
  // leaving it behind after the spool is gone would strand a stale file.
  Storage.remove((base + ".idx").c_str());
  return spool;
}

// ---------------------------------------------------------------------------
// session lifecycle

bool ConversationSpool::begin(const char* sessionId) {
  open_ = false;
  agentTurnOpen_ = false;
  size_ = 0;
  turnCount_ = 0;
  lastTurnOffset_ = 0;
  lastTurnEpoch_ = 0;
  pages_.clear();
  turns_.clear();

  Storage.ensureDirectoryExists(kSessionDir);

  if (sessionId != nullptr && *sessionId != '\0') {
    sessionId_ = sessionId;
  } else {
    // Resume whatever session we were in before the last reset. Deep-sleep wake
    // is a reset, so this is the normal path, not an error path.
    char buf[64] = {0};
    const size_t n = Storage.readFileToBuffer(kCurrentPath, buf, sizeof(buf));
    sessionId_.assign(buf, n);
    while (!sessionId_.empty() && (sessionId_.back() == '\n' || sessionId_.back() == '\r')) {
      sessionId_.pop_back();
    }
    if (sessionId_.empty()) return startNewSession();
  }

  path_ = std::string(kSessionDir) + "/" + sessionId_ + ".jsonl";
  if (!Storage.exists(path_.c_str())) {
    // Named session that does not exist yet: create it empty.
    HalFile f = Storage.open(path_.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (!f) {
      LOG_ERR("SPOOL", "cannot create %s", path_.c_str());
      return false;
    }
    f.close();
    Storage.writeFile(kCurrentPath, String(sessionId_.c_str()));
  }
  open_ = true;
  return scan();
}

bool ConversationSpool::startNewSession() {
  char name[32];
  // Monotonic-enough and collision-free across resets: the RTC-backed clock is
  // not guaranteed here, so fall back to a counter file would cost a second
  // write per boot. millis() since boot plus an existence check is sufficient.
  uint32_t n = 1;
  for (;; ++n) {
    snprintf(name, sizeof(name), "s%04u", static_cast<unsigned>(n));
    const std::string p = std::string(kSessionDir) + "/" + name + ".jsonl";
    if (!Storage.exists(p.c_str())) break;
    if (n > 9998) break;  // wrap rather than spin forever
  }
  sessionId_ = name;
  path_ = std::string(kSessionDir) + "/" + sessionId_ + ".jsonl";

  HalFile f = Storage.open(path_.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!f) {
    LOG_ERR("SPOOL", "cannot create %s", path_.c_str());
    open_ = false;
    return false;
  }
  f.close();
  Storage.writeFile(kCurrentPath, String(sessionId_.c_str()));

  open_ = true;
  agentTurnOpen_ = false;
  size_ = 0;
  turnCount_ = 0;
  lastTurnOffset_ = 0;
  lastTurnEpoch_ = 0;
  pages_.clear();
  turns_.clear();
  LOG_INF("SPOOL", "new session %s", sessionId_.c_str());
  return true;
}

// ---------------------------------------------------------------------------
// writing

void ConversationSpool::appendJsonEscaped(std::string& out, const std::string& in) {
  for (const char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        // Control characters would make the line unparseable; UTF-8 continuation
        // bytes (>= 0x80) pass through untouched.
        if (static_cast<unsigned char>(c) < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c) & 0xFF);
          out += esc;
        } else {
          out += c;
        }
        break;
    }
  }
}

bool ConversationSpool::appendRecord(const Role role, const bool continuation, const std::string& text) {
  if (!open_ || text.empty()) return false;

  std::string line;
  line.reserve(text.size() + 48);
  line += "{\"r\":\"";
  line += (role == Role::User) ? 'u' : 'a';
  line += "\",";
  uint32_t epoch = 0;
  if (continuation) {
    line += "\"c\":1,";
  } else {
    // Only a turn's START record is timestamped, so this is one I2C read per
    // turn rather than per streamed delta.
    //
    // millis() used to go here, which was worse than useless: it resets on every
    // deep-sleep wake, so a resumed session's timestamps ran BACKWARDS partway
    // through its own file. When the clock cannot be trusted the field is now
    // omitted entirely — an absent timestamp reads as "unknown", where a wrong
    // one silently answers "how long since we last spoke?".
    if (halClock.nowEpoch(epoch)) {
      char ts[32];
      snprintf(ts, sizeof(ts), "\"t\":%lu,", static_cast<unsigned long>(epoch));
      line += ts;
    }
  }
  line += "\"x\":\"";
  appendJsonEscaped(line, text);
  line += "\"}\n";

  // Opened per record and closed immediately: the SD card and the e-paper share
  // the SPI bus, and holding a write handle open across a render is how you get
  // a half-written line on the card after a panel refresh.
  HalFile f = Storage.open(path_.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (!f) {
    LOG_ERR("SPOOL", "append open failed");
    return false;
  }
  const size_t wrote = f.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
  f.flush();
  f.close();
  if (wrote != line.size()) {
    LOG_ERR("SPOOL", "short write %u/%u", static_cast<unsigned>(wrote), static_cast<unsigned>(line.size()));
    return false;
  }

  if (!continuation) {
    lastTurnOffset_ = size_;
    if (epoch != 0) lastTurnEpoch_ = epoch;
    if (turnCount_ < 0xFFFF) turnCount_++;
  }
  size_ += static_cast<uint32_t>(line.size());
  return true;
}

bool ConversationSpool::appendUserTurn(const std::string& text) {
  agentTurnOpen_ = false;
  return appendRecord(Role::User, false, text);
}

bool ConversationSpool::appendAgentDelta(const std::string& text) {
  const bool continuation = agentTurnOpen_;
  if (!appendRecord(Role::Agent, continuation, text)) return false;
  agentTurnOpen_ = true;
  return true;
}

// ---------------------------------------------------------------------------
// reading

uint32_t ConversationSpool::readLine(const uint32_t offset, std::string& line) const {
  line.clear();
  if (!open_ || offset >= size_) return 0;
  HalFile f;
  if (!Storage.openFileForRead("SPOOL", path_, f) || !f) return 0;
  if (!f.seek(offset)) {
    f.close();
    return 0;
  }
  uint32_t pos = offset;
  for (;;) {
    const int c = f.read();
    if (c < 0) break;
    pos++;
    if (c == '\n') {
      f.close();
      return pos;
    }
    if (line.size() < kMaxRecordBytes) line.push_back(static_cast<char>(c));
  }
  f.close();
  // Trailing partial line (power lost mid-write): report it as consumed so the
  // caller stops rather than looping on the same offset.
  return line.empty() ? 0 : pos;
}

bool ConversationSpool::parseRecord(const std::string& line, Role& role, bool& continuation, uint32_t& epoch,
                                    std::string& text) {
  if (line.empty() || line[0] != '{') return false;
  JsonDocument doc;
  if (deserializeJson(doc, line.c_str(), line.size())) return false;
  const char* r = doc["r"] | "";
  role = (r[0] == 'u') ? Role::User : Role::Agent;
  continuation = (doc["c"] | 0) != 0;
  text = static_cast<const char*>(doc["x"] | "");

  // Spools written before timestamps were real carry millis()-since-boot here.
  // Anything below this is not a plausible epoch, and treating it as one would
  // date the turn to 1970 and make every old conversation look infinitely stale.
  constexpr uint32_t kMinPlausibleEpoch = 1000000000u;  // 2001-09-09
  const uint32_t t = doc["t"] | 0u;
  epoch = (t >= kMinPlausibleEpoch) ? t : 0u;
  return true;
}

bool ConversationSpool::readTurnAt(const uint32_t offset, const uint16_t index, Turn& out) const {
  out.text.clear();
  out.index = index;
  out.offset = offset;
  out.nextOffset = offset;
  if (!open_ || offset >= size_) return false;

  std::string line;
  uint32_t pos = readLine(offset, line);
  if (pos == 0) return false;

  Role role = Role::Agent;
  bool continuation = false;
  uint32_t epoch = 0;
  std::string text;
  if (!parseRecord(line, role, continuation, epoch, text)) return false;
  out.role = role;
  out.text = text;
  out.epoch = epoch;
  out.nextOffset = pos;

  // An agent turn continues across every following {"c":1} record.
  if (role == Role::Agent) {
    while (pos < size_) {
      const uint32_t next = readLine(pos, line);
      if (next == 0) break;
      Role r2 = Role::Agent;
      bool cont2 = false;
      uint32_t e2 = 0;
      std::string t2;
      if (!parseRecord(line, r2, cont2, e2, t2)) break;
      if (r2 != Role::Agent || !cont2) break;  // a new turn starts here
      out.text += t2;
      pos = next;
      out.nextOffset = pos;
    }
  }
  return true;
}

bool ConversationSpool::scan() {
  turnCount_ = 0;
  lastTurnOffset_ = 0;
  lastTurnEpoch_ = 0;
  size_ = 0;
  agentTurnOpen_ = false;

  HalFile f;
  if (!Storage.openFileForRead("SPOOL", path_, f) || !f) return false;
  const uint32_t fileSize = static_cast<uint32_t>(f.size());

  uint32_t pos = 0;
  uint32_t lineStart = 0;
  std::string line;
  bool sawAgentTail = false;
  while (pos < fileSize) {
    const int c = f.read();
    if (c < 0) break;
    pos++;
    if (c != '\n') {
      if (line.size() < kMaxRecordBytes) line.push_back(static_cast<char>(c));
      continue;
    }
    Role role = Role::Agent;
    bool continuation = false;
    uint32_t epoch = 0;
    std::string text;
    if (parseRecord(line, role, continuation, epoch, text)) {
      if (!continuation) {
        lastTurnOffset_ = lineStart;
        if (epoch != 0) lastTurnEpoch_ = epoch;
        if (turnCount_ < 0xFFFF) turnCount_++;
      }
      sawAgentTail = (role == Role::Agent);
    }
    line.clear();
    lineStart = pos;
  }
  f.close();

  // A trailing partial line is a power loss mid-write: ignore it, and let the
  // next append start on a fresh line boundary.
  size_ = lineStart;
  // We resumed after a reset, so any agent turn that was streaming is over as
  // far as this boot is concerned — the next delta opens a new turn.
  (void)sawAgentTail;
  agentTurnOpen_ = false;

  LOG_INF("SPOOL", "%s: %u bytes, %u turns", sessionId_.c_str(), static_cast<unsigned>(size_),
          static_cast<unsigned>(turnCount_));
  return true;
}

// ---------------------------------------------------------------------------
// page index

bool ConversationSpool::applySpec(const RenderSpec& spec) {
  if (spec_ == spec) return false;
  spec_ = spec;
  pages_.clear();
  return true;
}

namespace {
// Bumped whenever the on-disk layout below changes, so a stale file is
// rejected rather than misread.
constexpr uint32_t kIndexMagic = 0x58444953;  // "SIDX"
constexpr uint8_t kIndexVersion = 2;  // v2 adds the turn table

struct IndexHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t pad[3];
  int32_t fontId;
  uint16_t viewportWidth;
  uint16_t linesPerPage;
  uint8_t fontPointSize;
  uint8_t pad2[3];
  uint32_t spoolSize;    // the spool this index was built against
  uint32_t spoolOffset;  // resume cursor
  uint32_t docLine;
  uint16_t turnIndex;
  uint16_t pad3;
  uint32_t pageCount;
  uint32_t turnRefCount;  // v2
};
}  // namespace

std::string ConversationSpool::indexPath() const {
  return std::string(kSessionDir) + "/" + sessionId_ + ".idx";
}

bool ConversationSpool::saveIndex(const IndexCursor& cursor) const {
  if (!open_) return false;
  IndexHeader h{};
  h.magic = kIndexMagic;
  h.version = kIndexVersion;
  h.fontId = spec_.fontId;
  h.viewportWidth = spec_.viewportWidth;
  h.linesPerPage = spec_.linesPerPage;
  h.fontPointSize = spec_.fontPointSize;
  h.spoolSize = size_;
  h.spoolOffset = cursor.spoolOffset;
  h.docLine = cursor.docLine;
  h.turnIndex = cursor.turnIndex;
  h.pageCount = static_cast<uint32_t>(pages_.size());
  h.turnRefCount = static_cast<uint32_t>(turns_.size());

  const std::string path = indexPath();
  HalFile f = Storage.open(path.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!f) {
    LOG_ERR("SPOOL", "cannot write %s", path.c_str());
    return false;
  }
  bool ok = f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h)) == sizeof(h);
  if (ok && !pages_.empty()) {
    const size_t bytes = pages_.size() * sizeof(PageRef);
    ok = f.write(reinterpret_cast<const uint8_t*>(pages_.data()), bytes) == bytes;
  }
  if (ok && !turns_.empty()) {
    const size_t bytes = turns_.size() * sizeof(TurnRef);
    ok = f.write(reinterpret_cast<const uint8_t*>(turns_.data()), bytes) == bytes;
  }
  f.flush();
  f.close();
  if (!ok) {
    // A half-written index would be read back as truth, so remove it and let
    // the next boot rebuild instead.
    Storage.remove(path.c_str());
    LOG_ERR("SPOOL", "index write failed, removed");
    return false;
  }
  return true;
}

bool ConversationSpool::loadIndex(IndexCursor& cursor) {
  if (!open_) return false;
  HalFile f;
  if (!Storage.openFileForRead("SPOOL", indexPath(), f) || !f) return false;

  IndexHeader h{};
  bool ok = f.read(&h, sizeof(h)) == static_cast<int>(sizeof(h));
  ok = ok && h.magic == kIndexMagic && h.version == kIndexVersion;
  // A spec change means the file describes a different layout entirely, and a
  // spool shorter than the index means it was replaced underneath us.
  ok = ok && h.fontId == spec_.fontId && h.viewportWidth == spec_.viewportWidth &&
       h.linesPerPage == spec_.linesPerPage && h.fontPointSize == spec_.fontPointSize;
  ok = ok && h.spoolSize <= size_ && h.spoolOffset <= size_;
  if (!ok) {
    f.close();
    return false;
  }

  pages_.clear();
  turns_.clear();
  if (h.pageCount > 0) {
    if (h.pageCount > 65535) {  // refuse an absurd count rather than allocate on it
      f.close();
      return false;
    }
    pages_.resize(h.pageCount);
    const size_t bytes = pages_.size() * sizeof(PageRef);
    if (f.read(pages_.data(), bytes) != static_cast<int>(bytes)) {
      pages_.clear();
      f.close();
      return false;
    }
  }
  if (h.turnRefCount > 0) {
    if (h.turnRefCount > kMaxTurnRefs) {
      pages_.clear();
      f.close();
      return false;
    }
    turns_.resize(h.turnRefCount);
    const size_t bytes = turns_.size() * sizeof(TurnRef);
    if (f.read(turns_.data(), bytes) != static_cast<int>(bytes)) {
      // The page index alone is still usable; only the rail loses its rows. But
      // a truncated file means something is wrong, so rebuild rather than guess.
      pages_.clear();
      turns_.clear();
      f.close();
      return false;
    }
  }
  f.close();

  cursor.spoolOffset = h.spoolOffset;
  cursor.turnIndex = h.turnIndex;
  cursor.docLine = h.docLine;
  LOG_INF("SPOOL", "index loaded: %u pages, %u turns, resume at %u/%u", static_cast<unsigned>(pages_.size()),
          static_cast<unsigned>(turns_.size()), static_cast<unsigned>(cursor.spoolOffset),
          static_cast<unsigned>(size_));
  return true;
}

void ConversationSpool::appendTurnRef(const Turn& turn) {
  if (turns_.size() >= kMaxTurnRefs) return;  // backstop; paging is unaffected
  TurnRef ref;
  ref.offset = turn.offset;
  ref.epoch = turn.epoch;
  ref.turnIndex = turn.index;
  ref.role = static_cast<uint8_t>(turn.role);

  // First words of the turn, collapsed to single spaces and cut on a word
  // boundary where one is available, so a rail row does not end mid-word.
  const size_t cap = sizeof(ref.snippet) - 1;
  size_t n = 0;
  bool pendingSpace = false;
  for (const char c : turn.text) {
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      if (n > 0) pendingSpace = true;
      continue;
    }
    if (pendingSpace) {
      if (n >= cap) break;
      ref.snippet[n++] = ' ';
      pendingSpace = false;
    }
    if (n >= cap) break;
    ref.snippet[n++] = c;
  }
  if (n == cap) {
    // Cut on the last word boundary so a row does not end mid-word.
    for (size_t i = n; i-- > 0;) {
      if (ref.snippet[i] == ' ') {
        n = i;
        break;
      }
    }
  }

  // The cut above is by BYTE, and this firmware renders 34 languages — so walk
  // back to the start of the final codepoint and drop it if the cut landed
  // inside it. A half-written UTF-8 sequence renders as a missing glyph.
  size_t lead = n;
  while (lead > 0 && (static_cast<unsigned char>(ref.snippet[lead - 1]) & 0xC0) == 0x80) lead--;
  if (lead > 0) {
    const unsigned char c = static_cast<unsigned char>(ref.snippet[lead - 1]);
    size_t needed = 1;
    if ((c & 0xE0) == 0xC0) {
      needed = 2;
    } else if ((c & 0xF0) == 0xE0) {
      needed = 3;
    } else if ((c & 0xF8) == 0xF0) {
      needed = 4;
    }
    if (lead - 1 + needed > n) n = lead - 1;
  }

  ref.snippet[n] = '\0';
  turns_.push_back(ref);
}

size_t ConversationSpool::userTurnRefCount() const {
  size_t n = 0;
  for (const TurnRef& t : turns_) {
    if (t.role == static_cast<uint8_t>(Role::User)) n++;
  }
  return n;
}

// Reads a session's index directly, without opening its spool. Defined here so
// the on-disk layout stays knowledge of this file alone.
bool ConversationSpool::readSummary(const std::string& id, Summary& out) {
  out = Summary{};
  out.id = id;

  HalFile f;
  const std::string path = std::string(kSessionDir) + "/" + id + ".idx";
  if (!Storage.openFileForRead("SPOOL", path, f) || !f) return false;

  IndexHeader h{};
  if (f.read(&h, sizeof(h)) != static_cast<int>(sizeof(h)) || h.magic != kIndexMagic ||
      h.version != kIndexVersion) {
    f.close();
    return false;  // never indexed, or indexed by an older build
  }
  out.turnCount = h.turnIndex;

  if (h.turnRefCount == 0 || h.turnRefCount > kMaxTurnRefs) {
    f.close();
    return true;  // header is usable even with no turn table
  }

  const size_t tableStart = sizeof(IndexHeader) + static_cast<size_t>(h.pageCount) * sizeof(PageRef);

  // The first question. Turn 0 is normally the user's, but an agent greeting
  // would push it along, so scan a few rather than assuming.
  if (f.seek(tableStart)) {
    const uint32_t probe = h.turnRefCount < 8 ? h.turnRefCount : 8;
    for (uint32_t i = 0; i < probe; ++i) {
      TurnRef ref;
      if (f.read(&ref, sizeof(ref)) != static_cast<int>(sizeof(ref))) break;
      if (ref.role == static_cast<uint8_t>(Role::User) && ref.snippet[0] != '\0') {
        out.firstQuestion = ref.snippet;
        break;
      }
    }
  }

  // And the newest timestamp, for "2 hours ago".
  const size_t lastEntry = tableStart + static_cast<size_t>(h.turnRefCount - 1) * sizeof(TurnRef);
  if (f.seek(lastEntry)) {
    TurnRef ref;
    if (f.read(&ref, sizeof(ref)) == static_cast<int>(sizeof(ref))) out.lastEpoch = ref.epoch;
  }
  f.close();
  return true;
}

size_t ConversationSpool::firstPageOfTurn(const uint16_t turnIndex) const {
  for (size_t i = 0; i < pages_.size(); ++i) {
    if (pages_[i].turnIndex >= turnIndex) return i;
  }
  return pages_.size();
}
