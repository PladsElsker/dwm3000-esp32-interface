"""Runtime/saved device state snapshots and background polling."""
from __future__ import annotations

import threading
import time

from .models import DeviceState, RuntimeConfig, SavedConfig
from .protocol import decode_runtime_config, decode_saved_config, decode_schedule, decode_status, decode_targets


class StateManager:
    def __init__(self, transport, poll_interval: float = 1.0):
        self.transport = transport
        self.poll_interval = max(0.2, float(poll_interval))
        self._lock = threading.RLock()
        self.runtime = RuntimeConfig()
        self.saved = SavedConfig()
        self.status = {}
        self.status_valid = False
        self.status_updated_at = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def refresh(self, include_saved: bool = False) -> DeviceState:
        try:
            runtime = decode_runtime_config(self.transport.request("get_config"))
            runtime["targets"] = decode_targets(self.transport.request("get_targets"))
            runtime["schedule"] = decode_schedule(self.transport.request("get_schedule"))
            with self._lock:
                self.runtime = RuntimeConfig(runtime, True, time.monotonic())
        except Exception:
            with self._lock:
                self.runtime = RuntimeConfig(self.runtime.values, False, self.runtime.updated_at)

        try:
            status = decode_status(self.transport.request("get_status"))
            with self._lock:
                self.status = status
                self.status_valid = True
                self.status_updated_at = time.monotonic()
        except Exception:
            with self._lock:
                self.status_valid = False

        if include_saved:
            try:
                saved = decode_saved_config(self.transport.request("get_saved_config"))
                with self._lock:
                    self.saved = SavedConfig(saved, True, time.monotonic())
            except Exception:
                with self._lock:
                    self.saved = SavedConfig(self.saved.values, False, self.saved.updated_at)
        return self.snapshot()

    def snapshot(self) -> DeviceState:
        with self._lock:
            return DeviceState(
                runtime=RuntimeConfig(dict(self.runtime.values), self.runtime.valid, self.runtime.updated_at),
                saved=SavedConfig(dict(self.saved.values), self.saved.valid, self.saved.updated_at),
                status=dict(self.status), status_valid=self.status_valid,
                status_updated_at=self.status_updated_at,
            )

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._poll_loop, name="d3kble-state-poller", daemon=True)
        self._thread.start()

    def _poll_loop(self):
        first = True
        while not self._stop.is_set():
            if self.transport.connected:
                self.refresh(include_saved=first)
                first = False
            self._stop.wait(self.poll_interval)

    def close(self):
        self._stop.set()
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=2)
