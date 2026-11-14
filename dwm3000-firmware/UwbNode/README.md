# XIAO ESP32-C3 + DWM3000 UWB Node

Arduino firmware and a Raspberry Pi UART client for the custom board described in the project conversation.

## Contents

- `UwbNodeV7.ino` — host protocol, configuration, ranging scheduler, measurement ring buffer, asynchronous events, power control, and BLE integration.
- `Protocol.h/.cpp` — serial version 1 binary protocol (COBS framing + CRC-16/CCITT-FALSE).
- `BLE/` — NimBLE-Arduino BLE Central/Peripheral transport, packet framing, reassembly, retries, queues, and delivery-result handling.
- `pi_client.py` — Python command-line client for the Raspberry Pi.
- `PROTOCOL.md` — frame format, opcodes, payloads, and field formats.

## Hardware pin map

These assignments preserve the agreed XIAO ESP32-C3 map. UART pins are the XIAO's pin names, not Raspberry Pi physical pin numbers.

| XIAO pin | ESP32-C3 GPIO | Connect to |
|---|---:|---|
| D0 | GPIO2 | DWM3000 `SPICSn` |
| D1 | GPIO3 | DWM3000 `WAKEUP` |
| D2 | GPIO4 | DWM3000 `RSTn` (open-drain) |
| D3 | GPIO5 | DWM3000 `IRQ` |
| D4 | GPIO6 | Unused in this design |
| D5 | GPIO7 | Unused in this design |
| D6 | GPIO21 | UART TX to Raspberry Pi RXD, physical pin 10 |
| D7 | GPIO20 | UART RX from Raspberry Pi TXD, physical pin 8 |
| D8 | GPIO8 | DWM3000 SPI clock |
| D9 | GPIO9 | DWM3000 SPI MISO |
| D10 | GPIO10 | DWM3000 SPI MOSI |

Connect Pi ground to board ground and use 3.3 V UART logic. Never drive DWM3000 signal pins with 5 V logic.

**Boot-strapping warning:** GPIO2, GPIO8, and GPIO9 influence ESP32-C3 startup. D9 also connects to the XIAO BOOT button. Keep the agreed pinout only after testing cold boot, reset, USB upload, and BOOT-mode upload on your actual PCB. Add suitable biasing per the ESP32-C3/XIAO guidance, and verify that the attached DWM3000 does not force GPIO8/GPIO9 to incorrect levels at reset. In particular, avoid a pull-down on GPIO9 and ensure GPIO8 is high when entering the ROM download mode. The XIAO's D6 is also the ROM UART0 TX and can emit boot messages before application startup; the Pi parser discards malformed/CRC-invalid startup bytes.

Recommended passive components discussed during board design: a 10 kΩ pull-up on DWM3000 CSn / XIAO GPIO2, a 10 kΩ pull-up on XIAO GPIO8/SCK to support ROM download boot when BOOT forces GPIO9 low, a 100 kΩ pull-down on DWM3000 IRQ, and an optional 100 kΩ pull-down on WAKEUP. Confirm all pull values and reset topology against the actual DWM3000 reference schematic and module layout.

## Build environment

The supported project build uses PlatformIO and the Seeed XIAO ESP32-C3 board definition. The project pins **NimBLE-Arduino 2.5.2** in `platformio.ini`; the local Makerfabs `Dw3000` library is bundled under `lib/Dw3000` and contains the board-specific SPI/reset port changes already applied.

```bash
python3 -m pip install platformio
pio run
```

To upload to the connected board:

```bash
pio run -t upload
```

The monitor speed is 460800 baud. If using Arduino IDE instead, install NimBLE-Arduino 2.5.2, add the bundled `lib/Dw3000` library to the Arduino libraries folder, open `UwbNodeV7.ino` as a sketch, and select Seeed XIAO ESP32C3 with USB CDC on boot enabled. PlatformIO is the reference build because the dependency and board settings are declared in this project.

