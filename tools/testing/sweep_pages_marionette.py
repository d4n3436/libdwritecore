#!/usr/bin/env python3
"""
Capture one Firefox side of a whole plan over a single Marionette connection.

    tools/testing/sweep_pages_marionette.py <backend> <host> <port> <prefix> \
                                            <width> <height> <label> \
                                            <out_dir> <cells> [--css RULES]

<cells> is a file with one cell per line, "<index> <path> <scroll>". Each cell
becomes <out_dir>/cell<index>/<label>_clean.png, exactly width by height. The
page is laid out STRIP pixels taller than that, the same on every Marionette
side, and the strip below the compared part is where the capture marker goes.
One line per cell goes to stdout, "ok <index>" followed by the milliseconds
the load, the settle, the capture and the encode took, or "fail <index>
<why>", so the caller can compare finished cells while later ones are still
being captured. This is the sibling of sweep_pages_cdp.py for a side whose
driver is marionette.

The screen is still what gets read. Firefox has no faithful in-browser capture:
WebDriver:TakeScreenshot renders the page again through drawSnapshot, and
ctx.drawWindow with DRAWWINDOW_USE_WIDGET_LAYERS draws into a canvas, and both
land on an ARGB surface where subpixel antialiasing cannot exist. Measured on
16px Arial, black on white, same page and same moment: 82.0% of ink pixels
carry a color fringe in a photograph of the screen and 0.0% through either
capture, with a different ink count and a different mean besides. Three corpus
pages that are pixel-exact on screen read 97.02%, 98.59% and 99.58% that way.

So the speed comes from everywhere else:

  the grab       straight out of the root window through Xlib into memory,
                 1.2 ms for the viewport rectangle against 98 ms for `import`
                 and a PNG round trip
  the process    one for the plan, so the interpreter and numpy are paid for
                 once instead of the 191 ms per page a fresh one costs
  the connection one for the plan, which is worth 2 ms a page and is the
                 smallest of the three

`backend` is x11:<display> or libvirt:<domain>, as in capture_viewport.sh, or
guest:<host>[:<port>] for a window read over TCP from wincap.exe inside a
Windows guest, on the Marionette port plus one when no port is named. A
libvirt guest is grabbed with `virsh screenshot`, which takes the whole display
and has no cheaper form, so only the other two get the fast path.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time

import numpy as np
from PIL import Image

import viewport_protocol as vp

# The page's own rules reach a bare div. `left:0` positions the margin box, so
# a `div { margin: 0 20px }` moves the marker and the crop runs off the
# window, and a `div { border }` grows it, either of which displaces or
# reshapes it. `all:initial` covers the properties a shorter reset would miss.
# The same marker compare_viewport.py reads, and the same two reasons for it.
# A flat fill cannot be told from page content, since rgb(255,0,255) is
# `magenta`, `fuchsia` and `#f0f` at once, and a loose "high red, low green,
# high blue" test also swallows the pinks and violets a COLR palette test
# paints. A page that paints the marker's color in the corner then hides the
# marker for good. Two colors at a fixed spacing cannot be matched by a flat
# fill, and values off the primaries keep the small tolerance from being a
# range a page can paint inside.
#
# Vertical stripes, so the geometry is explicit. A conic gradient's quadrants
# pair at one corner only, and which corner that is depends on where the
# gradient starts, so an origin read from it can sit rows away from the true
# one. Stripes pair over the marker's whole height, making the first paired
# pixel its top left corner.
#
# The marker sits below the compared viewport, in a strip the window is laid
# out taller by and the saved frame is cropped of. That is what lets the frame
# that shows the marker be the frame that is kept, one grab a cell, and on a
# guest a grab is the scarce thing, since the session renders one window at a
# time at about 30 ms each however many browsers are asking. The strip is also
# where Firefox draws its network status panel, so a marker that shows through
# proves the panel has finished fading as well.
#
# The marker is never taken down, so the screen can still be showing the last
# page with the last marker when the next one is looked for. The stripes swap
# order from one cell to the next, and a grab looks for the order its own cell
# planted, so a leftover marker cannot pass for the new one.
STRIP = 16

# How long a page has to go without painting after a grab before the grab is
# kept. The pages that paint late for a stated reason are waited for by name
# in PAGE_DONE; this is for the rest, such as text whose font arrives after
# the layout that asked for it.
QUIET = 0.15
MARK = """
document.documentElement.style.scrollbarWidth = 'none';
const s = document.createElement('style');
s.id = '__dwc_no_scrollbars';
s.textContent = '*{scrollbar-width:none!important}';
document.documentElement.appendChild(s);
const d = document.createElement('div');
d.id = '__dwc_origin_mark';
const stripes = arguments[1] ? 'rgb(3,251,3) 0 50%,rgb(253,3,251) 50% 100%'
                             : 'rgb(253,3,251) 0 50%,rgb(3,251,3) 50% 100%';
