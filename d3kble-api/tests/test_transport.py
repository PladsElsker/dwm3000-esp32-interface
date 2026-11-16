import queue
import threading
import time

from d3kble.protocol import Frame, TYPE_RESPONSE, decode_frame, encode_frame
from d3kble.transport import SerialTransport


class FakeSerial:
    def __init__(self, *args, **kwargs):
        self.rx = queue.Queue()
        self.closed = False
        self.writes = []

    def read(self, size):
        try:
            return self.rx.get(timeout=0.05)
        except queue.Empty:
            return b""

    def write(self, packet):
        self.writes.append(packet)
        request = decode_frame(packet[:-1])
        response = encode_frame(Frame(
            TYPE_RESPONSE, request.seq, request.opcode, 0,
            request.payload[::-1],
        ))
        self.rx.put(response)
        return len(packet)

    def close(self):
        self.closed = True


def test_serial_transport_request_and_close():
    serials = []

    def factory(*args, **kwargs):
        instance = FakeSerial(*args, **kwargs)
        serials.append(instance)
        return instance

    transport = SerialTransport("/dev/fake-uart", serial_factory=factory, retries=0)
    transport.open()
    assert transport.request("ping", b"abc") == b"cba"
    transport.close()
    assert serials[0].closed
