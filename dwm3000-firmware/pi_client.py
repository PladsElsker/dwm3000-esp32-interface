#!/usr/bin/env python3
"""Host client for the UWB Node V2.1 UART protocol (frame version 1).

Install: python3 -m pip install pyserial
Example: python3 pi_client.py --port /dev/serial0 ping
"""
from __future__ import annotations
import argparse
import json
import queue
import struct
import sys
import threading
import time
from dataclasses import dataclass
from typing import Optional

try:
    import serial
except ImportError:
    serial = None

VERSION = 1
TYPE_REQUEST, TYPE_RESPONSE, TYPE_EVENT = 1, 2, 3
MAX_PAYLOAD = 500

OPS = {
    "ping": 0x01, "get-info": 0x02, "status": 0x03, "stats": 0x04,
    "clear-stats": 0x05, "last-error": 0x06, "get-config": 0x10, "get-saved-config": 0x17,
    "set-config": 0x11, "reset-config": 0x13,
    "apply-save-config": 0x18,
    "time-sync": 0x14, "get-ant-delays": 0x15, "set-ant-delays": 0x16,
    "set-role": 0x20, "set-targets": 0x21, "get-targets": 0x22,
    "set-schedule": 0x23, "get-schedule": 0x24, "start": 0x25,
    "stop": 0x26, "range-once": 0x27, "cancel": 0x28,
    "get-last": 0x30, "get-history": 0x31, "subscribe": 0x32,
    "unsubscribe": 0x33, "clear-history": 0x34, "diagnostics": 0x35,
    "uwb-sleep": 0x40, "uwb-wake": 0x41, "system-sleep": 0x42,
    "reboot": 0x43,
    "set-peers": 0x50, "get-peers": 0x51, "start-ble": 0x52,
    "stop-ble": 0x53, "peer-status": 0x56,
    "send-to-peer": 0x57, "get-received-data": 0x58,
}
EVENT_NAMES = {0x80: "RANGE_RESULT", 0x81: "RANGE_FAILED", 0x82: "STATE_CHANGED",
               0x83: "ERROR", 0x84: "HISTORY_OVERRUN", 0x85: "DEVICE_READY",
               0x86: "BLE_STATE_CHANGED", 0x87: "BLE_PEER_FOUND",
               0x88: "BLE_PEER_CONNECTED", 0x89: "BLE_PEER_DISCONNECTED",
               0x8A: "BLE_CONNECTION_FAILED", 0x8B: "BLE_DATA_RECEIVED",
               0x8C: "BLE_ERROR", 0x8D: "BLE_DATA_SENT", 0x8E: "BLE_DATA_EXPIRED"}
ERROR_NAMES = {0: "OK", 1: "BAD_ARGUMENT", 2: "UNKNOWN_COMMAND", 3: "BAD_STATE",
               4: "BUSY", 5: "NOT_SUPPORTED", 6: "TIMEOUT", 7: "RADIO_FAILURE",
               8: "RATE_UNACHIEVABLE", 9: "INTERNAL", 10: "NOT_FOUND"}
ROLE_NAMES = {0: "idle", 1: "initiator", 2: "responder"}
BLE_MODE_NAMES = {0: "idle", 1: "hub", 2: "peripheral"}
BLE_PEER_STATE_NAMES = {
    0: "unknown", 1: "configured", 2: "found", 3: "connected",
    4: "connecting", 5: "handshaking", 6: "backoff",
}
RECORD_SIZE = 28


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for x in data:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_index = 0
    code = 1
    for b in data:
        if b == 0:
            out[code_index] = code
            code_index = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_index] = code
                code_index = len(out)
                out.append(0)
                code = 1
    out[code_index] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        if code == 0:
            raise ValueError("zero byte inside COBS frame")
        i += 1
        end = i + code - 1
        if end > len(data):
            raise ValueError("truncated COBS frame")
        out.extend(data[i:end])
        i = end
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


@dataclass
class Frame:
    type: int
    seq: int
    opcode: int
    flags: int
    payload: bytes


def encode_frame(frame: Frame) -> bytes:
    if len(frame.payload) > MAX_PAYLOAD:
        raise ValueError("payload exceeds max")
    raw = struct.pack("<BBHBBH", VERSION, frame.type, frame.seq, frame.opcode,
                      frame.flags, len(frame.payload)) + frame.payload
    raw += struct.pack("<H", crc16(raw))
    return cobs_encode(raw) + b"\x00"


