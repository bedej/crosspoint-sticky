#include "MainHistory.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "../activities/transcript/ConversationSpool.h"
#include "../network/HttpDownloader.h"

namespace {

// Small pages, deliberately. A 200-turn window is ~60 KB of JSON and parsing
// that in one document risks the internal-RAM allocation that ArduinoJson
// makes; 25 turns is ~8 KB, and a page is also the unit of progress the user
// sees moving.
constexpr uint16_t kPageTurns = 25;
constexpr uint16_t kMaxTarget = 400;   // the device caches a window, not the chat
constexpr uint32_t kTaskStack = 8192;  // HTTP + JSON, not the render path
// A cache this far behind is not "behind" any more: appending that much costs
// more than the window refetch it would be approximating, and the dedupe below
// has that many more chances to be wrong. Past this the window is refetched.
constexpr uint16_t kMaxCatchUp = 300;
// Local turns awaiting the mirror. Short records, but the list is read into RAM
// and the honest answer to a long backlog is the refetch, not a bigger array.
constexpr size_t kMaxAnchors = 64;

struct Job {
  MainHistory::Target to;
  uint16_t tail = 0;
  bool fresh = false;
  uint32_t backfill = 0;
  bool catchUp = false;
  int64_t sinceSeq = 0;
};

struct Shared {
  SemaphoreHandle_t mtx = nullptr;
  MainHistory::Status status;
  TaskHandle_t task = nullptr;
  Job job;
  // Set while a turn is in flight; the worker parks between pages.
  bool held = false;
};

Shared& sh() {
  static Shared s;
  if (!s.mtx) s.mtx = xSemaphoreCreateMutex();
  return s;
}

struct Lock {
  Lock() { xSemaphoreTake(sh().mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(sh().mtx); }
};

void setState(const MainHistory::State state, const char* note) {
  Lock lk;
  sh().status.state = state;
  if (note) {
    snprintf(sh().status.note, sizeof(sh().status.note), "%s", note);
  }
}

void setProgress(const uint16_t turns) {
  Lock lk;
  sh().status.turns = turns;
}

void setHighestSeq(const int64_t seq) {
  Lock lk;
  sh().status.highestSeq = seq;
}

std::string urlFor(const MainHistory::Target& to, const std::string& query) {
  char head[96];
  snprintf(head, sizeof(head), "http://%s:%u/v1/history?", to.host.c_str(), static_cast<unsigned>(to.port));
  std::string url(head);
  url += query;
  if (!to.token.empty()) {
    url += "&token=";
    url += to.token;
  }
  return url;
}

// Writes the staging file. Kept open across pages: reopening per page costs an
// SD directory walk each time, and this runs while the user may be talking.
class Staging {
 public:
  bool open(const char* path) {
    Storage.ensureDirectoryExists(ConversationSpool::sessionDir());
    Storage.remove(path);
    file_ = Storage.open(path, O_WRITE | O_CREAT | O_TRUNC);
    return static_cast<bool>(file_);
  }

  bool write(const std::string& line) {
    if (!file_) return false;
    return file_.write(line.data(), line.size()) == static_cast<int>(line.size());
  }

  void close() {
    if (file_) file_.close();
  }

