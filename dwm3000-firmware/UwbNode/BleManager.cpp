#include "BleManager.h"

#include <NimBLEDevice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace UwbBle {

static constexpr char SERVICE_UUID[] = "6f5a0001-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char RX_UUID[]      = "6f5a0002-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char TX_UUID[]      = "6f5a0003-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char ID_READ_UUID[] = "6f5a0004-8f7b-4b2d-a6d8-2d9a7b3c0001";
static constexpr char ID_WRITE_UUID[]= "6f5a0005-8f7b-4b2d-a6d8-2d9a7b3c0001";
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
static constexpr uint8_t MAX_ADVERTISE_NAME = 16;

static Manager* s_manager = nullptr;

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
    if (!s_manager->peerConfigured(peerId)) return;

    bool notifyFound = false;
    for (uint8_t i = 0; i < s_manager->config_.peerCount; ++i) {
      if (s_manager->config_.peers[i] == peerId && !s_manager->foundNotified_[i]) {
        s_manager->foundNotified_[i] = true;
        notifyFound = true;
        break;
      }
    }
    if (notifyFound) {
      uint8_t payload[2];
      Manager::writeU16(payload, peerId);
      s_manager->enqueueEvent(EVENT_PEER_FOUND, payload, sizeof(payload));
    }

    if (s_manager->connectRequested_ && s_manager->requestedPeerId_ == peerId &&
        !s_manager->scanAddressReady_) {
      const NimBLEAddress address = device->getAddress();
      memcpy(s_manager->foundAddressBytes_, address.getVal(), sizeof(s_manager->foundAddressBytes_));
      s_manager->foundAddressType_ = address.getType();
      s_manager->scanAddressReady_ = true;
    }
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
    if (s_manager && s_manager->serverConnectionActive_ &&
        s_manager->serverConnectionHandle_ == info.getConnHandle())
      s_manager->mtu_ = mtu;
  }
};

class BleNimBLERxCallbacks : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    if (!s_manager || !characteristic) return;
    const NimBLEAttValue value = characteristic->getValue();
    if (value.length() > 0)
      s_manager->enqueueReceivedPacket(value.data(), value.length());
  }
};

class BleNimBLEIdentityCallbacks : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    if (!s_manager || !characteristic) return;
    const NimBLEAttValue value = characteristic->getValue();
    if (value.length() == 2)
      s_manager->noteRemoteIdentity(Manager::readU16(value.data()));
  }
};

class BleNimBLEClientCallbacks : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient*) override {
    if (s_manager) s_manager->noteClientConnected();
  }
  void onDisconnect(NimBLEClient*, int reason) override {
    if (s_manager) s_manager->noteClientDisconnected(reason);
  }
};