The custom Makerfabs port is already patched for the XIAO pin map. Do not run `patch_makerfabs_port.py` against the bundled source again unless you have first reviewed the source diff. The legacy helper remains available for patching a separately downloaded unmodified Makerfabs port.

## BLE peer messaging

BLE uses NimBLE-Arduino 2.5.2. The transport runs independently of the DWM3000 ranging state machine and uses the existing USB CDC/UART serial protocol as its host API.

- `IDLE` disables BLE activity; `HUB` runs as BLE Central; `PERIPHERAL` advertises as a BLE Peripheral.
- `START_BLE` and `STOP_BLE` control activity. `BLE_CONNECT` and `BLE_DISCONNECT` control a HUB connection.
- Configure up to eight peer IDs. The transport manager supports one active BLE connection at a time.
- Application messages are limited to 496 bytes and are fragmented according to the negotiated ATT MTU.
- `BLE_DATA_SENT` means the remote host retrieved the complete message. Fragment ACKs alone do not mean application-level delivery.
- Queues and in-flight messages are volatile and cleared on disconnection. The GATT link is not paired or encrypted in this release.

Example two-node setup (choose distinct local IDs; use matching peer lists):

Peripheral board, local ID 2:

```bash
python pi_client.py --port /dev/serial0 apply-save-config 2 0xDECA 5 16385 16385 --ble-mode peripheral --ble-peers 1
python pi_client.py --port /dev/serial0 start-ble
```

HUB board, local ID 1:

```bash
python pi_client.py --port /dev/serial0 apply-save-config 1 0xDECA 5 16385 16385 --ble-mode hub --ble-peers 2
python pi_client.py --port /dev/serial0 start-ble
python pi_client.py --port /dev/serial0 ble-connect 2
python pi_client.py --port /dev/serial0 peer-status 2
```

The host subscribes to event mask bits 6–14 for BLE events. On `BLE_DATA_RECEIVED`, retrieve the complete payload with `GET_RECEIVED_DATA` (`get-received-data PEER_ID MESSAGE_ID`). `send-to-peer PEER_ID TEXT` sends UTF-8 text; `send-to-peer PEER_ID HEX --hex` sends binary data. `GET_RECEIVED_DATA` is destructive at the BLE queue level, but the existing serial request-response cache replays the prior response for an identical serial request so a host retry does not consume a second message.

The complete BLE command/event layouts, GATT UUIDs, packet framing, retry policy, configuration extension, and delivery semantics are defined in `PROTOCOL.md`. Stop BLE before changing its operating role, peer list, scan settings, advertising settings, or retry/timeout configuration; `SET_CONFIG` returns `BUSY` rather than silently keeping stale radio parameters.

## Ranging details

- Radio profile: channel 5, 6.8 Mbps, 128-symbol preamble, STS disabled.
- Ranging: custom 4-frame double-sided two-way ranging (DS-TWR): `POLL → RESP → FINAL → REPORT`.
- A target must run this compatible firmware in `responder` role, with the same PAN ID/channel/radio profile and a unique 16-bit device ID.
- The `initiator` cycles through configured target IDs one at a time. `target_period_ms` is a desired minimum interval per target, not a guaranteed sample rate; the actual period grows with target count, transaction timeouts, and retries.
- As this is a custom over-the-air protocol, it is not automatically interoperable with Qorvo's standard Decawave example frames. All nodes that range with each other must use a compatible message format and matching UWB settings.
- Antenna delay values are placeholders (`16385`) and require calibration to achieve accurate absolute distances. Tune them with measured reference distances.
- STS is disabled in the initial radio profile and the custom ranging messages are not cryptographically authenticated. Treat this as an engineering prototype, not a secure ranging system.
- The default local clock is monotonic microseconds since boot. Send `TIME_SYNC` with Pi Unix time (microseconds) to populate the optional epoch timestamp on subsequent measurement records.

### DWM3000 sleep limitation

The bundled driver contains the Qorvo `dwt_configuresleep()` / `dwt_entersleep()` APIs, but the current firmware command path does not configure or invoke them. Consequently:

