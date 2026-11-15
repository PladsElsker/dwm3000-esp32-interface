#!/usr/bin/env python3
"""Host-side tests for the V2.1 BLE config and packet helpers."""
import argparse
import contextlib
import importlib.util
import io
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pi_client_ble_test_module", ROOT / "pi_client.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def config_args(peers):
    return argparse.Namespace(
        ble_mode="hub",
        ble_peers=peers,
        ble_advertising_interval_ms=160,
        ble_scan_interval_ms=160,
        ble_scan_window_ms=80,
        ble_retry_interval_ms=1000,
        ble_connection_timeout_ms=5000,
        ble_retries=3,
        ble_rx_message_timeout_ms=1000,
        ble_auto_start=False,
    )


# The extension stays 16 bytes plus two bytes for each peer. Byte 7
# independently persists whether BLE should start automatically after boot.
extension = module.encode_ble_config(config_args([2, 3]))
assert len(extension) == 20
assert extension[7] == 0
decoded_config = module.decode_ble_config(extension)
assert decoded_config["ble_mode"] == "hub"
assert decoded_config["ble_peers"] == [2, 3]
assert decoded_config["ble_retry_interval_ms"] == 1000
assert decoded_config["ble_auto_start"] is False

auto_start_args = config_args([2, 3])
auto_start_args.ble_auto_start = True
auto_start_extension = module.encode_ble_config(auto_start_args)
assert auto_start_extension[7] == 1
assert module.decode_ble_config(auto_start_extension)["ble_auto_start"] is True

idle_auto_start_args = config_args([])
idle_auto_start_args.ble_mode = "idle"
idle_auto_start_args.ble_auto_start = True
try:
    module.encode_ble_config(idle_auto_start_args)
except ValueError:
    pass
else:
    raise AssertionError("BLE auto-start accepted with IDLE mode")

# Values other than 0 or 1 are invalid boot-preference flags.
invalid_auto_start = bytearray(extension)
invalid_auto_start[7] = 2
try:
    module.decode_ble_config(bytes(invalid_auto_start))
except ValueError:
    pass
else:
    raise AssertionError("invalid BLE auto-start flag accepted")

# Open mode is represented by an empty peer list.
open_extension = module.encode_ble_config(config_args([]))
assert len(open_extension) == 16
assert module.decode_ble_config(open_extension)["ble_peers"] == []

# The configured peer list supports all eight IDs.
eight_peers = list(range(1, 9))
eight_extension = module.encode_ble_config(config_args(eight_peers))
assert len(eight_extension) == 32
assert module.decode_ble_config(eight_extension)["ble_peers"] == eight_peers

for bad_peers in ([1, 1], [0], [65535], list(range(1, 10))):
    try:
        module.encode_ble_config(config_args(bad_peers))
    except ValueError:
        pass
    else:
        raise AssertionError(f"invalid peer list accepted: {bad_peers}")

# Manual peer connection commands are not part of the V2.1 command surface.
assert "ble-connect" not in module.OPS
assert "ble-disconnect" not in module.OPS
assert "save-config" not in module.OPS
assert 0x12 not in module.OPS.values()
assert 0x54 not in module.OPS.values()
assert 0x55 not in module.OPS.values()
assert module.OPS["start-ble"] == 0x52
assert module.OPS["stop-ble"] == 0x53
assert module.OPS["peer-status"] == 0x56
assert module.OPS["ble-trace"] == 0x59

# The firmware BLE trace is a count byte plus 13-byte little-endian records.
trace_payload = bytes([1]) + struct.pack("<IBHBBhBB", 12345, 12, 3, 6, 4, 13, 1, 0b00010001)
trace = module.record_response(0x59, trace_payload)
assert trace["count"] == 1
assert trace["records"][0] == {
    "uptime_ms": 12345, "code": 12, "peer_id": 3, "reason": 6,
    "stage": 4, "detail": 13, "active_links": 1, "state_flags": 0b00010001
}
try:
    module.record_response(0x59, trace_payload + b"\\x00")
except ValueError:
    pass
else:
    raise AssertionError("invalid BLE diagnostic trace length accepted")

# The CLI rejects removed connection-management and generic save commands,
# and no longer accepts a reconnect-enable switch.
original_argv = sys.argv
for removed_command in ("ble-connect", "ble-disconnect", "save-config"):
    sys.argv = ["pi_client.py", removed_command]
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            module.parse_args()
    except SystemExit as exc:
        assert exc.code == 2
    else:
        raise AssertionError(f"removed command was accepted: {removed_command}")
