# ESP32-C3 + DWM3000 Firmware and Host Tools

This directory contains the firmware project, the host-side protocol reference, a Python serial client and protocol tests. The firmware sketch and its embedded dependencies live in `UwbNode/`.

## Repository contents

- `UwbNode/` — Arduino/PlatformIO firmware for the Seeed XIAO ESP32-C3 and DWM3000
- `PROTOCOL.md` — host command, event, frame and payload reference
- `pi_client.py` — Python command-line serial client
- `requirements.txt` — Python host-client dependencies
- `tests/` — host-side protocol tests

## Hardware and pin map

The reference target is a Seeed XIAO ESP32-C3 connected to a DWM3000 module.

| Signal | XIAO pin | ESP32-C3 GPIO |
|---|---:|---:|
| DWM3000 CS | D0 | 2 |
| DWM3000 WAKE | D1 | 3 |
| DWM3000 RESET | D2 | 4 |
| DWM3000 IRQ | D3 | 5 |
| UART TX | D6 | 21 |
| UART RX | D7 | 20 |
| SPI SCK | D8 | 8 |
| SPI MISO | D9 | 9 |
| SPI MOSI | D10 | 10 |

Use 3.3 V logic and a common ground. GPIO2, GPIO8 and GPIO9 are ESP32-C3 strapping pins; verify boot, reset and upload behavior with the actual board circuitry. Confirm this pin map against the assembled hardware revision before wiring.

## Build and flash firmware

PlatformIO is the reference build workflow. Run these commands from `UwbNode/`:

```sh
python3 -m pip install platformio
pio run
pio run -t upload
```

The firmware targets the Seeed XIAO ESP32-C3. The project configures the board, build flags and libraries in its PlatformIO configuration. The serial host interface uses UART1 at 460800 baud, 8N1; USB CDC uses the same binary protocol.

The firmware provides UWB DS-TWR ranging and BLE messaging through NimBLE. BLE is managed by the firmware; host software controls it through the serial protocol. Review the source and [PROTOCOL.md](PROTOCOL.md) for current command and behavior details.

## Use the Python client

From this directory:

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
python pi_client.py --help
python pi_client.py --port /dev/serial0 ping
```

On Windows, activate with `.venv\\Scripts\\activate`. Replace the example serial port with the port used by your host.

## Run host-side tests

From this directory:

```sh
python -m py_compile pi_client.py tests/test_protocol.py tests/test_ble_protocol.py
python tests/test_protocol.py
python tests/test_ble_protocol.py
```

These tests exercise host-side protocol code; they do not replace a firmware build or tests on physical hardware.

## Protocol development

Read [PROTOCOL.md](PROTOCOL.md) before implementing or changing a host. Keep command IDs, event IDs, field widths, byte order, framing and delivery semantics synchronized across the firmware, D3KM and Python client. Update the protocol document in the same change as any wire-format change.

For system-level orientation, see the [repository README](../README.md).
