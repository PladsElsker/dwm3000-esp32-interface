#include "BleManager.h"

#include <NimBLEDevice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace UwbBle {

static constexpr char SERVICE_UUID[] = "6f5a0001-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char RX_UUID[] = "6f5a0002-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char TX_UUID[] = "6f5a0003-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char ID_READ_UUID[] = "6f5a0004-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char ID_WRITE_UUID[] = "6f5a0005-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char PROTOCOL_VERSION_UUID[] = "6f5a0006-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr uint8_t BLE_PROTOCOL_VERSION = 1;
static constexpr uint8_t PKT_DATA = 0x01;
static constexpr uint8_t PKT_ACK = 0x02;
static constexpr uint8_t PKT_NACK = 0x03;
static constexpr uint8_t PKT_MESSAGE_RESULT = 0x04;
static constexpr uint8_t PKT_MESSAGE_RESULT_ACK = 0x05;
static constexpr uint8_t RESULT_CONSUMED = 0;
static constexpr uint8_t RESULT_CONSUMPTION_TIMEOUT = 1;
static constexpr uint32_t ACK_TIMEOUT_MS = 300;
static constexpr uint32_t RESULT_TIMEOUT_MS = 400;
static constexpr uint32_t REASSEMBLY_TIMEOUT_MS = 1500;
static constexpr uint32_t OPEN_PEER_STALE_MS = 10000;
static constexpr uint8_t MAX_ADVERTISE_NAME = 16;

static Manager* s_manager = nullptr;

// NimBLEServer invokes onConnect only when an incoming link succeeds. Observe
// failed Peripheral-side GAP connection results through NimBLE's global event
// listener so controller rejections are visible to the host application.
static int onBleGlobalGapEvent(ble_gap_event* event, void* arg) {
  auto* manager = static_cast<Manager*>(arg);
  if (manager && event && event->type == BLE_GAP_EVENT_CONNECT) {
    manager->noteGapConnectResult(event->connect.status);
  }
  return 0;
}

static bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(now - since) >= interval;
}

class BleNimBLEScanCallbacks : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* device) override {
    if (!s_manager || !device || !device->haveName()) return;
    const std::string name = device->getName();
    if (name.size() != 8 || name.compare(0, 4, "UWB-") != 0) return;
    char* end = nullptr;
    const unsigned long parsed = strtoul(name.c_str() + 4, &end, 16);
    if (!end || *end != '\0' || parsed == 0 || parsed >= 0xFFFF) return;
    const uint16_t peerId = static_cast<uint16_t>(parsed);
    s_manager->notePeerDiscovered(peerId, device->getAddress().getVal(),
                                  device->getAddress().getType());
  }
};

class BleNimBLEServerCallbacks : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    if (s_manager) s_manager->noteServerConnected(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    if (s_manager) s_manager->noteServerDisconnected(info.getConnHandle(), reason);
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    if (s_manager) s_manager->noteServerMtuChanged(info.getConnHandle(), mtu);
  }
};

class BleNimBLERxCallbacks : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (!s_manager || !characteristic) return;
    const NimBLEAttValue value = characteristic->getValue();
    if (value.length())
      s_manager->enqueueServerPacket(value.data(), value.length(), info.getConnHandle());
  }
};

class BleNimBLEIdentityCallbacks : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (!s_manager || !characteristic) return;
    const NimBLEAttValue value = characteristic->getValue();
    if (value.length() == 2)
      s_manager->noteRemoteIdentity(info.getConnHandle(), Manager::readU16(value.data()));
  }
};

class BleNimBLEClientCallbacks : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient* client) override {
    if (s_manager) s_manager->noteClientConnected(client);
  }
  void onConnectFail(NimBLEClient* client, int reason) override {
    if (s_manager) s_manager->noteClientConnectFailed(client, reason);
  }
  void onDisconnect(NimBLEClient* client, int reason) override {
    if (s_manager) s_manager->noteClientDisconnected(client, reason);
  }
};

static void onRemoteNotify(NimBLERemoteCharacteristic* characteristic, uint8_t* data,
                           size_t length, bool) {
  if (s_manager && characteristic && data && length)
    s_manager->enqueueClientPacket(data, length, characteristic);
}

Manager::Manager() {}

uint16_t Manager::readU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

void Manager::writeU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

uint16_t Manager::packetCrc(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

bool Manager::begin(uint16_t localDeviceId) {
  if (initialized_) return true;
  if (!localDeviceId || localDeviceId == 0xFFFF) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 0, static_cast<int16_t>(localDeviceId));
    return false;
  }
  localDeviceId_ = localDeviceId;
  char name[MAX_ADVERTISE_NAME];
  snprintf(name, sizeof(name), "UWB-%04X", localDeviceId_);
  if (!NimBLEDevice::init(name)) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 1, -1);
    return false;
  }
  s_manager = this;
  if (!NimBLEDevice::setMTU(MAX_ATT_MTU)) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 2, -1);
    return false;
  }
  NimBLEDevice::setPower(3);
  if (!NimBLEDevice::setCustomGapHandler(onBleGlobalGapEvent, this)) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 3, -1);
    return false;
  }

  NimBLEServer* server = NimBLEDevice::createServer();
  if (!server) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 4, -1);
    return false;
  }
  server_ = server;
  auto* serverCallbacks = new BleNimBLEServerCallbacks();
  auto* rxCallbacks = new BleNimBLERxCallbacks();
  auto* identityCallbacks = new BleNimBLEIdentityCallbacks();
  auto* clientCallbacks = new BleNimBLEClientCallbacks();
  serverCallbacks_ = serverCallbacks;
  rxCallbacks_ = rxCallbacks;
  identityCallbacks_ = identityCallbacks;
  clientCallbacks_ = clientCallbacks;
  server->setCallbacks(serverCallbacks);
  server->advertiseOnDisconnect(false);

  NimBLEService* service = server->createService(SERVICE_UUID);
  if (!service) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 5, -1);
    return false;
  }
  NimBLECharacteristic* rx = service->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  NimBLECharacteristic* tx = service->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* idRead = service->createCharacteristic(ID_READ_UUID, NIMBLE_PROPERTY::READ);
  NimBLECharacteristic* idWrite = service->createCharacteristic(ID_WRITE_UUID, NIMBLE_PROPERTY::WRITE);
  NimBLECharacteristic* protocolVersion = service->createCharacteristic(PROTOCOL_VERSION_UUID, NIMBLE_PROPERTY::READ);
  if (!rx || !tx || !idRead || !idWrite || !protocolVersion) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 6, -1);
    return false;
  }
  rx->setCallbacks(rxCallbacks);
  idWrite->setCallbacks(identityCallbacks);
  uint8_t idBytes[2];
  writeU16(idBytes, localDeviceId_);
  idRead->setValue(idBytes, sizeof(idBytes));
  protocolVersion->setValue(&BLE_PROTOCOL_VERSION, sizeof(BLE_PROTOCOL_VERSION));
  if (!server->start()) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 7, -1);
    return false;
  }
  rxCharacteristic_ = rx;
  txCharacteristic_ = tx;
  identityReadCharacteristic_ = idRead;
  identityWriteCharacteristic_ = idWrite;
  protocolVersionCharacteristic_ = protocolVersion;

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) {
    recordDiagnostic(DIAG_INIT_FAILED, 0, 1, 8, -1);
    return false;
  }
  scan_ = scan;
  auto* scanCallbacks = new BleNimBLEScanCallbacks();
  scanCallbacks_ = scanCallbacks;
  scan->setScanCallbacks(scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setMaxResults(0);

  initialized_ = true;
  recordDiagnostic(DIAG_INIT_COMPLETE);
  return true;
}

static bool sameBleConfig(const Config& a, const Config& b) {
  return a.mode == b.mode && a.advertisingIntervalMs == b.advertisingIntervalMs &&
         a.scanIntervalMs == b.scanIntervalMs && a.scanWindowMs == b.scanWindowMs &&
         a.reconnectIntervalMs == b.reconnectIntervalMs && a.autoStart == b.autoStart &&
         a.connectionTimeoutMs == b.connectionTimeoutMs && a.maxRetries == b.maxRetries &&
         a.rxMessageTimeoutMs == b.rxMessageTimeoutMs && a.peerCount == b.peerCount &&
         memcmp(a.peers, b.peers, sizeof(a.peers)) == 0;
}

