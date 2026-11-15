/*
 * UWB Node firmware for Seeed Studio XIAO ESP32-C3 + Qorvo DWM3000.
 *
 * Protocol: binary COBS frames over USB CDC and UART1, CRC-16/CCITT-FALSE.
 * Both host transports use the same frame format and command handlers.
 * UART1 baud is configured by UART_BAUD; USB CDC is a native USB data link.
 * Radio: 4-frame DS-TWR protocol (POLL / RESP / FINAL / REPORT).
 *
 * Target driver: Makerfabs/Makerfabs-ESP32-UWB-DW3000 (Qorvo DW3000 API).
 * The driver port MUST be patched for the XIAO's custom SPI pins; see README.md.
 *
 * Sleep note: the bundled driver includes Qorvo sleep API declarations, but this
 * firmware's UWB_SLEEP command currently selects IDLE_RC standby rather than
 * configuring/entering DWM3000 deep sleep. SYSTEM_SLEEP deep-sleeps the ESP32-C3;
 * UART cannot wake the ESP32 from that state.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

#include "dw3000.h"       // Makerfabs DW3000/Qorvo API
#include "Protocol.h"
#include "BleManager.h"

using namespace UwbProto;

// The Arduino-ESP32 HWCDC and HardwareSerial defaults both use 256-byte RX
// queues. A maximum-size COBS command frame can be MAX_ENCODED bytes, so leave
// room for the full frame plus another frame's worth of USB/UART burst data.
static constexpr size_t HOST_RX_BUFFER_SIZE = MAX_ENCODED * 2;

// This firmware uses the ESP32-C3 native USB Serial/JTAG peripheral through
// Arduino-ESP32 Hardware CDC. Select: USB Mode = Hardware CDC and JTAG,
// USB CDC On Boot = Enabled. ESP32-C3 does not use the S2/S3 USB-OTG CDC API.
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  #if !defined(ARDUINO_USB_MODE) || (ARDUINO_USB_MODE != 1)
    #error "ESP32-C3 requires Arduino USB Mode = Hardware CDC and JTAG."
  #endif
  #if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT != 1)
    #error "ESP32-C3 requires USB CDC On Boot = Enabled."
  #endif
#endif

// -------------------- XIAO ESP32-C3 pin map (preserved) --------------------
static constexpr int PIN_DWM_CS   = 2;   // XIAO D0 -> DWM3000 SPICSn
static constexpr int PIN_DWM_WAKE = 3;   // XIAO D1 -> DWM3000 WAKEUP
static constexpr int PIN_DWM_RST  = 4;   // XIAO D2 -> DWM3000 RSTn (open drain)
static constexpr int PIN_DWM_IRQ  = 5;   // XIAO D3 -> DWM3000 IRQ
static constexpr int PIN_PI_TX    = 21;  // XIAO D6 -> Pi RX
static constexpr int PIN_PI_RX    = 20;  // XIAO D7 <- Pi TX
static constexpr int PIN_SPI_SCK  = 8;   // XIAO D8 -> DWM3000 SPICLK
static constexpr int PIN_SPI_MISO = 9;   // XIAO D9 <- DWM3000 SPIMISO
static constexpr int PIN_SPI_MOSI = 10;  // XIAO D10 -> DWM3000 SPIMOSI

// Preserve the currently configured hardware-UART rate for Pi/WROOM compatibility.
static constexpr uint32_t UART_BAUD = 460800;
// USB CDC is not clocked at this baud; this is only its host-side line-coding value.
static constexpr uint32_t USB_CDC_BAUD = 115200;
static constexpr uint16_t MAX_TARGETS = 16;
static constexpr uint16_t HISTORY_CAPACITY = 256;
static constexpr uint8_t JOB_QUEUE_CAPACITY = 8;
static constexpr uint16_t MAX_HISTORY_PER_REPLY = 17;
static constexpr uint16_t RADIO_FRAME_MAX = 127;
static constexpr uint16_t RADIO_FCS_BYTES = 2;
static constexpr uint16_t RADIO_COMMON_LEN = 10;
static constexpr uint8_t MSG_POLL = 0xE0;
static constexpr uint8_t MSG_RESP = 0xE1;
static constexpr uint8_t MSG_FINAL = 0xE2;
static constexpr uint8_t MSG_REPORT = 0xE3;
static constexpr uint16_t DEFAULT_PAN_ID = 0xDECA;
static constexpr uint8_t DEFAULT_CHANNEL = 5;
static constexpr uint32_t UWB_UUS_TO_DWT_TIME = 65536UL;
static constexpr uint16_t RESP_DELAY_UUS = 1200;
static constexpr uint16_t FINAL_DELAY_UUS = 1200;
static constexpr uint16_t REPORT_DELAY_UUS = 1200;
static constexpr double DWT_TICK_METERS = 299792458.0 / (499.2e6 * 128.0);
static constexpr uint16_t DEFAULT_TX_ANT_DLY = 16385;
static constexpr uint16_t DEFAULT_RX_ANT_DLY = 16385;

HardwareSerial PiSerial(1);
Preferences preferences;

// -------------------- Host command and event identifiers --------------------
enum Opcode : uint8_t {
  OP_PING = 0x01, OP_GET_INFO = 0x02, OP_GET_STATUS = 0x03,
  OP_GET_STATISTICS = 0x04, OP_CLEAR_STATISTICS = 0x05, OP_GET_LAST_ERROR = 0x06,
  OP_GET_CONFIG = 0x10, OP_SET_CONFIG = 0x11,
  OP_RESET_CONFIG = 0x13, OP_TIME_SYNC = 0x14,
  OP_GET_ANTENNA_DELAYS = 0x15, OP_SET_ANTENNA_DELAYS = 0x16,
  // Persistent configuration extensions. These keep NVS reads/writes separate
  // from the runtime controls used by the ranging test page.
  OP_GET_SAVED_CONFIG = 0x17, OP_APPLY_SAVE_CONFIG = 0x18,
  OP_SAVE_ANTENNA_DELAYS = 0x19,
  OP_SET_ROLE = 0x20, OP_SET_TARGETS = 0x21, OP_GET_TARGETS = 0x22,
  OP_SET_UWB_SCHEDULE = 0x23, OP_GET_UWB_SCHEDULE = 0x24,
  OP_START_RANGING = 0x25, OP_STOP_RANGING = 0x26,
  OP_RANGE_ONCE = 0x27, OP_CANCEL_OPERATION = 0x28,
  OP_GET_LAST_RANGE = 0x30, OP_GET_HISTORY = 0x31,
  OP_SUBSCRIBE = 0x32, OP_UNSUBSCRIBE = 0x33,
  OP_CLEAR_HISTORY = 0x34, OP_GET_DIAGNOSTICS = 0x35,
  OP_GET_RADIO_TRACE = 0x36, // Legacy alias; use GET_TRACE.
  OP_GET_RADIO_TRACE_DETAIL = 0x37, // Legacy alias; use GET_TRACE.
  OP_GET_TRACE = 0x38, // source:u8 (0 radio, 1 detailed radio TX, 2 BLE lifecycle)
  OP_UWB_SLEEP = 0x40, OP_UWB_WAKE = 0x41,
  OP_SYSTEM_SLEEP = 0x42, OP_REBOOT = 0x43,
  OP_SET_PEERS = 0x50, OP_GET_PEERS = 0x51, OP_START_BLE = 0x52,
  OP_STOP_BLE = 0x53, OP_GET_PEER_STATUS = 0x56,
  OP_SEND_TO_PEER = 0x57, OP_GET_RECEIVED_DATA = 0x58,
  OP_GET_BLE_DIAGNOSTIC_TRACE = 0x59, // Legacy alias; use GET_TRACE.
  EV_RANGE_RESULT = 0x80, EV_RANGE_FAILED = 0x81,
  EV_STATE_CHANGED = 0x82, EV_ERROR = 0x83,
  EV_HISTORY_OVERRUN = 0x84, EV_DEVICE_READY = 0x85,
  EV_BLE_STATE_CHANGED = 0x86, EV_BLE_PEER_FOUND = 0x87,
  EV_BLE_PEER_CONNECTED = 0x88, EV_BLE_PEER_DISCONNECTED = 0x89,
  EV_BLE_CONNECTION_FAILED = 0x8A, EV_BLE_DATA_RECEIVED = 0x8B,
  EV_BLE_ERROR = 0x8C, EV_BLE_DATA_SENT = 0x8D, EV_BLE_DATA_EXPIRED = 0x8E
};

enum ErrorCode : uint8_t {
  ERR_OK = 0, ERR_BAD_ARGUMENT = 1, ERR_UNKNOWN_COMMAND = 2,
  ERR_BAD_STATE = 3, ERR_BUSY = 4, ERR_NOT_SUPPORTED = 5,
  ERR_TIMEOUT = 6, ERR_RADIO_FAILURE = 7, ERR_RATE_UNACHIEVABLE = 8,
  ERR_INTERNAL = 9, ERR_NOT_FOUND = 10
};

enum Role : uint8_t { ROLE_IDLE = 0, ROLE_INITIATOR = 1, ROLE_RESPONDER = 2 };
enum RangeStatus : uint8_t {
  RANGE_VALID = 0, RANGE_TIMEOUT = 1, RANGE_INVALID = 2,
  RANGE_CANCELLED = 3, RANGE_RADIO_ERROR = 4
};

// SUBSCRIBE mask bits (bit N subscribes to event EV_RANGE_RESULT + N).
static constexpr uint16_t SUB_RANGE_RESULT = 1u << 0;
static constexpr uint16_t SUB_RANGE_FAILED = 1u << 1;
static constexpr uint16_t SUB_STATE_CHANGED = 1u << 2;
static constexpr uint16_t SUB_ERROR = 1u << 3;
static constexpr uint16_t SUB_HISTORY_OVERRUN = 1u << 4;
static constexpr uint16_t SUB_DEVICE_READY = 1u << 5;
static constexpr uint16_t SUB_BLE_STATE_CHANGED = 1u << 6;
static constexpr uint16_t SUB_BLE_PEER_FOUND = 1u << 7;
static constexpr uint16_t SUB_BLE_PEER_CONNECTED = 1u << 8;
static constexpr uint16_t SUB_BLE_PEER_DISCONNECTED = 1u << 9;
static constexpr uint16_t SUB_BLE_CONNECTION_FAILED = 1u << 10;
static constexpr uint16_t SUB_BLE_DATA_RECEIVED = 1u << 11;
static constexpr uint16_t SUB_BLE_ERROR = 1u << 12;
static constexpr uint16_t SUB_BLE_DATA_SENT = 1u << 13;
static constexpr uint16_t SUB_BLE_DATA_EXPIRED = 1u << 14;

struct Config {
  uint16_t deviceId = 1;
  uint16_t panId = DEFAULT_PAN_ID;
  uint8_t channel = DEFAULT_CHANNEL;
  uint16_t txAntennaDelay = DEFAULT_TX_ANT_DLY;
  uint16_t rxAntennaDelay = DEFAULT_RX_ANT_DLY;
  uint32_t targetPeriodMs = 100;
  uint16_t transactionTimeoutMs = 120;
  uint8_t retries = 1;
};
Config cfg;
UwbBle::Manager ble;
UwbBle::Config bleCfg;

struct Measurement {
  uint32_t sampleSeq = 0;
  uint16_t targetId = 0;
  uint64_t monoUs = 0;
  uint64_t epochUs = 0;
  int32_t rangeMm = -1;
  uint8_t status = RANGE_INVALID;
  uint8_t qualityFlags = 0;  // 0 means no quality metric supplied by this driver.
};
Measurement history[HISTORY_CAPACITY];
uint16_t historyHead = 0;    // next write location
uint16_t historyCount = 0;
uint32_t nextSampleSeq = 1;
bool historyHasOverrun = false;

struct Counters {
  uint32_t rangesOk = 0, rangesFailed = 0, rxErrors = 0, txErrors = 0;
  uint32_t transactionTimeouts = 0, protocolErrors = 0, uartOverflows = 0;
  uint32_t historyOverwrites = 0, malformedRadioFrames = 0, duplicateRequests = 0;
} stats;

Role role = ROLE_IDLE;
bool rangingEnabled = false;
// Persistent boot policy. Runtime START/STOP and SET_ROLE commands never change this.
bool autoStartRanging = false;
bool radioReady = false;
bool radioStandby = false;  // IDLE_RC standby; NOT true DWM3000 deep sleep.
uint16_t targetIds[MAX_TARGETS];
uint32_t targetDueMs[MAX_TARGETS];
uint8_t targetCount = 0;
uint32_t schedulePeriodMs = 100;
uint16_t scheduleTimeoutMs = 120;
uint8_t scheduleRetries = 1;
uint16_t nextJobId = 1;
uint16_t eventSeq = 0;
int64_t epochOffsetUs = 0;
bool clockSynchronized = false;
static constexpr uint8_t ERROR_SUBSYSTEM_SYSTEM = 0;
static constexpr uint8_t ERROR_SUBSYSTEM_UWB = 1;
static constexpr uint8_t ERROR_SUBSYSTEM_BLE = 2;
enum TraceSource : uint8_t {
  TRACE_SOURCE_UWB_RADIO = 0,
  TRACE_SOURCE_UWB_TX_START = 1,
  TRACE_SOURCE_BLE_LIFECYCLE = 2
};
uint8_t lastErrorSubsystem = ERROR_SUBSYSTEM_SYSTEM;
uint8_t lastErrorCode = ERR_OK;
uint64_t lastErrorMonoUs = 0;
uint16_t lastErrorPeerId = 0;
uint32_t lastErrorContext = 0;
uint32_t currentRadioStatus = 0;

// -------------------- Host transport state --------------------
// Each interface has independent framing, subscriptions and duplicate replay.
// Command/radio/configuration state remains shared by the physical node.
struct ReplyCacheEntry {
  bool valid = false;
  uint16_t seq = 0;
  uint8_t opcode = 0;
  uint16_t payloadCrc = 0;
  Frame response;
};

struct HostTransport {
  Stream *io = nullptr;
  const char *name = nullptr;
  uint8_t encoded[MAX_ENCODED]{};
  size_t encodedLen = 0;
  bool droppingFrame = false;
  uint16_t subscriptionMask = 0;
  ReplyCacheEntry replyCache[8]{};
  uint8_t replyCacheNext = 0;
};

static HostTransport UsbTransport;
static HostTransport UartTransport;

// USB Serial/JTAG has no dedicated disconnect callback in the HWCDC event
// enum. Poll HWCDC::isConnected() and debounce the transition so short link
// fluctuations do not clear a live host session.
static constexpr uint32_t USB_DISCONNECT_DEBOUNCE_MS = 150;
static bool usbSessionObservedConnected = false;
static bool usbDisconnectCandidateActive = false;
static uint32_t usbDisconnectCandidateMs = 0;

// -------------------- Radio ranging state machine --------------------
enum RadioState : uint8_t {
  RS_READY = 0,
  RS_RX_LISTEN,
  RS_TX_POLL,
  RS_WAIT_RESP,
  RS_TX_FINAL,
  RS_WAIT_REPORT,
  RS_TX_RESP,
  RS_WAIT_FINAL,
  RS_TX_REPORT
};
RadioState radioState = RS_READY;
uint32_t radioStateStartedMs = 0;
uint32_t txnStartedAtMs = 0;
uint32_t lastTransactionMs = 0;
uint32_t averageTransactionMs = 0;
uint8_t radioSequence = 0;
uint8_t txnRadioSeq = 0;
uint16_t txnTargetId = 0;
uint16_t txnJobId = 0;           // 0 for continuous schedule; nonzero for RANGE_ONCE
uint8_t txnRetriesUsed = 0;
int8_t txnTargetIndex = -1;
bool txnActive = false;
uint32_t pollTxTs = 0;
uint32_t respRxTs = 0;
uint32_t respPollRxTs = 0;
uint32_t respTxTs = 0;
uint32_t finalTxTs = 0;
uint32_t respDelayReg = 0;
uint32_t finalDelayReg = 0;
uint16_t peerId = 0;
int32_t computedRangeMm = -1;
uint8_t computedRangeStatus = RANGE_INVALID;

// Volatile-in-RAM diagnostic trace. No text is printed on USB CDC/UART because
// both transports carry binary protocol frames. Read records with opcode 0x36.
static constexpr uint8_t RADIO_TRACE_CAPACITY = 8;
static constexpr uint8_t RADIO_TRACE_RECORD_BYTES = 28;
static constexpr uint8_t RADIO_TRACE_DETAIL_CAPACITY = 8;
static constexpr uint8_t RADIO_TRACE_DETAIL_RECORD_BYTES = 44;
enum RadioTraceEvent : uint8_t {
  TRACE_TX_START_FAILED = 1,
  TRACE_RX_ARM_FAILED = 2,
  TRACE_TX_COMPLETION_TIMEOUT = 3,
  TRACE_RX_ERROR = 4
};
struct RadioTraceRecord {
  uint32_t uptimeMs;
  uint32_t statusBefore;
  uint32_t statusAfter;
  uint32_t delayedTimeReg;
  uint16_t peer;
  uint16_t jobId;
  uint8_t event;
  uint8_t state;
  uint8_t function;
  uint8_t radioSeq;
  uint8_t delayed;
  uint8_t retriesUsed;
  uint8_t rc;
  uint8_t reserved;
};
static RadioTraceRecord radioTrace[RADIO_TRACE_CAPACITY]{};
static uint8_t radioTraceHead = 0;  // next write slot
static uint8_t radioTraceCount = 0;

// Application-only TX diagnostics. These are snapshots immediately before and
// after dwt_starttx(); the Makerfabs driver itself remains unmodified.
enum RadioTraceDetailEvent : uint8_t { TRACE_DETAIL_TX_START_FAILED = 1 };
enum RadioTraceFailureHint : uint8_t {
  TRACE_HINT_UNKNOWN = 0,
  TRACE_HINT_HPDWARN_OBSERVED_AFTER = 1,
  TRACE_HINT_TXERR_STATE_OBSERVED_AFTER = 2
};
struct RadioTraceDetailRecord {
  uint32_t uptimeMs;
  uint32_t statusBefore;
  uint32_t statusAfter;
  uint32_t sysStateBefore;
  uint32_t sysStateAfter;
  uint32_t sysTimeBefore;
  uint32_t delayedTimeReg;
  uint32_t scheduledTxTimestampLow;
  uint16_t peer;
  uint16_t jobId;
  uint8_t event;
  uint8_t state;
  uint8_t function;
  uint8_t radioSeq;
  uint8_t delayed;
  uint8_t retriesUsed;
  uint8_t rc;
  uint8_t failureHint;
};
static RadioTraceDetailRecord radioTraceDetail[RADIO_TRACE_DETAIL_CAPACITY]{};
static uint8_t radioTraceDetailHead = 0;
static uint8_t radioTraceDetailCount = 0;

static void recordRadioTraceDetail(uint8_t function, bool delayed,
                                   uint32_t delayedTimeReg, int rc,
                                   uint32_t statusBefore, uint32_t statusAfter,
                                   uint32_t sysStateBefore, uint32_t sysStateAfter,
                                   uint32_t sysTimeBefore) {
  RadioTraceDetailRecord &r = radioTraceDetail[radioTraceDetailHead];
  r.uptimeMs = millis();
  r.statusBefore = statusBefore;
  r.statusAfter = statusAfter;
  r.sysStateBefore = sysStateBefore;
  r.sysStateAfter = sysStateAfter;
  r.sysTimeBefore = sysTimeBefore;
  r.delayedTimeReg = delayedTimeReg;
  r.scheduledTxTimestampLow = delayed ? (((delayedTimeReg & 0xFFFFFFFEUL) << 8) +
                                         cfg.txAntennaDelay) : 0;
  r.peer = peerId;
  r.jobId = txnJobId;
  r.event = TRACE_DETAIL_TX_START_FAILED;
  r.state = static_cast<uint8_t>(radioState);
  r.function = function;
  r.radioSeq = txnRadioSeq;
  r.delayed = delayed ? 1 : 0;
  r.retriesUsed = txnRetriesUsed;
  r.rc = static_cast<uint8_t>(rc);
  r.failureHint = (statusAfter & SYS_STATUS_HPDWARN_BIT_MASK)
                    ? TRACE_HINT_HPDWARN_OBSERVED_AFTER
                    : ((sysStateAfter == DW_SYS_STATE_TXERR)
                        ? TRACE_HINT_TXERR_STATE_OBSERVED_AFTER
                        : TRACE_HINT_UNKNOWN);
  radioTraceDetailHead = (radioTraceDetailHead + 1) % RADIO_TRACE_DETAIL_CAPACITY;
  if (radioTraceDetailCount < RADIO_TRACE_DETAIL_CAPACITY) ++radioTraceDetailCount;
}

static void recordRadioTrace(uint8_t event, uint8_t function, bool delayed,
                             uint32_t delayedTimeReg, int rc,
                             uint32_t statusBefore, uint32_t statusAfter) {
  RadioTraceRecord &r = radioTrace[radioTraceHead];
  r.uptimeMs = millis();
  r.statusBefore = statusBefore;
  r.statusAfter = statusAfter;
  r.delayedTimeReg = delayedTimeReg;
  r.peer = peerId;
  r.jobId = txnJobId;
  r.event = event;
  r.state = static_cast<uint8_t>(radioState);
  r.function = function;
  r.radioSeq = txnRadioSeq;
  r.delayed = delayed ? 1 : 0;
  r.retriesUsed = txnRetriesUsed;
  r.rc = static_cast<uint8_t>(rc);
  r.reserved = 0;
  radioTraceHead = (radioTraceHead + 1) % RADIO_TRACE_CAPACITY;
  if (radioTraceCount < RADIO_TRACE_CAPACITY) ++radioTraceCount;
}


struct Job { uint16_t id; uint16_t targetId; };
Job jobQueue[JOB_QUEUE_CAPACITY];
uint8_t jobHead = 0;
uint8_t jobCount = 0;
bool activeJobIsOneShot = false;

// -------------------- Small utility helpers --------------------
static uint64_t monoMicros() { return static_cast<uint64_t>(esp_timer_get_time()); }
static uint64_t epochMicros() {
  if (!clockSynchronized) return 0;
  const int64_t v = static_cast<int64_t>(monoMicros()) + epochOffsetUs;
  return v > 0 ? static_cast<uint64_t>(v) : 0;
}
static bool elapsed(uint32_t now, uint32_t since, uint32_t ms) { return uint32_t(now - since) >= ms; }
static bool validTargetId(uint16_t id) { return id != 0 && id != 0xFFFF && id != cfg.deviceId; }
static const char *roleText(Role r) { return r == ROLE_INITIATOR ? "initiator" : (r == ROLE_RESPONDER ? "responder" : "idle"); }
static bool isTxState(RadioState s) { return s == RS_TX_POLL || s == RS_TX_FINAL || s == RS_TX_RESP || s == RS_TX_REPORT; }

static uint16_t eventMaskForOpcode(uint8_t op) {
  switch (op) {
    case EV_RANGE_RESULT: return SUB_RANGE_RESULT;
    case EV_RANGE_FAILED: return SUB_RANGE_FAILED;
    case EV_STATE_CHANGED: return SUB_STATE_CHANGED;
    case EV_ERROR: return SUB_ERROR;
    case EV_HISTORY_OVERRUN: return SUB_HISTORY_OVERRUN;
    case EV_DEVICE_READY: return SUB_DEVICE_READY;
    case EV_BLE_STATE_CHANGED: return SUB_BLE_STATE_CHANGED;
    case EV_BLE_PEER_FOUND: return SUB_BLE_PEER_FOUND;
    case EV_BLE_PEER_CONNECTED: return SUB_BLE_PEER_CONNECTED;
    case EV_BLE_PEER_DISCONNECTED: return SUB_BLE_PEER_DISCONNECTED;
    case EV_BLE_CONNECTION_FAILED: return SUB_BLE_CONNECTION_FAILED;
    case EV_BLE_DATA_RECEIVED: return SUB_BLE_DATA_RECEIVED;
    case EV_BLE_ERROR: return SUB_BLE_ERROR;
    case EV_BLE_DATA_SENT: return SUB_BLE_DATA_SENT;
    case EV_BLE_DATA_EXPIRED: return SUB_BLE_DATA_EXPIRED;
    default: return 0;
  }
}

static bool usbCdcConnected() {
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  // On ESP32-C3, Serial maps to HWCDCSerial when Hardware CDC/JTAG and
  // CDC-on-boot are enabled. isConnected() reflects an active CDC host session.
  return Serial.isConnected();
#else
  // This firmware targets ESP32-C3. Keep other Arduino targets buildable, but
  // do not claim automatic USB-session detection on their non-HWCDC Serial.
  return true;
#endif
}

static void clearUsbSessionState() {
  UsbTransport.subscriptionMask = 0;
  UsbTransport.encodedLen = 0;
  UsbTransport.droppingFrame = false;
  for (auto &entry : UsbTransport.replyCache) entry.valid = false;
  UsbTransport.replyCacheNext = 0;
}

static void pollUsbConnection() {
  const bool connected = usbCdcConnected();
  if (connected) {
    usbSessionObservedConnected = true;
    usbDisconnectCandidateActive = false;
    return;
  }

  // Do not treat the never-connected-at-boot state as a disconnect event.
  if (!usbSessionObservedConnected) return;

  if (!usbDisconnectCandidateActive) {
    usbDisconnectCandidateActive = true;
    usbDisconnectCandidateMs = millis();
    return;
  }

  if (elapsed(millis(), usbDisconnectCandidateMs, USB_DISCONNECT_DEBOUNCE_MS)) {
    usbSessionObservedConnected = false;
    usbDisconnectCandidateActive = false;
    clearUsbSessionState();
  }
}

static void sendFrameTo(HostTransport &transport, const Frame &f) {
  if (!transport.io) return;
  uint8_t packet[MAX_ENCODED];
  const size_t n = encodeFrame(f, packet, sizeof(packet));
  if (n) transport.io->write(packet, n);
}

// Do not send unsolicited USB events into a disconnected CDC session. The
// measurement itself is stored independently in history and remains available
// to GET_LAST_RANGE / GET_HISTORY after the host reconnects.
static void sendEventFrameTo(HostTransport &transport, const Frame &f) {
  if (&transport == &UsbTransport && !usbCdcConnected()) return;
  sendFrameTo(transport, f);
}

static void rememberError(uint8_t code, uint8_t subsystem = ERROR_SUBSYSTEM_SYSTEM,
                          uint16_t peerId = 0, uint32_t context = 0) {
  lastErrorSubsystem = subsystem;
  lastErrorCode = code;
  lastErrorMonoUs = monoMicros();
  lastErrorPeerId = peerId;
  lastErrorContext = context;
  if (code == ERR_OK) return;

  const bool usbSubscribed = (UsbTransport.subscriptionMask & SUB_ERROR) != 0;
  const bool uartSubscribed = (UartTransport.subscriptionMask & SUB_ERROR) != 0;
  if (!usbSubscribed && !uartSubscribed) return;

  Frame e;
  e.type = TYPE_EVENT; e.seq = ++eventSeq; e.opcode = EV_ERROR; e.flags = 0;
  e.payload[0] = code; putU64(e.payload + 1, lastErrorMonoUs); e.length = 9;
  if (usbSubscribed) sendEventFrameTo(UsbTransport, e);
  if (uartSubscribed) sendFrameTo(UartTransport, e);
}

static void sendResponse(HostTransport &source, const Frame &request, uint8_t error,
                         const uint8_t *payload = nullptr, uint16_t length = 0,
                         bool cache = true) {
  Frame r;
  r.type = TYPE_RESPONSE; r.seq = request.seq; r.opcode = request.opcode; r.flags = error;
  if (length > MAX_PAYLOAD) { r.flags = ERR_INTERNAL; length = 0; }
  r.length = length;
  if (length && payload) memcpy(r.payload, payload, length);
  sendFrameTo(source, r);
  if (cache && request.type == TYPE_REQUEST) {
    ReplyCacheEntry &e = source.replyCache[source.replyCacheNext++ % 8];
    e.valid = true; e.seq = request.seq; e.opcode = request.opcode;
    e.payloadCrc = crc16(request.payload, request.length); e.response = r;
  }
  if (error != ERR_OK) {
    uint8_t subsystem = error == ERR_RADIO_FAILURE ? ERROR_SUBSYSTEM_UWB : ERROR_SUBSYSTEM_SYSTEM;
    uint16_t peerId = 0;
    if (request.opcode >= OP_SET_PEERS && request.opcode <= OP_GET_BLE_DIAGNOSTIC_TRACE) {
      subsystem = ERROR_SUBSYSTEM_BLE;
      if ((request.opcode == OP_GET_PEER_STATUS || request.opcode == OP_SEND_TO_PEER ||
           request.opcode == OP_GET_RECEIVED_DATA) && request.length >= 2) {
        peerId = getU16(request.payload);
      }
    }
    rememberError(error, subsystem, peerId, request.opcode);
  }
}

static void emitEvent(uint8_t opcode, const uint8_t *payload, uint16_t length, bool force = false) {
  const uint16_t mask = eventMaskForOpcode(opcode);
  const bool usbSubscribed = mask != 0 && (UsbTransport.subscriptionMask & mask) != 0;
  const bool uartSubscribed = mask != 0 && (UartTransport.subscriptionMask & mask) != 0;
  if (!force && !usbSubscribed && !uartSubscribed) return;
  Frame e;
  e.type = TYPE_EVENT; e.seq = ++eventSeq; e.opcode = opcode; e.flags = 0;
  if (length > MAX_PAYLOAD) return;
  e.length = length;
  if (length && payload) memcpy(e.payload, payload, length);

  // Events are generated once, then copied to whichever hosts subscribed.
  if (force || usbSubscribed) sendEventFrameTo(UsbTransport, e);
  if (force || uartSubscribed) sendFrameTo(UartTransport, e);
}

static void emitStateChanged() {
  uint8_t p[4]; p[0] = static_cast<uint8_t>(role);
  p[1] = rangingEnabled ? 1 : 0; p[2] = radioStandby ? 1 : 0; p[3] = radioReady ? 1 : 0;
  emitEvent(EV_STATE_CHANGED, p, sizeof(p));
}

static void encodeMeasurement(uint8_t *p, const Measurement &m) {
  putU32(p, m.sampleSeq); putU16(p + 4, m.targetId); putU64(p + 6, m.monoUs);
  putU64(p + 14, m.epochUs); putU32(p + 22, static_cast<uint32_t>(m.rangeMm));
  p[26] = m.status; p[27] = m.qualityFlags;
}

static void insertMeasurement(uint16_t target, int32_t mm, uint8_t status, uint8_t quality, uint16_t jobId) {
  Measurement m;
  m.sampleSeq = nextSampleSeq++;
  if (nextSampleSeq == 0) nextSampleSeq = 1;
  m.targetId = target; m.monoUs = monoMicros(); m.epochUs = epochMicros();
  m.rangeMm = mm; m.status = status; m.qualityFlags = quality;
  if (historyCount == HISTORY_CAPACITY) {
    stats.historyOverwrites++;
    historyHasOverrun = true;
    uint8_t p[8]; putU32(p, history[historyHead].sampleSeq); putU32(p + 4, m.sampleSeq);
    emitEvent(EV_HISTORY_OVERRUN, p, sizeof(p));
  } else historyCount++;
  history[historyHead] = m;
  historyHead = (historyHead + 1) % HISTORY_CAPACITY;
  if (status == RANGE_VALID) stats.rangesOk++; else stats.rangesFailed++;

  if (status == RANGE_VALID) {
    uint8_t p[2 + 28]; putU16(p, jobId); encodeMeasurement(p + 2, m);
    emitEvent(EV_RANGE_RESULT, p, sizeof(p));
  } else {
    uint8_t p[2 + 2 + 1 + 8]; putU16(p, jobId); putU16(p + 2, target); p[4] = status; putU64(p + 5, m.monoUs);
    emitEvent(EV_RANGE_FAILED, p, sizeof(p));
  }
}

static void cancelQueuedJobs() {
  for (uint8_t i = 0; i < jobCount; ++i) {
    const Job &j = jobQueue[(jobHead + i) % JOB_QUEUE_CAPACITY];
    insertMeasurement(j.targetId, -1, RANGE_CANCELLED, 0, j.id);
  }
  jobHead = 0;
  jobCount = 0;
}

// BLE configuration extension appended to GET_CONFIG / SET_CONFIG and the
// explicit saved-configuration snapshot. All multibyte values are little-endian.
static constexpr uint16_t BLE_CONFIG_FIXED_LENGTH = 16;

static uint16_t encodeBleConfig(uint8_t *dst, const UwbBle::Config &c) {
  dst[0] = c.mode;
  putU16(dst + 1, c.advertisingIntervalMs);
  putU16(dst + 3, c.scanIntervalMs);
  putU16(dst + 5, c.scanWindowMs);
  dst[7] = c.autoStart; // Automatically start BLE after boot; independent of runtime state.
  putU16(dst + 8, c.reconnectIntervalMs);
  putU16(dst + 10, c.connectionTimeoutMs);
  dst[12] = c.maxRetries;
  putU16(dst + 13, c.rxMessageTimeoutMs);
  dst[15] = c.peerCount;
  for (uint8_t i = 0; i < c.peerCount; ++i) putU16(dst + 16 + i * 2, c.peers[i]);
  return BLE_CONFIG_FIXED_LENGTH + uint16_t(c.peerCount) * 2;
}

static bool decodeBleConfig(const uint8_t *src, uint16_t length, UwbBle::Config &c) {
  if (!src || length < BLE_CONFIG_FIXED_LENGTH) return false;
  UwbBle::Config parsed;
  parsed.mode = src[0];
  parsed.advertisingIntervalMs = getU16(src + 1);
  parsed.scanIntervalMs = getU16(src + 3);
  parsed.scanWindowMs = getU16(src + 5);
  parsed.autoStart = src[7]; // Persisted boot preference; independent of runtime state.
  parsed.reconnectIntervalMs = getU16(src + 8);
  parsed.connectionTimeoutMs = getU16(src + 10);
  parsed.maxRetries = src[12];
  parsed.rxMessageTimeoutMs = getU16(src + 13);
  parsed.peerCount = src[15];
  if (parsed.peerCount > UwbBle::MAX_PEERS ||
      length != BLE_CONFIG_FIXED_LENGTH + uint16_t(parsed.peerCount) * 2) return false;
  for (uint8_t i = 0; i < parsed.peerCount; ++i) parsed.peers[i] = getU16(src + 16 + i * 2);
  if (parsed.mode > UwbBle::MODE_PERIPHERAL || parsed.autoStart > 1 ||
      parsed.maxRetries > 5 || parsed.advertisingIntervalMs < 20 ||
      parsed.advertisingIntervalMs > 10240 || parsed.scanIntervalMs < 10 ||
      parsed.scanIntervalMs > 10240 || parsed.scanWindowMs < 10 ||
      parsed.scanWindowMs > parsed.scanIntervalMs || parsed.connectionTimeoutMs < 100 ||
      parsed.connectionTimeoutMs > 30000 || parsed.reconnectIntervalMs < 100 ||
      parsed.reconnectIntervalMs > 60000 || parsed.rxMessageTimeoutMs < 100 ||
      parsed.rxMessageTimeoutMs > 2000) return false;
  for (uint8_t i = 0; i < parsed.peerCount; ++i) {
    if (parsed.peers[i] == 0 || parsed.peers[i] == 0xFFFF) return false;
    for (uint8_t j = 0; j < i; ++j) if (parsed.peers[j] == parsed.peers[i]) return false;
  }
  c = parsed;
  return true;
}

static bool persistBlePeerList(const UwbBle::Config &c) {
  if (!preferences.begin("uwb-node", false)) return false;
  bool ok = preferences.putUChar("ble_pcnt", c.peerCount) == sizeof(uint8_t);
  if (c.peerCount) ok &= preferences.putBytes("ble_peers", c.peers,
                                                c.peerCount * sizeof(uint16_t)) ==
                         c.peerCount * sizeof(uint16_t);
  else preferences.remove("ble_peers");
  preferences.end();
  return ok;
}

struct StoredSettings {
  Config config;
  Role role = ROLE_IDLE;
  uint16_t targets[MAX_TARGETS]{};
  uint8_t targetCount = 0;
  uint32_t periodMs = 100;
  uint16_t timeoutMs = 120;
  uint8_t retries = 1;
  bool autoStart = false;
  UwbBle::Config bleConfig;
};

static bool readStoredSettings(StoredSettings &stored);

static bool readStoredSettings(StoredSettings &stored) {
  stored = StoredSettings();
  stored.periodMs = stored.config.targetPeriodMs;
  stored.timeoutMs = stored.config.transactionTimeoutMs;
  stored.retries = stored.config.retries;
  if (!preferences.begin("uwb-node", true)) return false;
  stored.config.deviceId = preferences.getUShort("dev", stored.config.deviceId);
  stored.config.panId = preferences.getUShort("pan", stored.config.panId);
  stored.config.channel = preferences.getUChar("chan", stored.config.channel);
  stored.config.txAntennaDelay = preferences.getUShort("txdel", stored.config.txAntennaDelay);
  stored.config.rxAntennaDelay = preferences.getUShort("rxdel", stored.config.rxAntennaDelay);
  stored.periodMs = preferences.getUInt("period", stored.periodMs);
  stored.timeoutMs = preferences.getUShort("timeout", stored.timeoutMs);
  stored.retries = preferences.getUChar("retry", stored.retries);
  const uint8_t savedRole = preferences.getUChar("role", ROLE_IDLE);
  stored.role = savedRole <= ROLE_RESPONDER ? static_cast<Role>(savedRole) : ROLE_IDLE;
  const uint8_t count = preferences.getUChar("ntarget", 0);
  if (count <= MAX_TARGETS) {
    uint16_t saved[MAX_TARGETS]{};
    const size_t bytes = count ? preferences.getBytes("targets", saved, sizeof(saved)) : 0;
    if (count == 0 || bytes >= count * sizeof(uint16_t)) {
      stored.targetCount = count;
      if (count) memcpy(stored.targets, saved, count * sizeof(uint16_t));
    }
  }
  stored.autoStart = preferences.getBool("autostart", false);
  stored.bleConfig.mode = preferences.getUChar("ble_mode", stored.bleConfig.mode);
  stored.bleConfig.advertisingIntervalMs = preferences.getUShort("ble_adv", stored.bleConfig.advertisingIntervalMs);
  stored.bleConfig.scanIntervalMs = preferences.getUShort("ble_scan_i", stored.bleConfig.scanIntervalMs);
  stored.bleConfig.scanWindowMs = preferences.getUShort("ble_scan_w", stored.bleConfig.scanWindowMs);
  stored.bleConfig.autoStart = preferences.getBool("ble_auto", preferences.getBool("ble_on", false)) ? 1 : 0; // Migrate the previous persisted-enabled behavior.
  stored.bleConfig.reconnectIntervalMs = preferences.getUShort("ble_rec_ms", stored.bleConfig.reconnectIntervalMs);
  stored.bleConfig.connectionTimeoutMs = preferences.getUShort("ble_con_ms", stored.bleConfig.connectionTimeoutMs);
  stored.bleConfig.maxRetries = preferences.getUChar("ble_retry", stored.bleConfig.maxRetries);
  stored.bleConfig.rxMessageTimeoutMs = preferences.getUShort("ble_rx_ms", stored.bleConfig.rxMessageTimeoutMs);
  const uint8_t blePeerCount = preferences.getUChar("ble_pcnt", 0);
  if (blePeerCount <= UwbBle::MAX_PEERS) {
    uint16_t savedBlePeers[UwbBle::MAX_PEERS]{};
    const size_t bleBytes = blePeerCount ? preferences.getBytes("ble_peers", savedBlePeers, sizeof(savedBlePeers)) : 0;
    if (blePeerCount == 0 || bleBytes >= blePeerCount * sizeof(uint16_t)) {
      stored.bleConfig.peerCount = blePeerCount;
      if (blePeerCount) memcpy(stored.bleConfig.peers, savedBlePeers, blePeerCount * sizeof(uint16_t));
    }
  }
  preferences.end();
  stored.config.targetPeriodMs = stored.periodMs;
  stored.config.transactionTimeoutMs = stored.timeoutMs;
  stored.config.retries = stored.retries;
  return true;
}

static bool writeStoredSettings(const Config &savedCfg, Role savedRole,
                                const uint16_t *savedTargets, uint8_t savedCount,
                                uint32_t periodMs, uint16_t timeoutMs,
                                uint8_t retries, bool autoStart,
                                const UwbBle::Config &savedBle) {
  if (!preferences.begin("uwb-node", false)) return false;
  bool ok = true;
  ok &= preferences.putUShort("dev", savedCfg.deviceId) == sizeof(uint16_t);
  ok &= preferences.putUShort("pan", savedCfg.panId) == sizeof(uint16_t);
  ok &= preferences.putUChar("chan", savedCfg.channel) == sizeof(uint8_t);
  ok &= preferences.putUShort("txdel", savedCfg.txAntennaDelay) == sizeof(uint16_t);
  ok &= preferences.putUShort("rxdel", savedCfg.rxAntennaDelay) == sizeof(uint16_t);
  ok &= preferences.putUInt("period", periodMs) == sizeof(uint32_t);
  ok &= preferences.putUShort("timeout", timeoutMs) == sizeof(uint16_t);
  ok &= preferences.putUChar("retry", retries) == sizeof(uint8_t);
  ok &= preferences.putUChar("role", static_cast<uint8_t>(savedRole)) == sizeof(uint8_t);
  ok &= preferences.putUChar("ntarget", savedCount) == sizeof(uint8_t);
  if (savedCount) {
    const size_t bytes = preferences.putBytes("targets", savedTargets,
                                                savedCount * sizeof(uint16_t));
    ok &= bytes == savedCount * sizeof(uint16_t);
  }
  ok &= preferences.putBool("autostart", autoStart) == sizeof(uint8_t);
  ok &= preferences.putUChar("ble_mode", savedBle.mode) == sizeof(uint8_t);
  ok &= preferences.putUShort("ble_adv", savedBle.advertisingIntervalMs) == sizeof(uint16_t);
  ok &= preferences.putUShort("ble_scan_i", savedBle.scanIntervalMs) == sizeof(uint16_t);
  ok &= preferences.putUShort("ble_scan_w", savedBle.scanWindowMs) == sizeof(uint16_t);
  ok &= preferences.putBool("ble_auto", savedBle.autoStart != 0) == sizeof(uint8_t);
  preferences.remove("ble_on"); // Remove the legacy runtime/auto-start key after migration.
  ok &= preferences.putUShort("ble_rec_ms", savedBle.reconnectIntervalMs) == sizeof(uint16_t);
  ok &= preferences.putUShort("ble_con_ms", savedBle.connectionTimeoutMs) == sizeof(uint16_t);
  ok &= preferences.putUChar("ble_retry", savedBle.maxRetries) == sizeof(uint8_t);
  ok &= preferences.putUShort("ble_rx_ms", savedBle.rxMessageTimeoutMs) == sizeof(uint16_t);
  ok &= preferences.putUChar("ble_pcnt", savedBle.peerCount) == sizeof(uint8_t);
  if (savedBle.peerCount) ok &= preferences.putBytes("ble_peers", savedBle.peers,
      savedBle.peerCount * sizeof(uint16_t)) == savedBle.peerCount * sizeof(uint16_t);
  else preferences.remove("ble_peers");
  preferences.end();
  return ok;
}

static void loadConfig() {
  StoredSettings stored;
  cfg = Config();
  schedulePeriodMs = cfg.targetPeriodMs;
  scheduleTimeoutMs = cfg.transactionTimeoutMs;
  scheduleRetries = cfg.retries;
  role = ROLE_IDLE;
  targetCount = 0;
  autoStartRanging = false;
  if (!readStoredSettings(stored)) return;
  cfg = stored.config;
  role = stored.role;
  targetCount = stored.targetCount;
  if (targetCount) memcpy(targetIds, stored.targets, targetCount * sizeof(uint16_t));
  schedulePeriodMs = stored.periodMs;
  scheduleTimeoutMs = stored.timeoutMs;
  scheduleRetries = stored.retries;
  autoStartRanging = stored.autoStart;
  bleCfg = stored.bleConfig;
}

// -------------------- DWM3000 initialization/low-level radio helpers --------------------
static dwt_config_t radioConfig = {
  DEFAULT_CHANNEL, DWT_PLEN_128, DWT_PAC8, 9, 9, 1,
  DWT_BR_6M8, DWT_PHRMODE_STD, DWT_PHRRATE_STD,
  (129 + 8 - 8), DWT_STS_MODE_OFF, DWT_STS_LEN_64, DWT_PDOA_M0
};
extern dwt_txconfig_t txconfig_options;

static bool applyRadioConfiguration() {
  radioConfig.chan = cfg.channel;
  // Preamble code set must match the selected UWB channel.
  radioConfig.txCode = (cfg.channel == 9) ? 17 : 9;
  radioConfig.rxCode = (cfg.channel == 9) ? 17 : 9;
  if (dwt_configure(&radioConfig) == DWT_ERROR) return false;
  dwt_configuretxrf(&txconfig_options);
  dwt_settxantennadelay(cfg.txAntennaDelay);
  dwt_setrxantennadelay(cfg.rxAntennaDelay);
  return true;
}

static bool radioInitialize() {
  // Requires the library port patch documented in README.md so spiBegin() uses
  // SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_DWM_CS).
  spiBegin(PIN_DWM_IRQ, PIN_DWM_RST);
  spiSelect(PIN_DWM_CS);
  delay(3);

  uint32_t start = millis();
  while (!dwt_checkidlerc() && !elapsed(millis(), start, 1200)) delay(1);
  if (!dwt_checkidlerc()) { radioReady = false; return false; }
  if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) { radioReady = false; return false; }
  if (!applyRadioConfiguration()) { radioReady = false; return false; }

  radioReady = true;
  radioStandby = false;
  radioState = RS_READY;
  radioStateStartedMs = millis();
  currentRadioStatus = 0;
  return true;
}

static void radioOff() {
  if (radioReady && !radioStandby) dwt_forcetrxoff();
  radioState = RS_READY;
  radioStateStartedMs = millis();
  txnActive = false;
}

static bool radioSetStandby() {
  if (!radioReady || radioStandby) return radioReady;
  dwt_forcetrxoff();
  dwt_setdwstate(DWT_DW_IDLE_RC);  // Low-power oscillator state, NOT deep sleep.
  radioStandby = true;
  radioState = RS_READY;
  txnActive = false;
  return true;
}

static bool radioWake() {
  // UWB_SLEEP currently uses IDLE_RC, so wake by an SPI register access/state
  // change rather than the dedicated WAKEUP pin. If SPI no longer responds,
  // fall back to hardware reset and full initialization. This is not a claim
  // that a DWM3000 in true deep sleep can be awakened by this path.
  if (!radioReady) return radioInitialize();
  dwt_setdwstate(DWT_DW_IDLE);
  delay(2);
  const uint32_t status = dwt_read32bitreg(SYS_STATUS_ID);
  if (status != 0xFFFFFFFFUL && applyRadioConfiguration()) {
    radioStandby = false;
    radioState = RS_READY;
    radioStateStartedMs = millis();
    currentRadioStatus = status;
    return true;
  }
  digitalWrite(PIN_DWM_RST, LOW); delayMicroseconds(10); digitalWrite(PIN_DWM_RST, HIGH);
  delay(3);
  radioReady = false; radioStandby = false;
  return radioInitialize();
}

static uint64_t read40Timestamp(bool tx) {
  uint8_t b[5] = {0};
  if (tx) dwt_readtxtimestamp(b); else dwt_readrxtimestamp(b);
  return uint64_t(b[0]) | (uint64_t(b[1]) << 8) | (uint64_t(b[2]) << 16) |
         (uint64_t(b[3]) << 24) | (uint64_t(b[4]) << 32);
}

static bool armRx(RadioState nextState) {
  if (!radioReady || radioStandby) return false;
  const uint32_t statusBefore = dwt_read32bitreg(SYS_STATUS_ID);
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXPRD_BIT_MASK | SYS_STATUS_RXSFDD_BIT_MASK | SYS_STATUS_RXFR_BIT_MASK | SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO);
  const int rc = dwt_rxenable(DWT_START_RX_IMMEDIATE);
  if (rc == DWT_ERROR) {
    const uint32_t statusAfter = dwt_read32bitreg(SYS_STATUS_ID);
    recordRadioTrace(TRACE_RX_ARM_FAILED, 0, false, 0, rc, statusBefore, statusAfter);
    return false;
  }
  radioState = nextState;
  radioStateStartedMs = millis();
  return true;
}

static uint8_t buildRadioFrame(uint8_t *out, uint16_t dest, uint8_t radioSeqNum,
                               uint8_t function, const uint8_t *payload, uint8_t payloadLen) {
  out[0] = 0x41; out[1] = 0x88; out[2] = radioSeqNum;
  out[3] = static_cast<uint8_t>(cfg.panId); out[4] = static_cast<uint8_t>(cfg.panId >> 8);
  out[5] = static_cast<uint8_t>(dest); out[6] = static_cast<uint8_t>(dest >> 8);
  out[7] = static_cast<uint8_t>(cfg.deviceId); out[8] = static_cast<uint8_t>(cfg.deviceId >> 8);
  out[9] = function;
  if (payloadLen) memcpy(out + RADIO_COMMON_LEN, payload, payloadLen);
  return RADIO_COMMON_LEN + payloadLen;
}

static bool radioSend(uint16_t dest, uint8_t function, const uint8_t *payload, uint8_t payloadLen,
                      bool delayed, uint32_t delayedTimeReg, RadioState txState) {
  if (!radioReady || radioStandby) return false;
  uint8_t tx[RADIO_FRAME_MAX];
  const uint8_t len = buildRadioFrame(tx, dest, txnRadioSeq, function, payload, payloadLen);
  const uint32_t statusBefore = dwt_read32bitreg(SYS_STATUS_ID);
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
  dwt_writetxdata(len, tx, 0);
  dwt_writetxfctrl(len + RADIO_FCS_BYTES, 0, 1);
  if (delayed) dwt_setdelayedtrxtime(delayedTimeReg);
  // Snapshot available registers in application code; no driver modifications.
  const uint32_t sysStateBefore = dwt_read32bitreg(SYS_STATE_LO_ID);
  const uint32_t sysTimeBefore = dwt_read32bitreg(SYS_TIME_ID);
  const int rc = dwt_starttx(delayed ? DWT_START_TX_DELAYED : DWT_START_TX_IMMEDIATE);
  if (rc == DWT_ERROR) {
    const uint32_t statusAfter = dwt_read32bitreg(SYS_STATUS_ID);
    const uint32_t sysStateAfter = dwt_read32bitreg(SYS_STATE_LO_ID);
    ++stats.txErrors;
    recordRadioTrace(TRACE_TX_START_FAILED, function, delayed, delayedTimeReg,
                     rc, statusBefore, statusAfter);
    recordRadioTraceDetail(function, delayed, delayedTimeReg, rc,
                           statusBefore, statusAfter, sysStateBefore,
                           sysStateAfter, sysTimeBefore);
    currentRadioStatus = statusAfter;
    return false;
  }
  radioState = txState;
  radioStateStartedMs = millis();
  return true;
}

static bool validAddressedFrame(const uint8_t *b, uint16_t dataLen) {
  if (dataLen < RADIO_COMMON_LEN) return false;
  if (b[0] != 0x41 || b[1] != 0x88) return false;
  if (b[3] != static_cast<uint8_t>(cfg.panId) || b[4] != static_cast<uint8_t>(cfg.panId >> 8)) return false;
  const uint16_t dst = getU16(b + 5);
  const uint16_t src = getU16(b + 7);
  return dst == cfg.deviceId && src != 0 && src != 0xFFFF && src != cfg.deviceId;
}

static bool isAllowedPeer(uint16_t id) {
  if (targetCount == 0) return true;
  for (uint8_t i = 0; i < targetCount; ++i) if (targetIds[i] == id) return true;
  return false;
}

static void updateDueForTarget(uint16_t target) {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < targetCount; ++i) if (targetIds[i] == target) targetDueMs[i] = now + schedulePeriodMs;
  if (txnTargetIndex >= 0 && txnTargetIndex < targetCount) targetDueMs[txnTargetIndex] = now + schedulePeriodMs;
}

static void finishActiveTransaction() {
  if (txnActive) {
    lastTransactionMs = uint32_t(millis() - txnStartedAtMs);
    if (averageTransactionMs == 0) averageTransactionMs = lastTransactionMs;
    else averageTransactionMs = (averageTransactionMs * 7U + lastTransactionMs) / 8U;
  }
  txnActive = false;
  txnJobId = 0;
  txnTargetIndex = -1;
  txnRetriesUsed = 0;
  activeJobIsOneShot = false;
  radioState = RS_READY;
  radioStateStartedMs = millis();
}

static void failActiveTransaction(uint8_t statusCode, bool timeout) {
  if (timeout) stats.transactionTimeouts++;
  if (statusCode != RANGE_CANCELLED && role == ROLE_INITIATOR && txnActive &&
      txnRetriesUsed < scheduleRetries && (rangingEnabled || txnJobId != 0) && !radioStandby) {
    ++txnRetriesUsed;
    dwt_forcetrxoff();
    radioState = RS_READY;
    radioStateStartedMs = millis() - 1; // eligible for quick retry next scheduler pass
    return;
  }
  if (txnActive) {
    insertMeasurement(txnTargetId, -1, statusCode, 0, txnJobId);
    updateDueForTarget(txnTargetId);
  }
  dwt_forcetrxoff();
  finishActiveTransaction();
  if (role == ROLE_RESPONDER && rangingEnabled) armRx(RS_RX_LISTEN);
}

static bool beginPoll(uint16_t target, uint16_t jobId, int8_t targetIndex) {
  if (!radioReady || radioStandby || role != ROLE_INITIATOR) return false;
  txnActive = true; txnTargetId = target; txnJobId = jobId; txnTargetIndex = targetIndex;
  txnStartedAtMs = millis();
  activeJobIsOneShot = jobId != 0;
  txnRadioSeq = ++radioSequence;
  txnRetriesUsed = 0;
  peerId = target;
  pollTxTs = respRxTs = respPollRxTs = respTxTs = finalTxTs = 0;
  uint32_t dummy = 0;
  (void)dummy;
  if (!radioSend(target, MSG_POLL, nullptr, 0, false, 0, RS_TX_POLL)) {
    failActiveTransaction(RANGE_RADIO_ERROR, false);
    return false;
  }
  return true;
}

static void computeResponderRange(uint32_t t1, uint32_t t4, uint32_t t5, uint32_t t6) {
  // Use modulo-2^32 intervals; each interval must be shorter than 67 ms.
  const uint32_t Ra = t4 - t1;
  const uint32_t Da = t5 - t4;
  const uint32_t Db = respTxTs - respPollRxTs;
  const uint32_t Rb = t6 - respTxTs;
  const double denom = double(Ra) + double(Da) + double(Db) + double(Rb);
  if (denom <= 0.0) { computedRangeStatus = RANGE_INVALID; computedRangeMm = -1; return; }
  const double tof = (double(Ra) * double(Rb) - double(Da) * double(Db)) / denom;
  const double mm = tof * DWT_TICK_METERS * 1000.0;
  if (!isfinite(mm) || mm < 0.0 || mm > 200000.0) {
    computedRangeStatus = RANGE_INVALID; computedRangeMm = -1; return;
  }
  computedRangeStatus = RANGE_VALID;
  computedRangeMm = static_cast<int32_t>(llround(mm));
}

static bool tryReadRadioFrame(uint8_t *buf, uint16_t &dataLen, uint64_t &rxTs) {
  const uint32_t status = dwt_read32bitreg(SYS_STATUS_ID);
  currentRadioStatus = status;
  if (status & SYS_STATUS_ALL_RX_ERR) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
    stats.rxErrors++;
    recordRadioTrace(TRACE_RX_ERROR, 0, false, 0, 0, status,
                     dwt_read32bitreg(SYS_STATUS_ID));
    return false;
  }
  if (!(status & SYS_STATUS_RXFCG_BIT_MASK)) return false;
  const uint32_t frameInfo = dwt_read32bitreg(RX_FINFO_ID);
  const uint16_t frameWithFcs = frameInfo & RXFLEN_MASK;
  if (frameWithFcs < RADIO_COMMON_LEN + RADIO_FCS_BYTES || frameWithFcs > RADIO_FRAME_MAX + RADIO_FCS_BYTES) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
    stats.malformedRadioFrames++; return false;
  }
  rxTs = read40Timestamp(false);
  dataLen = frameWithFcs - RADIO_FCS_BYTES;
  dwt_readrxdata(buf, dataLen, 0);
  dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);
  if (!validAddressedFrame(buf, dataLen)) { stats.malformedRadioFrames++; return false; }
  return true;
}

static void radioArmListenRecovery();

static void handleRadioReceive(const uint8_t *b, uint16_t len, uint64_t rxTimestamp) {
  const uint16_t src = getU16(b + 7);
  const uint8_t seq = b[2], func = b[9];
  if (role == ROLE_RESPONDER) {
    if (!rangingEnabled) return;
    if (radioState == RS_RX_LISTEN && func == MSG_POLL && len == RADIO_COMMON_LEN && isAllowedPeer(src)) {
      peerId = src; txnTargetId = src; txnJobId = 0; txnTargetIndex = -1;
      txnActive = true; txnStartedAtMs = millis(); txnRetriesUsed = 0; txnRadioSeq = seq;
      respPollRxTs = static_cast<uint32_t>(rxTimestamp);
      const uint64_t respTxAbs = rxTimestamp + uint64_t(RESP_DELAY_UUS) * UWB_UUS_TO_DWT_TIME;
      respDelayReg = static_cast<uint32_t>(respTxAbs >> 8);
      respTxTs = static_cast<uint32_t>((uint64_t(respDelayReg & 0xFFFFFFFEUL) << 8) + cfg.txAntennaDelay);
      uint8_t p[8]; putU32(p, respPollRxTs); putU32(p + 4, respTxTs);
      if (!radioSend(peerId, MSG_RESP, p, sizeof(p), true, respDelayReg, RS_TX_RESP)) {
        failActiveTransaction(RANGE_RADIO_ERROR, false);
      }
      return;
    }
    if (radioState == RS_WAIT_FINAL && func == MSG_FINAL && len == RADIO_COMMON_LEN + 12 &&
        seq == txnRadioSeq && src == peerId && txnActive) {
      const uint32_t t1 = getU32(b + RADIO_COMMON_LEN);
      const uint32_t t4 = getU32(b + RADIO_COMMON_LEN + 4);
      const uint32_t t5 = getU32(b + RADIO_COMMON_LEN + 8);
      const uint32_t t6 = static_cast<uint32_t>(rxTimestamp);
      computeResponderRange(t1, t4, t5, t6);

      // Schedule the time-critical radio REPORT before notifying either host.
      // USB CDC writes are synchronous and must not precede delayed radio TX.
      const uint64_t reportTxAbs = rxTimestamp + uint64_t(REPORT_DELAY_UUS) * UWB_UUS_TO_DWT_TIME;
      const uint32_t reportDelayReg = static_cast<uint32_t>(reportTxAbs >> 8);
      uint8_t report[5];
      putU32(report, static_cast<uint32_t>(computedRangeMm));
      report[4] = computedRangeStatus;
      const bool reportStarted =
          radioSend(peerId, MSG_REPORT, report, sizeof(report), true,
                    reportDelayReg, RS_TX_REPORT);

      // Always retain the measurement, even when the USB host is absent.
      insertMeasurement(peerId, computedRangeMm, computedRangeStatus, 0, 0);
      if (!reportStarted) radioArmListenRecovery();
      return;
    }
    return;
  }

  if (role != ROLE_INITIATOR || !txnActive || seq != txnRadioSeq || src != txnTargetId) return;
  if (radioState == RS_WAIT_RESP && func == MSG_RESP && len == RADIO_COMMON_LEN + 8) {
    respRxTs = static_cast<uint32_t>(rxTimestamp);
    respPollRxTs = getU32(b + RADIO_COMMON_LEN);
    respTxTs = getU32(b + RADIO_COMMON_LEN + 4);
    const uint64_t finalTxAbs = rxTimestamp + uint64_t(FINAL_DELAY_UUS) * UWB_UUS_TO_DWT_TIME;
    finalDelayReg = static_cast<uint32_t>(finalTxAbs >> 8);
    finalTxTs = static_cast<uint32_t>((uint64_t(finalDelayReg & 0xFFFFFFFEUL) << 8) + cfg.txAntennaDelay);
    uint8_t p[12]; putU32(p, pollTxTs); putU32(p + 4, respRxTs); putU32(p + 8, finalTxTs);
    if (!radioSend(txnTargetId, MSG_FINAL, p, sizeof(p), true, finalDelayReg, RS_TX_FINAL)) {
      failActiveTransaction(RANGE_RADIO_ERROR, false);
    }
  } else if (radioState == RS_WAIT_REPORT && func == MSG_REPORT && len == RADIO_COMMON_LEN + 5) {
    const int32_t mm = static_cast<int32_t>(getU32(b + RADIO_COMMON_LEN));
    const uint8_t status = b[RADIO_COMMON_LEN + 4];
    if (status == RANGE_VALID && mm >= 0 && mm <= 200000) insertMeasurement(txnTargetId, mm, RANGE_VALID, 0, txnJobId);
    else insertMeasurement(txnTargetId, -1, RANGE_INVALID, 0, txnJobId);
    updateDueForTarget(txnTargetId);
    finishActiveTransaction();
  }
}

// Forward declaration used by response-failure path.
static void radioArmListenRecovery() {
  dwt_forcetrxoff();
  finishActiveTransaction();
  if (role == ROLE_RESPONDER && rangingEnabled && radioReady && !radioStandby) armRx(RS_RX_LISTEN);
}

static void radioPoll() {
  if (!radioReady || radioStandby) return;
  uint32_t now = millis();
  uint32_t status = dwt_read32bitreg(SYS_STATUS_ID);
  currentRadioStatus = status;

  if (isTxState(radioState) && (status & SYS_STATUS_TXFRS_BIT_MASK)) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_TXFRS_BIT_MASK);
    if (radioState == RS_TX_POLL) {
      pollTxTs = static_cast<uint32_t>(read40Timestamp(true));
      if (!armRx(RS_WAIT_RESP)) failActiveTransaction(RANGE_RADIO_ERROR, false);
    } else if (radioState == RS_TX_FINAL) {
      if (!armRx(RS_WAIT_REPORT)) failActiveTransaction(RANGE_RADIO_ERROR, false);
    } else if (radioState == RS_TX_RESP) {
      if (!armRx(RS_WAIT_FINAL)) failActiveTransaction(RANGE_RADIO_ERROR, false);
    } else if (radioState == RS_TX_REPORT) {
      radioArmListenRecovery();
    }
    return;
  }

  // RX errors invalidate an active receive step; a responder simply re-arms.
  if (status & SYS_STATUS_ALL_RX_ERR) {
    dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
    stats.rxErrors++;
    recordRadioTrace(TRACE_RX_ERROR, 0, false, 0, 0, status,
                     dwt_read32bitreg(SYS_STATUS_ID));
    if (radioState == RS_RX_LISTEN) { armRx(RS_RX_LISTEN); return; }
    if (radioState == RS_WAIT_RESP || radioState == RS_WAIT_REPORT || radioState == RS_WAIT_FINAL) {
      const RadioState waitState = radioState;
      const uint32_t originalStart = radioStateStartedMs;
      if (elapsed(now, originalStart, scheduleTimeoutMs)) {
        failActiveTransaction(RANGE_TIMEOUT, true); return;
      }
      if (armRx(waitState)) radioStateStartedMs = originalStart;
      return;
    }
  }

  if (status & SYS_STATUS_RXFCG_BIT_MASK) {
    uint8_t rx[RADIO_FRAME_MAX]; uint16_t len = 0; uint64_t rxTs = 0;
    const RadioState stateBeforeRead = radioState;
    const uint32_t originalStateStart = radioStateStartedMs;
    const bool valid = tryReadRadioFrame(rx, len, rxTs);
    if (valid) handleRadioReceive(rx, len, rxTs);
    if (radioState == stateBeforeRead && !isTxState(radioState) && radioReady && !radioStandby) {
      // The frame was malformed/unexpected. Re-arm without extending transaction timeout.
      if (armRx(stateBeforeRead) &&
          (stateBeforeRead == RS_WAIT_RESP || stateBeforeRead == RS_WAIT_REPORT || stateBeforeRead == RS_WAIT_FINAL)) {
        radioStateStartedMs = originalStateStart;
      }
    }
    return;
  }

  if (radioState == RS_READY) return;
  if (radioState == RS_RX_LISTEN) return;
  if (elapsed(now, radioStateStartedMs, scheduleTimeoutMs)) {
    if (isTxState(radioState)) {
      recordRadioTrace(TRACE_TX_COMPLETION_TIMEOUT, 0, false, 0, 0,
                       status, dwt_read32bitreg(SYS_STATUS_ID));
    }
    failActiveTransaction(RANGE_TIMEOUT, true);
  }
}

static void radioScheduler() {
  if (!radioReady || radioStandby) return;
  if (role == ROLE_RESPONDER) {
    if (rangingEnabled && radioState == RS_READY) armRx(RS_RX_LISTEN);
    return;
  }
  if (role != ROLE_INITIATOR || radioState != RS_READY) return;

  // A timed-out/failed transaction stays active while its retry budget remains.
  if (txnActive) {
    if (!elapsed(millis(), radioStateStartedMs, 2)) return;
    if (!radioSend(txnTargetId, MSG_POLL, nullptr, 0, false, 0, RS_TX_POLL)) {
      failActiveTransaction(RANGE_RADIO_ERROR, false);
    }
    return;
  }

  if (jobCount) {
    Job j = jobQueue[jobHead];
    jobHead = (jobHead + 1) % JOB_QUEUE_CAPACITY; jobCount--;
    // beginPoll records immediate start failures and keeps the transaction active
    // when a configured retry remains; do not enqueue a duplicate job here.
    beginPoll(j.targetId, j.id, -1);
    return;
  }
  if (!rangingEnabled || targetCount == 0) return;
  const uint32_t now = millis();
  for (uint8_t i = 0; i < targetCount; ++i) {
    if (static_cast<int32_t>(now - targetDueMs[i]) >= 0) {
      beginPoll(targetIds[i], 0, static_cast<int8_t>(i));
      return;
    }
  }
}

static void onCommandRoleChanged() {
  radioOff();
  if (role == ROLE_RESPONDER && rangingEnabled && radioReady && !radioStandby) armRx(RS_RX_LISTEN);
  emitStateChanged();
}

// -------------------- Protocol command handling --------------------
static void processRequest(HostTransport &source, const Frame &q);

static bool tryDuplicateReplay(HostTransport &source, const Frame &q) {
  const uint16_t pcrc = crc16(q.payload, q.length);
  for (const auto &e : source.replyCache) {
    if (e.valid && e.seq == q.seq && e.opcode == q.opcode && e.payloadCrc == pcrc) {
      stats.duplicateRequests++;
      sendFrameTo(source, e.response);
      return true;
    }
  }
  return false;
}

static uint32_t estimatedPerTargetPeriodMs() {
  if (role != ROLE_INITIATOR || targetCount == 0) return schedulePeriodMs;
  // Estimate serialized radio occupancy using measured transaction duration.
  // Four milliseconds is a startup heuristic; the estimate adapts as ranges finish.
  const uint32_t oneTxn = averageTransactionMs ? averageTransactionMs : 4U;
  const uint64_t serialized = uint64_t(oneTxn) * targetCount;
  return serialized > schedulePeriodMs ? static_cast<uint32_t>(serialized) : schedulePeriodMs;
}

static void processRequest(HostTransport &source, const Frame &q) {
  if (q.type != TYPE_REQUEST) return;
  if (tryDuplicateReplay(source, q)) return;
  const uint8_t *p = q.payload; const uint16_t n = q.length;
  uint8_t out[MAX_PAYLOAD]{}; uint16_t outLen = 0; uint8_t error = ERR_OK;

  switch (q.opcode) {
    case OP_PING: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      out[0] = 0x50; out[1] = 0x4F; out[2] = 0x4E; out[3] = 0x47; outLen = 4;
      break;
    }

    case OP_GET_INFO: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      out[0] = 1; out[1] = 0; out[2] = 0; out[3] = VERSION;
      putU16(out + 4, cfg.deviceId); out[6] = static_cast<uint8_t>(MAX_TARGETS);
      putU16(out + 7, HISTORY_CAPACITY); out[9] = 1; // radioType 1 = DWM3000
      putU16(out + 10, MAX_PAYLOAD);
      out[12] = 1; // BLE is supported on this device.
      out[13] = (1u << UwbBle::MODE_IDLE) | (1u << UwbBle::MODE_HUB) |
                (1u << UwbBle::MODE_PERIPHERAL);
      out[14] = UwbBle::MAX_PEERS;
      out[15] = UwbBle::MAX_ACTIVE_CONNECTIONS;
      putU16(out + 16, UwbBle::MAX_MESSAGE_LENGTH);
      putU16(out + 18, UwbBle::MAX_ATT_MTU);
      out[20] = 1; // BLE application protocol version.
      outLen = 21;
      break;
    }

    case OP_GET_STATUS: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      out[0] = static_cast<uint8_t>(role); out[1] = rangingEnabled ? 1 : 0;
      out[2] = radioReady ? 1 : 0; out[3] = radioStandby ? 1 : 0;
      out[4] = targetCount; out[5] = clockSynchronized ? 1 : 0;
      putU32(out + 6, static_cast<uint32_t>(monoMicros() / 1000));
      putU32(out + 10, stats.rangesOk); putU32(out + 14, stats.rangesFailed);
      outLen = 18;
      out[18] = bleCfg.mode;
      out[19] = ble.enabled() ? 1 : 0;
      out[20] = ble.connected() ? 1 : 0;
      putU16(out + 21, ble.connectedPeerId());
      UwbBle::PeerStatus localBleStatus;
      ble.getPeerStatus(ble.connectedPeerId(), localBleStatus);
      out[23] = localBleStatus.advertising;
      out[24] = localBleStatus.scanning;
      putU16(out + 25, localBleStatus.mtu);
      outLen = 27;
      break;
    }

    case OP_GET_STATISTICS: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      putU32(out, stats.rangesOk); putU32(out + 4, stats.rangesFailed);
      putU32(out + 8, stats.rxErrors); putU32(out + 12, stats.txErrors);
      putU32(out + 16, stats.transactionTimeouts); putU32(out + 20, stats.protocolErrors);
      putU32(out + 24, stats.uartOverflows); putU32(out + 28, stats.historyOverwrites);
      putU32(out + 32, stats.malformedRadioFrames); putU32(out + 36, stats.duplicateRequests);
      UwbBle::Statistics bleStats; ble.getStatistics(bleStats);
      putU32(out + 40, bleStats.messagesSent); putU32(out + 44, bleStats.messagesReceived);
      putU32(out + 48, bleStats.txErrors); putU32(out + 52, bleStats.rxErrors);
      putU32(out + 56, bleStats.connectionFailures); putU32(out + 60, bleStats.rxQueueOverflows);
      putU32(out + 64, bleStats.eventQueueOverflows);
      outLen = 68;
      break;
    }

    case OP_CLEAR_STATISTICS: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      stats = Counters(); ble.clearStatistics(); outLen = 0; break;
    }

    case OP_GET_LAST_ERROR: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      out[0] = lastErrorSubsystem; out[1] = lastErrorCode;
      putU64(out + 2, lastErrorMonoUs); putU16(out + 10, lastErrorPeerId);
      putU32(out + 12, lastErrorContext); outLen = 16; break;
    }

    case OP_GET_CONFIG: {
      // Runtime RAM settings. GET_STATUS separately reports current operating state.
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      putU16(out, cfg.deviceId); putU16(out + 2, cfg.panId); out[4] = cfg.channel;
      putU16(out + 5, cfg.txAntennaDelay); putU16(out + 7, cfg.rxAntennaDelay);
      putU32(out + 9, schedulePeriodMs); putU16(out + 13, scheduleTimeoutMs);
      out[15] = scheduleRetries; outLen = 16;
      outLen += encodeBleConfig(out + outLen, bleCfg);
      break;
    }

    case OP_GET_SAVED_CONFIG: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      StoredSettings saved;
      if (!readStoredSettings(saved)) { error = ERR_INTERNAL; break; }
      putU16(out, saved.config.deviceId); putU16(out + 2, saved.config.panId);
      out[4] = saved.config.channel;
      putU16(out + 5, saved.config.txAntennaDelay);
      putU16(out + 7, saved.config.rxAntennaDelay);
      putU32(out + 9, saved.periodMs); putU16(out + 13, saved.timeoutMs);
      out[15] = saved.retries; out[16] = static_cast<uint8_t>(saved.role);
      out[17] = saved.targetCount;
      for (uint8_t i = 0; i < saved.targetCount; ++i)
        putU16(out + 18 + i * 2, saved.targets[i]);
      out[18 + saved.targetCount * 2] = saved.autoStart ? 1 : 0;
      outLen = 19 + saved.targetCount * 2;
      outLen += encodeBleConfig(out + outLen, saved.bleConfig);
      break;
    }

    case OP_APPLY_SAVE_CONFIG: {
      // Payload: dev:u16, pan:u16, channel:u8, tx:u16, rx:u16,
      // period:u32, timeout:u16, retries:u8, role:u8, count:u8,
      // targets[count]:u16, autoStart:u8. This is the explicit NVS snapshot
      // submitted by the Configuration page, not whatever happens to be in RAM.
      if (n < 19) { error = ERR_BAD_ARGUMENT; break; }
      const uint16_t newDev = getU16(p), newPan = getU16(p + 2);
      const uint8_t newChannel = p[4];
      const uint16_t txDly = getU16(p + 5), rxDly = getU16(p + 7);
      const uint32_t newPeriod = getU32(p + 9);
      const uint16_t newTimeout = getU16(p + 13);
      const uint8_t newRetries = p[15], newRoleValue = p[16], newCount = p[17];
      const uint16_t baseLength = 19 + 2 * newCount;
      if (newCount > MAX_TARGETS || n < baseLength + BLE_CONFIG_FIXED_LENGTH) {
        error = ERR_BAD_ARGUMENT; break;
      }
      const uint8_t autoStartValue = p[18 + 2 * newCount];
      UwbBle::Config newBleCfg;
      if (!decodeBleConfig(p + baseLength, n - baseLength, newBleCfg)) {
        error = ERR_BAD_ARGUMENT; break;
      }
      if (newDev == 0 || newDev == 0xFFFF ||
          (newChannel != 5 && newChannel != 9) ||
          newPeriod < 20 || newPeriod > 60000 ||
          newTimeout < 50 || newTimeout > 5000 || newRetries > 5 ||
          newRoleValue > ROLE_RESPONDER || autoStartValue > 1 ||
          (newBleCfg.autoStart && newBleCfg.mode == UwbBle::MODE_IDLE)) {
        error = ERR_BAD_ARGUMENT; break;
      }
      if (autoStartValue && (newRoleValue == ROLE_IDLE ||
          (newRoleValue == ROLE_INITIATOR && newCount == 0))) {
        error = ERR_BAD_ARGUMENT; break;
      }
      if (txnActive || rangingEnabled || jobCount || ble.connected()) { error = ERR_BUSY; break; }
      uint16_t newTargets[MAX_TARGETS]{};
      for (uint8_t i = 0; i < newCount; ++i) {
        const uint16_t id = getU16(p + 18 + i * 2);
        if (id == 0 || id == 0xFFFF || id == newDev) { error = ERR_BAD_ARGUMENT; break; }
        for (uint8_t j = 0; j < i; ++j) if (newTargets[j] == id) error = ERR_BAD_ARGUMENT;
        if (error != ERR_OK) break;
        newTargets[i] = id;
      }
      if (error != ERR_OK) break;

      Config newCfg;
      newCfg.deviceId = newDev; newCfg.panId = newPan; newCfg.channel = newChannel;
      newCfg.txAntennaDelay = txDly; newCfg.rxAntennaDelay = rxDly;
      newCfg.targetPeriodMs = newPeriod; newCfg.transactionTimeoutMs = newTimeout;
      newCfg.retries = newRetries;
      const Role newRole = static_cast<Role>(newRoleValue);
      const bool newAutoStart = autoStartValue != 0;
      const bool bleWasEnabled = ble.enabled();
      if (bleWasEnabled) ble.stop();
      if (!ble.configure(newBleCfg, newDev)) {
        if (bleWasEnabled) ble.start();
        error = ERR_BAD_ARGUMENT; break;
      }
      if (!writeStoredSettings(newCfg, newRole, newTargets, newCount,
                               newPeriod, newTimeout, newRetries, newAutoStart, newBleCfg)) {
        ble.configure(bleCfg, cfg.deviceId);
        if (bleWasEnabled) ble.start();
        error = ERR_INTERNAL; break;
      }

      radioOff();
      cfg = newCfg; bleCfg = newBleCfg; role = newRole; targetCount = newCount;
      if (targetCount) memcpy(targetIds, newTargets, targetCount * sizeof(uint16_t));
      schedulePeriodMs = newPeriod; scheduleTimeoutMs = newTimeout;
      scheduleRetries = newRetries; autoStartRanging = newAutoStart;
      rangingEnabled = false;
      // BLE is an independent subsystem; restore it even when UWB radio startup
      // fails so a temporary UWB fault does not suppress the configured BLE role.
      // Preserve the live enabled/disabled state across configuration saves.
      // The auto-start preference controls only what happens on the next boot.
      if (bleWasEnabled && newBleCfg.mode != UwbBle::MODE_IDLE && !ble.start()) {
        error = ERR_INTERNAL;
      }
      if (!radioStandby) radioReady = radioInitialize();
      for (uint8_t i = 0; i < targetCount; ++i) targetDueMs[i] = millis();
      if (!radioReady && !radioStandby && error == ERR_OK) error = ERR_RADIO_FAILURE;
      emitStateChanged();
      outLen = 0;
      break;
    }

    case OP_SAVE_ANTENNA_DELAYS: {
      // Calibration persistence is deliberately narrow: only the two antenna
      // delay keys are updated. Runtime role/schedule/targets are never saved.
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      if (txnActive || rangingEnabled) { error = ERR_BUSY; break; }
      if (!preferences.begin("uwb-node", false)) { error = ERR_INTERNAL; break; }
      {
        bool ok = preferences.putUShort("txdel", cfg.txAntennaDelay) == sizeof(uint16_t);
        ok &= preferences.putUShort("rxdel", cfg.rxAntennaDelay) == sizeof(uint16_t);
        preferences.end();
        if (!ok) { error = ERR_INTERNAL; break; }
      }
      putU16(out, cfg.txAntennaDelay); putU16(out + 2, cfg.rxAntennaDelay);
      outLen = 4;
      break;
    }

    case OP_SET_CONFIG: {
      if (n < 9 + BLE_CONFIG_FIXED_LENGTH) { error = ERR_BAD_ARGUMENT; break; }
      const uint16_t newDev = getU16(p), newPan = getU16(p + 2);
      const uint8_t newChannel = p[4];
      const uint16_t txDly = getU16(p + 5), rxDly = getU16(p + 7);
      if (newDev == 0 || newDev == 0xFFFF || (newChannel != 5 && newChannel != 9)) { error = ERR_BAD_ARGUMENT; break; }
      if (txnActive || rangingEnabled || ble.connected()) { error = ERR_BUSY; break; }
      for (uint8_t i = 0; i < targetCount; ++i) if (targetIds[i] == newDev) { error = ERR_BAD_ARGUMENT; break; }
      if (error != ERR_OK) break;
      UwbBle::Config newBleCfg;
      if (!decodeBleConfig(p + 9, n - 9, newBleCfg)) { error = ERR_BAD_ARGUMENT; break; }
      if (!ble.configure(newBleCfg, newDev)) { error = ERR_BUSY; break; }
      bleCfg = newBleCfg;
      cfg.deviceId = newDev; cfg.panId = newPan; cfg.channel = newChannel;
      cfg.txAntennaDelay = txDly; cfg.rxAntennaDelay = rxDly;
      radioOff();
      if (!radioStandby) radioReady = radioInitialize();
      if (!radioReady) { error = ERR_RADIO_FAILURE; break; }
      outLen = 0;
      break;
    }

    case OP_RESET_CONFIG: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      if (txnActive) { error = ERR_BUSY; break; }
      preferences.begin("uwb-node", false); preferences.clear(); preferences.end();
      ble.stop();
      cfg = Config(); bleCfg = UwbBle::Config();
      ble.configure(bleCfg, cfg.deviceId);
      schedulePeriodMs = cfg.targetPeriodMs; scheduleTimeoutMs = cfg.transactionTimeoutMs;
      scheduleRetries = cfg.retries; targetCount = 0; role = ROLE_IDLE;
      rangingEnabled = false; autoStartRanging = false;
      cancelQueuedJobs();
      radioOff();
      radioStandby = false;
      radioReady = radioInitialize();
      outLen = 0; emitStateChanged();
      if (!radioReady) error = ERR_RADIO_FAILURE;
      break;
    }

    case OP_TIME_SYNC: {
      if (n != 8) { error = ERR_BAD_ARGUMENT; break; }
      epochOffsetUs = static_cast<int64_t>(getU64(p)) - static_cast<int64_t>(monoMicros());
      clockSynchronized = true;
      putU64(out, monoMicros()); putU64(out + 8, static_cast<uint64_t>(epochOffsetUs)); outLen = 16;
      break;
    }

    case OP_GET_ANTENNA_DELAYS: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      putU16(out, cfg.txAntennaDelay); putU16(out + 2, cfg.rxAntennaDelay); outLen = 4; break;
    }

    case OP_SET_ANTENNA_DELAYS: {
      if (n != 4) { error = ERR_BAD_ARGUMENT; break; }
      if (txnActive || rangingEnabled) { error = ERR_BUSY; break; }
      cfg.txAntennaDelay = getU16(p); cfg.rxAntennaDelay = getU16(p + 2);
      if (radioReady && !radioStandby) {
        dwt_settxantennadelay(cfg.txAntennaDelay); dwt_setrxantennadelay(cfg.rxAntennaDelay);
      }
      outLen = 0; break;
    }

    case OP_SET_ROLE: {
      if (n != 1 || p[0] > ROLE_RESPONDER) { error = ERR_BAD_ARGUMENT; break; }
      if (txnActive) { error = ERR_BUSY; break; }
      cancelQueuedJobs();
      role = static_cast<Role>(p[0]); rangingEnabled = false; onCommandRoleChanged(); outLen = 0; break;
    }

    case OP_SET_TARGETS: {
      if (n < 1) { error = ERR_BAD_ARGUMENT; break; }
      const uint8_t count = p[0];
      if (count > MAX_TARGETS || n != 1 + 2 * count) { error = ERR_BAD_ARGUMENT; break; }
      uint16_t newList[MAX_TARGETS]{};
      for (uint8_t i = 0; i < count; ++i) {
        const uint16_t id = getU16(p + 1 + i * 2);
        if (!validTargetId(id)) { error = ERR_BAD_ARGUMENT; break; }
        for (uint8_t j = 0; j < i; ++j) if (newList[j] == id) error = ERR_BAD_ARGUMENT;
        if (error != ERR_OK) break;
        newList[i] = id;
      }
      if (error != ERR_OK) break;
      if (txnActive) { error = ERR_BUSY; break; }
      memcpy(targetIds, newList, count * sizeof(uint16_t)); targetCount = count;
      for (uint8_t i = 0; i < targetCount; ++i) targetDueMs[i] = millis();
      out[0] = targetCount; for (uint8_t i = 0; i < targetCount; ++i) putU16(out + 1 + 2 * i, targetIds[i]);
      outLen = 1 + 2 * targetCount; break;
    }

    case OP_GET_TARGETS: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      out[0] = targetCount; for (uint8_t i = 0; i < targetCount; ++i) putU16(out + 1 + 2 * i, targetIds[i]);
      outLen = 1 + 2 * targetCount; break;
    }

    case OP_SET_UWB_SCHEDULE: {
      if (n != 7) { error = ERR_BAD_ARGUMENT; break; }
      const uint32_t period = getU32(p); const uint16_t timeout = getU16(p + 4); const uint8_t retries = p[6];
      if (period < 20 || period > 60000 || timeout < 50 || timeout > 5000 || retries > 5) { error = ERR_BAD_ARGUMENT; break; }
      schedulePeriodMs = period; scheduleTimeoutMs = timeout; scheduleRetries = retries;
      cfg.targetPeriodMs = period; cfg.transactionTimeoutMs = timeout; cfg.retries = retries;
      for (uint8_t i = 0; i < targetCount; ++i) targetDueMs[i] = millis();
      outLen = 0; break;
    }

    case OP_GET_UWB_SCHEDULE: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      putU32(out, schedulePeriodMs); putU16(out + 4, scheduleTimeoutMs); out[6] = scheduleRetries;
      out[7] = targetCount; out[8] = jobCount; out[9] = txnActive ? 1 : 0;
      putU32(out + 10, estimatedPerTargetPeriodMs()); putU32(out + 14, averageTransactionMs);
      outLen = 18; break;
    }

    case OP_START_RANGING: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      if (radioStandby || !radioReady) { error = ERR_BAD_STATE; break; }
      if (role == ROLE_IDLE) { error = ERR_BAD_STATE; break; }
      if (role == ROLE_INITIATOR && targetCount == 0 && jobCount == 0) { error = ERR_BAD_STATE; break; }
      rangingEnabled = true;
      if (role == ROLE_RESPONDER) { radioOff(); armRx(RS_RX_LISTEN); }
      else for (uint8_t i = 0; i < targetCount; ++i) targetDueMs[i] = millis();
      emitStateChanged(); outLen = 0; break;
    }

    case OP_STOP_RANGING: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      rangingEnabled = false;
      if (txnActive) failActiveTransaction(RANGE_CANCELLED, false); else radioOff();
      cancelQueuedJobs(); emitStateChanged(); outLen = 0; break;
    }

    case OP_RANGE_ONCE: {
      if (n != 2) { error = ERR_BAD_ARGUMENT; break; }
      const uint16_t target = getU16(p);
      if (role != ROLE_INITIATOR || !radioReady || radioStandby || !validTargetId(target)) { error = ERR_BAD_STATE; break; }
      if (jobCount >= JOB_QUEUE_CAPACITY) { error = ERR_BUSY; break; }
      uint16_t job = nextJobId++; if (nextJobId == 0) nextJobId = 1;
      const uint8_t tail = (jobHead + jobCount) % JOB_QUEUE_CAPACITY;
      jobQueue[tail] = {job, target}; jobCount++;
      putU16(out, job); outLen = 2; break;
    }

    case OP_CANCEL_OPERATION: {
      if (n != 2 || getU16(p) == 0) { error = ERR_BAD_ARGUMENT; break; }
      const uint16_t job = getU16(p); bool removed = false;
      Job tmp[JOB_QUEUE_CAPACITY]; uint8_t kept = 0;
      for (uint8_t i = 0; i < jobCount; ++i) {
        const Job &j = jobQueue[(jobHead + i) % JOB_QUEUE_CAPACITY];
        if (j.id == job) {
          removed = true;
          insertMeasurement(j.targetId, -1, RANGE_CANCELLED, 0, j.id);
        } else tmp[kept++] = j;
      }
      jobHead = 0; jobCount = kept; for (uint8_t i = 0; i < kept; ++i) jobQueue[i] = tmp[i];
      if (txnActive && txnJobId == job) { failActiveTransaction(RANGE_CANCELLED, false); removed = true; }
      if (!removed) { error = ERR_NOT_FOUND; break; }
      outLen = 0; break;
    }

    case OP_GET_LAST_RANGE: {
      if (n != 2) { error = ERR_BAD_ARGUMENT; break; }
      const uint16_t filter = getU16(p);
      Measurement found[MAX_TARGETS]; uint8_t foundCount = 0;
      for (int k = historyCount - 1; k >= 0 && foundCount < MAX_TARGETS; --k) {
        const uint16_t idx = (historyHead + HISTORY_CAPACITY - historyCount + k) % HISTORY_CAPACITY;
        const Measurement &m = history[idx];
        if (filter != 0xFFFF && m.targetId != filter) continue;
        bool already = false;
        for (uint8_t j = 0; j < foundCount; ++j) if (found[j].targetId == m.targetId) already = true;
        if (!already) found[foundCount++] = m;
        if (filter != 0xFFFF && foundCount) break;
      }
      out[0] = foundCount;
      for (uint8_t i = 0; i < foundCount; ++i) encodeMeasurement(out + 1 + i * 28, found[i]);
      outLen = 1 + foundCount * 28; break;
    }

    case OP_GET_HISTORY: {
      if (n != 8) { error = ERR_BAD_ARGUMENT; break; }
      const uint32_t afterSeq = getU32(p); uint16_t maxRecs = getU16(p + 4); const uint16_t filter = getU16(p + 6);
      if (maxRecs == 0) maxRecs = 1;
      if (maxRecs > MAX_HISTORY_PER_REPLY) maxRecs = MAX_HISTORY_PER_REPLY;
      const uint16_t oldestIdx = historyCount ? (historyHead + HISTORY_CAPACITY - historyCount) % HISTORY_CAPACITY : historyHead;
      const uint32_t oldest = historyCount ? history[oldestIdx].sampleSeq : 0;
      const uint32_t newest = historyCount ? history[(historyHead + HISTORY_CAPACITY - 1) % HISTORY_CAPACITY].sampleSeq : 0;
      const bool overrun = historyCount && afterSeq != 0 && afterSeq < oldest - 1;
      putU32(out, oldest); putU32(out + 4, newest); putU32(out + 8, afterSeq); out[12] = overrun ? 1 : 0;
      out[13] = 0; uint8_t count = 0; uint32_t cursor = afterSeq;
      for (uint16_t k = 0; k < historyCount && count < maxRecs; ++k) {
        const uint16_t idx = (oldestIdx + k) % HISTORY_CAPACITY; const Measurement &m = history[idx];
        if (overrun && m.sampleSeq < oldest) continue;
        if (m.sampleSeq <= afterSeq) continue;
        cursor = m.sampleSeq; // advance across filtered-out records too
        if (filter != 0xFFFF && m.targetId != filter) continue;
        encodeMeasurement(out + 14 + count * 28, m); count++;
      }
      out[13] = count; putU32(out + 8, cursor); outLen = 14 + count * 28;
      if (overrun) historyHasOverrun = false;
      break;
    }

    case OP_SUBSCRIBE: {
      if (n != 2) { error = ERR_BAD_ARGUMENT; break; }
      source.subscriptionMask |= getU16(p); putU16(out, source.subscriptionMask); outLen = 2; break;
    }

    case OP_UNSUBSCRIBE: {
      if (n != 2) { error = ERR_BAD_ARGUMENT; break; }
      source.subscriptionMask &= ~getU16(p); putU16(out, source.subscriptionMask); outLen = 2; break;
    }

    case OP_SET_PEERS: {
      if (n < 1) { error = ERR_BAD_ARGUMENT; break; }
      const uint8_t count = p[0];
      if (count > UwbBle::MAX_PEERS || n != 1 + 2 * count) { error = ERR_BAD_ARGUMENT; break; }
      uint16_t peers[UwbBle::MAX_PEERS]{};
      for (uint8_t i = 0; i < count; ++i) peers[i] = getU16(p + 1 + i * 2);
      UwbBle::Config updated = bleCfg;
      updated.peerCount = count;
      memset(updated.peers, 0, sizeof(updated.peers));
      if (count) memcpy(updated.peers, peers, count * sizeof(uint16_t));
      if (!ble.configure(updated)) { error = ERR_BUSY; break; }
      if (!persistBlePeerList(updated)) {
        ble.configure(bleCfg);
        error = ERR_INTERNAL; break;
      }
      bleCfg = updated;
      outLen = 0;
      break;
    }

    case OP_GET_PEERS: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      out[0] = bleCfg.peerCount;
      for (uint8_t i = 0; i < bleCfg.peerCount; ++i) putU16(out + 1 + i * 2, bleCfg.peers[i]);
      outLen = 1 + 2 * bleCfg.peerCount;
      break;
    }

    case OP_START_BLE: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      if (bleCfg.mode == UwbBle::MODE_IDLE) { error = ERR_BAD_STATE; break; }
      if (!ble.enabled() && !ble.start()) {
        error = ERR_INTERNAL; break;
      }
      outLen = 0;
      break;
    }

    case OP_STOP_BLE: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      ble.stop();
      outLen = 0; break;
    }

    case OP_GET_PEER_STATUS: {
      if (n != 2) { error = ERR_BAD_ARGUMENT; break; }
      UwbBle::PeerStatus status;
      if (!ble.getPeerStatus(getU16(p), status)) { error = ERR_NOT_FOUND; break; }
      out[0] = status.state; out[1] = status.mode; out[2] = status.enabled;
      out[3] = status.advertising; out[4] = status.scanning; out[5] = status.connected;
      putU16(out + 6, status.peerId); putU16(out + 8, status.mtu);
      out[10] = status.lastFailureReason;
      out[11] = status.lastFailureStage;
      putU16(out + 12, static_cast<uint16_t>(status.lastFailureDetail));
      outLen = 14;
      break;
    }

    case OP_GET_BLE_DIAGNOSTIC_TRACE: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      outLen = ble.getDiagnosticTrace(out, sizeof(out));
      if (!outLen) error = ERR_INTERNAL;
      break;
    }

    case OP_SEND_TO_PEER: {
      if (n < 4) { error = ERR_BAD_ARGUMENT; break; }
      const uint16_t peerId = getU16(p);
      const uint16_t dataLength = getU16(p + 2);
      if (dataLength > UwbBle::MAX_MESSAGE_LENGTH || n != 4 + dataLength) { error = ERR_BAD_ARGUMENT; break; }
      uint16_t messageId = 0;
      if (!ble.sendToPeer(peerId, p + 4, dataLength, messageId)) {
        error = ble.connected() ? ERR_BUSY : ERR_BAD_STATE; break;
      }
      putU16(out, messageId); outLen = 2;
      break;
    }

    case OP_GET_RECEIVED_DATA: {
      if (n != 4) { error = ERR_BAD_ARGUMENT; break; }
      const uint16_t peerId = getU16(p), messageId = getU16(p + 2);
      uint16_t dataLength = 0;
      if (!ble.getReceivedData(peerId, messageId, out + 4,
                               UwbBle::MAX_MESSAGE_LENGTH, dataLength)) {
        error = ERR_NOT_FOUND; break;
      }
      putU16(out, messageId); putU16(out + 2, dataLength);
      outLen = 4 + dataLength;
      break;
    }

    case OP_GET_DIAGNOSTICS: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      putU32(out, currentRadioStatus); putU32(out + 4, radioReady && !radioStandby ? dwt_read32bitreg(SYS_STATUS_ID) : 0);
      putU32(out + 8, stats.malformedRadioFrames); putU32(out + 12, stats.rxErrors);
      putU32(out + 16, stats.txErrors); out[20] = static_cast<uint8_t>(radioState);
      out[21] = radioStandby ? 1 : 0;
      UwbBle::Diagnostics bleDiag; ble.getDiagnostics(bleDiag);
      out[22] = bleDiag.mode; out[23] = bleDiag.enabled;
      out[24] = bleDiag.advertising; out[25] = bleDiag.scanning;
      out[26] = bleDiag.activeLinks; out[27] = bleDiag.rxQueueDepth;
      out[28] = bleDiag.eventQueueDepth; outLen = 29; break;
    }

    // Canonical trace access. Source 0 = radio state-machine trace,
    // 1 = detailed TX-start trace, 2 = BLE lifecycle trace. Response:
    // trace_source:u8, record_size:u8, count:u8, then count records.
    case OP_GET_TRACE: {
      if (n != 1 || p[0] > TRACE_SOURCE_BLE_LIFECYCLE) { error = ERR_BAD_ARGUMENT; break; }
      const uint8_t sourceType = p[0];
      if (sourceType == TRACE_SOURCE_UWB_RADIO) {
        out[0] = sourceType; out[1] = RADIO_TRACE_RECORD_BYTES; out[2] = radioTraceCount;
        const uint8_t oldest = (radioTraceHead + RADIO_TRACE_CAPACITY - radioTraceCount) % RADIO_TRACE_CAPACITY;
        for (uint8_t i = 0; i < radioTraceCount; ++i) {
          const RadioTraceRecord &r = radioTrace[(oldest + i) % RADIO_TRACE_CAPACITY];
          uint8_t *d = out + 3 + i * RADIO_TRACE_RECORD_BYTES;
          putU32(d, r.uptimeMs); putU32(d + 4, r.statusBefore);
          putU32(d + 8, r.statusAfter); putU32(d + 12, r.delayedTimeReg);
          putU16(d + 16, r.peer); putU16(d + 18, r.jobId);
          d[20] = r.event; d[21] = r.state; d[22] = r.function;
          d[23] = r.radioSeq; d[24] = r.delayed; d[25] = r.retriesUsed;
          d[26] = r.rc; d[27] = r.reserved;
        }
        outLen = 3 + radioTraceCount * RADIO_TRACE_RECORD_BYTES;
      } else if (sourceType == TRACE_SOURCE_UWB_TX_START) {
        out[0] = sourceType; out[1] = RADIO_TRACE_DETAIL_RECORD_BYTES; out[2] = radioTraceDetailCount;
        const uint8_t oldest = (radioTraceDetailHead + RADIO_TRACE_DETAIL_CAPACITY -
                                radioTraceDetailCount) % RADIO_TRACE_DETAIL_CAPACITY;
        for (uint8_t i = 0; i < radioTraceDetailCount; ++i) {
          const RadioTraceDetailRecord &r =
            radioTraceDetail[(oldest + i) % RADIO_TRACE_DETAIL_CAPACITY];
          uint8_t *d = out + 3 + i * RADIO_TRACE_DETAIL_RECORD_BYTES;
          putU32(d, r.uptimeMs); putU32(d + 4, r.statusBefore);
          putU32(d + 8, r.statusAfter); putU32(d + 12, r.sysStateBefore);
          putU32(d + 16, r.sysStateAfter); putU32(d + 20, r.sysTimeBefore);
          putU32(d + 24, r.delayedTimeReg); putU32(d + 28, r.scheduledTxTimestampLow);
          putU16(d + 32, r.peer); putU16(d + 34, r.jobId);
          d[36] = r.event; d[37] = r.state; d[38] = r.function; d[39] = r.radioSeq;
          d[40] = r.delayed; d[41] = r.retriesUsed; d[42] = r.rc; d[43] = r.failureHint;
        }
        outLen = 3 + radioTraceDetailCount * RADIO_TRACE_DETAIL_RECORD_BYTES;
      } else {
        uint8_t legacy[1 + UwbBle::BLE_DIAGNOSTIC_TRACE_CAPACITY * UwbBle::BLE_DIAGNOSTIC_RECORD_SIZE]{};
        const uint16_t legacyLength = ble.getDiagnosticTrace(legacy, sizeof(legacy));
        out[0] = sourceType; out[1] = UwbBle::BLE_DIAGNOSTIC_RECORD_SIZE; out[2] = legacy[0];
        if (legacyLength > 1) memcpy(out + 3, legacy + 1, legacyLength - 1);
        outLen = 3 + (legacyLength > 1 ? legacyLength - 1 : 0);
      }
      break;
    }

    // Additive diagnostic extension; all existing v1 commands/payloads stay unchanged.
    // Response: count:u8, then count 28-byte records, oldest first.
    // Record: uptimeMs:u32, statusBefore:u32, statusAfter:u32, delayedTimeReg:u32,
    //         peer:u16, jobId:u16, event:u8, state:u8, function:u8, radioSeq:u8,
    //         delayed:u8, retriesUsed:u8, rc:u8, reserved:u8.
    case OP_GET_RADIO_TRACE: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      out[0] = radioTraceCount;
      const uint8_t oldest = (radioTraceHead + RADIO_TRACE_CAPACITY - radioTraceCount) % RADIO_TRACE_CAPACITY;
      for (uint8_t i = 0; i < radioTraceCount; ++i) {
        const RadioTraceRecord &r = radioTrace[(oldest + i) % RADIO_TRACE_CAPACITY];
        uint8_t *d = out + 1 + i * RADIO_TRACE_RECORD_BYTES;
        putU32(d, r.uptimeMs); putU32(d + 4, r.statusBefore);
        putU32(d + 8, r.statusAfter); putU32(d + 12, r.delayedTimeReg);
        putU16(d + 16, r.peer); putU16(d + 18, r.jobId);
        d[20] = r.event; d[21] = r.state; d[22] = r.function;
        d[23] = r.radioSeq; d[24] = r.delayed; d[25] = r.retriesUsed;
        d[26] = r.rc; d[27] = r.reserved;
      }
      outLen = 1 + radioTraceCount * RADIO_TRACE_RECORD_BYTES;
      break;
    }

    // Legacy detail response: count:u8 followed by 44-byte records, oldest first.
    // This is an application-only snapshot; post-call registers may already
    // reflect the driver's TXRXOFF cleanup after a failed start.
    case OP_GET_RADIO_TRACE_DETAIL: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      out[0] = radioTraceDetailCount;
      const uint8_t oldest = (radioTraceDetailHead + RADIO_TRACE_DETAIL_CAPACITY -
                              radioTraceDetailCount) % RADIO_TRACE_DETAIL_CAPACITY;
      for (uint8_t i = 0; i < radioTraceDetailCount; ++i) {
        const RadioTraceDetailRecord &r =
          radioTraceDetail[(oldest + i) % RADIO_TRACE_DETAIL_CAPACITY];
        uint8_t *d = out + 1 + i * RADIO_TRACE_DETAIL_RECORD_BYTES;
        putU32(d, r.uptimeMs);
        putU32(d + 4, r.statusBefore);
        putU32(d + 8, r.statusAfter);
        putU32(d + 12, r.sysStateBefore);
        putU32(d + 16, r.sysStateAfter);
        putU32(d + 20, r.sysTimeBefore);
        putU32(d + 24, r.delayedTimeReg);
        putU32(d + 28, r.scheduledTxTimestampLow);
        putU16(d + 32, r.peer);
        putU16(d + 34, r.jobId);
        d[36] = r.event; d[37] = r.state; d[38] = r.function; d[39] = r.radioSeq;
        d[40] = r.delayed; d[41] = r.retriesUsed; d[42] = r.rc; d[43] = r.failureHint;
      }
      outLen = 1 + radioTraceDetailCount * RADIO_TRACE_DETAIL_RECORD_BYTES;
      break;
    }

    case OP_UWB_SLEEP: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      if (txnActive) { error = ERR_BUSY; break; }
      rangingEnabled = false; radioOff(); cancelQueuedJobs();
      if (!radioSetStandby()) { error = ERR_RADIO_FAILURE; break; }
      out[0] = 1; // 1 = IDLE_RC standby; true DW3000 deep sleep is not enabled here.
      outLen = 1; emitStateChanged(); break;
    }

    case OP_UWB_WAKE: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      if (!radioStandby && radioReady) { out[0] = 1; outLen = 1; break; }
      if (!radioWake()) { error = ERR_RADIO_FAILURE; break; }
      out[0] = 0; outLen = 1; emitStateChanged(); break;
    }

    case OP_SYSTEM_SLEEP: {
      if (n != 4) { error = ERR_BAD_ARGUMENT; break; }
      const uint32_t ms = getU32(p);
      if (ms < 1000 || ms > 7UL * 24 * 60 * 60 * 1000) { error = ERR_BAD_ARGUMENT; break; }
      if (txnActive) { error = ERR_BUSY; break; }
      rangingEnabled = false; radioOff(); cancelQueuedJobs();
      if (radioReady && !radioStandby) radioSetStandby();
      putU32(out, ms); outLen = 4;
      sendResponse(source, q, ERR_OK, out, outLen);
      if (source.io) source.io->flush();
      delay(20);
      esp_sleep_enable_timer_wakeup(uint64_t(ms) * 1000ULL);
      esp_deep_sleep_start();
      return;
    }

    case OP_REBOOT: {
      if (n != 0) { error = ERR_BAD_ARGUMENT; break; }
      sendResponse(source, q, ERR_OK, nullptr, 0);
      if (source.io) source.io->flush();
      delay(20); ESP.restart(); return;
    }

    default:
      error = ERR_UNKNOWN_COMMAND; break;
  }
  sendResponse(source, q, error, out, outLen);
}

static void feedTransport(HostTransport &transport) {
  if (!transport.io) return;
  // Bound work per endpoint so a continuous stream on one link cannot starve the other.
  size_t byteBudget = sizeof(transport.encoded);
  while (byteBudget-- && transport.io->available()) {
    const uint8_t b = static_cast<uint8_t>(transport.io->read());
    if (b == 0) {
      if (!transport.droppingFrame && transport.encodedLen) {
        Frame f;
        if (decodeFrame(transport.encoded, transport.encodedLen, f)) processRequest(transport, f);
        else { stats.protocolErrors++; rememberError(ERR_BAD_ARGUMENT); }
      }
      transport.encodedLen = 0;
      transport.droppingFrame = false;
    } else if (!transport.droppingFrame) {
      if (transport.encodedLen < sizeof(transport.encoded)) transport.encoded[transport.encodedLen++] = b;
      else {
        transport.encodedLen = 0;
        transport.droppingFrame = true;
        stats.uartOverflows++; // Protocol counter retained; applies to either host transport.
      }
    }
  }
}

static void processBleEvents() {
  UwbBle::Event event;
  while (ble.popEvent(event)) {
    uint8_t opcode = 0;
    switch (event.code) {
      case UwbBle::EVENT_STATE_CHANGED: opcode = EV_BLE_STATE_CHANGED; break;
      case UwbBle::EVENT_PEER_FOUND: opcode = EV_BLE_PEER_FOUND; break;
      case UwbBle::EVENT_PEER_CONNECTED: opcode = EV_BLE_PEER_CONNECTED; break;
      case UwbBle::EVENT_PEER_DISCONNECTED: opcode = EV_BLE_PEER_DISCONNECTED; break;
      case UwbBle::EVENT_CONNECTION_FAILED: opcode = EV_BLE_CONNECTION_FAILED; break;
      case UwbBle::EVENT_DATA_RECEIVED: opcode = EV_BLE_DATA_RECEIVED; break;
      case UwbBle::EVENT_ERROR: opcode = EV_BLE_ERROR; break;
      case UwbBle::EVENT_DATA_SENT: opcode = EV_BLE_DATA_SENT; break;
      case UwbBle::EVENT_DATA_EXPIRED: opcode = EV_BLE_DATA_EXPIRED; break;
      default: break;
    }
    if (event.code == UwbBle::EVENT_ERROR && event.length >= 7) {
      const uint16_t peerId = static_cast<uint16_t>(event.payload[1]) |
                              (static_cast<uint16_t>(event.payload[2]) << 8);
      const uint16_t messageId = static_cast<uint16_t>(event.payload[3]) |
                                 (static_cast<uint16_t>(event.payload[4]) << 8);
      rememberError(event.payload[6], ERROR_SUBSYSTEM_BLE, peerId,
                    uint32_t(messageId) | (uint32_t(event.payload[5]) << 16) |
                    (uint32_t(event.payload[0]) << 24));
    } else if (event.code == UwbBle::EVENT_CONNECTION_FAILED && event.length >= 6) {
      const uint16_t peerId = static_cast<uint16_t>(event.payload[0]) |
                              (static_cast<uint16_t>(event.payload[1]) << 8);
      const uint16_t detail = static_cast<uint16_t>(event.payload[4]) |
                              (static_cast<uint16_t>(event.payload[5]) << 8);
      const uint32_t context = uint32_t(event.payload[3]) | (uint32_t(detail) << 8);
      rememberError(event.payload[2], ERROR_SUBSYSTEM_BLE, peerId, context);
    } else if (event.code == UwbBle::EVENT_DATA_EXPIRED && event.length >= 4) {
      const uint16_t peerId = static_cast<uint16_t>(event.payload[0]) |
                              (static_cast<uint16_t>(event.payload[1]) << 8);
      const uint16_t messageId = static_cast<uint16_t>(event.payload[2]) |
                                 (static_cast<uint16_t>(event.payload[3]) << 8);
      rememberError(1, ERROR_SUBSYSTEM_BLE, peerId, messageId);
    }
    if (opcode) emitEvent(opcode, event.payload, event.length);
  }
}

void setup() {
  // Serial is USB CDC when built with CDCOnBoot=cdc. Do not print text to either
  // endpoint: both carry the same binary COBS protocol, not diagnostic text.
  pinMode(PIN_DWM_CS, OUTPUT); digitalWrite(PIN_DWM_CS, HIGH);
  pinMode(PIN_DWM_WAKE, OUTPUT); digitalWrite(PIN_DWM_WAKE, LOW);
  pinMode(PIN_DWM_RST, OUTPUT_OPEN_DRAIN); digitalWrite(PIN_DWM_RST, HIGH); // release reset
  pinMode(PIN_DWM_IRQ, INPUT);

  // Configure RX queues before begin(): both Arduino transports otherwise
  // default to 256 bytes, smaller than a maximum-length encoded protocol frame.
  const size_t usbRxSize = Serial.setRxBufferSize(HOST_RX_BUFFER_SIZE);
  const size_t uartRxSize = PiSerial.setRxBufferSize(HOST_RX_BUFFER_SIZE);
  Serial.begin(USB_CDC_BAUD);
  PiSerial.begin(UART_BAUD, SERIAL_8N1, PIN_PI_RX, PIN_PI_TX);
  if (usbRxSize != HOST_RX_BUFFER_SIZE || uartRxSize != HOST_RX_BUFFER_SIZE)
    rememberError(ERR_INTERNAL);
  usbSessionObservedConnected = usbCdcConnected();
  UsbTransport.io = &Serial;
  UsbTransport.name = "USB CDC";
  UartTransport.io = &PiSerial;
  UartTransport.name = "UART1";
  loadConfig();
  if (!ble.begin(cfg.deviceId) || !ble.configure(bleCfg)) {
    rememberError(ERR_INTERNAL);
  } else if (bleCfg.autoStart && bleCfg.mode != UwbBle::MODE_IDLE && !ble.start()) {
    rememberError(ERR_INTERNAL);
  }

  if (!radioInitialize()) rememberError(ERR_RADIO_FAILURE, ERROR_SUBSYSTEM_UWB);
  for (uint8_t i = 0; i < targetCount; ++i) targetDueMs[i] = millis();
  // Start from the persistent boot policy, not from any prior runtime test state.
  if (radioReady && autoStartRanging && role != ROLE_IDLE &&
      (role != ROLE_INITIATOR || targetCount > 0)) {
    rangingEnabled = true;
    if (role == ROLE_RESPONDER && !armRx(RS_RX_LISTEN)) rangingEnabled = false;
  }
  Frame ready; ready.type = TYPE_EVENT; ready.seq = ++eventSeq; ready.opcode = EV_DEVICE_READY;
  ready.payload[0] = radioReady ? 1 : 0; ready.payload[1] = VERSION; ready.length = 2;
  // Forced once on each interface so either host can identify the node at boot.
  sendEventFrameTo(UsbTransport, ready);
  sendFrameTo(UartTransport, ready);
}

void loop() {
  // Detect CDC session closure before radio processing can publish another event.
  pollUsbConnection();
  feedTransport(UsbTransport);
  feedTransport(UartTransport);
  ble.poll();
  processBleEvents();
  if (radioReady && !radioStandby) {
    radioPoll();
    radioScheduler();
  }
}
