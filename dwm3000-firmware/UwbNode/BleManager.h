#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace UwbBle {

static constexpr uint16_t MAX_MESSAGE_LENGTH = 496;
static constexpr uint8_t MAX_FRAGMENTS = 46;
static constexpr uint8_t MAX_PEERS = 8;
// Reserve one NimBLE host slot beyond the eight application peer slots.
static constexpr uint8_t MAX_ACTIVE_CONNECTIONS = 8;
static constexpr uint16_t MAX_ATT_MTU = 256;
static constexpr uint16_t MAX_PACKET_LENGTH = MAX_ATT_MTU - 3;
static constexpr uint8_t RX_PACKET_QUEUE_SIZE = 4;
static constexpr uint8_t EVENT_QUEUE_SIZE = 16;

// Values are part of the serial API and persisted configuration.
enum Mode : uint8_t { MODE_IDLE = 0, MODE_HUB = 1, MODE_PERIPHERAL = 2 };
enum PeerState : uint8_t { PEER_UNKNOWN = 0, PEER_CONFIGURED = 1, PEER_FOUND = 2, PEER_CONNECTED = 3 };
enum EventCode : uint8_t {
  EVENT_STATE_CHANGED = 0,
  EVENT_PEER_FOUND,
  EVENT_PEER_CONNECTED,
  EVENT_PEER_DISCONNECTED,
  EVENT_CONNECTION_FAILED,
  EVENT_DATA_RECEIVED,
  EVENT_ERROR,
  EVENT_DATA_SENT,
  EVENT_DATA_EXPIRED
};
enum NackCode : uint8_t {
  NACK_RX_QUEUE_FULL = 1,
  NACK_RX_BUSY = 2,
  NACK_MESSAGE_TOO_LARGE = 3,
  NACK_INVALID_FRAGMENT = 4,
  NACK_METADATA_MISMATCH = 5,
  NACK_UNSUPPORTED_VERSION = 6
};

struct Config {
  uint8_t mode = MODE_IDLE;
  uint16_t advertisingIntervalMs = 160;
  uint16_t scanIntervalMs = 160;
  uint16_t scanWindowMs = 80;
  uint8_t reconnectEnabled = 0;
  uint16_t reconnectIntervalMs = 1000;
  uint16_t connectionTimeoutMs = 5000;
  uint8_t maxRetries = 1;
  uint16_t rxMessageTimeoutMs = 1000;
  uint8_t peerCount = 0;
  uint16_t peers[MAX_PEERS]{};
};

struct Event {
  uint8_t code = 0;
  uint8_t length = 0;
  uint8_t payload[10]{};
};

struct PeerStatus {
  uint8_t state = PEER_UNKNOWN;
  uint8_t mode = MODE_IDLE;
  uint8_t enabled = 0;
  uint8_t advertising = 0;
  uint8_t scanning = 0;
  uint8_t connected = 0;
  uint16_t peerId = 0;
  uint16_t mtu = 23;
};

class Manager {
 public:
  Manager();
  bool begin(uint16_t localDeviceId);
  bool configure(const Config& config);
  bool configure(const Config& config, uint16_t localDeviceId);
  bool setPeers(const uint16_t* peers, uint8_t count);
  bool setLocalDeviceId(uint16_t localDeviceId);
  bool start();
  void stop();
  bool connect(uint16_t peerId);
  bool disconnect(uint16_t peerId);
  void poll();

  bool sendToPeer(uint16_t peerId, const uint8_t* data, uint16_t length, uint16_t& messageId);
  bool getReceivedData(uint16_t peerId, uint16_t messageId,
                       uint8_t* destination, uint16_t capacity, uint16_t& length);

  bool popEvent(Event& event);
  bool getPeerStatus(uint16_t peerId, PeerStatus& status) const;
  const Config& config() const { return config_; }
  bool enabled() const { return enabled_; }
  bool connected() const { return connected_; }
  uint16_t connectedPeerId() const { return connectedPeerId_; }
  uint16_t negotiatedMtu() const;

  // Entry points used by NimBLE callbacks. They only copy bounded data/state;
  // protocol processing is deferred to poll() in the firmware loop.
  void enqueueReceivedPacket(const uint8_t* data, size_t length);
  void noteServerConnected(uint16_t connectionHandle);
  void noteServerDisconnected(uint16_t connectionHandle, int reason);
  void noteClientConnected();
  void noteClientDisconnected(int reason);
  void noteRemoteIdentity(uint16_t peerId);

 private:
  friend class BleNimBLEScanCallbacks;
  friend class BleNimBLEServerCallbacks;
  friend class BleNimBLERxCallbacks;
  friend class BleNimBLEIdentityCallbacks;
  friend class BleNimBLEClientCallbacks;

  struct PacketSlot {
    uint16_t length = 0;
    uint8_t data[MAX_PACKET_LENGTH]{};
  };
  struct Message {
    bool active = false;
    uint16_t peerId = 0;
    uint16_t messageId = 0;
    uint16_t totalLength = 0;
    uint8_t fragmentCount = 0;
    uint8_t fragmentIndex = 0;
    uint8_t retryCount = 0;
    uint16_t fragmentDataLength = 0;
    uint32_t lastActionMs = 0;
    uint8_t data[MAX_MESSAGE_LENGTH]{};
  };
  struct Reassembly {
    bool active = false;
    uint16_t peerId = 0;
    uint16_t messageId = 0;
    uint16_t totalLength = 0;
    uint16_t fragmentDataLength = 0;
    uint8_t fragmentCount = 0;
    uint8_t receivedCount = 0;
    uint64_t receivedMask = 0;
    uint32_t lastActivityMs = 0;
    uint32_t completedAtMs = 0;
    bool complete = false;
    uint8_t data[MAX_MESSAGE_LENGTH]{};
  };
  struct PendingResult {
    bool active = false;
    uint16_t peerId = 0;
    uint16_t messageId = 0;
    uint8_t status = 0;
    uint8_t retryCount = 0;
    uint32_t lastActionMs = 0;
  };