def decode_frame(packet: bytes) -> Frame:
    raw = cobs_decode(packet)
    if len(raw) < 10:
        raise ValueError("frame too short")
    version, typ, seq, opcode, flags, length = struct.unpack_from("<BBHBBH", raw)
    if version != VERSION:
        raise ValueError(f"protocol version {version} unsupported")
    if length > MAX_PAYLOAD or len(raw) != 8 + length + 2:
        raise ValueError("bad frame length")
    got_crc, = struct.unpack_from("<H", raw, 8 + length)
    if crc16(raw[:8 + length]) != got_crc:
        raise ValueError("CRC mismatch")
    return Frame(typ, seq, opcode, flags, raw[8:8 + length])


def decode_record(buf: bytes) -> dict:
    if len(buf) != RECORD_SIZE:
        raise ValueError("bad measurement record length")
    sample_seq, target_id, mono_us, epoch_us, range_mm, status, quality = struct.unpack("<IHQQiBB", buf)
    return {"sample_seq": sample_seq, "target_id": target_id, "mono_us": mono_us,
            "epoch_us": epoch_us or None, "range_mm": None if range_mm < 0 else range_mm,
            "status": status, "quality_flags": quality}


def encode_ble_config(args: argparse.Namespace) -> bytes:
    """Encode the fixed 16-byte BLE config extension followed by peer IDs."""
    mode = {"idle": 0, "hub": 1, "peripheral": 2}[args.ble_mode]
    peers = list(args.ble_peers or [])
    if len(peers) > 8 or any(x < 1 or x > 65534 for x in peers) or len(set(peers)) != len(peers):
        raise ValueError("BLE peer IDs must be unique integers 1..65534 (max 8)")
    if not 20 <= args.ble_advertising_interval_ms <= 10240:
        raise ValueError("BLE advertising interval must be 20..10240 ms")
    if not 10 <= args.ble_scan_interval_ms <= 10240:
        raise ValueError("BLE scan interval must be 10..10240 ms")
    if not 10 <= args.ble_scan_window_ms <= args.ble_scan_interval_ms:
        raise ValueError("BLE scan window must be 10 ms..scan interval")
    if not 100 <= args.ble_retry_interval_ms <= 60000:
        raise ValueError("BLE retry interval must be 100..60000 ms")
    if not 100 <= args.ble_connection_timeout_ms <= 30000:
        raise ValueError("BLE connection timeout must be 100..30000 ms")
    if not 0 <= args.ble_retries <= 5:
        raise ValueError("BLE retries must be 0..5")
    if not 100 <= args.ble_rx_message_timeout_ms <= 2000:
        raise ValueError("BLE RX message timeout must be 100..2000 ms")
    ble_auto_start = bool(getattr(args, "ble_auto_start", False))
    if ble_auto_start and mode == 0:
        raise ValueError("BLE auto-start requires HUB or PERIPHERAL mode")
    fixed = struct.pack(
        "<BHHHBHHBHB",
        mode, args.ble_advertising_interval_ms, args.ble_scan_interval_ms,
        args.ble_scan_window_ms, int(ble_auto_start),
        args.ble_retry_interval_ms, args.ble_connection_timeout_ms,
        args.ble_retries, args.ble_rx_message_timeout_ms, len(peers))
    return fixed + (struct.pack("<" + "H" * len(peers), *peers) if peers else b"")


def decode_ble_config(payload: bytes) -> dict:
    if len(payload) < 16:
        return {}
    mode, adv, scan_i, scan_w, auto_start, retry_i, conn_timeout, retries, rx_timeout, count = struct.unpack_from(
        "<BHHH BHH BHB", payload)
    if count > 8 or len(payload) != 16 + count * 2:
        raise ValueError("invalid BLE config extension length")
    peers = list(struct.unpack_from("<" + "H" * count, payload, 16)) if count else []
    if mode not in BLE_MODE_NAMES or auto_start not in (0, 1):
        raise ValueError("invalid BLE mode or auto-start flag")
    if not 20 <= adv <= 10240 or not 10 <= scan_i <= 10240 or not 10 <= scan_w <= scan_i:
        raise ValueError("invalid BLE advertising or scan interval")
    if not 100 <= retry_i <= 60000 or not 100 <= conn_timeout <= 30000:
        raise ValueError("invalid BLE connection timing")
    if not 0 <= retries <= 5 or not 100 <= rx_timeout <= 2000:
        raise ValueError("invalid BLE retry or receive timeout setting")
    if any(peer in (0, 0xFFFF) for peer in peers) or len(set(peers)) != len(peers):
        raise ValueError("invalid BLE peer list")
    return {"ble_mode": BLE_MODE_NAMES.get(mode, mode), "ble_advertising_interval_ms": adv,
            "ble_scan_interval_ms": scan_i, "ble_scan_window_ms": scan_w,
            "ble_auto_start": bool(auto_start), "ble_retry_interval_ms": retry_i, "ble_connection_timeout_ms": conn_timeout,
            "ble_max_retries": retries,
            "ble_rx_message_timeout_ms": rx_timeout, "ble_peers": peers}


