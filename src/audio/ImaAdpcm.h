#pragma once

// IMA ADPCM encoder (HomeLab-itn): 4:1 compression, 16 kHz PCM16 -> ~64 kbps, so a
// turn fits BLE comfortably. Measured on an iPhone 16 Pro, the link sustains
// 120-160 kbps even backgrounded and locked, so this leaves roughly 2x headroom.
//
// Each frame restarts from the carried predictor/index and writes them into a
// 4-byte header, so a dropped notification costs one frame rather than
// desynchronising the rest of the utterance. Audio notifications are
// unacknowledged by design: late audio is worse than missing audio for ASR.
//
// No heap, no floats: this runs in the capture path between i2s_channel_read and
// the BLE notify.

#include <stddef.h>
#include <stdint.h>

class ImaAdpcm {
 public:
  // Bytes produced for `samples` input samples: 4-byte state header + one nibble
  // per sample.
  static constexpr size_t encodedSize(size_t samples) { return 4 + (samples + 1) / 2; }

  void reset();

  // Encode one frame. `out` must hold at least encodedSize(count) bytes.
  // Returns bytes written.
  size_t encode(const int16_t* pcm, size_t count, uint8_t* out);

 private:
  int32_t predictor_ = 0;
  int32_t index_ = 0;
};
