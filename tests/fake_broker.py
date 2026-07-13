#!/usr/bin/env python3
"""Minimal MQTT 3.1.1 broker stub — just enough protocol to test shutdown
behavior against a broker that accepts a session but never acks an
UNSUBSCRIBE. Used by test_command_handler_shutdown.

  - CONNECT  -> CONNACK (accepted)
  - SUBSCRIBE -> SUBACK (granted)
  - UNSUBSCRIBE -> read and discarded, NO UNSUBACK ever sent

No TLS. Plain TCP only, for local test use.
"""
import socket
import sys

LOCAL_PORT = int(sys.argv[1])


def read_remaining_length(sock):
    multiplier = 1
    value = 0
    while True:
        b = sock.recv(1)
        if not b:
            return None
        byte = b[0]
        value += (byte & 0x7F) * multiplier
        if (byte & 0x80) == 0:
            break
        multiplier *= 128
    return value


def read_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", LOCAL_PORT))
    srv.listen(1)
    print(f"fake broker listening on 127.0.0.1:{LOCAL_PORT}", flush=True)

    conn, _ = srv.accept()
    print("client connected (TCP)", flush=True)

    while True:
        header = conn.recv(1)
        if not header:
            print("client closed connection", flush=True)
            break
        packet_type = header[0] >> 4
        remaining_len = read_remaining_length(conn)
        if remaining_len is None:
            break
        body = read_exact(conn, remaining_len) if remaining_len > 0 else b""

        if packet_type == 1:  # CONNECT
            print("got CONNECT -> sending CONNACK", flush=True)
            conn.sendall(bytes([0x20, 0x02, 0x00, 0x00]))
        elif packet_type == 8:  # SUBSCRIBE
            packet_id = body[0:2]
            print(f"got SUBSCRIBE (packet_id={packet_id.hex()}) -> sending SUBACK", flush=True)
            conn.sendall(bytes([0x90, 0x03]) + packet_id + bytes([0x01]))
        elif packet_type == 10:  # UNSUBSCRIBE
            packet_id = body[0:2]
            print(f"got UNSUBSCRIBE (packet_id={packet_id.hex()}) -> staying silent, no UNSUBACK", flush=True)
        elif packet_type == 12:  # PINGREQ
            conn.sendall(bytes([0xD0, 0x00]))
        elif packet_type == 14:  # DISCONNECT
            print("got DISCONNECT", flush=True)
            break
        else:
            print(f"got unhandled packet type {packet_type}, ignoring", flush=True)


if __name__ == "__main__":
    main()