static void onRemoteNotify(NimBLERemoteCharacteristic*, uint8_t* data,
                           size_t length, bool) {
  if (s_manager && data && length)
    s_manager->enqueueReceivedPacket(data, length);
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
  if (localDeviceId == 0 || localDeviceId == 0xFFFF) return false;
  localDeviceId_ = localDeviceId;
  char name[MAX_ADVERTISE_NAME];
  snprintf(name, sizeof(name), "UWB-%04X", localDeviceId_);
  if (!NimBLEDevice::init(name)) return false;
  if (!NimBLEDevice::setMTU(MAX_ATT_MTU)) return false;
  NimBLEDevice::setPower(3);
  s_manager = this;

  // The same GATT service is used on every node. Central nodes write RX and
  // subscribe to TX; Peripheral nodes accept RX writes and notify on TX.
  NimBLEServer* server = NimBLEDevice::createServer();
  if (!server) return false;
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
  if (!service) return false;
  NimBLECharacteristic* rx = service->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  NimBLECharacteristic* tx = service->createCharacteristic(
      TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* idRead = service->createCharacteristic(
      ID_READ_UUID, NIMBLE_PROPERTY::READ);
  NimBLECharacteristic* idWrite = service->createCharacteristic(
      ID_WRITE_UUID, NIMBLE_PROPERTY::WRITE);
  NimBLECharacteristic* protocolVersion = service->createCharacteristic(
      PROTOCOL_VERSION_UUID, NIMBLE_PROPERTY::READ);
  if (!rx || !tx || !idRead || !idWrite || !protocolVersion) return false;
  rx->setCallbacks(rxCallbacks);
  idWrite->setCallbacks(identityCallbacks);
  uint8_t idBytes[2];
  writeU16(idBytes, localDeviceId_);
  idRead->setValue(idBytes, sizeof(idBytes));
  protocolVersion->setValue(&BLE_PROTOCOL_VERSION, sizeof(BLE_PROTOCOL_VERSION));
  // NimBLE-Arduino 2.5.2 starts all registered services through the server.
  if (!server->start()) return false;
  rxCharacteristic_ = rx;
  txCharacteristic_ = tx;
  identityReadCharacteristic_ = idRead;
  identityWriteCharacteristic_ = idWrite;
  protocolVersionCharacteristic_ = protocolVersion;

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) return false;
  scan_ = scan;
  auto* scanCallbacks = new BleNimBLEScanCallbacks();
  scanCallbacks_ = scanCallbacks;
  scan->setScanCallbacks(scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setMaxResults(0);

  initialized_ = true;
  return true;
}

static bool sameBleConfig(const Config& a, const Config& b) {
  return a.mode == b.mode &&
         a.advertisingIntervalMs == b.advertisingIntervalMs &&
         a.scanIntervalMs == b.scanIntervalMs && a.scanWindowMs == b.scanWindowMs &&
         a.reconnectEnabled == b.reconnectEnabled &&
         a.reconnectIntervalMs == b.reconnectIntervalMs &&
         a.connectionTimeoutMs == b.connectionTimeoutMs &&
         a.maxRetries == b.maxRetries && a.rxMessageTimeoutMs == b.rxMessageTimeoutMs &&
         a.peerCount == b.peerCount &&
         memcmp(a.peers, b.peers, sizeof(a.peers)) == 0;
}

bool Manager::configure(const Config& config) {
  return configure(config, localDeviceId_);
}

bool Manager::configure(const Config& config, uint16_t localDeviceId) {
  if (localDeviceId == 0 || localDeviceId == 0xFFFF ||
      config.mode > MODE_PERIPHERAL || config.peerCount > MAX_PEERS ||
      config.maxRetries > 5 || config.reconnectEnabled > 1 ||
      config.advertisingIntervalMs < 20 || config.advertisingIntervalMs > 10240 ||
      config.scanIntervalMs < 10 || config.scanIntervalMs > 10240 ||
      config.scanWindowMs < 10 || config.scanWindowMs > config.scanIntervalMs ||
      config.connectionTimeoutMs < 100 || config.connectionTimeoutMs > 30000 ||
      config.reconnectIntervalMs < 100 || config.reconnectIntervalMs > 60000 ||
      config.rxMessageTimeoutMs < 100 || config.rxMessageTimeoutMs > 2000) return false;
  for (uint8_t i = 0; i < config.peerCount; ++i) {
    if (config.peers[i] == 0 || config.peers[i] == 0xFFFF || config.peers[i] == localDeviceId)
      return false;
    for (uint8_t j = 0; j < i; ++j)
      if (config.peers[j] == config.peers[i]) return false;
  }
  // Active advertising/scanning cannot safely adopt new interval, role or peer
  // settings in-place. A caller must stop BLE first; an identity-only change is
  // allowed because the advertised name and identity characteristic are updated.
  if (enabled_ && (connected_ || !sameBleConfig(config, config_))) return false;

  config_ = config;
  const bool identityChanged = localDeviceId_ != localDeviceId;
  localDeviceId_ = localDeviceId;
  if (identityChanged) {
    uint8_t idBytes[2];
    writeU16(idBytes, localDeviceId_);
    if (auto* idRead = static_cast<NimBLECharacteristic*>(identityReadCharacteristic_))
      idRead->setValue(idBytes, sizeof(idBytes));
    char name[MAX_ADVERTISE_NAME];
    snprintf(name, sizeof(name), "UWB-%04X", localDeviceId_);
    NimBLEDevice::setDeviceName(name);
    if (auto* advertising = NimBLEDevice::getAdvertising()) {
      const bool restart = enabled_ && config_.mode == MODE_PERIPHERAL && advertising->isAdvertising();
      if (restart) advertising->stop();
      advertising->setName(name);
      if (restart) {
        advertising->start();
        advertising_ = advertising->isAdvertising();
      }
    }
  }
  memset(foundNotified_, 0, sizeof(foundNotified_));
  return true;
}

bool Manager::setLocalDeviceId(uint16_t localDeviceId) {
  return configure(config_, localDeviceId);
}

bool Manager::setPeers(const uint16_t* peers, uint8_t count) {
  if (count > MAX_PEERS || (count && !peers)) return false;
  Config updated = config_;
  updated.peerCount = count;
  memset(updated.peers, 0, sizeof(updated.peers));
  if (count) memcpy(updated.peers, peers, count * sizeof(uint16_t));
  return configure(updated);
}

bool Manager::peerConfigured(uint16_t peerId) const {
  if (peerId == 0 || peerId == 0xFFFF || peerId == localDeviceId_) return false;
  if (config_.peerCount == 0) return true;
  for (uint8_t i = 0; i < config_.peerCount; ++i)
    if (config_.peers[i] == peerId) return true;
  return false;
}

bool Manager::start() {
  if (!initialized_ || config_.mode == MODE_IDLE) return false;
  if (enabled_) return true;
  enabled_ = true;
  advertising_ = false;
  scanning_ = false;
  if (config_.mode == MODE_PERIPHERAL) {
    auto* advertising = NimBLEDevice::getAdvertising();
    if (!advertising) { enabled_ = false; return false; }
    char name[MAX_ADVERTISE_NAME];
    snprintf(name, sizeof(name), "UWB-%04X", localDeviceId_);
    advertising->setName(name);
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->enableScanResponse(true);
    const uint16_t intervalUnits = static_cast<uint16_t>(config_.advertisingIntervalMs * 8 / 5);
    advertising->setMinInterval(intervalUnits);
    advertising->setMaxInterval(intervalUnits);
    advertising->start();
    advertising_ = advertising->isAdvertising();
    if (!advertising_) { enabled_ = false; return false; }
  } else {
    NimBLEScan* scan = static_cast<NimBLEScan*>(scan_);
    scan->setInterval(config_.scanIntervalMs);
    scan->setWindow(config_.scanWindowMs);
    scan->setMaxResults(0);
    scanning_ = scan->start(0);
    if (!scanning_) { enabled_ = false; return false; }
  }
  uint8_t state[4] = {1, config_.mode, static_cast<uint8_t>(advertising_ ? 1 : 0),
                       static_cast<uint8_t>(scanning_ ? 1 : 0)};
  enqueueEvent(EVENT_STATE_CHANGED, state, sizeof(state));
  return true;
}

void Manager::stop() {
  if (!initialized_) return;
  if (connectedPeerId_) pendingDisconnectedPeerId_ = connectedPeerId_;
  if (auto* scan = static_cast<NimBLEScan*>(scan_)) scan->stop();
  if (auto* advertising = NimBLEDevice::getAdvertising()) advertising->stop();
  if (config_.mode == MODE_HUB && client_) {
    auto* client = static_cast<NimBLEClient*>(client_);
    if (client->isConnected()) client->disconnect();
    else client->cancelConnect();
  } else if (config_.mode == MODE_PERIPHERAL && server_ && serverConnectionActive_) {
    static_cast<NimBLEServer*>(server_)->disconnect(serverConnectionHandle_);
  }
  clearMessageState(true);
  enabled_ = false;
  advertising_ = false;
  scanning_ = false;
  connected_ = false;
  connectedPeerId_ = 0;
  requestedPeerId_ = 0;
  connectRequested_ = false;
  uint8_t state[4] = {0, config_.mode, 0, 0};
  enqueueEvent(EVENT_STATE_CHANGED, state, sizeof(state));
}

bool Manager::connect(uint16_t peerId) {
  if (!enabled_ || config_.mode != MODE_HUB || !peerConfigured(peerId) || connected_) return false;
  requestedPeerId_ = peerId;
  connectRequested_ = true;
  scanAddressReady_ = false;
  connectStartedMs_ = millis();
  lastReconnectAttemptMs_ = connectStartedMs_;
  if (!scanning_) {
    NimBLEScan* scan = static_cast<NimBLEScan*>(scan_);
    scanning_ = scan && scan->start(0);
  }
  if (!scanning_) {
    connectRequested_ = false;
    requestedPeerId_ = 0;
    return false;
  }
  return true;
}

bool Manager::disconnect(uint16_t peerId) {
  if (!connected_ || peerId != connectedPeerId_) return false;
  if (config_.mode == MODE_HUB && client_) {
    const bool requested = static_cast<NimBLEClient*>(client_)->disconnect();
    if (requested) { connectRequested_ = false; requestedPeerId_ = 0; }
    return requested;
  }
  if (config_.mode == MODE_PERIPHERAL && server_ && serverConnectionActive_) {
    const bool requested = static_cast<NimBLEServer*>(server_)->disconnect(serverConnectionHandle_);
    return requested;
  }
  return false;
}

uint16_t Manager::negotiatedMtu() const {
  if (config_.mode == MODE_PERIPHERAL && server_ && serverConnectionActive_) {
    const uint16_t peerMtu = static_cast<NimBLEServer*>(server_)->getPeerMTU(serverConnectionHandle_);
    return peerMtu >= 23 ? peerMtu : 23;
  }
  if (config_.mode == MODE_HUB && client_ && static_cast<NimBLEClient*>(client_)->isConnected()) {
    const uint16_t peerMtu = static_cast<NimBLEClient*>(client_)->getMTU();
    return peerMtu >= 23 ? peerMtu : 23;
  }
  return 23;
}

bool Manager::transmitPacket(const uint8_t* packet, uint16_t length) {
  if (!connected_ || !packet || length == 0 || length > MAX_PACKET_LENGTH) return false;
  if (config_.mode == MODE_HUB) {
    auto* remote = static_cast<NimBLERemoteCharacteristic*>(remoteRxCharacteristic_);
    return remote && remote->writeValue(packet, length, false);
  }
  auto* tx = static_cast<NimBLECharacteristic*>(txCharacteristic_);
  if (!tx) return false;
  tx->setValue(packet, length);
  return tx->notify();
}

bool Manager::enqueueEvent(uint8_t code, const uint8_t* payload, uint8_t length) {
  if (length > sizeof(Event::payload)) return false;
  bool ok = false;
  portENTER_CRITICAL(&mux_);
  if (eventCount_ < EVENT_QUEUE_SIZE) {
    Event& event = events_[eventHead_];
    event.code = code;
    event.length = length;
    if (length && payload) memcpy(event.payload, payload, length);
    eventHead_ = (eventHead_ + 1) % EVENT_QUEUE_SIZE;
    ++eventCount_;
    ok = true;
  }
  portEXIT_CRITICAL(&mux_);
  return ok;
}

bool Manager::popEvent(Event& event) {
  bool ok = false;
  portENTER_CRITICAL(&mux_);
  if (eventCount_) {
    event = events_[eventTail_];
    eventTail_ = (eventTail_ + 1) % EVENT_QUEUE_SIZE;
    --eventCount_;
    ok = true;
  }
  portEXIT_CRITICAL(&mux_);
  return ok;
}

void Manager::enqueueReceivedPacket(const uint8_t* data, size_t length) {
  if (!data || length == 0 || length > MAX_PACKET_LENGTH) return;
  portENTER_CRITICAL(&mux_);
  if (rxCount_ < RX_PACKET_QUEUE_SIZE) {
    PacketSlot& slot = rxPackets_[rxHead_];
    slot.length = static_cast<uint16_t>(length);
    memcpy(slot.data, data, length);
    rxHead_ = (rxHead_ + 1) % RX_PACKET_QUEUE_SIZE;
    ++rxCount_;
  } else {
    // RX callback never blocks; overflow is surfaced by the main-loop state machine.
    portEXIT_CRITICAL(&mux_);
    emitError(1, connectedPeerId_, 0, 0xFF, NACK_RX_BUSY);
    return;
  }
  portEXIT_CRITICAL(&mux_);
}

void Manager::noteServerConnected(uint16_t connectionHandle) {
  // The transport manager maintains one active peer at a time. Reject any
  // additional peripheral-side link rather than corrupting connection state.
  if (serverConnectionActive_ && serverConnectionHandle_ != connectionHandle) {
    if (server_) static_cast<NimBLEServer*>(server_)->disconnect(connectionHandle);
    return;
  }
  serverConnectionHandle_ = connectionHandle;
  serverConnectionActive_ = true;
  serverConnectStartedMs_ = millis();
  serverHandshakeTimedOut_ = false;
  connected_ = false; // identity write completes the peer handshake.
  connectedPeerId_ = 0;
  mtu_ = 23;
}

void Manager::noteServerDisconnected(uint16_t connectionHandle, int reason) {
  if (!serverConnectionActive_ || serverConnectionHandle_ != connectionHandle) return;
  if (connectedPeerId_) pendingDisconnectedPeerId_ = connectedPeerId_;
  disconnectReason_ = reason;
  serverDisconnectPending_ = true;
  serverConnectionActive_ = false;
}

void Manager::noteClientConnected() { clientConnectComplete_ = true; }
void Manager::noteClientDisconnected(int reason) {
  if (connectedPeerId_) pendingDisconnectedPeerId_ = connectedPeerId_;
  disconnectReason_ = reason;
  clientDisconnectPending_ = true;
}
void Manager::noteRemoteIdentity(uint16_t peerId) {
  pendingRemoteIdentity_ = peerId;
  remoteIdentityPending_ = true;
}

uint16_t Manager::fragmentDataCapacity() const {
  // The packet buffer and requested local MTU are bounded at 256. Clamp even
  // if a peer or future stack build reports a larger negotiated MTU.
  uint16_t mtu = negotiatedMtu();
  if (mtu > MAX_ATT_MTU) mtu = MAX_ATT_MTU;
  if (mtu <= 12) return 1;
  const uint16_t capacity = mtu - 3 - 7 - 2;
  return capacity > MAX_MESSAGE_LENGTH ? MAX_MESSAGE_LENGTH : capacity;
}

bool Manager::sendToPeer(uint16_t peerId, const uint8_t* data, uint16_t length,
                         uint16_t& messageId) {
  if (!connected_ || peerId != connectedPeerId_ || tx_.active ||
      length > MAX_MESSAGE_LENGTH || (length && !data)) return false;
  const uint16_t capacity = fragmentDataCapacity();
  uint8_t fragments = length == 0 ? 1 : static_cast<uint8_t>((length + capacity - 1) / capacity);
  if (fragments == 0 || fragments > MAX_FRAGMENTS) return false;
  tx_ = Message();
  tx_.active = true;
  tx_.peerId = peerId;
  tx_.messageId = nextMessageId_++;
  // 0 is a valid wrapping message ID.
  tx_.totalLength = length;
  tx_.fragmentCount = fragments;
  tx_.fragmentDataLength = capacity;
  if (length) memcpy(tx_.data, data, length);
  tx_.lastActionMs = millis();
  txWaitingAck_ = false;
  txWaitingResult_ = false;
  messageId = tx_.messageId;
  return true;
}

bool Manager::getReceivedData(uint16_t peerId, uint16_t messageId,
                              uint8_t* destination, uint16_t capacity, uint16_t& length) {
  if (!rxMessageReady_ || peerId != readyPeerId_ || messageId != readyMessageId_ ||
      (readyLength_ && !destination) || capacity < readyLength_) return false;
  length = readyLength_;
  if (length) memcpy(destination, readyData_, length);
  rxMessageReady_ = false;
  readyPeerId_ = 0;
  readyMessageId_ = 0;
  readyLength_ = 0;
  rx_ = Reassembly();
  result_ = PendingResult();
  result_.active = true;
  result_.peerId = peerId;
  result_.messageId = messageId;
  result_.status = RESULT_CONSUMED;
  result_.lastActionMs = millis();
  sendMessageResult(messageId, RESULT_CONSUMED);
  return true;
}

bool Manager::buildAndSendSimple(uint8_t type, uint16_t messageId,
                                 uint8_t fragmentIndex, uint8_t errorCode) {
  uint8_t packet[8]{};
  packet[0] = type;
  writeU16(packet + 1, messageId);
  uint16_t length = 3;
  if (type == PKT_ACK) {
    packet[3] = fragmentIndex;
    length = 4;
  } else if (type == PKT_NACK) {
    packet[3] = fragmentIndex;
    packet[4] = errorCode;
    length = 5;
  }
  writeU16(packet + length, packetCrc(packet, length));
  return transmitPacket(packet, length + 2);
}

bool Manager::sendMessageResult(uint16_t messageId, uint8_t status) {
  uint8_t packet[6];
  packet[0] = PKT_MESSAGE_RESULT;
  writeU16(packet + 1, messageId);
  packet[3] = status;
  writeU16(packet + 4, packetCrc(packet, 4));
  return transmitPacket(packet, sizeof(packet));
}

void Manager::emitError(uint8_t direction, uint16_t peerId, uint16_t messageId,
                        uint8_t fragmentIndex, uint8_t errorCode) {
  uint8_t payload[8];
  payload[0] = direction;
  writeU16(payload + 1, peerId);
  writeU16(payload + 3, messageId);
  payload[5] = fragmentIndex;
  payload[6] = errorCode;
  payload[7] = 0;
  enqueueEvent(EVENT_ERROR, payload, sizeof(payload));
}

void Manager::failTransmit(uint8_t errorCode, uint8_t fragmentIndex) {
  if (!tx_.active) return;
  emitError(0, tx_.peerId, tx_.messageId, fragmentIndex, errorCode);
  tx_ = Message();
  txWaitingAck_ = false;
  txWaitingResult_ = false;
}

void Manager::handleData(const uint8_t* p, uint16_t length) {
  if (length < 9) return;
  const uint16_t messageId = readU16(p + 1);
  const uint8_t fragmentIndex = p[3];
  const uint8_t fragmentCount = p[4];
  const uint16_t totalLength = readU16(p + 5);
  const uint16_t payloadLength = length - 9;
  const uint16_t capacity = fragmentDataCapacity();

  if (totalLength > MAX_MESSAGE_LENGTH) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_MESSAGE_TOO_LARGE); return;
  }
  if (fragmentCount == 0 || fragmentCount > MAX_FRAGMENTS || fragmentIndex >= fragmentCount) {
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

  if (!rx_.active) {
    if (lastCompletedValid_ && messageId == lastCompletedMessageId_ &&
        lastCompletedFragmentCount_ && fragmentIndex < lastCompletedFragmentCount_) {
      buildAndSendSimple(PKT_ACK, messageId, fragmentIndex);
      return;
    }
    if (result_.active && messageId == result_.messageId) {
      // A sender may retransmit a fragment after receiving MESSAGE_RESULT but
      // before its RESULT_ACK reaches the peer. Re-ACK it without re-delivery.
      buildAndSendSimple(PKT_ACK, messageId, fragmentIndex);
      return;
    }
    if (result_.active) {
      // Preserve the one pending final-result record until its ACK arrives.
      buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_RX_BUSY);
      return;
    }
    if (rxMessageReady_) {
      if (messageId == readyMessageId_ && fragmentIndex < lastCompletedFragmentCount_) {
        buildAndSendSimple(PKT_ACK, messageId, fragmentIndex); return;
      }
      buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_RX_QUEUE_FULL); return;
    }
    if (fragmentIndex != 0) {
      buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_INVALID_FRAGMENT); return;
    }
    rx_ = Reassembly();
    rx_.active = true;
    rx_.peerId = connectedPeerId_;
    rx_.messageId = messageId;
    rx_.totalLength = totalLength;
    rx_.fragmentDataLength = capacity;
    rx_.fragmentCount = fragmentCount;
    rx_.lastActivityMs = millis();
  }

  if (rx_.messageId != messageId) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex,
                       rx_.complete ? NACK_RX_QUEUE_FULL : NACK_RX_BUSY); return;
  }
  if (rx_.totalLength != totalLength || rx_.fragmentCount != fragmentCount ||
      rx_.fragmentDataLength != capacity) {
    buildAndSendSimple(PKT_NACK, messageId, fragmentIndex, NACK_METADATA_MISMATCH); return;
  }

  const uint64_t bit = (uint64_t(1) << fragmentIndex);
  if (rx_.receivedMask & bit) {
    rx_.lastActivityMs = millis();
    buildAndSendSimple(PKT_ACK, messageId, fragmentIndex); return;
  }
  if (!rx_.complete && payloadLength)
    memcpy(rx_.data + uint16_t(fragmentIndex) * capacity, p + 7, payloadLength);
  rx_.receivedMask |= bit;
  ++rx_.receivedCount;
  rx_.lastActivityMs = millis();
  buildAndSendSimple(PKT_ACK, messageId, fragmentIndex);

  if (rx_.receivedCount == rx_.fragmentCount && !rx_.complete) {
    rx_.complete = true;
    rx_.completedAtMs = millis();
    rxMessageReady_ = true;
    readyPeerId_ = rx_.peerId;
    readyMessageId_ = rx_.messageId;
    readyLength_ = rx_.totalLength;
    if (readyLength_) memcpy(readyData_, rx_.data, readyLength_);
    lastCompletedMessageId_ = rx_.messageId;
    lastCompletedFragmentCount_ = rx_.fragmentCount;
    lastCompletedValid_ = true;
    uint8_t payload[6];
    writeU16(payload, readyPeerId_);
    writeU16(payload + 2, readyMessageId_);
    writeU16(payload + 4, readyLength_);
    enqueueEvent(EVENT_DATA_RECEIVED, payload, sizeof(payload));
  }
}

