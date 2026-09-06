#!/usr/bin/env python3
"""
Capture one side of a whole plan over a single DevTools connection.

    tools/testing/sweep_pages_cdp.py <host> <port> <prefix> <width> <height> \
                                     <label> <out_dir> <cells> [--css RULES]

<cells> is a file with one cell per line, "<index> <path> <scroll>". Each cell
is exactly the viewport, taken with Page.captureScreenshot through
viewport_protocol.capture_direct. The pixels go to the comparison pool over
--frames and nothing is written down.

Connecting, attaching and sizing the window happen once, here, instead of once
per page, which is what makes a sweep cost its page loads and nothing else.
One line per cell goes to stdout, "ok <index>" or "fail <index> <why>", so the
caller can compare finished cells while later ones are still being captured.
"""

import argparse
import os
import sys
import time

import viewport_protocol as vp


# Mirrors open_sender and send_frame in sweep_pages_marionette.py; the wire
# format is compare_viewport.py --serve-frames.
#
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


# Page.captureScreenshot answers PNG bytes, which the stability check needs
# encoded anyway, so the decode the pool does for a file happens here instead.
# A page painting no background of its own arrives transparent, and compositing
# it over white is what crop() does on the way in from a file; without it the
# alpha resolves to black and the cell differs everywhere.
def to_frame(data):
    import io as _io

    import numpy as np
    from PIL import Image
    shot = Image.open(_io.BytesIO(data))
    if shot.mode in ("RGBA", "LA") or "transparency" in shot.info:
        ground = Image.new("RGBA", shot.size, (255, 255, 255, 255))
        shot = Image.alpha_composite(ground, shot.convert("RGBA"))
    return np.asarray(shot.convert("RGB"))


def send_frame(stream, index, label, labels, frame, out, clusters, tag, wanted):
    import json

    import numpy as np
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
    ap.add_argument("--frames", required=True,
                    help="a unix socket to send the captured frames to. The "
                         "pair is compared in memory there and no capture is "
                         "written down.")
    ap.add_argument("--labels", default=None,
                    help="both sides' labels, comma separated and in the "
                         "order the comparison reports them")
    ap.add_argument("--clusters", type=int, default=0,
                    help="how many differing-pixel clusters to describe")
    ap.add_argument("--scheme", default=None, choices=("light", "dark"),
                    help="the prefers-color-scheme this side is expected to "
                         "report, checked after pin_color_scheme has set it")
    ap.add_argument("--scale", type=float, default=None,
                    help="the device pixel ratio this side is expected to be "
                         "at. Only one is honored here, since this route pins "
                         "deviceScaleFactor to it in force_viewport, and a "
                         "plan asking for another is refused rather than swept "
                         "at the wrong one")
    args = ap.parse_args()

    if args.scale is not None and abs(args.scale - 1.0) > 1e-6:
        sys.exit("this side captures over DevTools, which is pinned to a "
                 "device pixel ratio of 1, and the plan asks for %g. The "
                 "scaled route exists on the Marionette sides only."
                 % args.scale)

    def connect():
        b = vp.CdpBrowser(args.host, args.port)
        vp.converge_inner_size(b, args.width, args.height, vp.SETUP_TIMEOUT)
        vp.hide_scrollbars(b)
        vp.pin_color_scheme(b)
        vp.block_offserver(b, args.prefix)
        return b

    browser = connect()
    # What this side converged to, so compare_pages.sh can hold the two to the
    # same viewport. It waits for this line from both sides before it captures
    # anything, so a sweeper that never prints it stops the plan.
    vp_dw, vp_dh, vp_aw, vp_ah = browser.script(vp.VIEWPORT_UNITS)
    print("viewport %dx%d device %dx%d appunits"
          % (vp_dw, vp_dh, vp_aw, vp_ah), file=sys.stderr, flush=True)
    if args.scheme is not None:
        seen = browser.script("return matchMedia('(prefers-color-scheme: dark)')"
                              ".matches ? 'dark' : 'light';")
        print("scheme %s" % seen, file=sys.stderr, flush=True)
        if seen != args.scheme:
            sys.exit("this side reports a %s color scheme and the plan asks "
                     "for %s; set DWC_COLOR_SCHEME before starting it"
                     % (seen, args.scheme))
    # Connected here, once the viewport has been reported and the caller has
    # started the comparison on the strength of it. Every sweeper connects
    # whether or not it has a frame to send, since the pool waits for all of
    # them before it decides the plan is over.
    sender = open_sender(args.frames)
    labels = args.labels.split(",") if args.labels else [args.label, args.label]
    try:
        with open(args.cells, encoding="utf-8") as handle:
            for line in handle:
                parts = line.split()
                if len(parts) != 3:
                    continue
                index, path, scroll = parts[0], parts[1], int(parts[2])
                cell = os.path.join(args.out_dir, "cell" + index)
                os.makedirs(cell, exist_ok=True)
                shot = None
                why = None
                # SystemExit included: the waits in viewport_protocol exit on
                # a timeout, which stops a per-page driver cleanly but must
                # not take the rest of the plan with it here. A scroll offset
                # past the page's height, for one, times out that way.
                # A cell that fails gets one more try on a fresh connection:
                # a renderer that died takes its DevTools socket with it, and
                # the app reloads the page target when that happens, so
                # reconnecting is what recovery looks like from here.
                for attempt in (0, 1):
                    try:
                        shot = vp.capture_direct(
                            browser, args.prefix + "/" + path, args.width,
                            args.height, scroll=scroll, css=args.css)
                        why = None
                        break
                    except (Exception, SystemExit) as failure:  # noqa: BLE001
                        why = failure
                        if attempt == 1:
                            break
                        try:
                            browser.close()
                        except Exception:  # noqa: BLE001
                            pass
                        time.sleep(2)
                        try:
                            browser = connect()
                        except (Exception, SystemExit) as again:  # noqa: BLE001
                            why = "reconnect: %s" % again
                            break
                # Handing the frame over sits outside the retry, so a cell that
                # was captured twice is still only sent once. A send that fails
                # is the pool being gone, which a retry cannot mend.
                if why is None:
                    try:
                        send_frame(sender, index, args.label, labels,
                                   to_frame(shot), os.path.join(cell, "out"),
                                   os.path.join(cell, "clusters.tsv"),
                                   "%s %d" % (path, scroll), args.clusters)
                    except Exception as failure:  # noqa: BLE001
                        why = failure
                if why is not None:
                    print("fail %s %s" % (index, why), flush=True)
                    # Nothing else marks a cell this side never delivered, and
                    # the caller waits on the mark.
                    if sender is not None:
                        open(os.path.join(cell, "failed"), "a").close()
                    continue
                print("ok " + index, flush=True)
    finally:
        # Closed so the pool knows this sweeper's share is done; it ends once
        # every sender has connected and then closed.
        if sender is not None:
            try:
                sender.close()
            except Exception:  # noqa: BLE001
                pass
        browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
