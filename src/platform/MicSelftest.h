#pragma once

// VOICE_MIC_SELFTEST builds only: bring up the PDM mic straight from setup(),
// skipping WiFi/WS/UI, and log capture stats (plus mic pin ownership) forever.
// Never returns.
void runMicSelftest();