void Manager::handleAck(const uint8_t* p, uint16_t length) {
  if (length != 6 || !tx_.active || !txWaitingAck_) return;
  const uint16_t messageId = readU16(p + 1);
  const uint8_t fragmentIndex = p[3];
  if (messageId != tx_.messageId || fragmentIndex != tx_.fragmentIndex) return;
  txWaitingAck_ = false;
  tx_.retryCount = 0;
  if (tx_.fragmentIndex + 1 < tx_.fragmentCount) {
    ++tx_.fragmentIndex;
    tx_.lastActionMs = millis();
  } else {
    txWaitingResult_ = true;
    tx_.lastActionMs = millis();
    tx_.retryCount = 0;
  }
}

void Manager::handleNack(const uint8_t* p, uint16_t length) {
  if (length != 7 || !tx_.active || !txWaitingAck_) return;
  const uint16_t messageId = readU16(p + 1);
  const uint8_t fragmentIndex = p[3];
  const uint8_t errorCode = p[4];
  if (messageId != tx_.messageId || fragmentIndex != tx_.fragmentIndex) return;
  if (errorCode == NACK_RX_QUEUE_FULL || errorCode == NACK_RX_BUSY) {
    if (tx_.retryCount >= config_.maxRetries) failTransmit(errorCode, fragmentIndex);
    else tx_.lastActionMs = millis() - ACK_TIMEOUT_MS;
  } else {
    failTransmit(errorCode, fragmentIndex);
  }
}

