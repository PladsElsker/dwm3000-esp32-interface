"""Desired-state diffing and volatile-only runtime reconciliation."""
from __future__ import annotations

import struct
import threading
import time
from dataclasses import dataclass

from .exceptions import DeviceCommandError, ReconciliationError
from .protocol import OPS, encode_set_config, encode_targets


# The executor deliberately cannot issue any NVS-writing opcode.
VOLATILE_RECONCILIATION_OPCODES = frozenset({
    OPS["set_config"], OPS["set_antenna_delays"], OPS["set_role"],
    OPS["set_targets"], OPS["set_schedule"], OPS["start_ranging"],
    OPS["stop_ranging"], OPS["start_ble"], OPS["stop_ble"],
})


@dataclass(frozen=True, slots=True)
class ConfigChange:
    path: str
    current: object
    desired: object


@dataclass(frozen=True, slots=True)
class ReconciliationAction:
    name: str
    opcode: int
    payload: bytes = b""
    disruptive: bool = False
    description: str = ""


@dataclass(frozen=True, slots=True)
class ReconciliationPlan:
    actions: tuple[ReconciliationAction, ...]
    changes: tuple[ConfigChange, ...]

    @property
    def nvs_writes(self) -> int:
        return 0

    @property
    def requires_ranging_stop(self) -> bool:
        return any(a.name == "stop_ranging" for a in self.actions)

    @property
    def requires_ble_stop(self) -> bool:
        return any(a.name == "stop_ble" for a in self.actions)


class VolatileCommandExecutor:
    def __init__(self, transport, state):
        self.transport = transport
        self.state = state

    def execute(self, action: ReconciliationAction):
        if action.opcode not in VOLATILE_RECONCILIATION_OPCODES:
            raise ReconciliationError(
                f"opcode 0x{action.opcode:02x} is not permitted for reconciliation"
            )
        self.transport.request(action.opcode, action.payload, retryable=False)


