# Gazebo D3K node simulator

A Gazebo Sim system plugin that emulates a D3K node at the same host-facing
boundary as the ESP32 firmware. It is maintained alongside the firmware and
host API because the simulator implements the device's UART and BLE behavior.

The plugin targets **Gazebo Harmonic / Gazebo Sim 8** and Linux PTYs. It is not a
BLE radio or DWM3000 RF/PHY simulator.

## What it emulates

Each plugin instance is one virtual D3K device:

- A Linux pseudo-terminal (PTY) that the unmodified `d3kble.D3KNode` opens as a
  serial device. The plugin uses the same binary COBS frame codec and CRC-16 /
  CCITT-FALSE implementation as the firmware (`dwm3000-firmware/UwbNode/Protocol.*`).
- The host commands and events used by `D3KNode` for startup, state polling,
  configuration, UWB ranging, BLE operations and message transfer. It supports
  the command families in `dwm3000-firmware/PROTOCOL.md`; `SYSTEM_SLEEP` is
  explicitly unsupported because a Gazebo model cannot deep-sleep an ESP32.
- Simulated UWB ranging between registered D3K plugin instances. Ranges are
  computed from their configured reference-link world positions. Initiator /
  responder roles, PAN ID, channel, target allow-lists, periodic scheduling,
  one-shot jobs, result/failure events and measurement history are modelled.
- Simulated BLE discovery and connection between HUB/Central and PERIPHERAL
  nodes, including peer allow-lists and peer state events. Application payloads
  remain opaque bytes. The plugin models the D3K custom GATT message lifecycle:
  DATA fragmentation, per-fragment ACK validation, reassembly, host consumption,
  MESSAGE_RESULT acknowledgement, and expiry. Receive deadlines and result
  retries use wall-clock time like firmware `millis()`, rather than Gazebo
  simulation time. Queue-full / busy NACKs follow the configured retry budget,
  and expiry produces the same receiver and sender events as firmware. It uses
  the existing UART BLE commands/events; the Python `d3kble.comms` layer runs
  unchanged.
- Per-instance saved configuration initialized from SDF. This is the simulated
  NVS snapshot returned by `GET_SAVED_CONFIG`. Firmware commands that persist
  configuration update the in-memory snapshot for the lifetime of the Gazebo
  server process.

The BLE link is simulated in-process. It does **not** start NimBLE, create real
GATT connections, use Bluetooth hardware, or test RF coexistence. Fragment
transfers are lossless in this first version; the protocol packets and CRCs are
modelled, but packet-loss/retry fault injection is not yet exposed. Fragment
pacing, unread-message deadlines, and message-result retries use wall-clock
runtime, independently of the Gazebo update rate and world pause state.

## Build

Install the Gazebo Harmonic development packages and a C++17 toolchain. On
Ubuntu with Gazebo Harmonic already configured, the relevant packages are
`libgz-sim8-dev` and `libgz-plugin2-dev`.

From the repository root:

```bash
cmake -S simulation/gazebo -B build/d3k-gazebo -DCMAKE_BUILD_TYPE=Release
cmake --build build/d3k-gazebo --parallel
ctest --test-dir build/d3k-gazebo --output-on-failure
```

The build compiles the plugin and the firmware's shared UART frame codec. The
codec test checks a documented PING wire vector and CRC rejection. The BLE
protocol-model test covers NACK retry budgets, wall-clock receive expiry,
MESSAGE_RESULT / acknowledgement framing, and sender/receiver event payloads.
Gazebo end-to-end integration still needs to be run on a host with Gazebo
Harmonic installed.

## Run the example

Make the plugin discoverable and launch the example world:

```bash
export GZ_SIM_SYSTEM_PLUGIN_PATH="$PWD/build/d3k-gazebo${GZ_SIM_SYSTEM_PLUGIN_PATH:+:$GZ_SIM_SYSTEM_PLUGIN_PATH}"
gz sim -r simulation/gazebo/examples/two_node_world.sdf
```

The two plugin instances create these symlinks:

```text
/tmp/d3k-plane  -> /dev/pts/<n>
/tmp/d3k-payload -> /dev/pts/<n>
```

Use those paths as the `uart_device` values in the existing VTOL and payload
configuration JSON files. No simulation-specific Python adapter is needed.

The plugin logs the PTY path and its fixed per-node range bias at startup. The
PTY is removed when the plugin is unloaded. Set a unique `uart.pty_path` for
each simulated D3K model.

## SDF configuration

Attach one `d3k_sim::D3kNodeSystem` plugin to each model representing a D3K
antenna. `reference_link` selects the link whose world position is used for
ranging; otherwise the model origin is used.