void Manager::handleMessageResult(const uint8_t* p, uint16_t length) {
  if (length != 6) return;
  const uint16_t messageId = readU16(p + 1);
  const uint8_t status = p[3];
  uint8_t ack[5];
  ack[0] = PKT_MESSAGE_RESULT_ACK;
  writeU16(ack + 1, messageId);
  writeU16(ack + 3, packetCrc(ack, 3));
  transmitPacket(ack, sizeof(ack));
  if (status > RESULT_CONSUMPTION_TIMEOUT) {
    emitError(0, connectedPeerId_, messageId, 0xFF, 11);
    return;
  }

  if (tx_.active && tx_.messageId == messageId) {
    // A final result proves the receiver reassembled the whole message, even
    // if the ACK for the final fragment was lost on its way back.
    if (status == RESULT_CONSUMED) {
      uint8_t payload[6];
      writeU16(payload, tx_.peerId);
      writeU16(payload + 2, tx_.messageId);
      writeU16(payload + 4, tx_.totalLength);
      enqueueEvent(EVENT_DATA_SENT, payload, sizeof(payload));
    } else {
      emitError(0, tx_.peerId, tx_.messageId, 0xFF, RESULT_CONSUMPTION_TIMEOUT);
    }
    tx_ = Message();
    txWaitingAck_ = false;
    txWaitingResult_ = false;
  }
}

