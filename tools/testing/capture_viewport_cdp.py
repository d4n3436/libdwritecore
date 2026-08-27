#!/usr/bin/env python3
"""
Put one Chromium or Electron into an exactly known state for a screenshot.

    tools/testing/capture_viewport_cdp.py <host> <port> <url> <width> <height> \
                                          <tag> [--scroll Y]

The Chromium-side counterpart of capture_viewport.py, which does the same for
Firefox over Marionette. Both take the page state, the size convergence and
the sentinel handshake from viewport_protocol.py, so the two sides of a
comparison are put into the same state by the same code and only the way the
browser is spoken to differs.

<port> is the browser's --remote-debugging-port. Start it with, for example:

    electron43 --remote-debugging-port=9222 /path/to/app

The handshake and what it is for are described in viewport_protocol.capture.
"""

import argparse
import sys

import viewport_protocol as vp


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("url")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("tag")
    ap.add_argument("--scroll", type=int, default=0)
    ap.add_argument("--connect-timeout", type=float, default=30.0)
    args = ap.parse_args()

    try:
        browser = vp.CdpBrowser(args.host, args.port, args.connect_timeout)
    except (RuntimeError, OSError) as exc:
        browser = None
        sys.exit("could not attach to DevTools on %s:%d: %s"
                 % (args.host, args.port, exc))
    try:
        vp.capture(browser, args.url, args.width, args.height, args.tag, args.scroll)
    finally:
        browser.close()


if __name__ == "__main__":
    main()
