"""Real loopback integration using only Python's standard library."""
import base64
import concurrent.futures
import json
import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile


def exact(sock, count):
    result = b""
    while len(result) < count:
        chunk = sock.recv(count - len(result))
        if not chunk:
            raise EOFError("connection closed")
        result += chunk
    return result


class Client:
    def __init__(self, port, split=False):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (f"GET /ws HTTP/1.1\r\nHost: localhost\r\nupgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n\r\n").encode()
        if split:
            for byte in request:
                self.socket.sendall(bytes([byte]))
        else:
            self.socket.sendall(request)
        response = b""
        while not response.endswith(b"\r\n\r\n"):
            response += exact(self.socket, 1)
        assert response.startswith(b"HTTP/1.1 101"), response

    def send(self, data, opcode=1, split=False):
        payload = json.dumps(data).encode() if not isinstance(data, bytes) else data
        mask = b"mask"
        header = bytes([0x80 | opcode, 0x80 | len(payload)]) if len(payload) < 126 else bytes([0x80 | opcode, 0xfe]) + struct.pack("!H", len(payload))
        frame = header + mask + bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        if split:
            for byte in frame:
                self.socket.sendall(bytes([byte]))
        else:
            self.socket.sendall(frame)

    def receive(self):
        first, second = exact(self.socket, 2)
        length = second & 127
        if length == 126:
            length = struct.unpack("!H", exact(self.socket, 2))[0]
        assert not second & 128
        payload = exact(self.socket, length)
        return (first & 15, payload)

    def event(self, expected):
        for _ in range(100):
            opcode, payload = self.receive()
            assert opcode == 1
            data = json.loads(payload)
            if data["event"] == expected:
                return data
        raise AssertionError(f"missing {expected}")

    def close(self):
        self.socket.close()


def main():
    with tempfile.TemporaryDirectory(prefix="tradesim-ws-") as directory:
        process = subprocess.Popen([sys.argv[1], "0", "2", directory], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        clients = []
        try:
            assert select.select([process.stdout], [], [], 10)[0], "server did not start"
            line = process.stdout.readline()
            port = int(re.search(r":(\d+)/ws", line).group(1))
            client = Client(port, split=True)
            clients.append(client)
            client.send({"type": "N", "sym": "AAA", "side": "S", "px": 100, "qty": 10}, split=True)
            first = client.event("ack")
            assert first["resting"] and first["remaining"] == 10 and first["durability"] == "fsync"
            client.send({"type": "N", "sym": "BBB", "side": "B", "px": 200, "qty": 4})
            assert client.event("ack")["resting"]
            client.send(b'{"type":"C","id":-1}')
            client.event("error")
            client.send(b'{"type":"C","id":1,"id":2}')
            client.event("error")
            client.send({"type": "P", "sym": "AAA"})
            depth = client.event("depth")
            assert depth["asks"] == [[100, 10, 1]] and depth["bids"] == []
            assert len(depth["state_hash"]) == 64
            client.send({"type": "C", "id": first["id"]})
            assert client.event("ack")["cancelled"]
            client.send(b"heartbeat", opcode=9)
            assert client.receive() == (10, b"heartbeat")

            def producer(number):
                peer = Client(port)
                try:
                    for _ in range(25):
                        peer.send({"type": "N", "sym": f"S{number}", "side": "B", "px": 50, "qty": 1})
                        assert peer.event("ack")["resting"]
                    peer.send({"type": "P", "sym": f"S{number}"})
                    assert peer.event("depth")["bids"] == [[50, 25, 25]]
                finally:
                    peer.close()

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
                list(executor.map(producer, range(8)))
            for frame in (b"\x81\x01x", b"\x81\xff" + struct.pack("!Q", 2**40)):
                bad = Client(port)
                bad.socket.sendall(frame)
                assert bad.socket.recv(1) == b""
                bad.close()
            client.send({"type": "P", "sym": "BBB"})
            assert client.event("depth")["bids"] == [[200, 4, 1]]
            process.send_signal(signal.SIGTERM)
            _, errors = process.communicate(timeout=5)
            assert process.returncode == 0, errors
            print("PASS split handshake/frames, ACK/depth, validation, ping/pong, 8 clients, hostile lengths, graceful shutdown")
        finally:
            for peer in clients:
                peer.close()
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    main()
