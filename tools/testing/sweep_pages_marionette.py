#!/usr/bin/env python3
"""
Capture one Firefox side of a whole plan over a single Marionette connection.

    tools/testing/sweep_pages_marionette.py <backend> <host> <port> <prefix> \
                                            <width> <height> <label> \
                                            <out_dir> <cells> [--css RULES]

<cells> is a file with one cell per line, "<index> <path> <scroll>". Each cell
becomes <out_dir>/cell<index>/<label>_clean.png, exactly the viewport. One line
per cell goes to stdout, "ok <index>" or "fail <index> <why>", so the caller can
compare finished cells while later ones are still being captured. This is the
sibling of sweep_pages_cdp.py for a side whose driver is marionette.

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

`backend` is x11:<display> or libvirt:<domain>, as in capture_viewport.sh. A
guest is grabbed with `virsh screenshot`, which takes the whole display and has
no cheaper form, so only an X11 side gets the fast path.
"""

import argparse
import json
import os
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
MARK = """
document.documentElement.style.scrollbarWidth = 'none';
const d = document.createElement('div');
d.id = '__dwc_origin_mark';
d.style.cssText = 'all:initial;position:fixed;left:0;top:0;width:8px;height:8px;'
                + 'z-index:2147483647;'
                + 'background:linear-gradient(90deg,'
                + 'rgb(253,3,251) 0 50%,rgb(3,251,3) 50% 100%);';
document.documentElement.appendChild(d);
return 1;
"""
UNMARK = """
const d = document.getElementById('__dwc_origin_mark');
if (d) { d.remove(); }
return 1;
"""

# The same two as async scripts that also wait out two animation frames, which
# is one round trip to the guest instead of two. The wait is not redundant with
# the screen poll below, because a page that draws from requestAnimationFrame
# has not drawn yet when the marker goes.
MARK_PAINTED = MARK.replace("return 1;", "") + """
const done = arguments[arguments.length - 1];
requestAnimationFrame(() => requestAnimationFrame(() => done(1)));
"""
UNMARK_PAINTED = UNMARK.replace("return 1;", "") + """
const done = arguments[arguments.length - 1];
requestAnimationFrame(() => requestAnimationFrame(() => done(1)));
"""



class X11Grabber:
    """The root window of one X display, read straight into memory."""

    def __init__(self, spec):
        from Xlib import display, X
        self._X = X
        self._display = display.Display(spec)
        self._root = self._display.screen().root
        geometry = self._root.get_geometry()
        self.width, self.height = geometry.width, geometry.height

    def grab(self, x=0, y=0, w=None, h=None):
        w = self.width if w is None else w
        h = self.height if h is None else h
        raw = self._root.get_image(x, y, w, h, self._X.ZPixmap, 0xFFFFFFFF)
        flat = np.frombuffer(raw.data, dtype=np.uint8)
        return flat.reshape(h, w, 4)[:, :, 2::-1]


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


def open_grabber(backend):
    kind, _, rest = backend.partition(":")
    if kind == "x11":
        return X11Grabber(rest)
    if kind == "libvirt":
        return LibvirtGrabber(rest)
    sys.exit("backend must be x11:<display> or libvirt:<domain>")


# What an unobstructed marker is worth to marker_mask, which is the magenta
# half of an 8x8 marker, 32 pixels. Half of that is the threshold, leaving room
# for an edge the capture softened without admitting a stray pair.
MARKER_PAIRS = 16

# Kept the same as compare_viewport.py's, which reads the same marker.
MARKER_A = (253, 3, 251)
MARKER_B = (3, 251, 3)
MARKER_TOLERANCE = 2


def marker_mask(frame):
    """Where the frame carries the checker, which is not where it is magenta.

    A magenta pixel counts only where green sits four columns along, that being
    the checker's period. Matches compare_viewport.py's origin(), and a frame
    narrower than the period carries no marker by definition.
    """
    def near(rgb):
        return ((np.abs(frame[:, :, 0].astype(np.int16) - rgb[0]) <= MARKER_TOLERANCE)
                & (np.abs(frame[:, :, 1].astype(np.int16) - rgb[1]) <= MARKER_TOLERANCE)
                & (np.abs(frame[:, :, 2].astype(np.int16) - rgb[2]) <= MARKER_TOLERANCE))

    magenta, green = near(MARKER_A), near(MARKER_B)
    paired = magenta.copy()
    paired[:, :-4] &= green[:, 4:]
    paired[:, -4:] = False
    return paired


def marker_pixels(frame):
    return int(marker_mask(frame).sum())