bool Manager::configure(const Config& config) { return configure(config, localDeviceId_); }

bool Manager::configure(const Config& config, uint16_t localDeviceId) {
  if (!localDeviceId || localDeviceId == 0xFFFF || config.mode > MODE_PERIPHERAL ||
      config.peerCount > MAX_PEERS || config.maxRetries > 5 || config.autoStart > 1 ||
      (config.autoStart && config.mode == MODE_IDLE) ||
      config.advertisingIntervalMs < 20 || config.advertisingIntervalMs > 10240 ||
      config.scanIntervalMs < 10 || config.scanIntervalMs > 10240 ||
      config.scanWindowMs < 10 || config.scanWindowMs > config.scanIntervalMs ||
      config.connectionTimeoutMs < 100 || config.connectionTimeoutMs > 30000 ||
      config.reconnectIntervalMs < 100 || config.reconnectIntervalMs > 60000 ||
      config.rxMessageTimeoutMs < 100 || config.rxMessageTimeoutMs > 2000) return false;
  for (uint8_t i = 0; i < config.peerCount; ++i) {
    if (!config.peers[i] || config.peers[i] == 0xFFFF || config.peers[i] == localDeviceId) return false;
    for (uint8_t j = 0; j < i; ++j) if (config.peers[j] == config.peers[i]) return false;
  }
  if (enabled_ && (activeConnectionCount() || !sameBleConfig(config, config_))) return false;

  const bool identityChanged = localDeviceId_ != localDeviceId;
  config_ = config;
  localDeviceId_ = localDeviceId;
  // Reconcile remembered Central peers when the allow-list changes. An empty
  // list is open mode, so start with fresh discovery rather than pinning the
  // previous eight discovered IDs. Cached NimBLEClient objects remain reusable.
  for (auto& link : links_) {
    if (link.used && !link.serverSide &&
        (config_.mode != MODE_HUB || config_.peerCount == 0 || !peerConfigured(link.peerId))) {
      void* cachedClient = link.client;
      const uint32_t retryAt = millis() + config_.reconnectIntervalMs;
      link = Link();
      link.client = cachedClient;
      link.nextAttemptMs = retryAt;
    }
  }
  if (identityChanged) {
    uint8_t idBytes[2];
    writeU16(idBytes, localDeviceId_);
    if (auto* idRead = static_cast<NimBLECharacteristic*>(identityReadCharacteristic_))
      idRead->setValue(idBytes, sizeof(idBytes));
    char name[MAX_ADVERTISE_NAME];
    snprintf(name, sizeof(name), "UWB-%04X", localDeviceId_);
    NimBLEDevice::setDeviceName(name);
    if (auto* advertising = NimBLEDevice::getAdvertising()) advertising->setName(name);
  }
  return true;
}

bool Manager::setLocalDeviceId(uint16_t localDeviceId) { return configure(config_, localDeviceId); }

bool Manager::setPeers(const uint16_t* peers, uint8_t count) {
  if (count > MAX_PEERS || (count && !peers)) return false;
  Config updated = config_;
  updated.peerCount = count;
  memset(updated.peers, 0, sizeof(updated.peers));
  if (count) memcpy(updated.peers, peers, count * sizeof(uint16_t));
  return configure(updated);
}

bool Manager::peerConfigured(uint16_t peerId) const {
  if (!peerId || peerId == 0xFFFF || peerId == localDeviceId_) return false;
  if (config_.peerCount == 0) return true;
  for (uint8_t i = 0; i < config_.peerCount; ++i)
    if (config_.peers[i] == peerId) return true;
  return false;
}

Manager::Link* Manager::findLink(uint16_t peerId) {
  if (!peerId) return nullptr;
  for (auto& link : links_) if (link.used && link.peerId == peerId) return &link;
  return nullptr;
}

const Manager::Link* Manager::findLink(uint16_t peerId) const {
  if (!peerId) return nullptr;
  for (const auto& link : links_) if (link.used && link.peerId == peerId) return &link;
  return nullptr;
}

Manager::Link* Manager::findLinkByHandle(uint16_t handle) {
  for (auto& link : links_) if (link.used && link.serverSide && link.connectionHandle == handle) return &link;
  return nullptr;
}

Manager::Link* Manager::findLinkByClient(void* client) {
  for (auto& link : links_) if (link.used && !link.serverSide && link.client == client) return &link;
  return nullptr;
}

Manager::Link* Manager::findLinkByCharacteristic(void* characteristic) {
  for (auto& link : links_) if (link.used && link.remoteTx == characteristic) return &link;
  return nullptr;
}

Manager::Link* Manager::allocateLink(uint16_t peerId, bool serverSide) {
  if (peerId) {
    if (Link* existing = findLink(peerId)) return existing;
  }
  for (auto& link : links_) {
    if (!link.used) {
      // Keep a disconnected Central client object cached in the slot for reuse.
      void* cachedClient = link.client;
      const uint32_t retryAt = link.nextAttemptMs;
      link = Link();
      link.used = true;
      link.serverSide = serverSide;
      link.peerId = peerId;
      link.client = cachedClient;
      link.nextAttemptMs = retryAt;
      return &link;
    }
  }
  return nullptr;
}

uint8_t Manager::activeConnectionCount() const {
  uint8_t count = 0;
  for (const auto& link : links_) if (link.used && (link.connected || link.connecting)) ++count;
  return count;
}

void Manager::refreshConnectedSummary() {
  connected_ = false;
  connectedPeerId_ = 0;
  for (const auto& link : links_) {
    if (link.used && link.connected) {
      connected_ = true;
      if (!connectedPeerId_) connectedPeerId_ = link.peerId;
    }
  }
}

bool Manager::start() {
  if (!initialized_ || config_.mode == MODE_IDLE) {
    recordDiagnostic(DIAG_START_FAILED, 0, 1, 0,
                     static_cast<int16_t>(config_.mode));
    return false;
  }
  if (enabled_) return true;
  enabled_ = true;
  if (config_.mode == MODE_PERIPHERAL) {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (!advertising) {
      enabled_ = false;
      recordDiagnostic(DIAG_START_FAILED, 0, 2, 1, -1);
      return false;
    }
    char name[MAX_ADVERTISE_NAME];
    snprintf(name, sizeof(name), "UWB-%04X", localDeviceId_);
    advertising->setName(name);
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->enableScanResponse(true);
    const uint16_t units = static_cast<uint16_t>(config_.advertisingIntervalMs * 8 / 5);
    advertising->setMinInterval(units);
    advertising->setMaxInterval(units);
    const bool started = advertising->start();
    advertising_ = advertising->isAdvertising();
    if (!started || !advertising_) {
      enabled_ = false;
      recordDiagnostic(DIAG_START_FAILED, 0, 2, 2,
                       static_cast<int16_t>(started ? 1 : 0));
      return false;
    }
  } else {
    for (uint8_t i = 0; i < config_.peerCount; ++i) allocateLink(config_.peers[i], false);
    NimBLEScan* scan = static_cast<NimBLEScan*>(scan_);
    if (!scan) {
      enabled_ = false;
      recordDiagnostic(DIAG_START_FAILED, 0, 3, 1, -1);
      return false;
    }
    scan->setInterval(config_.scanIntervalMs);
    scan->setWindow(config_.scanWindowMs);
    scan->setMaxResults(0);
    scanning_ = scan->start(0);
    if (!scanning_) {
      enabled_ = false;
      recordDiagnostic(DIAG_SCAN_START_FAILED, 0, 3, 0, -1);
      recordDiagnostic(DIAG_START_FAILED, 0, 3, 2, -1);
      return false;
    }
    recordDiagnostic(DIAG_SCAN_STARTED);
  }
  recordDiagnostic(DIAG_START_COMPLETE);
  uint8_t state[4] = {1, config_.mode, static_cast<uint8_t>(advertising_), static_cast<uint8_t>(scanning_)};
  enqueueEvent(EVENT_STATE_CHANGED, state, sizeof(state));
  return true;
}

