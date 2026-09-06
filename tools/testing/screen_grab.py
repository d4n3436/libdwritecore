#!/usr/bin/env python3
"""
How to photograph the screen a browser is on.

A browser cannot take the shot that a comparison is made of. What is compared
is what the compositor actually put on the screen, so the frame is read from
outside the browser: from an X display, from a guest's emulated framebuffer,
or from a window inside a guest that is not the session on screen.

    x11:<display>       the root window of that display, read into memory
    libvirt:<domain>    that guest's display, through QEMU's own screendump
    guest:<host>[:port] a window inside a Windows guest, over TCP from wincap

open_grabber(backend, marionette_port) returns the one a backend string names,
and each answers .grab() with the pixels, so the caller does not care which it
is. The whole-plan sweep and the single-cell capture both read a screen through
this, so a cell measured on its own and the same cell in a sweep are
photographed the same way.
"""

import os
import socket
import subprocess
import sys
import time

import numpy as np


class X11Grabber:
    """The root window of one X display, read straight into memory."""

    # GetImage answers BadMatch for a root window the server will not read
    # right then, and the next attempt a moment later succeeds. One of those
    # used to lose the cell, and a lost cell reads as a difference, so the
    # grab is retried the way the rest of the capture retakes a shot that did
    # not answer. The geometry is read again first, since a root that resized
    # is the other thing BadMatch reports.
    _RETRIES = 4

    def __init__(self, spec):
        from Xlib import display, X, error
        self._X = X
        self._error = error
        self._display = display.Display(spec)
        self._root = self._display.screen().root
        self._measure()

    def _measure(self):
        geometry = self._root.get_geometry()
        self.width, self.height = geometry.width, geometry.height

    def grab(self, x=0, y=0, w=None, h=None):
        want_w, want_h = w, h
        last = None
        for attempt in range(self._RETRIES):
            w = self.width if want_w is None else want_w
            h = self.height if want_h is None else want_h
            try:
                raw = self._root.get_image(x, y, w, h, self._X.ZPixmap, 0xFFFFFFFF)
            except (self._error.BadMatch, self._error.BadDrawable) as caught:
                last = caught
                time.sleep(0.05 * (attempt + 1))
                self._measure()
                continue
            flat = np.frombuffer(raw.data, dtype=np.uint8)
            return flat.reshape(h, w, 4)[:, :, 2::-1]
        raise last


class LibvirtGrabber:
    """A guest's whole display, through QEMU's own screendump.

    libvirt's screenshot API hands back a PNG, and the encode is most of what it
    costs: 124 ms against 17 ms for the same frame dumped as a PPM, and 37 ms to
    decode against 4 ms. QEMU writes the file on the host itself, so the image
    never crosses the libvirt stream. The two routes were compared over a whole
    2560x1440 frame and are identical, max channel difference 0.
    """

    def __init__(self, domain):
        self.domain = domain
        self.path = "/tmp/dwc-screendump-%s.ppm" % domain
        self._pixels = None
        self._dump()
        self._header()

    def _dump(self):
        uri = os.environ.get("LIBVIRT_URI", "qemu:///system")
        command = json.dumps({"execute": "screendump",
                              "arguments": {"filename": self.path}})
        subprocess.run(["virsh", "--connect", uri, "qemu-monitor-command",
                        self.domain, command], check=True, capture_output=True)

    def _header(self):
        """Where the pixels start in the P6 file, and how wide a row is.

        Read once: the guest's mode does not change under a sweep. It lets a
        poll read the sixteen rows it looks at instead of all 11 MB.
        """
        if self._pixels is None:
            with open(self.path, "rb") as handle:
                fields = []
                while len(fields) < 4:
                    line = handle.readline()
                    if line.startswith(b"#"):
                        continue
                    fields.extend(line.split())
                self._pixels = handle.tell()
            self.width, self.height = int(fields[1]), int(fields[2])
        return self._pixels

    def grab(self, x=0, y=0, w=None, h=None):
        self._dump()
        start = self._header()
        rows = self.height if h is None else h
        flat = np.fromfile(self.path, dtype=np.uint8,
                           count=rows * self.width * 3,
                           offset=start + y * self.width * 3)
        frame = flat.reshape(rows, self.width, 3)
        return frame if w is None else frame[:, x:x + w]


