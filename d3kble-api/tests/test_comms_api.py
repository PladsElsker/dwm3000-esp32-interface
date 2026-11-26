import asyncio
import json
import threading
import time
from concurrent.futures import Future

from d3kble.comms import CommsManager
from d3kble.models import BleMessage, CommsResponse


class FakeTicket:
    def __init__(self):
        self.future = Future()
        self.future.set_result({"delivered": True})

    def wait(self, timeout=None):
        return self.future.result(timeout=timeout)


class FakeBle:
    def __init__(self):
        self.on_message = None
        self.sent = []

    def send(self, peer_id, data):
        self.sent.append((peer_id, data))
        return FakeTicket()


class FakeDispatcher:
    def emit(self, *args):
        pass


def test_send_many_uses_numeric_peer_ids_and_separate_tickets():
    ble = FakeBle()
    manager = CommsManager(ble, FakeDispatcher())
    tickets = manager.send_many([2, 5, 8], {"operation": "status"})
    assert [ticket.peer_id for ticket in tickets] == [2, 5, 8]
    assert len({ticket.message_id for ticket in tickets}) == 3
    rows = manager.store.due_outbox(10**12)
    assert {row[1] for row in rows} == {2, 5, 8}
    manager.close()


def test_receiver_records_in_memory_before_sending_acceptance_ack():
    ble = FakeBle()
    manager = CommsManager(ble, FakeDispatcher())
    envelope = {
        "v": 1, "id": "application-id", "kind": "message",
        "payload": {"value": 42},
    }
    message = BleMessage(
        peer_id=7, message_id=11,
        data=json.dumps(envelope).encode("utf-8"), received_at=1.0,
    )
    manager.receive_ble_message(message)

    rows = manager.store.due_inbox(10**12)
    assert len(rows) == 1
    assert rows[0][0:3] == (7, "application-id", "message")
    assert len(ble.sent) == 1
    ack = json.loads(ble.sent[0][1].decode("utf-8"))
    assert ack["kind"] == "ack"
    assert ack["ack_id"] == "application-id"

    manager.receive_ble_message(message)
    assert manager.duplicates == 1
    assert len(ble.sent) == 2
    assert len(manager.store.due_inbox(10**12)) == 1
    manager.close()


def test_request_ticket_is_awaitable_and_returns_response_object():
    manager = CommsManager(FakeBle(), FakeDispatcher())
    ticket = manager.request(7, {"operation": "status"})
    manager._receive_response(7, {
        "v": 1,
        "id": "response-id",
        "kind": "response",
        "reply_to": ticket.message_id,
        "payload": {
            "status": "ok",
            "error": None,
            "content": {"ready": True},
        },
    })

    async def wait_for_ticket():
        return await ticket

    response = asyncio.run(wait_for_ticket())
    assert response == CommsResponse("ok", None, {"ready": True})
    manager.close()


def test_new_incoming_message_wakes_dispatcher():
    manager = CommsManager(FakeBle(), FakeDispatcher())
    handled = threading.Event()
    manager.on_message(lambda _message: handled.set())
    manager.start()
    try:
        envelope = {
            "v": 1, "id": "event-driven-message", "kind": "message",
            "payload": {"move": "forward"},
        }
        message = BleMessage(
            peer_id=7, message_id=12,
            data=json.dumps(envelope).encode("utf-8"), received_at=time.monotonic(),
        )
        manager.receive_ble_message(message)
        assert handled.wait(timeout=0.5)
    finally:
        manager.close()


def test_outgoing_message_wakes_transmit_scheduler():
    ble = FakeBle()
    manager = CommsManager(ble, FakeDispatcher())
    manager.start()
    try:
        ticket = manager.send(7, {"operation": "status"})
        deadline = time.monotonic() + 0.5
        while not ble.sent and time.monotonic() < deadline:
            time.sleep(0.001)
        sent_messages = [
            json.loads(payload.decode("utf-8"))
            for _peer, payload in ble.sent
        ]
        assert any(
            item.get("kind") == "message" and item["id"] == ticket.message_id
            for item in sent_messages
        )
    finally:
        manager.close()


def test_request_timeout_resolves_to_error_response_and_cleans_up():
    manager = CommsManager(FakeBle(), FakeDispatcher(), response_timeout=0.05)
    ticket = manager.request(7, {"operation": "status"})
    manager.start()
    try:
        response = ticket.wait_response(timeout=0.5)
        assert response.status == "error"
        assert "0.050 seconds" in response.error
        assert response.content is None
        assert manager.pending_requests() == []
        assert all(row[0] != ticket.message_id for row in manager.store.due_outbox(time.monotonic() + 10))
    finally:
        manager.close()


def test_async_request_handler_returns_structured_response():
    manager = CommsManager(FakeBle(), FakeDispatcher())

    async def handler(request):
        await asyncio.sleep(0.001)
        return {"echo": request.payload}

    manager.on_request(handler)
    envelope = {
        "v": 1, "id": "incoming-request", "kind": "request",
        "payload": {"value": 42},
    }
    manager.store.insert_inbox(
        8, "incoming-request", "request", json.dumps(envelope), time.time()
    )
    manager._dispatch_inbox(manager.store.due_inbox(time.monotonic() + 1)[0])

    rows = manager.store.due_outbox(time.monotonic() + 1)
    assert len(rows) == 1
    assert rows[0][2] == "response"
    wire = json.loads(rows[0][3].decode("utf-8"))["payload"]
    assert wire == {"status": "ok", "error": None, "content": {"echo": {"value": 42}}}
    manager.close()


def test_request_handler_failure_is_returned_and_not_retried():
    manager = CommsManager(FakeBle(), FakeDispatcher())
    calls = []

    def handler(request):
        calls.append(request.message_id)
        raise RuntimeError("operation failed")

    manager.on_request(handler)
    envelope = {
        "v": 1, "id": "failing-request", "kind": "request", "payload": {},
    }
    manager.store.insert_inbox(
        8, "failing-request", "request", json.dumps(envelope), time.time()
    )
    manager._dispatch_inbox(manager.store.due_inbox(time.monotonic() + 1)[0])

    assert calls == ["failing-request"]
    assert manager.store.due_inbox(time.monotonic() + 60) == []
    rows = manager.store.due_outbox(time.monotonic() + 1)
    assert len(rows) == 1
    response = json.loads(rows[0][3].decode("utf-8"))["payload"]
    assert response["status"] == "error"
    assert "operation failed" in response["error"]
    manager.close()



def test_closing_manager_resolves_outstanding_request_ticket():
    manager = CommsManager(FakeBle(), FakeDispatcher())
    ticket = manager.request(7, {"operation": "status"})
    manager.close()
    response = ticket.wait_response(timeout=0.1)
    assert response.status == "error"
    assert "manager closed" in response.error
