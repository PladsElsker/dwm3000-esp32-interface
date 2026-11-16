import time
import pytest

from d3kble.comms_state import CommsState


def test_volatile_outbox_ack_and_inbox_deduplication():
    state = CommsState()
    state.enqueue("m1", 7, "message", b'{"id":"m1"}')
    rows = state.due_outbox(time.time() + 1)
    assert rows[0][0:3] == ("m1", 7, "message")
    state.mark_transport_attempt("m1", 1, time.time() + 1)
    assert state.acknowledge(7, "m1") == "message"
    assert state.due_outbox(time.time() + 10) == []

    assert state.insert_inbox(7, "m2", "request", '{"id":"m2"}', time.time())
    assert not state.insert_inbox(7, "m2", "request", '{"id":"m2"}', time.time())
    pending = state.due_inbox(time.time() + 1)
    assert pending[0][0:3] == (7, "m2", "request")
    state.complete_inbox(7, "m2")
    assert state.due_inbox(time.time() + 1) == []


def test_response_is_enqueued_at_most_once_per_request():
    state = CommsState()
    assert state.enqueue_response_once(7, "request-1", "response-1", b"{}")
    assert not state.enqueue_response_once(7, "request-1", "response-2", b"{}")
    rows = state.due_outbox(time.time() + 1)
    assert len(rows) == 1
    assert rows[0][0:3] == ("response-1", 7, "response")


def test_request_outbox_and_ack_state_are_in_memory():
    state = CommsState()
    deadline = time.time() + 1
    state.enqueue("request-1", 7, "request", b"{}", response_deadline=deadline)
    assert state.get_request("request-1") == (7, False, None, deadline)
    assert state.acknowledge(7, "request-1") == "request"
    assert state.get_request("request-1") == (7, True, None, deadline)
    assert state.store_response("request-1", '{"status":"ok"}')
    assert state.get_request("request-1") == (7, True, '{"status":"ok"}', deadline)


def test_processed_inbox_history_is_bounded_but_pending_items_are_kept():
    state = CommsState(max_inbox_records=2)
    for i in range(3):
        state.insert_inbox(7, f"m{i}", "message", "{}", time.time())
        state.complete_inbox(7, f"m{i}")
    assert len(state.due_inbox(time.time() + 1)) == 0
    assert len(state._inbox) == 2
    assert (7, "m0") not in state._inbox

    state.insert_inbox(7, "pending", "message", "{}", time.time())
    state.insert_inbox(7, "pending2", "message", "{}", time.time())
    assert (7, "pending") in state._inbox
    assert (7, "pending2") in state._inbox


def test_close_discards_all_volatile_message_state():
    state = CommsState()
    state.enqueue("m1", 7, "message", b"{}")
    state.insert_inbox(7, "m2", "message", "{}", time.time())
    state.close()
    assert state._outbox == {}
    assert state._inbox == {}
    with pytest.raises(RuntimeError):
        state.enqueue("m3", 7, "message", b"{}")


def test_bytes_are_json_safe():
    from d3kble.comms import _json_safe, _from_json_safe
    original = {"raw": b"\x00\x01", "nested": [1, b"payload"]}
    assert _from_json_safe(_json_safe(original)) == original



def test_request_deadline_expires_once_and_rejects_late_response():
    state = CommsState()
    deadline = time.time() + 0.01
    state.enqueue("request-timeout", 7, "request", b"{}", response_deadline=deadline)
    time.sleep(0.02)
    assert state.due_requests(time.time()) == [
        ("request-timeout", 7, deadline)
    ]
    assert state.expire_request("request-timeout", '{"status":"error"}')
    assert state.due_requests(time.time() + 10) == []
    assert not state.store_response("request-timeout", '{"status":"ok"}')
    assert state.get_request("request-timeout")[2] == '{"status":"error"}'
