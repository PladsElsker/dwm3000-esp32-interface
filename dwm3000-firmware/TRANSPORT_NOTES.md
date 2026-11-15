# Host transports

The UWB node exposes one binary host protocol over two interfaces:

- **USB CDC:** native ESP32-C3 USB Serial/JTAG peripheral through Arduino `Serial`. Configure Hardware CDC/JTAG USB mode and enable USB CDC On Boot.
- **UART1:** `PiSerial` at 460800 baud, 8N1, using XIAO D6 (GPIO21 TX) and D7 (GPIO20 RX).

Both interfaces use the same COBS-framed, CRC-protected command and event protocol. They share command handlers and device state, including configuration, ranging, measurement history, statistics, and BLE state.

Each transport has independent receive framing, duplicate-response caching, and event subscriptions. A response or duplicate replay is returned on the interface that received the request. Events are generated once and sent to each interface subscribed to the corresponding event type. A USB CDC disconnect clears that interface's receive and subscription session after the disconnect debounce interval; the UART session remains independent.

Neither interface carries text diagnostics. Use D3KM or `pi_client.py` to interact with the node; both communicate using the binary protocol. The USB serial monitor is not a human-readable console.

## Hardware integration

The firmware targets the Seeed XIAO ESP32-C3. UART1 uses GPIO20 RX and GPIO21 TX. USB CDC requires the Arduino Hardware CDC/JTAG and USB CDC On Boot settings. The bundled Makerfabs DW3000 driver includes the XIAO-specific SPI and reset pin configuration documented in the firmware README.

## BLE processing

The BLE manager uses NimBLE-Arduino 2.5.2. It runs from the main firmware loop after host commands are serviced and before the UWB radio scheduler. NimBLE callbacks enqueue bounded packets and state changes; the main loop processes those queues through `ble.poll()`. BLE events are translated into host protocol events and delivered according to each transport's subscription mask.

BLE scanning, advertising, peer connection management, reconnection, and message delivery run in the node firmware. D3KM and the Python client configure the BLE network and use host commands to inspect peer state and exchange application messages. See [PROTOCOL.md](PROTOCOL.md) for the serial and GATT interfaces.
