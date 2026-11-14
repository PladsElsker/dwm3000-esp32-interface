# D3KM — DWM3000 Manager

Desktop application built with **Tauri 2 + Vue 3 Composition API + TypeScript**. It connects to the ESP32-C3 USB CDC endpoint and speaks UWB Node UART Protocol v1 over the USB byte stream.

## Features

- Enumerate serial/USB ports, connect at 460800 line coding, disconnect and reconnect.
- Protocol handshake (`PING`) and device identity/status/configuration discovery.
- Device health overview: radio readiness, role, uptime, range counters, clock state, firmware/protocol details.
- Configuration editor for device ID, PAN ID, channel 5/9, TX/RX antenna delays, period, timeout, retries and up to 16 targets.
- Apply volatile settings and separately save effective config/targets to ESP32 NVS.
- Role selection (idle, initiator/sender, responder/receiver), continuous ranging start/stop and one-shot ranging.
- Subscribe/unsubscribe to all six event types, live range results/failures, latest-per-target and paginated history retrieval.
- Diagnostics counters, radio status registers, last error, clock sync, antenna delay readback and job cancellation.
- Radio IDLE_RC standby/wake, ESP32 timer deep sleep, reboot and reset-to-compiled-defaults actions.

## Run locally

Prerequisites: Node.js 20+, Rust stable, and the Tauri v2 platform prerequisites for your operating system. See https://v2.tauri.app/start/prerequisites/.

```sh
npm install
npm run tauri dev
```

Build a desktop bundle:

```sh
npm run tauri build
```

## USB / protocol implementation

The Rust backend owns the serial port and performs the protocol framing:

- Protocol version 1; request/response/event frame types.
- Little-endian fields; COBS encoding and `0x00` delimiter.
- CRC-16/CCITT-FALSE (initial value `0xFFFF`, polynomial `0x1021`).
- Sequence/opcode response correlation; events are emitted separately and can interleave with responses.
- 1.8 second request timeout; serial read timeout is 25 ms.

USB CDC is a native USB data link. The firmware sets the USB CDC line-coding value to 460800 for compatibility; unlike UART1, USB CDC is not physically clocked at that baud. The app requests that line-coding value when opening the port.

## Configuration behavior

`SET_CONFIG`, `SET_ANTENNA_DELAYS`, `SET_SCHEDULE` and `SET_TARGETS` apply values in RAM. `SAVE_CONFIG` persists the effective configuration and target list. Role defaults to IDLE and ranging is stopped after boot. Role changes stop ranging. Initiator mode needs a target list for continuous ranging; responder mode interprets a nonempty target list as an allow-list, while an empty list accepts any compatible peer.

Radio standby is reported as `IDLE_RC`. The firmware's current driver integration does not expose true DWM3000 deep-sleep functions. ESP32 `SYSTEM_SLEEP` is a separate timer-woken deep-sleep operation; UART cannot wake the ESP32.

## Validation notes / known limitations

- The project files are scaffolded but were **not compiled or tested against physical hardware** in this environment. Install dependencies and run the build commands above on the target development machine.
- This implementation uses a single serial request/response reader. It consumes event frames interleaved while waiting for command responses; events arriving between requests are picked up by the next periodic status request. This avoids a second reader racing the response parser.
- USB disconnect/reconnect and operating-system serial-port permissions depend on the host OS and device driver.
- A real ranging test requires a second compatible UWB node configured with matching PAN/channel and the appropriate peer ID/role.
- Antenna delays materially affect absolute ranging accuracy; the app exposes the configured values but does not perform automatic calibration.
- Quality flags are displayed as raw values. The current firmware integration supplies zero and does not provide a validated quality estimate.

## Project layout

```text
src/                 Vue UI, Composition API, styles
src-tauri/src/       Tauri commands, USB serial transport and protocol framing
src-tauri/           Tauri v2 configuration and Rust manifest
```