d.style.cssText = 'all:initial;position:fixed;left:0;top:' + arguments[0] + 'px;'
                + 'width:8px;height:8px;z-index:2147483647;'
                + 'background:linear-gradient(90deg,' + stripes + ');';
document.documentElement.appendChild(d);
// A contained root clips its descendants to its own box, and a short page's
// root ends above the strip. Only such a root is made tall enough to reach it.
if (getComputedStyle(document.documentElement).contain !== 'none') {
  document.documentElement.style.minHeight = (arguments[0] + 16) + 'px';
}
// Keeps the time of the page's latest paint, for PAINTED_AFTER. The event
// carries no rectangle under WebRender, so paints are told apart by when
// they were reported.
if (!window.__dwc_on_paint) {
  window.__dwc_on_paint = () => { window.__dwc_last_paint = performance.now(); };
  window.addEventListener('MozAfterPaint', window.__dwc_on_paint);
}
return 1;
"""
UNMARK = """
const d = document.getElementById('__dwc_origin_mark');
if (d) { d.remove(); }
return 1;
"""

# The same as an async script that also waits for the page's images to
# decode and then out two animation frames, which is one round trip to the
# guest instead of three. The frames are not redundant with the screen poll
# below, because a page that draws from requestAnimationFrame has not drawn
# yet when the marker lands.
MARK_PAINTED = MARK.replace("return 1;", "") + """
const done = arguments[arguments.length - 1];
const decoded = Array.from(document.images)
    .filter(i => i.currentSrc)
    .map(i => i.decode().catch(() => {}));
