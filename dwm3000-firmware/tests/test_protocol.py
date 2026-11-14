#!/usr/bin/env python3
"""Host-only tests for the Python UART protocol implementation."""
import importlib.util
import pathlib
import sys

path = pathlib.Path(__file__).resolve().parents[1] / "pi_client.py"
spec = importlib.util.spec_from_file_location("pi_client_test_module", path)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

assert module.crc16(b"123456789") == 0x29B1
samples = [b"", b"plain text", b"\x00\x01\x00\x02\x03\x00", bytes(range(1, 255)), bytes([0]) * 500]
for data in samples:
    frame = module.Frame(module.TYPE_REQUEST, 0x1234, 0x31, 0, data)
    wire = module.encode_frame(frame)
    assert wire.endswith(b"\x00")
    decoded = module.decode_frame(wire[:-1])
    assert decoded.type == frame.type
    assert decoded.seq == frame.seq
    assert decoded.opcode == frame.opcode
    assert decoded.payload == data
print("protocol CRC/COBS/frame round-trip tests passed")
