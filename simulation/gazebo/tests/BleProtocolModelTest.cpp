#include "BleProtocolModel.hh"

#include <chrono>
#include <cstdint>
#include <vector>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;

  // Firmware permits maxRetries retries after the initial transmission.
  uint8_t retries = 0;
  CHECK(!d3k_sim::ble::ShouldRetryNack(retries, 0));
  CHECK(retries == 0);

  retries = 0;
  CHECK(d3k_sim::ble::ShouldRetryNack(retries, 2));
  CHECK(retries == 1);
  CHECK(d3k_sim::ble::ShouldRetryNack(retries, 2));
  CHECK(retries == 2);
  CHECK(!d3k_sim::ble::ShouldRetryNack(retries, 2));
  CHECK(retries == 2);

  // RX message deadlines are elapsed runtime, not Gazebo simulation time.
  const auto completedAt = Clock::time_point{} + 10s;
  CHECK(!d3k_sim::ble::MessageExpired(completedAt, completedAt + 499ms, 500));
  CHECK(d3k_sim::ble::MessageExpired(completedAt, completedAt + 500ms, 500));
  CHECK(!d3k_sim::ble::MessageExpired(completedAt, completedAt - 1ms, 500));
  CHECK(!d3k_sim::ble::FragmentIntervalElapsed(completedAt, completedAt + 999us));
  CHECK(d3k_sim::ble::FragmentIntervalElapsed(completedAt, completedAt + 1ms));

  // MESSAGE_RESULT retries follow the firmware's 400 ms timer.
  CHECK(d3k_sim::ble::kMessageResultTimeoutMs == 400);
  CHECK(!d3k_sim::ble::MessageResultRetryDue(completedAt, completedAt + 399ms));
  CHECK(d3k_sim::ble::MessageResultRetryDue(completedAt, completedAt + 400ms));
  CHECK(!d3k_sim::ble::MessageResultRetryDue(completedAt, completedAt - 1ms));

  // Check the custom BLE MESSAGE_RESULT and MESSAGE_RESULT_ACK packets.
  const auto result = d3k_sim::ble::BuildMessageResultPacket(0x1234, 0);
  CHECK(result.size() == 6);
  CHECK(result[0] == 0x04 && result[1] == 0x34 &&
        result[2] == 0x12 && result[3] == 0x00);
  CHECK(d3k_sim::ble::HasValidPacketCrc(result));

  const auto resultAck = d3k_sim::ble::BuildMessageResultAckPacket(0x1234);
  CHECK(resultAck.size() == 5);
  CHECK(resultAck[0] == 0x05 && resultAck[1] == 0x34 &&
        resultAck[2] == 0x12);
  CHECK(d3k_sim::ble::HasValidPacketCrc(resultAck));

  auto corruptedResult = result;
  corruptedResult.back() ^= 0x01;
  CHECK(!d3k_sim::ble::HasValidPacketCrc(corruptedResult));

  // Firmware reports consumption timeout as sender-side EVENT_ERROR (0x8C).
  const auto senderError = d3k_sim::ble::BuildErrorPayload(
      0, 0x5678, 0x1234, 0xFF, 1);
  CHECK((senderError == std::vector<uint8_t>{
      0x00, 0x78, 0x56, 0x34, 0x12, 0xFF, 0x01, 0x00}));

  const auto dataSent = d3k_sim::ble::BuildDataSentPayload(0x5678, 0x1234, 42);
  CHECK((dataSent == std::vector<uint8_t>{
      0x78, 0x56, 0x34, 0x12, 0x2A, 0x00}));

  const auto dataExpired = d3k_sim::ble::BuildDataExpiredPayload(0x5678, 0x1234);
  CHECK((dataExpired == std::vector<uint8_t>{
      0x78, 0x56, 0x34, 0x12}));
  return 0;
}
