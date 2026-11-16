"""Serve application-messaging requests by returning their payload unchanged."""
from __future__ import annotations

import argparse
import time

from common import add_connection_arguments, connected_node


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_connection_arguments(parser)
    parser.add_argument("--duration", type=float, default=120.0)
    args = parser.parse_args()

    if args.duration <= 0:
        parser.error("--duration must be greater than zero")

    try:
        with connected_node(args) as node:
            def echo(request):
                print(
                    f"Request from peer {request.peer_id}: "
                    f"message_id={request.message_id}, payload={request.payload!r}",
                    flush=True,
                )
                return {"echo": request.payload}

            node.comms.on_request(echo)
            print(
                f"Echo server ready on {node.uart_device} for {args.duration:.1f}s. "
                "Run 07_comms_request.py on the other node.",
                flush=True,
            )
            deadline = time.monotonic() + args.duration
            while time.monotonic() < deadline:
                time.sleep(min(0.25, deadline - time.monotonic()))
            print("Echo server time window ended.")
            return 0
    except KeyboardInterrupt:
        print("Echo server stopped.")
        return 0
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
