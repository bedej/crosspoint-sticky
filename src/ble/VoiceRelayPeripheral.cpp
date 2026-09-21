#include "VoiceRelayPeripheral.h"

VoiceRelayPeripheral& VoiceRelayPeripheral::instance() {
  static VoiceRelayPeripheral inst;
  return inst;
}

#if FREEINK_CAP_BLE_VOICE_RELAY

#include <Arduino.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include <cstring>
#include <mutex>

namespace {

constexpr char kServiceUuid[] = "3CB420D1-7509-4199-A48C-0A1A58CC0E33";
constexpr char kAudioUpUuid[] = "727BBFAC-27E4-4FA8-BFE1-CE5EBB638761";
constexpr char kControlUuid[] = "875FCF17-3C92-41AB-8019-4F68F0ABAB49";
constexpr char kAnswerDownUuid[] = "A684CBCC-ED6F-4E9F-94F0-E0254BA8FB42";

// One notification must fit a single ATT payload (MTU - 3). iOS negotiates a
// large MTU (515 observed), but nothing may depend on that: 20 ms of IMA ADPCM
// is 167 bytes, which fits even the 23-byte-MTU worst case after fragmentation
// is not available for audio — so the encoder's frame size is the contract.
constexpr size_t kMaxAnswerBuffer = 4096;

struct State {
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* audioUp = nullptr;
  NimBLECharacteristic* control = nullptr;
  NimBLECharacteristic* answerDown = nullptr;

  std::mutex mtx;  // guards everything below (NimBLE host task vs app)
  bool connected = false;
  bool streaming = false;
  bool wantsStream = false;  // subscribed, but not yet proven secure
  bool encrypted = false;
  bool bonded = false;
  // Only while this is open may a new phone bond (HomeLab-jhe drives it from the UI).
  bool pairingOpen = false;
  uint16_t seq = 0;
  std::string answerPartial;  // fragments accumulating
  std::string answerReady;    // one complete object, waiting for the app
  std::string linkState = "—";
};

State& st() {
  static State s;
  return s;
}

void notifyControlJson(const std::string& json) {
  auto& s = st();
  if (!s.control) return;
  s.control->setValue(reinterpret_cast<const uint8_t*>(json.data()), json.size());
  s.control->notify();
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    {
      std::lock_guard<std::mutex> lk(st().mtx);
      st().connected = true;
      st().encrypted = info.isEncrypted();
      st().bonded = info.isBonded();
    }
    LOG_INF("BLE", "central %s connected (mtu=%u bonded=%d encrypted=%d, %d stored bond(s))",
            info.getAddress().toString().c_str(), (unsigned)server->getPeerMTU(info.getConnHandle()),
            (int)info.isBonded(), (int)info.isEncrypted(), NimBLEDevice::getNumBonds());
    // Outside a pairing window only an already-bonded phone may stay. A stranger
    // is disconnected rather than left to attempt pairing.
    if (!info.isBonded() && !st().pairingOpen) {
      LOG_INF("BLE", "unbonded central and no pairing window; disconnecting");
      server->disconnect(info.getConnHandle());
      return;
    }
    // Ask for encryption immediately rather than waiting to refuse a subscribe:
    // a central that is merely refused learns nothing and never pairs, so the
    // link would fail closed and stay that way. This starts pairing for a new
    // phone and re-establishes encryption from stored keys for a known one.
    if (!info.isEncrypted()) {
      int rc = 0;
      const bool ok = NimBLEDevice::startSecurity(info.getConnHandle(), &rc);
      LOG_INF("BLE", "requested encryption (ok=%d rc=%d)", (int)ok, rc);
    }
    // Keep advertising off while connected: one phone at a time, and a second
    // central subscribing to audio-up would fork the stream.
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo&, int reason) override {
    {
      std::lock_guard<std::mutex> lk(st().mtx);
      st().connected = false;
      st().streaming = false;
      st().wantsStream = false;
      st().encrypted = false;
      st().bonded = false;
      st().linkState = "—";
      st().answerPartial.clear();
    }
    LOG_INF("BLE", "central disconnected (reason=%d), advertising again", reason);
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override { LOG_INF("BLE", "MTU now %u", (unsigned)mtu); }

  // Pairing finishes well after the central has discovered services and
  // subscribed, so this is where the stream is actually allowed to start.
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    bool release = false;
    {
      std::lock_guard<std::mutex> lk(st().mtx);
      st().encrypted = info.isEncrypted();
      st().bonded = info.isBonded();
      release = st().wantsStream && !st().streaming && st().encrypted && st().bonded;
      if (release) {
        st().streaming = true;
        st().seq = 0;
      }
    }
    LOG_INF("BLE", "pairing complete (bonded=%d encrypted=%d auth=%d) bonds=%d", (int)info.isBonded(),
            (int)info.isEncrypted(), (int)info.isAuthenticated(), NimBLEDevice::getNumBonds());
    if (!info.isEncrypted()) {
      // Pairing ran and failed. The usual reason is a one-sided bond: the phone
      // still holds keys for a bond this device no longer has (every reflash
      // used to erase them), so its encryption attempt fails against a key we
      // cannot produce. Drop our half and the connection so the next attempt
      // starts clean, rather than sitting here unencrypted forever.
      LOG_ERR("BLE", "link did not encrypt; dropping our bond for %s and disconnecting",
              info.getAddress().toString().c_str());
      const int before = NimBLEDevice::getNumBonds();
      NimBLEDevice::deleteBond(info.getAddress());
      if (NimBLEDevice::getNumBonds() >= before && before > 0) {
        // The peer connected under a resolvable private address that does not
        // match the identity the bond was stored against, so it cannot be
        // deleted by the address we can see. Pairing has already failed, so
        // there is nothing left worth keeping.
        LOG_ERR("BLE", "could not delete that bond by address; erasing all %d", before);
        NimBLEDevice::deleteAllBonds();
      }
      if (st().server) st().server->disconnect(info.getConnHandle());
      return;
    }
    if (release) {
      LOG_INF("BLE", "audio-up released after pairing");
      notifyControlJson(R"({"type":"ready","fw":"sticky","codecs":[1]})");
    }
  }
};

class AudioUpCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t subValue) override {
    const bool on = subValue > 0;
    const bool secure = info.isEncrypted() && info.isBonded();
    // A central subscribes as soon as it has discovered the service, which is
    // before pairing completes. Refusing outright is a dead end — nothing
    // prompts it to try again — so remember the request and hold the audio
    // until onAuthenticationComplete says the link is encrypted and bonded.
    {
      std::lock_guard<std::mutex> lk(st().mtx);
      st().wantsStream = on;
      st().streaming = on && secure;
      st().seq = 0;
    }
    LOG_INF("BLE", "audio-up %s (secure=%d)", on ? "subscribed" : "unsubscribed", (int)secure);
    if (on && !secure) LOG_INF("BLE", "holding audio until the link is encrypted");
    if (on && secure) notifyControlJson(R"({"type":"ready","fw":"sticky","codecs":[1]})");
  }
};

class InboundCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const std::string value = c->getValue();
    if (value.empty()) return;

    if (c->getUUID().equals(NimBLEUUID(kAnswerDownUuid))) {
      // 1-byte fragment header: 0 = more follows, 1 = final fragment.
      const uint8_t flag = static_cast<uint8_t>(value[0]);
      std::lock_guard<std::mutex> lk(st().mtx);
      if (st().answerPartial.size() + value.size() > kMaxAnswerBuffer) {
        st().answerPartial.clear();  // runaway peer; drop rather than grow
        return;
      }
      st().answerPartial.append(value, 1, std::string::npos);
      if (flag == 1) {
        st().answerReady.swap(st().answerPartial);
        st().answerPartial.clear();
      }
      return;
    }

    // control: the phone's own link state, for the device's screen.
    const size_t k = value.find("\"link\"");
    if (k != std::string::npos) {
      const size_t a = value.find('"', value.find(':', k) + 1);
      const size_t b = a == std::string::npos ? std::string::npos : value.find('"', a + 1);
      if (b != std::string::npos) {
        std::lock_guard<std::mutex> lk(st().mtx);
        st().linkState = value.substr(a + 1, b - a - 1);
      }
    }
  }
};

}  // namespace

bool VoiceRelayPeripheral::supported() { return true; }

