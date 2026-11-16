"""Check the UART handshake and read the device state."""
from __future__ import annotations

import argparse
from dataclasses import asdict
from pprint import pprint

from common import add_connection_arguments, connected_node, refresh_state


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_connection_arguments(parser)
    args = parser.parse_args()

    try:
        with connected_node(args) as node:
            state = refresh_state(node)
            print(f"UART: {node.uart_device}")
            print(f"Connected: {node.is_connected}; ready: {node.is_ready}")
            print(f"Runtime configuration valid: {state.runtime.valid}")
            print(f"Saved configuration valid: {state.saved.valid}")
            print("Device state:")
            pprint(asdict(state), sort_dicts=True)
            print("Host diagnostics:")
            pprint(node.get_diagnostics(), sort_dicts=True)

            if not state.runtime.valid:
                print("FAIL: runtime configuration could not be read")
                return 1

            print("PASS: UART handshake and status read succeeded")
            return 0
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
