#!/usr/bin/env python3
"""
Capture one Firefox side of a whole plan over a single Marionette connection.

    tools/testing/sweep_pages_marionette.py <backend> <host> <port> <prefix> \
                                            <width> <height> <label> \
                                            <out_dir> <cells> [--css RULES]

<cells> is a file with one cell per line, "<index> <path> <scroll>". Each
cell's frame goes to the comparison over --frames, exactly width by height,
and nothing is written down. The page is laid out STRIP pixels taller than
that, the same on every Marionette side, and the strip below the compared part
is where the capture marker goes. One line per cell goes to stdout, "ok
<index>" followed by the milliseconds the load, the settle, the capture and
the hand-over took, or "fail <index> <why>", so the caller can compare
finished cells while later ones are still being captured. This is the sibling of sweep_pages_cdp.py for a side whose
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

from screen_grab import open_grabber
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


def device(css, scale):
    """A length the page was given, in the pixels the screen is read in.

    The window is sized in CSS pixels and the frame is grabbed in device
    pixels, and above a device pixel ratio of one those are different numbers.
    Every length handed to the page stays CSS and every length that indexes a
    grabbed frame comes through here. At a ratio of one the two units coincide
    and nothing moves.
    """
    return int(round(css * scale))

# How long a page has to go without painting after a grab before the grab is
# kept. The pages that paint late for a stated reason are waited for by name
# in PAGE_DONE; this is for the rest, such as text whose font arrives after
# the layout that asked for it.
QUIET = 0.15
MARK = """
document.documentElement.style.scrollbarWidth = 'none';
// Same-origin frames as well as the top document. A frame whose content
// overflows keeps its own scrollbar, and that one is drawn by the platform:
// the Windows theme draws it and GTK draws the Linux one, so it lands in the
// capture as a strip of difference that has nothing to do with text. A
// cross-origin frame throws on the property access and is left alone, and so
// is one that is not loaded yet.
for (const f of Array.from(document.querySelectorAll('iframe, frame'))) {
  try {
    const d = f.contentDocument;
    if (!d || !d.documentElement) { continue; }
    d.documentElement.style.scrollbarWidth = 'none';
    if (!d.getElementById('__dwc_no_scrollbars')) {
      const fs = d.createElement('style');
      fs.id = '__dwc_no_scrollbars';
      fs.textContent = '*{scrollbar-width:none!important}';
      d.documentElement.appendChild(fs);
    }
  } catch (e) {}
}
const s = document.createElement('style');
s.id = '__dwc_no_scrollbars';
// The second rule refuses the marker a generated box. Its element name is
// one no page selects; this covers the pages that reach it through *::after,
// which would paint over the marker and fail the grab.
s.textContent = '*{scrollbar-width:none!important}'
              + '#__dwc_origin_mark::before,#__dwc_origin_mark::after'
              + '{content:none!important;display:none!important}';
document.documentElement.appendChild(s);
const d = document.createElement('dwc-origin-mark');
d.id = '__dwc_origin_mark';
const stripes = arguments[1] ? 'rgb(3,251,3) 0 50%,rgb(253,3,251) 50% 100%'
                             : 'rgb(253,3,251) 0 50%,rgb(3,251,3) 50% 100%';
d.style.cssText = 'all:initial;position:fixed;left:0;top:' + arguments[0] + 'px;'
                + 'width:8px;height:8px;z-index:2147483647;'
                + 'background:linear-gradient(90deg,' + stripes + ');';
// A page with an open dialog paints its backdrop over everything in the
// normal layer, whatever z-index the marker carries, and the marker is then
// invisible on both sides and the cell is never measured. A popover is in the
// top layer too, and the top layer paints in the order it was entered, so a
// marker shown last sits over a dialog opened earlier. `manual` closes nothing
// the page opened.
if (typeof d.showPopover === 'function') {
  d.setAttribute('popover', 'manual');
}
document.documentElement.appendChild(d);
if (d.hasAttribute('popover')) {
  try { d.showPopover(); } catch (e) {}
}
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





# What an unobstructed marker is worth to marker_mask, which is the magenta
# half of an 8x8 marker, 32 pixels. Half of that is the threshold, leaving room
# for an edge the capture softened without admitting a stray pair.
MARKER_PAIRS = 16

# Kept the same as compare_viewport.py's, which reads the same marker.
MARKER_A = (253, 3, 251)
MARKER_B = (3, 251, 3)
MARKER_TOLERANCE = 2


