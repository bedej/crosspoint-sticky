#pragma once

#include <Arduino.h>

// Test hook: install a .cpfont over the USB serial link, for headless font
// studies (sticky-harness/fontsweep.py). The web upload needs Wi-Fi and a tap
// through the network-mode picker; this needs neither.
//
//   host:   CMD:PUTFONT <family> <filename> <bytes> <crc32 hex>
//   device: PUTFONT_NEXT <n>      (repeated: host sends exactly n bytes)
//   device: PUTFONT_OK | PUTFONT_ERR <reason>
//
// Block-acknowledged so an SD write can never overflow the UART's receive
// buffer. Written to <file>.part and renamed only after the CRC (zlib's)
// matches, then the font registry is marked dirty.
void handleSerialPutFont(const String& args);

// Test hook: remove an installed SD font family, as the web Fonts page does.
//   host:   CMD:DELFONT <family>
//   device: DELFONT_OK <family> | DELFONT_ERR <family>
void handleSerialDelFont(const String& family);