The `<nvs>` block is the virtual device's saved NVS configuration. UWB settings
live under `<nvs><uwb>` and BLE settings under `<nvs><ble>`. The tag names and
ranges follow `GET_SAVED_CONFIG` / `APPLY_SAVE_CONFIG` in the firmware protocol.

```xml
<plugin filename="libd3k_gazebo_system.so" name="d3k_sim::D3kNodeSystem">
  <uart>
    <pty_path>/tmp/d3k-plane</pty_path>
    <baud_rate>460800</baud_rate>
  </uart>
  <reference_link>uwb_antenna</reference_link>

  <nvs>
    <uwb>
      <device_id>1</device_id>
      <pan_id>57034</pan_id>
      <channel>5</channel>
      <tx_antenna_delay>16385</tx_antenna_delay>
      <rx_antenna_delay>16385</rx_antenna_delay>
      <role>initiator</role>
      <targets>2</targets>
      <period_ms>100</period_ms>
      <timeout_ms>120</timeout_ms>
      <retries>1</retries>
      <auto_start>true</auto_start>
    </uwb>
    <ble>
      <mode>hub</mode>
      <peer_ids>2</peer_ids>
      <auto_start>true</auto_start>
      <advertising_interval_ms>100</advertising_interval_ms>
      <scan_interval_ms>100</scan_interval_ms>
      <scan_window_ms>50</scan_window_ms>
      <retry_interval_ms>1000</retry_interval_ms>
      <connection_timeout_ms>5000</connection_timeout_ms>
      <max_retries>3</max_retries>
      <rx_message_timeout_ms>1000</rx_message_timeout_ms>
    </ble>
  </nvs>

  <range_model>
    <noise_stddev_m>0.015</noise_stddev_m>
    <node_bias_limit_m>0.03</node_bias_limit_m>
    <max_range_m>100.0</max_range_m>
    <seed>0</seed>
  </range_model>
</plugin>
```

`seed=0` derives a stable seed from `device_id`. Each node receives a persistent
per-instance bias uniformly drawn from `[-node_bias_limit_m, +node_bias_limit_m]`.
An explicit `<bias_m>` overrides the generated value. A range uses the sum of
the two endpoint biases plus independent zero-mean Gaussian noise with standard
deviation `<noise_stddev_m>`.

### Ranging error assumptions

The default is intentionally a simple calibrated line-of-sight error model,
not a radio propagation simulation or a vendor accuracy guarantee. Each
simulated node receives a fixed residual calibration bias bounded to ±3 cm;
the pair's combined fixed bias is therefore bounded to ±6 cm. Each measurement
also receives independent zero-mean Gaussian noise with a 1.5 cm standard
deviation. These are pragmatic starting assumptions for integration testing of
a correctly calibrated setup. They should be tuned against measurements from
the actual board, antenna, mounting and environment before using simulation
results as a quantitative hardware-performance prediction. A fixed SDF bias
and seed make tests repeatable. There is no NLOS/multipath, antenna orientation,
temperature drift, frequency-offset or signal-strength model yet.

The `<range_model>` block is simulator-only configuration, not NVS. The UWB and
BLE values inside `<nvs>` are the emulated saved configuration.

## Current boundaries and limitations

- One PTY / UART host transport per plugin instance. USB CDC is not simulated.
- BLE links are virtual connections within one Gazebo process. The plugin does
  not emulate BLE radio scanning timing, GATT discovery latency, real link loss,
  interference, or physical ATT MTU negotiation.
- Virtual BLE messages are fragmented to the configured protocol's MTU payload
  size and pass through the firmware-style DATA / ACK / MESSAGE_RESULT lifecycle.
  Fragment scheduling and protocol deadlines use elapsed runtime, not Gazebo
  simulation time, so BLE continues while physics is paused. NACK retry budgets,
  unread-message expiry, and sender/receiver result events follow firmware
  behavior. The virtual radio assumes successful packet delivery; RF loss,
  interference, and packet-loss fault injection are future work.
- UWB uses direct Euclidean distance. Occlusion and multipath are not inferred
  from Gazebo collision geometry.
- Simulated NVS writes are retained only while the Gazebo server process stays
  alive. Restarting Gazebo reloads the values from SDF.
- PTYs require Linux/POSIX `openpty` and the `libutil` development library.

## Protocol ownership

`dwm3000-firmware/PROTOCOL.md` remains the normative UART/BLE protocol contract.
The plugin compiles `dwm3000-firmware/UwbNode/Protocol.cpp` directly so COBS and
CRC framing are not copied into a second implementation. Update the protocol
specification, firmware, Python API and simulator together when wire behavior
changes.
