# D3KM — DWM3000 Manager

D3KM is a Tauri 2 desktop application built with Vue 3 and TypeScript. It configures and monitors an ESP32-C3/DWM3000 node over a serial connection.

## What it does

- Connects to a node over USB serial and displays connection and device status.
- Configures node identity, UWB ranging settings and Bluetooth transport settings.
- Starts ranging operations and displays measurements and history.
- Configures BLE role and peer policy, sets whether Bluetooth automatically starts after boot, controls the current runtime enabled state, monitors up to eight peer links, and sends peer-directed test messages through the firmware.
- Displays device diagnostics and protocol events.

The Rust backend owns the serial connection and protocol transactions. The Vue frontend configures the BLE network, persists the independent auto-start-after-boot preference, controls the current runtime enabled state, and displays per-peer connection state. BLE discovery, connections, reconnection, and unicast routing are performed autonomously by the node firmware; D3KM does not establish or manage direct BLE links. An empty peer list selects open mode; a non-empty list acts as an allow-list.

## Requirements

- Node.js 20 or newer
- Rust stable toolchain
- Tauri 2 platform prerequisites for your operating system
- A compatible, flashed ESP32-C3/DWM3000 node for hardware integration

## Run in development

From this directory:

```sh
npm install
npm run tauri dev
```

## Build

```sh
npm run build
npm run tauri build
```

For a backend-only compile check:

```sh
cd src-tauri
cargo check
```

## Project structure

- `src/App.vue` — main application views and UI state
- `src/main.ts` — Vue application entry point
- `src/style.css` — global styling
- `src-tauri/src/lib.rs` — Tauri commands, serial lifecycle and application backend
- `src-tauri/src/protocol.rs` — host-protocol framing and message definitions
- `src-tauri/tauri.conf.json` — Tauri application configuration

## Development notes

- Implement UI changes in the Vue frontend; keep serial I/O and device transactions in the Rust backend.
- Keep command IDs, event IDs and payload layouts synchronized with the firmware [protocol reference](../dwm3000-firmware/PROTOCOL.md).
- Treat BLE connection state as per-peer: `GET_STATUS` is only an aggregate summary, while `GET_PEER_STATUS` reports an individual link. Do not reintroduce manual connect/disconnect actions as the normal workflow.
- Test frontend changes with `npm run build` and backend changes with `cargo check`.
- A successful software build does not verify serial interoperability, ranging accuracy or BLE operation. Test those with target hardware and a second compatible node.
