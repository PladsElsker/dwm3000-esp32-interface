"""Application-level reliable messaging on top of the firmware BLE transport."""
from __future__ import annotations

import asyncio
import base64
import inspect
import json
import threading
import time
import uuid
from concurrent.futures import Future, ThreadPoolExecutor

from .comms_state import CommsState
from .exceptions import RequestTimeout
from .models import ApplicationMessage, CommsResponse


def _json_safe(value):
    if isinstance(value, bytes):
        return {"$d3kble_bytes": base64.b64encode(value).decode("ascii")}
    if isinstance(value, dict):
        return {str(k): _json_safe(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(v) for v in value]
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    raise TypeError(f"payload value {type(value).__name__} is not JSON serializable")


def _from_json_safe(value):
    if isinstance(value, dict):
        if set(value) == {"$d3kble_bytes"}:
            return base64.b64decode(value["$d3kble_bytes"])
        return {k: _from_json_safe(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_from_json_safe(v) for v in value]
    return value


class CommsTicket:
    """Non-blocking handle for a queued message or request.

    Request tickets are awaitable and resolve to a :class:`CommsResponse`.
    """

    def __init__(self, message_id, peer_id, kind, ack_future, response_future=None):
        self.message_id = message_id
        self.peer_id = peer_id
        self.kind = kind
        self.ack_future = ack_future
        self.response_future = response_future

    @property
    def done(self) -> bool:
        return self.response_future.done() if self.response_future else self.ack_future.done()

    def wait_ack(self, timeout: float | None = None):
        return self.ack_future.result(timeout=timeout)

    def wait_response(self, timeout: float | None = None) -> CommsResponse:
        if self.response_future is None:
            raise RuntimeError("this ticket is not a request")
        return self.response_future.result(timeout=timeout)

    def wait(self, timeout: float | None = None):
        return self.wait_ack(timeout)

    def __await__(self):
        if self.response_future is None:
            raise RuntimeError("only request tickets can be awaited")
        async def wait_for_response():
            wrapped = asyncio.wrap_future(self.response_future)
            return await asyncio.shield(wrapped)
        return wait_for_response().__await__()


class IncomingRequest:
    def __init__(self, message: ApplicationMessage, respond):
        self.message = message
        self._respond_callback = respond
        self._responded = False

    @property
    def peer_id(self):
        return self.message.peer_id

    @property
    def payload(self):
        return self.message.payload

    @property
    def message_id(self):
        return self.message.message_id

    def respond(self, payload):
        if self._responded:
            raise RuntimeError("request has already been answered")
        self._responded = True
        if not isinstance(payload, CommsResponse):
            payload = CommsResponse(status="ok", error=None, content=payload)
        return self._respond_callback(payload)

    def fail(self, reason: str, content=None):
        """Return an explicit error response to the requester."""
        if not reason:
            raise ValueError("reason must not be empty")
        return self.respond(CommsResponse(status="error", error=str(reason), content=content))


class CommsManager:
    VERSION = 1

    def __init__(
        self, ble, dispatcher,
        ack_timeout: float = 1.5, delivery_timeout: float = 8.0,
        max_attempts: int = 12, response_timeout: float = 1.0,
    ):
        if response_timeout <= 0:
            raise ValueError("response_timeout must be greater than zero")
        self.ble = ble
        self.dispatcher = dispatcher
        self.store = CommsState()
        self._asyncio_loop: asyncio.AbstractEventLoop | None = None
        self.ack_timeout = ack_timeout
        self.delivery_timeout = delivery_timeout
        self.max_attempts = max_attempts
        self.response_timeout = response_timeout
        self._callbacks = {"message": [], "request": [], "response": []}
        self._callback_lock = threading.RLock()
        self._tickets: dict[str, CommsTicket] = {}
        self._tickets_lock = threading.RLock()
        self._active_tx: set[str] = set()
        self._active_tx_lock = threading.RLock()
        self._active_dispatch: set[tuple[int, str]] = set()
        self._active_dispatch_lock = threading.RLock()
        self._running = threading.Event()
        self._thread: threading.Thread | None = None
        self._scheduler_condition = threading.Condition()
        self._scheduler_generation = 0
        self._executor = ThreadPoolExecutor(max_workers=8, thread_name_prefix="d3kble-comms-tx")
        self._dispatch_executor = ThreadPoolExecutor(max_workers=4, thread_name_prefix="d3kble-comms-handler")
        self.tx_retries = 0
        self.duplicates = 0
        self.rx_messages = 0
        self.ble.on_message = self.receive_ble_message

    def start(self):
        if self._running.is_set():
            return
        self._running.set()
        self._thread = threading.Thread(target=self._run, name="d3kble-comms", daemon=True)
        self._thread.start()

    def _wake_scheduler(self) -> None:
        """Wake the scheduler after new work or a deadline change."""
        with self._scheduler_condition:
            self._scheduler_generation += 1
            self._scheduler_condition.notify()

    def bind_asyncio_loop(self, loop: asyncio.AbstractEventLoop):
        """Run async request/message/response handlers on an application event loop."""
        if not loop.is_running():
            raise RuntimeError("the asyncio event loop must already be running")
        self._asyncio_loop = loop

    @staticmethod
    async def _await_value(value):
        return await value

    def _run_awaitable(self, value):
        loop = self._asyncio_loop
        if loop is not None and loop.is_running():
            return asyncio.run_coroutine_threadsafe(
                self._await_value(value), loop
            ).result()
        return asyncio.run(self._await_value(value))

    def on_message(self, callback=None):
        return self._register("message", callback)

    def on_request(self, callback=None):
        return self._register("request", callback)

    def on_response(self, callback=None):
        return self._register("response", callback)

    def _register(self, kind, callback):
        def add(fn):
            if not callable(fn):
                raise TypeError("callback must be callable")
            with self._callback_lock:
                self._callbacks[kind].append(fn)
            if kind in {"message", "request"}:
                self.store.make_inbox_ready(kind)
            self._wake_scheduler()
            return fn
        return add(callback) if callback is not None else add

    def _new_envelope(self, kind: str, payload=None, reply_to: str | None = None):
        message_id = uuid.uuid4().hex
        envelope = {"v": self.VERSION, "id": message_id, "kind": kind, "payload": _json_safe(payload)}
        if reply_to is not None:
            envelope["reply_to"] = reply_to
        encoded = json.dumps(envelope, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        if len(encoded) > 496:
            raise ValueError(f"encoded envelope is {len(encoded)} bytes; BLE limit is 496")
        return message_id, encoded

    def send(self, peer_id: int, payload) -> CommsTicket:
        return self._enqueue(peer_id, "message", payload)

    def send_many(self, peer_ids, payload) -> list[CommsTicket]:
        peers = [int(peer) for peer in peer_ids]
        if len(set(peers)) != len(peers):
            raise ValueError("send_many peer IDs must be unique")
        return [self.send(peer, payload) for peer in peers]

    def request(self, peer_id: int, payload) -> CommsTicket:
        """Queue a request. Await the returned ticket for its response (1s deadline)."""
        return self._enqueue(
            peer_id, "request", payload, expects_response=True,
            response_timeout=self.response_timeout,
        )

    def _enqueue(
        self, peer_id: int, kind: str, payload,
        expects_response: bool = False, response_timeout: float | None = None,
    ):
        peer_id = int(peer_id)
        if not 1 <= peer_id <= 65534:
            raise ValueError("peer_id must be in 1..65534")
        message_id, envelope = self._new_envelope(kind, payload)
        deadline = time.monotonic() + response_timeout if response_timeout is not None else None
        self.store.enqueue(message_id, peer_id, kind, envelope, response_deadline=deadline)
        ack = Future()
        response = Future() if expects_response else None
        ticket = CommsTicket(message_id, peer_id, kind, ack, response)
        with self._tickets_lock:
            self._tickets[message_id] = ticket
        self._wake_scheduler()
        return ticket

    def resume_request(self, message_id: str) -> CommsTicket:
        row = self.store.get_request(message_id)
        if row is None:
            raise KeyError(message_id)
        peer_id, accepted, response_json, _deadline = row
        ack, response = Future(), Future()
        if accepted:
            ack.set_result({"accepted": True, "message_id": message_id})
        if response_json is not None:
            saved_response = self._decode_response(json.loads(response_json))
            response.set_result(saved_response)
            if not accepted:
                ack.set_exception(RequestTimeout(saved_response.error or "request did not receive an acknowledgement"))
        ticket = CommsTicket(message_id, peer_id, "request", ack, response)
        with self._tickets_lock:
            self._tickets[message_id] = ticket
        self._wake_scheduler()
        return ticket

    def receive_ble_message(self, message) -> None:
        try:
            envelope = json.loads(message.data.decode("utf-8"))
            if envelope.get("v") != self.VERSION or not isinstance(envelope.get("id"), str):
                raise ValueError("unsupported application envelope")
            kind = envelope.get("kind")
            if kind == "ack":
                self._receive_ack(message.peer_id, envelope)
                return
            if kind not in {"message", "request", "response"}:
                raise ValueError(f"unknown message kind {kind!r}")
            message_id = envelope["id"]
            received_at = time.time()
            encoded = json.dumps(envelope, separators=(",", ":"), ensure_ascii=False)
            inserted = self.store.insert_inbox(message.peer_id, message_id, kind, encoded, received_at)
            if not inserted:
                self.duplicates += 1
            else:
                self.rx_messages += 1
            # Record receipt in process memory before sending the application ACK.
            self._send_ack(message.peer_id, message_id)
            if inserted and kind != "response":
                self._wake_scheduler()
            if inserted and kind == "response":
                self._receive_response(message.peer_id, envelope)
                self.store.complete_inbox(message.peer_id, message_id)
            self.dispatcher.emit("comms_received", {
                "peer_id": message.peer_id, "message_id": message_id,
                "kind": kind, "duplicate": not inserted,
            })
        except Exception as exc:
            self.dispatcher.emit("comms_error", exc)

    def _send_ack(self, peer_id: int, acknowledged_id: str):
        envelope = {
            "v": self.VERSION, "id": uuid.uuid4().hex,
            "kind": "ack", "ack_id": acknowledged_id,
        }
        data = json.dumps(envelope, separators=(",", ":")).encode("utf-8")
        try:
            future = self.ble.send(peer_id, data).future
            future.add_done_callback(
                lambda f: self.dispatcher.emit("comms_error", f.exception())
                if not f.cancelled() and f.exception() else None
            )
        except Exception as exc:
            self.dispatcher.emit("comms_error", exc)

    def _receive_ack(self, peer_id: int, envelope: dict):
        ack_id = envelope.get("ack_id")
        if not isinstance(ack_id, str):
            return
        kind = self.store.acknowledge(peer_id, ack_id)
        with self._tickets_lock:
            ticket = self._tickets.get(ack_id)
            if ticket and not ticket.ack_future.done():
                ticket.ack_future.set_result({
                    "accepted": True, "peer_id": peer_id, "message_id": ack_id,
                })
                if kind != "request":
                    self._tickets.pop(ack_id, None)
        self.dispatcher.emit("comms_ack", {"peer_id": peer_id, "message_id": ack_id, "kind": kind})
        self._wake_scheduler()

    @staticmethod
    def _response_to_wire(response: CommsResponse) -> dict:
        return {
            "status": response.status,
            "error": response.error,
            "content": _json_safe(response.content),
        }

    @staticmethod
    def _decode_response(value) -> CommsResponse:
        value = _from_json_safe(value)
        if isinstance(value, dict) and {"status", "error", "content"} <= set(value):
            return CommsResponse(
                status=value["status"], error=value["error"], content=value["content"]
            )
        # Accept an unwrapped response from an older d3kble host.
        return CommsResponse(status="ok", error=None, content=value)

    def _receive_response(self, peer_id: int, envelope: dict):
        request_id = envelope.get("reply_to")
        if not isinstance(request_id, str):
            return
        response = self._decode_response(envelope.get("payload"))
        serialized = json.dumps(
            self._response_to_wire(response), separators=(",", ":"), ensure_ascii=False
        )
        # A response that arrives after the local deadline is deliberately ignored.
        if not self.store.store_response(request_id, serialized, peer_id=peer_id):
            return
        # The response resolves this request's deadline; recalculate the next
        # scheduler wake rather than sleeping until the now-obsolete timeout.
        self._wake_scheduler()
        with self._tickets_lock:
            ticket = self._tickets.get(request_id)
            if ticket:
                if not ticket.ack_future.done():
                    ticket.ack_future.set_result({
                        "accepted": True, "peer_id": peer_id, "message_id": request_id,
                    })
                if ticket.response_future and not ticket.response_future.done():
                    ticket.response_future.set_result(response)
                self._tickets.pop(request_id, None)
        app_message = ApplicationMessage(
            peer_id=peer_id, message_id=envelope["id"], kind="response",
            payload=response, received_at=time.time(), reply_to=request_id,
        )
        with self._callback_lock:
            callbacks = tuple(self._callbacks["response"])
        for callback in callbacks:
            self._dispatch_executor.submit(self._safe_response_callback, callback, app_message)
        self.dispatcher.emit("comms_response", app_message)

    def _expire_request(self, message_id: str, peer_id: int, deadline: float):
        reason = f"no response received within {self.response_timeout:.3f} seconds"
        response = CommsResponse(status="error", error=reason, content=None)
        serialized = json.dumps(
            self._response_to_wire(response), separators=(",", ":"), ensure_ascii=False
        )
        if not self.store.expire_request(message_id, serialized):
            return
        with self._tickets_lock:
            ticket = self._tickets.pop(message_id, None)
        if ticket:
            if not ticket.ack_future.done():
                ticket.ack_future.set_exception(RequestTimeout(reason))
            if ticket.response_future and not ticket.response_future.done():
                ticket.response_future.set_result(response)
        self.dispatcher.emit("comms_timeout", {
            "peer_id": peer_id, "message_id": message_id, "error": reason,
            "deadline": deadline,
        })

    def _fail_request(self, message_id: str, peer_id: int, reason: str):
        response = CommsResponse(status="error", error=reason, content=None)
        serialized = json.dumps(
            self._response_to_wire(response), separators=(",", ":"), ensure_ascii=False
        )
        if not self.store.expire_request(message_id, serialized):
            return
        with self._tickets_lock:
            ticket = self._tickets.pop(message_id, None)
        if ticket:
            if not ticket.ack_future.done():
                ticket.ack_future.set_exception(RequestTimeout(reason))
            if ticket.response_future and not ticket.response_future.done():
                ticket.response_future.set_result(response)
        self.dispatcher.emit("comms_failed", {
            "peer_id": peer_id, "message_id": message_id, "error": reason,
        })

    def _safe_response_callback(self, callback, response):
        try:
            result = callback(response)
            if inspect.isawaitable(result):
                self._run_awaitable(result)
        except Exception as exc:
            self.dispatcher.emit("comms_handler_error", {
                "peer_id": response.peer_id, "message_id": response.message_id, "error": exc,
            })

    def _run(self):
        while self._running.is_set():
            with self._scheduler_condition:
                generation = self._scheduler_generation

            now = time.monotonic()
            for message_id, peer_id, deadline in self.store.due_requests(now):
                self._expire_request(message_id, peer_id, deadline)
            for row in self.store.due_outbox(now):
                message_id = row[0]
                with self._active_tx_lock:
                    if message_id in self._active_tx:
                        continue
                    self._active_tx.add(message_id)
                self._executor.submit(self._transmit, row)
            for row in self.store.due_inbox(now):
                peer_id, message_id, kind, _encoded, _received_at, _attempts = row
                with self._callback_lock:
                    has_handler = bool(self._callbacks[
                        "request" if kind == "request" else "message"
                    ])
                if not has_handler:
                    # Retain the inbox entry without periodically retrying an
                    # unavailable handler. Handler registration reactivates it.
                    self.store.retry_inbox_later(peer_id, message_id, _attempts, None)
                    continue
                key = (peer_id, message_id)
                with self._active_dispatch_lock:
                    if key in self._active_dispatch:
                        continue
                    self._active_dispatch.add(key)
                self._dispatch_executor.submit(self._dispatch_inbox, row)

            with self._active_tx_lock:
                active_tx = set(self._active_tx)
            with self._active_dispatch_lock:
                active_dispatch = set(self._active_dispatch)
            deadline = self.store.next_deadline(active_tx, active_dispatch)
            timeout = None if deadline is None else max(0.0, deadline - time.monotonic())

            with self._scheduler_condition:
                if not self._running.is_set():
                    break
                if generation == self._scheduler_generation:
                    self._scheduler_condition.wait(timeout)

    def _transmit(self, row):
        message_id, peer_id, kind, envelope, attempts = row
        try:
            result = self.ble.send(peer_id, bytes(envelope)).wait(timeout=self.delivery_timeout + 1.0)
            if not result.get("delivered"):
                raise ConnectionError(f"BLE message expired for peer {peer_id}")
            attempts += 1
            if attempts >= self.max_attempts:
                error = f"application ACK not received after {attempts} attempts"
                self.store.mark_failed(message_id, attempts, error)
                if kind == "request":
                    self._fail_request(message_id, peer_id, error)
                else:
                    with self._tickets_lock:
                        ticket = self._tickets.pop(message_id, None)
                    if ticket and not ticket.ack_future.done():
                        ticket.ack_future.set_exception(TimeoutError(error))
                    self.dispatcher.emit("comms_failed", {
                        "peer_id": peer_id, "message_id": message_id, "error": error,
                    })
            else:
                self.store.mark_transport_attempt(message_id, attempts, time.monotonic() + self.ack_timeout)
        except Exception as exc:
            attempts += 1
            self.tx_retries += 1
            if attempts >= self.max_attempts:
                self.store.mark_failed(message_id, attempts, str(exc))
                if kind == "request":
                    self._fail_request(message_id, peer_id, str(exc))
                else:
                    with self._tickets_lock:
                        ticket = self._tickets.pop(message_id, None)
                    if ticket and not ticket.ack_future.done():
                        ticket.ack_future.set_exception(exc)
                    self.dispatcher.emit("comms_failed", {
                        "peer_id": peer_id, "message_id": message_id, "error": str(exc),
                    })
            else:
                delay = min(10.0, 0.25 * (2 ** min(attempts, 5)))
                self.store.mark_transport_attempt(message_id, attempts, time.monotonic() + delay, str(exc))
        finally:
            with self._active_tx_lock:
                self._active_tx.discard(message_id)
            self._wake_scheduler()

    def _dispatch_inbox(self, row):
        peer_id, message_id, kind, encoded, received_at, attempts = row
        try:
            envelope = json.loads(encoded)
            if kind == "response":
                self._receive_response(peer_id, envelope)
                self.store.complete_inbox(peer_id, message_id)
                return
            payload = _from_json_safe(envelope.get("payload"))
            message = ApplicationMessage(
                peer_id=peer_id, message_id=message_id, kind=kind,
                payload=payload, received_at=received_at, reply_to=envelope.get("reply_to"),
            )
            callback_kind = "request" if kind == "request" else "message"
            with self._callback_lock:
                callbacks = tuple(self._callbacks[callback_kind])
            if not callbacks:
                self.store.retry_inbox_later(peer_id, message_id, attempts, time.monotonic() + 1.0)
                return
            if kind == "request":
                request = IncomingRequest(
                    message, lambda response: self._respond(peer_id, message_id, response)
                )
                result = callbacks[0](request)
                if inspect.isawaitable(result):
                    result = self._run_awaitable(result)
                if not request._responded:
                    request.respond(result)
            else:
                for callback in callbacks:
                    result = callback(message)
                    if inspect.isawaitable(result):
                        self._run_awaitable(result)
            self.store.complete_inbox(peer_id, message_id)
            self.dispatcher.emit("comms_processed", {
                "peer_id": peer_id, "message_id": message_id, "kind": kind,
            })
        except Exception as exc:
            if kind == "request":
                # Do not automatically re-run request handlers: they may have
                # performed side effects before raising.
                try:
                    self._respond(
                        peer_id, message_id,
                        CommsResponse(
                            status="error",
                            error=f"{type(exc).__name__}: {exc}",
                            content=None,
                        ),
                    )
                    self.store.complete_inbox(peer_id, message_id)
                except Exception as response_error:
                    self.dispatcher.emit("comms_error", response_error)
                    self.store.complete_inbox(peer_id, message_id)
            else:
                attempts += 1
                self.store.retry_inbox_later(
                    peer_id, message_id, attempts,
                    time.monotonic() + min(30.0, 0.5 * (2 ** min(attempts, 6))),
                )
            self.dispatcher.emit("comms_handler_error", {
                "peer_id": peer_id, "message_id": message_id, "error": exc,
            })
        finally:
            with self._active_dispatch_lock:
                self._active_dispatch.discard((peer_id, message_id))
            self._wake_scheduler()

    def _respond(self, peer_id: int, request_id: str, payload):
        if not isinstance(payload, CommsResponse):
            payload = CommsResponse(status="ok", error=None, content=payload)
        response_id, envelope = self._new_envelope(
            "response", self._response_to_wire(payload), reply_to=request_id
        )
        self.store.enqueue_response_once(peer_id, request_id, response_id, envelope)
        self.store.complete_inbox(peer_id, request_id)
        self._wake_scheduler()
        return response_id

    def pending_requests(self):
        return self.store.pending_requests()

    def close(self):
        self._running.clear()
        self._wake_scheduler()
        reason = "communications manager closed before the operation completed"
        with self._tickets_lock:
            tickets = tuple(self._tickets.values())
            self._tickets.clear()
        for ticket in tickets:
            if not ticket.ack_future.done():
                ticket.ack_future.set_exception(RequestTimeout(reason))
            if ticket.response_future and not ticket.response_future.done():
                ticket.response_future.set_result(
                    CommsResponse(status="error", error=reason, content=None)
                )
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=2)
        self._executor.shutdown(wait=True, cancel_futures=True)
        self._dispatch_executor.shutdown(wait=True, cancel_futures=True)
        self.store.close()
