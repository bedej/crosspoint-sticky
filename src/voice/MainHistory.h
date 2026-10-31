#pragma once

// The Muse main chat, cached on the SD card from the voice service's mirror
// (HomeLab-rc0).
//
// The service owns the real record: it reads Muse's `/chat/history` and keeps
// every turn, including the ones made in the Muse app on other devices. This
// class pulls a window of that onto the SD card so the conversation opens from
// disk rather than from the network.
//
// Three rules shape it:
//
// 1. **Forward only.** ConversationSpool appends and paginates forward, so the
//    window is fetched oldest-first: one short request for the window's floor
//    (`tail=N&meta=1`), then pages forward from it. Paging backwards would mean
//    prepending, which the spool cannot do.
// 2. **Staged, then renamed.** Pages are written to `main.staging.jsonl` by a
//    worker task and swapped in by the UI task, so a half-fetched window never
//    becomes the thing on screen, and the live conversation keeps working while
//    it downloads. Widening the window re-fetches rather than prepends.
// 3. **The cache is not the record.** For Main the spool is a copy of the
//    service's mirror. A rewrite re-derives it, so a turn taken seconds ago can
//    only survive it by coming back from the mirror — which is why a rewrite
//    asks the service to poll Muse first (`fresh=1`).

#include <stdint.h>

#include <string>

class MainHistory {
 public:
  enum class State : uint8_t { Idle, Working, Ready, Failed };

  struct Status {
    State state = State::Idle;
    uint16_t turns = 0;   // written to the staging file so far
    uint16_t target = 0;  // how many the window is meant to hold
    // Why it failed, for the log and the status line. Never a secret.
    char note[48] = {0};
  };

  // Where the voice service is, from /.crosspoint/voice.json.
  struct Target {
    std::string host;
    uint16_t port = 0;
    std::string token;
  };

  // Starts the worker task. `tail` is how many turns the window should hold;
  // `fresh` asks the service to poll Muse for new turns first; `backfill` asks
  // it to widen its own mirror before answering (nothing extra is downloaded).
  // Returns false if a fetch is already running.
  static bool start(const Target& to, uint16_t tail, bool fresh, uint32_t backfill);

  static Status status();
  static bool busy();

  // Hold the fetch between pages while a turn is in flight, and let it go again
  // afterwards. Wi-Fi and BLE share one radio on the S3: a sustained HTTP
  // download during a turn starved the audio going up over Bluetooth to 0.9
  // seconds in 39, and the service answered "didn't catch that" to a question
  // it never heard. "Never blocks a turn" has to mean the fetch yields to one,
  // not merely that it runs on another task.
  static void hold(bool held);

  // Swap the staged window in: removes the Main spool and its index, then
  // renames. UI task only — it replaces files the view has open, so the caller
  // must close the view first and re-open it after.
  static bool commit();

  // Forget a finished or failed fetch, so start() may run again.
  static void clear();

  // One short request, nothing downloaded: ask the service to widen its own
  // mirror so the turns are there when the user pages back. Used on device
  // activation, where spending seconds on a download would be wrong.
  static bool askServiceToWiden(const Target& to, uint32_t turns);

  // Reads host/port/token from /.crosspoint/voice.json. The voice activity has
  // its own loader for the rest of that file; this one exists so a caller with
  // no activity (the wake path in main.cpp) can reach the service too.
  static bool loadTarget(Target& out);

  // `/sticky/sessions/main.staging.jsonl`.
  static const char* stagingPath();
};
