# Manual hardware tests

These scripts exercise the `d3kble` API against real ESP32-C3 / DWM3000 devices. They are deliberately kept separate from the package examples and normal unit tests because they require connected hardware and an operator to prepare the device state.

## Setup

From the `d3kble-api/` directory, install the package in editable mode and run the scripts:

```sh
python -m pip install -e '.[dev]'
python manual_tests/01_status.py --uart /dev/ttyAMA2
```

Every script requires an explicit UART device path. The scripts import the local API package.

Use the UART port actually wired to the ESP32. The default baud rate is 460800; pass `--baudrate` to override it. The scripts do not scan for ports or fall back to USB.

## Tests

### 1. UART connection and device status

```sh
python manual_tests/01_status.py --uart /dev/ttyAMA2
```

Checks the host handshake, runtime configuration read, saved configuration read, live status read, and host-side diagnostics. Resolve failures here before proceeding to UWB or BLE tests.

### 2. One-shot UWB ranging

```sh
python manual_tests/02_uwb_single_range.py --uart /dev/ttyAMA2 --target-id 200
```

Issues one `RANGE_ONCE` job and waits for its matching result or failure event. The local node must already be configured as an initiator, and the radio must be ready and awake. The target ID must belong to a reachable responder. The script does not change the device role, target list, or radio state.

Use `--timeout 30` to extend the result deadline.

### 3. Periodic UWB measurement stream

Observe ranging that is already running:

```sh
python manual_tests/03_uwb_stream.py --uart /dev/ttyAMA2 --duration 10
```

Start ranging for the duration of this test:

```sh
python manual_tests/03_uwb_stream.py --uart /dev/ttyAMA2 --duration 10 --start
```

The node must have initiator role, at least one configured target, and a valid schedule. Without `--start`, the script only observes an already-running stream. With `--start`, it starts ranging only if ranging is currently stopped, then stops the ranging service when the observation period ends.

### 4. Raw BLE receive and send

Start the receiver first:

```sh
python manual_tests/04_ble_receive.py --uart /dev/ttyAMA3 --timeout 30
```

Then send from the other node:

```sh
python manual_tests/05_ble_send.py --uart /dev/ttyAMA2 --peer-id 2 --text "hello from node 1"
```

Use the receiving node's configured device ID for the sender's `--peer-id`. The receive script can optionally filter by sender with `--peer-id`.

Both nodes must already have compatible BLE configuration and an established connection to each other. These scripts do not enable BLE or initiate peer discovery. The send test waits for the firmware's BLE delivery result, which is emitted after the receiving host retrieves the complete raw message. This confirms transport delivery, not an application-level response.

### 5. Application messaging request/response

Start the echo server on the receiving node:

```sh
python manual_tests/06_comms_echo_server.py --uart /dev/ttyAMA3 --duration 120
```

Send a request from the other node:

```sh
python manual_tests/07_comms_request.py --uart /dev/ttyAMA2 --peer-id 2
```

Both scripts exercise the awaitable API. The echo server binds its running asyncio loop and registers an `async def` request handler. The requester calls `node.comms.request(...)`, receives a ticket immediately, and awaits that ticket for its correlated `CommsResponse`. The response deadline is one second. The nodes must already have an established BLE connection and compatible peer configuration.

## Notes

- Replace example UART paths and peer IDs with values matching the actual hardware.
- The sender's `--peer-id` is the remote device ID. The receiver identifies the sender by its device ID.
- Start receive and echo-server processes before sending.
- UWB tests require the appropriate role, target IDs, radio state, and schedule. They report missing preconditions instead of silently reconfiguring the node.
- BLE tests require BLE to be enabled and the intended peer to be connected before the test starts.
- Messaging state is in memory only. These are functional checks, not persistence or process-restart tests.
- Do not run these tests concurrently with another host application that is controlling the same UART or changing device configuration.