void Manager::clearMessageState(Link& link, bool reportIndeterminate) {
  if (reportIndeterminate && link.tx.active)
    emitError(0, link.peerId, link.tx.messageId, link.tx.fragmentIndex, 9);
  link.tx = Message();
  link.txWaitingAck = false;
  link.txWaitingResult = false;
  link.rx = Reassembly();
  link.rxMessageReady = false;
  link.readyMessageId = link.readyLength = 0;
  link.lastCompletedValid = false;
  link.lastCompletedMessageId = 0;
  link.lastCompletedFragmentCount = 0;
  link.result = PendingResult();
}

void Manager::stop() {
  if (!initialized_) return;
  enabled_ = false;
  if (auto* scan = static_cast<NimBLEScan*>(scan_)) scan->stop();
  if (auto* advertising = NimBLEDevice::getAdvertising()) advertising->stop();
  for (auto& link : links_) {
    if (!link.used) continue;
    if (link.serverSide) {
      if (server_) static_cast<NimBLEServer*>(server_)->disconnect(link.connectionHandle);
    } else if (link.client) {
      auto* client = static_cast<NimBLEClient*>(link.client);
      if (client->isConnected()) client->disconnect();
      else client->cancelConnect();
      // Keep the disconnected client object for reuse after BLE is re-enabled.
    }
    if (link.peerId && (link.connected || link.connecting)) {
      uint8_t payload[3]; writeU16(payload, link.peerId); payload[2] = 0;
      enqueueEvent(EVENT_PEER_DISCONNECTED, payload, sizeof(payload));
    }
    clearMessageState(link, true);
    if (link.serverSide) link = Link();
    else {
      const uint16_t id = link.peerId;
      void* client = link.client;
      link = Link();
      link.client = client;
      link.nextAttemptMs = millis() + config_.reconnectIntervalMs;
      // Only configured IDs remain reserved. Open mode rediscovers the current
      // set of peers after enable, and can reuse the eight cached client objects.
      if (config_.peerCount && peerConfigured(id)) {
        link.used = true;
        link.peerId = id;
      }
    }
  }
  advertising_ = scanning_ = false;
  recordDiagnostic(DIAG_STOPPED);
  refreshConnectedSummary();
  portENTER_CRITICAL(&mux_);
  rxHead_ = rxTail_ = rxCount_ = 0;
  portEXIT_CRITICAL(&mux_);
  uint8_t state[4] = {0, config_.mode, 0, 0};
  enqueueEvent(EVENT_STATE_CHANGED, state, sizeof(state));
}

uint16_t Manager::mtuForPeer(uint16_t peerId) const {
  const Link* link = findLink(peerId);
  return (link && link->mtu >= 23) ? link->mtu : 23;
}

uint16_t Manager::negotiatedMtu() const { return mtuForPeer(connectedPeerId_); }

bool Manager::transmitPacket(const uint8_t* packet, uint16_t length) {
  if (!packet || !length || length > MAX_PACKET_LENGTH) return false;
  Link* link = findLink(selectedPeerId_);
  if (!link || !link->connected) return false;
  if (link->serverSide) {
    auto* tx = static_cast<NimBLECharacteristic*>(txCharacteristic_);
    return tx && tx->notify(packet, length, link->connectionHandle);
  }
  auto* remote = static_cast<NimBLERemoteCharacteristic*>(link->remoteRx);
  return remote && remote->writeValue(packet, length, false);
}

bool Manager::enqueueEvent(uint8_t code, const uint8_t* payload, uint8_t length) {
  if (length > sizeof(Event::payload)) {
    recordDiagnostic(DIAG_EVENT_QUEUE_FULL, 0, code, 0, length);
    return false;
  }
  bool ok = false;
  portENTER_CRITICAL(&mux_);
  if (eventCount_ < EVENT_QUEUE_SIZE) {
    Event& event = events_[eventHead_];
    event.code = code; event.length = length;
    if (length && payload) memcpy(event.payload, payload, length);
    eventHead_ = (eventHead_ + 1) % EVENT_QUEUE_SIZE;
    ++eventCount_; ok = true;
  }
  portEXIT_CRITICAL(&mux_);
  if (!ok) recordDiagnostic(DIAG_EVENT_QUEUE_FULL, 0, code, 0, length);
  return ok;
}

bool Manager::popEvent(Event& event) {
  bool ok = false;
  portENTER_CRITICAL(&mux_);
  if (eventCount_) {
    event = events_[eventTail_];
    eventTail_ = (eventTail_ + 1) % EVENT_QUEUE_SIZE;
    --eventCount_; ok = true;
  }
  portEXIT_CRITICAL(&mux_);
  return ok;
}

bool Manager::enqueueReceivedPacket(const uint8_t* data, size_t length, uint16_t peerId) {
  if (!data || !length || length > MAX_PACKET_LENGTH || !peerId) return false;
  bool ok = false;
  portENTER_CRITICAL(&mux_);
  if (rxCount_ < RX_PACKET_QUEUE_SIZE) {
    PacketSlot& slot = rxPackets_[rxHead_];
    slot.peerId = peerId;
    slot.length = static_cast<uint16_t>(length);
    memcpy(slot.data, data, length);
    rxHead_ = (rxHead_ + 1) % RX_PACKET_QUEUE_SIZE;
    ++rxCount_; ok = true;
  }
  portEXIT_CRITICAL(&mux_);
  if (!ok) {
    recordDiagnostic(DIAG_RX_QUEUE_FULL, peerId, NACK_RX_BUSY, 0,
                     static_cast<int16_t>(length));
    emitError(1, peerId, 0, 0xFF, NACK_RX_BUSY);
  }
  return ok;
}

void Manager::enqueueServerPacket(const uint8_t* data, size_t length, uint16_t handle) {
  Link* link = findLinkByHandle(handle);
  if (link && link->connected) enqueueReceivedPacket(data, length, link->peerId);
}

void Manager::enqueueClientPacket(const uint8_t* data, size_t length, void* characteristic) {
  Link* link = findLinkByCharacteristic(characteristic);
  if (link && link->connected) enqueueReceivedPacket(data, length, link->peerId);
}

void Manager::notePeerDiscovered(uint16_t peerId, const uint8_t* address, uint8_t addressType) {
  if (!enabled_ || config_.mode != MODE_HUB || !peerConfigured(peerId) || !address) return;
  Link* link = allocateLink(peerId, false);
  if (!link && config_.peerCount == 0) {
    // In open mode, replace an idle peer that has not been seen recently.
    // Its Central client is retained in the free slot and reused for the new ID.
    for (auto& candidate : links_) {
      if (candidate.used && !candidate.serverSide && !candidate.connected &&
          !candidate.connecting && elapsed(millis(), candidate.lastSeenMs, OPEN_PEER_STALE_MS)) {
        void* cachedClient = candidate.client;
        candidate = Link();
        candidate.client = cachedClient;
        link = allocateLink(peerId, false);
        break;
      }
    }
  }
  if (!link) {
    recordDiagnostic(DIAG_CLIENT_ALLOC_FAILED, peerId, 2, 0,
                     static_cast<int16_t>(activeConnectionCount()));
    return;
  }
  const bool firstDiscovery = !link->foundNotified;
  memcpy(link->address, address, sizeof(link->address));
  link->addressType = addressType;
  link->addressReady = true;
  link->lastSeenMs = millis();
  if (firstDiscovery) {
    link->foundNotified = true;
    recordDiagnostic(DIAG_PEER_DISCOVERED, peerId);
    uint8_t payload[2]; writeU16(payload, peerId);
    enqueueEvent(EVENT_PEER_FOUND, payload, sizeof(payload));
  }
}

