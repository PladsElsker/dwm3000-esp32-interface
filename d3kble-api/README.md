# d3kble

A persistent, domain-agnostic Python API for ESP32-C3 / DWM3000 nodes. The Raspberry Pi CM4 connects to the ESP32 through an explicitly selected hardware UART. USB is not used by this host library.

`d3kble` owns serial framing, request/response matching, reconnects, UWB event decoding, BLE transport queues, application messaging reliability, device-state polling, and safe runtime reconciliation. Applications own peer IDs, recipient selection, message meaning, sensor abstractions, and sensor fusion.

## Install

Python 3.10+:

```sh
python -m pip install -e '.[dev]'
```

## Quick start

```python
from d3kble import D3KNode

PEER_ID = 2

node = D3KNode(
    uart_device="/dev/ttyAMA2",  # Use the actual CM4 UART configured for the wiring.
    baudrate=460800,
)

try:
    node.start()
    node.wait_until_ready(timeout=10)

    # Independent, bounded measurement stream.
    distances = node.uwb.measurements.subscribe(maxsize=256)

    @node.comms.on_message
    def on_message(message):
        if message.peer_id != PEER_ID:
            return
        print("Received:", message.payload)

    node.comms.send(PEER_ID, {"operation": "status"})

    while True:
        measurement = distances.get(timeout=1.0)
        if measurement.valid:
            print(measurement.target_id, measurement.distance_m)
finally:
    node.close()
```

The UART path is required; the library does not guess a port or fall back to USB. The default baud rate is 460800.

You can also use `with D3KNode(...) as node:` to start, wait for readiness, and close automatically. `stop()` pauses the session without discarding subscriptions or persistent message queues; `start()` resumes it. `reconnect()` explicitly reconnects.

## UWB measurements

`node.uwb.measurements.subscribe()` is the primary real-time measurement interface. Every subscriber has an independent bounded queue; subscribers do not steal records from each other.

```python
stream = node.uwb.measurements.subscribe(maxsize=256, overflow="drop_oldest")
measurement = stream.get(timeout=1.0)
```

Each immutable `DistanceMeasurement` includes:

- `target_id`, `distance_m`, and `valid`
- `sample_sequence`
- `device_monotonic_us` (measurement time since device boot)
- `epoch_us` when device time has been synchronized, otherwise `None`
- `status` and `quality_flags`
- `received_at` (host monotonic receive time)

Use the device measurement timestamp for sensor fusion. Host callback/receive time is not a substitute for the actual measurement time.

The stream is independent of consumers: an estimator, logger, and visualizer can each subscribe. Queue overflow is observable with `subscription.dropped_count`. The default overflow policy discards the oldest queued item; choose `drop_newest` or `raise` when appropriate.

UWB controls and inspection:

```python
node.uwb.start_ranging()
node.uwb.stop_ranging()
job_id = node.uwb.range_once(target_id=200)
latest = node.uwb.get_measurements()  # Latest retained record per target.
history = node.uwb.get_history(after_sample_seq=0, max_records=17)
```

Ranging role, target list, and schedule must be configured on the ESP32. The library does not run a Kalman filter or know about any other sensors.

`node.uwb.on_measurement(callback)` is available as a callback convenience, but independent stream subscriptions are the preferred decoupled interface.

## BLE and application communications

The firmware offers peer-directed unicast, not application broadcast. Peer IDs and recipient selection belong to the application:

```python
PLANE_NODE_PEER_ID = 2
ALL_PAYLOAD_PEERS = [5, 6, 7, 8, 9]

node.comms.send(PLANE_NODE_PEER_ID, {"operation": "status"})
tickets = node.comms.send_many(ALL_PAYLOAD_PEERS, {"operation": "sync"})
```

`send_many` performs independent unicast sends. Each recipient has its own application message ID, transport delivery status, ACK state, and retry lifecycle. It returns one `CommsTicket` per peer.

Incoming application messages from all peers are delivered through one callback. The application filters by numeric ID:

```python
@node.comms.on_message
def on_message(message):
    if message.peer_id != PLANE_NODE_PEER_ID:
        return
    # Handle message.payload.
```

The public message contains `peer_id`, `message_id`, `kind`, `payload`, `received_at`, and optional `reply_to`. JSON-compatible payloads and bytes are supported. Bytes are base64 encoded inside the application envelope; the 496-byte firmware payload limit applies to the complete envelope, so the usable application payload is smaller when envelope overhead/encoding is present.

### Request/response

```python
request = node.comms.request(PEER_ID, {"operation": "get_status"})
accepted = request.wait_ack(timeout=10)
response = request.wait_response(timeout=10)
```