def marker_mask(frame, flipped=False, scale=1.0):
    """Where the frame carries the checker, which is not where it is magenta.

    A magenta pixel counts only where green sits the checker's period along;
    `flipped` looks for the stripes the other way round. Matches
    compare_viewport.py's origin(), and a frame narrower than the period
    carries no marker by definition.

    The period is half the marker, which is 8 CSS pixels, so it is four device
    columns only while the ratio is one. A scaled marker is wider and its
    halves meet further apart, and a period left at four then pairs magenta
    with magenta and finds no marker at all.
    """
    period = device(4, scale)
    def near(rgb):
        return ((np.abs(frame[:, :, 0].astype(np.int16) - rgb[0]) <= MARKER_TOLERANCE)
                & (np.abs(frame[:, :, 1].astype(np.int16) - rgb[1]) <= MARKER_TOLERANCE)
                & (np.abs(frame[:, :, 2].astype(np.int16) - rgb[2]) <= MARKER_TOLERANCE))

    first, second = near(MARKER_A), near(MARKER_B)
    if flipped:
        first, second = second, first
    paired = first.copy()
    paired[:, :-period] &= second[:, period:]
    paired[:, -period:] = False
    return paired


def marker_pixels(frame, flipped=False, scale=1.0):
    return int(marker_mask(frame, flipped, scale).sum())


def find_origin(browser, grabber, height, scale, budget=30.0):
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
        ys, xs = np.nonzero(marker_mask(frame, False, scale))
        if len(ys) >= MARKER_PAIRS:
            browser.script(UNMARK, [])
            return int(xs.min()), int(ys.min()) - device(height, scale)
    browser.script(UNMARK, [])
    raise SystemExit("the origin marker never appeared on the screen")


def handshake_frame(browser, grabber, x, y, w, h, scale, flipped,
                    budget=2.0, final=False):
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
    # The marker is planted at a CSS offset and looked for at a device one.
    dev_w, dev_h = device(w, scale), device(h, scale)
    dev_strip, dev_window = device(STRIP, scale), device(16, scale)
    deadline = time.time() + budget
    while True:
        frame = grabber.grab(x, y, dev_w, dev_h + dev_strip)
        if marker_pixels(frame[dev_h:dev_h + dev_strip, :dev_window],
                         flipped, scale) < MARKER_PAIRS:
            if time.time() > deadline:
                raise RuntimeError("the marked frame never reached the screen")
            time.sleep(0.01)
            continue
        stamp = browser.script(STAMP)
        time.sleep(QUIET)
        if not browser.script(PAINTED_AFTER, [stamp]):
            return frame[:dev_h]
        if time.time() > deadline:
            if final:
                return frame[:dev_h]
            raise RuntimeError("the page was still painting when the frame was taken")
        browser.script_async(vp.PAINTED)


# A frame goes to the comparison pool over a socket, and the reply is what
# paces the sweep. Waiting for it holds a side that has run ahead until its
# frames have been paired, so the skew between the two sides costs a bounded
# number of frames instead of one per cell.
def open_sender(path, deadline=60.0):
    import socket
    # The pool is started once the caller knows how many sweepers there are,
    # which is after the sweepers themselves, so the socket can be a moment
    # behind this.
    end = time.time() + deadline
    while True:
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            conn.connect(path)
            return conn.makefile("rwb")
        except OSError:
            conn.close()
            if time.time() > end:
                raise
            time.sleep(0.1)


