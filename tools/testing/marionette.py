#!/usr/bin/env python3
"""
A Marionette client, shared by the tools here that drive a browser.

Framing is "<byte length>:<json>" and every command is [0, id, name, params],
which is the whole protocol these tools need.

Start the browser with:

    firefox -marionette -profile <dir> about:blank &

which listens on 127.0.0.1:2828 by default; set marionette.port in the profile
to move it. Chrome-context scripts additionally need
-remote-allow-system-access.

To reach a Marionette in a virtual machine, forward the port to the guest -
Marionette binds to loopback only - and pass the guest's address.
"""

import json
import socket


class Marionette:
    def __init__(self, host="127.0.0.1", port=2828, timeout=120):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""
        self.next_id = 1
        self.recv()                       # server handshake

    def close(self):
        """End the session before dropping the socket.

        Marionette takes one client at a time, and a session it was never asked
        to delete outlives the process that opened it: the next client then
        waits for a handshake that never comes.
        """
        sock = getattr(self, "sock", None)
        if sock is None:
            return
        self.sock = None
        try:
            sock.settimeout(5)
            message = json.dumps([0, self.next_id, "WebDriver:DeleteSession", {}]).encode()
            sock.sendall(b"%d:%s" % (len(message), message))
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        self.close()

    def wait(self, seconds):
        """How long one reply may take."""
        self.sock.settimeout(seconds)

    def recv(self):
        while b":" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("marionette closed the connection")
            self.buf += chunk
        length, _, rest = self.buf.partition(b":")
        need = int(length)
        while len(rest) < need:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("marionette closed the connection")
            rest += chunk
        self.buf = rest[need:]
        return json.loads(rest[:need])

    def call(self, name, params=None):
        message = json.dumps([0, self.next_id, name, params or {}]).encode()
        self.next_id += 1
        self.sock.sendall(b"%d:%s" % (len(message), message))
        reply = self.recv()
        if reply[2] is not None:
            raise RuntimeError("%s: %s" % (name, reply[2]))
        return reply[3]

    def start(self, context="content"):
        self.call("WebDriver:NewSession", {"capabilities": {}})
        self.call("Marionette:SetContext", {"value": context})

    def script(self, source, args=None, sandbox=None):
        """Run a script and unwrap the value the way Marionette returns it.

        sandbox="system" runs the script with chrome privileges *inside the
        content process*, the only way to reach a chrome-only global such as
        InspectorUtils and the page's DOM at the same time. The browser must
        have been started with -remote-allow-system-access.
        """
        params = {"script": source, "args": args or []}
        if sandbox:
            params["sandbox"] = sandbox
        value = self.call("WebDriver:ExecuteScript", params)
        if isinstance(value, dict) and "value" in value:
            return value["value"]
        return value
