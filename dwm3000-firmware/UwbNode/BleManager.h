#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace UwbBle {

static constexpr uint16_t MAX_MESSAGE_LENGTH = 496;
static constexpr uint8_t MAX_FRAGMENTS = 46;
static constexpr uint8_t MAX_PEERS = 8;
static constexpr uint8_t MAX_ACTIVE_CONNECTIONS = 8;
static constexpr uint16_t MAX_ATT_MTU = 256;
static constexpr uint16_t MAX_PACKET_LENGTH = MAX_ATT_MTU - 3;
static constexpr uint8_t RX_PACKET_QUEUE_SIZE = 16;
static constexpr uint8_t EVENT_QUEUE_SIZE = 24;
static constexpr uint8_t BLE_DIAGNOSTIC_TRACE_CAPACITY = 32;
static constexpr uint8_t BLE_DIAGNOSTIC_RECORD_SIZE = 13;

enum Mode : uint8_t { MODE_IDLE = 0, MODE_HUB = 1, MODE_PERIPHERAL = 2 };
enum PeerState : uint8_t {
  PEER_UNKNOWN = 0, PEER_CONFIGURED = 1, PEER_FOUND = 2, PEER_CONNECTED = 3,
  PEER_CONNECTING = 4, PEER_HANDSHAKING = 5, PEER_BACKOFF = 6
};
enum ConnectionFailureStage : uint8_t {
  FAILURE_STAGE_SCAN_STOP = 1,
  FAILURE_STAGE_CLIENT_ALLOCATE = 2,
  FAILURE_STAGE_GAP_START = 3,
  FAILURE_STAGE_GAP_RESULT = 4,
  FAILURE_STAGE_GATT_SERVICE = 5,
  FAILURE_STAGE_GATT_CHARACTERISTICS = 6,
  FAILURE_STAGE_PROTOCOL_VERSION = 7,
  FAILURE_STAGE_PEER_IDENTITY = 8,
  FAILURE_STAGE_IDENTITY_WRITE = 9,
  FAILURE_STAGE_NOTIFY_SUBSCRIBE = 10,
  FAILURE_STAGE_PERIPHERAL_IDENTITY = 11,
  FAILURE_STAGE_PERIPHERAL_HANDSHAKE_TIMEOUT = 12
};
enum DiagnosticCode : uint8_t {
  DIAG_INIT_COMPLETE = 1, DIAG_INIT_FAILED, DIAG_START_COMPLETE, DIAG_START_FAILED,
  DIAG_STOPPED, DIAG_SCAN_STARTED, DIAG_SCAN_START_FAILED, DIAG_SCAN_STOP_FAILED,
  DIAG_PEER_DISCOVERED, DIAG_CLIENT_ALLOC_FAILED, DIAG_CENTRAL_CONNECT_ATTEMPT,
  DIAG_CONNECTION_FAILED, DIAG_CENTRAL_CONNECTED, DIAG_CENTRAL_HANDSHAKE_STEP,
  DIAG_CENTRAL_HANDSHAKE_COMPLETE, DIAG_SERVER_CONNECTED, DIAG_SERVER_CAPACITY_REJECTED,
  DIAG_SERVER_SLOT_ALLOC_FAILED,
  DIAG_SERVER_IDENTITY_ACCEPTED, DIAG_SERVER_IDENTITY_REJECTED, DIAG_HANDSHAKE_TIMEOUT,
  DIAG_PEER_DISCONNECTED, DIAG_ADVERTISING_RESTART_FAILED, DIAG_SCAN_RESTART_FAILED,
  DIAG_GAP_EVENT_CALLBACK, DIAG_EVENT_QUEUE_FULL, DIAG_RX_QUEUE_FULL
};
enum EventCode : uint8_t {
  EVENT_STATE_CHANGED = 0, EVENT_PEER_FOUND, EVENT_PEER_CONNECTED,
  EVENT_PEER_DISCONNECTED, EVENT_CONNECTION_FAILED, EVENT_DATA_RECEIVED,
  EVENT_ERROR, EVENT_DATA_SENT, EVENT_DATA_EXPIRED
};
enum NackCode : uint8_t {
  NACK_RX_QUEUE_FULL = 1, NACK_RX_BUSY = 2, NACK_MESSAGE_TOO_LARGE = 3,
  NACK_INVALID_FRAGMENT = 4, NACK_METADATA_MISMATCH = 5,
  NACK_UNSUPPORTED_VERSION = 6
};