void Manager::noteServerConnected(uint16_t handle) {
  const uint8_t active = activeConnectionCount();
  if (!enabled_ || config_.mode != MODE_PERIPHERAL || active >= MAX_ACTIVE_CONNECTIONS) {
    recordDiagnostic(DIAG_SERVER_CAPACITY_REJECTED, 0, 1, 0, active);
    if (server_) static_cast<NimBLEServer*>(server_)->disconnect(handle);
    return;
  }
  Link* link = allocateLink(0, true);
  if (!link) {
    recordDiagnostic(DIAG_SERVER_SLOT_ALLOC_FAILED, 0, 2, 0, active);
    if (server_) static_cast<NimBLEServer*>(server_)->disconnect(handle);
    return;
  }
  recordDiagnostic(DIAG_SERVER_CONNECTED, 0, 0, 0,
                   static_cast<int16_t>(handle));
  link->connectionHandle = handle;
  link->connectStartedMs = millis();
  link->mtu = 23;
  link->connecting = true; // Count unauthenticated server links against the eight-link limit.
  advertisingRestartPending_ = true;
}

void Manager::noteServerDisconnected(uint16_t handle, int reason) {
  Link* link = findLinkByHandle(handle);
  recordDiagnostic(DIAG_PEER_DISCONNECTED, link ? link->peerId : 0, 2, 0,
                   static_cast<int16_t>(reason));
  if (!link) return;
  link->disconnectReason = reason;
  link->disconnectPending = true;
}

void Manager::noteServerMtuChanged(uint16_t handle, uint16_t mtu) {
  if (Link* link = findLinkByHandle(handle)) link->mtu = mtu;
}

void Manager::noteClientConnected(void* client) {
  if (Link* link = findLinkByClient(client)) {
    recordDiagnostic(DIAG_CENTRAL_CONNECTED, link->peerId);
    link->connectComplete = true;
  } else {
    recordDiagnostic(DIAG_CENTRAL_CONNECTED, 0, 1, 0, -1);
  }
}

void Manager::noteClientConnectFailed(void* client, int reason) {
  if (Link* link = findLinkByClient(client)) {
    recordDiagnostic(DIAG_GAP_EVENT_CALLBACK, link->peerId, 6,
                     FAILURE_STAGE_GAP_RESULT, static_cast<int16_t>(reason));
    if (link->connecting && !link->connected) {
      link->connectFailedReason = reason;
      link->connectFailedPending = true;
    }
  } else {
    recordDiagnostic(DIAG_GAP_EVENT_CALLBACK, 0, 6,
                     FAILURE_STAGE_GAP_RESULT, static_cast<int16_t>(reason));
  }
}

void Manager::noteClientDisconnected(void* client, int reason) {
  if (Link* link = findLinkByClient(client)) {
    recordDiagnostic(DIAG_PEER_DISCONNECTED, link->peerId, 1, 0,
                     static_cast<int16_t>(reason));
    link->disconnectReason = reason;
    link->disconnectPending = true;
  } else {
    recordDiagnostic(DIAG_PEER_DISCONNECTED, 0, 1, 0,
                     static_cast<int16_t>(reason));
  }
}

void Manager::noteGapConnectResult(int status) {
  recordDiagnostic(DIAG_GAP_EVENT_CALLBACK, 0, status ? 6 : 0,
                   status ? FAILURE_STAGE_GAP_RESULT : 0,
                   static_cast<int16_t>(status));
  if (status != 0) notePeripheralConnectFailed(status);
}

void Manager::notePeripheralConnectFailed(int reason) {
  // The global GAP listener sees failed incoming connection results before
  // NimBLEServer::onConnect (which is success-only). Keep this Peripheral-only
  // so Central failures continue through the per-client callback.
  if (!enabled_ || config_.mode != MODE_PERIPHERAL || reason == 0) return;
  emitConnectionFailure(0, 6, FAILURE_STAGE_GAP_RESULT,
                        static_cast<int16_t>(reason));
}

void Manager::noteRemoteIdentity(uint16_t handle, uint16_t peerId) {
  if (Link* link = findLinkByHandle(handle)) {
    link->pendingIdentity = peerId;
    link->identityPending = true;
  }
}

uint16_t Manager::fragmentDataCapacity() const {
  uint16_t mtu = mtuForPeer(selectedPeerId_);
  if (mtu > MAX_ATT_MTU) mtu = MAX_ATT_MTU;
  if (mtu <= 12) return 1;
  const uint16_t capacity = mtu - 3 - 7 - 2;
  return capacity > MAX_MESSAGE_LENGTH ? MAX_MESSAGE_LENGTH : capacity;
}

bool Manager::sendToPeer(uint16_t peerId, const uint8_t* data, uint16_t length,
                         uint16_t& messageId) {
  Link* link = findLink(peerId);
  if (!link || !link->connected || link->tx.active || length > MAX_MESSAGE_LENGTH || (length && !data))
    return false;
  selectedPeerId_ = peerId;
  const uint16_t capacity = fragmentDataCapacity();
  const uint8_t fragments = length == 0 ? 1 : static_cast<uint8_t>((length + capacity - 1) / capacity);
  if (!fragments || fragments > MAX_FRAGMENTS) return false;
  link->tx = Message();
  link->tx.active = true;
  link->tx.peerId = peerId;
  link->tx.messageId = nextMessageId_++;
  link->tx.totalLength = length;
  link->tx.fragmentCount = fragments;
  link->tx.fragmentDataLength = capacity;
  if (length) memcpy(link->tx.data, data, length);
  link->tx.lastActionMs = millis();
  link->txWaitingAck = link->txWaitingResult = false;
  messageId = link->tx.messageId;
  return true;
}

bool Manager::getReceivedData(uint16_t peerId, uint16_t messageId,
                              uint8_t* destination, uint16_t capacity, uint16_t& length) {
  Link* link = findLink(peerId);
  if (!link || !link->rxMessageReady || messageId != link->readyMessageId ||
      (link->readyLength && !destination) || capacity < link->readyLength) return false;
  selectedPeerId_ = peerId;
  length = link->readyLength;
  if (length) memcpy(destination, link->readyData, length);
  link->rxMessageReady = false;
  link->readyMessageId = link->readyLength = 0;
  link->rx = Reassembly();
  link->result = PendingResult();
  link->result.active = true;
  link->result.messageId = messageId;
  link->result.status = RESULT_CONSUMED;
  link->result.lastActionMs = millis();
  sendMessageResult(messageId, RESULT_CONSUMED);
  return true;
}

bool Manager::buildAndSendSimple(uint8_t type, uint16_t messageId,
                                 uint8_t fragmentIndex, uint8_t errorCode) {
  uint8_t packet[8]{};
  packet[0] = type; writeU16(packet + 1, messageId);
  uint16_t length = 3;
  if (type == PKT_ACK) { packet[3] = fragmentIndex; length = 4; }
  else if (type == PKT_NACK) { packet[3] = fragmentIndex; packet[4] = errorCode; length = 5; }
  writeU16(packet + length, packetCrc(packet, length));
  return transmitPacket(packet, length + 2);
}

bool Manager::sendMessageResult(uint16_t messageId, uint8_t status) {
  uint8_t packet[6]; packet[0] = PKT_MESSAGE_RESULT;
  writeU16(packet + 1, messageId); packet[3] = status;
  writeU16(packet + 4, packetCrc(packet, 4));
  return transmitPacket(packet, sizeof(packet));
}

void Manager::emitError(uint8_t direction, uint16_t peerId, uint16_t messageId,
                        uint8_t fragmentIndex, uint8_t errorCode) {
  uint8_t payload[8]; payload[0] = direction;
  writeU16(payload + 1, peerId); writeU16(payload + 3, messageId);
  payload[5] = fragmentIndex; payload[6] = errorCode; payload[7] = 0;
  enqueueEvent(EVENT_ERROR, payload, sizeof(payload));
}

void Manager::failTransmit(Link& link, uint8_t errorCode, uint8_t fragmentIndex) {
  if (!link.tx.active) return;
  emitError(0, link.peerId, link.tx.messageId, fragmentIndex, errorCode);
  link.tx = Message();
  link.txWaitingAck = link.txWaitingResult = false;
}