def fragment_ble_message(message_id: int, data: bytes, att_mtu: int) -> list[bytes]:
    """Create DATA fragments using the DWM3000 BLE packet wire layout."""
    if not 0 <= message_id <= 0xFFFF:
        raise ValueError("message_id must fit uint16")
    if not 23 <= att_mtu <= 256:
        raise ValueError("ATT MTU must be in 23..256 (firmware negotiated-MTU cap)")
    if len(data) > 496:
        raise ValueError("application message exceeds 496 bytes")
    capacity = att_mtu - 12  # ATT(3) + DATA header(7) + CRC(2)
    count = max(1, (len(data) + capacity - 1) // capacity)
    if count > 46:
        raise ValueError("fragment count exceeds 46")
    packets = []
    for index in range(count):
        chunk = data[index * capacity:(index + 1) * capacity]
        packet = struct.pack("<BHBBH", 0x01, message_id, index, count, len(data)) + chunk
        packet += struct.pack("<H", crc16(packet))
        packets.append(packet)
    return packets


def decode_ble_packet(packet: bytes) -> dict:
    if len(packet) < 5 or crc16(packet[:-2]) != struct.unpack_from("<H", packet, len(packet) - 2)[0]:
        raise ValueError("invalid BLE packet CRC or length")
    kind = packet[0]
    if kind == 0x01 and len(packet) >= 9:
        message_id, index, count, total = struct.unpack_from("<HBBH", packet, 1)
        if count < 1 or count > 46 or index >= count or total > 496:
            raise ValueError("invalid DATA metadata")
        return {"type": "DATA", "message_id": message_id, "fragment_index": index,
                "fragment_count": count, "total_length": total, "payload": packet[7:-2]}
    if kind == 0x02 and len(packet) == 6:
        return {"type": "ACK", "message_id": struct.unpack_from("<H", packet, 1)[0],
                "fragment_index": packet[3]}
    if kind == 0x03 and len(packet) == 7:
        return {"type": "NACK", "message_id": struct.unpack_from("<H", packet, 1)[0],
                "fragment_index": packet[3], "error_code": packet[4]}
    if kind == 0x04 and len(packet) == 6:
        return {"type": "MESSAGE_RESULT", "message_id": struct.unpack_from("<H", packet, 1)[0],
                "status": packet[3]}
    if kind == 0x05 and len(packet) == 5:
        return {"type": "MESSAGE_RESULT_ACK", "message_id": struct.unpack_from("<H", packet, 1)[0]}
    raise ValueError("unknown BLE packet type or invalid packet length")


class UwbClient:
    def __init__(self, port: str, baud: int = 460800, timeout: float = 1.5, retries: int = 1):
        if serial is None:
            raise RuntimeError("pyserial is required: python3 -m pip install pyserial")
        self.ser = serial.Serial(port, baudrate=baud, timeout=0.05, write_timeout=1)
        self.timeout = timeout
        self.retries = max(0, int(retries))
        self.seq = 1
        self.lock = threading.Lock()
        self.pending: dict[int, queue.Queue[Frame]] = {}
        self.events: queue.Queue[Frame] = queue.Queue()
        self.running = True
        self.rx_errors = 0
        self.reader = threading.Thread(target=self._reader, daemon=True)
        self.reader.start()

    def close(self):
        self.running = False
        self.reader.join(timeout=0.5)
        self.ser.close()

    def _reader(self):
        buff = bytearray()
        while self.running:
            data = self.ser.read(256)
            if not data:
                continue
            for b in data:
                if b == 0:
                    if buff:
                        try:
                            f = decode_frame(bytes(buff))
                            if f.type == TYPE_RESPONSE:
                                q = self.pending.get(f.seq)
                                if q is not None:
                                    q.put(f)
                            elif f.type == TYPE_EVENT:
                                self.events.put(f)
                        except Exception:
                            self.rx_errors += 1
                        buff.clear()
                else:
                    if len(buff) < 600:
                        buff.append(b)
                    else:
                        buff.clear()
                        self.rx_errors += 1

    def request(self, opcode: int, payload: bytes = b"", timeout: Optional[float] = None) -> Frame:
        with self.lock:
            seq = self.seq & 0xFFFF
            self.seq = (self.seq + 1) & 0xFFFF
            if self.seq == 0:
                self.seq = 1
            waiter: queue.Queue[Frame] = queue.Queue(maxsize=1)
            self.pending[seq] = waiter
            packet = encode_frame(Frame(TYPE_REQUEST, seq, opcode, 0, payload))
            self.ser.write(packet)
        wait_s = self.timeout if timeout is None else timeout
        try:
            resp = None
            for attempt in range(self.retries + 1):
                if attempt:
                    # Same sequence/opcode/payload: device-side duplicate cache makes
                    # retries safe for commands such as RANGE_ONCE.
                    self.ser.write(packet)
                try:
                    resp = waiter.get(timeout=wait_s)
                    break
                except queue.Empty:
                    if attempt == self.retries:
                        raise TimeoutError(f"timeout waiting for opcode 0x{opcode:02x} response")
            if resp is None:
                raise TimeoutError(f"timeout waiting for opcode 0x{opcode:02x} response")
        finally:
            self.pending.pop(seq, None)
        if resp.flags != 0:
            raise RuntimeError(f"ESP32 error {ERROR_NAMES.get(resp.flags, resp.flags)} (0x{resp.flags:02x})")
        return resp

    def poll_event(self, timeout: float = 0.5) -> Optional[Frame]:
        try:
            return self.events.get(timeout=timeout)
        except queue.Empty:
            return None


def parse_args():
    ap = argparse.ArgumentParser(description="XIAO ESP32-C3 / DWM3000 host protocol client")
    ap.add_argument("--port", default="/dev/serial0", help="UART device, default /dev/serial0")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--timeout", type=float, default=1.5)
    sub = ap.add_subparsers(dest="command", required=True)
    for name in ("ping", "get-info", "status", "stats", "clear-stats", "last-error", "get-config",
                 "get-saved-config", "reset-config", "get-ant-delays", "get-targets", "get-schedule", "start",
                 "stop", "clear-history", "diagnostics", "uwb-sleep", "uwb-wake", "reboot",
                 "get-peers", "start-ble", "stop-ble"):
        sub.add_parser(name)
    p = sub.add_parser("set-role"); p.add_argument("role", choices=ROLE_NAMES.values())
    p = sub.add_parser("set-targets"); p.add_argument("ids", nargs="+", type=int)
    p = sub.add_parser("set-schedule"); p.add_argument("--period-ms", type=int, default=100); p.add_argument("--timeout-ms", type=int, default=120); p.add_argument("--retries", type=int, default=1)
    p = sub.add_parser("range-once"); p.add_argument("target", type=int)
    p = sub.add_parser("cancel"); p.add_argument("job_id", type=int)
    p = sub.add_parser("get-last"); p.add_argument("--target", type=int, default=65535)
    p = sub.add_parser("get-history"); p.add_argument("--after", type=int, default=0); p.add_argument("--n", type=int, default=16); p.add_argument("--target", type=int, default=65535)
    p = sub.add_parser("subscribe"); p.add_argument("--mask", type=lambda x: int(x, 0), default=0x7FFF)
    p = sub.add_parser("unsubscribe"); p.add_argument("--mask", type=lambda x: int(x, 0), default=0xFFFF)
    p = sub.add_parser("system-sleep"); p.add_argument("ms", type=int)
    p = sub.add_parser("set-ant-delays"); p.add_argument("tx", type=int); p.add_argument("rx", type=int)
    p = sub.add_parser("time-sync")
    p = sub.add_parser("set-config"); p.add_argument("device_id", type=int); p.add_argument("pan_id", type=lambda x: int(x, 0)); p.add_argument("channel", type=int, choices=(5, 9)); p.add_argument("tx_delay", type=int); p.add_argument("rx_delay", type=int)
    p.add_argument("--ble-mode", choices=BLE_MODE_NAMES.values())
    p.add_argument("--ble-advertising-interval-ms", type=int, default=160)
    p.add_argument("--ble-scan-interval-ms", type=int, default=160)
    p.add_argument("--ble-scan-window-ms", type=int, default=80)
    p.add_argument("--ble-auto-start", action="store_true",
                   help="automatically start the configured BLE role after boot")
    p.add_argument("--ble-retry-interval-ms", type=int, default=1000,
                   help="delay between automatic connection attempts (100..60000 ms)")
    p.add_argument("--ble-connection-timeout-ms", type=int, default=5000)
    p.add_argument("--ble-retries", type=int, default=1)
    p.add_argument("--ble-rx-message-timeout-ms", type=int, default=1000)
    p.add_argument("--ble-peers", type=int, nargs="*", default=[])
    p = sub.add_parser("apply-save-config")
    p.add_argument("device_id", type=int); p.add_argument("pan_id", type=lambda x: int(x, 0))
    p.add_argument("channel", type=int, choices=(5, 9)); p.add_argument("tx_delay", type=int); p.add_argument("rx_delay", type=int)
    p.add_argument("--period-ms", type=int, default=100); p.add_argument("--timeout-ms", type=int, default=120)
    p.add_argument("--retries", type=int, default=1); p.add_argument("--role", choices=ROLE_NAMES.values(), default="idle")
    p.add_argument("--targets", type=int, nargs="*", default=[]); p.add_argument("--auto-start", action="store_true")
    p.add_argument("--ble-mode", choices=BLE_MODE_NAMES.values(), default="idle")
    p.add_argument("--ble-advertising-interval-ms", type=int, default=160)
    p.add_argument("--ble-scan-interval-ms", type=int, default=160); p.add_argument("--ble-scan-window-ms", type=int, default=80)
    p.add_argument("--ble-auto-start", action="store_true",
                   help="automatically start the configured BLE role after boot")
    p.add_argument("--ble-retry-interval-ms", type=int, default=1000,
                   help="delay between automatic connection attempts (100..60000 ms)")
    p.add_argument("--ble-connection-timeout-ms", type=int, default=5000)
    p.add_argument("--ble-retries", type=int, default=1,
                   help="additional message-delivery attempts (0..5)")
    p.add_argument("--ble-rx-message-timeout-ms", type=int, default=1000); p.add_argument("--ble-peers", type=int, nargs="*", default=[])
    p = sub.add_parser("set-peers"); p.add_argument("ids", nargs="*", type=int)
    p = sub.add_parser("peer-status"); p.add_argument("peer_id", type=int)
    p = sub.add_parser("send-to-peer"); p.add_argument("peer_id", type=int); p.add_argument("data"); p.add_argument("--hex", action="store_true", help="interpret data as hexadecimal bytes")
    p = sub.add_parser("get-received-data"); p.add_argument("peer_id", type=int); p.add_argument("message_id", type=int)
    return ap.parse_args()


def record_response(op: int, payload: bytes) -> object:
    if op == 0x01:
        return {"pong": payload == b"PONG"}
    if op == 0x02 and len(payload) >= 12:
        return {"firmware": f"{payload[0]}.{payload[1]}.{payload[2]}", "protocol_version": payload[3],
                "device_id": struct.unpack_from("<H", payload, 4)[0], "max_targets": payload[6],
                "history_capacity": struct.unpack_from("<H", payload, 7)[0], "radio_type": payload[9],
                "max_payload": struct.unpack_from("<H", payload, 10)[0]}
    if op == 0x03 and len(payload) >= 18:
        result = {"role": ROLE_NAMES.get(payload[0], payload[0]), "ranging_enabled": bool(payload[1]),
                  "radio_ready": bool(payload[2]), "radio_standby": bool(payload[3]),
                  "target_count": payload[4], "clock_synchronized": bool(payload[5]),
                  "uptime_ms": struct.unpack_from("<I", payload, 6)[0],
                  "ranges_ok": struct.unpack_from("<I", payload, 10)[0],
                  "ranges_failed": struct.unpack_from("<I", payload, 14)[0]}
        if len(payload) >= 27:
            result.update(ble_mode=BLE_MODE_NAMES.get(payload[18], payload[18]),
                          ble_enabled=bool(payload[19]), ble_any_peer_connected=bool(payload[20]),
                          ble_representative_peer_id=struct.unpack_from("<H", payload, 21)[0],
                          ble_advertising=bool(payload[23]), ble_scanning=bool(payload[24]),
                          ble_representative_peer_mtu=struct.unpack_from("<H", payload, 25)[0])
        return result
    if op == 0x10 and len(payload) >= 16:
        dev, pan = struct.unpack_from("<HH", payload); chan = payload[4]; tx, rx = struct.unpack_from("<HH", payload, 5)
        period, timeout = struct.unpack_from("<IH", payload, 9)
        result = {"device_id": dev, "pan_id": pan, "channel": chan, "tx_antenna_delay": tx,
                  "rx_antenna_delay": rx, "target_period_ms": period, "timeout_ms": timeout, "retries": payload[15]}
        if len(payload) > 16:
            result.update(decode_ble_config(payload[16:]))
        return result
    if op == 0x17 and len(payload) >= 19:
        count = payload[17]
        base = 19 + count * 2
        if count > 16 or len(payload) < base:
            return {"parse_error": "invalid saved configuration"}
        ids = list(struct.unpack_from("<" + "H" * count, payload, 18)) if count else []
        result = {"device_id": struct.unpack_from("<H", payload)[0],
                  "pan_id": struct.unpack_from("<H", payload, 2)[0], "channel": payload[4],
                  "tx_antenna_delay": struct.unpack_from("<H", payload, 5)[0],
                  "rx_antenna_delay": struct.unpack_from("<H", payload, 7)[0],
                  "target_period_ms": struct.unpack_from("<I", payload, 9)[0],
                  "timeout_ms": struct.unpack_from("<H", payload, 13)[0], "retries": payload[15],
                  "role": ROLE_NAMES.get(payload[16], payload[16]), "targets": ids,
                  "auto_start": bool(payload[18 + 2 * count])}
        if len(payload) > base:
            result.update(decode_ble_config(payload[base:]))
        return result
    if op == 0x50 and len(payload) == 0:
        return {"ok": True}
    if op == 0x51 and payload:
        count = payload[0]
        if len(payload) != 1 + count * 2:
            return {"parse_error": "peer list length mismatch"}
        return {"peers": list(struct.unpack_from("<" + "H" * count, payload, 1)) if count else []}
    if op == 0x56 and len(payload) == 10:
        return {"state": BLE_PEER_STATE_NAMES.get(payload[0], payload[0]),
                "mode": BLE_MODE_NAMES.get(payload[1], payload[1]), "enabled": bool(payload[2]),
                "advertising": bool(payload[3]), "scanning": bool(payload[4]),
                "connected": bool(payload[5]), "peer_id": struct.unpack_from("<H", payload, 6)[0],
                "mtu": struct.unpack_from("<H", payload, 8)[0]}
    if op == 0x57 and len(payload) == 2:
        return {"message_id": struct.unpack("<H", payload)[0], "accepted_for_transmission": True}
    if op == 0x58 and len(payload) >= 4:
        message_id, length = struct.unpack_from("<HH", payload)
        data = payload[4:]
        if len(data) != length:
            return {"parse_error": "received-data length mismatch"}
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = None
        return {"message_id": message_id, "length": length, "data_hex": data.hex(), "data_utf8": text}
    if op in (0x15, 0x16) and len(payload) == 4:
        return {"tx_antenna_delay": struct.unpack_from("<H", payload)[0], "rx_antenna_delay": struct.unpack_from("<H", payload, 2)[0]}
    if op == 0x22 and payload:
        cnt = payload[0]; return {"targets": list(struct.unpack_from("<" + "H" * cnt, payload, 1)) if cnt else []}
    if op == 0x24 and len(payload) == 18:
        period, timeout = struct.unpack_from("<IH", payload); estimated, avg = struct.unpack_from("<II", payload, 10)
        return {"requested_period_ms": period, "timeout_ms": timeout, "retries": payload[6],
                "target_count": payload[7], "queued_jobs": payload[8], "transaction_active": bool(payload[9]),
                "estimated_effective_period_per_target_ms": estimated, "average_transaction_ms": avg}
    if op == 0x27 and len(payload) == 2:
        return {"job_id": struct.unpack("<H", payload)[0]}
    if op in (0x30, 0x31):
        if op == 0x30:
            if not payload: return {"records": []}
            count = payload[0]; start = 1; meta = {}
        else:
            oldest, newest, cursor = struct.unpack_from("<III", payload); overrun, count = payload[12], payload[13]; start = 14
            meta = {"oldest_seq": oldest, "newest_seq": newest, "next_cursor": cursor, "overrun": bool(overrun)}
        records = []
        for i in range(count): records.append(decode_record(payload[start + i * RECORD_SIZE:start + (i + 1) * RECORD_SIZE]))
        return {**meta, "records": records}
    if op == 0x32 or op == 0x33:
        return {"subscription_mask": struct.unpack("<H", payload)[0] if len(payload) >= 2 else None}
    if op == 0x40:
        return {"sleep_kind": "IDLE_RC standby (not DWM3000 deep sleep)" if payload and payload[0] == 1 else "unknown"}
    if op == 0x41:
        return {"already_awake": bool(payload and payload[0] == 1)}
    if op == 0x42 and len(payload) == 4:
        return {"sleep_ms": struct.unpack("<I", payload)[0]}
    if len(payload) == 0:
        return {"ok": True}
    return {"raw_hex": payload.hex(), "length": len(payload)}


def event_to_dict(frame: Frame) -> dict:
    name = EVENT_NAMES.get(frame.opcode, f"EVENT_0x{frame.opcode:02x}")
    result = {"event": name, "event_seq": frame.seq}
    p = frame.payload
    try:
        if frame.opcode == 0x80 and len(p) == 30:
            result["job_id"] = struct.unpack_from("<H", p)[0]
            result["measurement"] = decode_record(p[2:])
        elif frame.opcode == 0x81 and len(p) == 13:
            job, target = struct.unpack_from("<HH", p); result.update(job_id=job, target_id=target, status=p[4], mono_us=struct.unpack_from("<Q", p, 5)[0])
        elif frame.opcode == 0x82 and len(p) >= 4:
            result.update(role=ROLE_NAMES.get(p[0], p[0]), ranging_enabled=bool(p[1]), standby=bool(p[2]), radio_ready=bool(p[3]))
        elif frame.opcode == 0x83 and len(p) >= 9:
            result.update(error=ERROR_NAMES.get(p[0], p[0]), mono_us=struct.unpack_from("<Q", p, 1)[0])
        elif frame.opcode == 0x84 and len(p) == 8:
            result.update(overwritten_seq=struct.unpack_from("<I", p)[0], new_seq=struct.unpack_from("<I", p, 4)[0])
        elif frame.opcode == 0x85 and len(p) >= 2:
            result.update(radio_ready=bool(p[0]), protocol_version=p[1])
        elif frame.opcode == 0x86 and len(p) == 4:
            result.update(enabled=bool(p[0]), mode=BLE_MODE_NAMES.get(p[1], p[1]),
                          advertising=bool(p[2]), scanning=bool(p[3]))
        elif frame.opcode in (0x87, 0x88) and len(p) == 2:
            result["peer_id"] = struct.unpack_from("<H", p)[0]
        elif frame.opcode in (0x89, 0x8A) and len(p) == 3:
            result.update(peer_id=struct.unpack_from("<H", p)[0], reason=p[2])
        elif frame.opcode == 0x8B and len(p) == 6:
            result.update(peer_id=struct.unpack_from("<H", p)[0], message_id=struct.unpack_from("<H", p, 2)[0],
                          length=struct.unpack_from("<H", p, 4)[0])
        elif frame.opcode == 0x8C and len(p) == 8:
            direction = "tx" if p[0] == 0 else "rx"
            result.update(direction=direction, peer_id=struct.unpack_from("<H", p, 1)[0],
                          message_id=struct.unpack_from("<H", p, 3)[0], fragment_index=p[5], error_code=p[6])
        elif frame.opcode == 0x8D and len(p) == 6:
            result.update(peer_id=struct.unpack_from("<H", p)[0], message_id=struct.unpack_from("<H", p, 2)[0],
                          length=struct.unpack_from("<H", p, 4)[0])
        elif frame.opcode == 0x8E and len(p) == 4:
            result.update(peer_id=struct.unpack_from("<H", p)[0], message_id=struct.unpack_from("<H", p, 2)[0])
    except Exception as exc:
        result["parse_error"] = str(exc)
    if not p:
        result["payload_hex"] = ""
    return result


def main():
    a = parse_args()
    try:
        client = UwbClient(a.port, a.baud, a.timeout)
    except Exception as e:
        print(f"cannot open UART: {e}", file=sys.stderr); return 2
    try:
        op = OPS[a.command]
        payload = b""
        if a.command == "set-role":
            payload = bytes([{"idle": 0, "initiator": 1, "responder": 2}[a.role]])
        elif a.command == "set-targets":
            if len(a.ids) > 16 or any(x < 1 or x > 65534 for x in a.ids) or len(set(a.ids)) != len(a.ids):
                raise ValueError("target ids must be unique integers 1..65534 (max 16)")
            payload = bytes([len(a.ids)]) + struct.pack("<" + "H" * len(a.ids), *a.ids)
        elif a.command == "set-schedule":
            payload = struct.pack("<IHB", a.period_ms, a.timeout_ms, a.retries)
        elif a.command in ("range-once", "cancel"):
            payload = struct.pack("<H", a.target if a.command == "range-once" else a.job_id)
        elif a.command in ("get-last",):
            payload = struct.pack("<H", a.target)
        elif a.command == "get-history":
            payload = struct.pack("<IHH", a.after, a.n, a.target)
        elif a.command in ("subscribe", "unsubscribe"):
            payload = struct.pack("<H", a.mask)
        elif a.command == "system-sleep":
            payload = struct.pack("<I", a.ms)
        elif a.command == "set-ant-delays":
            payload = struct.pack("<HH", a.tx, a.rx)
        elif a.command == "set-config":
            payload = struct.pack("<HHBHH", a.device_id, a.pan_id, a.channel, a.tx_delay, a.rx_delay)
            if a.ble_mode is not None:
                payload += encode_ble_config(a)
        elif a.command == "apply-save-config":
            if not 1 <= a.device_id <= 65534 or not 0 <= a.pan_id <= 65535:
                raise ValueError("device ID must be 1..65534 and PAN ID 0..65535")
            if not 20 <= a.period_ms <= 60000 or not 50 <= a.timeout_ms <= 5000 or not 0 <= a.retries <= 5:
                raise ValueError("invalid UWB schedule settings")
            if len(a.targets) > 16 or any(x < 1 or x > 65534 or x == a.device_id for x in a.targets) or len(set(a.targets)) != len(a.targets):
                raise ValueError("target IDs must be unique, differ from device ID and be 1..65534 (max 16)")
            if a.auto_start and (a.role == "idle" or (a.role == "initiator" and not a.targets)):
                raise ValueError("auto-start requires responder role or initiator with targets")
            payload = struct.pack("<HHBHHIHBBB", a.device_id, a.pan_id, a.channel, a.tx_delay,
                                  a.rx_delay, a.period_ms, a.timeout_ms, a.retries,
                                  {"idle": 0, "initiator": 1, "responder": 2}[a.role], len(a.targets))
            if a.targets:
                payload += struct.pack("<" + "H" * len(a.targets), *a.targets)
            payload += bytes([int(a.auto_start)])
            payload += encode_ble_config(a)
        elif a.command == "set-peers":
            if len(a.ids) > 8 or any(x < 1 or x > 65534 for x in a.ids) or len(set(a.ids)) != len(a.ids):
                raise ValueError("BLE peer IDs must be unique integers 1..65534 (max 8)")
            payload = bytes([len(a.ids)]) + (struct.pack("<" + "H" * len(a.ids), *a.ids) if a.ids else b"")
        elif a.command == "peer-status":
            if not 1 <= a.peer_id <= 65534:
                raise ValueError("peer ID must be in 1..65534")
            payload = struct.pack("<H", a.peer_id)
        elif a.command == "send-to-peer":
            if not 1 <= a.peer_id <= 65534:
                raise ValueError("peer ID must be in 1..65534")
            data = bytes.fromhex(a.data) if a.hex else a.data.encode("utf-8")
            if len(data) > 496:
                raise ValueError("application message exceeds 496 bytes")
            payload = struct.pack("<HH", a.peer_id, len(data)) + data
        elif a.command == "get-received-data":
            if not 1 <= a.peer_id <= 65534 or not 0 <= a.message_id <= 65535:
                raise ValueError("peer ID must be 1..65534 and message ID 0..65535")
            payload = struct.pack("<HH", a.peer_id, a.message_id)
        elif a.command == "time-sync":
            payload = struct.pack("<Q", time.time_ns() // 1000)

        # Subscription/listening is long-lived; install subscription, then print events.
        response = client.request(op, payload)
        print(json.dumps({"response": record_response(op, response.payload), "error": None}, indent=2))
        if a.command == "subscribe":
            print("Listening for events; Ctrl-C to stop.", flush=True)
            while True:
                ev = client.poll_event(1.0)
                if ev is not None:
                    print(json.dumps(event_to_dict(ev), separators=(",", ":")), flush=True)
    except KeyboardInterrupt:
        return 0
    except Exception as e:
        print(f"error: {e}", file=sys.stderr); return 1
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