  bool enqueueEvent(uint8_t code, const uint8_t* payload, uint8_t length);
  bool transmitPacket(const uint8_t* packet, uint16_t length);
  bool buildAndSendSimple(uint8_t type, uint16_t messageId, uint8_t fragmentIndex,
                          uint8_t errorCode = 0);
  bool sendMessageResult(uint16_t messageId, uint8_t status);
  void handlePacket(const uint8_t* packet, uint16_t length);
  void handleData(const uint8_t* packet, uint16_t length);
  void handleAck(const uint8_t* packet, uint16_t length);
  void handleNack(const uint8_t* packet, uint16_t length);
  void handleMessageResult(const uint8_t* packet, uint16_t length);
  void handleMessageResultAck(const uint8_t* packet, uint16_t length);
  void serviceTransmit();
  void serviceReceiveTimeout();
  void serviceResultRetry();
  void failTransmit(uint8_t errorCode, uint8_t fragmentIndex);
  uint16_t fragmentDataCapacity() const;
  bool peerConfigured(uint16_t peerId) const;
  void clearMessageState(bool reportIndeterminate);
  void emitError(uint8_t direction, uint16_t peerId, uint16_t messageId,
                 uint8_t fragmentIndex, uint8_t errorCode);
  static uint16_t packetCrc(const uint8_t* data, size_t length);
  static uint16_t readU16(const uint8_t* p);
  static void writeU16(uint8_t* p, uint16_t value);

  Config config_{};
  uint16_t localDeviceId_ = 0;
  uint16_t nextMessageId_ = 1;
  bool initialized_ = false;
  bool enabled_ = false;
  bool advertising_ = false;
  bool scanning_ = false;
  volatile bool connected_ = false;
  volatile bool clientConnectComplete_ = false;
  volatile bool clientDisconnectPending_ = false;
  volatile bool serverDisconnectPending_ = false;
  volatile bool remoteIdentityPending_ = false;
  volatile uint16_t connectedPeerId_ = 0;
  volatile uint16_t serverConnectionHandle_ = 0;
  volatile bool serverConnectionActive_ = false;
  uint32_t serverConnectStartedMs_ = 0;
  bool serverHandshakeTimedOut_ = false;
  volatile uint16_t pendingRemoteIdentity_ = 0;
  volatile int disconnectReason_ = 0;
  volatile uint16_t pendingDisconnectedPeerId_ = 0;
  volatile uint16_t requestedPeerId_ = 0;
  uint16_t mtu_ = 23;
  uint32_t connectStartedMs_ = 0;
  uint32_t lastReconnectAttemptMs_ = 0;
  volatile bool connectRequested_ = false;
  volatile bool scanAddressReady_ = false;
  uint8_t foundAddressBytes_[6]{};
  uint8_t foundAddressType_ = 0;
  bool serviceSetupPending_ = false;
  bool advertisingRestartPending_ = false;
  bool foundNotified_[MAX_PEERS]{};
  Message tx_{};
  bool txWaitingAck_ = false;
  bool txWaitingResult_ = false;
  Reassembly rx_{};
  bool rxMessageReady_ = false;
  uint16_t readyPeerId_ = 0;
  uint16_t readyMessageId_ = 0;
  uint16_t readyLength_ = 0;
  uint8_t readyData_[MAX_MESSAGE_LENGTH]{};
  uint16_t lastCompletedMessageId_ = 0;
  uint8_t lastCompletedFragmentCount_ = 0;
  bool lastCompletedValid_ = false;
  PendingResult result_{};

  PacketSlot rxPackets_[RX_PACKET_QUEUE_SIZE]{};
  volatile uint8_t rxHead_ = 0;
  volatile uint8_t rxTail_ = 0;
  volatile uint8_t rxCount_ = 0;
  Event events_[EVENT_QUEUE_SIZE]{};
  volatile uint8_t eventHead_ = 0;
  volatile uint8_t eventTail_ = 0;
  volatile uint8_t eventCount_ = 0;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  // NimBLE objects are opaque here to keep NimBLE headers out of the main sketch.
  void* server_ = nullptr;
  void* rxCharacteristic_ = nullptr;
  void* txCharacteristic_ = nullptr;
  void* identityReadCharacteristic_ = nullptr;
  void* protocolVersionCharacteristic_ = nullptr;
  void* identityWriteCharacteristic_ = nullptr;
  void* scan_ = nullptr;
  void* client_ = nullptr;
  void* remoteRxCharacteristic_ = nullptr;
  void* remoteTxCharacteristic_ = nullptr;
  void* remoteIdentityReadCharacteristic_ = nullptr;
  void* remoteIdentityWriteCharacteristic_ = nullptr;
  void* scanCallbacks_ = nullptr;
  void* serverCallbacks_ = nullptr;
  void* rxCallbacks_ = nullptr;
  void* identityCallbacks_ = nullptr;
  void* clientCallbacks_ = nullptr;
};

} // namespace UwbBle
