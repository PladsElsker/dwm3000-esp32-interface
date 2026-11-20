#pragma once

#include <stddef.h>
#include <stdint.h>

namespace UwbProto {
static constexpr uint8_t VERSION = 1;
static constexpr uint8_t TYPE_REQUEST = 1;
static constexpr uint8_t TYPE_RESPONSE = 2;
static constexpr uint8_t TYPE_EVENT = 3;
static constexpr size_t HEADER_LEN = 8;
static constexpr size_t MAX_PAYLOAD = 500;
static constexpr size_t MAX_DECODED = HEADER_LEN + MAX_PAYLOAD + 2;
static constexpr size_t MAX_ENCODED = MAX_DECODED + (MAX_DECODED / 254) + 2;

struct Frame {
  uint8_t version = VERSION;
  uint8_t type = 0;
  uint16_t seq = 0;
  uint8_t opcode = 0;
  uint8_t flags = 0;
  uint16_t length = 0;
  uint8_t payload[MAX_PAYLOAD]{};
};

uint16_t crc16(const uint8_t *data, size_t len);
size_t cobsEncode(const uint8_t *input, size_t length, uint8_t *output, size_t capacity);
size_t cobsDecode(const uint8_t *input, size_t length, uint8_t *output, size_t capacity);
bool decodeFrame(const uint8_t *encoded, size_t encodedLen, Frame &out);
size_t encodeFrame(const Frame &frame, uint8_t *encoded, size_t capacity);

inline uint16_t getU16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t getU32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t getU64(const uint8_t *p) { return uint64_t(getU32(p)) | (uint64_t(getU32(p + 4)) << 32); }
inline void putU16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void putU32(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
inline void putU64(uint8_t *p, uint64_t v) { putU32(p, uint32_t(v)); putU32(p + 4, uint32_t(v >> 32)); }
} // namespace UwbProto
