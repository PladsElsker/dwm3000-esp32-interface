"""Minimal d3kble application entry point.

Select the CM4 UART path that is actually wired to the ESP32.
"""
from d3kble import D3KNode

PEER_ID = 2


def main():
    with D3KNode(uart_device="/dev/ttyAMA2", baudrate=460800) as node:
        distances = node.uwb.measurements.subscribe(maxsize=256)

        @node.comms.on_message
        def handle_message(message):
            if message.peer_id != PEER_ID:
                return
            print("application message:", message.payload)

        node.comms.send(PEER_ID, {"operation": "status"})

        while True:
            measurement = distances.get(timeout=1.0)
            if measurement.valid:
                print(measurement.target_id, measurement.distance_m)


if __name__ == "__main__":
    main()
