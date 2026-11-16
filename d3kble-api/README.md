# d3kble API

Persistent Python host library for ESP32-C3 / DWM3000 nodes over a Raspberry Pi CM4 UART. It provides serial session management, UWB measurement streams, BLE transport, application messaging, observed device state, and safe runtime configuration reconciliation.

The library is application-domain agnostic: applications own numeric peer IDs, recipient selection, message meaning, sensor abstractions, and sensor fusion.

## Install

```sh
python -m pip install -e '.[dev]'
```

Python 3.10+ and pyserial are required.

## Quick start

```python
from d3kble import D3KNode

node = D3KNode(uart_device="/dev/ttyAMA2", baudrate=460800)
try:
    node.start()
    node.wait_until_ready(timeout=10)
    distances = node.uwb.measurements.subscribe()

    @node.comms.on_message
    def on_message(message):
        if message.peer_id != MY_PEER_ID:
            return
        handle_payload(message.payload)

    node.comms.send(PEER_ID, {"op": "status"})
finally:
    node.close()
```

Select the actual CM4 UART explicitly. There is no USB fallback or port guessing.

## Public API

- `node.uwb.measurements.subscribe()` creates an independent bounded measurement queue. Each subscriber receives typed `DistanceMeasurement` objects with device monotonic time, optional synchronized epoch time, sequence, status, quality flags, and host reception time. One slow consumer cannot steal from or block another.
- `node.comms.send(peer_id, payload)` sends to one numeric peer. `send_many(peer_ids, payload)` performs independent unicast sends and returns one ticket per peer. Peer IDs and recipient selection belong to the application.
- `@node.comms.on_message` receives messages from every peer. The application filters by `message.peer_id`.
- `node.get_state()` returns the latest runtime and saved state snapshots.
- `node.config.get_diff()` and `get_plan()` inspect desired-vs-runtime differences. Reconciliation defaults to report-only.

`d3kble` does not know about planes, payloads, vehicles, Kalman filters, IMUs, or computer vision. Those belong in the application.

## Communications reliability

The communications layer uses versioned application envelopes, unique application message IDs, SQLite inbox/outbox persistence, acceptance ACKs, response correlation, retransmission, and duplicate detection. ACKs are ordinary application payloads transported by BLE; firmware does not interpret them.

This provides at-least-once delivery with deduplication when both endpoints retain their persistent queues and eventually reconnect. Application side effects must be idempotent if retries could repeat them. The firmware's `GET_RECEIVED_DATA` consumes an unread transport message; if a host crash happens before durable acceptance, the sender's missing application ACK causes retransmission.

Default database: `~/.local/state/d3kble/messages.sqlite3`. Use `message_db_path=None` for explicitly non-durable in-memory storage.

## Firmware constraints

This implementation follows `../dwm3000-firmware/PROTOCOL.md`: up to eight BLE peer links, 496-byte payloads, one active outgoing message per peer, one unread complete incoming message per peer, `RANGE_RESULT` events, and the current configuration command semantics. `send_many` is fan-out over unicast; the firmware does not broadcast application payloads.

The runtime reconciler cannot access NVS-writing operations. It does not call `APPLY_SAVE_CONFIG`, `SAVE_ANTENNA_DELAYS`, `SET_PEERS`, or `RESET_CONFIG`. `SET_CONFIG` is used for volatile UWB/BLE configuration including the peer allow-list.

## Tests

```sh
python -m pytest
```

Unit tests use fake serial transports and SQLite. They do not replace hardware validation.