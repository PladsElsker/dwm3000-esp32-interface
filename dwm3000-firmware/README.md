# ESP32-C3 + DWM3000 Firmware and Host Tools

This directory contains the firmware project, its development instructions, the host API specification, a Python serial client, and protocol tests. The firmware sketch and its embedded dependencies live in `UwbNode/`.

The firmware README describes how to build, flash, wire, and test the firmware. `PROTOCOL.md` is the API contract for host applications; it is the single reference for command opcodes, payload layouts, responses, events, and BLE messaging.

## Repository contents

- `UwbNode/` — Arduino CLI firmware for the Seeed XIAO ESP32-C3 and DWM3000
- `PROTOCOL.md` — normative host API specification with per-command request/response schemas, examples, events, and BLE wire formats
- `pi_client.py` — Python host client
- `requirements.txt` — Python host-client dependencies
- `tests/` — host-side protocol tests
- `FIRMWARE_VERSION` — semantic firmware release version, independent of the wire protocol version
- `flash-windows.ps1` — Windows helper for uploading a CI-precompiled build using Arduino CLI

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

## Build and flash firmware locally

Run the setup script from this directory to create the custom Arduino-ESP32 core, build its ESP32-C3 libraries, verify the selected Arduino CLI platform, and compile the firmware:

```sh
./setup-custom-arduino-core.sh
```

The script pins the library builder to commit `165a3d6`, uses the Arduino-ESP32 `release/v3.3.x` branch and ESP-IDF `release/v5.5` branch, and builds with:

```ini
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=8
CONFIG_BT_CTRL_BLE_MAX_ACT=9
```

It installs the custom core in Arduino CLI's manual platform directory (`directories.user/hardware/espressif/esp32`), creates an isolated Python environment for the builder, installs the Arduino platform tools, installs the ESP-IDF tools and ESP32-C3 Python environment using the system Python, activates `idf.py`, aligns the core checkout to the exact Arduino-ESP32 source revision used by the builder, and initializes the bundled DWM3000 driver submodule before the long build. It does not change a Boards Manager installation. The local platform is selected with the `espressif:esp32` FQBN, distinct from the Boards Manager package's `esp32:esp32` FQBN.

Requirements: Arduino CLI, Git, Python 3.9 or newer, and the native build prerequisites listed by the script for Ubuntu/Debian or macOS. The script installs the matching ESP-IDF tools and Python environment for the ESP32-C3 itself, then activates that environment before running the library build. Use `--core-only` to build and install the custom core without compiling the sketch.

The default core and builder directories can be overridden with `ARDUINO_SKETCHBOOK_DIR`, `ESP32_ARDUINO_CORE_DIR`, and `ESP32_ARDUINO_LIB_BUILDER_DIR`.

After the custom core has been built, subsequent firmware-only builds can use Arduino CLI directly without BLE compiler flag overrides:

```sh
arduino-cli compile --fqbn espressif:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin \
  --libraries "./UwbNode/lib/Makerfabs-ESP32-UWB-DW3000" ./UwbNode
```

Upload the compiled sketch:

```sh
arduino-cli upload -p /dev/ttyACM0 --fqbn espressif:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin ./UwbNode
```

The serial host interface uses UART1 at 460800 baud, 8N1; USB CDC uses the same binary protocol. The bundled Makerfabs port contains the board-specific SPI/reset changes for the XIAO pin map.

## CI build artifacts and firmware releases

The independent firmware release version is maintained in `FIRMWARE_VERSION` using `MAJOR.MINOR.PATCH` semantic versioning. It is not the protocol version defined in `UwbNode/Protocol.h`. Update this file when preparing a firmware release.

A push to `main` that changes firmware sources runs the custom eight-peer ESP32-C3 build and publishes the `dwm3000-firmware-esp32c3` Actions artifact. When `FIRMWARE_VERSION` changes, the workflow also creates a Git tag named `firmware-v<VERSION>` and a GitHub Release containing a ZIP of the complete artifact. Reusing an existing firmware version is rejected; bump the version file before the next release.

On Windows, install Arduino CLI and the standard Espressif ESP32 board package (for the board definition and Windows upload tools; it is not used to compile this firmware):

```powershell
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

Download and extract the latest successful `dwm3000-firmware-esp32c3` artifact from the repository's **Actions** tab. Run the included helper, replacing `COM5` with the node's serial port:

```powershell
powershell -ExecutionPolicy Bypass -File .\flash-windows.ps1 -Port COM5
```

The helper invokes `arduino-cli upload` with the precompiled `build` directory. It does not compile on Windows, and does not require the custom Arduino CLI executable or a Windows port of the eight-peer build patch. The eight-peer NimBLE settings are already compiled into the firmware. The helper accepts `-Fqbn` to override the default Windows Espressif FQBN and `-Baud` to change upload speed.

Actions artifacts are retained for 90 days. Firmware releases are retained as GitHub Releases and are not subject to that artifact retention period.

## BLE peer messaging

BLE uses NimBLE-Arduino 2.5.0 and runs independently of UWB ranging.

- `IDLE` leaves BLE stopped; `HUB` operates as a Central; `PERIPHERAL` advertises as a Peripheral.
- Configuration stores the BLE role, peer policy, timing parameters, and the `Automatically start Bluetooth after boot` preference in NVS. At boot, the node starts the saved HUB or PERIPHERAL role when auto-start is enabled.
- `START_BLE` and `STOP_BLE` start and stop BLE for the current runtime session. The boot auto-start preference is configured and saved separately.
- While enabled, HUB nodes scan for peers and run one Central connection attempt at a time; scanning pauses through connection and GATT identity validation. Failed attempts enter backoff and are retried automatically. PERIPHERAL nodes advertise and accept permitted peers. The node manages scanning, advertising, connections, and reconnection autonomously; hosts read per-peer status and detailed connection failure events.
- Up to eight concurrent peer links are maintained. The NimBLE host is configured for eight connections; the Bluetooth controller activity pool is configured for nine activities.
- An empty peer list is open mode and accepts any valid peer. A non-empty list is an allow-list.
- Application messages are unicast. `send-to-peer PEER_ID TEXT` routes to the specified connected peer; the node does not broadcast application payloads.
- Messages are limited to 496 bytes and fragmented according to the negotiated ATT MTU. `BLE_DATA_SENT` indicates that the remote host retrieved the complete message.

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