struct Config {
  uint8_t mode = MODE_IDLE;
  uint16_t advertisingIntervalMs = 160;
  uint16_t scanIntervalMs = 160;
  uint16_t scanWindowMs = 80;
  uint16_t reconnectIntervalMs = 1000; // Automatic retry interval; no opt-in switch.
  uint8_t autoStart = 0;            // Persisted boot preference; independent of runtime enablement.
  uint16_t connectionTimeoutMs = 5000;
  uint8_t maxRetries = 1;              // Message-delivery retries only.
  uint16_t rxMessageTimeoutMs = 1000;
  uint8_t peerCount = 0;               // Empty list means open mode.
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
  uint8_t lastFailureReason = 0;
  uint8_t lastFailureStage = 0;
  int16_t lastFailureDetail = 0;
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
  void poll();

  bool sendToPeer(uint16_t peerId, const uint8_t* data, uint16_t length, uint16_t& messageId);
  bool getReceivedData(uint16_t peerId, uint16_t messageId,
                       uint8_t* destination, uint16_t capacity, uint16_t& length);

  bool popEvent(Event& event);
  bool getPeerStatus(uint16_t peerId, PeerStatus& status) const;
  uint16_t getDiagnosticTrace(uint8_t* destination, uint16_t capacity) const;
  const Config& config() const { return config_; }
  bool enabled() const { return enabled_; }
  bool connected() const { return connected_; }
  uint16_t connectedPeerId() const { return connectedPeerId_; }
  uint16_t negotiatedMtu() const;

  // NimBLE callback entry points; all application work is deferred to poll().
  void notePeerDiscovered(uint16_t peerId, const uint8_t* address, uint8_t addressType);
  void noteServerConnected(uint16_t connectionHandle);
  void noteServerDisconnected(uint16_t connectionHandle, int reason);
  void noteServerMtuChanged(uint16_t connectionHandle, uint16_t mtu);
  void noteClientConnected(void* client);
  void noteClientConnectFailed(void* client, int reason);
  void noteClientDisconnected(void* client, int reason);
  void notePeripheralConnectFailed(int reason);
  void noteGapConnectResult(int status);
  void noteRemoteIdentity(uint16_t connectionHandle, uint16_t peerId);
  void enqueueServerPacket(const uint8_t* data, size_t length, uint16_t connectionHandle);
  void enqueueClientPacket(const uint8_t* data, size_t length, void* characteristic);

 private:
  friend class BleNimBLEScanCallbacks;
  friend class BleNimBLEServerCallbacks;
  friend class BleNimBLERxCallbacks;
  friend class BleNimBLEIdentityCallbacks;
  friend class BleNimBLEClientCallbacks;

  struct PacketSlot {
    uint16_t peerId = 0;
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
    uint16_t messageId = 0;
    uint8_t status = 0;
    uint8_t retryCount = 0;
    uint32_t lastActionMs = 0;
  };
  struct PeerFailure {
    bool used = false;
    uint16_t peerId = 0;
    uint8_t reason = 0;
    uint8_t stage = 0;
    int16_t detail = 0;
  };
  struct DiagnosticRecord {
    uint32_t uptimeMs = 0;
    uint8_t code = 0;
    uint8_t reason = 0;
    uint8_t stage = 0;
    uint16_t peerId = 0;
    int16_t detail = 0;
    uint8_t activeLinks = 0;
    uint8_t stateFlags = 0;
  };
  struct Link {
    bool used = false;
    bool serverSide = false;
    bool addressReady = false;
    bool foundNotified = false;
    bool connected = false;
    bool connecting = false;
    bool connectComplete = false;
    bool connectFailedPending = false;
    int connectFailedReason = 0;
    bool disconnectPending = false;
    bool identityPending = false;
    bool handshakeTimedOut = false;
    uint16_t peerId = 0;
    uint16_t connectionHandle = 0;
    uint16_t pendingIdentity = 0;
    uint16_t mtu = 23;
    uint8_t address[6]{};
    uint8_t addressType = 0;
    int disconnectReason = 0;
    uint32_t connectStartedMs = 0;
    uint32_t nextAttemptMs = 0;
    uint32_t lastSeenMs = 0;
    void* client = nullptr;
    void* remoteRx = nullptr;
    void* remoteTx = nullptr;
    Message tx{};
    bool txWaitingAck = false;
    bool txWaitingResult = false;
    Reassembly rx{};
    bool rxMessageReady = false;
    uint16_t readyMessageId = 0;
    uint16_t readyLength = 0;
    uint8_t readyData[MAX_MESSAGE_LENGTH]{};
    uint16_t lastCompletedMessageId = 0;
    uint8_t lastCompletedFragmentCount = 0;
    bool lastCompletedValid = false;
    PendingResult result{};
  };

