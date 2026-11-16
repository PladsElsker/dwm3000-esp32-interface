"""Thread-isolated event callbacks and bounded independent subscriptions."""
from __future__ import annotations

import queue
import threading
from collections import defaultdict
from typing import Callable, Generic, TypeVar

T = TypeVar("T")


class Subscription(Generic[T]):
    def __init__(self, maxsize: int = 256, overflow: str = "drop_oldest"):
        if maxsize < 1:
            raise ValueError("maxsize must be positive")
        if overflow not in {"drop_oldest", "drop_newest", "raise"}:
            raise ValueError("overflow must be drop_oldest, drop_newest, or raise")
        self._queue: queue.Queue[T] = queue.Queue(maxsize=maxsize)
        self.overflow = overflow
        self.dropped_count = 0
        self._closed = False
        self._lock = threading.Lock()

    def publish(self, item: T) -> None:
        with self._lock:
            if self._closed:
                return
            try:
                self._queue.put_nowait(item)
                return
            except queue.Full:
                self.dropped_count += 1
                if self.overflow == "drop_newest":
                    return
                if self.overflow == "raise":
                    raise
                try:
                    self._queue.get_nowait()
                except queue.Empty:
                    pass
                self._queue.put_nowait(item)

    def get(self, timeout: float | None = None) -> T:
        if self._closed and self._queue.empty():
            raise RuntimeError("subscription is closed")
        return self._queue.get(timeout=timeout)

    def get_nowait(self) -> T:
        return self._queue.get_nowait()

    def close(self) -> None:
        with self._lock:
            self._closed = True

    def __iter__(self):
        while not self._closed or not self._queue.empty():
            try:
                yield self.get(timeout=0.25)
            except queue.Empty:
                continue
            except RuntimeError:
                return


class EventDispatcher:
    def __init__(self, workers: int = 2, max_queue: int = 2048):
        self._callbacks: dict[str, list[Callable]] = defaultdict(list)
        self._queue: queue.Queue = queue.Queue(maxsize=max_queue)
        self._lock = threading.RLock()
        self._running = True
        self.errors = 0
        self.dropped = 0
        self._threads = [
            threading.Thread(target=self._run, name=f"d3kble-callback-{i}", daemon=True)
            for i in range(max(1, workers))
        ]
        for thread in self._threads:
            thread.start()

    def on(self, event: str, callback: Callable):
        if not callable(callback):
            raise TypeError("callback must be callable")
        with self._lock:
            self._callbacks[event].append(callback)
        return callback

    def off(self, event: str, callback: Callable) -> None:
        with self._lock:
            try:
                self._callbacks[event].remove(callback)
            except (KeyError, ValueError):
                pass

    def emit(self, event: str, value: object) -> None:
        try:
            self._queue.put_nowait((event, value))
        except queue.Full:
            self.dropped += 1

    def _run(self):
        while self._running or not self._queue.empty():
            try:
                event, value = self._queue.get(timeout=0.2)
            except queue.Empty:
                continue
            with self._lock:
                callbacks = tuple(self._callbacks.get(event, ())) + tuple(self._callbacks.get("*", ()))
            for callback in callbacks:
                try:
                    callback(value)
                except Exception:
                    self.errors += 1
            self._queue.task_done()

    def close(self):
        self._running = False
        for thread in self._threads:
            thread.join(timeout=1)
