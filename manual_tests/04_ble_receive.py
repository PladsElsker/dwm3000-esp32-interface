"""Wait for one raw BLE message and display its peer and payload."""
from __future__ import annotations

import argparse
import queue
import time

from common import add_connection_arguments, connected_node


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_connection_arguments(parser)
    parser.add_argument("--peer-id", type=int, help="Only accept this sender ID (default: any peer)")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    messages: queue.Queue = queue.Queue()
    try:
        with connected_node(args) as node:
            def on_message(message):
                messages.put(message)

            node.on("ble_message", on_message)
            print("Waiting for a raw BLE message...")
            try:
                deadline = time.monotonic() + args.timeout
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("no matching BLE message received")
                    try:
                        message = messages.get(timeout=remaining)
                    except queue.Empty as exc:
                        raise TimeoutError("no matching BLE message received") from exc
                    if args.peer_id is not None and message.peer_id != args.peer_id:
                        continue

                    print(f"Peer ID: {message.peer_id}")
                    print(f"Firmware message ID: {message.message_id}")
                    print(f"Payload length: {len(message.data)} bytes")
                    print(f"UTF-8: {message.data.decode('utf-8', errors='replace')}")
                    print(f"Hex: {message.data.hex()}")
                    print("PASS: raw BLE payload received")
                    return 0
            finally:
                node.off("ble_message", on_message)
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