void Manager::handleMessageResultAck(const uint8_t* p, uint16_t length) {
  if (length != 5 || !result_.active) return;
  if (readU16(p + 1) == result_.messageId) result_ = PendingResult();
}

void Manager::handlePacket(const uint8_t* p, uint16_t length) {
  if (length < 3) return;
  const uint16_t receivedCrc = readU16(p + length - 2);
  if (packetCrc(p, length - 2) != receivedCrc) return;
  switch (p[0]) {
    case PKT_DATA: handleData(p, length); break;
    case PKT_ACK: handleAck(p, length); break;
    case PKT_NACK: handleNack(p, length); break;
    case PKT_MESSAGE_RESULT: handleMessageResult(p, length); break;
    case PKT_MESSAGE_RESULT_ACK: handleMessageResultAck(p, length); break;
    default: break;
  }
}

void Manager::serviceTransmit() {
  if (!tx_.active || !connected_) return;
  const uint32_t now = millis();
  if (txWaitingAck_) {
    if (!elapsed(now, tx_.lastActionMs, ACK_TIMEOUT_MS)) return;
    if (tx_.retryCount >= config_.maxRetries) {
      failTransmit(8, tx_.fragmentIndex); return;
    }
    ++tx_.retryCount;
  } else if (txWaitingResult_) {
    if (!elapsed(now, tx_.lastActionMs, RESULT_TIMEOUT_MS)) return;
    if (tx_.retryCount >= config_.maxRetries) {
      failTransmit(9, 0xFF); return; // final result not observed: delivery is indeterminate.
    }
    ++tx_.retryCount;
  }

  if (!txWaitingResult_) {
    const uint16_t capacity = tx_.fragmentDataLength;
    const uint16_t offset = uint16_t(tx_.fragmentIndex) * capacity;
    const uint16_t remaining = tx_.totalLength > offset ? tx_.totalLength - offset : 0;
    const uint16_t chunk = remaining > capacity ? capacity : remaining;
    uint8_t packet[MAX_PACKET_LENGTH]{};
    packet[0] = PKT_DATA;
    writeU16(packet + 1, tx_.messageId);
    packet[3] = tx_.fragmentIndex;
    packet[4] = tx_.fragmentCount;
    writeU16(packet + 5, tx_.totalLength);
    if (chunk) memcpy(packet + 7, tx_.data + offset, chunk);
    writeU16(packet + 7 + chunk, packetCrc(packet, 7 + chunk));
    if (!transmitPacket(packet, 9 + chunk)) {
      if (tx_.retryCount >= config_.maxRetries) {
        failTransmit(7, tx_.fragmentIndex);
        return;
      }
      txWaitingAck_ = true;
      tx_.lastActionMs = now;
      return;
    }
    txWaitingAck_ = true;
    tx_.lastActionMs = now;
  } else {
    tx_.lastActionMs = now;
  }
}