def send_frame(stream, index, label, labels, frame, out, clusters, tag, wanted):
    import json
    pixels = np.ascontiguousarray(frame[:, :, :3], dtype=np.uint8)
    head = {"cell": int(index), "label": label, "labels": labels,
            "h": int(pixels.shape[0]), "w": int(pixels.shape[1]),
            "out": out, "clusters": clusters, "tag": tag, "wanted": wanted}
    stream.write((json.dumps(head) + "\n").encode("utf-8"))
    stream.write(pixels.tobytes())
    stream.flush()
    if not stream.readline():
        raise RuntimeError("the comparison pool closed before the frame landed")


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
    ap.add_argument("--scheme", default=None, choices=("light", "dark"),
                    help="the prefers-color-scheme this side is expected to "
                         "report. Refused when it does not, since the scheme "
                         "is set when the browser starts and nothing in a "
                         "plan otherwise says which one a run measured")
    ap.add_argument("--frames", required=True,
                    help="a unix socket to send the captured frames to. The "
                         "pair is compared in memory there and no capture is "
                         "written down.")
    ap.add_argument("--labels", default=None,
                    help="both sides' labels, comma separated and in the "
                         "order the comparison reports them")
    ap.add_argument("--clusters", type=int, default=0,
                    help="how many differing-pixel clusters to describe")
    ap.add_argument("--scale", type=float, default=None,
                    help="the device pixel ratio this side is expected to be "
                         "at. The sweep refuses a browser that is not, since a "
                         "side that quietly ignored the scale still captures, "
                         "still compares and reports a difference that is the "
                         "scale rather than the shim")
    args = ap.parse_args()

    browser = vp.open_browser("marionette:%s:%d" % (args.host, args.port))
    grabber = open_grabber(args.backend, args.port)
    labels = args.labels.split(",") if args.labels else [args.label, args.label]
    vp.converge_inner_size(browser, args.width, args.height + STRIP,
                           vp.SETUP_TIMEOUT)
    vp.hide_scrollbars(browser)
    # Asked of the browser instead of taken from the caller, so the number the
    # frame is measured with is the one the page is actually being drawn at.
    # Reported so a sweep whose two sides disagree can be told from one whose
    # sides agree on the wrong value.
    scale = float(browser.script("return window.devicePixelRatio;"))
    print("scale %g" % scale, file=sys.stderr, flush=True)
    # What this side converged to, so compare_pages.sh can hold the two to
    # the same viewport.
    vp_dw, vp_dh, vp_aw, vp_ah = browser.script(vp.VIEWPORT_UNITS)
    print("viewport %dx%d device %dx%d appunits"
          % (vp_dw, vp_dh, vp_aw, vp_ah), file=sys.stderr, flush=True)
    # Connected here, once the viewport has been reported and the caller has
    # started the comparison on the strength of it. Every sweeper connects
    # whether or not it has a frame to send, since the pool waits for all of
    # them before it decides the plan is over.
    sender = open_sender(args.frames)
    if args.scheme is not None:
        seen = browser.script("return matchMedia('(prefers-color-scheme: dark)')"
                              ".matches ? 'dark' : 'light';")
        print("scheme %s" % seen, file=sys.stderr, flush=True)
        if seen != args.scheme:
            sys.exit("this side reports a %s color scheme and the plan asks "
                     "for %s; the profile pref never reached the browser"
                     % (seen, args.scheme))
    if args.scale is not None and abs(scale - args.scale) > 1e-6:
        sys.exit("this side is at a device pixel ratio of %g and the plan asks "
                 "for %g; the profile pref never reached the browser"
                 % (scale, args.scale))
    origin_x, origin_y = find_origin(browser, grabber, args.height, scale)

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
            # SystemExit included: the waits in viewport_protocol exit on a
            # timeout, which must not take the rest of the plan with it here.
            try:
                url = args.prefix.rstrip("/") + "/" + path.lstrip("/")
                started = time.time()
                browser.navigate(url)
                vp.await_condition(browser, vp.PAGE_LOADED, vp.WORK_TIMEOUT,
                                   "the page never finished loading")
                # A page whose script measures text during parsing gets the
                # fallback face while its web font is still arriving, and the
                # font is only in the cache once a load has fetched it. The
                # side that is never restarted is always past that first load,
                # so the two sides would be comparing different layouts. Load
                # it again where a font came over the network, which is where
                # the first answer could have been the fallback one.
                if browser.script(vp.FONT_OVER_NETWORK):
                    browser.navigate(url)
                    vp.await_condition(browser, vp.PAGE_LOADED, vp.WORK_TIMEOUT,
                                       "the page never finished its second load")
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
                                            args.width, args.height, scale,
                                            taken % 2 == 1)
                except RuntimeError:
                    frame = handshake_frame(browser, grabber, origin_x, origin_y,
                                            args.width, args.height, scale,
                                            taken % 2 == 1, final=True)
                grabbed = time.time()
                send_frame(sender, index, args.label, labels, frame,
                           os.path.join(cell, "out"),
                           os.path.join(cell, "clusters.tsv"),
                           "%s %d" % (path, scroll), args.clusters)
            except (Exception, SystemExit) as exc:
                print("fail %s %s" % (index, exc), flush=True)
                # Nothing else marks a cell this side never delivered, and the
                # caller waits on the mark.
                open(os.path.join(cell, "failed"), "a").close()
                continue
            # Where the cell's time went, in milliseconds, so a slow sweep
            # can be read off its logs: the load, the settle, the capture
            # handshake, and handing the frame over.
            print("ok %s %d %d %d %d" % (index, (loaded - started) * 1000,
                                         (settled - loaded) * 1000,
                                         (grabbed - settled) * 1000,
                                         (time.time() - grabbed) * 1000),
                  flush=True)

    # Closing is what tells the pool this side is done; it waits for every
    # sweeper to have connected and then gone.
    sender.close()


if __name__ == "__main__":
    sys.exit(main())
