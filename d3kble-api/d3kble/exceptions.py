"""Typed exceptions raised by d3kble."""


class D3KError(Exception):
    """Base exception."""


class ProtocolError(D3KError):
    """Malformed or unsupported protocol data."""


class ConnectionLost(D3KError):
    """Serial session disconnected."""


class RequestTimeout(D3KError, TimeoutError):
    """A request did not receive a response before its deadline."""


class DeviceCommandError(D3KError):
    """The firmware rejected a command."""

    def __init__(self, opcode: int, code: int):
        self.opcode = opcode
        self.code = code
        super().__init__(f"firmware rejected opcode 0x{opcode:02x} (error {code})")


class ReconciliationError(D3KError):
    """A reconciliation plan could not be safely executed."""
