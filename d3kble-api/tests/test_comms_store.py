import json
import time

from d3kble.comms_store import MessageStore
from d3kble.comms import _json_safe, _from_json_safe


def test_outbox_ack_and_durable_inbox(tmp_path):
    path = tmp_path / "messages.sqlite3"
    store = MessageStore(str(path))
    store.enqueue("m1", 7, "message", b'{"id":"m1"}')
    rows = store.due_outbox(time.time() + 1)
    assert rows[0][0:3] == ("m1", 7, "message")
    store.mark_transport_attempt("m1", 1, time.time() + 1)
    assert store.acknowledge(7, "m1") == "message"
    assert store.due_outbox(time.time() + 10) == []

    assert store.insert_inbox(7, "m2", "request", '{"id":"m2"}', time.time())
    assert not store.insert_inbox(7, "m2", "request", '{"id":"m2"}', time.time())
    pending = store.due_inbox(time.time() + 1)
    assert pending[0][0:3] == (7, "m2", "request")
    store.complete_inbox(7, "m2")
    assert store.due_inbox(time.time() + 1) == []
    store.close()

    reopened = MessageStore(str(path))
    assert reopened.due_inbox(time.time() + 1) == []
    reopened.close()


def test_response_is_enqueued_at_most_once():
    store = MessageStore(None)
    assert store.enqueue_response_once(7, "request-1", "response-1", b"{}")
    assert not store.enqueue_response_once(7, "request-1", "response-2", b"{}")
    rows = store.due_outbox(time.time() + 1)
    assert len(rows) == 1
    assert rows[0][0:3] == ("response-1", 7, "response")
    store.close()


def test_bytes_are_json_safe():
    original = {"raw": b"\x00\x01", "nested": [1, b"payload"]}
    round_trip = _from_json_safe(_json_safe(original))
    assert round_trip == original



def test_request_outbox_and_request_index_commit_atomically():
    import sqlite3
    import pytest

    store = MessageStore(None)
    store._db.execute("""
        CREATE TRIGGER reject_request BEFORE INSERT ON requests
        BEGIN SELECT RAISE(ABORT, 'test failure'); END;
    """)
    with pytest.raises(sqlite3.IntegrityError):
        store.enqueue("request-atomic", 7, "request", b"{}")
    assert store.due_outbox(time.time() + 1) == []
    store._db.execute("DROP TRIGGER reject_request")
    store.enqueue("request-atomic", 7, "request", b"{}")
    assert store.get_request("request-atomic") is not None
    store.close()
