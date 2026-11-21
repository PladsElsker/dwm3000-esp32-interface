import struct

import pytest

from d3kble.ble import BleManager
from d3kble.protocol import Frame, TYPE_EVENT


class FakeDispatcher:
    def emit(self, *args):
        pass


class EarlyResultTransport:
    """Deliver the asynchronous result before SEND_TO_PEER returns its ID."""

    def __init__(self, manager, event_opcode):
        self.manager = manager
        self.event_opcode = event_opcode

    def request(self, opcode, payload, retryable=False):
        peer_id, data_length = struct.unpack_from("<HH", payload)
        assert opcode == "send_to_peer"
        assert data_length == len(payload) - 4

        message_id = 37
        if self.event_opcode == 0x8D:
            event_payload = struct.pack("<HHH", peer_id, message_id, data_length)
        else:
            event_payload = struct.pack("<HH", peer_id, message_id)

        # This runs before _tx_worker can parse the SEND_TO_PEER response and
        # register the future, reproducing the observed event-router race.
        self.manager.on_event(Frame(TYPE_EVENT, 0, self.event_opcode, 0, event_payload))
        return struct.pack("<H", message_id)


@pytest.mark.parametrize(
    ("event_opcode", "expected"),
    [
        (
            0x8D,
            {"delivered": True, "peer_id": 18, "message_id": 37, "total_length": 4},
        ),
        (
            0x8E,
            {"delivered": False, "expired": True, "peer_id": 18, "message_id": 37},
        ),
    ],
)
def test_delivery_result_is_retained_when_event_precedes_ticket_registration(
    event_opcode, expected
):
    manager = BleManager(None, FakeDispatcher(), delivery_timeout=0.2)
    manager.transport = EarlyResultTransport(manager, event_opcode)
    manager.start()
    try:
        ticket = manager.send(18, b"pong")
        assert ticket.wait(timeout=1.0) == expected
        assert manager.sent_count == (1 if event_opcode == 0x8D else 0)
        assert manager.expired_count == (1 if event_opcode == 0x8E else 0)
        assert (18, 37) not in manager._tickets
        assert (18, 37) not in manager._early_results
    finally:
        manager.close()
