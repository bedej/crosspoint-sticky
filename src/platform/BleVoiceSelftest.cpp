#include "BleVoiceSelftest.h"

#if defined(VOICE_BLE_SELFTEST)

#include <Arduino.h>
#include <Logging.h>
#include <Microphone.h>

#include "audio/ImaAdpcm.h"
#include "audio/MicConditioner.h"
#include "ble/VoiceRelayPeripheral.h"

// Headless proof of the BLE transport (HomeLab-9k7 + itn): capture from the PDM
// mic, encode IMA ADPCM, advertise the voice-relay service, and stream frames to
// whichever central subscribes — no WiFi, no UI, no AgentVoiceActivity.
//
// Deliberately standalone: the activity is being reworked in a parallel session,
// and the transport is worth proving on its own before it is wired into a moving
// target. The iPhone gateway is the other end and is already verified against a
// macOS peripheral, so this test has exactly one unknown in it: this firmware.
//
// A "turn" here is time-based (talk for kTurnMs, then stop) since there are no
// button presses in a self-test. The phone opens its backend socket on turn.start
// and closes it on turn.stop, exactly as it will for a real press-to-talk.

namespace {

constexpr uint32_t kFrameSamples = 320;  // 20 ms @ 16 kHz
constexpr uint32_t kTurnMs = 6000;       // length of each simulated utterance
constexpr uint32_t kGapMs = 8000;        // pause between them, to watch reconnects

}  // namespace

void runBleVoiceSelftest() {
  LOG_INF("BLEST", "VOICE_BLE_SELFTEST build %s %s", __DATE__, __TIME__);

  auto& ble = VoiceRelayPeripheral::instance();
  // The real UI time-boxes this (HomeLab-jhe); a self-test has no buttons, so it
  // leaves the window open. Relaying still requires the central to bond first.
  ble.setPairingWindow(true);
  if (!ble.begin("Sticky")) {
    LOG_ERR("BLEST", "peripheral begin failed");
    for (;;) delay(1000);
  }
  // Bonds deliberately SURVIVE a reboot. Erasing them on every boot looks tidy
  // and is the thing that breaks pairing: the phone keeps its half, this device
  // throws its half away on each reflash, and every later connection fails to
  // encrypt with no prompt and no error on either side. The peripheral now
  // drops a bond only when pairing against it has actually failed.
  //
  // (forgetBonds() must be called after begin() in any case: it reaches into
  // the NimBLE host and panics into a boot loop before init().)

  Microphone mic;
  if (!mic.begin(16000)) {
    LOG_ERR("BLEST", "mic begin failed");
  }

  static int16_t pcm[kFrameSamples];
  static uint8_t coded[ImaAdpcm::encodedSize(kFrameSamples)];
  ImaAdpcm codec;
  MicConditioner conditioner;
  std::string answer;
  uint32_t turn = 0;

  for (;;) {
    // Wait for a central to subscribe before announcing a turn: frames sent
    // before that are dropped, and a turn nobody hears is just confusing.
    if (!ble.isStreaming()) {
      static uint32_t lastLog = 0;
      if (millis() - lastLog > 5000) {
        lastLog = millis();
        LOG_INF("BLEST", "waiting for a central (connected=%d bonded=%d bonds=%d)", (int)ble.isConnected(),
                (int)ble.isBonded(), ble.bondCount());
      }
      while (ble.popAnswer(answer)) LOG_INF("BLEST", "answer <- %s", answer.c_str());
      delay(200);
      continue;
    }

    LOG_INF("BLEST", "turn %lu start", (unsigned long)turn++);
    codec.reset();
    conditioner.reset();
    ble.notifyTurnStart();

    const uint32_t started = millis();
    uint32_t frames = 0, dropped = 0, bytes = 0;
    int32_t peak = 0;
    while (millis() - started < kTurnMs && ble.isStreaming()) {
      const int n = mic.read(pcm, kFrameSamples, 50);
      if (n <= 0) continue;
      // Without this the stream is raw PDM: a big DC offset with speech barely
      // above it, which ASR transcribes as nothing.
      conditioner.process(pcm, static_cast<size_t>(n));
      for (int i = 0; i < n; i++) {
        const int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
      }
      const size_t len = codec.encode(pcm, static_cast<size_t>(n), coded);
      if (ble.sendAudioFrame(coded, len)) {
        frames++;
        bytes += len + 3;
      } else {
        dropped++;
      }
      while (ble.popAnswer(answer)) LOG_INF("BLEST", "answer <- %s", answer.c_str());
    }

    ble.notifyTurnStop();
    const uint32_t el = millis() - started;
    LOG_INF("BLEST", "turn end: %lu frames %lu dropped %lu bytes in %lums (%lu kbps, peak=%ld, link=%s)",
            (unsigned long)frames, (unsigned long)dropped, (unsigned long)bytes, (unsigned long)el,
            (unsigned long)(el ? bytes * 8 / el : 0), (long)peak, ble.linkState().c_str());

    // Answers keep arriving after the utterance ends (ASR, then Hermes).
    const uint32_t until = millis() + kGapMs;
    while (millis() < until) {
      while (ble.popAnswer(answer)) LOG_INF("BLEST", "answer <- %s", answer.c_str());
      delay(50);
    }
  }
}

#endif  // VOICE_BLE_SELFTEST
