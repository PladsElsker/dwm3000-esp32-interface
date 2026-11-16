"""Observe periodic UWB measurements for a bounded duration."""
from __future__ import annotations

import argparse
import queue
import time
from dataclasses import asdict

from common import add_connection_arguments, connected_node, refresh_state


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_connection_arguments(parser)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument(
        "--start",
        action="store_true",
        help="Start ranging for this test; requires initiator role, targets, and schedule",
    )
    args = parser.parse_args()

    if args.duration <= 0:
        parser.error("--duration must be greater than zero")

    try:
        with connected_node(args) as node:
            state = refresh_state(node)
            if not state.runtime.valid:
                raise RuntimeError("runtime configuration is invalid; run 01_status.py first")
            if state.status.get("role") != "initiator":
                raise RuntimeError("periodic ranging requires the initiator role")

            runtime = state.runtime.values
            targets = runtime.get("targets") or ()
            schedule = runtime.get("schedule") or {}
            if not targets:
                raise RuntimeError("no UWB targets are configured on this node")
            if not schedule.get("period_ms"):
                raise RuntimeError("no valid UWB period is configured")

            subscription = node.uwb.measurements.subscribe(maxsize=256)
            started_here = False
            count = 0
            valid_count = 0
            try:
                if args.start:
                    if state.status.get("ranging_enabled"):
                        raise RuntimeError(
                            "ranging is already enabled; omit --start to observe the existing stream"
                        )
                    node.uwb.start_ranging()
                    started_here = True
                    print("Started periodic ranging for this test.")
                elif not state.status.get("ranging_enabled"):
                    raise RuntimeError(
                        "ranging is not enabled; run with --start or start it separately"
                    )

                deadline = time.monotonic() + args.duration
                print(f"Observing UWB measurements for {args.duration:.1f}s...")
                while time.monotonic() < deadline:
                    remaining = deadline - time.monotonic()
                    try:
                        measurement = subscription.get(timeout=min(remaining, 1.0))
                    except queue.Empty:
                        continue
                    count += 1
                    valid_count += int(measurement.valid)
                    print(asdict(measurement), flush=True)
            finally:
                node.uwb.measurements.unsubscribe(subscription)
                if started_here:
                    try:
                        node.uwb.stop_ranging()
                        print("Stopped ranging started by this test.")
                    except Exception as exc:
                        print(f"WARNING: could not stop ranging cleanly: {exc}")

            print(f"Received {count} measurement(s), {valid_count} valid")
            if count == 0 or valid_count == 0:
                print("FAIL: no valid measurements were received")
                return 1
            print("PASS: periodic UWB measurements received")
            return 0
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
