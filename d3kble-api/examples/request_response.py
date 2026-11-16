"""Asynchronous application-level request/response exchange."""
import asyncio

from d3kble import D3KNode, CommsResponse

PEER_ID = 2


async def main(node: D3KNode):
    node.comms.bind_asyncio_loop(asyncio.get_running_loop())

    @node.comms.on_request
    async def handle_request(request):
        if request.peer_id != PEER_ID:
            return CommsResponse(
                status="error",
                error="unexpected peer",
                content=None,
            )
        if request.payload.get("operation") == "status":
            return {"state": "ready"}
        return CommsResponse(
            status="error",
            error="unsupported operation",
            content=None,
        )

    response = await node.comms.request(PEER_ID, {"operation": "status"})
    if response.status == "error":
        print("Request failed:", response.error)
    else:
        print("Response:", response.content)


def run():
    node = D3KNode(uart_device="/dev/ttyAMA2")
    try:
        node.start()
        node.wait_until_ready(timeout=10)
        asyncio.run(main(node))
    finally:
        node.close()


if __name__ == "__main__":
    run()
