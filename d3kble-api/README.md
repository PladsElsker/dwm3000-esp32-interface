# d3kble

`d3kble` is a Python host library for ESP32-C3 / DWM3000 nodes. It communicates with one node over an explicitly selected hardware UART and exposes UWB controls and measurements, BLE transport, application messaging, device state, configuration reconciliation, events, and diagnostics.

The UART device is required. The library does not scan for ports or fall back to USB.

## Requirements and installation

- Python 3.10 or newer
- A UART connection to the ESP32
- `pyserial`

Install from this directory:

```sh
python -m pip install -e '.[dev]'
```

## Connect to a node

```python
from d3kble import D3KNode

node = D3KNode(
    uart_device="/dev/ttyAMA2",
    baudrate=460800,
)
try:
    node.start()
    node.wait_until_ready(timeout=10)
    print(node.get_state())
finally:
    node.close()
```

The UART path must be selected explicitly. The default baud rate is 460800. Use `with D3KNode(...)` to start the node, wait for it to become ready, and close it automatically:

```python
with D3KNode("/dev/ttyAMA2") as node:
    print(node.get_state())
```

### Connection lifecycle

- `start()` starts the session and its background workers.
- `wait_until_ready(timeout=10.0)` waits for the UART session, event subscription, device information read, and initial status read to complete.
- `stop()` pauses the session and closes the UART connection. The node object and its in-memory manager state remain available for a later `start()`.
- `reconnect(timeout=10.0)` requests a new connection and waits for readiness.
- `close()` permanently closes the node object. A closed object cannot be restarted.
- `is_connected` reports the local serial transport state. It does not prove that the ESP32 is responding.
- `is_ready` is set after the initial device handshake and status read have succeeded.

With `auto_reconnect=True` (the default), the session supervisor retries failed connection attempts and reconnects after the serial reader detects a transport error. This is not a heartbeat or device-health watchdog: a silent UART link where reads continue to time out without raising an I/O error can remain marked connected, even while requests time out. A request timeout alone does not trigger reconnection. Set `auto_reconnect=False` to require an explicit `reconnect()` after a detected disconnect.

The constructor also accepts `poll_interval` (default 1.0 seconds), `request_timeout` (default 1.5 seconds), `request_retries` (default 1), and `reconcile_interval` (default 2.0 seconds).

## UWB

### Measurements

Every decoded ranging result is available through a callback or an independent subscription stream.

Register a callback:

```python
@node.uwb.on_measurement
def on_measurement(measurement):
    print(
        measurement.target_id,
        measurement.distance_m,
        measurement.valid,
    )
```

Or subscribe to an independent bounded queue:

```python
measurements = node.uwb.measurements.subscribe(
    maxsize=256,
    overflow="drop_oldest",
)

while True:
    measurement = measurements.get(timeout=1.0)
    print(measurement)
```

Each subscription has its own queue; consumers do not take measurements away from other subscribers. Supported overflow policies are:

- `drop_oldest` (default): discard the oldest queued item.
- `drop_newest`: discard the incoming item.
- `raise`: report `queue.Full` through the node's `error` event when the event router attempts to publish to a full queue.

The subscription exposes `dropped_count`, `get()`, `get_nowait()`, `close()`, and iteration. `get(timeout=...)` raises `queue.Empty` when its timeout expires. Remove a subscription with `node.uwb.measurements.unsubscribe(subscription)`.

A `DistanceMeasurement` contains:

| Field | Description |
|---|---|
| `target_id` | Target node ID |
| `distance_m` | Distance in metres, or `None` when the range is invalid |
| `valid` | Whether a usable distance is present and the firmware status is successful |
| `sample_sequence` | Firmware measurement sequence |
| `device_monotonic_us` | Device monotonic timestamp in microseconds |
| `epoch_us` | Synchronized epoch timestamp in microseconds, or `None` if unavailable |
| `status` | Firmware measurement status |
| `quality_flags` | Firmware measurement quality flags |
| `received_at` | Host monotonic time when the record was decoded |
| `job_id` | Ranging job identifier |

The device timestamps are preserved as reported by firmware. `received_at` is a separate host-side receipt timestamp.

### Ranging controls and history

```python
node.uwb.set_role("initiator")       # "idle", "initiator", or "responder"
node.uwb.set_targets([200, 201])
node.uwb.set_schedule(period_ms=100, timeout_ms=120, retries=1)

node.uwb.start_ranging()
job_id = node.uwb.range_once(target_id=200)
node.uwb.stop_ranging()

latest = node.uwb.get_measurements()
history = node.uwb.get_history(after_sample_seq=0, max_records=17)
```

`get_measurements(target_id=0xFFFF)` reads the latest firmware measurement records; pass a target ID to filter. `get_history()` returns a `UwbHistoryPage` with the available records, sequence bounds, next cursor, and overrun indication. History is firmware-managed and subject to the device's available history window.

