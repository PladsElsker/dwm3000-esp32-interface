"""Wire protocol primitives matching dwm3000-firmware/PROTOCOL.md."""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .exceptions import ProtocolError

VERSION = 1
TYPE_REQUEST, TYPE_RESPONSE, TYPE_EVENT = 1, 2, 3
MAX_PAYLOAD = 500

OPS = {
    "ping": 0x01, "get_info": 0x02, "get_status": 0x03,
    "get_statistics": 0x04, "clear_statistics": 0x05, "get_last_error": 0x06,
    "get_diagnostics": 0x07, "get_trace": 0x08, "subscribe": 0x09,
    "unsubscribe": 0x0A, "time_sync": 0x0B,
    "get_config": 0x10, "set_config": 0x11, "reset_config": 0x12,
    "get_saved_config": 0x13, "apply_save_config": 0x14,
    "get_antenna_delays": 0x15, "set_antenna_delays": 0x16,
    "save_antenna_delays": 0x17,
    "set_role": 0x20, "get_targets": 0x21, "set_targets": 0x22,
    "get_schedule": 0x23, "set_schedule": 0x24,
    "start_ranging": 0x25, "stop_ranging": 0x26, "range_once": 0x27,
    "cancel_operation": 0x28, "uwb_sleep": 0x29, "uwb_wake": 0x2A,
    "get_last_range": 0x30, "get_history": 0x31,
    "system_sleep": 0x40, "reboot": 0x41,
    "get_peers": 0x50, "set_peers": 0x51, "start_ble": 0x52,
    "stop_ble": 0x53, "get_peer_status": 0x54,
    "send_to_peer": 0x55, "get_received_data": 0x56,
}
EVENT_NAMES = {
    0x80: "range_result", 0x81: "range_failed", 0x82: "state_changed",
    0x83: "error", 0x84: "history_overrun", 0x85: "device_ready",
    0x86: "ble_state_changed", 0x87: "ble_peer_found",
    0x88: "ble_peer_connected", 0x89: "ble_peer_disconnected",
    0x8A: "ble_connection_failed", 0x8B: "ble_data_received",
    0x8C: "ble_error", 0x8D: "ble_data_sent", 0x8E: "ble_data_expired",
}
ERROR_NAMES = {
    0: "OK", 1: "BAD_ARGUMENT", 2: "UNKNOWN_COMMAND", 3: "BAD_STATE",
    4: "BUSY", 5: "NOT_SUPPORTED", 6: "TIMEOUT", 7: "RADIO_FAILURE",
    8: "RATE_UNACHIEVABLE", 9: "INTERNAL", 10: "NOT_FOUND",
}
ROLE_NAMES = {0: "idle", 1: "initiator", 2: "responder"}
BLE_MODE_NAMES = {0: "idle", 1: "hub", 2: "peripheral"}
RANGE_RECORD_SIZE = 28
RANGE_RECORD = struct.Struct("<IHQQiBB")


@dataclass(frozen=True, slots=True)
class Frame:
    type: int
    seq: int
    opcode: int
    flags: int
    payload: bytes


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_index, code = 0, 1
    for byte in data:
        if byte == 0:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
        else:
            out.append(byte)
            code += 1
            if code == 0xFF:
                out[code_index] = code
                code_index = len(out)
                out.append(0)
                code = 1
    out[code_index] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    if not data:
        raise ProtocolError("empty COBS frame")
    out = bytearray()
    index = 0
    while index < len(data):
        code = data[index]
        if code == 0:
            raise ProtocolError("zero byte inside COBS frame")
        index += 1
        end = index + code - 1
        if end > len(data):
            raise ProtocolError("truncated COBS block")
        out.extend(data[index:end])
        index = end
        if code != 0xFF and index < len(data):
            out.append(0)
    return bytes(out)


def encode_frame(frame: Frame) -> bytes:
    if len(frame.payload) > MAX_PAYLOAD:
        raise ValueError(f"payload exceeds {MAX_PAYLOAD} bytes")
    raw = struct.pack("<BBHBBH", VERSION, frame.type, frame.seq, frame.opcode,
                      frame.flags, len(frame.payload)) + frame.payload
    raw += struct.pack("<H", crc16(raw))
    return cobs_encode(raw) + b"\x00"