def find_origin(browser, grabber, budget=30.0):
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
    browser.script(MARK, [])
    browser.script_async(vp.PAINTED)
    deadline = time.time() + budget
    while time.time() < deadline:
        frame = grabber.grab()
        ys, xs = np.nonzero(marker_mask(frame))
        if len(ys) >= MARKER_PAIRS:
            browser.script(UNMARK, [])
            return int(xs.min()), int(ys.min())
    browser.script(UNMARK, [])
    sys.exit("the origin marker never appeared on the screen")


def handshake_frame(browser, grabber, x, y, w, h, budget=2.0):
    """The frame for one cell, once the screen proves it arrived.

    The marker is planted per page even though the origin is already known,
    because it is also the proof that the frame reached the screen. A
    script-level settle says the DOM and the scroll offset have stopped moving,
    and on a guest that regularly happens before anything is painted: waiting
    instead for two identical frames took the corpus to 4 of 12 exact, with
    wikipedia at 30.8% against a Linux side that had painted, since two
    identical *blank* frames satisfy a settle. Seeing the marker arrive proves
    one paint and seeing it go proves the next.

    What is cheap now is the grab, so on X11 this costs two reads of memory
    where it used to cost two whole-screen photographs and two PNG encodes.

    Two seconds, because the distribution is bimodal with two orders of
    magnitude of clear air in it. Over 727 timed cells: median 301 ms, p90 401,
    p99 503, then nothing whatever between 1 s and 14 s, then five cells at the
    budget itself. So two seconds is four times p99 and can cut off nothing
    legitimate, while the fifteen it replaces cost about nine minutes of a
    thirty-six minute sweep on the pages that never settle - all of them COLR
    font-palette tests, and they fail on both machines alike.
    """
    browser.script_async(MARK_PAINTED)
    deadline = time.time() + budget
    while marker_pixels(grabber.grab(x, y, 16, 16)) < MARKER_PAIRS:
        if time.time() > deadline:
            raise RuntimeError("the marked frame never reached the screen")
    browser.script_async(UNMARK_PAINTED)
    deadline = time.time() + budget
    while True:
        frame = grabber.grab(x, y, w, h)
        if marker_pixels(frame[:16, :16]) == 0:
            break
        if time.time() > deadline:
            raise RuntimeError("the marker never left the screen")

    # The marker proves the page painted, and it cannot prove anything about
    # the browser's own chrome, which is drawn over the same rectangle. Firefox
    # puts its network status panel across the bottom left corner during a
    # navigation and fades it out afterwards, so a capture taken as soon as the
    # page settles catches whatever is left of the fade. It reads as a font
    # difference on any page with ink down there, and only on the side whose
    # page server is not loopback, which is the guest.
    #
    # One more grab, and the frame is only accepted once two in a row agree.
    # This is not a settle condition on its own, since two identical blank
    # frames satisfy it while nothing has been painted. It runs after the
    # marker has already proved the paint.
    while True:
        again = grabber.grab(x, y, w, h)
        if (again == frame).all():
            return frame
        frame = again
        if time.time() > deadline:
            return frame


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
    grabber = open_grabber(args.backend)
    vp.converge_inner_size(browser, args.width, args.height, vp.SETUP_TIMEOUT)
    vp.hide_scrollbars(browser)
    origin_x, origin_y = find_origin(browser, grabber)

    with open(args.cells, encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 3:
                continue
            index, path, scroll = parts[0], parts[1], int(parts[2])
            cell = os.path.join(args.out_dir, "cell" + index)
            os.makedirs(cell, exist_ok=True)
            out = os.path.join(cell, args.label + "_clean.png")
            # SystemExit included: the waits in viewport_protocol exit on a
            # timeout, which must not take the rest of the plan with it here.
            try:
                url = args.prefix.rstrip("/") + "/" + path.lstrip("/")
                browser.navigate(url)
                vp.await_condition(browser, vp.PAGE_LOADED, vp.WORK_TIMEOUT,
                                   "the page never finished loading")
                if args.css:
                    browser.script(vp.EXTRA_CSS, [args.css])
                    vp.await_condition(browser, vp.PAGE_LOADED, vp.WORK_TIMEOUT,
                                       "the page never settled after the extra css")
                vp.hide_scrollbars(browser)
                browser.script(vp.SETTLE, [scroll])
                vp.await_condition(browser, vp.VIEWPORT_READY, vp.WORK_TIMEOUT,
                                   "the viewport never settled after scrolling",
                                   [scroll])
                vp.park_pointer(browser)
                frame = handshake_frame(browser, grabber, origin_x, origin_y,
                                        args.width, args.height)
                Image.fromarray(frame).save(out, compress_level=1)
            except (Exception, SystemExit) as exc:
                print("fail %s %s" % (index, exc), flush=True)
                continue
            print("ok %s" % index, flush=True)


if __name__ == "__main__":
    sys.exit(main())
