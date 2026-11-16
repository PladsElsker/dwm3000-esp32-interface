import queue

from d3kble.events import Subscription
from d3kble.models import DistanceMeasurement
from d3kble.streams import MeasurementStream


def measurement(sequence):
    return DistanceMeasurement(
        target_id=42, distance_m=1.25, sample_sequence=sequence,
        device_monotonic_us=sequence * 1000, epoch_us=None,
        status=0, quality_flags=0, received_at=1.0,
    )


def test_measurement_subscribers_are_independent():
    stream = MeasurementStream()
    first = stream.subscribe()
    second = stream.subscribe()
    value = measurement(1)
    stream.publish(value)
    assert first.get_nowait() is value
    assert second.get_nowait() is value
    first.close()
    stream.publish(measurement(2))
    assert second.get_nowait().sample_sequence == 2
    stream.close()


def test_subscription_drop_oldest_is_observable():
    subscription = Subscription(maxsize=2, overflow="drop_oldest")
    for i in range(3):
        subscription.publish(measurement(i))
    assert subscription.dropped_count == 1
    assert subscription.get_nowait().sample_sequence == 1
    assert subscription.get_nowait().sample_sequence == 2


def test_subscription_drop_newest():
    subscription = Subscription(maxsize=1, overflow="drop_newest")
    subscription.publish(measurement(1))
    subscription.publish(measurement(2))
    assert subscription.dropped_count == 1
    assert subscription.get_nowait().sample_sequence == 1
