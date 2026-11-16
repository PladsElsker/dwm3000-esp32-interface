"""Send a request and verify its correlated echo response."""
from __future__ import annotations

import argparse
import json

from common import add_connection_arguments, connected_node


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_connection_arguments(parser)
    parser.add_argument("--peer-id", type=int, required=True, help="Remote echo-server node ID")
    parser.add_argument(
        "--payload",
        default='{"manual_test": "d3kble"}',
        help="JSON value to send",
    )
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    try:
        payload = json.loads(args.payload)
    except json.JSONDecodeError as exc:
        parser.error(f"--payload must be valid JSON: {exc}")

    try:
        with connected_node(args) as node:
            ticket = node.comms.request(args.peer_id, payload)
            print("Waiting for application ACK...")
            print(ticket.wait_ack(timeout=args.timeout))
            print("Waiting for correlated response...")
            response = ticket.wait_response(timeout=args.timeout)
            print(f"Response: {response}")

            if response != {"echo": payload}:
                print("FAIL: response does not match the expected echo")
                return 1

            print("PASS: application ACK and request/response exchange succeeded")
            return 0
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
