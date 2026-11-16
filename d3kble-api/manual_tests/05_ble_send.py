"""Send a raw BLE payload to a connected peer and wait for delivery."""
from __future__ import annotations

import argparse

from common import add_connection_arguments, connected_node


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_connection_arguments(parser)
    parser.add_argument("--peer-id", type=int, required=True, help="Remote BLE peer device ID")
    parser.add_argument("--text", default="d3kble manual BLE test")
    parser.add_argument("--timeout", type=float, default=12.0)
    args = parser.parse_args()

    data = args.text.encode("utf-8")
    if len(data) > 496:
        parser.error(f"encoded payload is {len(data)} bytes; firmware limit is 496")

    try:
        with connected_node(args) as node:
            ticket = node.ble.send(peer_id=args.peer_id, data=data)
            print(f"Sent {len(data)} byte(s) to peer {args.peer_id}; waiting for delivery...")
            result = ticket.wait(timeout=args.timeout)
            print(result)
            if not result.get("delivered"):
                print("FAIL: firmware did not report BLE delivery")
                return 1
            print(
                "PASS: firmware reported delivery after the receiving host consumed "
                "the complete BLE message."
            )
            return 0
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
