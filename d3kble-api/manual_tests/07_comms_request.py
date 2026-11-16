"""Send an awaitable request and verify its correlated echo response."""
from __future__ import annotations

import argparse
import asyncio
import json

from common import add_connection_arguments, connected_node


async def run_request(node, peer_id: int, payload) -> int:
    node.comms.bind_asyncio_loop(asyncio.get_running_loop())

    # request() queues immediately and returns a CommsTicket; awaiting the
    # ticket waits asynchronously for its correlated CommsResponse.
    ticket = node.comms.request(peer_id, payload)
    print("Awaiting correlated response (1 second response deadline)...")
    response = await ticket

    print(f"Response status: {response.status}")
    print(f"Error: {response.error}")
    print(f"Content: {response.content}")

    if not response.ok or response.content != {"echo": payload}:
        print("FAIL: response does not match the expected echo")
        return 1

    print("PASS: awaitable request/response exchange succeeded")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_connection_arguments(parser)
    parser.add_argument("--peer-id", type=int, required=True, help="Remote echo-server node ID")
    parser.add_argument(
        "--payload",
        default='{"manual_test": "d3kble"}',
        help="JSON value to send",
    )
    args = parser.parse_args()

    try:
        payload = json.loads(args.payload)
    except json.JSONDecodeError as exc:
        parser.error(f"--payload must be valid JSON: {exc}")

    try:
        with connected_node(args) as node:
            return asyncio.run(run_request(node, args.peer_id, payload))
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
