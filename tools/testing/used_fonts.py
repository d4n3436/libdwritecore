#!/usr/bin/env python3
"""
Report which font faces actually drew each text node on a page.

    tools/testing/used_fonts.py --url file:///.../index.html
    tools/testing/used_fonts.py --host 127.0.0.1 --port 2929 --selector pre

Why this exists: a pixel comparison that says "98.4% equal" cannot say whether
the two machines picked different *fonts* or rasterized the same font
differently, and only the second is something a FreeType interceptor can do
anything about. This asks the layout engine directly.

The mechanism is InspectorUtils.getUsedFontFaces, the same call the DevTools
font panel makes. It reports the faces a Range was actually shaded with, after
every fallback step Firefox took to get there - so it answers for the whole
chain in gfxFontGroup::FindFontForChar: the fonts named by the page, then the
pref fonts (font.name-list.<generic>.<langgroup>, platform-independent code),
then gfxPlatform::GetCommonFallbackFonts (a per-platform compiled-in list),
then gfxPlatformFontList::GlobalFontFallback.

Only the pref-font step has a configuration surface, so a family that differs
between two machines is worth chasing exactly when the pref step could have
claimed the character and did not.

InspectorUtils is a chrome-only global, so the browser has to be started with
-remote-allow-system-access; the script itself runs in content context, where
Marionette's sandbox still carries the system principal.

Output is one line per text node, ordered as the document is, and is meant to
be diffed between two machines:

    tools/testing/used_fonts.py --port 2828 --url file:///... > linux.txt
    tools/testing/used_fonts.py --port 2929 --url https://.../ > windows.txt
    diff -u linux.txt windows.txt
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from marionette import Marionette

# Runs in the page. Walks text nodes in document order, and for each asks the
# inspector which faces shaded it. The 0 is maxRanges - the per-run ranges are
# not asked for, because they are far slower and --lines answers the same
# question by subdividing the node instead.
SCRIPT = r"""
var selector = arguments[0];
var limit = arguments[1];
var lines = arguments[2];
var want = arguments[3];
var roots = selector ? document.querySelectorAll(selector) : [document.body];
var out = [];
var seen = 0;
for (var r = 0; r < roots.length && out.length < limit; r++) {
  var walker = document.createTreeWalker(roots[r], NodeFilter.SHOW_TEXT, null);
  var node;
  while ((node = walker.nextNode()) && out.length < limit) {
    var text = node.nodeValue;
    if (!text || !/\S/.test(text)) {
      continue;
    }
    seen++;
    var range = document.createRange();
    range.selectNodeContents(node);
    var faces;
    try {
      faces = InspectorUtils.getUsedFontFaces(range, 0, true);
    } catch (e) {
      return {error: String(e)};
    }
    var names = [];
    for (var i = 0; i < faces.length; i++) {
      names.push(faces[i].name);
    }
    out.push({index: seen, text: text.replace(/\s+/g, " ").trim(),
              fonts: names});

    // One node can hold a whole document - the sampler pages put every script
    // in a single <pre> - and "these ten families drew it" then says nothing
    // about where. --lines subdivides the node at its newlines and asks again
    // per line, which is what turns "DejaVu Serif is in the list" into the one
    // line that used it.
    if (lines) {
      var at = 0;
      var pieces = text.split("\n");
      for (var p = 0; p < pieces.length; p++) {
        var start = at;
        at += pieces[p].length + 1;
        if (!/\S/.test(pieces[p])) { continue; }
        var sub = document.createRange();
        sub.setStart(node, start);
        sub.setEnd(node, start + pieces[p].length);
        var subFaces = InspectorUtils.getUsedFontFaces(sub, 0, true);
        var subNames = [];
        for (var k = 0; k < subFaces.length; k++) {
          subNames.push(subFaces[k].name);
        }
        if (want && subNames.indexOf(want) < 0) { continue; }
        out.push({index: seen, line: p + 1, text: pieces[p],
                  fonts: subNames});
      }
    }
    range.detach && range.detach();
  }
}
return {nodes: out, height: document.documentElement.scrollHeight,
        width: document.documentElement.scrollWidth};
"""


def excerpt(text, width):
    """A stable, printable prefix.

    Escaped to ASCII so the two sides diff cleanly whatever the terminals do,
    and so a line is never split by a character the other machine drew as a
    different width.
    """
    text = text[:width]
    return text.encode("unicode_escape").decode("ascii")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2828)
    ap.add_argument("--url", help="navigate here first; omit to use the "
                                  "page already loaded")
    ap.add_argument("--selector", help="restrict to text under this CSS "
                                       "selector (default: the whole body)")
    ap.add_argument("--limit", type=int, default=4000,
                    help="stop after this many text nodes (default 4000)")
    ap.add_argument("--chars", type=int, default=40,
                    help="characters of each node to print (default 40)")
    ap.add_argument("--lines", action="store_true",
                    help="also report each newline-separated line of a text "
                         "node separately. A sampler page is often one <pre> "
                         "holding every script it demonstrates, and the node's "
                         "own font list cannot say which line used which.")
    ap.add_argument("--which", metavar="FAMILY",
                    help="with --lines, print only the lines this family drew")
    ap.add_argument("--first-only", action="store_true",
                    help="print only the first face of each node, which is "
                         "the one that drew the run's leading characters")
    args = ap.parse_args()

    client = Marionette(args.host, args.port)
    client.start("content")
    if args.url:
        client.call("WebDriver:Navigate", {"url": args.url})

    result = client.script(SCRIPT,
                           [args.selector, args.limit, args.lines,
                            args.which],
                           sandbox="system")
    if result is None:
        sys.exit("no result; is the browser still alive?")
    if result.get("error"):
        sys.exit("InspectorUtils unavailable: %s\n"
                 "Start Firefox with -remote-allow-system-access."
                 % result["error"])

    nodes = result["nodes"]
    print("# %d text nodes, document %dx%d"
          % (len(nodes), result["width"], result["height"]))
    for node in nodes:
        fonts = node["fonts"][:1] if args.first_only else node["fonts"]
        where = ("%5d" % node["index"] if "line" not in node
                 else "%5d:%-4d" % (node["index"], node["line"]))
        print("%-11s %-*s  %s"
              % (where, args.chars, excerpt(node["text"], args.chars),
                 ", ".join(fonts) or "(none)"))


if __name__ == "__main__":
    main()