  bool enqueueEvent(uint8_t code, const uint8_t* payload, uint8_t length);
  bool enqueueReceivedPacket(const uint8_t* data, size_t length, uint16_t peerId);
  bool transmitPacket(const uint8_t* packet, uint16_t length);
  bool buildAndSendSimple(uint8_t type, uint16_t messageId, uint8_t fragmentIndex,
                          uint8_t errorCode = 0);
  bool sendMessageResult(uint16_t messageId, uint8_t status);
  void handlePacket(uint16_t peerId, const uint8_t* packet, uint16_t length);
  void handleData(const uint8_t* packet, uint16_t length);
  void handleAck(const uint8_t* packet, uint16_t length);
  void handleNack(const uint8_t* packet, uint16_t length);
  void handleMessageResult(const uint8_t* packet, uint16_t length);
  void handleMessageResultAck(const uint8_t* packet, uint16_t length);
  void serviceTransmit(Link& link);
  void serviceReceiveTimeout(Link& link);
  void serviceResultRetry(Link& link);
  void failTransmit(Link& link, uint8_t errorCode, uint8_t fragmentIndex);
  uint16_t fragmentDataCapacity() const;
  bool peerConfigured(uint16_t peerId) const;
  void clearMessageState(Link& link, bool reportIndeterminate);
  Link* findLink(uint16_t peerId);
  const Link* findLink(uint16_t peerId) const;
  Link* findLinkByHandle(uint16_t connectionHandle);
  Link* findLinkByClient(void* client);
  Link* findLinkByCharacteristic(void* characteristic);
  Link* allocateLink(uint16_t peerId, bool serverSide = false);
  uint8_t activeConnectionCount() const;
  void refreshConnectedSummary();
  void emitConnectionFailure(uint16_t peerId, uint8_t reason,
                             uint8_t stage, int16_t detail);
  void recordDiagnostic(uint8_t code, uint16_t peerId = 0, uint8_t reason = 0,
                        uint8_t stage = 0, int16_t detail = 0);
  const PeerFailure* findPeerFailure(uint16_t peerId) const;
  void clearPeerFailure(uint16_t peerId);
  void maintainHubConnections();
  void processLinkEvents();
  uint16_t mtuForPeer(uint16_t peerId) const;
  void emitError(uint8_t direction, uint16_t peerId, uint16_t messageId,
                 uint8_t fragmentIndex, uint8_t errorCode);
  static uint16_t packetCrc(const uint8_t* data, size_t length);
  static uint16_t readU16(const uint8_t* p);
  static void writeU16(uint8_t* p, uint16_t value);

  Config config_{};
  uint16_t localDeviceId_ = 0;
  uint16_t nextMessageId_ = 1;
  uint16_t selectedPeerId_ = 0;
  bool initialized_ = false;
  bool enabled_ = false;
  bool advertising_ = false;
  bool scanning_ = false;
  bool connected_ = false;
  uint16_t connectedPeerId_ = 0;
  bool advertisingRestartPending_ = false;
  Link links_[MAX_ACTIVE_CONNECTIONS]{};
  PeerFailure peerFailures_[MAX_PEERS]{};
  uint8_t peerFailureReplaceIndex_ = 0;
  DiagnosticRecord diagnosticTrace_[BLE_DIAGNOSTIC_TRACE_CAPACITY]{};
  uint8_t diagnosticTraceHead_ = 0;
  uint8_t diagnosticTraceCount_ = 0;
  uint32_t lastScanStartFailureMs_ = 0;
  bool scanStartFailureSeen_ = false;

  PacketSlot rxPackets_[RX_PACKET_QUEUE_SIZE]{};
  volatile uint8_t rxHead_ = 0;
  volatile uint8_t rxTail_ = 0;
  volatile uint8_t rxCount_ = 0;
  Event events_[EVENT_QUEUE_SIZE]{};
  volatile uint8_t eventHead_ = 0;
  volatile uint8_t eventTail_ = 0;
  volatile uint8_t eventCount_ = 0;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  void* server_ = nullptr;
  void* rxCharacteristic_ = nullptr;
  void* txCharacteristic_ = nullptr;
  void* identityReadCharacteristic_ = nullptr;
  void* protocolVersionCharacteristic_ = nullptr;
  void* identityWriteCharacteristic_ = nullptr;
  void* scan_ = nullptr;
  void* scanCallbacks_ = nullptr;
  void* serverCallbacks_ = nullptr;
  void* rxCallbacks_ = nullptr;
  void* identityCallbacks_ = nullptr;
  void* clientCallbacks_ = nullptr;
};

} // namespace UwbBle