 private:
  HalFile file_;
};

// `meta=1` first: the floor is the oldest seq of the window we want, and the
// only way to then read it in the order the spool can write.
bool fetchFloor(const Job& job, int64_t& floorSeq, bool& empty) {
  char q[96];
  snprintf(q, sizeof(q), "tail=%u&meta=1%s", static_cast<unsigned>(job.tail), job.fresh ? "&fresh=1" : "");
  std::string query(q);
  if (job.backfill > 0) {
    char b[48];
    snprintf(b, sizeof(b), "&backfill=%lu", static_cast<unsigned long>(job.backfill));
    query += b;
  }
  std::string body;
  if (!HttpDownloader::fetchUrl(urlFor(job.to, query), body)) {
    setState(MainHistory::State::Failed, "history: service unreachable");
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    setState(MainHistory::State::Failed, "history: bad reply");
    return false;
  }
  if (doc["floor_seq"].isNull()) {
    empty = true;  // the mirror holds nothing yet
    return true;
  }
  floorSeq = doc["floor_seq"].as<int64_t>();
  empty = false;
  LOG_INF("MHIST", "window floor=%lld held=%u", static_cast<long long>(floorSeq),
          static_cast<unsigned>(doc["held"] | 0));
  return true;
}

// One page forward. Returns false on a transport or parse failure; `done` means
// the mirror had nothing more to give.
bool fetchPage(const Job& job, int64_t& cursor, Staging& out, uint16_t& written, bool& done) {
  char q[96];
  snprintf(q, sizeof(q), "from_seq=%lld&limit=%u", static_cast<long long>(cursor), static_cast<unsigned>(kPageTurns));
  std::string body;
  if (!HttpDownloader::fetchUrl(urlFor(job.to, q), body)) {
    setState(MainHistory::State::Failed, "history: transfer failed");
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    setState(MainHistory::State::Failed, "history: bad reply");
    return false;
  }
  JsonArray turns = doc["turns"].as<JsonArray>();
  if (turns.isNull() || turns.size() == 0) {
    done = true;
    return true;
  }
  int64_t highest = cursor;
  for (JsonObject t : turns) {
    const char* text = t["t"] | "";
    if (!text || !*text) continue;
    const bool user = std::string(t["r"] | "a") == "u";
    // `at` is milliseconds from Muse; the spool stores seconds, and 0 reads
    // back as "unknown", which is the honest value when Muse sent none.
    const int64_t atMs = t["at"] | 0;
    const uint32_t epoch = atMs > 0 ? static_cast<uint32_t>(atMs / 1000) : 0;
    const std::string line = ConversationSpool::recordLine(
        user ? ConversationSpool::Role::User : ConversationSpool::Role::Agent, text, epoch);
    if (!out.write(line)) {
      setState(MainHistory::State::Failed, "history: SD write failed");
      return false;
    }
    written++;
    const int64_t seq = t["seq"] | 0;
    highest = std::max(highest, seq);
  }
  setProgress(written);
  // Strictly past the highest seq we just took, so a page can never repeat.
  cursor = highest + 1;
  if (turns.size() < kPageTurns) done = true;
  return true;
}

// Between pages, while the user is talking: the radio is shared, and a page in
// flight is the thing that starves the audio going up over Bluetooth.
void parkWhileHeld() {
  bool waited = false;
  for (;;) {
    bool held = false;
    {
      Lock lk;
      held = sh().held;
    }
    if (!held) break;
    if (!waited) {
      waited = true;
      LOG_INF("MHIST", "holding the fetch while a turn is in flight");
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  if (waited) LOG_INF("MHIST", "resuming the fetch");
}

// Pages forward from `cursor`, writing to `out`, until the mirror runs out or
// `cap` turns have been written. Returns false with the reason already set.
bool pageForward(const Job& job, int64_t cursor, Staging& out, const uint16_t cap, uint16_t& written, bool& done) {
  bool ok = true;
  done = false;
  while (ok && !done && written < cap) {
    parkWhileHeld();
    ok = fetchPage(job, cursor, out, written, done);
    setHighestSeq(cursor - 1);  // fetchPage leaves the cursor one past what it took
  }
  return ok;
}

// The cache is merely behind: fetch only what is past it, to be APPENDED rather
// than swapped in. The floor request carries `fresh=1`, so the service polls
// Muse before answering — which both brings in the turns made elsewhere and
// puts this device's own recent turns into the mirror, where applyCatchUp()
// expects to find them.
void catchUpWorker(const Job& job) {
  int64_t newestSeq = 0;
  bool empty = false;
  if (!fetchFloor(job, newestSeq, empty)) return;  // setState already said why
  if (empty || newestSeq <= job.sinceSeq) {
    setHighestSeq(job.sinceSeq);
    setState(MainHistory::State::Ready, "history: up to date");
    LOG_INF("MHIST", "catch-up: nothing past seq %lld", static_cast<long long>(job.sinceSeq));
    return;
  }
  Staging out;
  if (!out.open(MainHistory::catchUpPath())) {
    setState(MainHistory::State::Failed, "history: cannot stage");
    return;
  }
  uint16_t written = 0;
  bool done = false;
  const bool ok = pageForward(job, job.sinceSeq + 1, out, kMaxCatchUp, written, done);
  out.close();
  if (!ok) {
    Storage.remove(MainHistory::catchUpPath());
    return;
  }
  if (!done && written >= kMaxCatchUp) {
    // Further behind than an append is worth. The window path is correct at any
    // distance, so hand it over rather than appending part of the gap.
    Storage.remove(MainHistory::catchUpPath());
    setState(MainHistory::State::Failed, "history: too far behind");
    return;
  }
  char note[48];
  snprintf(note, sizeof(note), "history: +%u turns", static_cast<unsigned>(written));
  setState(MainHistory::State::Ready, note);
  LOG_INF("MHIST", "catch-up staged %u turns, newest seq %lld", static_cast<unsigned>(written),
          static_cast<long long>(newestSeq));
}

// A fresh window, oldest-first, into a file that replaces the cache.
void windowWorker(const Job& job) {
  int64_t floorSeq = 0;
  bool empty = false;
  if (!fetchFloor(job, floorSeq, empty)) return;  // setState already said why
  if (empty) {
    setState(MainHistory::State::Ready, "history: nothing to load");
    return;
  }
  Staging out;
  if (!out.open(MainHistory::stagingPath())) {
    setState(MainHistory::State::Failed, "history: cannot stage");
    return;
  }
  uint16_t written = 0;
  bool done = false;
  const bool ok = pageForward(job, floorSeq, out, job.tail, written, done);
  out.close();
  if (!ok) {
    Storage.remove(MainHistory::stagingPath());
    return;
  }
  char note[48];
  snprintf(note, sizeof(note), "history: %u turns", static_cast<unsigned>(written));
  setState(MainHistory::State::Ready, note);
  LOG_INF("MHIST", "staged %u turns", static_cast<unsigned>(written));
}

void worker(void*) {
  Job job;
  {
    Lock lk;
    job = sh().job;
  }
  if (job.catchUp) {
    catchUpWorker(job);
  } else {
    windowWorker(job);
  }

  {
    Lock lk;
    sh().task = nullptr;
  }
  vTaskDelete(nullptr);
}

}  // namespace

const char* MainHistory::stagingPath() {
  static const std::string p = std::string(ConversationSpool::sessionDir()) + "/main.staging.jsonl";
  return p.c_str();
}

const char* MainHistory::catchUpPath() {
  static const std::string p = std::string(ConversationSpool::sessionDir()) + "/main.catchup.jsonl";
  return p.c_str();
}

namespace {
std::string markPath() { return std::string(ConversationSpool::sessionDir()) + "/main.mark"; }
}  // namespace

bool MainHistory::readMark(Mark& out) {
  HalFile f;
  if (!Storage.openFileForRead("MHIST", markPath().c_str(), f) || !f) return false;
  const size_t n = f.size();
  if (n == 0 || n > 256) return false;
  std::string buf;
  buf.resize(n);
  const int got = f.read(&buf[0], n);
  if (got <= 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, buf.c_str(), static_cast<size_t>(got))) return false;
  if (doc["seq"].isNull()) return false;
  out.seq = doc["seq"].as<int64_t>();
  out.size = doc["size"] | 0;
  return out.seq > 0;
}

bool MainHistory::writeMark(const Mark& mark) {
  char body[96];
  snprintf(body, sizeof(body), "{\"seq\":%lld,\"size\":%lu}\n", static_cast<long long>(mark.seq),
           static_cast<unsigned long>(mark.size));
  HalFile f = Storage.open(markPath().c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!f) {
    LOG_ERR("MHIST", "cannot write the mark");
    return false;
  }
  const size_t len = strlen(body);
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(body), len) == static_cast<int>(len);
  f.flush();
  f.close();
  if (!ok) {
    // A mark that does not describe the spool would have the next catch-up
    // append turns the cache already holds, so remove it and refetch instead.
    Storage.remove(markPath().c_str());
    return false;
  }
  LOG_INF("MHIST", "mark: seq %lld at %lu bytes", static_cast<long long>(mark.seq),
          static_cast<unsigned long>(mark.size));
  return true;
}

bool MainHistory::start(const Target& to, const uint16_t tail, const bool fresh, const uint32_t backfill) {
  if (to.host.empty() || to.port == 0) {
    setState(State::Failed, "history: no service configured");
    return false;
  }
  {
    Lock lk;
    if (sh().task != nullptr) return false;  // one at a time
    sh().status = Status{};
    sh().status.state = State::Working;
    sh().status.target = std::min(tail, kMaxTarget);
    snprintf(sh().status.note, sizeof(sh().status.note), "history: loading");
    sh().job = Job{to, std::min(tail, kMaxTarget), fresh, backfill};
  }
  TaskHandle_t handle = nullptr;
  if (xTaskCreate(&worker, "mainhist", kTaskStack, nullptr, 1, &handle) != pdPASS) {
    setState(State::Failed, "history: no task");
    return false;
  }
  Lock lk;
  sh().task = handle;
  return true;
}

bool MainHistory::startCatchUp(const Target& to, const int64_t sinceSeq) {
  if (to.host.empty() || to.port == 0) {
    setState(State::Failed, "history: no service configured");
    return false;
  }
  if (sinceSeq <= 0) return false;  // nothing to count from; the window path owns that
  {
    Lock lk;
    if (sh().task != nullptr) return false;  // one at a time
    sh().status = Status{};
    sh().status.state = State::Working;
    sh().status.catchUp = true;
    sh().job = Job{to, /*tail=*/1, /*fresh=*/true, /*backfill=*/0, /*catchUp=*/true, sinceSeq};
  }
  TaskHandle_t handle = nullptr;
  if (xTaskCreate(&worker, "mainhist", kTaskStack, nullptr, 1, &handle) != pdPASS) {
    setState(State::Failed, "history: no task");
    return false;
  }
  Lock lk;
  sh().task = handle;
  return true;
}

MainHistory::Status MainHistory::status() {
  Lock lk;
  return sh().status;
}

void MainHistory::hold(const bool held) {
  Lock lk;
  sh().held = held;
}

bool MainHistory::busy() {
  Lock lk;
  return sh().task != nullptr;
}

void MainHistory::clear() {
  Lock lk;
  if (sh().task != nullptr) return;
  sh().status = Status{};
}

bool MainHistory::commit() {
  if (busy()) return false;
  if (!Storage.exists(stagingPath())) return false;
  const std::string dir = ConversationSpool::sessionDir();
  const std::string live = dir + "/" + ConversationSpool::kMainSessionId + ".jsonl";
  const std::string index = dir + "/" + ConversationSpool::kMainSessionId + ".idx";
  Storage.remove(live.c_str());
  // The index describes the file being replaced; leaving it would have the view
  // read pages that no longer exist.
  Storage.remove(index.c_str());
  if (!Storage.rename(stagingPath(), live.c_str())) {
    LOG_ERR("MHIST", "could not rename the staged window into place");
    return false;
  }
  // The window IS the mirror's prefix as of this seq, with nothing local after
  // it — so the next open can append rather than fetch the whole thing again.
  Mark mark;
  mark.seq = status().highestSeq;
  mark.size = ConversationSpool::sessionBytes(ConversationSpool::kMainSessionId);
  if (mark.seq > 0) {
    writeMark(mark);
  } else {
    Storage.remove(markPath().c_str());  // no seq to count from; don't leave a stale one
  }
  LOG_INF("MHIST", "window committed");
  return true;
}

namespace {

// The escaped text of a record, which is what two records can be compared by:
// the staged lines and the spool's own come from one formatter
// (ConversationSpool::recordLine), so the bytes match when the text does. The
// timestamp is deliberately outside this — the same turn carries Muse's `at`
// here and the device's clock there.
std::string textPayload(const std::string& line) {
  const size_t at = line.find("\"x\":\"");
  if (at == std::string::npos) return std::string();
  size_t end = line.size();
  while (end > at && (line[end - 1] == '\n' || line[end - 1] == '\r')) end--;
  return line.substr(at, end - at);
}

bool isUserRecord(const std::string& line) { return line.compare(0, 8, "{\"r\":\"u\"") == 0; }

// Reads `path` a line at a time, calling `fn(line)` for each non-empty one.
// Streamed rather than slurped: a 300-turn catch-up is ~90 KB.
template <typename Fn>
bool forEachLine(const char* path, Fn fn) {
  HalFile f;
  if (!Storage.openFileForRead("MHIST", path, f) || !f) return false;
  char buf[256];
  std::string line;
  for (;;) {
    const int got = f.read(buf, sizeof(buf));
    if (got <= 0) break;
    for (int i = 0; i < got; i++) {
      if (buf[i] == '\n') {
        if (!line.empty()) fn(line);
        line.clear();
      } else if (line.size() < 4096) {
        line.push_back(buf[i]);
      }
    }
  }
  if (!line.empty()) fn(line);
  f.close();
  return true;
}

// A turn written on this device and not yet seen coming back from the mirror.
struct Anchor {
  std::string payload;    // escaped text of the user turn
  bool hasReply = false;  // an agent turn follows it locally
  bool used = false;
};

}  // namespace

bool MainHistory::applyCatchUp(ConversationSpool& spool, uint16_t& appended) {
  appended = 0;
  if (busy()) return false;
  if (!Storage.exists(catchUpPath())) return false;
  const int64_t newSeq = status().highestSeq;

  Mark mark;
  if (!readMark(mark) || mark.size > spool.size()) {
    Storage.remove(catchUpPath());
    return false;  // nothing to anchor the comparison on; refetch the window
  }

  // Turns past the mark were written here. The mirror serves them back, so each
  // has to be recognised in the staged stream rather than appended twice.
  std::vector<Anchor> anchors;
  uint32_t off = mark.size;
  while (off < spool.size()) {
    ConversationSpool::Turn t;
    if (!spool.readTurnAt(off, 0, t)) break;
    if (t.role == ConversationSpool::Role::User) {
      if (anchors.size() >= kMaxAnchors) {
        Storage.remove(catchUpPath());
        LOG_ERR("MHIST", "catch-up: too many local turns to match");
        return false;
      }
      Anchor a;
      a.payload = textPayload(ConversationSpool::recordLine(t.role, t.text, 0));
      anchors.push_back(std::move(a));
    } else if (!anchors.empty()) {
      anchors.back().hasReply = true;
    }
    if (t.nextOffset <= off) break;
    off = t.nextOffset;
  }

  // Pass one decides, pass two writes: a stream that turns out to be missing
  // one of our turns must not have appended half of itself first.
  std::vector<bool> keep;
  size_t matched = 0;
  bool skipReply = false;
  const bool read = forEachLine(catchUpPath(), [&](const std::string& line) {
    if (isUserRecord(line)) {
      const std::string pay = textPayload(line);
      for (auto& a : anchors) {
        if (a.used || a.payload != pay) continue;
        a.used = true;
        matched++;
        // Skip the reply only if this exchange already has one here. A question
        // the device showed "no response" to has none, and the mirror's answer
        // is exactly what should land after it.
        skipReply = a.hasReply;
        keep.push_back(false);
        return;
      }
      skipReply = false;
      keep.push_back(true);
      return;
    }
    if (skipReply) {
      skipReply = false;
      keep.push_back(false);
      return;
    }
    keep.push_back(true);
  });

  if (!read) {
    Storage.remove(catchUpPath());
    return false;
  }
  if (matched != anchors.size()) {
    // The mirror has not caught up with what was said here. Appending now would
    // leave those turns unaccounted for behind a moved mark, so the window path
    // takes it: a refetch with fresh=1 is correct at any distance.
    LOG_INF("MHIST", "catch-up: %u of %u local turns are in the mirror; refetching instead",
            static_cast<unsigned>(matched), static_cast<unsigned>(anchors.size()));
    Storage.remove(catchUpPath());
    return false;
  }

  const std::string live =
      std::string(ConversationSpool::sessionDir()) + "/" + ConversationSpool::kMainSessionId + ".jsonl";
  HalFile out = Storage.open(live.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (!out) {
    LOG_ERR("MHIST", "catch-up: cannot append to the Main spool");
    return false;
  }
  size_t i = 0;
  bool wrote = true;
  forEachLine(catchUpPath(), [&](const std::string& line) {
    const bool take = i < keep.size() ? keep[i] : false;
    i++;
    if (!take || !wrote) return;
    std::string record = line;
    record.push_back('\n');
    if (out.write(record.data(), record.size()) != static_cast<int>(record.size())) {
      wrote = false;
      return;
    }
    appended++;
  });
  out.flush();
  out.close();
  Storage.remove(catchUpPath());

  if (!wrote) {
    // A short write leaves a torn last line, which costs that one turn on read.
    // The mark is not moved, so the next catch-up fetches the same turns again.
    LOG_ERR("MHIST", "catch-up: short write after %u turns", static_cast<unsigned>(appended));
    return false;
  }

  Mark moved;
  moved.seq = newSeq > mark.seq ? newSeq : mark.seq;
  moved.size = ConversationSpool::sessionBytes(ConversationSpool::kMainSessionId);
  writeMark(moved);
  LOG_INF("MHIST", "catch-up appended %u turns (skipped %u of ours), seq now %lld", static_cast<unsigned>(appended),
          static_cast<unsigned>(matched), static_cast<long long>(moved.seq));
  return true;
}

bool MainHistory::loadTarget(Target& out) {
  HalFile file;
  if (!Storage.openFileForRead("MHIST", "/.crosspoint/voice.json", file) || !file) return false;
  const size_t n = file.size();
  if (n == 0 || n > 4096) return false;
  std::string buf;
  buf.resize(n);
  const int got = file.read(&buf[0], n);
  if (got <= 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, buf.c_str(), static_cast<size_t>(got))) return false;
  out.token = static_cast<const char*>(doc["token"] | "");
  out.host = static_cast<const char*>(doc["host"] | "");
  out.port = doc["port"] | 18092;
  return !out.host.empty() && out.port != 0;
}

bool MainHistory::askServiceToWiden(const Target& to, const uint32_t turns) {
  if (to.host.empty() || to.port == 0 || turns == 0) return false;
  char q[64];
  // tail=1 keeps the reply tiny: the point is the work the service does before
  // answering, not what comes back.
  snprintf(q, sizeof(q), "tail=1&meta=1&fresh=1&backfill=%lu", static_cast<unsigned long>(turns));
  std::string body;
  const bool ok = HttpDownloader::fetchUrl(urlFor(to, q), body);
  LOG_INF("MHIST", "asked the service to widen by %lu: %s", static_cast<unsigned long>(turns), ok ? "ok" : "failed");
  return ok;
}