void Manager::handleData(const uint8_t* p, uint16_t length) {
  Link* link = findLink(selectedPeerId_);
  if (!link || length < 9) return;
  const uint16_t messageId = readU16(p + 1);
  const uint8_t fragmentIndex = p[3];
  const uint8_t fragmentCount = p[4];
  const uint16_t totalLength = readU16(p + 5);
  const uint16_t payloadLength = length - 9;
  const uint16_t capacity = fragmentDataCapacity();
  if (totalLength > MAX_MESSAGE_LENGTH) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_MESSAGE_TOO_LARGE); return;
  }
  if (!fragmentCount || fragmentCount > MAX_FRAGMENTS || fragmentIndex >= fragmentCount) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_INVALID_FRAGMENT); return;
  }
  const uint8_t expectedCount = totalLength == 0 ? 1 :
      static_cast<uint8_t>((totalLength + capacity - 1) / capacity);
  if (fragmentCount != expectedCount) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_METADATA_MISMATCH); return;
  }
  const uint16_t expectedPayload = (fragmentIndex + 1 == fragmentCount)
      ? static_cast<uint16_t>(totalLength - uint16_t(fragmentIndex) * capacity) : capacity;
  if (payloadLength != expectedPayload) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_INVALID_FRAGMENT); return;
  }

  Reassembly& rx = link->rx;
  if (!rx.active) {
    if (link->lastCompletedValid && messageId == link->lastCompletedMessageId &&
        link->lastCompletedFragmentCount && fragmentIndex < link->lastCompletedFragmentCount) {
      buildAndSendSimple(PKT_ACK, messageId, fragmentIndex); return;
    }
    if (link->result.active && messageId == link->result.messageId) {
      buildAndSendSimple(PKT_ACK, messageId, fragmentIndex); return;
    }
    if (link->result.active) {
      buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_RX_BUSY); return;
    }
    if (link->rxMessageReady) {
      if (messageId == link->readyMessageId && fragmentIndex < link->lastCompletedFragmentCount)
        buildAndSendSimple(PKT_ACK, messageId, fragmentIndex);
      else buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_RX_QUEUE_FULL);
      return;
    }
    if (fragmentIndex != 0) {
      buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_INVALID_FRAGMENT); return;
    }
    rx = Reassembly();
    rx.active = true; rx.peerId = selectedPeerId_; rx.messageId = messageId;
    rx.totalLength = totalLength; rx.fragmentDataLength = capacity;
    rx.fragmentCount = fragmentCount; rx.lastActivityMs = millis();
  }

  if (rx.messageId != messageId) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex,
                       rx.complete ? NACK_RX_QUEUE_FULL : NACK_RX_BUSY); return;
  }
  if (rx.totalLength != totalLength || rx.fragmentCount != fragmentCount ||
      rx.fragmentDataLength != capacity) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_METADATA_MISMATCH); return;
  }
  const uint64_t bit = (uint64_t(1) << fragmentIndex);
  if (rx.receivedMask & bit) {
    rx.lastActivityMs = millis(); buildAndSendSimple(PKT_ACK, messageId, fragmentIndex); return;
  }
  if (!rx.complete && payloadLength) memcpy(rx.data + uint16_t(fragmentIndex) * capacity, p + 7, payloadLength);
  rx.receivedMask |= bit; ++rx.receivedCount; rx.lastActivityMs = millis();
  buildAndSendSimple(PKT_ACK, messageId, fragmentIndex);
  if (rx.receivedCount == rx.fragmentCount && !rx.complete) {
    rx.complete = true; rx.completedAtMs = millis();
    link->rxMessageReady = true;
    link->readyMessageId = rx.messageId;
    link->readyLength = rx.totalLength;
    if (link->readyLength) memcpy(link->readyData, rx.data, link->readyLength);
    link->lastCompletedMessageId = rx.messageId;
    link->lastCompletedFragmentCount = rx.fragmentCount;
    link->lastCompletedValid = true;
    uint8_t payload[6]; writeU16(payload, selectedPeerId_);
    writeU16(payload + 2, link->readyMessageId); writeU16(payload + 4, link->readyLength);
    enqueueEvent(EVENT_DATA_RECEIVED, payload, sizeof(payload));
  }
}

void Manager::handleAck(const uint8_t* p, uint16_t length) {
  Link* link = findLink(selectedPeerId_);
  if (!link || length != 6 || !link->tx.active || !link->txWaitingAck) return;
  const uint16_t id = readU16(p + 1); const uint8_t index = p[3];
  if (id != link->tx.messageId || index != link->tx.fragmentIndex) return;
  link->txWaitingAck = false; link->tx.retryCount = 0;
  if (link->tx.fragmentIndex + 1 < link->tx.fragmentCount) {
    ++link->tx.fragmentIndex; link->tx.lastActionMs = millis();
  } else {
    link->txWaitingResult = true; link->tx.lastActionMs = millis(); link->tx.retryCount = 0;
  }
}

void Manager::handleNack(const uint8_t* p, uint16_t length) {
  Link* link = findLink(selectedPeerId_);
  if (!link || length != 7 || !link->tx.active || !link->txWaitingAck) return;
  const uint16_t id = readU16(p + 1); const uint8_t index = p[3]; const uint8_t error = p[4];
  if (id != link->tx.messageId || index != link->tx.fragmentIndex) return;
  if (error == NACK_RX_QUEUE_FULL || error == NACK_RX_BUSY) {
    if (link->tx.retryCount >= config_.maxRetries) failTransmit(*link, error, index);
    else link->tx.lastActionMs = millis() - ACK_TIMEOUT_MS;
  } else failTransmit(*link, error, index);
}

void Manager::handleMessageResult(const uint8_t* p, uint16_t length) {
  Link* link = findLink(selectedPeerId_);
  if (!link || length != 6) return;
  const uint16_t id = readU16(p + 1); const uint8_t status = p[3];
  uint8_t ack[5]; ack[0] = PKT_MESSAGE_RESULT_ACK; writeU16(ack + 1, id);
  writeU16(ack + 3, packetCrc(ack, 3)); transmitPacket(ack, sizeof(ack));
  if (status > RESULT_CONSUMPTION_TIMEOUT) { emitError(0, selectedPeerId_, id, 0xFF, 11); return; }
  if (link->tx.active && link->tx.messageId == id) {
    if (status == RESULT_CONSUMED) {
      uint8_t payload[6]; writeU16(payload, link->tx.peerId);
      writeU16(payload + 2, id); writeU16(payload + 4, link->tx.totalLength);
      enqueueEvent(EVENT_DATA_SENT, payload, sizeof(payload));
    } else emitError(0, link->tx.peerId, id, 0xFF, RESULT_CONSUMPTION_TIMEOUT);
    link->tx = Message(); link->txWaitingAck = link->txWaitingResult = false;
  }
}

void Manager::handleMessageResultAck(const uint8_t* p, uint16_t length) {
  Link* link = findLink(selectedPeerId_);
  if (link && length == 5 && link->result.active && readU16(p + 1) == link->result.messageId)
    link->result = PendingResult();
}

void Manager::handlePacket(uint16_t peerId, const uint8_t* p, uint16_t length) {
  if (length < 3) return;
  Link* link = findLink(peerId);
  if (!link || !link->connected) return;
  selectedPeerId_ = peerId;
  const uint16_t crc = readU16(p + length - 2);
  if (packetCrc(p, length - 2) != crc) return;
  switch (p[0]) {
    case PKT_DATA: handleData(p, length); break;
    case PKT_ACK: handleAck(p, length); break;
    case PKT_NACK: handleNack(p, length); break;
    case PKT_MESSAGE_RESULT: handleMessageResult(p, length); break;
    case PKT_MESSAGE_RESULT_ACK: handleMessageResultAck(p, length); break;
    default: break;
  }
}

