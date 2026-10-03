#include "VoiceBackend.h"

#include <Logging.h>
#include <Preferences.h>

#include <cstring>

namespace {

constexpr const char* kNamespace = "voice";
constexpr const char* kKeyBackend = "backend";

VoiceBackend cached = VoiceBackend::Hermes;
bool cacheValid = false;

// Hermes has always been there, so it is Ready until the server says otherwise.
// Muse starts Unknown: the server may not implement `backends` yet, and a
// device that shows "Ready" on no evidence is the kind of lie that costs an
// evening.
BackendStatus hermesStatus = BackendStatus::Ready;
BackendStatus museStatus = BackendStatus::Unknown;

}  // namespace

const char* voiceBackendWireName(const VoiceBackend backend) {
  return backend == VoiceBackend::Muse ? "muse" : "hermes";
}

VoiceBackend voiceBackend() {
  if (cacheValid) return cached;
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) {
    // Namespace absent simply means nothing has chosen yet.
    cached = VoiceBackend::Hermes;
    cacheValid = true;
    return cached;
  }
  const uint8_t stored = prefs.getUChar(kKeyBackend, static_cast<uint8_t>(VoiceBackend::Hermes));
  prefs.end();
  cached = stored == static_cast<uint8_t>(VoiceBackend::Muse) ? VoiceBackend::Muse : VoiceBackend::Hermes;
  cacheValid = true;
  return cached;
}

void setVoiceBackend(const VoiceBackend backend) {
  if (cacheValid && cached == backend) return;  // nothing to write
  cached = backend;
  cacheValid = true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
    LOG_ERR("VBE", "could not open NVS to store the backend");
    return;
  }
  prefs.putUChar(kKeyBackend, static_cast<uint8_t>(backend));
  prefs.end();
  LOG_INF("VBE", "backend is now %s", voiceBackendWireName(backend));
}

BackendStatus voiceBackendStatus(const VoiceBackend backend) {
  return backend == VoiceBackend::Muse ? museStatus : hermesStatus;
}

void setVoiceBackendStatus(const VoiceBackend backend, const BackendStatus status) {
  (backend == VoiceBackend::Muse ? museStatus : hermesStatus) = status;
}

BackendStatus parseBackendStatus(const char* word) {
  if (!word || !*word) return BackendStatus::Unknown;
  if (strcmp(word, "ready") == 0) return BackendStatus::Ready;
  if (strcmp(word, "connecting") == 0) return BackendStatus::Connecting;
  if (strcmp(word, "unpaired") == 0) return BackendStatus::Unpaired;
  if (strcmp(word, "error") == 0) return BackendStatus::Error;
  return BackendStatus::Unknown;
}
