#include "MicSelftest.h"

#if defined(VOICE_MIC_SELFTEST)

#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <Microphone.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include <climits>

namespace {

// Snapshot who owns the mic pins: USB-Serial-JTAG pad/PHY enables, the IO_MUX
// function select of each pin, and the GPIO-matrix output routing.
void logPinOwnership(const char* when) {
  const auto& mic = BoardConfig::ACTIVE.mic;
  const uint32_t conf0 = REG_READ(USB_SERIAL_JTAG_CONF0_REG);
  const uint32_t rtcUsb = REG_READ(RTC_CNTL_USB_CONF_REG);
  LOG_INF("MIC", "[%s] clk=%d data=%d en=%d | USJ_CONF0=0x%08lx pad_en=%d phy_sel=%d dp_pu=%d | RTC_USB_CONF=0x%08lx",
          when, mic.clk, mic.data, mic.enable, (unsigned long)conf0, (conf0 & USB_SERIAL_JTAG_USB_PAD_ENABLE) ? 1 : 0,
          (conf0 & USB_SERIAL_JTAG_PHY_SEL) ? 1 : 0, (conf0 & USB_SERIAL_JTAG_DP_PULLUP) ? 1 : 0,
          (unsigned long)rtcUsb);
  for (int pin : {19, 20}) {
    const uint32_t mux = REG_READ(GPIO_PIN_MUX_REG[pin]);
    const uint32_t outSel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * pin);
    LOG_INF("MIC", "[%s] GPIO%d iomux=0x%08lx mcu_sel=%lu fun_ie=%lu | out_sel=0x%03lx", when, pin, (unsigned long)mux,
            (unsigned long)((mux >> MCU_SEL_S) & MCU_SEL_V), (unsigned long)((mux >> FUN_IE_S) & 1),
            (unsigned long)outSel);
  }
}

// Busy-sample GPIO_IN for the clk/data pins and count edges: a live PDM bus
// shows both toggling; a held/unclocked line shows zero edges.
void logPinActivity(const char* when) {
  uint32_t prev = REG_READ(GPIO_IN_REG);
  uint32_t edges19 = 0, edges20 = 0, high20 = 0;
  constexpr uint32_t kIters = 200000;
  for (uint32_t i = 0; i < kIters; i++) {
    const uint32_t v = REG_READ(GPIO_IN_REG);
    const uint32_t diff = v ^ prev;
    edges19 += (diff >> 19) & 1;
    edges20 += (diff >> 20) & 1;
    high20 += (v >> 20) & 1;
    prev = v;
  }
  LOG_INF("MIC", "[%s] pin activity over %lu reads: GPIO19 edges=%lu GPIO20 edges=%lu GPIO20 high=%lu%%", when,
          (unsigned long)kIters, (unsigned long)edges19, (unsigned long)edges20,
          (unsigned long)(high20 * 100 / kIters));
}

}  // namespace

void runMicSelftest() {
  LOG_INF("MIC", "VOICE_MIC_SELFTEST build %s %s", __DATE__, __TIME__);
  logPinOwnership("boot");
  logPinActivity("boot");

  Microphone mic;
  if (!mic.begin(16000)) {
    LOG_ERR("MIC", "mic.begin failed");
  }
  logPinOwnership("after-begin");
  logPinActivity("after-begin");

  static int16_t buf[512];
  uint32_t window = 0;
  for (;;) {
    // "CMD:REC" over serial: capture 5 s into PSRAM, then dump raw s16le PCM
    // framed as REC_START:<bytes>\n ... REC_END\n for the host to save as WAV.
    if (logSerial.available() > 0) {
      String line = logSerial.readStringUntil('\n');
      line.trim();
      if (line == "CMD:REC") {
        constexpr size_t kRecSamples = 16000 * 5;
        auto* rec = static_cast<int16_t*>(ps_malloc(kRecSamples * sizeof(int16_t)));
        if (!rec) {
          LOG_ERR("MIC", "OOM: rec buffer");
        } else {
          size_t got = 0;
          while (got < kRecSamples) {
            const int r = mic.read(rec + got, kRecSamples - got, 200);
            if (r > 0) got += r;
          }
          logSerial.printf("REC_START:%u\n", (unsigned)(got * sizeof(int16_t)));
          logSerial.write(reinterpret_cast<const uint8_t*>(rec), got * sizeof(int16_t));
          logSerial.printf("REC_END\n");
          free(rec);
        }
      }
    }

    int64_t sum = 0, sumSq = 0, sumAbs = 0;
    int32_t mn = INT_MAX, mx = INT_MIN, peak = 0;
    uint32_t n = 0, zeroReads = 0;
    const uint32_t t0 = millis();
    while (n < 16000 && millis() - t0 < 2000) {
      const int got = mic.read(buf, sizeof(buf) / sizeof(buf[0]), 200);
      if (got <= 0) {
        zeroReads++;
        continue;
      }
      for (int i = 0; i < got; i++) {
        const int32_t s = buf[i];
        sum += s;
        sumSq += (int64_t)s * s;
        sumAbs += s < 0 ? -s : s;
        if (s < mn) mn = s;
        if (s > mx) mx = s;
        if ((s < 0 ? -s : s) > peak) peak = s < 0 ? -s : s;
      }
      n += got;
    }
    if (n == 0) {
      LOG_INF("MIC", "#%lu no samples (zeroReads=%lu)", (unsigned long)window++, (unsigned long)zeroReads);
      continue;
    }
    const double mean = (double)sum / n;
    const double acRms = sqrt((double)sumSq / n - mean * mean);
    LOG_INF("MIC", "#%lu n=%lu mean=%d acRms=%d avgAbs=%ld peak=%ld min=%ld max=%ld", (unsigned long)window++,
            (unsigned long)n, (int)mean, (int)acRms, (long)(sumAbs / n), (long)peak, (long)mn, (long)mx);
  }
}

#endif  // VOICE_MIC_SELFTEST
