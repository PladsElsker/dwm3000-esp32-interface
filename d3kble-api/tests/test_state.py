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


def test_background_poller_refreshes_status_and_runtime_at_different_cadences(monkeypatch):
    transport = FakeTransport()
    monkeypatch.setattr(state_module, "decode_runtime_config", lambda _raw: {})
    monkeypatch.setattr(state_module, "decode_targets", lambda _raw: [])
    monkeypatch.setattr(state_module, "decode_schedule", lambda _raw: {})
    monkeypatch.setattr(state_module, "decode_status", lambda _raw: {"ready": True})
    monkeypatch.setattr(state_module, "decode_saved_config", lambda _raw: {})

    manager = StateManager(transport, poll_interval=0.2)
    manager.refresh(include_saved=True)  # Same full snapshot performed on connect.
    transport.calls.clear()
    transport.status_read.clear()
    manager._runtime_poll_interval = 0.35
    manager.start()
    try:
        assert transport.status_read.wait(timeout=0.5)
        # Status polling must not trigger the multi-request configuration read.
        assert not any(call in transport.calls for call in (
            "get_config", "get_targets", "get_schedule"
        ))
        assert transport.status_read.is_set()
        # Configuration is still refreshed periodically, not left stale forever.
        deadline = time.monotonic() + 0.5
        while "get_config" not in transport.calls and time.monotonic() < deadline:
            time.sleep(0.005)
    finally:
        manager.close()

    assert transport.calls.count("get_status") >= 1
    assert transport.calls.count("get_config") == 1
    assert transport.calls.count("get_targets") == 1
    assert transport.calls.count("get_schedule") == 1


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
