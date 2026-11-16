"""A compact snapshot of host-side transport and middleware health."""
from __future__ import annotations


def collect_diagnostics(node) -> dict:
    return {
        "connected": node.is_connected,
        "ready": node.is_ready,
        "uart_device": node.uart_device,
        "transport": {
            "rx_errors": node.transport.rx_errors,
            "frames_received": node.transport.frames_received,
            "event_drops": node.transport.event_drops,
        },
        "callbacks": {
            "errors": node.events.errors,
            "dropped": node.events.dropped,
        },
        "ble": {
            "sent": node.ble.sent_count,
            "expired": node.ble.expired_count,
            "rx_errors": node.ble.rx_errors,
        },
        "comms": {
            "tx_retries": node.comms.tx_retries,
            "duplicates": node.comms.duplicates,
            "rx_messages": node.comms.rx_messages,
        },
    }
