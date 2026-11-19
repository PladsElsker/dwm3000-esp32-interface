# Gazebo D3K node simulator

This project will provide a Gazebo Sim system plugin that emulates one D3K node at the
same host-facing boundary as the ESP32 firmware.

The simulator is intentionally located beside `dwm3000-firmware/` and
`d3kble-api/`: firmware owns the device protocol, and this simulator implements
that protocol for virtual devices. It is not a replacement host API.

## Goals

- Expose a pseudo-terminal (PTY) per simulated node. The existing
  `d3kble.D3KNode` connects to that PTY using its normal serial transport.
- Implement the normative UART protocol in
  [dwm3000-firmware/PROTOCOL.md](../../dwm3000-firmware/PROTOCOL.md), including
  COBS framing, CRC-16/CCITT-FALSE, command responses, subscriptions, event
  sequence numbers, and duplicate-request response caching.
- Simulate BLE node discovery, connection state, identity checks, disconnection,
  fragmentation, acknowledgements, reassembly, message consumption, and final
  delivery results as observable through the existing UART BLE commands/events.
  No host application should need a special simulated-BLE API.
- Simulate UWB distances from Gazebo entity poses and emit the firmware's normal
  RANGE_RESULT / RANGE_FAILED events and history records.
- Initialize each node's saved configuration from SDF plugin parameters. The
  SDF configuration represents NVS contents; runtime state is initialized from
  that saved configuration using the same boot rules as firmware.
- Support multiple independent nodes in one Gazebo world.

## Non-goals

- Emulating the physical BLE radio, NimBLE controller, DWM3000 registers, SPI,
  antenna delay calibration, or RF propagation at the PHY level.
- Changing the `d3kble` host API to add a simulation-specific transport.
- Owning Avion Cargo vehicle models or scenario-specific placement.

## Initial SDF shape

The names below are the proposed simulator configuration interface. The
implementation must validate them and keep their semantics aligned with firmware
NVS keys and `GET_SAVED_CONFIG` / `APPLY_SAVE_CONFIG`.

```xml
<plugin filename="libd3k_gazebo.so" name="d3k::Node">
  <uart>
    <pty_path>/tmp/d3k-plane</pty_path>
    <baud_rate>460800</baud_rate>
  </uart>

  <nvs>
    <uwb>
      <device_id>1</device_id>
      <pan_id>570</pan_id>
      <channel>5</channel>
      <role>initiator</role>
      <targets>2,3,4</targets>
      <period_ms>100</period_ms>
      <timeout_ms>120</timeout_ms>
      <retries>1</retries>
      <auto_start>true</auto_start>
    </uwb>
    <ble>
      <mode>hub</mode>
      <peer_ids>2,3,4</peer_ids>
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

  <uwb>
    <range_noise_stddev_m>0.0</range_noise_stddev_m>
    <range_bias_m>0.0</range_bias_m>
    <max_range_m>100.0</max_range_m>
  </uwb>
</plugin>
```

This is a proposed schema, not an assertion that every child element is already
implemented. The plugin must map it to the documented firmware configuration
and reject invalid combinations at load time.

## Design constraints

### UART

The PTY master is owned by the Gazebo plugin; its slave path is reported in the
Gazebo log and optionally written to `pty_path`. The host opens the slave as a
normal 460800-baud serial endpoint. Serial line coding is not meaningful to the
PTY, but the simulator should still accept the same host byte stream.

The implementation should reuse or port the framing implementation from
`dwm3000-firmware/UwbNode/Protocol.{h,cpp}` where practical. The normative
contract remains `dwm3000-firmware/PROTOCOL.md`; tests must ensure byte-for-byte
compatibility with the Python decoder in `d3kble-api/d3kble/protocol.py`.

### BLE

BLE is simulated at the D3K firmware boundary. Simulated nodes discover/connect
according to each node's saved BLE role, peer allow-list, and timing parameters.
The host continues to use `START_BLE`, `STOP_BLE`, `GET_PEER_STATUS`,
`SEND_TO_PEER`, `GET_RECEIVED_DATA`, and the existing BLE events.

Application message payloads are opaque bytes. The simulator implements the
firmware BLE packet protocol and message-delivery lifecycle, not the
`d3kble.comms` application envelope. That layer remains in the real Python
library and therefore gets exercised unchanged.

### UWB

Each D3K node is attached to a Gazebo model/link/entity. The simulated range is
the Euclidean distance between the configured antenna/reference points. A
ranging job is possible only when roles, PAN/channel, target allow-lists, and
ranging enablement permit it. Scheduled ranging and one-shot jobs should produce
the same status values, sequence numbers, timestamps, history behavior, and
events as the firmware interface.

Noise, bias, maximum range, and deterministic seed are simulator parameters,
not firmware NVS fields. Defaults should make tests deterministic (zero noise,
zero bias).

### NVS

SDF values initialize a persistent-configuration model per plugin instance.
Runtime commands mutate runtime state or saved state according to the protocol:
for example, `SET_ROLE` and `SET_TARGETS` are runtime operations, while
`APPLY_SAVE_CONFIG` persists the complete UWB/BLE snapshot. `SET_PEERS`
persists the BLE peer list per the documented firmware semantics.
`GET_SAVED_CONFIG` must return the SDF-initialized saved snapshot until a
protocol command changes it.

The initial implementation may keep simulated NVS in memory for the lifetime of
the Gazebo server. Document that it does not survive a simulator process restart
unless file-backed persistence is added later.

## Suggested implementation phases

1. Extract/port a protocol-compatible frame codec and build PTY lifecycle.
2. Implement the common, configuration, UWB-control, measurement/history,
   status, and diagnostics commands required by `D3KNode.start()`,
   `wait_until_ready()`, configuration reconciliation, and measurement tests.
3. Implement inter-node UWB ranging and deterministic scheduled/one-shot events.
4. Implement BLE peer discovery/connection and the complete BLE message lifecycle.
5. Add multi-node SDF examples, integration tests using an actual
   `d3kble.D3KNode` over PTY, and protocol conformance tests against the Python
   codec and firmware documentation.

## Validation

The minimum end-to-end acceptance test launches two simulated D3K nodes, opens
their PTYs through the unmodified `d3kble.D3KNode` class, waits for readiness,
reads the SDF-backed saved configuration, enables UWB ranging and BLE, verifies
distance events from Gazebo poses, then sends an application message between
the nodes and observes the normal delivery/result events.

Protocol conformance tests should cover malformed COBS frames, invalid CRC,
unsupported versions, payload bounds, all implemented command payload lengths,
error codes, per-host event subscriptions, duplicate requests, BLE fragment
retransmission/reassembly, and BLE receive-consumption timeout.