void Manager::serviceTransmit(Link& link) {
  if (!link.connected || !link.tx.active) return;
  selectedPeerId_ = link.peerId;
  const uint32_t now = millis();
  if (link.txWaitingAck) {
    if (!elapsed(now, link.tx.lastActionMs, ACK_TIMEOUT_MS)) return;
    if (link.tx.retryCount >= config_.maxRetries) { failTransmit(link, 8, link.tx.fragmentIndex); return; }
    ++link.tx.retryCount;
  } else if (link.txWaitingResult) {
    if (!elapsed(now, link.tx.lastActionMs, RESULT_TIMEOUT_MS)) return;
    if (link.tx.retryCount >= config_.maxRetries) { failTransmit(link, 9, 0xFF); return; }
    ++link.tx.retryCount;
  }
  if (!link.txWaitingResult) {
    const uint16_t cap = link.tx.fragmentDataLength;
    const uint16_t offset = uint16_t(link.tx.fragmentIndex) * cap;
    const uint16_t remain = link.tx.totalLength > offset ? link.tx.totalLength - offset : 0;
    const uint16_t chunk = remain > cap ? cap : remain;
    uint8_t packet[MAX_PACKET_LENGTH]{};
    packet[0] = PKT_DATA; writeU16(packet + 1, link.tx.messageId);
    packet[3] = link.tx.fragmentIndex; packet[4] = link.tx.fragmentCount;
    writeU16(packet + 5, link.tx.totalLength);
    if (chunk) memcpy(packet + 7, link.tx.data + offset, chunk);
    writeU16(packet + 7 + chunk, packetCrc(packet, 7 + chunk));
    if (!transmitPacket(packet, 9 + chunk) && link.tx.retryCount >= config_.maxRetries) {
      failTransmit(link, 7, link.tx.fragmentIndex); return;
    }
    link.txWaitingAck = true; link.tx.lastActionMs = now;
  } else link.tx.lastActionMs = now;
}

void Manager::serviceReceiveTimeout(Link& link) {
  if (!link.rxMessageReady || !link.rx.active || !link.rx.complete ||
      !elapsed(millis(), link.rx.completedAtMs, config_.rxMessageTimeoutMs)) return;
  selectedPeerId_ = link.peerId;
  const uint16_t peer = link.peerId, id = link.readyMessageId;
  link.rxMessageReady = false; link.readyMessageId = link.readyLength = 0;
  link.rx = Reassembly();
  uint8_t payload[4]; writeU16(payload, peer); writeU16(payload + 2, id);
  enqueueEvent(EVENT_DATA_EXPIRED, payload, sizeof(payload));
  link.result = PendingResult(); link.result.active = true; link.result.messageId = id;
  link.result.status = RESULT_CONSUMPTION_TIMEOUT; link.result.lastActionMs = millis();
  sendMessageResult(id, RESULT_CONSUMPTION_TIMEOUT);
}

void Manager::serviceResultRetry(Link& link) {
  if (!link.connected || !link.result.active) return;
  if (!elapsed(millis(), link.result.lastActionMs, RESULT_TIMEOUT_MS)) return;
  if (link.result.retryCount >= config_.maxRetries) {
    emitError(1, link.peerId, link.result.messageId, 0xFF, 9);
    link.result = PendingResult(); return;
  }
  ++link.result.retryCount; link.result.lastActionMs = millis();
  selectedPeerId_ = link.peerId;
  sendMessageResult(link.result.messageId, link.result.status);
}

const Manager::PeerFailure* Manager::findPeerFailure(uint16_t peerId) const {
  for (const auto& failure : peerFailures_)
    if (failure.used && failure.peerId == peerId) return &failure;
  return nullptr;
}

void Manager::clearPeerFailure(uint16_t peerId) {
  for (auto& failure : peerFailures_) {
    if (failure.used && failure.peerId == peerId) {
      failure = PeerFailure();
      return;
    }
  }
}

void Manager::recordDiagnostic(uint8_t code, uint16_t peerId, uint8_t reason,
                               uint8_t stage, int16_t detail) {
  DiagnosticRecord record;
  record.uptimeMs = millis();
  record.code = code;
  record.reason = reason;
  record.stage = stage;
  record.peerId = peerId;
  record.detail = detail;
  record.activeLinks = activeConnectionCount();
  record.stateFlags = static_cast<uint8_t>((enabled_ ? 1 : 0) |
      (advertising_ ? 2 : 0) | (scanning_ ? 4 : 0) |
      (static_cast<uint8_t>(config_.mode) << 3));
  portENTER_CRITICAL(&mux_);
  diagnosticTrace_[diagnosticTraceHead_] = record;
  diagnosticTraceHead_ = static_cast<uint8_t>((diagnosticTraceHead_ + 1) %
                                               BLE_DIAGNOSTIC_TRACE_CAPACITY);
  if (diagnosticTraceCount_ < BLE_DIAGNOSTIC_TRACE_CAPACITY) ++diagnosticTraceCount_;
  portEXIT_CRITICAL(&mux_);
}

uint16_t Manager::getDiagnosticTrace(uint8_t* destination, uint16_t capacity) const {
  if (!destination || capacity < 1 + BLE_DIAGNOSTIC_RECORD_SIZE) return 0;
  DiagnosticRecord snapshot[BLE_DIAGNOSTIC_TRACE_CAPACITY];
  uint8_t count = 0;
  portENTER_CRITICAL(&mux_);
  const uint8_t maxRecords = static_cast<uint8_t>((capacity - 1) / BLE_DIAGNOSTIC_RECORD_SIZE);
  count = diagnosticTraceCount_ < maxRecords ? diagnosticTraceCount_ : maxRecords;
  const uint8_t oldest = static_cast<uint8_t>((diagnosticTraceHead_ +
      BLE_DIAGNOSTIC_TRACE_CAPACITY - count) % BLE_DIAGNOSTIC_TRACE_CAPACITY);
  for (uint8_t i = 0; i < count; ++i) {
    snapshot[i] = diagnosticTrace_[(oldest + i) % BLE_DIAGNOSTIC_TRACE_CAPACITY];
  }
  portEXIT_CRITICAL(&mux_);

  destination[0] = count;
  uint16_t offset = 1;
  for (uint8_t i = 0; i < count; ++i) {
    const DiagnosticRecord& record = snapshot[i];
    destination[offset++] = static_cast<uint8_t>(record.uptimeMs);
    destination[offset++] = static_cast<uint8_t>(record.uptimeMs >> 8);
    destination[offset++] = static_cast<uint8_t>(record.uptimeMs >> 16);
    destination[offset++] = static_cast<uint8_t>(record.uptimeMs >> 24);
    destination[offset++] = record.code;
    writeU16(destination + offset, record.peerId); offset += 2;
    destination[offset++] = record.reason;
    destination[offset++] = record.stage;
    writeU16(destination + offset, static_cast<uint16_t>(record.detail)); offset += 2;
    destination[offset++] = record.activeLinks;
    destination[offset++] = record.stateFlags;
  }
  return offset;
}

void Manager::emitConnectionFailure(uint16_t peerId, uint8_t reason,
                                    uint8_t stage, int16_t detail) {
  recordDiagnostic(DIAG_CONNECTION_FAILED, peerId, reason, stage, detail);
  // Keep bounded diagnostic history outside the live connection slots so
  // repeated rejected Peripheral handshakes cannot consume link capacity.
  if (peerId) {
    PeerFailure* record = nullptr;
    for (auto& failure : peerFailures_) {
      if (failure.used && failure.peerId == peerId) {
        record = &failure;
        break;
      }
      if (!failure.used && !record) record = &failure;
    }
    if (!record) {
      record = &peerFailures_[peerFailureReplaceIndex_];
      peerFailureReplaceIndex_ = (peerFailureReplaceIndex_ + 1) % MAX_PEERS;
    }
    record->used = true;
    record->peerId = peerId;
    record->reason = reason;
    record->stage = stage;
    record->detail = detail;
  }

  uint8_t payload[6]{};
  writeU16(payload, peerId);
  payload[2] = reason;
  payload[3] = stage;
  writeU16(payload + 4, static_cast<uint16_t>(detail));
  enqueueEvent(EVENT_CONNECTION_FAILED, payload, sizeof(payload));
}