Promise.all(decoded).then(() => {
  requestAnimationFrame(() => requestAnimationFrame(() => done(1)));
});
"""

# The page's clock, read right after a grab: every paint the grab could have
# shown was reported before this, so a paint reported after it is one the
# grab missed. PAINTED_AFTER asks that, given the stamp. Both need
# dom.send_after_paint_to_content, which the harness sets on both sides.
STAMP = "return performance.now();"
PAINTED_AFTER = "return (window.__dwc_last_paint || 0) > arguments[0];"



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
    sys.exit("backend must be x11:<display>, libvirt:<domain> "
             "or guest:<host>[:<port>]")


# What an unobstructed marker is worth to marker_mask, which is the magenta
# half of an 8x8 marker, 32 pixels. Half of that is the threshold, leaving room
# for an edge the capture softened without admitting a stray pair.
MARKER_PAIRS = 16

# Kept the same as compare_viewport.py's, which reads the same marker.
MARKER_A = (253, 3, 251)
MARKER_B = (3, 251, 3)
MARKER_TOLERANCE = 2


def marker_mask(frame, flipped=False):
    """Where the frame carries the checker, which is not where it is magenta.

    A magenta pixel counts only where green sits four columns along, that being
    the checker's period; `flipped` looks for the stripes the other way round.
    Matches compare_viewport.py's origin(), and a frame narrower than the
    period carries no marker by definition.
    """
    def near(rgb):
        return ((np.abs(frame[:, :, 0].astype(np.int16) - rgb[0]) <= MARKER_TOLERANCE)
                & (np.abs(frame[:, :, 1].astype(np.int16) - rgb[1]) <= MARKER_TOLERANCE)
                & (np.abs(frame[:, :, 2].astype(np.int16) - rgb[2]) <= MARKER_TOLERANCE))

    first, second = near(MARKER_A), near(MARKER_B)
    if flipped:
        first, second = second, first
    paired = first.copy()
    paired[:, :-4] &= second[:, 4:]
    paired[:, -4:] = False
    return paired


def marker_pixels(frame, flipped=False):
    return int(marker_mask(frame, flipped).sum())


def find_origin(browser, grabber, height, budget=30.0):
    """Where the viewport's top left corner sits on the screen.

    Done once for the plan. The window does not move between pages: the marker
    lands at the same place on all twelve corpus pages, wikipedia and canvas
    included.

    The search is for the marker's color anywhere on the screen, and a page is
    free to paint that color itself: `direction-upright-002.html` fills 2478
    pixels with `background: fuchsia`, which is exactly rgb(255,0,255). Left
    showing from an earlier run of the same plan it can take the corner the
    origin is read from. Navigating to about:blank first does not fix it and
    costs more than it saves, since the converged inner size is established
    before this runs and a navigation disturbs it.
    """
    browser.script(MARK, [height, False])
    browser.script_async(vp.PAINTED)
    deadline = time.time() + budget
    while time.time() < deadline:
        frame = grabber.grab()
        ys, xs = np.nonzero(marker_mask(frame))
        if len(ys) >= MARKER_PAIRS:
            browser.script(UNMARK, [])
            return int(xs.min()), int(ys.min()) - height
    browser.script(UNMARK, [])
    sys.exit("the origin marker never appeared on the screen")


def handshake_frame(browser, grabber, x, y, w, h, flipped, budget=2.0,
                    final=False):
    """The frame for one cell, once the screen proves it arrived.

    The marker is planted per page even though the origin is already known,
    because it is also the proof that the frame reached the screen. A
    script-level settle says the DOM and the scroll offset have stopped
    moving, and on a guest that regularly happens before anything is painted,
    so two identical blank frames would satisfy a wait for the screen to hold
    still. A frame that shows the marker was composed after the marker
    landed, which was after the settle, so the page in it is the settled page,
    and the marker is outside the part kept.

    Firefox draws its network status panel over the same corner during a
    navigation and fades it out afterwards, on the side whose page server is
    not loopback. The panel covers the marker while it is there, so the frame
    that shows the marker is one it has left.

    A frame that shows the marker can still be one of a page that was not
    done, if something painted after the grab. The page reports its own
    paints, so the grab is followed by a read of the page's clock and, QUIET
    seconds later, by the question whether anything painted after that
    reading. The wait comes after the grab, so a page that stays quiet costs
    one screen read and idle time, and only a page that painted again costs
    a second read. A page that never stops keeps its newest frame once the
    budget is spent.

    Two seconds is several times the slowest cell that does arrive; the pages
    that take longer never settle at all, on either machine. The pause between
    polls is for the other browsers on a guest, whose grabs queue behind this
    one's.

    A budget spent while the page is still painting is a frame of a page that
    was not done, so the caller is told and gives the cell a second budget.
    Under a full sweep every browser on a machine competes for it, and a page
    that misses one window makes the next. Only the final attempt keeps such
    a frame, which is what a page that never stops painting comes down to.
    """
    browser.script_async(MARK_PAINTED, [h, flipped])
    deadline = time.time() + budget
    while True:
        frame = grabber.grab(x, y, w, h + STRIP)
        if marker_pixels(frame[h:h + STRIP, :16], flipped) < MARKER_PAIRS:
            if time.time() > deadline:
                raise RuntimeError("the marked frame never reached the screen")
            time.sleep(0.01)
            continue
        stamp = browser.script(STAMP)
        time.sleep(QUIET)
        if not browser.script(PAINTED_AFTER, [stamp]):
            return frame[:h]
        if time.time() > deadline:
            if final:
                return frame[:h]
            raise RuntimeError("the page was still painting when the frame was taken")
        browser.script_async(vp.PAINTED)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("backend")
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("prefix")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("label")
    ap.add_argument("out_dir")
    ap.add_argument("cells")
    ap.add_argument("--css", default=None,
                    help="extra rule sheet, applied to both sides alike")
    args = ap.parse_args()

    browser = vp.open_browser("marionette:%s:%d" % (args.host, args.port))
    grabber = open_grabber(args.backend, args.port)
    vp.converge_inner_size(browser, args.width, args.height + STRIP,
                           vp.SETUP_TIMEOUT)
    vp.hide_scrollbars(browser)
    origin_x, origin_y = find_origin(browser, grabber, args.height)

    taken = 0
    with open(args.cells, encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 3:
                continue
            taken += 1
            index, path, scroll = parts[0], parts[1], int(parts[2])
            cell = os.path.join(args.out_dir, "cell" + index)
            os.makedirs(cell, exist_ok=True)
            out = os.path.join(cell, args.label + "_clean.png")
            # SystemExit included: the waits in viewport_protocol exit on a
            # timeout, which must not take the rest of the plan with it here.
            try:
                url = args.prefix.rstrip("/") + "/" + path.lstrip("/")
                started = time.time()
                browser.navigate(url)
                vp.await_condition(browser, vp.PAGE_LOADED, vp.WORK_TIMEOUT,
                                   "the page never finished loading")
                loaded = time.time()
                if args.css:
                    browser.script(vp.EXTRA_CSS, [args.css])
                    vp.await_condition(browser, vp.PAGE_LOADED, vp.WORK_TIMEOUT,
                                       "the page never settled after the extra css")
                vp.hide_scrollbars(browser)
                browser.script(vp.SETTLE, [scroll])
                vp.await_condition(browser, vp.VIEWPORT_READY, vp.WORK_TIMEOUT,
                                   "the viewport never settled after scrolling",
                                   [scroll])
                vp.wait_condition(browser, vp.PAGE_DONE, vp.DONE_TIMEOUT)
                vp.park_pointer(browser)
                # After the settle, so an animation that starts on load is
                # already running and gets caught. Both sides then photograph
                # the first frame instead of whatever phase each reached.
                vp.pin_animations(browser)
                settled = time.time()
                # A guest can stop composing for a few seconds at a time, and
                # every browser on it misses its budget together; one more
                # try is what tells that apart from a page that never paints.
                try:
                    frame = handshake_frame(browser, grabber, origin_x, origin_y,
                                            args.width, args.height,
                                            taken % 2 == 1)
                except RuntimeError:
                    frame = handshake_frame(browser, grabber, origin_x, origin_y,
                                            args.width, args.height,
                                            taken % 2 == 1, final=True)
                grabbed = time.time()
                # Through a temporary name, so a reader waiting for this
                # file never sees a partial one. The sweep compares cells as
                # soon as both sides' shots appear, and several comparisons
                # run at once, so the window between creating the file and
                # filling it is one a reader does reach.
                part = out + ".part"
                # Named, because the temporary has no .png on the end for
                # Image.save to infer the format from.
                Image.fromarray(frame).save(part, "PNG", compress_level=1)
                os.replace(part, out)
            except (Exception, SystemExit) as exc:
                print("fail %s %s" % (index, exc), flush=True)
                continue
            # Where the cell's time went, in milliseconds, so a slow sweep
            # can be read off its logs: the load, the settle, the capture
            # handshake, and the encode.
            print("ok %s %d %d %d %d" % (index, (loaded - started) * 1000,
                                         (settled - loaded) * 1000,
                                         (grabbed - settled) * 1000,
                                         (time.time() - grabbed) * 1000),
                  flush=True)


if __name__ == "__main__":
    sys.exit(main())
