"""Top-level persistent node API."""
from __future__ import annotations

import queue
import struct
import threading
import time

from .ble import BleManager
from .comms import CommsManager
from .config import ConfigManager
from .diagnostics import collect_diagnostics
from .events import EventDispatcher
from .protocol import EVENT_NAMES, decode_event_payload
from .state import StateManager
from .transport import SerialTransport
from .uwb import UwbManager


class D3KNode:
    """Own a persistent UART session to one ESP32 UWB/BLE node."""

    def __init__(
        self, uart_device: str, baudrate: int = 460800,
        poll_interval: float = 1.0, request_timeout: float = 1.5,
        request_retries: int = 1,
        auto_reconnect: bool = True, reconcile_interval: float = 2.0,
        serial_factory=None,
    ):
        self.transport = SerialTransport(
            uart_device, baudrate, request_timeout, request_retries,
            serial_factory=serial_factory,
        )
        self.events = EventDispatcher()
        self.uwb = UwbManager(self.transport, self.events)
        self.ble = BleManager(self.transport, self.events)
        self.comms = CommsManager(self.ble, self.events)
        self.state = StateManager(self.transport, poll_interval)
        self.config = ConfigManager(self.transport, self.state, self.events)
        self.auto_reconnect = auto_reconnect
        self.reconcile_interval = max(0.5, reconcile_interval)
        self._stop = threading.Event()
        self._paused = threading.Event()
        self._paused.set()
        self._ready = threading.Event()
        self._supervisor: threading.Thread | None = None
        self._event_thread: threading.Thread | None = None
        self._started = False
        self._closed = False
        self._last_reconcile = 0.0
        self._reconnect_requested = threading.Event()
        self.transport.on_disconnect = self._on_transport_disconnect

    @property
    def is_connected(self) -> bool:
        return self.transport.connected

    @property
    def is_ready(self) -> bool:
        return self._ready.is_set() and self.transport.connected

    @property
    def uart_device(self) -> str:
        return self.transport.uart_device

    def on(self, event: str, callback=None):
        if callback is None:
            return lambda fn: self.events.on(event, fn)
        return self.events.on(event, callback)

    def off(self, event: str, callback):
        self.events.off(event, callback)

    def start(self):
        if self._closed:
            raise RuntimeError("a closed D3KNode cannot be restarted")
        if self._started:
            self._paused.clear()
            self._reconnect_requested.set()
            return self
        self._started = True
        self._paused.clear()
        self.ble.start()
        self.comms.start()
        self.state.start()
        self._event_thread = threading.Thread(
            target=self._event_loop, name="d3kble-event-router", daemon=True
        )
        self._event_thread.start()
        self._stop.clear()
        self._supervisor = threading.Thread(
            target=self._supervise, name="d3kble-session", daemon=True
        )
        self._supervisor.start()
        return self

    def wait_until_ready(self, timeout: float | None = 10.0):
        if not self._started:
            self.start()
        if not self._ready.wait(timeout):
            raise TimeoutError(f"ESP32 at {self.uart_device} did not become ready")
        return self

    def _connect_once(self):
        self.transport.open()
        self.transport.request("subscribe", struct.pack("<H", 0x7FFF), retryable=True)
        info = self.transport.request("get_info")
        if len(info) != 21:
            raise ValueError("GET_INFO response must be 21 bytes")
        self.state.refresh(include_saved=True)
        if not self.state.snapshot().status_valid:
            raise RuntimeError("device status could not be read")
        self._ready.set()
        self.events.emit("connected", {"uart_device": self.uart_device, "info": info})
        self.events.emit("ready", self.state.snapshot())

    def _supervise(self):
        delay = 0.25
        attempted_once = False
        while not self._stop.is_set():
            if self._paused.is_set():
                self._stop.wait(0.1)
                continue
            if not self.transport.connected:
                self._ready.clear()
                if attempted_once and not self.auto_reconnect:
                    if not self._reconnect_requested.wait(0.2):
                        continue
                    self._reconnect_requested.clear()
                    if self._stop.is_set():
                        break
                else:
                    self._reconnect_requested.clear()
                attempted_once = True
                try:
                    self.transport.close()
                    self._connect_once()
                    delay = 0.25
                except Exception as exc:
                    self._ready.clear()
                    self.events.emit("connection_error", exc)
                    self._stop.wait(delay)
                    delay = min(5.0, delay * 1.7)
                    continue
            if self.config.reconciliation_enabled:
                now = time.monotonic()
                if now - self._last_reconcile >= self.reconcile_interval:
                    self._last_reconcile = now
                    try:
                        self.config.reconcile()
                    except Exception as exc:
                        self.events.emit("reconciliation_error", exc)
            self._stop.wait(0.2)
        self._ready.clear()

    def _on_transport_disconnect(self, error):
        self._ready.clear()
        self.events.emit("disconnected", error)

    def _event_loop(self):
        while not self._stop.is_set():
            try:
                frame = self.transport.events.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                name = EVENT_NAMES.get(frame.opcode, f"event_{frame.opcode:02x}")
                if frame.opcode >= 0x86:
                    self.ble.on_event(frame)
                elif frame.opcode in (0x80, 0x81, 0x84):
                    self.uwb.on_event(frame)
                else:
                    self.events.emit(name, decode_event_payload(frame.opcode, frame.payload))
            except Exception as exc:
                self.events.emit("error", exc)
            finally:
                self.transport.events.task_done()

    def get_state(self, refresh: bool = False):
        return self.state.refresh(include_saved=True) if refresh else self.state.snapshot()

    def refresh(self):
        return self.state.refresh(include_saved=True)

    def get_diagnostics(self) -> dict:
        return collect_diagnostics(self)

    def stop(self):
        """Pause the session without discarding subscriptions or in-memory message queues."""
        if self._closed or not self._started:
            return
        self._paused.set()
        self._ready.clear()
        self.transport.close()
        self.events.emit("disconnected", {"reason": "node stopped by application"})

    def reconnect(self, timeout: float = 10.0):
        if not self._started:
            self.start()
        self._paused.clear()
        if self._supervisor and not self._supervisor.is_alive():
            self._stop.clear()
            self._supervisor = threading.Thread(
                target=self._supervise, name="d3kble-session", daemon=True
            )
            self._supervisor.start()
        self._ready.clear()
        self._reconnect_requested.set()
        self.transport.close()
        return self.wait_until_ready(timeout)

    def close(self):
        if self._closed:
            return
        self.stop()
        self._closed = True
        self._stop.set()
        self.comms.close()
        self.ble.close()
        self.state.close()
        self.uwb.close()
        self.transport.close()
        if self._supervisor and self._supervisor is not threading.current_thread():
            self._supervisor.join(timeout=1)
        if self._event_thread and self._event_thread is not threading.current_thread():
            self._event_thread.join(timeout=1)
        self.events.close()

    def __enter__(self):
        self.start()
        self.wait_until_ready()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
