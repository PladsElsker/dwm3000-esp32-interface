"""Example application-level request/response exchange."""
from d3kble import D3KNode

PEER_ID = 2


def main():
    with D3KNode(uart_device="/dev/ttyAMA2") as node:
        @node.comms.on_request
        def handle_request(request):
            if request.peer_id != PEER_ID:
                return None
            if request.payload.get("operation") == "status":
                # Returning a value queues the correlated response in memory.
                return {"state": "ready"}
            return {"error": "unsupported operation"}

        ticket = node.comms.request(PEER_ID, {"operation": "status"})
        print("accepted:", ticket.wait_ack(timeout=10))
        print("response:", ticket.wait_response(timeout=10))


if __name__ == "__main__":
    main()
