#include "ImaAdpcm.h"

namespace {

// Standard IMA ADPCM tables; the phone's decoder carries the same two, and the
// pair must stay identical or the streams drift apart.
constexpr int32_t kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

constexpr int32_t kIndexAdjust[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

inline int32_t clampTo(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

void ImaAdpcm::reset() {
  predictor_ = 0;
  index_ = 0;
}

size_t ImaAdpcm::encode(const int16_t* pcm, size_t count, uint8_t* out) {
  // Header: the decoder starts each frame from this state, so a lost frame costs
  // only itself.
  const int16_t startPredictor = static_cast<int16_t>(clampTo(predictor_, -32768, 32767));
  out[0] = static_cast<uint8_t>(startPredictor & 0xFF);
  out[1] = static_cast<uint8_t>((startPredictor >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>(index_);
  out[3] = 0;

  size_t written = 4;
  uint8_t pending = 0;
  bool haveLowNibble = false;

  for (size_t i = 0; i < count; i++) {
    const int32_t step = kStep[index_];
    int32_t diff = static_cast<int32_t>(pcm[i]) - predictor_;
    int32_t code = 0;
    if (diff < 0) {
      code = 8;
      diff = -diff;
    }
    int32_t threshold = step;
    if (diff >= threshold) {
      code |= 4;
      diff -= threshold;
    }
    threshold >>= 1;
    if (diff >= threshold) {
      code |= 2;
      diff -= threshold;
    }
    threshold >>= 1;
    if (diff >= threshold) code |= 1;

    // Track the decoder exactly: it reconstructs from the code, so the encoder
    // must predict from the same reconstruction, not from the input sample.
    int32_t delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    predictor_ = clampTo(predictor_ + ((code & 8) ? -delta : delta), -32768, 32767);
    index_ = clampTo(index_ + kIndexAdjust[code], 0, 88);

    if (haveLowNibble) {
      out[written++] = static_cast<uint8_t>(pending | (code << 4));
      haveLowNibble = false;
    } else {
      pending = static_cast<uint8_t>(code);
      haveLowNibble = true;
    }
  }
  if (haveLowNibble) out[written++] = pending;
  return written;
}
