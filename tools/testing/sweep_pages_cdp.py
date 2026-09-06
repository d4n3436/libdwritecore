#!/usr/bin/env python3
"""
Capture one side of a whole plan over a single DevTools connection.

    tools/testing/sweep_pages_cdp.py <host> <port> <prefix> <width> <height> \
                                     <label> <out_dir> <cells> [--css RULES]

<cells> is a file with one cell per line, "<index> <path> <scroll>". Each cell
becomes <out_dir>/cell<index>/<label>_clean.png, exactly the viewport, taken
with Page.captureScreenshot through viewport_protocol.capture_direct.

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
    if args.scheme is not None:
        seen = browser.script("return matchMedia('(prefers-color-scheme: dark)')"
                              ".matches ? 'dark' : 'light';")
        print("scheme %s" % seen, file=sys.stderr, flush=True)
        if seen != args.scheme:
            sys.exit("this side reports a %s color scheme and the plan asks "
                     "for %s; set DWC_COLOR_SCHEME before starting it"
                     % (seen, args.scheme))
    try:
        with open(args.cells, encoding="utf-8") as handle:
            for line in handle:
                parts = line.split()
                if len(parts) != 3:
                    continue
                index, path, scroll = parts[0], parts[1], int(parts[2])
                cell = os.path.join(args.out_dir, "cell" + index)
                os.makedirs(cell, exist_ok=True)
                out = os.path.join(cell, args.label + "_clean.png")
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
                        vp.capture_direct(browser, args.prefix + "/" + path,
                                          args.width, args.height, out,
                                          scroll=scroll, css=args.css)
                        print("ok " + index, flush=True)
                        break
                    except (Exception, SystemExit) as why:  # noqa: BLE001
                        if attempt == 1:
                            print("fail %s %s" % (index, why), flush=True)
                            break
                        try:
                            browser.close()
                        except Exception:  # noqa: BLE001
                            pass
                        time.sleep(2)
                        try:
                            browser = connect()
                        except (Exception, SystemExit) as again:  # noqa: BLE001
                            print("fail %s reconnect: %s" % (index, again),
                                  flush=True)
                            break
    finally:
        browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
