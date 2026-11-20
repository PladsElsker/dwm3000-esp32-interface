"""Minimal d3kble application entry point.

Select the CM4 UART path that is actually wired to the ESP32.
"""
from d3kble import D3KNode

PEER_ID = 2


def main():
    with D3KNode(uart_device="/dev/ttyACM0", baudrate=460800) as node:
        distances = node.uwb.measurements.subscribe(maxsize=256)

        @node.comms.on_message
        def handle_message(message):
            if message.peer_id != PEER_ID:
                return
            print("application message:", message.payload)

        @node.uwb.on_measurement
        def handle_measurement(measurement):
            if measurement.valid:
                print(measurement.target_id, measurement.distance_m)

        ticket = node.comms.send(PEER_ID, {"operation": "status"})
        ticket.wait(timeout=10)

if __name__ == "__main__":
    main()