bool VoiceRelayPeripheral::begin(const char* deviceName) {
  auto& s = st();
  if (s.server) return true;

  if (!NimBLEDevice::init(deviceName ? deviceName : "Sticky")) {
    LOG_ERR("BLE", "NimBLEDevice::init failed");
    return false;
  }
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  // Bonded central only (HomeLab-tdg): pairing with LE Secure Connections, no MITM
  // passkey because the device has no keypad and the phone is the user's own.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  s.server = NimBLEDevice::createServer();
  s.server->setCallbacks(new ServerCallbacks());
  s.server->advertiseOnDisconnect(true);

  NimBLEService* svc = s.server->createService(kServiceUuid);
  // Every characteristic requires an encrypted link. Without this a central can
  // connect unbonded and subscribe to the microphone — which is what an earlier
  // build allowed, and is a live mic anyone in range can listen to.
  s.audioUp = svc->createCharacteristic(kAudioUpUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  s.audioUp->setCallbacks(new AudioUpCallbacks());
  s.control = svc->createCharacteristic(kControlUuid,
                                        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  s.control->setCallbacks(new InboundCallbacks());
  // The phone is the central, so device-bound messages are WRITES, not notifies.
  s.answerDown = svc->createCharacteristic(kAnswerDownUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  s.answerDown->setCallbacks(new InboundCallbacks());
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  // The service UUID must be in the advertisement packet itself: iOS background
  // scanning matches only on advertised service UUIDs.
  adv->addServiceUUID(kServiceUuid);
  adv->setName(deviceName ? deviceName : "Sticky");
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();

  LOG_INF("BLE", "voice relay advertising as %s", deviceName ? deviceName : "Sticky");
  return true;
}

void VoiceRelayPeripheral::end() {
  auto& s = st();
  if (!s.server) return;
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);
  s.server = nullptr;
  s.audioUp = s.control = s.answerDown = nullptr;
  std::lock_guard<std::mutex> lk(s.mtx);
  s.connected = s.streaming = false;
  LOG_INF("BLE", "voice relay stopped");
}

bool VoiceRelayPeripheral::isConnected() const {
  std::lock_guard<std::mutex> lk(st().mtx);
  return st().connected;
}

bool VoiceRelayPeripheral::isStreaming() const {
  std::lock_guard<std::mutex> lk(st().mtx);
  return st().streaming;
}

void VoiceRelayPeripheral::notifyTurnStart() {
  {
    std::lock_guard<std::mutex> lk(st().mtx);
    st().seq = 0;
  }
  notifyControlJson(R"({"type":"turn","action":"start"})");
}

void VoiceRelayPeripheral::notifyTurnStop() { notifyControlJson(R"({"type":"turn","action":"stop"})"); }

bool VoiceRelayPeripheral::sendAudioFrame(const uint8_t* coded, size_t len) {
  auto& s = st();
  uint16_t seq;
  {
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.streaming || !s.audioUp) return false;
    seq = s.seq++;
  }
  // ver(1) | seq(2, LE) | coded audio
  uint8_t frame[3 + 256];
  if (len + 3 > sizeof(frame)) return false;
  frame[0] = 1;
  frame[1] = static_cast<uint8_t>(seq & 0xFF);
  frame[2] = static_cast<uint8_t>(seq >> 8);
  memcpy(frame + 3, coded, len);
  s.audioUp->setValue(frame, len + 3);
  s.audioUp->notify();
  return true;
}

bool VoiceRelayPeripheral::popAnswer(std::string& out) {
  std::lock_guard<std::mutex> lk(st().mtx);
  if (st().answerReady.empty()) return false;
  out.swap(st().answerReady);
  st().answerReady.clear();
  return true;
}

void VoiceRelayPeripheral::setPairingWindow(bool open) {
  {
    std::lock_guard<std::mutex> lk(st().mtx);
    st().pairingOpen = open;
  }
  LOG_INF("BLE", "pairing window %s", open ? "OPEN — a new phone may bond" : "closed");
}

bool VoiceRelayPeripheral::isPairingWindowOpen() const {
  std::lock_guard<std::mutex> lk(st().mtx);
  return st().pairingOpen;
}

bool VoiceRelayPeripheral::isBonded() const {
  std::lock_guard<std::mutex> lk(st().mtx);
  return st().bonded && st().encrypted;
}

int VoiceRelayPeripheral::bondCount() const { return NimBLEDevice::getNumBonds(); }

void VoiceRelayPeripheral::forgetBonds() {
  // Guard rather than trust the caller: this reaches into the NimBLE host, and
  // calling it before init() panics — a boot loop with no useful log.
  if (!NimBLEDevice::isInitialized()) {
    LOG_ERR("BLE", "forgetBonds before begin(); ignored");
    return;
  }
  const int before = NimBLEDevice::getNumBonds();
  NimBLEDevice::deleteAllBonds();
  int left = NimBLEDevice::getNumBonds();
  // ble_store_clear() can leave records behind, and a bond we think is gone but
  // the phone still holds is the worst state to be in: pairing then fails with
  // no prompt and no error, and the link simply never encrypts. So check, and
  // delete the stragglers by address.
  for (int guard = 0; left > 0 && guard < 8; guard++) {
    NimBLEDevice::deleteBond(NimBLEDevice::getBondedAddress(0));
    const int now = NimBLEDevice::getNumBonds();
    if (now >= left) break;  // not shrinking — stop rather than spin
    left = now;
  }
  if (left > 0) {
    LOG_ERR("BLE", "bonds NOT erased: %d remain (was %d)", left, before);
  } else {
    LOG_INF("BLE", "bonds erased (%d)", before);
  }
}

std::string VoiceRelayPeripheral::linkState() const {
  std::lock_guard<std::mutex> lk(st().mtx);
  return st().linkState;
}

#else  // FREEINK_CAP_BLE_VOICE_RELAY — stubs, no BLE code linked.

bool VoiceRelayPeripheral::supported() { return false; }
bool VoiceRelayPeripheral::begin(const char*) { return false; }
void VoiceRelayPeripheral::end() {}
bool VoiceRelayPeripheral::isConnected() const { return false; }
bool VoiceRelayPeripheral::isStreaming() const { return false; }
void VoiceRelayPeripheral::notifyTurnStart() {}
void VoiceRelayPeripheral::notifyTurnStop() {}
bool VoiceRelayPeripheral::sendAudioFrame(const uint8_t*, size_t) { return false; }
bool VoiceRelayPeripheral::popAnswer(std::string&) { return false; }
void VoiceRelayPeripheral::setPairingWindow(bool) {}
bool VoiceRelayPeripheral::isPairingWindowOpen() const { return false; }
bool VoiceRelayPeripheral::isBonded() const { return false; }
int VoiceRelayPeripheral::bondCount() const { return 0; }
void VoiceRelayPeripheral::forgetBonds() {}
std::string VoiceRelayPeripheral::linkState() const { return "—"; }

#endif
