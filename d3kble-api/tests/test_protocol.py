import struct

import pytest

from d3kble.protocol import (
    Frame, TYPE_EVENT, TYPE_REQUEST, TYPE_RESPONSE, crc16, cobs_encode,
    cobs_decode, decode_frame, encode_frame, decode_ble_extension,
    encode_ble_extension, decode_runtime_config, decode_saved_config,
    decode_status, decode_range_result, decode_history, encode_set_config,
)


def test_cobs_and_frame_round_trip_including_zero_bytes():
    payload = bytes(range(256)) + b"\x00\x00d3kble\x00"
    frame = Frame(TYPE_REQUEST, 65534, 0x55, 0, payload[:500])
    wire = encode_frame(frame)
    assert wire.endswith(b"\x00")
    decoded = decode_frame(wire[:-1])
    assert decoded == frame
    raw = bytes(range(256)) * 2
    assert cobs_decode(cobs_encode(raw)) == raw


def test_rejects_corrupt_crc_and_invalid_length():
    wire = bytearray(encode_frame(Frame(TYPE_REQUEST, 1, 1, 0, b"abc")))
    wire[-2] ^= 0x80
    with pytest.raises(Exception):
        decode_frame(bytes(wire[:-1]))
    with pytest.raises(ValueError):
        encode_frame(Frame(TYPE_REQUEST, 1, 1, 0, b"x" * 501))


def sample_ble_config():
    return {
        "ble_mode": "hub", "ble_advertising_interval_ms": 160,
        "ble_scan_interval_ms": 160, "ble_scan_window_ms": 80,
        "ble_auto_start": True, "ble_retry_interval_ms": 1000,
        "ble_connection_timeout_ms": 5000, "ble_max_retries": 3,
        "ble_rx_message_timeout_ms": 1000, "ble_peers": (2, 3, 8),
    }


def test_ble_extension_round_trip_and_limits():
    config = sample_ble_config()
    encoded = encode_ble_extension(config)
    assert len(encoded) == 22
    decoded = decode_ble_extension(encoded)
    assert decoded["ble_mode"] == "hub"
    assert decoded["ble_auto_start"] is True
    assert decoded["ble_peers"] == (2, 3, 8)

    invalid = dict(config, ble_mode="idle")
    with pytest.raises(ValueError):
        encode_ble_extension(invalid)


def test_runtime_and_saved_config_decode():
    uwb_prefix = struct.pack("<HHBHHIHB", 100, 0xCAFE, 5, 16384, 16390, 250, 120, 2)
    ble = encode_ble_extension(sample_ble_config())
    runtime = decode_runtime_config(uwb_prefix + ble)
    assert runtime["device_id"] == 100
    assert runtime["channel"] == 5
    assert runtime["ble_peers"] == (2, 3, 8)

    saved_header = struct.pack("<HHBHHIHBBB", 100, 0xCAFE, 5, 16384, 16390,
                               250, 120, 2, 1, 2)
    targets = struct.pack("<HH", 200, 201)
    saved = decode_saved_config(saved_header + targets + b"\x01" + ble)
    assert saved["role"] == "initiator"
    assert saved["targets"] == (200, 201)
    assert saved["auto_start"] is True
    assert saved["ble_peers"] == (2, 3, 8)

    set_payload = encode_set_config(runtime)
    assert len(set_payload) == 9 + len(ble)


def test_status_and_range_measurement_decode():
    status = bytearray(27)
    status[:6] = bytes([1, 1, 1, 0, 2, 1])
    struct.pack_into("<III", status, 6, 1200, 42, 3)
    status[18:21] = bytes([1, 1, 1])
    struct.pack_into("<H", status, 21, 7)
    status[23:25] = bytes([0, 1])
    struct.pack_into("<H", status, 25, 185)
    decoded = decode_status(bytes(status))
    assert decoded["role"] == "initiator"
    assert decoded["ranging_enabled"] is True
    assert decoded["any_peer_connected"] is True
    assert decoded["representative_peer_id"] == 7

    record = struct.pack("<IHQQiBB", 8, 200, 123456, 1700000000000000,
                         2345, 0, 1)
    job_id, measurement = decode_range_result(struct.pack("<H", 22) + record)
    assert job_id == 22
    assert measurement.target_id == 200
    assert measurement.distance_m == pytest.approx(2.345)
    assert measurement.valid
    assert measurement.epoch_us == 1700000000000000


def test_history_decode_cursor_and_records():
    record = struct.pack("<IHQQiBB", 4, 10, 100, 0, 900, 0, 0)
    payload = struct.pack("<IIIBB", 1, 4, 4, 0, 1) + record
    page = decode_history(payload)
    assert page.oldest_seq == 1
    assert page.newest_seq == 4
    assert page.next_cursor == 4
    assert page.records[0].distance_m == pytest.approx(0.9)
