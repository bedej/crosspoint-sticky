#include "ConversationSpool.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace {
constexpr char kSessionDir[] = "/sticky/sessions";
constexpr char kCurrentPath[] = "/sticky/sessions/current";
// Records are short (one streamed delta); anything longer is a corrupt line and
// gets skipped rather than allowed to eat the heap.
constexpr size_t kMaxRecordBytes = 4096;
}  // namespace

// ---------------------------------------------------------------------------
// session lifecycle

bool ConversationSpool::begin(const char* sessionId) {
  open_ = false;
  agentTurnOpen_ = false;
  size_ = 0;
  turnCount_ = 0;
  lastTurnOffset_ = 0;
  pages_.clear();

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
  pages_.clear();
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
  if (continuation) {
    line += "\"c\":1,";
  } else {
    char ts[32];
    snprintf(ts, sizeof(ts), "\"t\":%lu,", static_cast<unsigned long>(millis()));
    line += ts;
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

bool ConversationSpool::parseRecord(const std::string& line, Role& role, bool& continuation, std::string& text) {
  if (line.empty() || line[0] != '{') return false;
  JsonDocument doc;
  if (deserializeJson(doc, line.c_str(), line.size())) return false;
  const char* r = doc["r"] | "";
  role = (r[0] == 'u') ? Role::User : Role::Agent;
  continuation = (doc["c"] | 0) != 0;
  text = static_cast<const char*>(doc["x"] | "");
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
  std::string text;
  if (!parseRecord(line, role, continuation, text)) return false;
  out.role = role;
  out.text = text;
  out.nextOffset = pos;

  // An agent turn continues across every following {"c":1} record.
  if (role == Role::Agent) {
    while (pos < size_) {
      const uint32_t next = readLine(pos, line);
      if (next == 0) break;
      Role r2 = Role::Agent;
      bool cont2 = false;
      std::string t2;
      if (!parseRecord(line, r2, cont2, t2)) break;
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
    std::string text;
    if (parseRecord(line, role, continuation, text)) {
      if (!continuation) {
        lastTurnOffset_ = lineStart;
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
constexpr uint8_t kIndexVersion = 1;

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
  f.close();

  cursor.spoolOffset = h.spoolOffset;
  cursor.turnIndex = h.turnIndex;
  cursor.docLine = h.docLine;
  LOG_INF("SPOOL", "index loaded: %u pages, resume at %u/%u", static_cast<unsigned>(pages_.size()),
          static_cast<unsigned>(cursor.spoolOffset), static_cast<unsigned>(size_));
  return true;
}

size_t ConversationSpool::firstPageOfTurn(const uint16_t turnIndex) const {
  for (size_t i = 0; i < pages_.size(); ++i) {
    if (pages_[i].turnIndex >= turnIndex) return i;
  }
  return pages_.size();
}