def decode_frame(packet: bytes) -> Frame:
    raw = cobs_decode(packet)
    if len(raw) < 10:
        raise ProtocolError("frame too short")
    version, frame_type, seq, opcode, flags, length = struct.unpack_from("<BBHBBH", raw)
    if version != VERSION:
        raise ProtocolError(f"unsupported protocol version {version}")
    if length > MAX_PAYLOAD or len(raw) != 10 + length:
        raise ProtocolError("invalid frame length")
    expected, = struct.unpack_from("<H", raw, 8 + length)
    if crc16(raw[:8 + length]) != expected:
        raise ProtocolError("CRC mismatch")
    if frame_type not in (TYPE_REQUEST, TYPE_RESPONSE, TYPE_EVENT):
        raise ProtocolError(f"unknown frame type {frame_type}")
    return Frame(frame_type, seq, opcode, flags, raw[8:8 + length])


def decode_range_record(data: bytes, received_at: float | None = None):
    import time
    from .models import DistanceMeasurement
    if len(data) != RANGE_RECORD_SIZE:
        raise ProtocolError(f"measurement record must be {RANGE_RECORD_SIZE} bytes")
    sample_seq, target_id, mono_us, epoch_us, range_mm, status, quality = RANGE_RECORD.unpack(data)
    return DistanceMeasurement(
        target_id=target_id, distance_m=None if range_mm < 0 else range_mm / 1000.0,
        sample_sequence=sample_seq, device_monotonic_us=mono_us,
        epoch_us=epoch_us or None, status=status, quality_flags=quality,
        received_at=time.monotonic() if received_at is None else received_at,
    )


def decode_range_result(payload: bytes):
    if len(payload) != 2 + RANGE_RECORD_SIZE:
        raise ProtocolError("RANGE_RESULT payload must be job_id plus a 28-byte record")
    job_id, = struct.unpack_from("<H", payload)
    return job_id, decode_range_record(payload[2:])


def decode_range_failed(payload: bytes):
    if len(payload) != 13:
        raise ProtocolError("RANGE_FAILED payload must be 13 bytes")
    job_id, target_id, status, mono_us = struct.unpack("<HHBQ", payload)
    from .models import RangeFailure
    import time
    return RangeFailure(job_id, target_id, status, mono_us, time.monotonic())


def decode_ble_extension(data: bytes) -> dict:
    if len(data) < 16:
        raise ProtocolError("BLE configuration extension is shorter than 16 bytes")
    mode, adv, scan_interval, scan_window, auto_start, retry_interval, conn_timeout, retries, rx_timeout, peer_count = struct.unpack_from(
        "<BHHHBHHBHB", data
    )
    if peer_count > 8 or len(data) != 16 + peer_count * 2:
        raise ProtocolError("invalid BLE configuration extension length")
    peers = list(struct.unpack_from("<" + "H" * peer_count, data, 16)) if peer_count else []
    if mode not in BLE_MODE_NAMES or auto_start not in (0, 1):
        raise ProtocolError("invalid BLE mode or auto-start flag")
    if any(peer in (0, 0xFFFF) for peer in peers) or len(set(peers)) != len(peers):
        raise ProtocolError("invalid BLE peer allow-list")
    return {
        "ble_mode": BLE_MODE_NAMES[mode],
        "ble_advertising_interval_ms": adv,
        "ble_scan_interval_ms": scan_interval,
        "ble_scan_window_ms": scan_window,
        "ble_auto_start": bool(auto_start),
        "ble_retry_interval_ms": retry_interval,
        "ble_connection_timeout_ms": conn_timeout,
        "ble_max_retries": retries,
        "ble_rx_message_timeout_ms": rx_timeout,
        "ble_peers": tuple(peers),
    }