sys.argv = [
    "pi_client.py", "set-config", "1", "4660", "5", "16384", "16384",
    "--ble-mode", "hub", "--ble-retry-interval-ms", "1500", "--ble-auto-start"
]
try:
    parsed_args = module.parse_args()
    assert parsed_args.ble_retry_interval_ms == 1500
    roundtrip = module.decode_ble_config(module.encode_ble_config(parsed_args))
    assert roundtrip["ble_retry_interval_ms"] == 1500
    assert roundtrip["ble_auto_start"] is True
    sys.argv = [
        "pi_client.py", "set-config", "1", "4660", "5", "16384", "16384",
        "--ble-mode", "hub", "--ble-reconnect"
    ]
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            module.parse_args()
    except SystemExit as exc:
        assert exc.code == 2
    else:
        raise AssertionError("removed BLE reconnect switch was accepted")
finally:
    sys.argv = original_argv

# GET_STATUS exposes aggregate link presence plus a representative peer/MTU.
status_payload = bytearray(27)
status_payload[18] = 1  # HUB
status_payload[19] = 1  # enabled
status_payload[20] = 1  # at least one peer connected
struct.pack_into("<H", status_payload, 21, 7)
status_payload[23] = 0  # advertising
status_payload[24] = 1  # scanning
struct.pack_into("<H", status_payload, 25, 185)
status = module.record_response(0x03, bytes(status_payload))
assert status["ble_mode"] == "hub"
assert status["ble_enabled"] is True
assert status["ble_any_peer_connected"] is True
assert status["ble_representative_peer_id"] == 7
assert status["ble_representative_peer_mtu"] == 185

# Per-peer manager state values.
assert module.BLE_PEER_STATE_NAMES[0] == "unknown"
assert module.BLE_PEER_STATE_NAMES[1] == "configured"
assert module.BLE_PEER_STATE_NAMES[2] == "found"
assert module.BLE_PEER_STATE_NAMES[3] == "connected"
assert module.BLE_PEER_STATE_NAMES[4] == "connecting"
assert module.BLE_PEER_STATE_NAMES[5] == "handshaking"
assert module.BLE_PEER_STATE_NAMES[6] == "backoff"
legacy_peer_status_payload = bytes([6, 1, 1, 0, 1, 0]) + struct.pack("<HH", 8, 23)
legacy_peer_status = module.record_response(0x56, legacy_peer_status_payload)
assert legacy_peer_status["state"] == "backoff"
assert "last_failure_reason" not in legacy_peer_status

peer_status_payload = (bytes([6, 1, 1, 0, 1, 0]) + struct.pack("<HH", 8, 23) +
                        struct.pack("<BBh", 6, 4, -5))
peer_status = module.record_response(0x56, peer_status_payload)
assert peer_status["state"] == "backoff"
assert peer_status["peer_id"] == 8
assert peer_status["enabled"] is True
assert peer_status["last_failure_reason"] == 6
assert peer_status["last_failure_stage"] == 4
assert peer_status["last_failure_detail"] == -5

# Fragmentation and packet CRC at the supported MTUs.
message = bytes(range(256)) + bytes(range(240))
for mtu, expected_count in ((23, 46), (100, 6), (185, 3), (256, 3)):
    packets = module.fragment_ble_message(0xFFFF, message, mtu)
    assert len(packets) == expected_count, (mtu, len(packets))
    for packet in packets:
        assert len(packet) <= mtu - 3
        parsed = module.decode_ble_packet(packet)
        assert parsed["type"] == "DATA"
        assert parsed["message_id"] == 0xFFFF

empty = module.fragment_ble_message(0, b"", 23)
assert len(empty) == 1
assert module.decode_ble_packet(empty[0])["payload"] == b""

# A 496-byte message at MTU 256 becomes payloads of 244, 244 and 8 bytes.
packets = module.fragment_ble_message(0x1234, message, 256)
assert [len(module.decode_ble_packet(packet)["payload"]) for packet in packets] == [244, 244, 8]


def packet_with_crc(body):
    return body + struct.pack("<H", module.crc16(body))


ack = packet_with_crc(struct.pack("<BHB", 0x02, 0x1234, 4))
assert module.decode_ble_packet(ack) == {
    "type": "ACK", "message_id": 0x1234, "fragment_index": 4
}
nack = packet_with_crc(struct.pack("<BHBB", 0x03, 0x1234, 5, 2))
assert module.decode_ble_packet(nack)["error_code"] == 2
result = packet_with_crc(struct.pack("<BHB", 0x04, 0x1234, 1))
assert module.decode_ble_packet(result)["status"] == 1
result_ack = packet_with_crc(struct.pack("<BH", 0x05, 0x1234))
assert module.decode_ble_packet(result_ack)["message_id"] == 0x1234

corrupt = bytearray(ack)
corrupt[1] ^= 0x01
try:
    module.decode_ble_packet(bytes(corrupt))
except ValueError:
    pass
else:
    raise AssertionError("corrupt CRC was accepted")

for bad_mtu in (22, 257, 518):
    try:
        module.fragment_ble_message(1, b"x", bad_mtu)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid ATT MTU was accepted")

assert 1 + 32 * 13 <= module.MAX_PAYLOAD
print("BLE V2.1 config, status, trace command, fragmentation and CRC tests passed")
