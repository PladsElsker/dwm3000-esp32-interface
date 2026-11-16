# ESP32-C3 + DWM3000 Firmware and Host Tools

This directory contains the firmware project, its development instructions, the host API specification, a Python serial client, and protocol tests. The firmware sketch and its embedded dependencies live in `UwbNode/`.

The firmware README describes how to build, flash, wire, and test the firmware. `PROTOCOL.md` is the API contract for host applications; it is the single reference for command opcodes, payload layouts, responses, events, and BLE messaging.

## Repository contents

- `UwbNode/` — Arduino CLI firmware for the Seeed XIAO ESP32-C3 and DWM3000
- `PROTOCOL.md` — normative host API specification with per-command request/response schemas, examples, events, and BLE wire formats
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

Build with Arduino CLI and the Seeed XIAO ESP32-C3 board definition. The firmware uses NimBLE-Arduino 2.5.0 and supports up to eight concurrent BLE peer links.

The BLE host/controller limits are part of the compiled ESP32 Arduino core libraries, not sketch compiler flags. This project uses the custom ESP32-C3 core libraries built by [esp32-arduino-lib-builder](https://github.com/PladsElsker/esp32-arduino-lib-builder), which sets:

```ini
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=8
CONFIG_BT_CTRL_BLE_MAX_ACT=9
```

Build and install the customized core once. The builder copies its generated libraries and configuration into a manually installed Arduino-ESP32 source tree; it does not modify a Boards Manager installation.

```sh
git clone https://github.com/PladsElsker/esp32-arduino-lib-builder
cd esp32-arduino-lib-builder
git checkout master
./build.sh -f -t esp32c3 -c "$HOME/Arduino/hardware/espressif/esp32"
```

The target directory must be a manual Arduino-ESP32 installation. The builder selects its Arduino-ESP32 and ESP-IDF source revisions; keep the generated core and the toolchain versions together, and do not replace its `tools/esp32-arduino-libs` directory with files from a Boards Manager package.

After the core has been built and installed, compile this sketch using that custom core's platform package. Arduino CLI must resolve the `esp32:esp32:esp32c3` platform to the manually installed core, not to a separate Boards Manager copy. For example, when the custom core is installed as a hardware package in your sketchbook:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin \
  --libraries "./UwbNode/lib/Makerfabs-ESP32-UWB-DW3000" ./UwbNode
```

Do not add `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` through `compiler.c.extra_flags` or `compiler.cpp.extra_flags`: those flags do not rebuild the precompiled NimBLE host or Bluetooth controller libraries.

To verify the selected core, inspect the installed platform's `tools/esp32-arduino-libs/esp32c3/sdkconfig` and the matching memory-variant header under `tools/esp32-arduino-libs/esp32c3/<memory_type>/include/sdkconfig.h`. They must contain `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=8` and `CONFIG_BT_CTRL_BLE_MAX_ACT=9`. The core's `platform.txt` selects this SDK directory using the board's chip variant and memory type.

Upload the compiled sketch:

```sh
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin ./UwbNode
```

The serial host interface uses UART1 at 460800 baud, 8N1; USB CDC uses the same binary protocol. The bundled Makerfabs port contains the board-specific SPI/reset changes for the XIAO pin map.

## BLE peer messaging

BLE uses NimBLE-Arduino 2.5.0 and runs independently of UWB ranging.

- `IDLE` leaves BLE stopped; `HUB` operates as a Central; `PERIPHERAL` advertises as a Peripheral.
- Configuration stores the BLE role, peer policy, timing parameters, and the `Automatically start Bluetooth after boot` preference in NVS. At boot, the node starts the saved HUB or PERIPHERAL role when auto-start is enabled.
- `START_BLE` and `STOP_BLE` start and stop BLE for the current runtime session. The boot auto-start preference is configured and saved separately.
- While enabled, HUB nodes scan for peers and run one Central connection attempt at a time; scanning pauses through connection and GATT identity validation. Failed attempts enter backoff and are retried automatically. PERIPHERAL nodes advertise and accept permitted peers. The node manages scanning, advertising, connections, and reconnection autonomously; hosts read per-peer status and detailed connection failure events.
- Up to eight concurrent peer links are maintained. The NimBLE host build uses nine connection slots.
- An empty peer list is open mode and accepts any valid peer. A non-empty list is an allow-list.
- Application messages are unicast. \`send-to-peer PEER_ID TEXT\` routes to the specified connected peer; the node does not broadcast application payloads.
- Messages are limited to 496 bytes and fragmented according to the negotiated ATT MTU. \`BLE_DATA_SENT\` indicates that the remote host retrieved the complete message.

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

Read [PROTOCOL.md](PROTOCOL.md) when implementing or changing a host. Keep command IDs, event IDs, field widths, byte order, framing and delivery semantics synchronized across the firmware, D3KM and Python client. Update this API specification in the same change as any wire-format change.

For system-level orientation, see the [repository README](../README.md).
