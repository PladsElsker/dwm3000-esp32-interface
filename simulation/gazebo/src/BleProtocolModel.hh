#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "Protocol.h"

namespace d3k_sim::ble {

inline constexpr uint16_t kMessageResultTimeoutMs = 400;

inline bool MessageResultRetryDue(std::chrono::steady_clock::time_point lastActionAt,
                                  std::chrono::steady_clock::time_point now) {
  return now >= lastActionAt &&
      now - lastActionAt >= std::chrono::milliseconds(kMessageResultTimeoutMs);
}

inline void AppendU16(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

inline bool ShouldRetryNack(uint8_t &retryCount, uint8_t maxRetries) {
  if (retryCount >= maxRetries) return false;
  ++retryCount;
  return true;
}

inline bool MessageExpired(std::chrono::steady_clock::time_point completedAt,
                           std::chrono::steady_clock::time_point now,
                           uint16_t timeoutMs) {
  return now >= completedAt &&
      now - completedAt >= std::chrono::milliseconds(timeoutMs);
}

inline bool FragmentIntervalElapsed(std::chrono::steady_clock::time_point lastFragmentAt,
                                    std::chrono::steady_clock::time_point now) {
  return now >= lastFragmentAt &&
      now - lastFragmentAt >= std::chrono::milliseconds(1);
}

inline std::vector<uint8_t> BuildMessageResultPacket(uint16_t messageId,
                                                      uint8_t status) {
  std::vector<uint8_t> packet{0x04};  // MESSAGE_RESULT
  AppendU16(packet, messageId);
  packet.push_back(status);
  AppendU16(packet, UwbProto::crc16(packet.data(), packet.size()));
  return packet;
}

inline std::vector<uint8_t> BuildMessageResultAckPacket(uint16_t messageId) {
  std::vector<uint8_t> packet{0x05};  // MESSAGE_RESULT_ACK
  AppendU16(packet, messageId);
  AppendU16(packet, UwbProto::crc16(packet.data(), packet.size()));
  return packet;
}

inline bool HasValidPacketCrc(const std::vector<uint8_t> &packet) {
  return packet.size() >= 3 &&
      UwbProto::crc16(packet.data(), packet.size() - 2) ==
          UwbProto::getU16(packet.data() + packet.size() - 2);
}

inline std::vector<uint8_t> BuildErrorPayload(uint8_t direction, uint16_t peerId,
                                               uint16_t messageId,
                                               uint8_t fragmentIndex,
                                               uint8_t errorCode) {
  std::vector<uint8_t> payload{direction};
  AppendU16(payload, peerId);
  AppendU16(payload, messageId);
  payload.push_back(fragmentIndex);
  payload.push_back(errorCode);
  payload.push_back(0);
  return payload;
}

inline std::vector<uint8_t> BuildDataSentPayload(uint16_t peerId,
                                                  uint16_t messageId,
                                                  uint16_t totalLength) {
  std::vector<uint8_t> payload;
  AppendU16(payload, peerId);
  AppendU16(payload, messageId);
  AppendU16(payload, totalLength);
  return payload;
}

inline std::vector<uint8_t> BuildDataExpiredPayload(uint16_t peerId,
                                                     uint16_t messageId) {
  std::vector<uint8_t> payload;
  AppendU16(payload, peerId);
  AppendU16(payload, messageId);
  return payload;
}

}  // namespace d3k_sim::ble
