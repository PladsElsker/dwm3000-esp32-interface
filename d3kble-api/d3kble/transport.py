"""UART transport: one reader, serialized writes, sequence-based response dispatch."""
from __future__ import annotations

import queue
import threading
from concurrent.futures import Future, TimeoutError as FutureTimeout
from typing import Callable

from .exceptions import ConnectionLost, DeviceCommandError, RequestTimeout
from .protocol import Frame, OPS, TYPE_EVENT, TYPE_REQUEST, TYPE_RESPONSE, decode_frame, encode_frame


class SerialTransport:
    def __init__(
        self, uart_device: str, baudrate: int = 460800, timeout: float = 1.5,
        retries: int = 1, serial_factory: Callable | None = None,
    ):
        if not uart_device:
            raise ValueError("uart_device must be explicitly selected")
        self.uart_device = uart_device
        self.baudrate = baudrate
        self.timeout = timeout
        self.retries = max(0, retries)
        self._serial_factory = serial_factory
        self._serial = None
        self._running = threading.Event()
        self._pending: dict[int, Future] = {}
        self._pending_lock = threading.RLock()
        self._write_lock = threading.Lock()
        self._seq_lock = threading.Lock()
        self._seq = 1
        self._reader: threading.Thread | None = None
        self.events: queue.Queue[Frame] = queue.Queue(maxsize=4096)
        self.on_disconnect: Callable[[Exception], None] | None = None
        self.rx_errors = 0
        self.frames_received = 0
        self.event_drops = 0

    @property
    def connected(self) -> bool:
        return self._serial is not None and self._running.is_set()

    def open(self) -> None:
        if self.connected:
            return
        factory = self._serial_factory
        if factory is None:
            try:
                import serial
            except ImportError as exc:
                raise RuntimeError("pyserial is required: pip install pyserial") from exc
            factory = serial.Serial
        self._serial = factory(
            self.uart_device, baudrate=self.baudrate, timeout=0.05, write_timeout=1
        )
        self._running.set()
        self._reader = threading.Thread(
            target=self._read_loop, name="d3kble-uart-reader", daemon=True
        )
        self._reader.start()

    def close(self) -> None:
        self._running.clear()
        serial_obj, self._serial = self._serial, None
        if serial_obj is not None:
            try:
                serial_obj.close()
            except Exception:
                pass
        if self._reader and self._reader is not threading.current_thread():
            self._reader.join(timeout=1)
        self._fail_pending(ConnectionLost("UART transport closed"))
        while True:
            try:
                self.events.get_nowait()
                self.events.task_done()
            except queue.Empty:
                break

    def _next_seq(self) -> int:
        with self._seq_lock, self._pending_lock:
            for _ in range(0xFFFF):
                seq = self._seq
                self._seq = (self._seq + 1) & 0xFFFF
                if self._seq == 0:
                    self._seq = 1
                if seq not in self._pending:
                    return seq
        raise RuntimeError("all protocol sequence numbers are in use")

    def _write(self, packet: bytes) -> None:
        with self._write_lock:
            if not self.connected:
                raise ConnectionLost("UART is not connected")
            self._serial.write(packet)

    def request(
        self, opcode: int | str, payload: bytes = b"", timeout: float | None = None,
        retries: int | None = None, retryable: bool = True,
    ) -> bytes:
        if isinstance(opcode, str):
            opcode = OPS[opcode]
        if not self.connected:
            raise ConnectionLost("UART is not connected")
        seq = self._next_seq()
        future: Future = Future()
        with self._pending_lock:
            self._pending[seq] = future
        packet = encode_frame(Frame(TYPE_REQUEST, seq, opcode, 0, payload))
        wait_s = self.timeout if timeout is None else timeout
        attempts = self.retries if retries is None else max(0, retries)
        if not retryable:
            attempts = 0
        try:
            for attempt in range(attempts + 1):
                self._write(packet)
                try:
                    response: Frame = future.result(timeout=wait_s)
                    if response.flags:
                        raise DeviceCommandError(opcode, response.flags)
                    return response.payload
                except FutureTimeout:
                    if attempt == attempts:
                        raise RequestTimeout(f"timeout waiting for opcode 0x{opcode:02x}")
            raise RequestTimeout(f"timeout waiting for opcode 0x{opcode:02x}")
        finally:
            with self._pending_lock:
                self._pending.pop(seq, None)

    def _read_loop(self):
        buffer = bytearray()
        try:
            while self._running.is_set():
                chunk = self._serial.read(256)
                if not chunk:
                    continue
                for byte in chunk:
                    if byte == 0:
                        if not buffer:
                            continue
                        try:
                            frame = decode_frame(bytes(buffer))
                            self.frames_received += 1
                        except Exception:
                            self.rx_errors += 1
                            buffer.clear()
                            continue
                        buffer.clear()
                        if frame.type == TYPE_RESPONSE:
                            with self._pending_lock:
                                future = self._pending.get(frame.seq)
                            if future is not None and not future.done():
                                future.set_result(frame)
                        elif frame.type == TYPE_EVENT:
                            try:
                                self.events.put_nowait(frame)
                            except queue.Full:
                                self.event_drops += 1
                    elif len(buffer) < 1024:
                        buffer.append(byte)
                    else:
                        buffer.clear()
                        self.rx_errors += 1
        except Exception as exc:
            if self._running.is_set():
                self._running.clear()
                self._fail_pending(ConnectionLost(str(exc)))
                if self.on_disconnect:
                    self.on_disconnect(exc)

    def _fail_pending(self, error: Exception):
        with self._pending_lock:
            futures = list(self._pending.values())
        for future in futures:
            if not future.done():
                future.set_exception(error)
