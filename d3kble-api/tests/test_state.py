import threading

import d3kble.state as state_module
from d3kble.state import StateManager


class FakeTransport:
    connected = True

    def __init__(self):
        self.calls = []
        self.status_read = threading.Event()

    def request(self, opcode):
        self.calls.append(opcode)
        if opcode == "get_status":
            self.status_read.set()
        return b"state"


def test_background_poller_only_refreshes_live_status(monkeypatch):
    transport = FakeTransport()
    monkeypatch.setattr(state_module, "decode_status", lambda _raw: {"ready": True})
    manager = StateManager(transport, poll_interval=0.2)
    manager.start()
    try:
        assert transport.status_read.wait(timeout=1.0)
    finally:
        manager.close()

    assert transport.calls
    assert set(transport.calls) == {"get_status"}


def test_explicit_refresh_still_reads_runtime_and_saved_configuration(monkeypatch):
    transport = FakeTransport()
    monkeypatch.setattr(state_module, "decode_runtime_config", lambda _raw: {})
    monkeypatch.setattr(state_module, "decode_targets", lambda _raw: [])
    monkeypatch.setattr(state_module, "decode_schedule", lambda _raw: {})
    monkeypatch.setattr(state_module, "decode_status", lambda _raw: {"ready": True})
    monkeypatch.setattr(state_module, "decode_saved_config", lambda _raw: {})
    manager = StateManager(transport)
    manager.refresh(include_saved=True)

    assert transport.calls == [
        "get_config", "get_targets", "get_schedule", "get_status", "get_saved_config"
    ]
