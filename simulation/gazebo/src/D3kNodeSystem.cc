#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <poll.h>
#include <pty.h>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <termios.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include <gz/common/Console.hh>
#include <gz/plugin/Register.hh>
#include <gz/sim/Model.hh>
#include <gz/sim/System.hh>
#include <gz/sim/Util.hh>
#include <gz/math/Vector3.hh>
#include <sdf/Element.hh>

#include "Protocol.h"
#include "BleProtocolModel.hh"

namespace d3k_sim {
class D3kNodeSystem;
namespace {

using SimNs = std::chrono::nanoseconds;
constexpr uint8_t kErrOk = 0;
constexpr uint8_t kErrBadArgument = 1;
constexpr uint8_t kErrUnknownCommand = 2;
constexpr uint8_t kErrBadState = 3;
constexpr uint8_t kErrBusy = 4;
constexpr uint8_t kErrNotSupported = 5;
constexpr uint8_t kErrNotFound = 10;
constexpr uint8_t kRoleIdle = 0;
constexpr uint8_t kRoleInitiator = 1;
constexpr uint8_t kRoleResponder = 2;
constexpr uint8_t kRangeValid = 0;
constexpr uint8_t kRangeTimeout = 1;
constexpr uint8_t kRangeInvalid = 2;
constexpr uint8_t kRangeCancelled = 3;
constexpr uint8_t kRangeRadioError = 4;
constexpr uint16_t kHistoryCapacity = 256;
constexpr uint8_t kMaxTargets = 16;
constexpr uint8_t kMaxBlePeers = 8;
constexpr uint16_t kMaxBleMessageLength = 496;
constexpr uint16_t kBleMtu = 256;
constexpr uint16_t kDefaultBleMtu = 23;
constexpr uint16_t kUartMaxPayload = 500;
constexpr size_t kBleFragmentPayload = kBleMtu - 12;
constexpr uint16_t kDefaultPanId = 0xDECA;
constexpr uint8_t kDefaultChannel = 5;

std::mutex &NetworkMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<uint16_t, D3kNodeSystem *> &NetworkNodes() {
  static std::map<uint16_t, D3kNodeSystem *> nodes;
  return nodes;
}

int64_t ToNs(const std::chrono::steady_clock::duration &duration) {
  return std::chrono::duration_cast<SimNs>(duration).count();
}

uint64_t ToUs(int64_t ns) {
  return ns <= 0 ? 0 : static_cast<uint64_t>(ns / 1000);
}

template <typename T>
T SdfValue(const sdf::ElementPtr &parent, const std::string &name, const T &fallback) {
  if (!parent || !parent->HasElement(name)) return fallback;

  // SDFormat 14's ParamValue variant supports int, unsigned int, and
  // unsigned long, but not fixed-width uint8_t / uint16_t types directly.
  // Read into a supported type, then validate before narrowing.
  if constexpr (std::is_same_v<T, uint8_t> ||
                std::is_same_v<T, uint16_t> ||
                std::is_same_v<T, uint32_t>) {
    const unsigned int value = parent->Get<unsigned int>(name);
    if (value > static_cast<unsigned int>(std::numeric_limits<T>::max())) {
      throw std::runtime_error("SDF value '" + name + "' is out of range");
    }
    return static_cast<T>(value);
  } else if constexpr (std::is_same_v<T, uint64_t>) {
    const unsigned long value = parent->Get<unsigned long>(name);
    if (value > static_cast<unsigned long>(std::numeric_limits<T>::max())) {
      throw std::runtime_error("SDF value '" + name + "' is out of range");
    }
    return static_cast<T>(value);
  } else {
    return parent->Get<T>(name);
  }
}

sdf::ElementPtr Child(const sdf::ElementPtr &parent, const std::string &name) {
  return parent && parent->HasElement(name) ? parent->GetElement(name) : sdf::ElementPtr{};
}

std::vector<uint16_t> ParseIds(const std::string &text) {
  std::vector<uint16_t> result;
  std::string token;
  std::stringstream stream(text);
  while (std::getline(stream, token, ',')) {
    std::stringstream number(token);
    unsigned int value = 0;
    if (!(number >> value) || value > 65535) throw std::runtime_error("invalid uint16 ID list: " + text);
    if (value == 0 || value == 65535) throw std::runtime_error("IDs must be in 1..65534");
    result.push_back(static_cast<uint16_t>(value));
  }
  if (result.size() > kMaxTargets) throw std::runtime_error("too many IDs in target/peer list");
  std::sort(result.begin(), result.end());
  if (std::adjacent_find(result.begin(), result.end()) != result.end())
    throw std::runtime_error("target/peer IDs must be unique");
  return result;
}

bool Contains(const std::vector<uint16_t> &values, uint16_t value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

uint8_t ParseRole(const std::string &value) {
  if (value == "idle") return kRoleIdle;
  if (value == "initiator") return kRoleInitiator;
  if (value == "responder") return kRoleResponder;
  throw std::runtime_error("UWB role must be idle, initiator, or responder");
}

uint8_t ParseBleMode(const std::string &value) {
  if (value == "idle") return 0;
  if (value == "hub" || value == "central") return 1;
  if (value == "peripheral") return 2;
  throw std::runtime_error("BLE mode must be idle, hub, or peripheral");
}

void AppendU16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

void AppendU32(std::vector<uint8_t> &out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void AppendU64(std::vector<uint8_t> &out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void AppendI16(std::vector<uint8_t> &out, int16_t value) {
  AppendU16(out, static_cast<uint16_t>(value));
}

bool ReadU16(const uint8_t *data, size_t length, size_t offset, uint16_t &value) {
  if (offset + 2 > length) return false;
  value = UwbProto::getU16(data + offset);
  return true;
}

bool ReadU32(const uint8_t *data, size_t length, size_t offset, uint32_t &value) {
  if (offset + 4 > length) return false;
  value = UwbProto::getU32(data + offset);
  return true;
}

bool ReadU64(const uint8_t *data, size_t length, size_t offset, uint64_t &value) {
  if (offset + 8 > length) return false;
  value = UwbProto::getU64(data + offset);
  return true;
}

struct DeviceConfig {
  uint16_t deviceId = 1;
  uint16_t panId = kDefaultPanId;
  uint8_t channel = kDefaultChannel;
  uint16_t txAntennaDelay = 16385;
  uint16_t rxAntennaDelay = 16385;
  uint32_t periodMs = 100;
  uint16_t timeoutMs = 120;
  uint8_t retries = 1;
  uint8_t role = kRoleIdle;
  std::vector<uint16_t> targets;
  bool autoStartRanging = false;
  uint8_t bleMode = 0;
  uint16_t bleAdvertisingIntervalMs = 100;
  uint16_t bleScanIntervalMs = 100;
  uint16_t bleScanWindowMs = 50;
  bool bleAutoStart = false;
  uint16_t bleRetryIntervalMs = 1000;
  uint16_t bleConnectionTimeoutMs = 5000;
  uint8_t bleMaxRetries = 3;
  uint16_t bleRxMessageTimeoutMs = 1000;
  std::vector<uint16_t> blePeers;
};

struct Measurement {
  uint32_t sampleSeq = 0;
  uint16_t targetId = 0;
  uint64_t monotonicUs = 0;
  uint64_t epochUs = 0;
  int32_t rangeMm = -1;
  uint8_t status = kRangeInvalid;
  uint8_t qualityFlags = 0;
};

struct RangeJob {
  uint16_t jobId = 0;
  uint16_t targetId = 0;
};

struct IncomingMessage {
  uint16_t messageId = 0;
  uint16_t peerId = 0;
  std::vector<uint8_t> data;
  std::chrono::steady_clock::time_point completedAt{};
};

struct PendingMessageResult {
  uint16_t messageId = 0;
  uint8_t status = 0;
  uint8_t retryCount = 0;
  bool initialAttemptPending = true;
  std::chrono::steady_clock::time_point lastActionAt{};
};

struct OutgoingMessage {
  uint16_t messageId = 0;
  uint16_t peerId = 0;
  std::vector<uint8_t> data;
  uint8_t fragmentCount = 1;
  uint8_t nextFragment = 0;
  uint8_t retryCount = 0;
  uint8_t resultRetryCount = 0;
  std::chrono::steady_clock::time_point lastFragmentAt{};
  bool waitingForConsumption = false;
};

struct CachedReply {
  uint16_t sequence = 0;
  uint8_t opcode = 0;
  uint16_t requestCrc = 0;
  UwbProto::Frame response;
};

bool IsValidIdList(const std::vector<uint16_t> &ids, size_t maxCount, uint16_t localId) {
  if (ids.size() > maxCount) return false;
  std::set<uint16_t> unique(ids.begin(), ids.end());
  return unique.size() == ids.size() &&
      std::all_of(ids.begin(), ids.end(), [localId](uint16_t id) {
        return id != 0 && id != 0xFFFF && id != localId;
      });
}

bool ValidateConfig(const DeviceConfig &config) {
  if (config.deviceId == 0 || config.deviceId == 0xFFFF) return false;
  if (config.channel != 5 && config.channel != 9) return false;
  if (config.periodMs < 20 || config.periodMs > 60000) return false;
  if (config.timeoutMs < 50 || config.timeoutMs > 5000 || config.retries > 5) return false;
  if (config.role > kRoleResponder || !IsValidIdList(config.targets, kMaxTargets, config.deviceId)) return false;
  if (config.bleMode > 2 || config.bleAdvertisingIntervalMs < 20 || config.bleAdvertisingIntervalMs > 10240)
    return false;
  if (config.bleScanIntervalMs < 10 || config.bleScanIntervalMs > 10240 ||
      config.bleScanWindowMs < 10 || config.bleScanWindowMs > config.bleScanIntervalMs) return false;
  if (config.bleRetryIntervalMs < 100 || config.bleRetryIntervalMs > 60000 ||
      config.bleConnectionTimeoutMs < 100 || config.bleConnectionTimeoutMs > 30000) return false;
  if (config.bleMaxRetries > 5 || config.bleRxMessageTimeoutMs < 100 || config.bleRxMessageTimeoutMs > 2000)
    return false;
  if (config.blePeers.size() > kMaxBlePeers || !IsValidIdList(config.blePeers, kMaxBlePeers, 0)) return false;
  if (config.autoStartRanging && (config.role == kRoleIdle ||
      (config.role == kRoleInitiator && config.targets.empty()))) return false;
  if (config.bleAutoStart && config.bleMode == 0) return false;
  return true;
}

std::vector<uint8_t> EncodeBleExtension(const DeviceConfig &config) {
  std::vector<uint8_t> result;
  result.reserve(16 + 2 * config.blePeers.size());
  result.push_back(config.bleMode);
  AppendU16(result, config.bleAdvertisingIntervalMs);
  AppendU16(result, config.bleScanIntervalMs);
  AppendU16(result, config.bleScanWindowMs);
  result.push_back(config.bleAutoStart ? 1 : 0);
  AppendU16(result, config.bleRetryIntervalMs);
  AppendU16(result, config.bleConnectionTimeoutMs);
  result.push_back(config.bleMaxRetries);
  AppendU16(result, config.bleRxMessageTimeoutMs);
  result.push_back(static_cast<uint8_t>(config.blePeers.size()));
  for (uint16_t peer : config.blePeers) AppendU16(result, peer);
  return result;
}

bool DecodeBleExtension(const uint8_t *data, size_t length, size_t offset, DeviceConfig &config) {
  if (offset + 16 > length) return false;
  config.bleMode = data[offset];
  config.bleAdvertisingIntervalMs = UwbProto::getU16(data + offset + 1);
  config.bleScanIntervalMs = UwbProto::getU16(data + offset + 3);
  config.bleScanWindowMs = UwbProto::getU16(data + offset + 5);
  if (data[offset + 7] > 1) return false;
  config.bleAutoStart = data[offset + 7] != 0;
  config.bleRetryIntervalMs = UwbProto::getU16(data + offset + 8);
  config.bleConnectionTimeoutMs = UwbProto::getU16(data + offset + 10);
  config.bleMaxRetries = data[offset + 12];
  config.bleRxMessageTimeoutMs = UwbProto::getU16(data + offset + 13);
  const uint8_t count = data[offset + 15];
  if (count > kMaxBlePeers || offset + 16 + 2 * count != length) return false;
  config.blePeers.clear();
  for (uint8_t i = 0; i < count; ++i)
    config.blePeers.push_back(UwbProto::getU16(data + offset + 16 + 2 * i));
  return true;
}

std::vector<uint8_t> EncodeRuntimeConfig(const DeviceConfig &config) {
  std::vector<uint8_t> result;
  AppendU16(result, config.deviceId);
  AppendU16(result, config.panId);
  result.push_back(config.channel);
  AppendU16(result, config.txAntennaDelay);
  AppendU16(result, config.rxAntennaDelay);
  AppendU32(result, config.periodMs);
  AppendU16(result, config.timeoutMs);
  result.push_back(config.retries);
  auto ble = EncodeBleExtension(config);
  result.insert(result.end(), ble.begin(), ble.end());
  return result;
}

std::vector<uint8_t> EncodeSavedConfig(const DeviceConfig &config) {
  std::vector<uint8_t> result;
  AppendU16(result, config.deviceId);
  AppendU16(result, config.panId);
  result.push_back(config.channel);
  AppendU16(result, config.txAntennaDelay);
  AppendU16(result, config.rxAntennaDelay);
  AppendU32(result, config.periodMs);
  AppendU16(result, config.timeoutMs);
  result.push_back(config.retries);
  result.push_back(config.role);
  result.push_back(static_cast<uint8_t>(config.targets.size()));
  for (uint16_t target : config.targets) AppendU16(result, target);
  result.push_back(config.autoStartRanging ? 1 : 0);
  auto ble = EncodeBleExtension(config);
  result.insert(result.end(), ble.begin(), ble.end());
  return result;
}

std::vector<uint8_t> EncodeMeasurement(const Measurement &measurement) {
  std::vector<uint8_t> result;
  result.reserve(28);
  AppendU32(result, measurement.sampleSeq);
  AppendU16(result, measurement.targetId);
  AppendU64(result, measurement.monotonicUs);
  AppendU64(result, measurement.epochUs);
  AppendU32(result, static_cast<uint32_t>(measurement.rangeMm));
  result.push_back(measurement.status);
  result.push_back(measurement.qualityFlags);
  return result;
}

}  // namespace

class D3kNodeSystem final : public gz::sim::System,
                            public gz::sim::ISystemConfigure,
                            public gz::sim::ISystemPostUpdate {
 public:
  D3kNodeSystem() = default;
  ~D3kNodeSystem() override { Shutdown(); }

  void Configure(const gz::sim::Entity &entity,
                 const std::shared_ptr<const sdf::Element> &sdf,
                 gz::sim::EntityComponentManager &ecm,
                 gz::sim::EventManager &) override {
    std::lock_guard<std::mutex> lock(NetworkMutex());
    modelEntity_ = entity;
    modelName_ = gz::sim::Model(entity).Name(ecm);
    if (modelName_.empty()) modelName_ = "d3k_node";

    try {
      LoadSdfConfig(sdf);
      ResolveReferenceEntity(ecm);
      OpenPty(sdf);
      if (NetworkNodes().count(runtime_.deviceId) != 0) {
        throw std::runtime_error("duplicate simulated D3K device_id " + std::to_string(runtime_.deviceId));
      }
      NetworkNodes()[runtime_.deviceId] = this;
      registered_ = true;
      running_.store(true);
      ioThread_ = std::thread(&D3kNodeSystem::IoLoop, this);
      lastSimTimeNs_ = 0;
      nextPeriodicNs_.clear();
      if (runtime_.autoStartRanging) {
        rangingEnabled_ = true;
        ScheduleNextRanges(0);
      }
      if (runtime_.bleAutoStart) bleEnabled_ = true;
      EmitStateChanged();
      EmitBleStateChanged();
      EmitEvent(0x85, std::vector<uint8_t>{static_cast<uint8_t>(radioReady_ ? 1 : 0), 1});
      gzmsg << "D3K simulated node '" << modelName_ << "' device_id=" << runtime_.deviceId
            << " UART PTY=" << uartPath_ << "\n";
      gzmsg << "D3K simulated node '" << modelName_ << "' ranging bias=" << nodeBiasM_
            << " m, measurement noise sigma=" << noiseStddevM_ << " m\n";
    } catch (const std::exception &error) {
      gzerr << "D3K simulator plugin on model '" << modelName_ << "' failed to configure: "
            << error.what() << "\n";
      ShutdownLocked();
    }
  }

  void PostUpdate(const gz::sim::UpdateInfo &info,
                  const gz::sim::EntityComponentManager &ecm) override {
    if (!registered_ || info.paused) return;
    std::lock_guard<std::mutex> lock(NetworkMutex());
    const auto pose = gz::sim::worldPose(referenceEntity_, ecm);
    worldPosition_ = pose.Pos();
    poseValid_ = true;
    const int64_t nowNs = ToNs(info.simTime);
    lastSimTimeNs_ = nowNs;
    nowSimTimeNs_ = nowNs;

    static int64_t lastNetworkTickNs = std::numeric_limits<int64_t>::min();
    if (nowNs < lastNetworkTickNs) lastNetworkTickNs = std::numeric_limits<int64_t>::min();
    if (nowNs == lastNetworkTickNs) return;
    lastNetworkTickNs = nowNs;
    TickNetwork(nowNs);
  }

 private:
  struct PeerConnection {
    bool found = false;
    bool connected = false;
  };

  struct IncomingKey {
    uint16_t peerId;
    uint16_t messageId;
    bool operator<(const IncomingKey &other) const {
      return std::tie(peerId, messageId) < std::tie(other.peerId, other.messageId);
    }
  };

  void LoadSdfConfig(const std::shared_ptr<const sdf::Element> &root) {
    sdf::ElementPtr rootPtr = std::const_pointer_cast<sdf::Element>(root);
    auto nvs = Child(rootPtr, "nvs");
    auto uwb = Child(nvs, "uwb");
    auto ble = Child(nvs, "ble");

    saved_.deviceId = SdfValue<uint16_t>(uwb, "device_id", 1);
    saved_.panId = SdfValue<uint16_t>(uwb, "pan_id", kDefaultPanId);
    saved_.channel = SdfValue<uint8_t>(uwb, "channel", kDefaultChannel);
    saved_.txAntennaDelay = SdfValue<uint16_t>(uwb, "tx_antenna_delay", 16385);
    saved_.rxAntennaDelay = SdfValue<uint16_t>(uwb, "rx_antenna_delay", 16385);
    saved_.periodMs = SdfValue<uint32_t>(uwb, "period_ms", 100);
    saved_.timeoutMs = SdfValue<uint16_t>(uwb, "timeout_ms", 120);
    saved_.retries = SdfValue<uint8_t>(uwb, "retries", 1);
    saved_.role = ParseRole(SdfValue<std::string>(uwb, "role", "idle"));
    saved_.targets = ParseIds(SdfValue<std::string>(uwb, "targets", ""));
    saved_.autoStartRanging = SdfValue<bool>(uwb, "auto_start", false);

    saved_.bleMode = ParseBleMode(SdfValue<std::string>(ble, "mode", "idle"));
    saved_.bleAdvertisingIntervalMs = SdfValue<uint16_t>(ble, "advertising_interval_ms", 100);
    saved_.bleScanIntervalMs = SdfValue<uint16_t>(ble, "scan_interval_ms", 100);
    saved_.bleScanWindowMs = SdfValue<uint16_t>(ble, "scan_window_ms", 50);
    saved_.bleAutoStart = SdfValue<bool>(ble, "auto_start", false);
    saved_.bleRetryIntervalMs = SdfValue<uint16_t>(ble, "retry_interval_ms", 1000);
    saved_.bleConnectionTimeoutMs = SdfValue<uint16_t>(ble, "connection_timeout_ms", 5000);
    saved_.bleMaxRetries = SdfValue<uint8_t>(ble, "max_retries", 3);
    saved_.bleRxMessageTimeoutMs = SdfValue<uint16_t>(ble, "rx_message_timeout_ms", 1000);
    saved_.blePeers = ParseIds(SdfValue<std::string>(ble, "peer_ids", ""));
    if (!ValidateConfig(saved_)) throw std::runtime_error("SDF NVS configuration violates firmware limits");
    runtime_ = saved_;

    auto uart = Child(rootPtr, "uart");
    uartPathPreference_ = SdfValue<std::string>(uart, "pty_path", "");
    const uint32_t baud = SdfValue<uint32_t>(uart, "baud_rate", 460800);
    if (baud != 460800) gzwarn << "D3K PTY ignores baud_rate=" << baud << "; host protocol is normally configured for 460800 baud\n";

    referenceLinkName_ = SdfValue<std::string>(rootPtr, "reference_link", "");
    auto noise = Child(rootPtr, "range_model");
    noiseStddevM_ = SdfValue<double>(noise, "noise_stddev_m", 0.015);
    nodeBiasLimitM_ = SdfValue<double>(noise, "node_bias_limit_m", 0.03);
    maxRangeM_ = SdfValue<double>(noise, "max_range_m", 100.0);
    const uint32_t seed = SdfValue<uint32_t>(noise, "seed", 0);
    if (!std::isfinite(noiseStddevM_) || noiseStddevM_ < 0.0 || noiseStddevM_ > 1.0)
      throw std::runtime_error("range_model.noise_stddev_m must be finite and in [0, 1]");
    if (!std::isfinite(nodeBiasLimitM_) || nodeBiasLimitM_ < 0.0 || nodeBiasLimitM_ > 1.0)
      throw std::runtime_error("range_model.node_bias_limit_m must be finite and in [0, 1]");
    if (!std::isfinite(maxRangeM_) || maxRangeM_ <= 0.0)
      throw std::runtime_error("range_model.max_range_m must be positive");

    const uint32_t effectiveSeed = seed == 0
        ? (0xD3B1E5u ^ (static_cast<uint32_t>(runtime_.deviceId) * 2654435761u))
        : (seed ^ (static_cast<uint32_t>(runtime_.deviceId) * 2654435761u));
    random_.seed(effectiveSeed);
    std::uniform_real_distribution<double> biasDistribution(-nodeBiasLimitM_, nodeBiasLimitM_);
    nodeBiasM_ = SdfValue<double>(noise, "bias_m", biasDistribution(random_));
    if (!std::isfinite(nodeBiasM_)) throw std::runtime_error("range_model.bias_m must be finite");
    noiseDistribution_ = std::normal_distribution<double>(0.0, noiseStddevM_);
  }

  void ResolveReferenceEntity(gz::sim::EntityComponentManager &ecm) {
    referenceEntity_ = modelEntity_;
    if (!referenceLinkName_.empty()) {
      const auto link = gz::sim::Model(modelEntity_).LinkByName(ecm, referenceLinkName_);
      if (link == gz::sim::kNullEntity)
        throw std::runtime_error("reference_link '" + referenceLinkName_ + "' was not found in model '" + modelName_ + "'");
      referenceEntity_ = link;
    }
  }

  void OpenPty(const std::shared_ptr<const sdf::Element> &) {
    int slaveFd = -1;
    std::array<char, 256> slaveName{};
    if (::openpty(&masterFd_, &slaveFd, slaveName.data(), nullptr, nullptr) != 0)
      throw std::runtime_error("openpty failed: " + std::string(std::strerror(errno)));

    termios settings{};
    if (::tcgetattr(slaveFd, &settings) == 0) {
      ::cfmakeraw(&settings);
      ::cfsetispeed(&settings, B460800);
      ::cfsetospeed(&settings, B460800);
      ::tcsetattr(slaveFd, TCSANOW, &settings);
    }
    ::close(slaveFd);
    const int flags = ::fcntl(masterFd_, F_GETFL, 0);
    if (flags >= 0) ::fcntl(masterFd_, F_SETFL, flags | O_NONBLOCK);

    if (!uartPathPreference_.empty()) {
      const std::filesystem::path path(uartPathPreference_);
      if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
      std::error_code error;
      std::filesystem::remove(path, error);
      if (::symlink(slaveName.data(), uartPathPreference_.c_str()) != 0)
        throw std::runtime_error("could not create PTY symlink '" + uartPathPreference_ + "': " + std::strerror(errno));
      symlinkPath_ = uartPathPreference_;
      uartPath_ = uartPathPreference_;
    } else {
      uartPath_ = slaveName.data();
    }
  }

  void Shutdown() {
    running_.store(false);
    if (ioThread_.joinable()) ioThread_.join();
    std::lock_guard<std::mutex> lock(NetworkMutex());
    ShutdownLocked();
  }

  void ShutdownLocked() {
    if (registered_) {
      auto found = NetworkNodes().find(runtime_.deviceId);
      if (found != NetworkNodes().end() && found->second == this) NetworkNodes().erase(found);
      registered_ = false;
    }
    running_.store(false);
    if (masterFd_ >= 0) {
      ::close(masterFd_);
      masterFd_ = -1;
    }
    if (!symlinkPath_.empty()) {
      std::error_code error;
      std::filesystem::remove(symlinkPath_, error);
      symlinkPath_.clear();
    }
  }

  void IoLoop() {
    std::array<uint8_t, 1024> input{};
    std::vector<uint8_t> encoded;
    encoded.reserve(UwbProto::MAX_ENCODED);
    while (running_.load()) {
      pollfd descriptor{};
      descriptor.fd = masterFd_;
      descriptor.events = POLLIN;
      const int ready = ::poll(&descriptor, 1, 10);
      if (ready > 0 && (descriptor.revents & POLLIN)) {
        const ssize_t count = ::read(masterFd_, input.data(), input.size());
        if (count > 0) {
          for (ssize_t i = 0; i < count; ++i) {
            const uint8_t byte = input[static_cast<size_t>(i)];
            if (byte == 0) {
              if (!encoded.empty()) {
                UwbProto::Frame request;
                if (UwbProto::decodeFrame(encoded.data(), encoded.size(), request) &&
                    request.type == UwbProto::TYPE_REQUEST) {
                  std::lock_guard<std::mutex> lock(NetworkMutex());
                  ProcessRequestLocked(request);
                } else {
                  std::lock_guard<std::mutex> lock(NetworkMutex());
                  ++protocolErrors_;
                }
              }
              encoded.clear();
            } else if (encoded.size() < UwbProto::MAX_ENCODED) {
              encoded.push_back(byte);
            } else {
              encoded.clear();
              std::lock_guard<std::mutex> lock(NetworkMutex());
              ++uartOverflows_;
            }
          }
        }
      }
      {
        // The firmware services BLE from its MCU loop, independently of UWB.
        // Use the same elapsed-runtime clock for virtual fragments, receive
        // expiry, and MESSAGE_RESULT retries; do not depend on Gazebo stepping.
        std::lock_guard<std::mutex> lock(NetworkMutex());
        const auto now = std::chrono::steady_clock::now();
        UpdateBleConnectionsLocked();
        TickBleTransfersLocked(now);
        TickBleExpiryLocked(now);
        TickBleResultTransactionsLocked(now);
      }
      FlushOutbound();
    }
    FlushOutbound();
  }

  void QueueFrame(const UwbProto::Frame &frame) {
    std::array<uint8_t, UwbProto::MAX_ENCODED> encoded{};
    const size_t length = UwbProto::encodeFrame(frame, encoded.data(), encoded.size());
    if (length == 0) return;
    std::lock_guard<std::mutex> lock(outboundMutex_);
    outbound_.emplace_back(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(length));
  }

  void FlushOutbound() {
    if (masterFd_ < 0) return;
    std::lock_guard<std::mutex> lock(outboundMutex_);
    while (!outbound_.empty()) {
      auto &packet = outbound_.front();
      const ssize_t written = ::write(masterFd_, packet.data() + outboundOffset_, packet.size() - outboundOffset_);
      if (written > 0) {
        outboundOffset_ += static_cast<size_t>(written);
        if (outboundOffset_ == packet.size()) {
          outbound_.pop_front();
          outboundOffset_ = 0;
        }
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
      // EIO means no slave has the PTY open. Drop queued telemetry so a host
      // that connects later starts with current state rather than stale events.
      outbound_.clear();
      outboundOffset_ = 0;
      return;
    }
  }

  void ProcessRequestLocked(const UwbProto::Frame &request) {
    const uint16_t requestCrc = UwbProto::crc16(request.payload, request.length);
    for (const auto &cached : replyCache_) {
      if (cached.sequence == request.seq && cached.opcode == request.opcode && cached.requestCrc == requestCrc) {
        QueueFrame(cached.response);
        ++duplicateRequests_;
        return;
      }
    }

    uint8_t error = kErrOk;
    std::vector<uint8_t> payload;
    HandleCommandLocked(request, error, payload);
    UwbProto::Frame response;
    response.type = UwbProto::TYPE_RESPONSE;
    response.seq = request.seq;
    response.opcode = request.opcode;
    response.flags = error;
    response.length = static_cast<uint16_t>(std::min(payload.size(), UwbProto::MAX_PAYLOAD));
    if (response.length) std::memcpy(response.payload, payload.data(), response.length);
    replyCache_.push_back(CachedReply{request.seq, request.opcode, requestCrc, response});
    if (replyCache_.size() > 8) replyCache_.pop_front();
    QueueFrame(response);
  }

  void HandleCommandLocked(const UwbProto::Frame &request, uint8_t &error, std::vector<uint8_t> &out) {
    const uint8_t *p = request.payload;
    const size_t n = request.length;
    uint16_t value16 = 0;
    uint32_t value32 = 0;
    uint64_t value64 = 0;

    switch (request.opcode) {
      case 0x01:  // PING
        if (n != 0) { error = kErrBadArgument; break; }
        out = {'P', 'O', 'N', 'G'};
        break;
      case 0x02:  // GET_INFO
        if (n != 0) { error = kErrBadArgument; break; }
        out = {1, 0, 0, 1};
        AppendU16(out, runtime_.deviceId);
        out.push_back(kMaxTargets);
        AppendU16(out, kHistoryCapacity);
        out.push_back(1);  // DWM3000 radio type
        AppendU16(out, kUartMaxPayload);
        out.push_back(1);  // BLE supported
        out.push_back(0x07);  // IDLE, HUB, PERIPHERAL
        out.push_back(kMaxBlePeers);
        out.push_back(kMaxBlePeers);
        AppendU16(out, kMaxBleMessageLength);
        AppendU16(out, kBleMtu);
        out.push_back(1);  // custom BLE packet protocol version
        break;
      case 0x03:  // GET_STATUS
        if (n != 0) { error = kErrBadArgument; break; }
        out.push_back(runtime_.role);
        out.push_back(rangingEnabled_ ? 1 : 0);
        out.push_back(radioReady_ ? 1 : 0);
        out.push_back(radioStandby_ ? 1 : 0);
        out.push_back(static_cast<uint8_t>(runtime_.targets.size()));
        out.push_back(clockSynchronized_ ? 1 : 0);
        AppendU32(out, static_cast<uint32_t>(ToUs(lastSimTimeNs_) / 1000));
        AppendU32(out, rangesOk_);
        AppendU32(out, rangesFailed_);
        out.push_back(runtime_.bleMode);
        out.push_back(bleEnabled_ ? 1 : 0);
        out.push_back(connectedPeers_.empty() ? 0 : 1);
        AppendU16(out, connectedPeers_.empty() ? 0 : *connectedPeers_.begin());
        out.push_back(bleEnabled_ && runtime_.bleMode == 2 ? 1 : 0);
        out.push_back(bleEnabled_ && runtime_.bleMode == 1 ? 1 : 0);
        AppendU16(out, connectedPeers_.empty() ? kDefaultBleMtu : kBleMtu);
        break;
      case 0x04:  // GET_STATISTICS
        if (n != 0) { error = kErrBadArgument; break; }
        AppendU32(out, rangesOk_);
        AppendU32(out, rangesFailed_);
        AppendU32(out, rxErrors_);
        AppendU32(out, txErrors_);
        AppendU32(out, transactionTimeouts_);
        AppendU32(out, protocolErrors_);
        AppendU32(out, uartOverflows_);
        AppendU32(out, historyOverwrites_);
        AppendU32(out, malformedRadioFrames_);
        AppendU32(out, duplicateRequests_);
        AppendU32(out, bleMessagesSent_);
        AppendU32(out, bleMessagesReceived_);
        AppendU32(out, bleTxErrors_);
        AppendU32(out, bleRxErrors_);
        AppendU32(out, bleConnectionFailures_);
        AppendU32(out, bleRxQueueOverflows_);
        AppendU32(out, bleEventQueueOverflows_);
        break;
      case 0x05:  // CLEAR_STATISTICS
        if (n != 0) { error = kErrBadArgument; break; }
        rangesOk_ = rangesFailed_ = rxErrors_ = txErrors_ = transactionTimeouts_ = 0;
        protocolErrors_ = uartOverflows_ = historyOverwrites_ = malformedRadioFrames_ = 0;
        duplicateRequests_ = bleMessagesSent_ = bleMessagesReceived_ = bleTxErrors_ = 0;
        bleRxErrors_ = bleConnectionFailures_ = bleRxQueueOverflows_ = bleEventQueueOverflows_ = 0;
        break;
      case 0x06:  // GET_LAST_ERROR
        if (n != 0) { error = kErrBadArgument; break; }
        out.push_back(0);  // system/protocol subsystem
        out.push_back(0);
        AppendU64(out, 0);
        AppendU16(out, 0);
        AppendU32(out, 0);
        break;
      case 0x07:  // GET_DIAGNOSTICS
        if (n != 0) { error = kErrBadArgument; break; }
        AppendU32(out, 0);
        AppendU32(out, 0);
        AppendU32(out, malformedRadioFrames_);
        AppendU32(out, rxErrors_);
        AppendU32(out, txErrors_);
        out.push_back(radioStandby_ ? 1 : 0);
        out.push_back(radioStandby_ ? 1 : 0);
        out.push_back(runtime_.bleMode);
        out.push_back(bleEnabled_ ? 1 : 0);
        out.push_back(bleEnabled_ && runtime_.bleMode == 2 ? 1 : 0);
        out.push_back(bleEnabled_ && runtime_.bleMode == 1 ? 1 : 0);
        out.push_back(static_cast<uint8_t>(connectedPeers_.size()));
        out.push_back(static_cast<uint8_t>(incomingMessages_.size()));
        out.push_back(0);
        break;
      case 0x08:  // GET_TRACE
        if (n != 0) { error = kErrBadArgument; break; }
        out.push_back(0);  // no simulated radio register trace
        break;
      case 0x09:  // SUBSCRIBE
        if (!ReadU16(p, n, 0, value16) || n != 2) { error = kErrBadArgument; break; }
        subscriptionMask_ |= value16;
        AppendU16(out, subscriptionMask_);
        break;
      case 0x0A:  // UNSUBSCRIBE
        if (!ReadU16(p, n, 0, value16) || n != 2) { error = kErrBadArgument; break; }
        subscriptionMask_ &= static_cast<uint16_t>(~value16);
        AppendU16(out, subscriptionMask_);
        break;
      case 0x0B:  // TIME_SYNC
        if (!ReadU64(p, n, 0, value64) || n != 8) { error = kErrBadArgument; break; }
        epochOffsetUs_ = static_cast<int64_t>(value64) - static_cast<int64_t>(ToUs(lastSimTimeNs_));
        clockSynchronized_ = true;
        AppendU64(out, ToUs(lastSimTimeNs_));
        AppendU64(out, static_cast<uint64_t>(epochOffsetUs_));
        break;
      case 0x10:  // GET_CONFIG
        if (n != 0) { error = kErrBadArgument; break; }
        out = EncodeRuntimeConfig(runtime_);
        break;
      case 0x11:  // SET_CONFIG
        HandleSetConfig(p, n, error);
        break;
      case 0x12:  // RESET_CONFIG
        if (n != 0) { error = kErrBadArgument; break; }
        if (NetworkNodes().count(1) && NetworkNodes().at(1) != this) { error = kErrBusy; break; }
        {
          const uint16_t previousId = runtime_.deviceId;
          saved_ = DeviceConfig{};
          runtime_ = saved_;
          if (previousId != runtime_.deviceId) {
            NetworkNodes().erase(previousId);
            NetworkNodes()[runtime_.deviceId] = this;
          }
        }
        rangingEnabled_ = false;
        bleEnabled_ = false;
        radioStandby_ = false;
        incomingMessages_.clear();
        outgoingMessages_.clear();
        foundPeers_.clear();
        nextPeriodicNs_.clear();
        CancelQueuedJobsLocked();
        UpdateBleConnectionsLocked(lastSimTimeNs_);
        EmitStateChanged();
        EmitBleStateChanged();
        break;
      case 0x13:  // GET_SAVED_CONFIG
        if (n != 0) { error = kErrBadArgument; break; }
        out = EncodeSavedConfig(saved_);
        break;
      case 0x14:  // APPLY_SAVE_CONFIG
        HandleApplySaveConfig(p, n, error);
        break;
      case 0x15:  // GET_ANTENNA_DELAYS
        if (n != 0) { error = kErrBadArgument; break; }
        AppendU16(out, runtime_.txAntennaDelay);
        AppendU16(out, runtime_.rxAntennaDelay);
        break;
      case 0x16:  // SET_ANTENNA_DELAYS
        if (n != 4) { error = kErrBadArgument; break; }
        if (rangingEnabled_) { error = kErrBusy; break; }
        runtime_.txAntennaDelay = UwbProto::getU16(p);
        runtime_.rxAntennaDelay = UwbProto::getU16(p + 2);
        break;
      case 0x17:  // SAVE_ANTENNA_DELAYS
        if (n != 0) { error = kErrBadArgument; break; }
        if (rangingEnabled_) { error = kErrBusy; break; }
        saved_.txAntennaDelay = runtime_.txAntennaDelay;
        saved_.rxAntennaDelay = runtime_.rxAntennaDelay;
        AppendU16(out, saved_.txAntennaDelay);
        AppendU16(out, saved_.rxAntennaDelay);
        break;
      case 0x20:  // SET_ROLE
        if (n != 1 || p[0] > kRoleResponder) { error = kErrBadArgument; break; }
        CancelQueuedJobsLocked();
        runtime_.role = p[0];
        rangingEnabled_ = false;
        nextPeriodicNs_.clear();
        EmitStateChanged();
        break;
      case 0x21:  // GET_TARGETS
        if (n != 0) { error = kErrBadArgument; break; }
        out.push_back(static_cast<uint8_t>(runtime_.targets.size()));
        for (uint16_t target : runtime_.targets) AppendU16(out, target);
        break;
      case 0x22:  // SET_TARGETS
        HandleSetTargets(p, n, error, out);
        break;
      case 0x23:  // GET_UWB_SCHEDULE
        if (n != 0) { error = kErrBadArgument; break; }
        AppendU32(out, runtime_.periodMs);
        AppendU16(out, runtime_.timeoutMs);
        out.push_back(runtime_.retries);
        out.push_back(static_cast<uint8_t>(runtime_.targets.size()));
        out.push_back(static_cast<uint8_t>(rangeJobs_.size()));
        out.push_back(0);  // simulator does not model a radio transaction in flight
        AppendU32(out, runtime_.periodMs * std::max<uint32_t>(1, runtime_.targets.size()));
        AppendU32(out, 0);  // no radio transaction is simulated; ranges resolve in this tick
        break;
      case 0x24:  // SET_UWB_SCHEDULE
        if (n != 7) { error = kErrBadArgument; break; }
        {
          const uint32_t period = UwbProto::getU32(p);
          const uint16_t timeout = UwbProto::getU16(p + 4);
          const uint8_t retries = p[6];
          if (period < 20 || period > 60000 || timeout < 50 || timeout > 5000 || retries > 5) {
            error = kErrBadArgument;
            break;
          }
          runtime_.periodMs = period;
          runtime_.timeoutMs = timeout;
          runtime_.retries = retries;
          if (rangingEnabled_) {
            nextPeriodicNs_.clear();
            for (uint16_t target : runtime_.targets) nextPeriodicNs_[target] = lastSimTimeNs_;
          }
        }
        break;
      case 0x25:  // START_RANGING
        if (n != 0) { error = kErrBadArgument; break; }
        if (!radioReady_ || radioStandby_ || runtime_.role == kRoleIdle ||
            (runtime_.role == kRoleInitiator && runtime_.targets.empty() && rangeJobs_.empty())) {
          error = kErrBadState;
          break;
        }
        rangingEnabled_ = true;
        ScheduleNextRanges(lastSimTimeNs_);
        EmitStateChanged();
        break;
      case 0x26:  // STOP_RANGING
        if (n != 0) { error = kErrBadArgument; break; }
        rangingEnabled_ = false;
        CancelQueuedJobsLocked();
        nextPeriodicNs_.clear();
        EmitStateChanged();
        break;
      case 0x27:  // RANGE_ONCE
        if (!ReadU16(p, n, 0, value16) || n != 2 || value16 == 0 || value16 == 0xFFFF) {
          error = kErrBadArgument;
          break;
        }
        if (value16 == runtime_.deviceId) { error = kErrBadArgument; break; }
        if (!radioReady_ || radioStandby_ || runtime_.role != kRoleInitiator) {
          error = kErrBadState;
          break;
        }
        if (rangeJobs_.size() >= 8) { error = kErrBusy; break; }
        {
          const uint16_t jobId = AllocateJobId();
          rangeJobs_.push_back(RangeJob{jobId, value16});
          AppendU16(out, jobId);
        }
        break;
      case 0x28:  // CANCEL_OPERATION
        if (!ReadU16(p, n, 0, value16) || n != 2 || value16 == 0) { error = kErrBadArgument; break; }
        if (!CancelJobLocked(value16)) error = kErrNotFound;
        break;
      case 0x29:  // UWB_SLEEP
        if (n != 0) { error = kErrBadArgument; break; }
        rangingEnabled_ = false;
        CancelQueuedJobsLocked();
        nextPeriodicNs_.clear();
        radioStandby_ = true;
        nextPeriodicNs_.clear();
        out.push_back(1);
        EmitStateChanged();
        break;
      case 0x2A:  // UWB_WAKE
        if (n != 0) { error = kErrBadArgument; break; }
        out.push_back(radioStandby_ ? 0 : 1);
        radioStandby_ = false;
        radioReady_ = true;
        EmitStateChanged();
        break;
      case 0x30:  // GET_LAST_RANGE
        if (!ReadU16(p, n, 0, value16) || n != 2) { error = kErrBadArgument; break; }
        HandleGetLastRange(value16, out);
        break;
      case 0x31:  // GET_HISTORY
        HandleGetHistory(p, n, error, out);
        break;
      case 0x40:  // SYSTEM_SLEEP (not physically modeled)
        if (!ReadU32(p, n, 0, value32) || n != 4 || value32 < 1000 || value32 > 604800000) {
          error = kErrBadArgument;
          break;
        }
        error = kErrNotSupported;
        break;
      case 0x41:  // REBOOT
        if (n != 0) { error = kErrBadArgument; break; }
        if (NetworkNodes().count(saved_.deviceId) && NetworkNodes().at(saved_.deviceId) != this) { error = kErrBusy; break; }
        {
          const uint16_t previousId = runtime_.deviceId;
          runtime_ = saved_;
          if (previousId != runtime_.deviceId) {
            NetworkNodes().erase(previousId);
            NetworkNodes()[runtime_.deviceId] = this;
          }
        }
        rangingEnabled_ = runtime_.autoStartRanging;
        bleEnabled_ = false;
        radioReady_ = true;
        radioStandby_ = false;
        nextPeriodicNs_.clear();
        rangeJobs_.clear();
        incomingMessages_.clear();
        outgoingMessages_.clear();
        foundPeers_.clear();
        UpdateBleConnectionsLocked(lastSimTimeNs_);
        bleEnabled_ = runtime_.bleAutoStart;
        subscriptionMask_ = 0;
        replyCache_.clear();
        history_.clear();
        nextSampleSeq_ = 1;
        nextJobId_ = 1;
        nextMessageId_ = 1;
        eventSequence_ = 0;
        clockSynchronized_ = false;
        epochOffsetUs_ = 0;
        rangesOk_ = rangesFailed_ = rxErrors_ = txErrors_ = transactionTimeouts_ = 0;
        protocolErrors_ = uartOverflows_ = historyOverwrites_ = malformedRadioFrames_ = 0;
        duplicateRequests_ = bleMessagesSent_ = bleMessagesReceived_ = bleTxErrors_ = 0;
        bleRxErrors_ = bleConnectionFailures_ = bleRxQueueOverflows_ = bleEventQueueOverflows_ = 0;
        if (rangingEnabled_) ScheduleNextRanges(lastSimTimeNs_);
        EmitStateChanged();
        EmitBleStateChanged();
        EmitEvent(0x85, std::vector<uint8_t>{static_cast<uint8_t>(radioReady_ ? 1 : 0), 1});
        break;
      case 0x50:  // GET_PEERS
        if (n != 0) { error = kErrBadArgument; break; }
        out.push_back(static_cast<uint8_t>(saved_.blePeers.size()));
        for (uint16_t peer : saved_.blePeers) AppendU16(out, peer);
        break;
      case 0x51:  // SET_PEERS
        HandleSetPeers(p, n, error);
        break;
      case 0x52:  // START_BLE
        if (n != 0) { error = kErrBadArgument; break; }
        if (runtime_.bleMode == 0) { error = kErrBadState; break; }
        bleEnabled_ = true;
        EmitBleStateChanged();
        break;
      case 0x53:  // STOP_BLE
        if (n != 0) { error = kErrBadArgument; break; }
        bleEnabled_ = false;
        EmitBleStateChanged();
        break;
      case 0x54:  // GET_PEER_STATUS
        HandleGetPeerStatus(p, n, error, out);
        break;
      case 0x55:  // SEND_TO_PEER
        HandleSendToPeer(p, n, error, out);
        break;
      case 0x56:  // GET_RECEIVED_DATA
        HandleGetReceivedData(p, n, error, out);
        break;
      default:
        error = kErrUnknownCommand;
        break;
    }
  }

  void HandleSetConfig(const uint8_t *p, size_t n, uint8_t &error) {
    if (n < 25) { error = kErrBadArgument; return; }
    if (rangingEnabled_ || bleEnabled_ || !connectedPeers_.empty()) { error = kErrBusy; return; }
    DeviceConfig updated = runtime_;
    updated.deviceId = UwbProto::getU16(p);
    updated.panId = UwbProto::getU16(p + 2);
    updated.channel = p[4];
    updated.txAntennaDelay = UwbProto::getU16(p + 5);
    updated.rxAntennaDelay = UwbProto::getU16(p + 7);
    if (!DecodeBleExtension(p, n, 9, updated) || !ValidateConfig(updated)) {
      error = kErrBadArgument;
      return;
    }
    if (NetworkNodes().count(updated.deviceId) && updated.deviceId != runtime_.deviceId) {
      error = kErrBusy;
      return;
    }
    const uint16_t previousId = runtime_.deviceId;
    runtime_.deviceId = updated.deviceId;
    runtime_.panId = updated.panId;
    runtime_.channel = updated.channel;
    runtime_.txAntennaDelay = updated.txAntennaDelay;
    runtime_.rxAntennaDelay = updated.rxAntennaDelay;
    runtime_.bleMode = updated.bleMode;
    runtime_.bleAdvertisingIntervalMs = updated.bleAdvertisingIntervalMs;
    runtime_.bleScanIntervalMs = updated.bleScanIntervalMs;
    runtime_.bleScanWindowMs = updated.bleScanWindowMs;
    runtime_.bleAutoStart = updated.bleAutoStart;
    runtime_.bleRetryIntervalMs = updated.bleRetryIntervalMs;
    runtime_.bleConnectionTimeoutMs = updated.bleConnectionTimeoutMs;
    runtime_.bleMaxRetries = updated.bleMaxRetries;
    runtime_.bleRxMessageTimeoutMs = updated.bleRxMessageTimeoutMs;
    runtime_.blePeers = updated.blePeers;
    if (previousId != runtime_.deviceId) {
      NetworkNodes().erase(previousId);
      NetworkNodes()[runtime_.deviceId] = this;
    }
  }

  void HandleApplySaveConfig(const uint8_t *p, size_t n, uint8_t &error) {
    if (n < 35) { error = kErrBadArgument; return; }
    if (rangingEnabled_ || !rangeJobs_.empty() || bleEnabled_ || !connectedPeers_.empty()) {
      error = kErrBusy;
      return;
    }
    DeviceConfig updated = saved_;
    updated.deviceId = UwbProto::getU16(p);
    updated.panId = UwbProto::getU16(p + 2);
    updated.channel = p[4];
    updated.txAntennaDelay = UwbProto::getU16(p + 5);
    updated.rxAntennaDelay = UwbProto::getU16(p + 7);
    updated.periodMs = UwbProto::getU32(p + 9);
    updated.timeoutMs = UwbProto::getU16(p + 13);
    updated.retries = p[15];
    updated.role = p[16];
    const uint8_t targetCount = p[17];
    const size_t bleOffset = 19 + 2 * targetCount;
    if (targetCount > kMaxTargets || bleOffset + 16 > n) { error = kErrBadArgument; return; }
    updated.targets.clear();
    for (uint8_t i = 0; i < targetCount; ++i) updated.targets.push_back(UwbProto::getU16(p + 18 + 2 * i));
    const uint8_t autoStartRanging = p[18 + 2 * targetCount];
    if (autoStartRanging > 1) { error = kErrBadArgument; return; }
    updated.autoStartRanging = autoStartRanging != 0;
    if (!DecodeBleExtension(p, n, bleOffset, updated) || !ValidateConfig(updated)) {
      error = kErrBadArgument;
      return;
    }
    if (NetworkNodes().count(updated.deviceId) && updated.deviceId != runtime_.deviceId) {
      error = kErrBusy;
      return;
    }
    const uint16_t previousId = runtime_.deviceId;
    saved_ = updated;
    runtime_ = updated;
    if (previousId != runtime_.deviceId) {
      NetworkNodes().erase(previousId);
      NetworkNodes()[runtime_.deviceId] = this;
    }
    nextPeriodicNs_.clear();
  }

  void HandleSetTargets(const uint8_t *p, size_t n, uint8_t &error, std::vector<uint8_t> &out) {
    if (n < 1) { error = kErrBadArgument; return; }
    const uint8_t count = p[0];
    if (count > kMaxTargets || n != static_cast<size_t>(1 + 2 * count)) { error = kErrBadArgument; return; }
    std::vector<uint16_t> targets;
    for (uint8_t i = 0; i < count; ++i) targets.push_back(UwbProto::getU16(p + 1 + 2 * i));
    if (!IsValidIdList(targets, kMaxTargets, runtime_.deviceId)) { error = kErrBadArgument; return; }
    runtime_.targets = targets;
    nextPeriodicNs_.clear();
    if (rangingEnabled_ && runtime_.role == kRoleInitiator) {
      for (uint16_t target : runtime_.targets) nextPeriodicNs_[target] = lastSimTimeNs_;
    }
    out.push_back(count);
    for (uint16_t target : targets) AppendU16(out, target);
  }

  void HandleSetPeers(const uint8_t *p, size_t n, uint8_t &error) {
    if (n < 1) { error = kErrBadArgument; return; }
    const uint8_t count = p[0];
    if (count > kMaxBlePeers || n != static_cast<size_t>(1 + 2 * count)) { error = kErrBadArgument; return; }
    if (bleEnabled_ || !connectedPeers_.empty()) { error = kErrBusy; return; }
    std::vector<uint16_t> peers;
    for (uint8_t i = 0; i < count; ++i) peers.push_back(UwbProto::getU16(p + 1 + 2 * i));
    if (!IsValidIdList(peers, kMaxBlePeers, runtime_.deviceId)) { error = kErrBadArgument; return; }
    saved_.blePeers = peers;
    runtime_.blePeers = peers;
  }

  void HandleGetPeerStatus(const uint8_t *p, size_t n, uint8_t &error, std::vector<uint8_t> &out) {
    uint16_t peerId = 0;
    if (!ReadU16(p, n, 0, peerId) || n != 2 || peerId == 0 || peerId == 0xFFFF) {
      error = kErrBadArgument;
      return;
    }
    uint8_t state = 0;
    if (connectedPeers_.count(peerId)) state = 3;
    else if (foundPeers_.count(peerId)) state = 2;
    else if (runtime_.blePeers.empty() || Contains(runtime_.blePeers, peerId)) state = 1;
    if (state == 0) { error = kErrNotFound; return; }
    out.push_back(state);
    out.push_back(runtime_.bleMode);
    out.push_back(bleEnabled_ ? 1 : 0);
    out.push_back(bleEnabled_ && runtime_.bleMode == 2 ? 1 : 0);
    out.push_back(bleEnabled_ && runtime_.bleMode == 1 ? 1 : 0);
    out.push_back(state == 3 ? 1 : 0);
    AppendU16(out, peerId);
    AppendU16(out, state == 3 ? kBleMtu : kDefaultBleMtu);
    out.push_back(0);  // last failure reason
    out.push_back(0);  // last failure stage
    AppendI16(out, 0);
  }

  void HandleSendToPeer(const uint8_t *p, size_t n, uint8_t &error, std::vector<uint8_t> &out) {
    uint16_t peerId = 0, dataLength = 0;
    if (!ReadU16(p, n, 0, peerId) || !ReadU16(p, n, 2, dataLength) ||
        dataLength > kMaxBleMessageLength || n != static_cast<size_t>(4 + dataLength)) {
      error = kErrBadArgument;
      return;
    }
    if (!connectedPeers_.count(peerId)) { error = kErrBadState; return; }
    for (const auto &entry : outgoingMessages_) {
      if (entry.second.peerId == peerId) { error = kErrBusy; return; }
    }
    const uint16_t messageId = nextMessageId_++;
    OutgoingMessage message;
    message.messageId = messageId;
    message.peerId = peerId;
    message.data.assign(p + 4, p + 4 + dataLength);
    message.fragmentCount = static_cast<uint8_t>(std::max<size_t>(1, (dataLength + kBleFragmentPayload - 1) / kBleFragmentPayload));
    message.lastFragmentAt = std::chrono::steady_clock::now();
    outgoingMessages_[messageId] = std::move(message);
    AppendU16(out, messageId);
  }

  void HandleGetReceivedData(const uint8_t *p, size_t n, uint8_t &error, std::vector<uint8_t> &out) {
    uint16_t peerId = 0, messageId = 0;
    if (!ReadU16(p, n, 0, peerId) || !ReadU16(p, n, 2, messageId) || n != 4) {
      error = kErrBadArgument;
      return;
    }
    const IncomingKey key{peerId, messageId};
    auto incoming = incomingMessages_.find(key);
    if (incoming == incomingMessages_.end()) { error = kErrNotFound; return; }
    const auto data = incoming->second.data;
    AppendU16(out, messageId);
    AppendU16(out, static_cast<uint16_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    incomingMessages_.erase(incoming);

    // GET_RECEIVED_DATA starts the firmware's MESSAGE_RESULT(CONSUMED)
    // transaction. The virtual BLE link is lossless, but the result and its
    // acknowledgement still pass through the same peer/message correlation
    // and sender-side completion path as the firmware.
    PendingMessageResult result;
    result.messageId = messageId;
    result.status = 0;  // CONSUMED
    result.lastActionAt = std::chrono::steady_clock::now();
    pendingMessageResults_[peerId] = result;
  }

  void HandleGetLastRange(uint16_t filter, std::vector<uint8_t> &out) {
    std::map<uint16_t, Measurement> latest;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
      if (filter != 0xFFFF && it->targetId != filter) continue;
      if (!latest.count(it->targetId)) latest.emplace(it->targetId, *it);
    }
    out.push_back(static_cast<uint8_t>(latest.size()));
    for (const auto &entry : latest) {
      auto record = EncodeMeasurement(entry.second);
      out.insert(out.end(), record.begin(), record.end());
    }
  }

  void HandleGetHistory(const uint8_t *p, size_t n, uint8_t &error, std::vector<uint8_t> &out) {
    uint32_t cursor = 0;
    uint16_t maxRecords = 0, filter = 0xFFFF;
    if (!ReadU32(p, n, 0, cursor) || !ReadU16(p, n, 4, maxRecords) || !ReadU16(p, n, 6, filter) || n != 8) {
      error = kErrBadArgument;
      return;
    }
    if (maxRecords == 0) maxRecords = 1;
    maxRecords = std::min<uint16_t>(maxRecords, 17);
    const uint32_t oldest = history_.empty() ? nextSampleSeq_ : history_.front().sampleSeq;
    const uint32_t newest = history_.empty() ? 0 : history_.back().sampleSeq;
    const bool overrun = !history_.empty() && cursor + 1 < oldest;
    uint32_t nextCursor = cursor;
    std::vector<Measurement> selected;
    for (const auto &record : history_) {
      if (record.sampleSeq <= cursor) continue;
      if (selected.size() >= maxRecords) break;
      nextCursor = record.sampleSeq;
      if (filter == 0xFFFF || filter == record.targetId) selected.push_back(record);
    }
    if (selected.size() < maxRecords && newest > nextCursor) nextCursor = newest;
    AppendU32(out, oldest);
    AppendU32(out, newest);
    AppendU32(out, nextCursor);
    out.push_back(overrun ? 1 : 0);
    out.push_back(static_cast<uint8_t>(selected.size()));
    for (const auto &record : selected) {
      auto bytes = EncodeMeasurement(record);
      out.insert(out.end(), bytes.begin(), bytes.end());
    }
  }

  uint16_t AllocateJobId() {
    uint16_t candidate = nextJobId_++;
    if (nextJobId_ == 0) nextJobId_ = 1;
    if (candidate == 0) candidate = nextJobId_++;
    return candidate;
  }

  void ScheduleNextRanges(int64_t nowNs) {
    nextPeriodicNs_.clear();
    if (runtime_.role == kRoleInitiator) {
      const int64_t period = static_cast<int64_t>(runtime_.periodMs) * 1000000LL;
      for (uint16_t target : runtime_.targets) nextPeriodicNs_[target] = nowNs + period;
    }
  }

  void CancelQueuedJobsLocked() {
    while (!rangeJobs_.empty()) {
      const auto job = rangeJobs_.front();
      rangeJobs_.pop_front();
      RecordRangeFailure(job.targetId, job.jobId, kRangeCancelled);
    }
  }

  bool CancelJobLocked(uint16_t jobId) {
    auto found = std::find_if(rangeJobs_.begin(), rangeJobs_.end(), [jobId](const RangeJob &job) {
      return job.jobId == jobId;
    });
    if (found == rangeJobs_.end()) return false;
    const auto job = *found;
    rangeJobs_.erase(found);
    RecordRangeFailure(job.targetId, job.jobId, kRangeCancelled);
    return true;
  }

  void TickNetwork(int64_t nowNs) {
    for (auto &entry : NetworkNodes()) {
      entry.second->lastSimTimeNs_ = nowNs;
      entry.second->nowSimTimeNs_ = nowNs;
    }
    for (auto &entry : NetworkNodes()) entry.second->TickUwbLocked(nowNs);
  }

  void TickUwbLocked(int64_t nowNs) {
    if (radioStandby_ || !radioReady_ || runtime_.role != kRoleInitiator) return;
    while (!rangeJobs_.empty()) {
      const auto job = rangeJobs_.front();
      rangeJobs_.pop_front();
      PerformRangeLocked(job.targetId, job.jobId);
    }
    if (!rangingEnabled_) return;
    const int64_t periodNs = static_cast<int64_t>(runtime_.periodMs) * 1000000LL;
    for (uint16_t target : runtime_.targets) {
      auto due = nextPeriodicNs_.find(target);
      if (due == nextPeriodicNs_.end()) {
        nextPeriodicNs_[target] = nowNs + periodNs;
        continue;
      }
      if (nowNs >= due->second) {
        PerformRangeLocked(target, 0);
        do { due->second += periodNs; } while (due->second <= nowNs);
      }
    }
  }

  bool PeerAllows(const DeviceConfig &config, uint16_t peerId) const {
    return config.blePeers.empty() || Contains(config.blePeers, peerId);
  }

  static bool UwbPeerAllows(const DeviceConfig &config, uint16_t peerId) {
    return config.targets.empty() || Contains(config.targets, peerId);
  }

  void PerformRangeLocked(uint16_t targetId, uint16_t jobId) {
    auto targetIt = NetworkNodes().find(targetId);
    if (targetIt == NetworkNodes().end()) {
      RecordRangeFailure(targetId, jobId, kRangeTimeout);
      ++transactionTimeouts_;
      return;
    }
    auto *target = targetIt->second;
    if (runtime_.role != kRoleInitiator || target->runtime_.role != kRoleResponder ||
        !target->rangingEnabled_ || target->radioStandby_ ||
        runtime_.panId != target->runtime_.panId || runtime_.channel != target->runtime_.channel ||
        !target->poseValid_ || !poseValid_ || !UwbPeerAllows(target->runtime_, runtime_.deviceId)) {
      RecordRangeFailure(targetId, jobId, kRangeTimeout);
      ++transactionTimeouts_;
      return;
    }
    const double geometricRange = (target->worldPosition_ - worldPosition_).Length();
    if (!std::isfinite(geometricRange) || geometricRange > maxRangeM_) {
      RecordRangeFailure(targetId, jobId, kRangeTimeout);
      ++transactionTimeouts_;
      return;
    }
    const double residualBias = nodeBiasM_ + target->nodeBiasM_;
    const double noisyRange = geometricRange + residualBias + noiseDistribution_(random_);
    if (!std::isfinite(noisyRange) || noisyRange < 0.0 || noisyRange > maxRangeM_) {
      RecordRangeFailure(targetId, jobId, kRangeInvalid);
      return;
    }
    Measurement measurement;
    measurement.sampleSeq = AllocateSampleSequence();
    measurement.targetId = targetId;
    measurement.monotonicUs = ToUs(nowSimTimeNs_);
    measurement.epochUs = clockSynchronized_ ? static_cast<uint64_t>(
        static_cast<int64_t>(measurement.monotonicUs) + epochOffsetUs_) : 0;
    measurement.rangeMm = static_cast<int32_t>(std::llround(noisyRange * 1000.0));
    measurement.status = kRangeValid;
    measurement.qualityFlags = 0;
    StoreMeasurement(measurement);
    ++rangesOk_;
    auto payload = EncodeMeasurement(measurement);
    std::vector<uint8_t> event;
    AppendU16(event, jobId);
    event.insert(event.end(), payload.begin(), payload.end());
    EmitEvent(0x80, event);

    // The responder completes the same ranging exchange and keeps its own
    // measurement record, with a local sequence number and clock timestamp.
    Measurement responderMeasurement;
    responderMeasurement.sampleSeq = target->AllocateSampleSequence();
    responderMeasurement.targetId = runtime_.deviceId;
    responderMeasurement.monotonicUs = ToUs(target->nowSimTimeNs_);
    responderMeasurement.epochUs = target->clockSynchronized_ ? static_cast<uint64_t>(
        static_cast<int64_t>(responderMeasurement.monotonicUs) + target->epochOffsetUs_) : 0;
    responderMeasurement.rangeMm = measurement.rangeMm;
    responderMeasurement.status = measurement.status;
    responderMeasurement.qualityFlags = measurement.qualityFlags;
    target->StoreMeasurement(responderMeasurement);
    ++target->rangesOk_;

    auto responderPayload = EncodeMeasurement(responderMeasurement);
    std::vector<uint8_t> responderEvent;
    AppendU16(responderEvent, 0);  // RANGE_ONCE job IDs belong to the initiator.
    responderEvent.insert(responderEvent.end(), responderPayload.begin(), responderPayload.end());
    target->EmitEvent(0x80, responderEvent);
  }

  void RecordRangeFailure(uint16_t targetId, uint16_t jobId, uint8_t status) {
    Measurement measurement;
    measurement.sampleSeq = AllocateSampleSequence();
    measurement.targetId = targetId;
    measurement.monotonicUs = ToUs(lastSimTimeNs_);
    measurement.epochUs = clockSynchronized_ ? static_cast<uint64_t>(
        static_cast<int64_t>(measurement.monotonicUs) + epochOffsetUs_) : 0;
    measurement.rangeMm = -1;
    measurement.status = status;
    measurement.qualityFlags = 0;
    StoreMeasurement(measurement);
    ++rangesFailed_;
    std::vector<uint8_t> event;
    AppendU16(event, jobId);
    AppendU16(event, targetId);
    event.push_back(status);
    AppendU64(event, measurement.monotonicUs);
    EmitEvent(0x81, event);
  }

  uint32_t AllocateSampleSequence() {
    uint32_t result = nextSampleSeq_++;
    if (nextSampleSeq_ == 0) nextSampleSeq_ = 1;
    if (result == 0) result = nextSampleSeq_++;
    return result;
  }

  void StoreMeasurement(const Measurement &measurement) {
    if (history_.size() == kHistoryCapacity) {
      history_.pop_front();
      ++historyOverwrites_;
    }
    history_.push_back(measurement);
  }

  void UpdateBleConnectionsLocked() {
    std::map<D3kNodeSystem *, std::set<uint16_t>> desired;
    for (const auto &localEntry : NetworkNodes()) {
      auto *local = localEntry.second;
      if (!local->bleEnabled_) continue;
      for (const auto &remoteEntry : NetworkNodes()) {
        auto *remote = remoteEntry.second;
        if (remote == local || !remote->bleEnabled_) continue;
        const bool complementaryRoles =
            (local->runtime_.bleMode == 1 && remote->runtime_.bleMode == 2) ||
            (local->runtime_.bleMode == 2 && remote->runtime_.bleMode == 1);
        if (!complementaryRoles) continue;
        if (!local->PeerAllows(local->runtime_, remote->runtime_.deviceId) ||
            !remote->PeerAllows(remote->runtime_, local->runtime_.deviceId)) continue;
        if (desired[local].size() >= kMaxBlePeers || desired[remote].size() >= kMaxBlePeers) continue;
        desired[local].insert(remote->runtime_.deviceId);
      }
    }
    for (const auto &entry : NetworkNodes()) {
      auto *node = entry.second;
      const auto newPeers = desired[node];
      for (uint16_t peer : newPeers) {
        if (!node->connectedPeers_.count(peer)) {
          if (node->runtime_.bleMode == 1 && !node->foundPeers_.count(peer)) {
            node->foundPeers_.insert(peer);
            std::vector<uint8_t> foundEvent;
            AppendU16(foundEvent, peer);
            node->EmitEvent(0x87, foundEvent);
          }
          node->connectedPeers_.insert(peer);
          std::vector<uint8_t> connectedEvent;
          AppendU16(connectedEvent, peer);
          node->EmitEvent(0x88, connectedEvent);
        }
      }
      std::vector<uint16_t> lost;
      for (uint16_t peer : node->connectedPeers_)
        if (!newPeers.count(peer)) lost.push_back(peer);
      for (uint16_t peer : lost) {
        node->connectedPeers_.erase(peer);
        std::vector<uint8_t> disconnectedEvent;
        AppendU16(disconnectedEvent, peer);
        disconnectedEvent.push_back(0);
        node->EmitEvent(0x89, disconnectedEvent);
        node->FailTransfersForPeerLocked(peer);
      }
    }
  }

  void FailTransfersForPeerLocked(uint16_t peerId) {
    for (auto it = outgoingMessages_.begin(); it != outgoingMessages_.end();) {
      if (it->second.peerId == peerId) {
        ++bleTxErrors_;
        it = outgoingMessages_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = incomingMessages_.begin(); it != incomingMessages_.end();) {
      if (it->first.peerId == peerId) it = incomingMessages_.erase(it);
      else ++it;
    }
    pendingMessageResults_.erase(peerId);
  }

  void TickBleTransfersLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = outgoingMessages_.begin(); it != outgoingMessages_.end();) {
      auto &message = it->second;
      if (message.waitingForConsumption) {
        // The firmware sender independently waits for MESSAGE_RESULT_ACK,
        // retrying its result wait at RESULT_TIMEOUT_MS up to max_retries.
        if (!ble::MessageResultRetryDue(message.lastFragmentAt, now)) {
          ++it;
          continue;
        }
        if (message.resultRetryCount >= runtime_.bleMaxRetries) {
          EmitBleMessageError(0, message.peerId, message.messageId, 0xFF, 9);
          it = outgoingMessages_.erase(it);
        } else {
          ++message.resultRetryCount;
          message.lastFragmentAt = now;
          ++it;
        }
        continue;
      }
      auto peer = NetworkNodes().find(message.peerId);
      if (!connectedPeers_.count(message.peerId) || peer == NetworkNodes().end()) {
        ++bleTxErrors_;
        it = outgoingMessages_.erase(it);
        continue;
      }
      if (!ble::FragmentIntervalElapsed(message.lastFragmentAt, now)) { ++it; continue; }

      // The firmware rejects the first fragment while a completed message is
      // unread (RX_QUEUE_FULL), or while a MESSAGE_RESULT is outstanding
      // (RX_BUSY). Its sender retries the NACK up to max_retries. The virtual
      // link is lossless, so model those deterministic NACK/retry cycles
      // without introducing radio packet loss.
      const bool resultPending =
          peer->second->pendingMessageResults_.count(runtime_.deviceId) != 0;
      const bool peerHasUnread = std::any_of(
          peer->second->incomingMessages_.begin(), peer->second->incomingMessages_.end(),
          [this](const auto &candidate) { return candidate.first.peerId == runtime_.deviceId; });
      if (message.nextFragment == 0 && (resultPending || peerHasUnread)) {
        const uint8_t nackCode = resultPending ? 2 : 1;  // RX_BUSY / RX_QUEUE_FULL
        if (!ble::ShouldRetryNack(message.retryCount, runtime_.bleMaxRetries)) {
          EmitBleMessageError(0, message.peerId, message.messageId, 0, nackCode);
          it = outgoingMessages_.erase(it);
          continue;
        }
        message.lastFragmentAt = now;
        ++it;
        continue;
      }

      const size_t offset = static_cast<size_t>(message.nextFragment) * kBleFragmentPayload;
      const size_t fragmentLength = std::min(kBleFragmentPayload, message.data.size() - std::min(offset, message.data.size()));
      if (!ValidateVirtualBleFragment(message, fragmentLength)) {
        ++bleTxErrors_;
        it = outgoingMessages_.erase(it);
        continue;
      }
      ++message.nextFragment;
      message.lastFragmentAt = now;
      if (message.nextFragment >= message.fragmentCount) {
        auto *receiver = peer->second;
        const IncomingKey key{runtime_.deviceId, message.messageId};
        IncomingMessage incoming;
        incoming.messageId = message.messageId;
        incoming.peerId = runtime_.deviceId;
        incoming.data = message.data;
        incoming.completedAt = now;
        receiver->incomingMessages_[key] = std::move(incoming);
        ++receiver->bleMessagesReceived_;
        std::vector<uint8_t> receivedEvent;
        AppendU16(receivedEvent, runtime_.deviceId);
        AppendU16(receivedEvent, message.messageId);
        AppendU16(receivedEvent, static_cast<uint16_t>(message.data.size()));
        receiver->EmitEvent(0x8B, receivedEvent);
        message.waitingForConsumption = true;
      }
      ++it;
    }
  }

  bool ValidateVirtualBleFragment(const OutgoingMessage &message, size_t fragmentLength) const {
    // Encode the D3K custom GATT DATA packet and verify its CRC exactly as the
    // firmware peer protocol does. The simulated link is lossless by default;
    // virtual packets are scheduled over Gazebo time rather than BLE hardware.
    std::vector<uint8_t> packet;
    packet.push_back(0x01);  // DATA
    AppendU16(packet, message.messageId);
    packet.push_back(message.nextFragment);
    packet.push_back(message.fragmentCount);
    AppendU16(packet, static_cast<uint16_t>(message.data.size()));
    const size_t offset = static_cast<size_t>(message.nextFragment) * kBleFragmentPayload;
    packet.insert(packet.end(), message.data.begin() + static_cast<std::ptrdiff_t>(offset),
                  message.data.begin() + static_cast<std::ptrdiff_t>(offset + fragmentLength));
    const uint16_t crc = UwbProto::crc16(packet.data(), packet.size());
    AppendU16(packet, crc);
    if (packet.size() < 9) return false;
    const uint16_t expected = UwbProto::getU16(packet.data() + packet.size() - 2);
    if (UwbProto::crc16(packet.data(), packet.size() - 2) != expected) return false;

    // Model the receiver's per-fragment ACK packet as well. In this MVP the
    // virtual link is lossless, so a valid ACK advances the sender immediately.
    std::vector<uint8_t> ack{0x02};  // ACK
    AppendU16(ack, message.messageId);
    ack.push_back(message.nextFragment);
    const uint16_t ackCrc = UwbProto::crc16(ack.data(), ack.size());
    AppendU16(ack, ackCrc);
    return UwbProto::crc16(ack.data(), ack.size() - 2) == UwbProto::getU16(ack.data() + ack.size() - 2);
  }

  void EmitBleMessageError(uint8_t direction, uint16_t peerId, uint16_t messageId,
                           uint8_t fragmentIndex, uint8_t errorCode) {
    EmitEvent(0x8C, ble::BuildErrorPayload(
        direction, peerId, messageId, fragmentIndex, errorCode));
    if (direction == 0) ++bleTxErrors_;
    else ++bleRxErrors_;
  }

  bool DeliverPendingMessageResultLocked(uint16_t senderId,
                                         const PendingMessageResult &result) {
    auto sender = NetworkNodes().find(senderId);
    if (sender == NetworkNodes().end() ||
        !sender->second->connectedPeers_.count(runtime_.deviceId)) return false;

    // MESSAGE_RESULT and MESSAGE_RESULT_ACK are real protocol packets. Their
    // delivery is immediate and reliable in the virtual link, but CRC and
    // message-ID matching are still checked before completing the send.
    const auto resultPacket = ble::BuildMessageResultPacket(result.messageId, result.status);
    if (result.status > 1 || !ble::HasValidPacketCrc(resultPacket)) return false;

    const auto ackPacket = ble::BuildMessageResultAckPacket(result.messageId);
    if (!ble::HasValidPacketCrc(ackPacket)) return false;

    auto outgoing = sender->second->outgoingMessages_.find(result.messageId);
    if (outgoing != sender->second->outgoingMessages_.end() &&
        outgoing->second.peerId == runtime_.deviceId) {
      if (result.status == 0) {
        sender->second->EmitEvent(0x8D, ble::BuildDataSentPayload(
            runtime_.deviceId, result.messageId,
            static_cast<uint16_t>(outgoing->second.data.size())));
        ++sender->second->bleMessagesSent_;
      } else {
        // Firmware reports a consumption timeout as an error on the sender;
        // EVENT_DATA_EXPIRED (0x8E) is emitted only by the receiving node.
        sender->second->EmitBleMessageError(
            0, runtime_.deviceId, result.messageId, 0xFF, result.status);
      }
      sender->second->outgoingMessages_.erase(outgoing);
    }
    return true;
  }

  void TickBleResultTransactionsLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = pendingMessageResults_.begin(); it != pendingMessageResults_.end();) {
      const uint16_t senderId = it->first;
      auto &result = it->second;

      // The receiver keeps result.active set until MESSAGE_RESULT_ACK returns.
      // The virtual link has a one-millisecond minimum packet interval, so
      // retain that state for one scheduler turn rather than completing the
      // entire handshake in the same call that consumes GET_RECEIVED_DATA.
      if (result.initialAttemptPending) {
        if (!ble::FragmentIntervalElapsed(result.lastActionAt, now)) {
          ++it;
          continue;
        }
        result.initialAttemptPending = false;
        if (DeliverPendingMessageResultLocked(senderId, result)) {
          it = pendingMessageResults_.erase(it);
        } else {
          result.lastActionAt = now;
          ++it;
        }
        continue;
      }

      // Subsequent MESSAGE_RESULT packets are sent only when the receiver's
      // 400 ms retry timer expires. Do not continuously re-send on each
      // invocation of the 10 ms UART service loop.
      if (!ble::MessageResultRetryDue(result.lastActionAt, now)) {
        ++it;
        continue;
      }
      if (result.retryCount >= runtime_.bleMaxRetries) {
        EmitBleMessageError(1, senderId, result.messageId, 0xFF, 9);
        it = pendingMessageResults_.erase(it);
      } else {
        ++result.retryCount;
        result.lastActionAt = now;
        if (DeliverPendingMessageResultLocked(senderId, result)) {
          it = pendingMessageResults_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  void TickBleExpiryLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = incomingMessages_.begin(); it != incomingMessages_.end();) {
      const IncomingMessage &message = it->second;
      if (!ble::MessageExpired(message.completedAt, now, runtime_.bleRxMessageTimeoutMs)) {
        ++it;
        continue;
      }

      const uint16_t senderId = message.peerId;
      const uint16_t messageId = message.messageId;
      EmitEvent(0x8E, ble::BuildDataExpiredPayload(senderId, messageId));

      // Firmware clears the unread receive buffer, then sends
      // MESSAGE_RESULT(CONSUMPTION_TIMEOUT) to the sender.
      PendingMessageResult result;
      result.messageId = messageId;
      result.status = 1;  // CONSUMPTION_TIMEOUT
      result.lastActionAt = now;
      pendingMessageResults_[senderId] = result;
      it = incomingMessages_.erase(it);
    }
  }

  void EmitStateChanged() {
    std::vector<uint8_t> event{runtime_.role, static_cast<uint8_t>(rangingEnabled_ ? 1 : 0),
                               static_cast<uint8_t>(radioStandby_ ? 1 : 0),
                               static_cast<uint8_t>(radioReady_ ? 1 : 0)};
    EmitEvent(0x82, event);
  }

  void EmitBleStateChanged() {
    std::vector<uint8_t> event{static_cast<uint8_t>(bleEnabled_ ? 1 : 0), runtime_.bleMode,
                               static_cast<uint8_t>(bleEnabled_ && runtime_.bleMode == 2 ? 1 : 0),
                               static_cast<uint8_t>(bleEnabled_ && runtime_.bleMode == 1 ? 1 : 0)};
    EmitEvent(0x86, event);
  }

  void EmitEvent(uint8_t opcode, const std::vector<uint8_t> &payload) {
    const uint8_t bit = static_cast<uint8_t>(opcode - 0x80);
    if (opcode != 0x85 && (bit >= 15 || (subscriptionMask_ & (1u << bit)) == 0)) return;
    UwbProto::Frame frame;
    frame.type = UwbProto::TYPE_EVENT;
    frame.seq = ++eventSequence_;
    frame.opcode = opcode;
    frame.flags = 0;
    frame.length = static_cast<uint16_t>(std::min(payload.size(), UwbProto::MAX_PAYLOAD));
    if (frame.length) std::memcpy(frame.payload, payload.data(), frame.length);
    QueueFrame(frame);
  }

 private:
  gz::sim::Entity modelEntity_ = gz::sim::kNullEntity;
  gz::sim::Entity referenceEntity_ = gz::sim::kNullEntity;
  std::string modelName_;
  std::string referenceLinkName_;
  std::string uartPathPreference_;
  std::string uartPath_;
  std::string symlinkPath_;
  int masterFd_ = -1;
  std::thread ioThread_;
  std::atomic<bool> running_{false};
  bool registered_ = false;

  DeviceConfig saved_;
  DeviceConfig runtime_;
  bool radioReady_ = true;
  bool radioStandby_ = false;
  bool rangingEnabled_ = false;
  bool bleEnabled_ = false;
  bool clockSynchronized_ = false;
  int64_t epochOffsetUs_ = 0;
  int64_t lastSimTimeNs_ = 0;
  int64_t nowSimTimeNs_ = 0;
  uint16_t subscriptionMask_ = 0;
  uint16_t eventSequence_ = 0;
  uint16_t nextJobId_ = 1;
  uint16_t nextMessageId_ = 1;
  uint32_t nextSampleSeq_ = 1;
  std::deque<CachedReply> replyCache_;
  std::deque<RangeJob> rangeJobs_;
  std::map<uint16_t, int64_t> nextPeriodicNs_;
  std::deque<Measurement> history_;
  std::set<uint16_t> connectedPeers_;
  std::set<uint16_t> foundPeers_;
  std::map<IncomingKey, IncomingMessage> incomingMessages_;
  std::map<uint16_t, PendingMessageResult> pendingMessageResults_;
  std::map<uint16_t, OutgoingMessage> outgoingMessages_;
  gz::math::Vector3d worldPosition_;
  bool poseValid_ = false;

  double noiseStddevM_ = 0.015;
  double nodeBiasLimitM_ = 0.03;
  double nodeBiasM_ = 0.0;
  double maxRangeM_ = 100.0;
  std::mt19937 random_{0xD3B1E5u};
  std::normal_distribution<double> noiseDistribution_{0.0, 0.015};

  std::mutex outboundMutex_;
  std::deque<std::vector<uint8_t>> outbound_;
  size_t outboundOffset_ = 0;

  uint32_t rangesOk_ = 0;
  uint32_t rangesFailed_ = 0;
  uint32_t rxErrors_ = 0;
  uint32_t txErrors_ = 0;
  uint32_t transactionTimeouts_ = 0;
  uint32_t protocolErrors_ = 0;
  uint32_t uartOverflows_ = 0;
  uint32_t historyOverwrites_ = 0;
  uint32_t malformedRadioFrames_ = 0;
  uint32_t duplicateRequests_ = 0;
  uint32_t bleMessagesSent_ = 0;
  uint32_t bleMessagesReceived_ = 0;
  uint32_t bleTxErrors_ = 0;
  uint32_t bleRxErrors_ = 0;
  uint32_t bleConnectionFailures_ = 0;
  uint32_t bleRxQueueOverflows_ = 0;
  uint32_t bleEventQueueOverflows_ = 0;
};

}  // namespace d3k_sim

// Gazebo's registration macro defines helper symbols in gz::plugin. Keep it
// at global scope so its namespace qualifiers are resolved as intended.
GZ_ADD_PLUGIN(d3k_sim::D3kNodeSystem,
              gz::sim::System,
              d3k_sim::D3kNodeSystem::ISystemConfigure,
              d3k_sim::D3kNodeSystem::ISystemPostUpdate)
GZ_ADD_PLUGIN_ALIAS(d3k_sim::D3kNodeSystem, "d3k_sim::D3kNodeSystem")
