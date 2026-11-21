"""Firmware BLE transport adapter with per-peer send and receive queues."""
from __future__ import annotations

from collections import OrderedDict
import queue
import struct
import threading
import time
from concurrent.futures import Future, TimeoutError as FutureTimeout
from dataclasses import dataclass

from .exceptions import ConnectionLost, RequestTimeout
from .models import BleMessage


@dataclass
class BleSendTicket:
    peer_id: int
    future: Future

    def wait(self, timeout: float | None = None):
        return self.future.result(timeout=timeout)

    @property
    def done(self) -> bool:
        return self.future.done()


class BleManager:
    def __init__(self, transport, dispatcher, delivery_timeout: float = 8.0):
        self.transport = transport
        self.dispatcher = dispatcher
        self.delivery_timeout = delivery_timeout
        self._tx_queues: dict[int, queue.Queue] = {}
        self._tx_active: set[int] = set()
        self._tx_lock = threading.RLock()
        self._rx_queues: dict[int, queue.Queue] = {}
        self._rx_active: set[int] = set()
        self._rx_lock = threading.RLock()
        self._tickets: dict[tuple[int, int], Future] = {}
        self._active_futures: set[Future] = set()
        self._ticket_lock = threading.RLock()
        # A delivery event can reach the event-router thread before the TX
        # worker registers the firmware-assigned message ID. Retain such
        # results briefly so that this scheduling race cannot lose them.
        self._early_results: OrderedDict[tuple[int, int], tuple[float, dict]] = OrderedDict()
        self._early_result_cache_size = 128
        self._early_result_cache_ttl = max(1.0, self.delivery_timeout * 2.0)
        self._running = threading.Event()
        self.on_message = None
        self.sent_count = 0
        self.expired_count = 0
        self.rx_errors = 0

    def start(self):
        self._running.set()

    def send(self, peer_id: int, data: bytes) -> BleSendTicket:
        peer_id = int(peer_id)
        if not self._running.is_set():
            raise ConnectionLost("BLE manager is not started")
        if not 1 <= peer_id <= 65534:
            raise ValueError("peer_id must be in 1..65534")
        if not isinstance(data, bytes):
            raise TypeError("BLE transport accepts bytes")
        if len(data) > 496:
            raise ValueError("BLE application payload exceeds 496 bytes")
        future: Future = Future()
        with self._tx_lock:
            q = self._tx_queues.setdefault(peer_id, queue.Queue())
            q.put((data, future))
            if peer_id not in self._tx_active:
                self._tx_active.add(peer_id)
                threading.Thread(
                    target=self._tx_worker, args=(peer_id,),
                    name=f"d3kble-ble-tx-{peer_id}", daemon=True,
                ).start()
        return BleSendTicket(peer_id, future)

    def _prune_early_results_locked(self, now: float | None = None) -> None:
        now = time.monotonic() if now is None else now
        while self._early_results:
            _key, (created_at, _result) = next(iter(self._early_results.items()))
            if now - created_at <= self._early_result_cache_ttl:
                break
            self._early_results.popitem(last=False)

    def _cache_early_result_locked(
        self, key: tuple[int, int], result: dict, now: float
    ) -> None:
        self._early_results[key] = (now, result)
        self._early_results.move_to_end(key)
        while len(self._early_results) > self._early_result_cache_size:
            self._early_results.popitem(last=False)

    def _count_delivery_result(self, result: dict) -> None:
        if result.get("delivered"):
            self.sent_count += 1
        else:
            self.expired_count += 1

    def _register_ticket(self, key: tuple[int, int], future: Future) -> None:
        with self._ticket_lock:
            now = time.monotonic()
            self._prune_early_results_locked(now)
            cached = self._early_results.pop(key, None)
            if cached is None:
                self._tickets[key] = future
                return

        # Resolve outside the lock: Future callbacks run synchronously and may
        # call back into the BLE manager.
        result = cached[1]
        if not future.done():
            future.set_result(result)
            self._count_delivery_result(result)

    def _tx_worker(self, peer_id: int):
        q = self._tx_queues[peer_id]
        try:
            while self._running.is_set():
                try:
                    data, future = q.get(timeout=0.2)
                except queue.Empty:
                    break
                try:
                    if not self._running.is_set():
                        raise ConnectionLost("BLE manager closed before transmission")
                    with self._ticket_lock:
                        self._active_futures.add(future)
                    response = self.transport.request(
                        "send_to_peer",
                        struct.pack("<HH", peer_id, len(data)) + data,
                        retryable=False,
                    )
                    if len(response) != 2:
                        raise ValueError("SEND_TO_PEER response must contain a uint16 message ID")
                    message_id, = struct.unpack("<H", response)
                    key = (peer_id, message_id)
                    self._register_ticket(key, future)
                    try:
                        result = future.result(timeout=self.delivery_timeout)
                    except FutureTimeout as exc:
                        with self._ticket_lock:
                            if self._tickets.get(key) is future:
                                self._tickets.pop(key, None)
                        raise RequestTimeout(
                            f"BLE delivery result timed out for peer {peer_id}, message {message_id}"
                        ) from exc
                    if not future.done():
                        future.set_result(result)
                except Exception as exc:
                    if not future.done():
                        future.set_exception(exc)
                finally:
                    with self._ticket_lock:
                        self._active_futures.discard(future)
                    q.task_done()
        finally:
            with self._tx_lock:
                self._tx_active.discard(peer_id)
                restart = self._running.is_set() and not q.empty()
                if restart:
                    self._tx_active.add(peer_id)
                    threading.Thread(
                        target=self._tx_worker, args=(peer_id,),
                        name=f"d3kble-ble-tx-{peer_id}", daemon=True,
                    ).start()

    def on_event(self, frame) -> None:
        if frame.opcode == 0x8B:
            if len(frame.payload) != 6:
                self.rx_errors += 1
                return
            peer_id, message_id, _total_length = struct.unpack("<HHH", frame.payload)
            with self._rx_lock:
                q = self._rx_queues.setdefault(peer_id, queue.Queue())
                try:
                    q.put_nowait(message_id)
                except queue.Full:
                    self.rx_errors += 1
                    return
                if peer_id not in self._rx_active:
                    self._rx_active.add(peer_id)
                    threading.Thread(
                        target=self._rx_worker, args=(peer_id,),
                        name=f"d3kble-ble-rx-{peer_id}", daemon=True,
                    ).start()
        elif frame.opcode in (0x8D, 0x8E):
            expected_len = 6 if frame.opcode == 0x8D else 4
            if len(frame.payload) != expected_len:
                self.rx_errors += 1
                return
            peer_id, message_id = struct.unpack_from("<HH", frame.payload)
            if frame.opcode == 0x8D:
                total_length, = struct.unpack_from("<H", frame.payload, 4)
                result = {
                    "delivered": True, "peer_id": peer_id,
                    "message_id": message_id, "total_length": total_length,
                }
            else:
                result = {
                    "delivered": False, "expired": True,
                    "peer_id": peer_id, "message_id": message_id,
                }

            key = (peer_id, message_id)
            with self._ticket_lock:
                now = time.monotonic()
                self._prune_early_results_locked(now)
                future = self._tickets.pop(key, None)
                if future is None:
                    # SEND_TO_PEER's response and its delivery event are read
                    # on independent host threads. The event may win this race.
                    self._cache_early_result_locked(key, result, now)
            if future is not None and not future.done():
                future.set_result(result)
                self._count_delivery_result(result)
        else:
            name = {
                0x86: "ble_state_changed", 0x87: "ble_peer_found",
                0x88: "ble_peer_connected", 0x89: "ble_peer_disconnected",
                0x8A: "ble_connection_failed", 0x8C: "ble_error",
            }.get(frame.opcode)
            if name:
                self.dispatcher.emit(name, frame.payload)

    def _rx_worker(self, peer_id: int):
        q = self._rx_queues[peer_id]
        try:
            while self._running.is_set():
                try:
                    message_id = q.get(timeout=0.2)
                except queue.Empty:
                    break
                try:
                    response = self.transport.request(
                        "get_received_data", struct.pack("<HH", peer_id, message_id),
                        timeout=0.8, retries=0, retryable=False,
                    )
                    if len(response) < 4:
                        raise ValueError("GET_RECEIVED_DATA response is truncated")
                    received_id, length = struct.unpack_from("<HH", response)
                    if received_id != message_id or len(response) != 4 + length:
                        raise ValueError("GET_RECEIVED_DATA response ID/length mismatch")
                    message = BleMessage(peer_id, message_id, response[4:], time.monotonic())
                    if self.on_message:
                        self.on_message(message)
                    self.dispatcher.emit("ble_message", message)
                except Exception as exc:
                    self.rx_errors += 1
                    self.dispatcher.emit("error", exc)
                finally:
                    q.task_done()
        finally:
            with self._rx_lock:
                self._rx_active.discard(peer_id)
                restart = self._running.is_set() and not q.empty()
                if restart:
                    self._rx_active.add(peer_id)
                    threading.Thread(
                        target=self._rx_worker, args=(peer_id,),
                        name=f"d3kble-ble-rx-{peer_id}", daemon=True,
                    ).start()

    def get_peer_status(self, peer_id: int) -> dict:
        response = self.transport.request("get_peer_status", struct.pack("<H", peer_id))
        if len(response) != 14:
            raise ValueError("GET_PEER_STATUS response must be 14 bytes")
        state, mode, enabled, advertising, scanning, connected = response[:6]
        actual_peer, mtu = struct.unpack_from("<HH", response, 6)
        reason, stage, detail = struct.unpack_from("<BBh", response, 10)
        return {
            "state": state, "mode": mode, "enabled": bool(enabled),
            "advertising": bool(advertising), "scanning": bool(scanning),
            "connected": bool(connected), "peer_id": actual_peer, "mtu": mtu,
            "last_failure_reason": reason, "last_failure_stage": stage,
            "last_failure_detail": detail,
        }

    def start_ble(self):
        return self.transport.request("start_ble", retryable=False)

    def stop_ble(self):
        return self.transport.request("stop_ble", retryable=False)

    def close(self):
        self._running.clear()
        with self._ticket_lock:
            pending = list(self._tickets.values()) + list(self._active_futures)
            self._tickets.clear()
            self._early_results.clear()
            self._active_futures.clear()
        for future in set(pending):
            if not future.done():
                future.set_exception(ConnectionLost("BLE manager closed"))
        # Fail messages that have not yet reached SEND_TO_PEER as well.
        queued = []
        with self._tx_lock:
            for q in self._tx_queues.values():
                while True:
                    try:
                        queued.append(q.get_nowait())
                        q.task_done()
                    except queue.Empty:
                        break
        for _data, future in queued:
            if not future.done():
                future.set_exception(ConnectionLost("BLE manager closed before transmission"))