def encode_ble_extension(config: dict) -> bytes:
    mode = {"idle": 0, "hub": 1, "peripheral": 2}.get(str(config.get("ble_mode", "idle")).lower())
    if mode is None:
        raise ValueError("ble_mode must be idle, hub, or peripheral")
    peers = tuple(int(peer) for peer in config.get("ble_peers", ()))
    if len(peers) > 8 or len(set(peers)) != len(peers) or any(peer < 1 or peer > 65534 for peer in peers):
        raise ValueError("ble_peers must contain up to eight unique IDs in 1..65534")
    adv = int(config.get("ble_advertising_interval_ms", 100))
    scan_i = int(config.get("ble_scan_interval_ms", 100))
    scan_w = int(config.get("ble_scan_window_ms", 50))
    retry_i = int(config.get("ble_retry_interval_ms", 1000))
    timeout = int(config.get("ble_connection_timeout_ms", 5000))
    retries = int(config.get("ble_max_retries", 3))
    rx_timeout = int(config.get("ble_rx_message_timeout_ms", 1000))
    auto_start = int(bool(config.get("ble_auto_start", False)))
    if not 20 <= adv <= 10240 or not 10 <= scan_i <= 10240 or not 10 <= scan_w <= scan_i:
        raise ValueError("invalid BLE advertising/scan timing")
    if not 100 <= retry_i <= 60000 or not 100 <= timeout <= 30000:
        raise ValueError("invalid BLE connection timing")
    if not 0 <= retries <= 5 or not 100 <= rx_timeout <= 2000:
        raise ValueError("invalid BLE retries/message timeout")
    if auto_start and mode == 0:
        raise ValueError("BLE auto-start requires a non-IDLE mode")
    return struct.pack("<BHHHBHHBHB", mode, adv, scan_i, scan_w, auto_start,
                       retry_i, timeout, retries, rx_timeout, len(peers)) + (
        struct.pack("<" + "H" * len(peers), *peers) if peers else b""
    )


def decode_runtime_config(payload: bytes) -> dict:
    if len(payload) < 32:
        raise ProtocolError("GET_CONFIG response is too short")
    device_id, pan_id, channel, tx_delay, rx_delay, period, timeout, retries = struct.unpack_from(
        "<HHBHHIHB", payload
    )
    return {
        "device_id": device_id, "pan_id": pan_id, "channel": channel,
        "tx_antenna_delay": tx_delay, "rx_antenna_delay": rx_delay,
        "period_ms": period, "timeout_ms": timeout, "retries": retries,
        **decode_ble_extension(payload[16:]),
    }


def decode_saved_config(payload: bytes) -> dict:
    if len(payload) < 19:
        raise ProtocolError("GET_SAVED_CONFIG response is too short")
    device_id, pan_id, channel, tx_delay, rx_delay, period, timeout, retries, role, count = struct.unpack_from(
        "<HHBHHIHBBB", payload
    )
    if count > 16:
        raise ProtocolError("saved target count exceeds firmware maximum")
    base = 18 + 2 * count
    if len(payload) < base + 1 + 16:
        raise ProtocolError("saved configuration is truncated")
    targets = struct.unpack_from("<" + "H" * count, payload, 18) if count else ()
    auto_start = payload[base]
    if auto_start not in (0, 1):
        raise ProtocolError("invalid UWB auto-start flag")
    return {
        "device_id": device_id, "pan_id": pan_id, "channel": channel,
        "tx_antenna_delay": tx_delay, "rx_antenna_delay": rx_delay,
        "period_ms": period, "timeout_ms": timeout, "retries": retries,
        "role": ROLE_NAMES.get(role, "idle"), "targets": tuple(targets),
        "auto_start": bool(auto_start),
        **decode_ble_extension(payload[base + 1:]),
    }


def encode_set_config(config: dict) -> bytes:
    device_id, pan_id, channel = int(config["device_id"]), int(config["pan_id"]), int(config["channel"])
    tx_delay, rx_delay = int(config["tx_antenna_delay"]), int(config["rx_antenna_delay"])
    if device_id in (0, 0xFFFF) or channel not in (5, 9):
        raise ValueError("invalid device_id or channel")
    return struct.pack("<HHBHH", device_id, pan_id, channel, tx_delay, rx_delay) + encode_ble_extension(config)


def decode_targets(payload: bytes) -> tuple[int, ...]:
    if not payload:
        raise ProtocolError("target list missing count")
    count = payload[0]
    if count > 16 or len(payload) != 1 + count * 2:
        raise ProtocolError("invalid target list length")
    return tuple(struct.unpack_from("<H", payload, 1 + i * 2)[0] for i in range(count))


def encode_targets(targets) -> bytes:
    values = tuple(int(x) for x in targets)
    if len(values) > 16 or len(set(values)) != len(values):
        raise ValueError("targets must contain at most 16 unique IDs")
    if any(x in (0, 0xFFFF) for x in values):
        raise ValueError("target IDs must be in 1..65534")
    return bytes([len(values)]) + (struct.pack("<" + "H" * len(values), *values) if values else b"")


