"""Volatile in-process state for application messaging.

No database or filesystem persistence is used. State survives serial link
reconnects while this Python process remains alive, and is discarded when
the process exits or the node is closed.
"""
from __future__ import annotations

import threading
import time
from collections import OrderedDict


class CommsState:
    """Bounded in-memory inbox/outbox used by the communications middleware."""

    def __init__(self, max_inbox_records: int = 4096, max_response_records: int = 2048):
        if max_inbox_records < 1 or max_response_records < 1:
            raise ValueError("in-memory record limits must be positive")
        self.max_inbox_records = max_inbox_records
        self.max_response_records = max_response_records
        self._lock = threading.RLock()
        self._outbox: dict[str, dict] = {}
        self._inbox: OrderedDict[tuple[int, str], dict] = OrderedDict()
        self._requests: OrderedDict[str, dict] = OrderedDict()
        self._responses: OrderedDict[tuple[int, str], str] = OrderedDict()
        self._closed = False

    def _check_open(self):
        if self._closed:
            raise RuntimeError("communications state is closed")

    def enqueue(self, message_id: str, peer_id: int, kind: str, envelope: bytes,
                response_deadline: float | None = None):
        with self._lock:
            self._check_open()
            self._outbox[message_id] = {
                "peer_id": peer_id, "kind": kind, "envelope": bytes(envelope),
                "attempts": 0, "next_attempt": time.monotonic(),
                "status": "pending", "last_error": None,
            }
            if kind == "request":
                self._requests[message_id] = {
                    "peer_id": peer_id, "accepted": False, "response_json": None,
                    "deadline": response_deadline,
                }
                self._trim_completed_requests()

    def enqueue_response_once(self, peer_id: int, request_id: str,
                              response_id: str, envelope: bytes) -> bool:
        key = (peer_id, request_id)
        with self._lock:
            self._check_open()
            if key in self._responses:
                return False
            self._responses[key] = response_id
            self._responses.move_to_end(key)
            self._trim_responses()
            self._outbox[response_id] = {
                "peer_id": peer_id, "kind": "response", "envelope": bytes(envelope),
                "attempts": 0, "next_attempt": time.monotonic(),
                "status": "pending", "last_error": None,
            }
            return True

    def due_outbox(self, now: float, limit: int = 64):
        with self._lock:
            self._check_open()
            rows = [
                (message_id, row["peer_id"], row["kind"], row["envelope"], row["attempts"])
                for message_id, row in self._outbox.items()
                if row["status"] == "pending" and row["next_attempt"] <= now
            ]
            rows.sort(key=lambda row: self._outbox[row[0]]["next_attempt"])
            return rows[:limit]

    def mark_transport_attempt(self, message_id: str, attempts: int,
                               next_attempt: float, error: str | None = None):
        with self._lock:
            row = self._outbox.get(message_id)
            if row is not None:
                row.update(attempts=attempts, next_attempt=next_attempt, last_error=error)

    def mark_failed(self, message_id: str, attempts: int, error: str):
        with self._lock:
            row = self._outbox.get(message_id)
            if row is not None:
                row.update(status="failed", attempts=attempts, last_error=error)

    def acknowledge(self, peer_id: int, message_id: str):
        with self._lock:
            row = self._outbox.get(message_id)
            if row is None or row["peer_id"] != peer_id:
                return None
            kind = row["kind"]
            del self._outbox[message_id]
            if kind == "request":
                request = self._requests.get(message_id)
                if request is not None:
                    request["accepted"] = True
                    self._requests.move_to_end(message_id)
            return kind

    def insert_inbox(self, peer_id: int, message_id: str, kind: str,
                     envelope: str, received_at: float) -> bool:
        key = (peer_id, message_id)
        with self._lock:
            self._check_open()
            if key in self._inbox:
                return False
            self._inbox[key] = {
                "kind": kind, "envelope": envelope, "received_at": received_at,
                "processed": False, "dispatch_attempts": 0, "next_dispatch": 0.0,
            }
            self._trim_inbox()
            return True

    def _trim_inbox(self):
        while len(self._inbox) > self.max_inbox_records:
            removable = next(
                (key for key, value in self._inbox.items() if value["processed"]),
                None,
            )
            if removable is None:
                # Never discard a pending item just to enforce the soft cap.
                break
            self._inbox.pop(removable, None)

    def _trim_responses(self):
        while len(self._responses) > self.max_response_records:
            self._responses.popitem(last=False)

    def _trim_completed_requests(self):
        # Preserve incomplete requests; discard only old requests with a response.
        while len(self._requests) > self.max_response_records:
            removable = next(
                (key for key, value in self._requests.items()
                 if value["response_json"] is not None),
                None,
            )
            if removable is None:
                break
            self._requests.pop(removable, None)

    def due_inbox(self, now: float, limit: int = 64):
        with self._lock:
            self._check_open()
            rows = []
            for (peer_id, message_id), row in self._inbox.items():
                if (
                    not row["processed"]
                    and row["next_dispatch"] is not None
                    and row["next_dispatch"] <= now
                ):
                    rows.append((
                        peer_id, message_id, row["kind"], row["envelope"],
                        row["received_at"], row["dispatch_attempts"],
                    ))
            rows.sort(key=lambda row: row[4])
            return rows[:limit]

    def retry_inbox_later(self, peer_id: int, message_id: str, attempts: int, when: float | None):
        with self._lock:
            row = self._inbox.get((peer_id, message_id))
            if row is not None and not row["processed"]:
                row.update(dispatch_attempts=attempts, next_dispatch=when)

    def complete_inbox(self, peer_id: int, message_id: str):
        with self._lock:
            row = self._inbox.get((peer_id, message_id))
            if row is not None:
                row["processed"] = True
                self._inbox.move_to_end((peer_id, message_id))
                self._trim_inbox()

    def store_response(self, request_id: str, response_json: str,
                       peer_id: int | None = None) -> bool:
        with self._lock:
            request = self._requests.get(request_id)
            if request is None or request["response_json"] is not None:
                return False
            if peer_id is not None and request["peer_id"] != peer_id:
                return False
            deadline = request["deadline"]
            if deadline is not None and time.monotonic() >= deadline:
                return False
            request["accepted"] = True
            request["response_json"] = response_json
            self._requests.move_to_end(request_id)
            self._trim_completed_requests()
            return True

    def next_deadline(
        self,
        active_outbox: set[str] | None = None,
        active_inbox: set[tuple[int, str]] | None = None,
    ) -> float | None:
        """Return the earliest pending monotonic deadline, excluding active work."""
        active_outbox = active_outbox or set()
        active_inbox = active_inbox or set()
        with self._lock:
            self._check_open()
            deadlines = [
                row["next_attempt"]
                for message_id, row in self._outbox.items()
                if row["status"] == "pending" and message_id not in active_outbox
            ]
            deadlines.extend(
                row["next_dispatch"]
                for key, row in self._inbox.items()
                if not row["processed"] and row["next_dispatch"] is not None and key not in active_inbox
            )
            deadlines.extend(
                row["deadline"]
                for row in self._requests.values()
                if row["response_json"] is None and row["deadline"] is not None
            )
            return min(deadlines) if deadlines else None

    def make_inbox_ready(self, kind: str) -> None:
        """Make retained messages of a newly registered handler type dispatchable."""
        with self._lock:
            self._check_open()
            for row in self._inbox.values():
                if not row["processed"] and row["kind"] == kind:
                    row["next_dispatch"] = 0.0

    def due_requests(self, now: float):
        with self._lock:
            return [
                (message_id, row["peer_id"], row["deadline"])
                for message_id, row in self._requests.items()
                if row["response_json"] is None
                and row["deadline"] is not None
                and row["deadline"] <= now
            ]

    def expire_request(self, request_id: str, response_json: str) -> bool:
        with self._lock:
            request = self._requests.get(request_id)
            if request is None or request["response_json"] is not None:
                return False
            request["response_json"] = response_json
            self._outbox.pop(request_id, None)
            self._requests.move_to_end(request_id)
            self._trim_completed_requests()
            return True

    def get_request(self, request_id: str):
        with self._lock:
            request = self._requests.get(request_id)
            if request is None:
                return None
            return (
                request["peer_id"], request["accepted"],
                request["response_json"], request["deadline"],
            )

    def pending_requests(self):
        with self._lock:
            return [
                (message_id, row["peer_id"], row["accepted"], row["response_json"])
                for message_id, row in self._requests.items()
                if row["response_json"] is None
            ]

    def close(self):
        with self._lock:
            self._closed = True
            self._outbox.clear()
            self._inbox.clear()
            self._requests.clear()
            self._responses.clear()
