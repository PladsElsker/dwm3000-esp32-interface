# Dual host transport change

The node exposes the same binary COBS protocol on two interfaces:

- USB CDC via `Serial` (requires ESP32-C3 Hardware CDC/JTAG USB mode and USB CDC On Boot enabled).
- UART1 via `PiSerial`, using the existing XIAO D6/D7 pin mapping (GPIO21 TX, GPIO20 RX).

Both inputs have independent COBS receive buffers, duplicate-response caches, and event subscription masks. Responses and duplicate replays go only to the interface that delivered the request. Events are generated once and broadcast to each interface that subscribed to the relevant event bit. Configuration, ranging state, history and radio state remain shared because both interfaces address the same node.

No text logging is written to either interface; both are binary protocol links. The USB serial monitor therefore needs a binary-capable host tool to exercise the protocol. A human-readable CLI/UI is intentionally not included in this transport change.

## Build note

Build the XIAO ESP32-C3 with Hardware CDC/JTAG USB mode and USB CDC On Boot enabled so `Serial` is the native USB CDC interface. `PiSerial` remains UART1 on GPIO20 RX / GPIO21 TX. The Makerfabs DW3000 driver is bundled under `lib/Dw3000`; its XIAO SPI/reset port modifications are already applied.

## BLE transport integration

The BLE transport is implemented in `BleManager.*` and uses NimBLE-Arduino 2.5.2. It runs from the shared main loop after both host transports are serviced and before the UWB radio scheduler. BLE callbacks copy bounded packets/state into queues; packet parsing, retries, and host events are processed by `ble.poll()` in the main loop. See `PROTOCOL.md` for the GATT and serial APIs.