void Manager::serviceReceiveTimeout() {
  if (!rxMessageReady_ || !rx_.active || !rx_.complete) return;
  if (!elapsed(millis(), rx_.completedAtMs, config_.rxMessageTimeoutMs)) return;
  const uint16_t peer = readyPeerId_;
  const uint16_t id = readyMessageId_;
  rxMessageReady_ = false;
  readyPeerId_ = readyMessageId_ = readyLength_ = 0;
  rx_ = Reassembly();
  uint8_t payload[4];
  writeU16(payload, peer); writeU16(payload + 2, id);
  enqueueEvent(EVENT_DATA_EXPIRED, payload, sizeof(payload));
  result_ = PendingResult();
  result_.active = true; result_.peerId = peer; result_.messageId = id;
  result_.status = RESULT_CONSUMPTION_TIMEOUT; result_.lastActionMs = millis();
  sendMessageResult(id, RESULT_CONSUMPTION_TIMEOUT);
}

void Manager::serviceResultRetry() {
  if (!result_.active || !connected_) return;
  if (!elapsed(millis(), result_.lastActionMs, RESULT_TIMEOUT_MS)) return;
  if (result_.retryCount >= config_.maxRetries) {
    emitError(1, result_.peerId, result_.messageId, 0xFF, 9);
    result_ = PendingResult();
    return;
  }
  ++result_.retryCount;
  result_.lastActionMs = millis();
  sendMessageResult(result_.messageId, result_.status);
}

void Manager::clearMessageState(bool reportIndeterminate) {
  if (reportIndeterminate && tx_.active) emitError(0, tx_.peerId, tx_.messageId, tx_.fragmentIndex, 9);
  tx_ = Message();
  txWaitingAck_ = false;
  txWaitingResult_ = false;
  rx_ = Reassembly();
  rxMessageReady_ = false;
  lastCompletedValid_ = false;
  lastCompletedMessageId_ = 0;
  lastCompletedFragmentCount_ = 0;
  readyPeerId_ = readyMessageId_ = readyLength_ = 0;
  result_ = PendingResult();
  portENTER_CRITICAL(&mux_);
  rxHead_ = rxTail_ = rxCount_ = 0;
  portEXIT_CRITICAL(&mux_);
}