void Manager::maintainHubConnections() {
  if (!enabled_ || config_.mode != MODE_HUB) {
    scanning_ = false;
    return;
  }

  NimBLEScan* scan = static_cast<NimBLEScan*>(scan_);
  if (!scan) {
    scanning_ = false;
    if (!scanStartFailureSeen_ || elapsed(millis(), lastScanStartFailureMs_, 2000)) {
      scanStartFailureSeen_ = true;
      lastScanStartFailureMs_ = millis();
      recordDiagnostic(DIAG_SCAN_START_FAILED, 0, 3, 1, -1);
    }
    return;
  }

  const uint32_t now = millis();

  // NimBLE-Arduino 2.5.0 stops an active scan when a Central connection
  // procedure receives BLE_HS_EBUSY. Keep scanning stopped for the complete
  // asynchronous connection and GATT identity handshake; restart it only
  // after processLinkEvents() marks that attempt complete or failed.
  for (const auto& link : links_) {
    if (link.used && !link.serverSide && link.connecting) {
      if (scan->isScanning()) scan->stop();
      scanning_ = scan->isScanning();
      return;
    }
  }

  Link* candidate = nullptr;
  for (auto& link : links_) {
    if (!link.used || link.serverSide || link.connected || link.connecting ||
        !link.addressReady) continue;
    if (activeConnectionCount() >= MAX_ACTIVE_CONNECTIONS) break;
    if (link.nextAttemptMs && static_cast<int32_t>(now - link.nextAttemptMs) < 0) continue;
    candidate = &link;
    break; // Run one Central connection procedure at a time; resume discovery between peers.
  }

  if (candidate) {
    // Stop discovery explicitly before connecting instead of relying on the
    // NimBLE client to cancel it after the first BLE_HS_EBUSY response.
    if (scan->isScanning() && !scan->stop()) {
      scanning_ = scan->isScanning();
      candidate->nextAttemptMs = now + config_.reconnectIntervalMs;
      recordDiagnostic(DIAG_SCAN_STOP_FAILED, candidate->peerId, 1,
                       FAILURE_STAGE_SCAN_STOP, -1);
      emitConnectionFailure(candidate->peerId, 1, FAILURE_STAGE_SCAN_STOP, -1);
      return;
    }
    // GAP scan cancellation completes asynchronously. Do not submit the
    // connection request until NimBLE reports discovery as inactive.
    if (scan->isScanning()) {
      scanning_ = true;
      return;
    }
    scanning_ = false;

    if (!candidate->client) {
      NimBLEClient* client = NimBLEDevice::createClient();
      if (!client) {
        candidate->nextAttemptMs = now + config_.reconnectIntervalMs;
        recordDiagnostic(DIAG_CLIENT_ALLOC_FAILED, candidate->peerId, 1,
                         FAILURE_STAGE_CLIENT_ALLOCATE, -1);
        emitConnectionFailure(candidate->peerId, 1, FAILURE_STAGE_CLIENT_ALLOCATE, -1);
        return;
      }
      candidate->client = client;
      client->setClientCallbacks(static_cast<NimBLEClientCallbacks*>(clientCallbacks_), false);
    }

    NimBLEClient* client = static_cast<NimBLEClient*>(candidate->client);
    client->setConnectTimeout(config_.connectionTimeoutMs);
    const NimBLEAddress address(candidate->address, candidate->addressType);
    candidate->connecting = true;
    candidate->connectStartedMs = now;
    candidate->nextAttemptMs = 0;
    candidate->connectFailedPending = false;
    recordDiagnostic(DIAG_CENTRAL_CONNECT_ATTEMPT, candidate->peerId, 0, 0,
                     static_cast<int16_t>(candidate->addressType));
    if (!client->connect(address, true, true, true)) {
      const int rc = client->getLastError();
      candidate->connecting = false;
      candidate->nextAttemptMs = now + config_.reconnectIntervalMs;
      emitConnectionFailure(candidate->peerId, 1, FAILURE_STAGE_GAP_START,
                            static_cast<int16_t>(rc));
    }
    return;
  }

  if (!scan->isScanning()) {
    scanning_ = scan->start(0);
    if (scanning_) {
      scanStartFailureSeen_ = false;
      recordDiagnostic(DIAG_SCAN_STARTED);
    } else if (!scanStartFailureSeen_ || elapsed(now, lastScanStartFailureMs_, 2000)) {
      scanStartFailureSeen_ = true;
      lastScanStartFailureMs_ = now;
      recordDiagnostic(DIAG_SCAN_RESTART_FAILED, 0, 3, 0, -1);
    }
  } else {
    scanning_ = true;
  }
}

