#!/usr/bin/env python3
"""
Put one browser into an exactly known state and hold it there for a screenshot.

    tools/testing/capture_viewport.py <host> <port> <url> <width> <height> \
                                      <tag> [--driver marionette|cdp] \
                                      [--wait S] [--scroll Y] [--css RULES]

--driver marionette drives Firefox, --driver cdp drives Chromium and Electron
over the DevTools protocol. Both sides of a comparison are put into the same
state by the same code in viewport_protocol.py; only the way the browser is
spoken to differs.

The size, the page state and the handshake are all viewport_protocol's:

  * the *inner* size is converged, not the window size. Window chrome differs
    between platforms and between window managers, so two machines told to use
    the same window size render different amounts of page.
  * the page is waited for on observables, never on a fixed sleep. Every wait
    stands on something the browser reports: the window holding the size, the
    fonts having loaded, the marker reaching the screen.
  * an eight-pixel magenta square is drawn at the viewport origin, "<tag>.marked"
    is written, and the caller is left to take its own screenshot of whatever
    it can photograph - a whole screen, a virtual framebuffer, a guest's
    display. Deleting the sentinel removes the marker and produces
    "<tag>.clean", which is the shot that gets compared, so the marker is never
    part of what is measured.

capture_viewport.sh is the caller that takes those two shots.
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
    ap.add_argument("--driver", default="marionette", choices=("marionette", "cdp"),
                    help="how to speak to the browser (default marionette)")
    ap.add_argument("--scroll", type=int, default=0)
    ap.add_argument("--css", default=None,
                    help="extra rule sheet, applied to both sides alike")
    ap.add_argument("--wait", type=float, default=vp.WORK_TIMEOUT,
                    help="how long the page itself is given, in seconds")
    ap.add_argument("--connect-timeout", type=float, default=30.0)
    args = ap.parse_args()

    spec = "%s:%s:%d" % (args.driver, args.host, args.port)
    try:
        browser = vp.open_browser(spec, args.connect_timeout)
    except (RuntimeError, OSError) as exc:
        sys.exit("could not attach to %s on %s:%d: %s"
                 % (args.driver, args.host, args.port, exc))
    try:
        vp.capture(browser, args.url, args.width, args.height, args.tag,
                   args.scroll, deadline=args.wait, css=args.css)
    finally:
        browser.close()


if __name__ == "__main__":
    main()
