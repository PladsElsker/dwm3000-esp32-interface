# d3kble

A persistent, domain-agnostic Python API for ESP32-C3 / DWM3000 nodes. The Raspberry Pi CM4 connects to the ESP32 through an explicitly selected hardware UART. USB is not used by this host library.

`d3kble` owns serial framing, request/response matching, reconnects, UWB event decoding, BLE transport queues, application messaging, device-state polling, and safe runtime reconciliation. Applications own peer IDs, recipient selection, message meaning, sensor abstractions, and sensor fusion.

## Install

Python 3.10+:

```sh
python -m pip install -e '.[dev]'
```

## Quick start

```python
from d3kble import D3KNode

PEER_ID = 2

node = D3KNode(uart_device="/dev/ttyAMA2", baudrate=460800)
try:
    node.start()
    node.wait_until_ready(timeout=10)
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

The UART path is required; the library does not guess a port or fall back to USB. The default baud rate is 460800. You can also use `with D3KNode(...) as node:` to manage lifecycle automatically. `stop()` pauses the session without discarding subscriptions or in-memory message queues; `start()` resumes it, and `reconnect()` explicitly reconnects.

## UWB measurements

`node.uwb.measurements.subscribe()` is the primary measurement interface. Every subscriber receives an independent bounded queue.

```python
stream = node.uwb.measurements.subscribe(maxsize=256, overflow="drop_oldest")
measurement = stream.get(timeout=1.0)
```

A `DistanceMeasurement` contains `target_id`, `distance_m`, `valid`, `sample_sequence`, device monotonic microseconds, optional synchronized epoch microseconds, status, quality flags, and host `received_at`. Use the device measurement timestamp for sensor fusion; host reception time is not a substitute.

Slow consumers do not steal measurements from other subscribers. Queue overflow is observable through `subscription.dropped_count`; policies are `drop_oldest` (default), `drop_newest`, and `raise`.

```python
node.uwb.start_ranging()
node.uwb.stop_ranging()
job_id = node.uwb.range_once(target_id=200)
latest = node.uwb.get_measurements()
history = node.uwb.get_history(after_sample_seq=0, max_records=17)
```

The application owns sensor fusion. d3kble does not know about Kalman filters, IMUs, ArUco, vehicles, or payloads.

## BLE and application messaging

Firmware sends to one numeric peer ID at a time. Peer IDs and recipient selection belong to the application:

```python
PLANE_NODE_PEER_ID = 2
ALL_PAYLOAD_PEERS = [5, 6, 7, 8, 9]

node.comms.send(PLANE_NODE_PEER_ID, {"operation": "status"})
tickets = node.comms.send_many(ALL_PAYLOAD_PEERS, {"operation": "sync"})
```

`send_many` is host-side fan-out into independent unicast sends, each with its own message ID, delivery ticket, ACK state, and retry lifecycle.

One general callback receives all peers; filter by numeric ID in the application:

```python
@node.comms.on_message
def on_message(message):
    if message.peer_id != PLANE_NODE_PEER_ID:
        return
    handle_payload(message.payload)
```

JSON-compatible values and `bytes` are supported. Bytes are base64-encoded in the application envelope. The 496-byte firmware limit applies to the complete encoded envelope, including metadata.

### Request/response

```python
ticket = node.comms.request(PEER_ID, {"operation": "get_status"})
accepted = ticket.wait_ack(timeout=10)
response = ticket.wait_response(timeout=10)
```

On the receiving node:

```python
@node.comms.on_request
def handle_request(request):
    if request.peer_id != PEER_ID:
        return {"error": "unsupported peer"}
    return {"status": "ready"}
```

A request handler must return a response or call `request.respond(payload)` before returning.

### Delivery and volatile message state

The communications middleware adds application message IDs, ACKs, bounded retries, duplicate detection, and request/response correlation over the firmware's BLE transport. The sender retains an unacknowledged envelope in its in-memory outbox and retransmits the same application ID. The receiver records the incoming ID in its in-memory inbox before acknowledging it. Duplicate deliveries while the process is alive are acknowledged without reprocessing.

**All messaging state is in memory.** There is no database, file persistence, or process-restart recovery. Outbox entries, inbox deduplication history, request tickets, and queued responses are lost when the Python process exits or the node object is closed. Reconnects within the same running process retain pending queues. A message processed before a process restart may be delivered and processed again if its sender retries it; application handlers should be idempotent when repeated side effects matter.

Inbox deduplication history is bounded (4096 records by default), so a sufficiently old duplicate may be treated as new after its record is evicted. Retries are bounded; exhausted sends are marked failed and reported through `comms_failed`.

## Device state

`node.get_state()` returns the last observed snapshot; `node.get_state(refresh=True)` forces a refresh. It separates runtime configuration in ESP32 RAM, saved configuration in NVS, and live status. Failed polls mark observations invalid/stale rather than pretending configuration is empty. Polling is automatic at a default one-second interval.

## Configuration and reconciliation

```python
node.config.set_source("nvs")
# or supply an explicit desired configuration
node.config.set_desired({
    "device_id": 100, "pan_id": 0xCAFE, "channel": 5,
    "role": "initiator", "targets": [200, 201],
    "period_ms": 100, "timeout_ms": 120, "retries": 1,
})
diff = node.config.get_diff()
plan = node.config.get_plan()
assert plan.nvs_writes == 0
```

Reconciliation defaults to `report_only`; other modes are `manual` and `automatic`. The volatile-only executor cannot issue `APPLY_SAVE_CONFIG`, `SAVE_ANTENNA_DELAYS`, `SET_PEERS`, or `RESET_CONFIG`. `SET_CONFIG` handles runtime UWB/BLE configuration, including the BLE peer allow-list. NVS boot preferences are not silently treated as live service enable/disable intent; specify `ranging_enabled` or `ble_enabled` for those runtime states.

## Events and diagnostics

```python
@node.on("connected")
def connected(info):
    print("Connected")

@node.on("range_failed")
def range_failed(failure):
    print(failure)

diagnostics = node.get_diagnostics()
```

Callbacks run outside the serial reader. Callback errors are isolated and counted.

## Firmware constraints

This package follows `../dwm3000-firmware/PROTOCOL.md`:

- Up to eight simultaneous BLE peer links.
- Maximum 496-byte BLE application payload.
- One active outgoing message and one unread complete incoming message per peer.
- `SEND_TO_PEER` is unicast; `send_many` is application-side fan-out over unicasts.
- `GET_RECEIVED_DATA` consumes the unread transport message. Application ACKs allow a sender that remains running to retransmit if the receiver did not acknowledge.

No firmware changes are required. BLE links currently do not require pairing or encryption; peer IDs are identifiers, not authentication credentials.

## Tests

```sh
python -m pip install -e '.[dev]'
python -m pytest
```

Tests use a fake serial transport and in-memory state. Hardware UART, RF ranging, and multi-peer BLE still require hardware validation.