On the receiving side:

```python
@node.comms.on_request
def handle_request(request):
    if request.peer_id != PEER_ID:
        return {"error": "unsupported peer"}
    return {"status": "ready"}
```

A request handler must return a response or call `request.respond(payload)` before it returns. The middleware persists the response in its outbox and retries until the requester acknowledges it.

### Reliability model

Application messaging is implemented entirely above the firmware BLE transport:

1. The sender persists the request/message in its SQLite outbox.
2. The receiver retrieves the BLE payload, commits it to its SQLite inbox, then sends an application ACK.
3. Request handlers process durable inbox records and persist correlated responses before sending them.
4. Responses are persisted by the requester before the final application ACK is sent.
5. Retries reuse the same application message ID; receivers deduplicate repeated IDs.

Application ACKs are ordinary payloads. The firmware does not interpret them.

The result is **at-least-once delivery with deduplication and restart recovery**, assuming both endpoints retain their SQLite databases and eventually reconnect. Handler callbacks may be retried if the process crashes before recording completion. Application operations with non-repeatable side effects must therefore be idempotent or transactional.

The default persistent database is `~/.local/state/d3kble/messages.sqlite3`. Use `message_db_path=None` only when in-memory, non-crash-persistent queues are acceptable. The default retry policy is bounded; exhausted messages are marked failed and reported through `comms_failed`.

## Device state

`node.get_state()` returns the last observed snapshot. `node.get_state(refresh=True)` forces a refresh.

The snapshot separates:

- `runtime`: configuration in ESP32 RAM
- `saved`: configuration stored in NVS
- `status`: live operating state, including ranging and BLE enabled state

Each configuration snapshot includes `valid` and `updated_at`. A failed poll marks the observation invalid/stale; it is never interpreted as an empty configuration.

The poller runs automatically with a default one-second interval.

## Configuration and reconciliation

The desired configuration can be selected from NVS or supplied explicitly:

```python
node.config.set_source("nvs")
# or
node.config.set_desired({
    "device_id": 100,
    "pan_id": 0xCAFE,
    "channel": 5,
    "role": "initiator",
    "targets": [200, 201],
    "period_ms": 100,
    "timeout_ms": 120,
    "retries": 1,
})
```

Selecting a source does not itself change the device. Inspect the difference and proposed plan:

```python
diff = node.config.get_diff()
plan = node.config.get_plan()
assert plan.nvs_writes == 0
```

Reconciliation defaults to `report_only`. Other modes are `manual` and `automatic`:

```python
node.config.set_mode("manual")
result = node.config.reconcile(approve=True)

# Or explicitly enable the background reconciler:
node.config.set_reconciliation_enabled(True)
```

The reconciler has a restricted volatile-command interface and a second opcode allow-list. It cannot issue `APPLY_SAVE_CONFIG`, `SAVE_ANTENNA_DELAYS`, `SET_PEERS`, or `RESET_CONFIG`. `SET_CONFIG` is used for runtime UWB/BLE configuration (including the BLE peer allow-list); the separate `SET_PEERS` command persists and is not exposed to reconciliation.

Live service state is separate from NVS boot preferences. Selecting NVS as the desired configuration source does not implicitly start or stop ranging/BLE; specify `ranging_enabled` or `ble_enabled` explicitly when the application wants those runtime states reconciled.

## Diagnostics and events

Register ordinary node events with `node.on`:

```python
@node.on("connected")
def connected(info):
    print("Connected")

@node.on("range_failed")
def range_failed(failure):
    print(failure)

diagnostics = node.get_diagnostics()
```

Callbacks run outside the serial reader thread. Callback exceptions are isolated and counted.

## Firmware constraints

This package follows `../dwm3000-firmware/PROTOCOL.md` and the current firmware implementation:

- Eight simultaneous BLE peer links.
- Maximum 496-byte BLE application payload.
- One active outgoing message and one unread complete incoming message per peer.
- `SEND_TO_PEER` is unicast; `send_many` is host-side fan-out over separate unicasts.
- `GET_RECEIVED_DATA` consumes the unread transport message. The application outbox/inbox ACK protocol provides recovery if it was consumed but not durably accepted by the receiving host.

No firmware changes are included or required for this API.

BLE links in the current firmware do not require pairing or encryption. Device IDs are identifiers, not authentication credentials. This library provides delivery reliability, not cryptographic peer authentication.

## Tests

```sh
python -m pip install -e '.[dev]'
python -m pytest
```

The tests use fake serial transport and SQLite. They do not substitute for UART wiring, radio-range, or on-hardware multi-peer validation.
