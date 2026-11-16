"""Immutable public data models."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(frozen=True, slots=True)
class DistanceMeasurement:
    target_id: int
    distance_m: float | None
    sample_sequence: int
    device_monotonic_us: int
    epoch_us: int | None
    status: int
    quality_flags: int
    received_at: float
    job_id: int = 0

    @property
    def valid(self) -> bool:
        return self.distance_m is not None and self.status == 0


@dataclass(frozen=True, slots=True)
class RangeFailure:
    job_id: int
    target_id: int
    status: int
    device_monotonic_us: int
    received_at: float


@dataclass(frozen=True, slots=True)
class BleMessage:
    peer_id: int
    message_id: int
    data: bytes
    received_at: float


@dataclass(frozen=True, slots=True)
class ApplicationMessage:
    peer_id: int
    message_id: str
    kind: str
    payload: Any
    received_at: float
    reply_to: str | None = None


@dataclass(frozen=True, slots=True)
class CommsResponse:
    """Result of an application-level request."""

    status: str
    error: str | None
    content: Any

    def __post_init__(self):
        if self.status not in {"ok", "error"}:
            raise ValueError("response status must be 'ok' or 'error'")
        if self.status == "ok" and self.error is not None:
            raise ValueError("successful responses cannot have an error")
        if self.status == "error" and not self.error:
            raise ValueError("error responses require an error reason")

    @property
    def ok(self) -> bool:
        return self.status == "ok"


@dataclass(frozen=True, slots=True)
class RuntimeConfig:
    values: dict[str, Any] = field(default_factory=dict)
    valid: bool = False
    updated_at: float | None = None


@dataclass(frozen=True, slots=True)
class SavedConfig:
    values: dict[str, Any] = field(default_factory=dict)
    valid: bool = False
    updated_at: float | None = None


@dataclass(frozen=True, slots=True)
class DeviceState:
    runtime: RuntimeConfig
    saved: SavedConfig
    status: dict[str, Any] = field(default_factory=dict)
    status_valid: bool = False
    status_updated_at: float | None = None


@dataclass(frozen=True, slots=True)
class UwbHistoryPage:
    oldest_seq: int
    newest_seq: int
    next_cursor: int
    overrun: bool
    records: tuple[DistanceMeasurement, ...]