Measurement callbacks are also emitted as the `range_result` event. Failed ranges are available through the `range_failed` event and `RangeFailure` records. History overruns are reported through `history_overrun`.

## BLE transport

`node.ble` exposes the firmware's byte-oriented BLE transport:

```python
ticket = node.ble.send(peer_id=200, data=b"hello")
result = ticket.wait(timeout=10)
print(result["delivered"])
```

The send operation is unicast to one numeric peer ID. The returned `BleSendTicket` exposes `peer_id`, `done`, and `wait(timeout)`. A successful result indicates that the firmware reported delivery at the BLE transport layer; it is not an application-level acknowledgement.

Incoming raw BLE payloads are emitted as `ble_message` events:

```python
@node.on("ble_message")
def on_ble_message(message):
    print(message.peer_id, message.message_id, message.data)
```

The event value is a `BleMessage` with `peer_id`, firmware `message_id`, raw byte `data`, and host `received_at`.

Other available operations are:

```python
node.ble.start_ble()
node.ble.stop_ble()
peer_status = node.ble.get_peer_status(peer_id=200)
```

The firmware supports up to eight simultaneous BLE peer links and a maximum 496-byte transport payload. It provides one active outgoing message and one unread complete incoming message per peer. `GET_RECEIVED_DATA` consumes that unread incoming message. A peer ID identifies a firmware peer; it is not an authentication credential. BLE links do not currently use pairing or encryption.

## Application messaging

`node.comms` provides message envelopes, message IDs, acknowledgements, bounded retries, duplicate detection, and request/response correlation on top of the BLE transport.

### Request/response

Request/response is the normal application messaging interface. Awaiting a request pauses only the current coroutine; other tasks on the same asyncio loop continue to run.

Start the node before entering the application event loop:

```python
import asyncio
from d3kble import D3KNode

node = D3KNode("/dev/ttyAMA2")
node.start()
node.wait_until_ready()

async def decision_loop():
    while True:
        response = await node.comms.request(
            200,
            {"operation": "get_status"},
        )

        if response.status == "error":
            print("Request failed:", response.error)
            continue

        status = response.content
        print("Remote status:", status)

async def main():
    node.comms.bind_asyncio_loop(asyncio.get_running_loop())
    await asyncio.gather(
        decision_loop(),
        sensor_fusion_loop(),
        camera_loop(),
        motor_control_loop(),
    )

try:
    asyncio.run(main())
finally:
    node.close()
```

The other application loops above are placeholders for application-owned coroutines. CPU-intensive work must not run synchronously on the asyncio event loop.

A request ticket is returned immediately. It is awaitable and resolves to a `CommsResponse`:

| Field | Description |
|---|---|
| `status` | `"ok"` or `"error"` |
| `error` | `None` on success, otherwise the error reason |
| `content` | Response data, or `None` when no content is available |
| `ok` | Convenience property; true when `status == "ok"` |

The default response deadline is **1.0 second**, measured from when the request is queued. If no response arrives before the deadline, awaiting the ticket returns an error `CommsResponse`; it does not raise a timeout exception. A late response is ignored. Use `ticket.wait_response(timeout=...)` only from synchronous code; it blocks that calling thread. `ticket.wait_ack()` waits for the separate internal receipt acknowledgement and is not required before awaiting the response.

Handle incoming requests by returning content, returning an explicit `CommsResponse`, calling `request.respond()`, or calling `request.fail()`:

```python
from d3kble import CommsResponse

@node.comms.on_request
async def handle_request(request):
    if request.payload.get("operation") == "get_status":
        return {"state": "ready"}

    return CommsResponse(
        status="error",
        error="unsupported operation",
        content=None,
    )
```

A plain return value (including `None`) is treated as successful response content. Handler exceptions are sent to the requester as error responses. Request handlers are not automatically re-executed after an exception because they may already have performed side effects.

By default, each async handler runs to completion on a communications worker thread using a temporary event loop. Call `node.comms.bind_asyncio_loop(asyncio.get_running_loop())` from a running application loop to execute async handlers on that loop instead. This allows handlers to coordinate with the application's other asyncio tasks.

Register `node.comms.on_response(callback)` to receive response objects through a callback as well. The callback receives an `ApplicationMessage`; its `payload` is the `CommsResponse`. A request ticket can be looked up again with `node.comms.resume_request(message_id)` while the same node process remains alive. `node.comms.pending_requests()` lists requests that have not yet received a response or timed out.

### One-way messages

One-way messages remain available for notifications or data that does not need a correlated result:

```python
ticket = node.comms.send(200, {"event": "heartbeat"})
ack = ticket.wait_ack(timeout=2)
```