class GuestGrabber:
    """A window inside a Windows guest, read over TCP from wincap.exe.

    screendump photographs the emulated framebuffer, so it sees only what the
    console is scanning out. wincap runs inside the session that owns the
    window and asks the window to render itself, so a capture does not need
    that session to be the one on screen.

    GRABZ is used for the payload, which is PackBits over whole pixels. A page
    of text compresses enough that the wire is not what a sweep costs.

    The connection is kept. wincap answers one request per connection unless
    the client opens with KEEP, and a handshake polls the screen several times
    a cell, so a fresh TCP connect for each costs more than a small grab. A
    server that declines KEEP is served one request per connection.
    """

    def __init__(self, spec, marionette_port):
        # guest:<host> alone means the capture server sits one above the
        # browser's Marionette port, which is where run_parity_firefox.sh
        # starts it for each instance.
        host, _, port = spec.rpartition(":")
        if not host:
            host, port = spec, str(marionette_port + 1)
        if not host or not port.isdigit():
            sys.exit("guest backend is guest:<host>[:<port>]")
        self.addr = (host, int(port))
        self.sock = None
        self.stream = None
        self.keep = self._open()
        self.width, self.height = self._size()

    def _open(self):
        """Connect and ask to keep it. True once the server has agreed."""
        sock = socket.create_connection(self.addr, timeout=60)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        stream = sock.makefile("rb")
        sock.sendall(b"KEEP\n")
        if stream.readline().strip() == b"OK":
            self.sock, self.stream = sock, stream
            return True
        stream.close()
        sock.close()
        return False

    def _drop(self):
        for handle in (self.stream, self.sock):
            try:
                if handle is not None:
                    handle.close()
            except OSError:
                pass
        self.sock = self.stream = None

    def _ask(self, request, read):
        """Send one request and read exactly its answer.

        Every answer is self-delimiting, so the reader stops on its own
        boundary and leaves the connection at the start of the next one.
        """
        if not self.keep:
            sock = socket.create_connection(self.addr, timeout=60)
            try:
                sock.sendall(request)
                return read(sock.makefile("rb"))
            finally:
                sock.close()
        for attempt in (0, 1):
            try:
                if self.sock is None and not self._open():
                    self.keep = False
                    return self._ask(request, read)
                self.sock.sendall(request)
                return read(self.stream)
            except OSError:
                self._drop()
                if attempt:
                    raise
        raise RuntimeError("unreachable")

    @staticmethod
    def _read_size(stream):
        return stream.readline().split()

    @staticmethod
    def _read_frame(stream):
        # "PK\n<w> <h> <bytes>\n", then the packed pixels.
        head = stream.readline().strip()
        if head != b"PK":
            sys.exit("wincap answered %r, not a packed frame" % head[:16])
        gw, gh, count = (int(v) for v in stream.readline().split())
        if gw <= 0 or gh <= 0:
            sys.exit("wincap could not capture the window")
        packed = stream.read(count)
        if len(packed) != count:
            raise OSError("wincap sent %d of %d packed bytes" % (len(packed), count))
        return _unpack(packed, gw, gh)

    def _size(self):
        answer = self._ask(b"SIZE\n", self._read_size)
        if len(answer) != 2:
            sys.exit("wincap did not answer SIZE")
        return int(answer[0]), int(answer[1])

    def grab(self, x=0, y=0, w=None, h=None):
        # The whole window means the window as it is now, which is not the
        # size it had when this opened once the sweep has resized it.
        if w is None or h is None:
            self.width, self.height = self._size()
        w = self.width if w is None else w
        h = self.height if h is None else h
        return self._ask(b"GRABZ %d %d %d %d\n" % (x, y, w, h), self._read_frame)


def _unpack(packed, w, h):
    """Decode PackBits over whole pixels, the encoding GRABZ writes.

    A leading byte over 127 is a run of 257 - b pixels of the one color that
    follows. Otherwise it is b + 1 literal pixels.
    """
    flat = np.empty((h * w, 3), dtype=np.uint8)
    src = 0
    out = 0
    total = h * w
    while out < total and src < len(packed):
        control = packed[src]
        src += 1
        if control > 127:
            run = min(257 - control, total - out)
            flat[out:out + run] = np.frombuffer(packed, np.uint8, 3, src)
            src += 3
            out += run
        else:
            lit = min(control + 1, total - out)
            flat[out:out + lit] = np.frombuffer(
                packed, np.uint8, lit * 3, src).reshape(lit, 3)
            src += (control + 1) * 3
            out += lit
    if out != total:
        sys.exit("wincap sent %d of %d pixels" % (out, total))
    return flat.reshape(h, w, 3)


def open_grabber(backend, marionette_port):
    kind, _, rest = backend.partition(":")
    if kind == "x11":
        return X11Grabber(rest)
    if kind == "libvirt":
        return LibvirtGrabber(rest)
    if kind == "guest":
        return GuestGrabber(rest, marionette_port)
    raise SystemExit("backend must be x11:<display>, libvirt:<domain> "
                     "or guest:<host>[:<port>]")


def main():
    """Take one shot of a screen and write it as a PNG.

    A sweep hands its frames to the comparison and writes nothing; this is the
    one place a capture is still put on disk, for the tools that want pixels.
    """
    import argparse

    from PIL import Image

    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("backend", help="x11:<display>, libvirt:<domain> or guest:<host>[:port]")
    ap.add_argument("out")
    ap.add_argument("--port", type=int, default=0,
                    help="the browser's marionette port, which guest: needs to "
                         "reach the capture server beside it")
    args = ap.parse_args()

    grabber = open_grabber(args.backend, args.port)
    frame = grabber.grab()
    Image.fromarray(frame).save(args.out)


if __name__ == "__main__":
    main()
