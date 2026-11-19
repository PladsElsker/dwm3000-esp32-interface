#include "Protocol.h"

namespace UwbProto {
uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= uint16_t(data[i]) << 8;
    for (uint8_t b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
  }
  return crc;
}

size_t cobsEncode(const uint8_t *input, size_t length, uint8_t *output, size_t capacity) {
  if (!output || capacity == 0) return 0;
  size_t readIndex = 0, writeIndex = 1, codeIndex = 0;
  uint8_t code = 1;
  while (readIndex < length) {
    if (input[readIndex] == 0) {
      if (codeIndex >= capacity) return 0;
      output[codeIndex] = code;
      code = 1;
      codeIndex = writeIndex++;
      if (writeIndex > capacity) return 0;
      ++readIndex;
    } else {
      if (writeIndex >= capacity) return 0;
      output[writeIndex++] = input[readIndex++];
      if (++code == 0xFF) {
        output[codeIndex] = code;
        code = 1;
        codeIndex = writeIndex++;
        if (writeIndex > capacity) return 0;
      }
    }
  }
  if (codeIndex >= capacity) return 0;
  output[codeIndex] = code;
  return writeIndex;
}

size_t cobsDecode(const uint8_t *input, size_t length, uint8_t *output, size_t capacity) {
  size_t readIndex = 0, writeIndex = 0;
  while (readIndex < length) {
    const uint8_t code = input[readIndex++];
    if (code == 0) return 0;
    const size_t copyLen = size_t(code - 1);
    if (readIndex + copyLen > length || writeIndex + copyLen > capacity) return 0;
    for (size_t i = 0; i < copyLen; ++i) output[writeIndex++] = input[readIndex++];
    if (code != 0xFF && readIndex < length) {
      if (writeIndex >= capacity) return 0;
      output[writeIndex++] = 0;
    }
  }
  return writeIndex;
}

bool decodeFrame(const uint8_t *encoded, size_t encodedLen, Frame &out) {
  uint8_t raw[MAX_DECODED];
  const size_t n = cobsDecode(encoded, encodedLen, raw, sizeof(raw));
  if (n < HEADER_LEN + 2 || raw[0] != VERSION) return false;
  const uint16_t payloadLen = getU16(raw + 6);
  if (payloadLen > MAX_PAYLOAD || n != HEADER_LEN + payloadLen + 2) return false;
  const uint16_t expected = getU16(raw + HEADER_LEN + payloadLen);
  if (crc16(raw, HEADER_LEN + payloadLen) != expected) return false;
  out.version = raw[0];
  out.type = raw[1];
  out.seq = getU16(raw + 2);
  out.opcode = raw[4];
  out.flags = raw[5];
  out.length = payloadLen;
  if (payloadLen) memcpy(out.payload, raw + HEADER_LEN, payloadLen);
  return out.type == TYPE_REQUEST || out.type == TYPE_RESPONSE || out.type == TYPE_EVENT;
}

size_t encodeFrame(const Frame &frame, uint8_t *encoded, size_t capacity) {
  if (frame.length > MAX_PAYLOAD || capacity < 2) return 0;
  uint8_t raw[MAX_DECODED];
  raw[0] = frame.version;
  raw[1] = frame.type;
  putU16(raw + 2, frame.seq);
  raw[4] = frame.opcode;
  raw[5] = frame.flags;
  putU16(raw + 6, frame.length);
  if (frame.length) memcpy(raw + HEADER_LEN, frame.payload, frame.length);
  putU16(raw + HEADER_LEN + frame.length, crc16(raw, HEADER_LEN + frame.length));
  const size_t n = cobsEncode(raw, HEADER_LEN + frame.length + 2, encoded, capacity - 1);
  if (!n || n >= capacity) return 0;
  encoded[n] = 0;
  return n + 1;
}
}  // namespace UwbProto
