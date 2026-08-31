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
    args = ap.parse_args()

    browser = vp.CdpBrowser(args.host, args.port)
    try:
        vp.converge_inner_size(browser, args.width, args.height, vp.SETUP_TIMEOUT)
        vp.hide_scrollbars(browser)
        vp.pin_color_scheme(browser)

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
                try:
                    vp.capture_direct(browser, args.prefix + "/" + path,
                                      args.width, args.height, out,
                                      scroll=scroll, css=args.css)
                    print("ok " + index, flush=True)
                except (Exception, SystemExit) as why:  # noqa: BLE001
                    print("fail %s %s" % (index, why), flush=True)
    finally:
        browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