void Manager::processLinkEvents() {
  for (auto& link : links_) {
    if (!link.used) continue;
    if (link.connectFailedPending) {
      const int failureReason = link.connectFailedReason;
      link.connectFailedPending = false;
      link.connectFailedReason = 0;
      if (enabled_ && config_.mode == MODE_HUB && !link.serverSide &&
          link.connecting && !link.connected) {
        link.connecting = false;
        link.nextAttemptMs = millis() + config_.reconnectIntervalMs;
        emitConnectionFailure(link.peerId, 6, FAILURE_STAGE_GAP_RESULT,
                              static_cast<int16_t>(failureReason));
      }
    }
    if (link.disconnectPending) {
      link.disconnectPending = false;
      const uint16_t peer = link.peerId;
      const int reason = link.disconnectReason;
      const bool wasConnected = link.connected;
      link.connected = false; link.connecting = false; link.connectComplete = false;
      link.remoteRx = link.remoteTx = nullptr;
      link.mtu = 23;
      clearMessageState(link, true);
      if (peer && wasConnected) {
        uint8_t payload[3]; writeU16(payload, peer); payload[2] = static_cast<uint8_t>(reason);
        enqueueEvent(EVENT_PEER_DISCONNECTED, payload, sizeof(payload));
      }
      if (enabled_ && config_.mode == MODE_HUB && !link.serverSide)
        link.nextAttemptMs = millis() + config_.reconnectIntervalMs;
      if (config_.mode == MODE_PERIPHERAL && link.serverSide) {
        advertisingRestartPending_ = true;
        link = Link();
      }
    }

    if (link.used && link.serverSide && !link.connected && !link.handshakeTimedOut &&
        elapsed(millis(), link.connectStartedMs, config_.connectionTimeoutMs)) {
      link.handshakeTimedOut = true;
      recordDiagnostic(DIAG_HANDSHAKE_TIMEOUT, 0, 5,
                       FAILURE_STAGE_PERIPHERAL_HANDSHAKE_TIMEOUT, 0);
      emitConnectionFailure(0, 5, FAILURE_STAGE_PERIPHERAL_HANDSHAKE_TIMEOUT, 0);
      if (server_) static_cast<NimBLEServer*>(server_)->disconnect(link.connectionHandle);
    }

    if (link.used && link.identityPending) {
      link.identityPending = false;
      const uint16_t peer = link.pendingIdentity;
      bool duplicate = false;
      for (const auto& other : links_)
        if (&other != &link && other.used && other.connected && other.peerId == peer) duplicate = true;
      if (!enabled_ || !link.serverSide || link.handshakeTimedOut || !peerConfigured(peer) || duplicate) {
        const int16_t detail = !enabled_ ? 1 : !link.serverSide ? 2 :
                               link.handshakeTimedOut ? 3 : !peerConfigured(peer) ? 4 : 5;
        recordDiagnostic(DIAG_SERVER_IDENTITY_REJECTED, peer, 3,
                         FAILURE_STAGE_PERIPHERAL_IDENTITY, detail);
        link.peerId = peer;
        emitConnectionFailure(peer, 3, FAILURE_STAGE_PERIPHERAL_IDENTITY, detail);
        if (server_) static_cast<NimBLEServer*>(server_)->disconnect(link.connectionHandle);
      } else {
        recordDiagnostic(DIAG_SERVER_IDENTITY_ACCEPTED, peer);
        link.peerId = peer; link.connected = true; link.connecting = false;
        clearPeerFailure(peer);
        link.mtu = static_cast<NimBLEServer*>(server_)->getPeerMTU(link.connectionHandle);
        uint8_t payload[2]; writeU16(payload, peer);
        enqueueEvent(EVENT_PEER_CONNECTED, payload, sizeof(payload));
      }
    }

    if (link.used && link.connectComplete) {
      link.connectComplete = false;
      NimBLEClient* client = static_cast<NimBLEClient*>(link.client);
      if (!enabled_ || !client || !client->isConnected()) continue;
      auto fail = [&](uint8_t reason, uint8_t stage, int16_t detail) {
        emitConnectionFailure(link.peerId, reason, stage, detail);
        // Keep the Central slot marked busy until NimBLE confirms disconnect;
        // this prevents the scanner or another connection attempt from racing
        // the GATT failure teardown.
        link.connecting = true;
        if (client->isConnected()) client->disconnect();
      };
      NimBLERemoteService* service = client->getService(SERVICE_UUID);
      if (!service) { fail(5, FAILURE_STAGE_GATT_SERVICE, 0); continue; }
      recordDiagnostic(DIAG_CENTRAL_HANDSHAKE_STEP, link.peerId, 0,
                       FAILURE_STAGE_GATT_SERVICE, 1);
      auto* remoteRx = service->getCharacteristic(RX_UUID);
      auto* remoteTx = service->getCharacteristic(TX_UUID);
      auto* remoteIdRead = service->getCharacteristic(ID_READ_UUID);
      auto* remoteIdWrite = service->getCharacteristic(ID_WRITE_UUID);
      auto* remoteVersion = service->getCharacteristic(PROTOCOL_VERSION_UUID);
      if (!remoteRx || !remoteRx->canWrite() || !remoteTx || !remoteTx->canNotify() ||
          !remoteIdRead || !remoteIdRead->canRead() || !remoteIdWrite || !remoteIdWrite->canWrite() ||
          !remoteVersion || !remoteVersion->canRead()) {
        fail(5, FAILURE_STAGE_GATT_CHARACTERISTICS, 0); continue;
      }
      recordDiagnostic(DIAG_CENTRAL_HANDSHAKE_STEP, link.peerId, 0,
                       FAILURE_STAGE_GATT_CHARACTERISTICS, 1);
      const NimBLEAttValue version = remoteVersion->readValue();
      if (version.length() != 1 || version.data()[0] != BLE_PROTOCOL_VERSION) {
        const int16_t detail = version.length() == 1 ? version.data()[0] : -1;
        fail(5, FAILURE_STAGE_PROTOCOL_VERSION, detail); continue;
      }
      recordDiagnostic(DIAG_CENTRAL_HANDSHAKE_STEP, link.peerId, 0,
                       FAILURE_STAGE_PROTOCOL_VERSION, version.data()[0]);
      const NimBLEAttValue id = remoteIdRead->readValue();
      if (id.length() != 2) {
        fail(5, FAILURE_STAGE_PEER_IDENTITY, static_cast<int16_t>(id.length())); continue;
      }
      const uint16_t remoteId = readU16(id.data());
      if (remoteId != link.peerId || !peerConfigured(remoteId)) {
        fail(3, FAILURE_STAGE_PEER_IDENTITY, static_cast<int16_t>(remoteId)); continue;
      }
      recordDiagnostic(DIAG_CENTRAL_HANDSHAKE_STEP, link.peerId, 0,
                       FAILURE_STAGE_PEER_IDENTITY, static_cast<int16_t>(remoteId));
      uint8_t ownId[2]; writeU16(ownId, localDeviceId_);
      if (!remoteIdWrite->writeValue(ownId, sizeof(ownId), true)) {
        fail(5, FAILURE_STAGE_IDENTITY_WRITE, -1); continue;
      }
      recordDiagnostic(DIAG_CENTRAL_HANDSHAKE_STEP, link.peerId, 0,
                       FAILURE_STAGE_IDENTITY_WRITE, 1);
      if (!remoteTx->subscribe(true, onRemoteNotify)) {
        fail(5, FAILURE_STAGE_NOTIFY_SUBSCRIBE, -1); continue;
      }
      recordDiagnostic(DIAG_CENTRAL_HANDSHAKE_STEP, link.peerId, 0,
                       FAILURE_STAGE_NOTIFY_SUBSCRIBE, 1);
      link.remoteRx = remoteRx; link.remoteTx = remoteTx;
      recordDiagnostic(DIAG_CENTRAL_HANDSHAKE_COMPLETE, link.peerId);
      clearPeerFailure(link.peerId);
      link.mtu = client->getMTU(); link.connected = true; link.connecting = false;
      uint8_t payload[2]; writeU16(payload, link.peerId);
      enqueueEvent(EVENT_PEER_CONNECTED, payload, sizeof(payload));
    }
  }
  refreshConnectedSummary();
  if (enabled_ && config_.mode == MODE_PERIPHERAL && advertisingRestartPending_) {
    advertisingRestartPending_ = false;
    if (activeConnectionCount() < MAX_ACTIVE_CONNECTIONS) {
      if (auto* advertising = NimBLEDevice::getAdvertising()) {
        const bool started = advertising->start();
        advertising_ = advertising->isAdvertising();
        if (!started || !advertising_) {
          recordDiagnostic(DIAG_ADVERTISING_RESTART_FAILED, 0, 2, 0,
                           static_cast<int16_t>(started ? 1 : 0));
        }
      } else {
        recordDiagnostic(DIAG_ADVERTISING_RESTART_FAILED, 0, 2, 0, -1);
      }
    }
  }
}

bool Manager::getPeerStatus(uint16_t peerId, PeerStatus& status) const {
  status = PeerStatus();
  status.mode = config_.mode; status.enabled = enabled_ ? 1 : 0;
  status.advertising = advertising_ ? 1 : 0; status.scanning = scanning_ ? 1 : 0;
  status.peerId = peerId;
  const Link* link = findLink(peerId);
  if (link) {
    status.connected = link->connected ? 1 : 0;
    status.mtu = link->connected ? link->mtu : 23;
    if (link->connected) status.state = PEER_CONNECTED;
    else if (link->connecting) status.state = link->serverSide ? PEER_HANDSHAKING : PEER_CONNECTING;
    else if (link->nextAttemptMs &&
             static_cast<int32_t>(millis() - link->nextAttemptMs) < 0) status.state = PEER_BACKOFF;
    else if (link->addressReady) status.state = PEER_FOUND;
    else if (config_.mode == MODE_PERIPHERAL) status.state = PEER_CONFIGURED;
    else status.state = PEER_BACKOFF;
  } else if (peerConfigured(peerId)) {
    status.state = PEER_CONFIGURED;
    if (config_.mode == MODE_PERIPHERAL) {
      // Server-side links have no peer ID until the identity characteristic is
      // written. Surface that pending handshake against configured peer rows.
      for (const auto& pending : links_) {
        if (pending.used && pending.serverSide && pending.connecting && !pending.peerId) {
          status.state = PEER_HANDSHAKING;
          break;
        }
      }
    }
  }
  if (const PeerFailure* failure = findPeerFailure(peerId)) {
    status.lastFailureReason = failure->reason;
    status.lastFailureStage = failure->stage;
    status.lastFailureDetail = failure->detail;
  }
  return true;
}

void Manager::poll() {
  if (!initialized_) return;
  processLinkEvents();
  maintainHubConnections();

  for (;;) {
    PacketSlot slot;
    bool have = false;
    portENTER_CRITICAL(&mux_);
    if (rxCount_) {
      slot = rxPackets_[rxTail_];
      rxTail_ = (rxTail_ + 1) % RX_PACKET_QUEUE_SIZE;
      --rxCount_; have = true;
    }
    portEXIT_CRITICAL(&mux_);
    if (!have) break;
    handlePacket(slot.peerId, slot.data, slot.length);
  }

  for (auto& link : links_) {
    if (!link.used || !link.connected) continue;
    if (link.rx.active && !link.rx.complete &&
        elapsed(millis(), link.rx.lastActivityMs, REASSEMBLY_TIMEOUT_MS)) {
      emitError(1, link.peerId, link.rx.messageId, 0xFF, 10);
      link.rx = Reassembly();
    }
    serviceTransmit(link);
    serviceReceiveTimeout(link);
    serviceResultRetry(link);
  }
  refreshConnectedSummary();
}

} // namespace UwbBle
