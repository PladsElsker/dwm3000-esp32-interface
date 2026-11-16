"""Issue one RANGE_ONCE request and wait for its matching result."""
from __future__ import annotations

import argparse
import queue
import time
from dataclasses import asdict

from common import add_connection_arguments, connected_node, refresh_state


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_connection_arguments(parser)
    parser.add_argument("--target-id", type=int, required=True, help="UWB target device ID")
    parser.add_argument("--timeout", type=float, default=15.0, help="Result timeout in seconds")
    args = parser.parse_args()

    try:
        with connected_node(args) as node:
            state = refresh_state(node)
            if not state.runtime.valid:
                raise RuntimeError("runtime configuration is invalid; run 01_status.py first")

            status = state.status
            if status.get("role") != "initiator":
                raise RuntimeError(
                    f"RANGE_ONCE requires initiator role; current role is {status.get('role')!r}"
                )
            if not status.get("radio_ready") or status.get("radio_standby"):
                raise RuntimeError("UWB radio must be ready and not in standby")

            events: queue.Queue = queue.Queue()

            def on_measurement(measurement):
                events.put(("result", measurement))

            def on_failure(failure):
                events.put(("failure", failure))

            node.uwb.on_measurement(on_measurement)
            node.on("range_failed", on_failure)
            try:
                job_id = node.uwb.range_once(args.target_id)
                print(f"RANGE_ONCE accepted: job_id={job_id}, target_id={args.target_id}")

                deadline = time.monotonic() + args.timeout
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError(
                            f"no result received for job {job_id} within {args.timeout:.1f}s"
                        )
                    try:
                        event_type, value = events.get(timeout=remaining)
                    except queue.Empty as exc:
                        raise TimeoutError(
                            f"no result received for job {job_id} within {args.timeout:.1f}s"
                        ) from exc

                    if value.job_id != job_id:
                        continue

                    print(f"{event_type.upper()}:")
                    pprint_value = asdict(value)
                    print(pprint_value)
                    if event_type == "failure" or not value.valid:
                        print("FAIL: the one-shot operation did not produce a valid range")
                        return 1

                    print("PASS: one-shot UWB range completed")
                    return 0
            finally:
                node.off("range_result", on_measurement)
                node.off("range_failed", on_failure)
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
