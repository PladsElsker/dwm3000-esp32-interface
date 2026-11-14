#!/usr/bin/env python3
"""Host-side tests for the BLE packet/config helpers used by pi_client.py."""
import argparse
import importlib.util
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pi_client_ble_test_module", ROOT / "pi_client.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

args = argparse.Namespace(
    ble_mode="hub", ble_peers=[2, 3], ble_advertising_interval_ms=160,
    ble_scan_interval_ms=160, ble_scan_window_ms=80, ble_reconnect=True,
    ble_reconnect_interval_ms=1000, ble_connection_timeout_ms=5000,
    ble_retries=3, ble_rx_message_timeout_ms=1000,
)
ext = module.encode_ble_config(args)
assert len(ext) == 20
cfg = module.decode_ble_config(ext)
assert cfg["ble_mode"] == "hub"
assert cfg["ble_peers"] == [2, 3]
assert cfg["ble_max_retries"] == 3

for mtu, expected_count in ((23, 46), (100, 6), (185, 3), (256, 3)):
    packets = module.fragment_ble_message(0xFFFF, bytes(range(256)) * 1 + bytes(range(240)), mtu)
    assert len(packets) == expected_count, (mtu, len(packets))
    for packet in packets:
        assert len(packet) <= mtu - 3
        decoded = module.decode_ble_packet(packet)
        assert decoded["type"] == "DATA"
        assert decoded["message_id"] == 0xFFFF

empty = module.fragment_ble_message(0, b"", 23)
assert len(empty) == 1
assert module.decode_ble_packet(empty[0])["payload"] == b""

# A 496-byte application payload at MTU 256 uses three fragments of 244, 244, 8 bytes.
packets = module.fragment_ble_message(0x1234, bytes(range(256)) + bytes(range(240)), 256)
assert [len(module.decode_ble_packet(p)["payload"]) for p in packets] == [244, 244, 8]

# Control packet layouts and CRC verification.
def packet_with_crc(body):
    return body + struct.pack("<H", module.crc16(body))

ack = packet_with_crc(struct.pack("<BHB", 0x02, 0x1234, 4))
assert module.decode_ble_packet(ack) == {"type": "ACK", "message_id": 0x1234, "fragment_index": 4}
nack = packet_with_crc(struct.pack("<BHBB", 0x03, 0x1234, 5, 2))
assert module.decode_ble_packet(nack)["error_code"] == 2
result = packet_with_crc(struct.pack("<BHB", 0x04, 0x1234, 1))
assert module.decode_ble_packet(result)["status"] == 1
result_ack = packet_with_crc(struct.pack("<BH", 0x05, 0x1234))
assert module.decode_ble_packet(result_ack)["message_id"] == 0x1234

bad = bytearray(ack)
bad[1] ^= 0x01
try:
    module.decode_ble_packet(bytes(bad))
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

print("BLE config, fragmentation, packet layout and CRC tests passed")
