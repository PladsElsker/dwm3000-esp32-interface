#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

#include "Protocol.h"

int main() {
  UwbProto::Frame request;
  request.type = UwbProto::TYPE_REQUEST;
  request.seq = 0x1234;
  request.opcode = 0x01;  // PING
  request.flags = 0;
  request.length = 0;

  std::array<uint8_t, UwbProto::MAX_ENCODED> encoded{};
  const size_t encodedLength = UwbProto::encodeFrame(request, encoded.data(), encoded.size());
  const std::array<uint8_t, 12> expected{
      0x06, 0x01, 0x01, 0x34, 0x12, 0x01, 0x01, 0x01, 0x03, 0xCC, 0xD1, 0x00};
  assert(encodedLength == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) assert(encoded[i] == expected[i]);

  UwbProto::Frame decoded;
  assert(UwbProto::decodeFrame(encoded.data(), encodedLength - 1, decoded));
  assert(decoded.type == UwbProto::TYPE_REQUEST);
  assert(decoded.seq == request.seq);
  assert(decoded.opcode == request.opcode);
  assert(decoded.length == 0);

  // Corrupt a non-delimiter byte and verify CRC validation rejects the frame.
  encoded[4] ^= 0x01;
  assert(!UwbProto::decodeFrame(encoded.data(), encodedLength - 1, decoded));

  std::cout << "D3K UART frame codec matches the documented PING vector.\n";
  return 0;
}
