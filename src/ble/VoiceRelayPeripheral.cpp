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
    }
    LOG_INF("BLE", "central connected (mtu=%u)", (unsigned)server->getPeerMTU(info.getConnHandle()));
    // Keep advertising off while connected: one phone at a time, and a second
    // central subscribing to audio-up would fork the stream.
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo&, int reason) override {
    {
      std::lock_guard<std::mutex> lk(st().mtx);
      st().connected = false;
      st().streaming = false;
      st().linkState = "—";
      st().answerPartial.clear();
    }
    LOG_INF("BLE", "central disconnected (reason=%d), advertising again", reason);
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override { LOG_INF("BLE", "MTU now %u", (unsigned)mtu); }
};

class AudioUpCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    const bool on = subValue > 0;
    {
      std::lock_guard<std::mutex> lk(st().mtx);
      st().streaming = on;
      st().seq = 0;
    }
    LOG_INF("BLE", "audio-up %s", on ? "subscribed" : "unsubscribed");
    if (on) notifyControlJson(R"({"type":"ready","fw":"sticky","codecs":[1]})");
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
  s.audioUp = svc->createCharacteristic(kAudioUpUuid, NIMBLE_PROPERTY::NOTIFY);
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
std::string VoiceRelayPeripheral::linkState() const { return "—"; }

#endif
