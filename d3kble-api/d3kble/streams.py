"""Independent measurement subscriptions."""
from __future__ import annotations

import threading

from .events import Subscription


class MeasurementStream:
    def __init__(self):
        self._subscriptions: set[Subscription] = set()
        self._lock = threading.RLock()

    def subscribe(self, maxsize: int = 256, overflow: str = "drop_oldest") -> Subscription:
        subscription = Subscription(maxsize=maxsize, overflow=overflow)
        with self._lock:
            self._subscriptions.add(subscription)
        return subscription

    def unsubscribe(self, subscription: Subscription) -> None:
        with self._lock:
            self._subscriptions.discard(subscription)
        subscription.close()

    def publish(self, measurement) -> None:
        with self._lock:
            subscribers = tuple(self._subscriptions)
        for subscription in subscribers:
            subscription.publish(measurement)

    def close(self) -> None:
        with self._lock:
            subscribers = tuple(self._subscriptions)
            self._subscriptions.clear()
        for subscription in subscribers:
            subscription.close()
