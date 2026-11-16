"""Shared helpers for manually run d3kble hardware checks."""
from __future__ import annotations

from contextlib import contextmanager

from d3kble import D3KNode


def add_connection_arguments(parser):
    parser.add_argument(
        "--uart",
        required=True,
        help="Explicit UART device connected to the ESP32 (for example /dev/ttyAMA2)",
    )
    parser.add_argument("--baudrate", type=int, default=460800)
    parser.add_argument("--connect-timeout", type=float, default=10.0)
    return parser


@contextmanager
def connected_node(args):
    node = D3KNode(
        uart_device=args.uart,
        baudrate=args.baudrate,
    )
    try:
        node.start()
        node.wait_until_ready(timeout=args.connect_timeout)
        yield node
    finally:
        node.close()


def refresh_state(node):
    state = node.get_state(refresh=True)
    if not state.status_valid:
        raise RuntimeError("GET_STATUS failed; device status is not valid")
    return state
