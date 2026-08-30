#!/usr/bin/env python3
"""Ask a guest capture server for its window size. Prints "W H", or nothing.

A separate script so the harness can poll it from a shell while waiting for
the browser's window to exist. Prints nothing when the server is not up yet or
has no window, which is what the caller loops on.
"""
import socket
import sys

try:
    sock = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=2)
    sock.sendall(b"SIZE\n")
    print(sock.recv(64).decode("ascii", "replace").strip())
    sock.close()
except OSError:
    pass