`send_many(peer_ids, payload)` creates independent unicast messages with separate message IDs and tickets. Peer IDs must be unique in a `send_many` call. Use request/response for commands where the application needs the remote outcome before making its next decision.

Payloads may contain JSON-compatible values and `bytes`. Byte strings are encoded inside the message envelope and restored on receipt. The 496-byte firmware limit applies to the complete encoded envelope, including its metadata.

### Acknowledgement and state lifetime

An application acknowledgement means the receiving process has recorded the message in its in-memory inbox. It does not mean the registered handler has completed. The response indicates the handler's result.

Messaging state is volatile and exists only in the running Python process. The library does not persist messages to disk or recover them after process restart. `stop()` and serial reconnects retain in-memory queues; `close()` clears them. The inbox deduplication history is bounded (4096 records by default), and the response history is bounded (2048 records by default). Retries are bounded (12 attempts by default); exhausted sends fail their ticket and emit `comms_failed`.

Because state is not durable, a message can be processed again after a process restart if the sender retries it. Applications that perform non-repeatable side effects should account for possible duplicate delivery.

## Device state

`node.get_state()` returns the latest snapshot. Use `node.get_state(refresh=True)` or `node.refresh()` to request fresh values.

A `DeviceState` contains:

- `runtime`: configuration currently reported by the ESP32 RAM state.
- `saved`: configuration read from non-volatile storage (NVS).
- `status`: live device status, including service state.
- Validity flags and monotonic update timestamps for each observation.

The background state poller refreshes runtime configuration and status at the configured `poll_interval`. Failed reads mark the affected observation invalid; previous values are retained but are not marked current.

## Configuration and reconciliation

The configuration manager compares a desired configuration with the device's observed runtime state and produces a plan of runtime actions.

Choose the desired source:

```python
node.config.set_source("nvs")  # use saved device configuration as the desired values
# or provide explicit desired values:
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

Inspect the difference and plan:

```python
changes = node.config.get_diff()
plan = node.config.get_plan()
print(changes, plan.actions)
assert plan.nvs_writes == 0
```

Reconciliation modes are:

- `report_only` (default): calculate and return the plan without executing it.
- `manual`: execute only when `reconcile(approve=True)` is called.
- `automatic`: the session supervisor periodically plans and executes reconciliation.

```python
node.config.set_mode("manual")
result = node.config.reconcile(approve=True)
```

Reconciliation is restricted to volatile runtime operations. The executor cannot issue `APPLY_SAVE_CONFIG`, `SAVE_ANTENNA_DELAYS`, `SET_PEERS`, or `RESET_CONFIG`; `plan.nvs_writes` is always zero. Selecting `"nvs"` reads saved values as the desired configuration but does not write to NVS. Runtime UWB and BLE service states can be specified with `ranging_enabled` and `ble_enabled`.

## Events and diagnostics

Register node events with `node.on(event_name, callback)` or as a decorator. Remove a callback with `node.off(event_name, callback)`.

Common events include:

- Connection: `connected`, `ready`, `disconnected`, `connection_error`
- UWB: `range_result`, `range_failed`, `history_overrun`
- Device: `state_changed`, `device_ready`, `error`
- BLE: `ble_state_changed`, `ble_peer_found`, `ble_peer_connected`, `ble_peer_disconnected`, `ble_connection_failed`, `ble_message`, `ble_error`
- Messaging: `comms_received`, `comms_ack`, `comms_response`, `comms_processed`, `comms_timeout`, `comms_failed`, `comms_error`, `comms_handler_error`
- Configuration: `reconciliation_complete`, `reconciliation_failed`, `reconciliation_error`

Callbacks are dispatched outside the serial reader thread. Exceptions raised by callbacks are isolated and counted in diagnostics.

```python
diagnostics = node.get_diagnostics()
```

The diagnostics snapshot includes transport frame and receive-error counters, event drops, callback errors, BLE send/receive counters, and communications retry/duplicate counters.

## Firmware protocol and tests

The package implements the host protocol documented in [`../dwm3000-firmware/PROTOCOL.md`](../dwm3000-firmware/PROTOCOL.md). Host and firmware versions must use compatible command, event, and payload definitions.

Run the automated tests:

```sh
python -m pip install -e '.[dev]'
python -m pytest
```

Automated tests use a fake serial transport. The [`manual_tests/`](manual_tests/README.md) directory contains hardware integration scripts for validating UART, UWB ranging, BLE transport, and application messaging on real devices. Run those tests from this directory after installing the package.

## Exceptions

The package exports `D3KError` as the base exception, with `ProtocolError`, `ConnectionLost`, `RequestTimeout`, `DeviceCommandError`, and `ReconciliationError` for protocol decoding, connection, request deadline, firmware command, and configuration reconciliation failures respectively. Standard Python exceptions such as `ValueError`, `TypeError`, and `TimeoutError` are also used for invalid arguments and wait deadlines.
