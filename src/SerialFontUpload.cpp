#include "SerialFontUpload.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>

#include "FontInstaller.h"
#include "SdCardFontSystem.h"

namespace {

constexpr size_t kBlock = 4096;
constexpr unsigned long kBlockTimeoutMs = 10000;

// Read exactly n bytes, yielding while the line is idle so the idle task and
// the watchdog are never starved.
bool readExactly(uint8_t* buf, const size_t n) {
  size_t got = 0;
  unsigned long last = millis();
  while (got < n) {
    const int avail = logSerial.available();
    if (avail > 0) {
      got += logSerial.readBytes(buf + got, std::min<size_t>(static_cast<size_t>(avail), n - got));
      last = millis();
    } else if (millis() - last > kBlockTimeoutMs) {
      return false;
    } else {
      vTaskDelay(1);
    }
  }
  return true;
}

void fail(const char* why) {
  logSerial.printf("PUTFONT_ERR %s\n", why);
  LOG_ERR("PUTFONT", "%s", why);
}

}  // namespace

void handleSerialPutFont(const String& args) {
  char family[40] = {0};
  char filename[80] = {0};
  unsigned long size = 0;
  unsigned long expectCrc = 0;
  if (sscanf(args.c_str(), "%39s %79s %lu %lx", family, filename, &size, &expectCrc) != 4 || size == 0) {
    fail("usage");
    return;
  }
  if (!FontInstaller::isValidFamilyName(family) || !FontInstaller::isValidCpfontFilename(filename)) {
    fail("bad name");
    return;
  }
  FontInstaller installer(sdFontSystem.registry());
  if (!installer.ensureFamilyDir(family)) {
    fail("mkdir");
    return;
  }
  char path[128];
  FontInstaller::buildFontPath(family, filename, path, sizeof(path));
  char part[140];
  snprintf(part, sizeof(part), "%s.part", path);

  HalFile file;
  if (!Storage.openFileForWrite("PUTFONT", part, file)) {
    fail("open");
    return;
  }
  auto buf = makeUniqueNoThrow<uint8_t[]>(kBlock);
  if (!buf) {
    file.close();
    Storage.remove(part);
    fail("oom");
    return;
  }

  uint32_t crc = 0;
  size_t remaining = size;
  bool ok = true;
  while (remaining > 0) {
    const size_t n = std::min(remaining, kBlock);
    logSerial.printf("PUTFONT_NEXT %u\n", static_cast<unsigned>(n));
    if (!readExactly(buf.get(), n)) {
      ok = false;
      fail("timeout");
      break;
    }
    if (remaining == size && memcmp(buf.get(), "CPFONT\0\0", 8) != 0) {
      ok = false;
      fail("magic");
      break;
    }
    crc = esp_rom_crc32_le(crc, buf.get(), static_cast<uint32_t>(n));
    if (file.write(buf.get(), n) != n) {
      ok = false;
      fail("write");
      break;
    }
    remaining -= n;
  }
  file.close();
  if (ok && crc != expectCrc) {
    ok = false;
    fail("crc");
  }
  if (!ok) {
    Storage.remove(part);
    return;
  }
  if (Storage.exists(path)) Storage.remove(path);
  if (!Storage.rename(part, path)) {
    Storage.remove(part);
    fail("rename");
    return;
  }
  sdFontSystem.markRegistryDirty();
  LOG_INF("PUTFONT", "installed %s (%lu bytes)", path, size);
  logSerial.printf("PUTFONT_OK %s\n", path);
}
