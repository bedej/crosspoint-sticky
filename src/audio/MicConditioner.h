#pragma once

// Mic conditioning shared by every transport that sends captured audio.
//
// The PDM stream carries a large DC offset (~1300 LSB) with speech only a few
// hundred LSB above it, which ASR hears as silence: replaying a real turn through
// the backend transcribed nothing raw, nothing DC-only, and correctly once gained.
// So this is not a nicety — audio that skips it does not transcribe.
//
// It lived in AgentVoiceActivity::pumpMic, which meant the BLE path silently sent
// raw samples and produced garbage transcripts. Keeping it here means a new
// transport cannot forget it.
//
// Fixed point, no heap: this runs between the i2s read and the encoder.

#include <stddef.h>
#include <stdint.h>

class MicConditioner {
 public:
  // Call at the start of each utterance so the tracker re-primes from the first
  // sample rather than ramping from the previous turn's state.
  void reset() {
    dcState_ = 0;
    primed_ = false;
  }

  // DC-block then apply make-up gain, in place.
  void process(int16_t* pcm, size_t count) {
    if (count == 0) return;
    if (!primed_) {
      dcState_ = static_cast<int32_t>(pcm[0]) << kDcFrac;
      primed_ = true;
    }
    for (size_t i = 0; i < count; i++) {
      const int32_t x = pcm[i];
      // Q<kDcFrac> tracker: at integer resolution the shift stalls while still
      // short of the offset, leaving up to ~500 LSB of DC for the gain to amplify.
      dcState_ += ((x << kDcFrac) - dcState_) >> kDcShift;
      const int32_t ac = (x - (dcState_ >> kDcFrac)) * kGain;
      pcm[i] = static_cast<int16_t>(ac > 32767 ? 32767 : (ac < -32768 ? -32768 : ac));
    }
  }

 private:
  static constexpr int kDcShift = 9;  // ~8 Hz corner at 16 kHz
  static constexpr int kDcFrac = 8;   // fixed-point bits below the LSB
  static constexpr int32_t kGain = 32;
  int32_t dcState_ = 0;
  bool primed_ = false;
};
