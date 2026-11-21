import struct
from types import SimpleNamespace

from d3kble.ble import BleManager


class _Dispatcher:
    def emit(self, *_args, **_kwargs):
        pass


class _TransportWithImmediateDeliveryResult:
    """Deliver 0x8D before SEND_TO_PEER returns its message ID."""

    def __init__(self, message_id=37):
        self.message_id = message_id
        self.manager = None

    def request(self, opcode, payload, **_kwargs):
        assert opcode == "send_to_peer"
        peer_id, length = struct.unpack_from("<HH", payload)
        assert length == len(payload) - 4
        self.manager.on_event(
            SimpleNamespace(
                opcode=0x8D,
                payload=struct.pack("<HHH", peer_id, self.message_id, length),
            )
        )
        return struct.pack("<H", self.message_id)


def test_send_ticket_resolves_early_delivery_result():
    transport = _TransportWithImmediateDeliveryResult()
    manager = BleManager(transport, _Dispatcher(), delivery_timeout=0.1)
    transport.manager = manager
    manager.start()
    try:
        ticket = manager.send(18, b"ack")

        result = ticket.wait(timeout=0.5)

        assert result == {
            "delivered": True,
            "peer_id": 18,
            "message_id": 37,
            "total_length": 3,
        }
        assert manager.sent_count == 1
        assert manager._tickets == {}
        assert manager._early_results == {}
    finally:
        manager.close()