def decode_schedule(payload: bytes) -> dict:
    if len(payload) != 18:
        raise ProtocolError("GET_UWB_SCHEDULE response must be 18 bytes")
    return {
        "period_ms": struct.unpack_from("<I", payload, 0)[0],
        "timeout_ms": struct.unpack_from("<H", payload, 4)[0],
        "retries": payload[6], "target_count": payload[7],
        "queued_jobs": payload[8], "transaction_active": bool(payload[9]),
        "effective_period_per_target_ms": struct.unpack_from("<I", payload, 10)[0],
        "average_transaction_ms": struct.unpack_from("<I", payload, 14)[0],
    }


def decode_status(payload: bytes) -> dict:
    if len(payload) != 27:
        raise ProtocolError("GET_STATUS response must be 27 bytes")
    role, ranging, radio_ready, standby, target_count, synchronized = struct.unpack_from("<6B", payload)
    uptime, ranges_ok, ranges_failed = struct.unpack_from("<III", payload, 6)
    ble_mode, ble_enabled, any_peer = struct.unpack_from("<3B", payload, 18)
    peer_id, = struct.unpack_from("<H", payload, 21)
    advertising, scanning = struct.unpack_from("<BB", payload, 23)
    mtu, = struct.unpack_from("<H", payload, 25)
    return {
        "role": ROLE_NAMES.get(role, "unknown"), "ranging_enabled": bool(ranging),
        "radio_ready": bool(radio_ready), "radio_standby": bool(standby),
        "target_count": target_count, "clock_synchronized": bool(synchronized),
        "uptime_ms": uptime, "ranges_ok": ranges_ok, "ranges_failed": ranges_failed,
        "ble_mode": BLE_MODE_NAMES.get(ble_mode, "unknown"), "ble_enabled": bool(ble_enabled),
        "any_peer_connected": bool(any_peer), "representative_peer_id": peer_id,
        "advertising": bool(advertising), "scanning": bool(scanning),
        "representative_peer_mtu": mtu,
    }


def decode_history(payload: bytes):
    if len(payload) < 14:
        raise ProtocolError("GET_HISTORY response shorter than header")
    oldest, newest, cursor = struct.unpack_from("<III", payload)
    overrun, count = payload[12], payload[13]
    if len(payload) != 14 + count * RANGE_RECORD_SIZE:
        raise ProtocolError("GET_HISTORY record count/length mismatch")
    records = tuple(
        decode_range_record(payload[14 + i * RANGE_RECORD_SIZE:14 + (i + 1) * RANGE_RECORD_SIZE])
        for i in range(count)
    )
    from .models import UwbHistoryPage
    return UwbHistoryPage(oldest, newest, cursor, bool(overrun), records)



def decode_event_payload(opcode: int, payload: bytes):
    """Decode non-BLE asynchronous event payloads into simple typed records."""
    if opcode == 0x82 and len(payload) == 4:
        role, ranging_enabled, radio_standby, radio_ready = payload
        return {
            "role": ROLE_NAMES.get(role, "unknown"),
            "ranging_enabled": bool(ranging_enabled),
            "radio_standby": bool(radio_standby),
            "radio_ready": bool(radio_ready),
        }
    if opcode == 0x83 and len(payload) == 9:
        error_code, monotonic_us = struct.unpack("<BQ", payload)
        return {"error_code": error_code, "monotonic_us": monotonic_us}
    if opcode == 0x84 and len(payload) == 8:
        overwritten, newest = struct.unpack("<II", payload)
        return {"overwritten_sample_seq": overwritten, "new_sample_seq": newest}
    if opcode == 0x85 and len(payload) == 2:
        radio_ready, protocol_version = payload
        return {"radio_ready": bool(radio_ready), "protocol_version": protocol_version}
    return payload



def pack_u16(value: int) -> bytes:
    value = int(value)
    if not 0 <= value <= 0xFFFF:
        raise ValueError("value must fit uint16")
    return struct.pack("<H", value)


def unpack_u16(data: bytes, offset: int = 0) -> int:
    if offset < 0 or len(data) < offset + 2:
        raise ProtocolError("truncated uint16")
    return struct.unpack_from("<H", data, offset)[0]
