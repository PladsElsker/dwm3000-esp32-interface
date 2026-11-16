# DWM3000 ESP32 Interface

A project for UWB ranging and BLE messaging using an ESP32-C3 and DWM3000.

## Projects

| Project | Purpose | Start here |
|---|---|---|
| [Firmware and host tools](dwm3000-firmware/README.md) | ESP32-C3 firmware, host protocol, Python client and protocol tests | Firmware README |
| [D3KM](d3km/README.md) | Tauri 2 desktop application for configuring and monitoring a node | D3KM README |
| [Hardware](dwm3000-board/README.md) | KiCad schematic, PCB and fabrication outputs | Hardware README |
| [d3kble Python API](d3kble-api/README.md) | Python API for UART access, UWB measurements, BLE transport, messaging and device configuration | d3kble API README |

## System architecture

The ESP32-C3 runs the UWB node firmware and controls the DWM3000 radio. Host applications communicate with the node over USB CDC or UART1 using the same binary protocol. D3KM is one host; the Python client is another.

BLE is handled by the node firmware. D3KM sends BLE operations over the serial host protocol; it does not connect directly to the node over BLE.

The protocol reference is [dwm3000-firmware/PROTOCOL.md](dwm3000-firmware/PROTOCOL.md). Keep host implementations synchronized with it when changing firmware commands, events or payloads.

## Repository layout

- `dwm3000-firmware/` — firmware project documentation, host protocol, Python client and tests
- `dwm3000-firmware/UwbNode/` — ESP32-C3 Arduino/PlatformIO firmware sketch and its embedded dependencies
- `d3km/` — Vue frontend and Tauri/Rust desktop backend
- `d3kble-api/` — Python host API for ESP32-C3 / DWM3000 nodes
- `dwm3000-board/` — KiCad design and manufacturing files

## Development workflow

1. Review the hardware README and firmware pin map before wiring a board.
2. Build and flash the firmware to compatible nodes.
3. Connect a node through D3KM or the Python client.
4. Configure distinct node IDs and compatible UWB/BLE peer settings.
5. Validate serial communication, UWB ranging and BLE messaging independently.

See each project README for its own setup and validation commands. Software tests do not establish hardware or RF performance.
