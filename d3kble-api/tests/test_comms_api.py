import json
from concurrent.futures import Future

from d3kble.comms import CommsManager
from d3kble.models import BleMessage


class FakeTicket:
    def __init__(self):
        self.future = Future()
        self.future.set_result({"delivered": True})


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

    # The ACK transmission is issued only after recording the in-memory inbox entry.
    rows = manager.store.due_inbox(10**12)
    assert len(rows) == 1
    assert rows[0][0:3] == (7, "application-id", "message")
    assert len(ble.sent) == 1
    ack = json.loads(ble.sent[0][1].decode("utf-8"))
    assert ack["kind"] == "ack"
    assert ack["ack_id"] == "application-id"

    # Re-delivery is deduplicated but still acknowledged.
    manager.receive_ble_message(message)
    assert manager.duplicates == 1
    assert len(ble.sent) == 2
    assert len(manager.store.due_inbox(10**12)) == 1
    manager.close()