- `UWB_SLEEP` enters the radio's `IDLE_RC` standby state, **not true DWM3000 deep sleep**. Its response payload is `01` to identify this standby mode.
- `UWB_WAKE` requests a transition from IDLE_RC to IDLE over SPI and reapplies the radio profile. If SPI does not respond, it falls back to an active-low reset pulse and full initialization. Its response payload `00` indicates a wake/reinitialization; payload `01` means it was already awake. This is not a verified CSn wake-up sequence from true DWM3000 deep sleep.
- `SYSTEM_SLEEP` does put the ESP32-C3 into deep sleep, with a timer wake requested by the Pi. It requires a duration between 1000 ms and seven days. The device restarts after the timer; the Pi must reconnect and reconfigure/restart ranging. **UART commands do not wake the ESP32-C3 from deep sleep.** Timer wake is the only system wake source implemented here.

Do not treat `IDLE_RC` as equivalent to deep sleep. Enabling true DWM3000 deep sleep remains a separate firmware task: configure the bundled Qorvo API for the board's wake sources, then validate `SPICSn` and `WAKEUP` timing on hardware before relying on it.

## Build and test status

The Python host client and host-side protocol tests have been run successfully. `BLE/BleManager.cpp` also passes a C++11 syntax-only check against a local NimBLE 2.5.2 API compatibility stub; this is **not** a substitute for compiling against the real Arduino-ESP32 and NimBLE headers. A PlatformIO/Arduino toolchain is not available in the current build environment, so the complete ESP32-C3 firmware has **not** been compiled and no `.bin` image is included. The bundled XIAO SPI/reset port changes are already present; do not apply the patch helper a second time. Before deployment, run `pio run`, flash two boards, validate BLE messaging in both directions, and test UWB ranging at known reference distances. Physical BLE connectivity, UWB RF performance, antenna-delay calibration, boot-strapping behavior, and sleep/wake timing remain hardware-validation items.

## Raspberry Pi setup

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install pyserial
python pi_client.py --port /dev/serial0 ping
python pi_client.py --port /dev/serial0 status
```

Use `/dev/serial0` only after enabling the Pi UART, disabling its serial login shell, and checking which device `/dev/serial0` maps to on your model. Ensure the UART voltage is 3.3 V and both boards share ground.

## Typical two-node setup

Choose a distinct 16-bit ID for each node, and use the same PAN ID and channel on both. The antenna-delay values below are starting values, not calibrated values.

Responder / anchor, ID 2:

```bash
python pi_client.py --port /dev/serial0 apply-save-config 2 0xDECA 5 16385 16385 --role responder --targets 1 --auto-start
python pi_client.py --port /dev/serial0 start
```

Initiator / tag, ID 1:

```bash
python pi_client.py --port /dev/serial0 apply-save-config 1 0xDECA 5 16385 16385 --role initiator --targets 2 --period-ms 100 --timeout-ms 120 --retries 1 --auto-start
python pi_client.py --port /dev/serial0 time-sync
python pi_client.py --port /dev/serial0 start
```

`SAVE_CONFIG` is retained for compatibility but returns `NOT_SUPPORTED`; use `APPLY_SAVE_CONFIG` for the complete saved configuration snapshot. These examples use starting antenna-delay values, not calibrated values.

To watch results, run `python pi_client.py --port /dev/serial0 subscribe` in a separate process or use the Python `UwbClient` class from an application. For a quick snapshot, use `get-last`; for loss recovery, page through `get-history --after SEQ --n 16` and advance using the response's `next_cursor`. `range-once TARGET_ID` returns a job ID; the result arrives as a subscribed event and is also saved to history.

Run `python3 -m py_compile pi_client.py tests/test_protocol.py tests/test_ble_protocol.py`, `python3 tests/test_protocol.py`, and `python3 tests/test_ble_protocol.py` for host-side syntax, serial framing, and BLE packet/configuration tests. These do not replace a board firmware build, two-board BLE test, or UWB RF calibration.
