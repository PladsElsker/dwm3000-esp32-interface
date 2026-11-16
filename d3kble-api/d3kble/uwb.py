"""UWB controls and publish/subscribe distance measurement streams."""
from __future__ import annotations

import struct
from .protocol import decode_history, decode_range_record, decode_targets, encode_targets, pack_u16
from .streams import MeasurementStream
from .models import UwbHistoryPage


class UwbManager:
    def __init__(self, transport, dispatcher):
        self.transport = transport
        self.dispatcher = dispatcher
        self.measurements = MeasurementStream()
        self.last_sample_sequence: dict[int, int] = {}

    def on_event(self, frame) -> None:
        if frame.opcode == 0x80:
            from .protocol import decode_range_result
            job_id, measurement = decode_range_result(frame.payload)
            measurement = type(measurement)(
                target_id=measurement.target_id, distance_m=measurement.distance_m,
                sample_sequence=measurement.sample_sequence,
                device_monotonic_us=measurement.device_monotonic_us,
                epoch_us=measurement.epoch_us, status=measurement.status,
                quality_flags=measurement.quality_flags,
                received_at=measurement.received_at, job_id=job_id,
            )
            self.last_sample_sequence[measurement.target_id] = measurement.sample_sequence
            self.measurements.publish(measurement)
            self.dispatcher.emit("range_result", measurement)
        elif frame.opcode == 0x81:
            from .protocol import decode_range_failed
            failure = decode_range_failed(frame.payload)
            self.dispatcher.emit("range_failed", failure)
        elif frame.opcode == 0x84:
            self.dispatcher.emit("history_overrun", frame.payload)

    def on_measurement(self, callback):
        """Convenience callback; prefer independent measurement subscriptions."""
        return self.dispatcher.on("range_result", callback)

    def start_ranging(self) -> None:
        self.transport.request("start_ranging")

    def stop_ranging(self) -> None:
        self.transport.request("stop_ranging")

    def range_once(self, target_id: int) -> int:
        payload = self.transport.request("range_once", pack_u16(target_id))
        if len(payload) != 2:
            raise ValueError("RANGE_ONCE response must contain a job ID")
        return struct.unpack("<H", payload)[0]

    def set_role(self, role: str | int) -> None:
        value = {"idle": 0, "initiator": 1, "responder": 2}.get(role, role)
        if value not in (0, 1, 2):
            raise ValueError("role must be idle, initiator, or responder")
        self.transport.request("set_role", bytes([value]), retryable=False)

    def set_targets(self, target_ids) -> tuple[int, ...]:
        payload = self.transport.request("set_targets", encode_targets(target_ids), retryable=False)
        return decode_targets(payload)

    def get_targets(self) -> tuple[int, ...]:
        return decode_targets(self.transport.request("get_targets"))

    def set_schedule(self, period_ms: int, timeout_ms: int = 120, retries: int = 1) -> None:
        if not 20 <= period_ms <= 60000 or not 50 <= timeout_ms <= 5000 or not 0 <= retries <= 5:
            raise ValueError("invalid UWB schedule")
        payload = struct.pack("<IHB", period_ms, timeout_ms, retries)
        self.transport.request("set_schedule", payload, retryable=False)

    def get_schedule(self) -> dict:
        from .protocol import decode_schedule
        return decode_schedule(self.transport.request("get_schedule"))

    def get_measurements(self, target_id: int = 0xFFFF):
        payload = self.transport.request("get_last_range", pack_u16(target_id))
        if not payload:
            return ()
        count = payload[0]
        if len(payload) != 1 + count * 28:
            raise ValueError("GET_LAST_RANGE response length mismatch")
        return tuple(
            decode_range_record(payload[1 + i * 28:1 + (i + 1) * 28])
            for i in range(count)
        )

    def get_history(self, after_sample_seq: int = 0, max_records: int = 17,
                    target_id: int = 0xFFFF) -> UwbHistoryPage:
        if not 0 <= after_sample_seq <= 0xFFFFFFFF:
            raise ValueError("after_sample_seq must fit uint32")
        if not 0 <= max_records <= 0xFFFF:
            raise ValueError("max_records must fit uint16")
        payload = struct.pack("<IHH", after_sample_seq, max_records, target_id)
        result = decode_history(self.transport.request("get_history", payload))
        return UwbHistoryPage(
            oldest_seq=result["oldest_seq"], newest_seq=result["newest_seq"],
            next_cursor=result["next_cursor"], overrun=result["overrun"],
            records=result["records"],
        )

    def close(self):
        self.measurements.close()
