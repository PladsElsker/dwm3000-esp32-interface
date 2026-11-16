import pytest

from d3kble.config import (
    ConfigManager, ReconciliationAction, VOLATILE_RECONCILIATION_OPCODES,
    VolatileCommandExecutor,
)
from d3kble.exceptions import ReconciliationError
from d3kble.models import DeviceState, RuntimeConfig, SavedConfig
from d3kble.protocol import OPS


class FakeState:
    def __init__(self, runtime, status, saved=None):
        self.value = DeviceState(
            RuntimeConfig(runtime, True, 10.0),
            SavedConfig(saved or {}, saved is not None, 9.0),
            status, True, 10.0,
        )

    def snapshot(self):
        return self.value

    def refresh(self, include_saved=False):
        return self.value


class FakeTransport:
    def __init__(self):
        self.calls = []

    def request(self, opcode, payload=b"", **kwargs):
        self.calls.append((opcode, payload, kwargs))
        return b""


class FakeDispatcher:
    def emit(self, *args):
        pass


def test_reconciliation_allowlist_excludes_all_nvs_writes():
    forbidden = {
        OPS["apply_save_config"], OPS["save_antenna_delays"],
        OPS["set_peers"], OPS["reset_config"],
    }
    assert forbidden.isdisjoint(VOLATILE_RECONCILIATION_OPCODES)
    transport = FakeTransport()
    executor = VolatileCommandExecutor(transport, None)
    with pytest.raises(ReconciliationError):
        executor.execute(ReconciliationAction("apply_save_config", OPS["apply_save_config"]))
    assert transport.calls == []


def test_executor_accepts_volatile_commands():
    transport = FakeTransport()
    executor = VolatileCommandExecutor(transport, None)
    executor.execute(ReconciliationAction("set_role", OPS["set_role"], b"\x01"))
    assert transport.calls[0][0] == OPS["set_role"]


def test_schedule_plan_stops_and_restores_ranging():
    runtime = {
        "device_id": 1, "pan_id": 0xCAFE, "channel": 5,
        "tx_antenna_delay": 16384, "rx_antenna_delay": 16384,
        "period_ms": 100, "timeout_ms": 120, "retries": 1,
        "targets": (2,), "ble_mode": "idle", "ble_peers": (),
        "ble_advertising_interval_ms": 100, "ble_scan_interval_ms": 100,
        "ble_scan_window_ms": 50, "ble_auto_start": False,
        "ble_retry_interval_ms": 1000, "ble_connection_timeout_ms": 5000,
        "ble_max_retries": 3, "ble_rx_message_timeout_ms": 1000,
    }
    status = {"role": "initiator", "ranging_enabled": True, "ble_enabled": False}
    state = FakeState(runtime, status)
    transport = FakeTransport()
    manager = ConfigManager(transport, state, FakeDispatcher())
    manager.set_desired({"period_ms": 250})
    plan = manager.get_plan()
    assert [action.name for action in plan.actions] == [
        "stop_ranging", "set_schedule", "start_ranging"
    ]
    assert plan.nvs_writes == 0
    assert all(action.opcode in VOLATILE_RECONCILIATION_OPCODES for action in plan.actions)


def test_nvs_boot_preferences_do_not_force_live_service_state():
    runtime = {
        "device_id": 1, "pan_id": 0xCAFE, "channel": 5,
        "tx_antenna_delay": 16384, "rx_antenna_delay": 16384,
        "period_ms": 100, "timeout_ms": 120, "retries": 1,
        "targets": (), "ble_mode": "idle", "ble_peers": (),
        "ble_advertising_interval_ms": 100, "ble_scan_interval_ms": 100,
        "ble_scan_window_ms": 50, "ble_auto_start": False,
        "ble_retry_interval_ms": 1000, "ble_connection_timeout_ms": 5000,
        "ble_max_retries": 3, "ble_rx_message_timeout_ms": 1000,
    }
    status = {"role": "idle", "ranging_enabled": False, "ble_enabled": False}
    saved = dict(runtime, role="idle", targets=(), auto_start=False, ble_auto_start=False)
    manager = ConfigManager(FakeTransport(), FakeState(runtime, status, saved), FakeDispatcher())
    manager.set_source("nvs")
    assert "ranging_enabled" not in {change.path for change in manager.get_diff()}
    assert "ble_enabled" not in {change.path for change in manager.get_diff()}



def test_idle_desired_role_and_ble_mode_stop_services_without_restarting():
    runtime = {
        "device_id": 1, "pan_id": 0xCAFE, "channel": 5,
        "tx_antenna_delay": 16384, "rx_antenna_delay": 16384,
        "period_ms": 100, "timeout_ms": 120, "retries": 1,
        "targets": (2,), "ble_mode": "hub", "ble_peers": (),
        "ble_advertising_interval_ms": 100, "ble_scan_interval_ms": 100,
        "ble_scan_window_ms": 50, "ble_auto_start": False,
        "ble_retry_interval_ms": 1000, "ble_connection_timeout_ms": 5000,
        "ble_max_retries": 3, "ble_rx_message_timeout_ms": 1000,
    }
    status = {"role": "initiator", "ranging_enabled": True, "ble_enabled": True}
    manager = ConfigManager(FakeTransport(), FakeState(runtime, status), FakeDispatcher())
    manager.set_desired({"role": "idle", "ble_mode": "idle"})
    plan = manager.get_plan()
    names = [action.name for action in plan.actions]
    assert "stop_ranging" in names
    assert "stop_ble" in names
    assert "set_role" in names
    assert "start_ranging" not in names
    assert "start_ble" not in names
