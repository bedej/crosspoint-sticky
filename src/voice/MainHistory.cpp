#include "MainHistory.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>

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

struct Job {
  MainHistory::Target to;
  uint16_t tail = 0;
  bool fresh = false;
  uint32_t backfill = 0;
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
  bool open() {
    Storage.ensureDirectoryExists(ConversationSpool::sessionDir());
    Storage.remove(MainHistory::stagingPath());
    file_ = Storage.open(MainHistory::stagingPath(), O_WRITE | O_CREAT | O_TRUNC);
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

void worker(void*) {
  Job job;
  {
    Lock lk;
    job = sh().job;
  }
  Staging out;
  int64_t floorSeq = 0;
  bool empty = false;
  uint16_t written = 0;

  if (!fetchFloor(job, floorSeq, empty)) {
    // setState already said why.
  } else if (empty) {
    setState(MainHistory::State::Ready, "history: nothing to load");
  } else if (!out.open()) {
    setState(MainHistory::State::Failed, "history: cannot stage");
  } else {
    bool ok = true;
    bool done = false;
    int64_t cursor = floorSeq;
    while (ok && !done && written < job.tail) {
      // Park between pages while the user is talking: the radio is shared, and
      // a page in flight is the thing that starves the audio.
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
      ok = fetchPage(job, cursor, out, written, done);
    }
    out.close();
    if (ok) {
      char note[48];
      snprintf(note, sizeof(note), "history: %u turns", static_cast<unsigned>(written));
      setState(MainHistory::State::Ready, note);
      LOG_INF("MHIST", "staged %u turns", static_cast<unsigned>(written));
    } else {
      Storage.remove(MainHistory::stagingPath());
    }
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
  LOG_INF("MHIST", "window committed");
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