bool Manager::getPeerStatus(uint16_t peerId, PeerStatus& status) const {
  status = PeerStatus();
  status.mode = config_.mode;
  status.enabled = enabled_ ? 1 : 0;
  status.advertising = advertising_ ? 1 : 0;
  status.scanning = scanning_ ? 1 : 0;
  status.connected = connected_ && connectedPeerId_ == peerId ? 1 : 0;
  status.peerId = status.connected ? connectedPeerId_ : peerId;
  status.mtu = status.connected ? negotiatedMtu() : 23;
  if (status.connected) status.state = PEER_CONNECTED;
  else if (peerConfigured(peerId)) {
    status.state = PEER_CONFIGURED;
    for (uint8_t i = 0; i < config_.peerCount; ++i) {
      if (config_.peers[i] == peerId && foundNotified_[i]) {
        status.state = PEER_FOUND;
        break;
      }
    }
  } else status.state = PEER_UNKNOWN;
  return true;
}

void Manager::poll() {
  if (!initialized_) return;

  if (clientDisconnectPending_ || serverDisconnectPending_) {
    clientDisconnectPending_ = false;
    serverDisconnectPending_ = false;
    const uint16_t disconnectedPeer = pendingDisconnectedPeerId_;
    pendingDisconnectedPeerId_ = 0;
    const int reason = disconnectReason_;
    connected_ = false;
    connectedPeerId_ = 0;
    remoteRxCharacteristic_ = nullptr;
    remoteTxCharacteristic_ = nullptr;
    remoteIdentityReadCharacteristic_ = nullptr;
    remoteIdentityWriteCharacteristic_ = nullptr;
    clearMessageState(true);
    uint8_t payload[3]; writeU16(payload, disconnectedPeer); payload[2] = static_cast<uint8_t>(reason);
    enqueueEvent(EVENT_PEER_DISCONNECTED, payload, sizeof(payload));
    uint8_t state[4] = {static_cast<uint8_t>(enabled_ ? 1 : 0), config_.mode,
                        static_cast<uint8_t>(advertising_ ? 1 : 0),
                        static_cast<uint8_t>(scanning_ ? 1 : 0)};
    enqueueEvent(EVENT_STATE_CHANGED, state, sizeof(state));
    if (enabled_ && config_.mode == MODE_PERIPHERAL) advertisingRestartPending_ = true;
    if (enabled_ && config_.mode == MODE_HUB && config_.reconnectEnabled && requestedPeerId_) {
      connectRequested_ = true;
      connectStartedMs_ = millis();
      lastReconnectAttemptMs_ = connectStartedMs_ - config_.reconnectIntervalMs;
    }
  }

  if (serverConnectionActive_ && !connected_ && !serverHandshakeTimedOut_ &&
      elapsed(millis(), serverConnectStartedMs_, config_.connectionTimeoutMs)) {
    serverHandshakeTimedOut_ = true;
    const uint16_t handle = serverConnectionHandle_;
    uint8_t payload[3]; writeU16(payload, 0); payload[2] = 5;
    enqueueEvent(EVENT_CONNECTION_FAILED, payload, sizeof(payload));
    if (server_) static_cast<NimBLEServer*>(server_)->disconnect(handle);
  }

  if (remoteIdentityPending_) {
    remoteIdentityPending_ = false;
    const uint16_t peer = pendingRemoteIdentity_;
    if (peerConfigured(peer) && config_.mode == MODE_PERIPHERAL &&
        serverConnectionActive_ && !serverHandshakeTimedOut_) {
      connectedPeerId_ = peer;
      connected_ = true;
      mtu_ = static_cast<NimBLEServer*>(server_)->getPeerMTU(serverConnectionHandle_);
      uint8_t payload[2]; writeU16(payload, peer);
      enqueueEvent(EVENT_PEER_CONNECTED, payload, sizeof(payload));
    } else if (config_.mode == MODE_PERIPHERAL && server_ && serverConnectionActive_) {
      uint8_t payload[3]; writeU16(payload, peer); payload[2] = 1;
      enqueueEvent(EVENT_CONNECTION_FAILED, payload, sizeof(payload));
      static_cast<NimBLEServer*>(server_)->disconnect(serverConnectionHandle_);
    }
  }

  if (advertisingRestartPending_) {
    advertisingRestartPending_ = false;
    if (auto* advertising = NimBLEDevice::getAdvertising()) {
      advertising->start();
      advertising_ = advertising->isAdvertising();
    }
  }

  if (scanAddressReady_ && config_.mode == MODE_HUB && enabled_) {
    scanAddressReady_ = false;
    auto* scan = static_cast<NimBLEScan*>(scan_);
    scan->stop();
    scanning_ = false;
    const NimBLEAddress address(foundAddressBytes_, foundAddressType_);
    if (client_) {
      auto* oldClient = static_cast<NimBLEClient*>(client_);
      if (oldClient->isConnected()) oldClient->disconnect();
    }
    NimBLEClient* client = static_cast<NimBLEClient*>(client_);
    if (!client) {
      client = NimBLEDevice::getDisconnectedClient();
      if (!client) client = NimBLEDevice::createClient();
      if (client) {
        client_ = client;
        auto* callbacks = static_cast<BleNimBLEClientCallbacks*>(clientCallbacks_);
        client->setClientCallbacks(callbacks, false);
      }
    }
    if (client) {
      client->setConnectTimeout(config_.connectionTimeoutMs);
      const bool started = client->connect(address, true, true, true); // async connection
      if (!started) {
        uint8_t payload[3]; writeU16(payload, requestedPeerId_); payload[2] = 1;
        enqueueEvent(EVENT_CONNECTION_FAILED, payload, sizeof(payload));
        connectRequested_ = false;
        requestedPeerId_ = 0;
      }
    } else {
      uint8_t payload[3]; writeU16(payload, requestedPeerId_); payload[2] = 2;
      enqueueEvent(EVENT_CONNECTION_FAILED, payload, sizeof(payload));
      connectRequested_ = false;
      requestedPeerId_ = 0;
    }
  }

  if (clientConnectComplete_) {
    clientConnectComplete_ = false;
    NimBLEClient* client = static_cast<NimBLEClient*>(client_);
    if (client && client->isConnected()) {
      if (!enabled_ || config_.mode != MODE_HUB || !connectRequested_) {
        client->disconnect();
      } else {
        auto failHandshake = [&](uint16_t peerId, uint8_t reason) {
          uint8_t payload[3]; writeU16(payload, peerId); payload[2] = reason;
          enqueueEvent(EVENT_CONNECTION_FAILED, payload, sizeof(payload));
          connectRequested_ = false;
          requestedPeerId_ = 0;
          client->disconnect();
        };
        NimBLERemoteService* service = client->getService(SERVICE_UUID);
        if (!service) {
          failHandshake(requestedPeerId_, 5);
        } else {
          auto* remoteRx = service->getCharacteristic(RX_UUID);
          auto* remoteTx = service->getCharacteristic(TX_UUID);
          auto* remoteIdRead = service->getCharacteristic(ID_READ_UUID);
          auto* remoteIdWrite = service->getCharacteristic(ID_WRITE_UUID);
          auto* remoteProtocolVersion = service->getCharacteristic(PROTOCOL_VERSION_UUID);
          if (!remoteRx || !remoteRx->canWrite() || !remoteTx || !remoteTx->canNotify() ||
              !remoteIdRead || !remoteIdRead->canRead() || !remoteIdWrite || !remoteIdWrite->canWrite() ||
              !remoteProtocolVersion || !remoteProtocolVersion->canRead()) {
            failHandshake(requestedPeerId_, 5);
          } else {
            const NimBLEAttValue versionValue = remoteProtocolVersion->readValue();
            const NimBLEAttValue idValue = remoteIdRead->readValue();
            if (versionValue.length() != 1 || versionValue.data()[0] != BLE_PROTOCOL_VERSION ||
                idValue.length() != 2) {
              failHandshake(requestedPeerId_, 5);
            } else {
              const uint16_t peerId = readU16(idValue.data());
              if (peerId != requestedPeerId_ || !peerConfigured(peerId)) {
                failHandshake(peerId, 3);
              } else {
                uint8_t ownId[2]; writeU16(ownId, localDeviceId_);
                if (!remoteIdWrite->writeValue(ownId, sizeof(ownId), true) ||
                    !remoteTx->subscribe(true, onRemoteNotify)) {
                  failHandshake(peerId, 5);
                } else {
                  remoteRxCharacteristic_ = remoteRx;
                  remoteTxCharacteristic_ = remoteTx;
                  remoteIdentityReadCharacteristic_ = remoteIdRead;
                  remoteIdentityWriteCharacteristic_ = remoteIdWrite;
                  connectedPeerId_ = peerId;
                  connected_ = true;
                  mtu_ = client->getMTU();
                  connectRequested_ = false;
                  uint8_t payload[2]; writeU16(payload, peerId);
                  enqueueEvent(EVENT_PEER_CONNECTED, payload, sizeof(payload));
                  uint8_t state[4] = {1, config_.mode, 0, 0};
                  enqueueEvent(EVENT_STATE_CHANGED, state, sizeof(state));
                }
              }
            }
          }
        }
      }
    }
  }

  if (enabled_ && config_.mode == MODE_HUB && connectRequested_ && !connected_ &&
      !scanning_ && !scanAddressReady_) {
    const uint32_t now = millis();
    if (elapsed(now, lastReconnectAttemptMs_, config_.reconnectIntervalMs) ||
        lastReconnectAttemptMs_ == connectStartedMs_) {
      NimBLEScan* scan = static_cast<NimBLEScan*>(scan_);
      scanning_ = scan->start(0);
      lastReconnectAttemptMs_ = now;
    }
  }

  if (enabled_ && config_.mode == MODE_HUB && connectRequested_ &&
      !connected_ && elapsed(millis(), connectStartedMs_, config_.connectionTimeoutMs)) {
    if (auto* scan = static_cast<NimBLEScan*>(scan_)) scan->stop();
    scanning_ = false;
    uint8_t payload[3]; writeU16(payload, requestedPeerId_); payload[2] = 4;
    enqueueEvent(EVENT_CONNECTION_FAILED, payload, sizeof(payload));
    if (client_) {
      auto* client = static_cast<NimBLEClient*>(client_);
      if (client->isConnected()) client->disconnect();
      else client->cancelConnect();
    }
    connectRequested_ = false;
    requestedPeerId_ = 0;
  }

  if (rx_.active && !rx_.complete &&
      elapsed(millis(), rx_.lastActivityMs, REASSEMBLY_TIMEOUT_MS)) {
    emitError(1, rx_.peerId, rx_.messageId, 0xFF, 10);
    rx_ = Reassembly();
  }

  if (!connected_) return;

  for (;;) {
    PacketSlot slot;
    bool have = false;
    portENTER_CRITICAL(&mux_);
    if (rxCount_) {
      slot = rxPackets_[rxTail_];
      rxTail_ = (rxTail_ + 1) % RX_PACKET_QUEUE_SIZE;
      --rxCount_;
      have = true;
    }
    portEXIT_CRITICAL(&mux_);
    if (!have) break;
    handlePacket(slot.data, slot.length);
  }

  serviceTransmit();
  serviceReceiveTimeout();
  serviceResultRetry();
}

} // namespace UwbBle
