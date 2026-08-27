#!/usr/bin/env python3
"""
Which font files a Chromium or Electron page actually rendered with.

    tools/testing/used_fonts_cdp.py <host> <port>

A pixel difference between two machines means nothing until both are shown to
have drawn with the same fonts, and a CSS family name does not settle that:
"Arial" resolves to Arial on Windows and, on a Linux box without it, to
whatever fontconfig substitutes. CSS.getPlatformFontsForNode reports what the
engine really used, per node, which is the only answer that settles it.

used_fonts.py does the same for Firefox over Marionette.
"""

import collections
import sys

import viewport_protocol as vp


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: used_fonts_cdp.py <host> <port>")
    browser = vp.CdpBrowser(sys.argv[1], int(sys.argv[2]))
    try:
        browser.call("DOM.enable")
        browser.call("CSS.enable")
        root = browser.call("DOM.getDocument", {"depth": -1})["root"]

        seen = collections.Counter()

        def walk(node):
            if node.get("nodeType") == 1:
                try:
                    fonts = browser.call("CSS.getPlatformFontsForNode",
                                         {"nodeId": node["nodeId"]})
                    for f in fonts.get("fonts", []):
                        seen[f.get("familyName", "?")] += f.get("glyphCount", 0)
                except RuntimeError:
                    pass
            for child in node.get("children", []):
                walk(child)

        walk(root)
        if not seen:
            print("no fonts reported; the page may have no text")
        for name, glyphs in sorted(seen.items(), key=lambda kv: -kv[1]):
            print("%-40s %d glyphs" % (name, glyphs))
    finally:
        browser.close()


if __name__ == "__main__":
    main()