class ConfigManager:
    CONFIG_FIELDS = (
        "device_id", "pan_id", "channel", "tx_antenna_delay", "rx_antenna_delay",
        "ble_mode", "ble_advertising_interval_ms", "ble_scan_interval_ms",
        "ble_scan_window_ms", "ble_auto_start", "ble_retry_interval_ms",
        "ble_connection_timeout_ms", "ble_max_retries",
        "ble_rx_message_timeout_ms", "ble_peers",
    )

    def __init__(self, transport, state_manager, dispatcher):
        self.transport = transport
        self.state_manager = state_manager
        self.dispatcher = dispatcher
        self.executor = VolatileCommandExecutor(transport, state_manager)
        self._source = "explicit"
        self._desired: dict | None = None
        self._mode = "report_only"
        self._lock = threading.Lock()
        self._reconcile_lock = threading.Lock()
        self.last_plan: ReconciliationPlan | None = None
        self.last_result: dict | None = None

    @property
    def reconciliation_enabled(self) -> bool:
        return self._mode == "automatic"

    def set_source(self, source: str):
        source = source.lower()
        if source not in {"nvs", "explicit"}:
            raise ValueError("desired configuration source must be 'nvs' or 'explicit'")
        self._source = source

    def set_desired(self, config: dict):
        if not isinstance(config, dict):
            raise TypeError("desired configuration must be a dictionary")
        self._desired = dict(config)
        self._source = "explicit"

    def set_mode(self, mode: str):
        mode = mode.lower()
        if mode not in {"report_only", "manual", "automatic"}:
            raise ValueError("mode must be report_only, manual, or automatic")
        self._mode = mode

    def set_reconciliation_enabled(self, enabled: bool):
        self._mode = "automatic" if enabled else "report_only"

    def _resolve_desired(self) -> dict:
        if self._source == "explicit":
            if self._desired is None:
                raise ReconciliationError("no explicit desired configuration has been supplied")
            return dict(self._desired)
        saved = self.state_manager.snapshot().saved
        if not saved.valid:
            raise ReconciliationError("saved NVS configuration is not currently valid")
        # NVS stores configuration and boot preferences, not the desired live
        # service state. Unless the application explicitly supplies service
        # intent, reconciliation preserves the currently observed running state.
        return dict(saved.values)

    def get_diff(self) -> tuple[ConfigChange, ...]:
        desired = self._resolve_desired()
        snapshot = self.state_manager.snapshot()
        if not snapshot.runtime.valid:
            raise ReconciliationError("runtime configuration is unknown or stale")
        current = dict(snapshot.runtime.values)
        current.update(snapshot.status)
        paths = set(self.CONFIG_FIELDS) | {
            "role", "targets", "period_ms", "timeout_ms", "retries",
            "ranging_enabled", "ble_enabled",
        }
        changes = []
        for key in sorted(paths):
            if key not in desired:
                continue
            actual = current.get(key)
            expected = desired[key]
            if isinstance(actual, (list, tuple)) and isinstance(expected, (list, tuple)):
                actual, expected = tuple(actual), tuple(expected)
            if actual != expected:
                changes.append(ConfigChange(key, actual, expected))
        return tuple(changes)

    def get_plan(self) -> ReconciliationPlan:
        desired = self._resolve_desired()
        snapshot = self.state_manager.snapshot()
        if not snapshot.runtime.valid or not snapshot.status_valid:
            raise ReconciliationError("cannot plan from an incomplete or stale device snapshot")
        current = dict(snapshot.runtime.values)
        current.update(snapshot.status)
        changes = self.get_diff()
        changed = {c.path for c in changes}
        actions: list[ReconciliationAction] = []

        core_or_ble = bool(changed & set(self.CONFIG_FIELDS))
        ranging_change = bool(changed & {
            "role", "targets", "period_ms", "timeout_ms", "retries",
            "device_id", "pan_id", "channel", "tx_antenna_delay", "rx_antenna_delay",
        })
        ble_change = bool(changed & {
            "device_id", "ble_mode", "ble_advertising_interval_ms",
            "ble_scan_interval_ms", "ble_scan_window_ms", "ble_auto_start",
            "ble_retry_interval_ms", "ble_connection_timeout_ms",
            "ble_max_retries", "ble_rx_message_timeout_ms", "ble_peers",
        })
        set_config_change = bool(changed & {
            "device_id", "pan_id", "channel", "ble_mode",
            "ble_advertising_interval_ms", "ble_scan_interval_ms",
            "ble_scan_window_ms", "ble_auto_start", "ble_retry_interval_ms",
            "ble_connection_timeout_ms", "ble_max_retries",
            "ble_rx_message_timeout_ms", "ble_peers",
        })

        was_ranging = bool(snapshot.status.get("ranging_enabled"))
        was_ble_enabled = bool(snapshot.status.get("ble_enabled"))
        if (ranging_change or set_config_change) and was_ranging:
            actions.append(ReconciliationAction(
                "stop_ranging", OPS["stop_ranging"], disruptive=True,
                description="Stop ranging before changing UWB configuration",
            ))
        if (ble_change or set_config_change) and was_ble_enabled:
            actions.append(ReconciliationAction(
                "stop_ble", OPS["stop_ble"], disruptive=True,
                description="Disconnect BLE peers before changing BLE configuration",
            ))

        if set_config_change:
            merged = dict(current)
            merged.update(desired)
            try:
                payload = encode_set_config(merged)
            except (KeyError, ValueError) as exc:
                raise ReconciliationError(f"cannot construct SET_CONFIG payload: {exc}") from exc
            actions.append(ReconciliationAction(
                "set_config", OPS["set_config"], payload, True,
                "Apply volatile radio and BLE configuration",
            ))
        elif changed & {"tx_antenna_delay", "rx_antenna_delay"}:
            merged = dict(current)
            merged.update(desired)
            payload = struct.pack(
                "<HH", int(merged["tx_antenna_delay"]), int(merged["rx_antenna_delay"])
            )
            actions.append(ReconciliationAction(
                "set_antenna_delays", OPS["set_antenna_delays"], payload, True,
                "Apply volatile antenna delays",
            ))

        if "role" in changed:
            role = {"idle": 0, "initiator": 1, "responder": 2}.get(desired["role"])
            if role is None:
                raise ReconciliationError("desired role must be idle, initiator, or responder")
            actions.append(ReconciliationAction(
                "set_role", OPS["set_role"], bytes([role]), True, "Set UWB role"
            ))
        if "targets" in changed:
            actions.append(ReconciliationAction(
                "set_targets", OPS["set_targets"], encode_targets(desired["targets"]),
                True, "Replace runtime UWB target list",
            ))
        if changed & {"period_ms", "timeout_ms", "retries"}:
            merged = dict(current)
            merged.update(desired)
            period = int(merged["period_ms"])
            timeout = int(merged["timeout_ms"])
            retries = int(merged["retries"])
            if not 20 <= period <= 60000 or not 50 <= timeout <= 5000 or not 0 <= retries <= 5:
                raise ReconciliationError("desired UWB schedule is outside firmware limits")
            actions.append(ReconciliationAction(
                "set_schedule", OPS["set_schedule"], struct.pack("<IHB", period, timeout, retries),
                True, "Update runtime UWB schedule",
            ))

        want_ble = bool(desired.get("ble_enabled", was_ble_enabled))
        want_ranging = bool(desired.get("ranging_enabled", was_ranging))
        if want_ble and str(desired.get("ble_mode", current.get("ble_mode", "idle"))).lower() == "idle":
            raise ReconciliationError("BLE cannot be started while desired BLE mode is IDLE")
        desired_role = str(desired.get("role", current.get("role", "idle"))).lower()
        if want_ranging and desired_role == "idle":
            raise ReconciliationError("UWB ranging cannot be enabled with an idle role")
        if want_ranging and desired_role == "initiator" and not desired.get("targets", current.get("targets", ())):
            raise ReconciliationError("initiator ranging requires at least one target")
        if want_ble and (ble_change or set_config_change or not was_ble_enabled):
            actions.append(ReconciliationAction(
                "start_ble", OPS["start_ble"], disruptive=True, description="Start BLE"
            ))
        elif not want_ble and was_ble_enabled and not any(a.name == "stop_ble" for a in actions):
            actions.append(ReconciliationAction("stop_ble", OPS["stop_ble"], disruptive=True))
        if want_ranging and (ranging_change or set_config_change or not was_ranging):
            actions.append(ReconciliationAction(
                "start_ranging", OPS["start_ranging"], disruptive=True,
                description="Start UWB ranging",
            ))
        elif not want_ranging and was_ranging and not any(a.name == "stop_ranging" for a in actions):
            actions.append(ReconciliationAction("stop_ranging", OPS["stop_ranging"], disruptive=True))

        plan = ReconciliationPlan(tuple(actions), changes)
        if any(action.opcode not in VOLATILE_RECONCILIATION_OPCODES for action in plan.actions):
            raise ReconciliationError("planner produced a forbidden operation")
        self.last_plan = plan
        return plan

    def reconcile(self, approve: bool = False):
        if self._mode == "report_only":
            return self.get_plan()
        if self._mode == "manual" and not approve:
            return self.get_plan()
        if not self._reconcile_lock.acquire(blocking=False):
            raise ReconciliationError("another reconciliation plan is already executing")
        try:
            # Refresh observations immediately before planning and executing.
            self.state_manager.refresh(include_saved=self._source == "nvs")
            plan = self.get_plan()
            results = []
            for action in plan.actions:
                # Recheck the capability boundary immediately before every write.
                if action.opcode not in VOLATILE_RECONCILIATION_OPCODES:
                    raise ReconciliationError(f"forbidden opcode 0x{action.opcode:02x}")
                try:
                    for attempt in range(5):
                        try:
                            self.executor.execute(action)
                            break
                        except DeviceCommandError as exc:
                            # BUSY is a transient firmware precondition, not a
                            # reason to hammer the UART or abandon immediately.
                            if exc.code != 4 or attempt == 4:
                                raise
                            self.state_manager.refresh(include_saved=False)
                            time.sleep(min(1.6, 0.1 * (2 ** attempt)))
                    results.append({"action": action.name, "success": True})
                except Exception as exc:
                    results.append({"action": action.name, "success": False, "error": str(exc)})
                    self.last_result = {"success": False, "results": results}
                    self.dispatcher.emit("reconciliation_failed", self.last_result)
                    raise ReconciliationError(f"action {action.name} failed: {exc}") from exc
            state = self.state_manager.refresh(include_saved=False)
            if not state.runtime.valid or not state.status_valid:
                raise ReconciliationError("post-action state verification failed: snapshot invalid")
            remaining = self.get_diff()
            self.last_result = {
                "success": not remaining, "results": results, "remaining_differences": remaining,
                "nvs_writes": 0,
            }
            self.dispatcher.emit("reconciliation_complete", self.last_result)
            return self.last_result
        finally:
            self._reconcile_lock.release()

    def apply_nvs_to_runtime(self, approve: bool = True):
        self.set_source("nvs")
        if self._mode == "report_only":
            self.set_mode("manual")
        return self.reconcile(approve=approve)
