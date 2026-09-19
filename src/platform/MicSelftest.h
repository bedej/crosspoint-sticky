#pragma once

// Log who owns the PDM mic pins (USB-Serial-JTAG pad enable, IO_MUX function,
// GPIO-matrix routing) and edge counts on them, for mic bring-up diagnostics.
void logMicPinOwnership(const char* when);
void logMicPinActivity(const char* when);

// VOICE_MIC_SELFTEST builds only: bring up the PDM mic straight from setup(),
// skipping WiFi/WS/UI, and log capture stats (plus mic pin ownership) forever.
// Never returns.
void runMicSelftest();
