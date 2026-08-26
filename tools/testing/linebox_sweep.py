#!/usr/bin/env python3
"""
Measure Firefox's own line boxes, over Marionette, on Linux or Windows.

    firefox --marionette --remote-allow-system-access -headless \
            -profile /tmp/p about:blank &
    tools/testing/linebox_sweep.py > linux.json
    tools/testing/linebox_sweep.py --families "Nirmala UI,Tahoma" --sizes 8-20
    tools/testing/linebox_sweep.py --compare linux.json windows.json [-v]
    tools/testing/linebox_sweep.py <host> <port> --advances "text" [--lang ja]
    tools/testing/linebox_sweep.py <host> <port> --probe <url> <scrollY> x,y [...] [--depth N] [--children]

This is the non-circular half of the line box work. `tools/testing/vmetrics.py` and
the shim both compute what Windows *should* produce; this asks the browser
what it actually laid out, and the same script runs against a Firefox in the
Windows 11 VM, so the two answers are comparable.

Per family and size it reports three numbers:

    box      the line box height with line-height:normal, which is what
             `GetNormalLineHeight` returns
    ascent   the baseline's offset from the top of that box, read off a
             zero-sized inline-block, which aligns its bottom to the baseline
    mwidth   the advance of "M", carried only as a guard: if the two
             platforms disagree here they did not use the same font, and the
             other two numbers are not comparable

--advances answers the question a pixel diff cannot: *where* in a run the two
machines stop agreeing. It reports the cumulative width after each character,
so diffing two of them names the glyph whose advance diverged - and everything
after it inherits a subpixel offset and rasterizes differently, which is what
a whole-line difference in a screenshot usually turns out to be. A run that
uses more than one font is exactly where this pays: the fallback boundary is a
likely place for the two to part company, and the character index says which
boundary it was.

Layout does not need a real display, so -headless is fine here - unlike the
rasterization comparison, which needs subpixel antialiasing and therefore an
X server.
"""

import json
import socket
import sys

# A default worth having, not a fixed set: --families and --sizes override it.
# The interesting families are the ones whose OS/2 usWin metrics differ from
# hhea, because that is where a reconstruction of DirectWrite's line box can be
# wrong while Arial and Times New Roman still agree.
FAMILIES = ["Segoe UI", "Malgun Gothic", "Microsoft YaHei", "Arial",
            "Yu Gothic", "Consolas"]
SIZES = list(range(8, 33))

# The CSS generics, which must reach the browser unquoted: '"monospace"' asks
# for a font *named* monospace, which nothing is.
GENERIC_NAMES = ("serif", "sans-serif", "monospace", "cursive", "fantasy",
                 "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace",
                 "ui-rounded", "math", "emoji", "fangsong")


def parse_sizes(text):
    """A comma-separated list, where "a-b" means the whole range.

    Fractional sizes are allowed and are worth asking for: CSS percentages
    produce them all day - 110% of 110% of 15px is 18.15px - and they are the
    sizes where the two platforms have the most room to disagree, because the
    FreeType side receives the size quantized to 1/64 px while DirectWrite
    receives it as a float. A range still steps by one.
    """
    out = []
    for part in text.split(","):
        part = part.strip()
        if "-" in part[1:]:                 # not a leading minus
            lo, hi = part[:1] + part[1:].split("-", 1)[0], part[1:].split("-", 1)[1]
            out.extend(range(int(lo), int(hi) + 1))
        elif part:
            out.append(float(part) if "." in part else int(part))
    return out

SCRIPT = r"""
const [families, sizes] = arguments;
const host = document.createElement("div");
host.style.cssText = "position:absolute;left:-9999px;top:0;width:2000px;";
document.body.appendChild(host);
const out = {};
for (const family of families) {
  out[family] = {};
  for (const size of sizes) {
    const line = document.createElement("div");
    // No fallback list: if the family is missing the guard below catches it
    // rather than a substitute quietly answering.
    //
    // The CSS generic keywords must not be quoted. '"system-ui"' is a request
    // for a font *named* system-ui, which nothing is, so it silently measures
    // the default font instead - and the generics are exactly the interesting
    // case, since which file they resolve to is what differs between machines.
    const GENERICS = ["serif", "sans-serif", "monospace", "cursive", "fantasy",
                      "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace",
                      "ui-rounded", "math", "emoji", "fangsong"];
    const spec = GENERICS.indexOf(family) >= 0 ? family : '"' + family + '"';
    line.style.cssText = 'font-family:' + spec + ';font-size:' + size +
                         'px;line-height:normal;white-space:nowrap;';
    const probe = document.createElement("span");
    probe.style.cssText = "display:inline-block;width:0;height:0;";
    line.appendChild(document.createTextNode("Hxg"));
    line.appendChild(probe);
    host.appendChild(line);

    const ruler = document.createElement("span");
    ruler.style.cssText = 'font-family:' + spec + ';font-size:' + size +
                          'px;white-space:pre;';
    ruler.textContent = "M";
    host.appendChild(ruler);

    const box = line.getBoundingClientRect();
    out[family][size] = [
      Math.round(box.height * 100) / 100,
      Math.round((probe.getBoundingClientRect().bottom - box.top) * 100) / 100,
      Math.round(ruler.getBoundingClientRect().width * 100) / 100,
    ];
    host.removeChild(line);
    host.removeChild(ruler);
  }
}
host.remove();
return out;
"""


ADVANCES = r"""
const [text, family, size, lang] = arguments;
const host = document.createElement("div");
host.style.cssText = "position:absolute;left:-9999px;top:0;width:4000px;";
const span = document.createElement("span");
span.style.cssText = 'font-family:' + family + ';font-size:' + size +
                     'px;white-space:pre;';
if (lang) { span.setAttribute("lang", lang); }
span.textContent = text;
host.appendChild(span);
document.body.appendChild(host);

// Cumulative width of each prefix, measured with a Range rather than by
// building one span per prefix: a Range measures the text as laid out in one
// go, so shaping and kerning across the whole run are preserved and the
// numbers add up to what is actually on screen.
const node = span.firstChild;
const out = [];
const range = document.createRange();
for (let i = 1; i <= text.length; i++) {
  range.setStart(node, 0);
  range.setEnd(node, i);
  out.push([text.codePointAt(i - 1),
            Math.round(range.getBoundingClientRect().width * 1000) / 1000]);
}
const total = span.getBoundingClientRect().width;
host.remove();
return {total: Math.round(total * 1000) / 1000, prefixes: out};
"""


class Marionette:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=60)
        self.buf = b""
        self.next_id = 1
        self.recv()

    def recv(self):
        while b":" not in self.buf:
            self.buf += self.sock.recv(65536)
        length, _, rest = self.buf.partition(b":")
        need = int(length)
        while len(rest) < need:
            rest += self.sock.recv(65536)
        self.buf = rest[need:]
        return json.loads(rest[:need])

    def call(self, name, params=None):
        message = json.dumps([0, self.next_id, name, params or {}]).encode()
        self.next_id += 1
        self.sock.sendall(b"%d:%s" % (len(message), message))
        reply = self.recv()
        if reply[2] is not None:
            raise RuntimeError("%s: %s" % (name, reply[2]))
        return reply[3]


def measure(host, port, families, sizes):
    m = Marionette(host, port)
    m.call("WebDriver:NewSession", {"capabilities": {}})
    m.call("Marionette:SetContext", {"value": "content"})
    m.call("WebDriver:Navigate", {"url": "data:text/html,<!doctype html><body>"})
    result = m.call("WebDriver:ExecuteScript",
                    {"script": SCRIPT, "args": [families, sizes]})
    return result["value"] if isinstance(result, dict) and "value" in result else result


PROBE = r"""
const [scrollY, points, depth, children] = arguments;
window.scrollTo({top: scrollY, left: 0, behavior: "instant"});
const out = [];
for (const [x, y] of points) {
  let el = document.elementFromPoint(x, y);
  if (!el) { out.push({point: [x, y], element: "none"}); continue; }
  // The chain, not just the innermost element: a height that disagrees is
  // usually contributed by one box and inherited by every ancestor above it,
  // and the chain says which one first differs.
  for (let level = 0; el && level <= depth; level++, el = el.parentElement) {
    const cs = getComputedStyle(el);
    const rect = el.getBoundingClientRect();
    out.push({
      point: [x, y],
      level: level,
      element: el.tagName + "." + (el.className || "").toString().slice(0, 24),
      rect: [Math.round(rect.x * 100) / 100, Math.round(rect.y * 100) / 100,
             Math.round(rect.width * 100) / 100, Math.round(rect.height * 100) / 100],
      font: cs.fontFamily.slice(0, 40),
      size: cs.fontSize,
      lineHeight: cs.lineHeight,
      lang: el.closest("[lang]") ? el.closest("[lang]").getAttribute("lang") : "",
      text: (el.textContent || "").trim().slice(0, 28),
    });
    if (children && level === depth) {
      // Every child box of the last element in the chain, plus its own
      // content edges. A parent that is taller than the sum of its children
      // has the difference in its own padding, its margins, or a line box -
      // and that is the distinction this makes visible.
      const cs2 = getComputedStyle(el);
      out.push({point: [x, y], level: level, element: "  (content edges)",
                rect: [cs2.paddingTop, cs2.paddingBottom, cs2.marginTop, cs2.marginBottom],
                lineHeight: cs2.lineHeight, text: cs2.display});
      for (const kid of el.children) {
        const kr = kid.getBoundingClientRect();
        const kcs = getComputedStyle(kid);
        out.push({
          point: [x, y], level: level + 1,
          element: "  " + kid.tagName + "." + (kid.className || "").toString().slice(0, 22),
          rect: [Math.round(kr.x * 100) / 100, Math.round(kr.y * 100) / 100,
                 Math.round(kr.width * 100) / 100, Math.round(kr.height * 100) / 100],
          lineHeight: kcs.lineHeight, size: kcs.fontSize,
          text: kcs.display + " " + (kid.textContent || "").trim().slice(0, 16),
        });
      }
    }
  }
}
return out;
"""


def probe(host, port, url, scroll, points, depth=0, children=False):
    """What is at these viewport points, and what line box did it get?

    A whole-page comparison says how much differs; run this on both machines at
    the row where they start to disagree and it says what is there. The rect
    height and the line-height are the two numbers that move when a font's
    vertical metrics are read differently.
    """
    m = Marionette(host, port)
    m.call("WebDriver:NewSession", {"capabilities": {}})
    m.call("Marionette:SetContext", {"value": "content"})
    m.call("WebDriver:Navigate", {"url": url})
    import time
    time.sleep(6)
    result = m.call("WebDriver:ExecuteScript",
                    {"script": PROBE, "args": [scroll, points, depth, children]})
    return result["value"] if isinstance(result, dict) and "value" in result else result


def compare(left_path, right_path, verbose=False):
    left = json.load(open(left_path))
    right = json.load(open(right_path))
    fields = ("box", "ascent", "mwidth")
    totals = [0, 0, 0]
    count = 0
    for family in left:
        if family not in right:
            print("%-18s missing on the other side" % family)
            continue
        hits = [0, 0, 0]
        misses = []
        sizes = sorted(set(left[family]) & set(right[family]), key=float)
        for size in sizes:
            for i in range(3):
                if left[family][size][i] == right[family][size][i]:
                    hits[i] += 1
            if left[family][size][:2] != right[family][size][:2]:
                misses.append((size, left[family][size], right[family][size]))
        for i in range(3):
            totals[i] += hits[i]
        count += len(sizes)
        note = "" if hits[2] == len(sizes) else "   <- different font, box/ascent not comparable"
        print("  %-18s %s%s" %
              (family,
               "  ".join("%s %2d/%d" % (fields[i], hits[i], len(sizes)) for i in range(3)),
               note))
        if verbose:
            for size, a, b in misses:
                print("      %3spx  box %-6s vs %-6s   ascent %-6s vs %-6s"
                      % (size, a[0], b[0], a[1], b[1]))
    print("  %-18s %s" % ("TOTAL",
                          "  ".join("%s %2d/%d" % (fields[i], totals[i], count)
                                    for i in range(3))))
    return 0 if totals[0] == count and totals[1] == count else 1


def show_advances(host, port, text, family, size, lang):
    m = Marionette(host, port)
    m.call("WebDriver:NewSession", {"capabilities": {}})
    m.call("Marionette:SetContext", {"value": "content"})
    m.call("WebDriver:Navigate", {"url": "data:text/html;charset=utf-8,"})
    result = m.call("WebDriver:ExecuteScript",
                    {"script": ADVANCES, "args": [text, family, size, lang]})
    if isinstance(result, dict) and "value" in result:
        result = result["value"]
    print("# %r in %s %gpx%s, total %s"
          % (text, family, size, (" lang=%s" % lang) if lang else "",
             result["total"]))
    previous = 0.0
    for index, (codepoint, width) in enumerate(result["prefixes"]):
        print("%3d  U+%04X  %-3s  cumulative %9.3f  advance %7.3f"
              % (index, codepoint,
                 chr(codepoint) if codepoint > 32 else " ",
                 width, width - previous))
        previous = width


def main():
    args = sys.argv[1:]
    verbose = "-v" in args or "--verbose" in args
    args = [a for a in args if a not in ("-v", "--verbose")]
    if "--advances" in args:
        i = args.index("--advances")
        text = args[i + 1]
        rest = args[:i] + args[i + 2:]
        lang = ""
        family = '"Courier New", monospace'
        size = 14.0
        for flag, setter in (("--lang", "lang"), ("--families", "family"),
                            ("--sizes", "size")):
            if flag in rest:
                j = rest.index(flag)
                value = rest[j + 1]
                rest = rest[:j] + rest[j + 2:]
                if setter == "lang":
                    lang = value
                elif setter == "family":
                    family = value if value in GENERIC_NAMES else '"%s"' % value
                else:
                    size = float(value)
        host = rest[0] if rest else "127.0.0.1"
        port = int(rest[1]) if len(rest) > 1 else 2828
        show_advances(host, port, text, family, size, lang)
        return

    if args and args[0] == "--compare":
        sys.exit(compare(args[1], args[2], verbose))
    if "--probe" in args:
        i = args.index("--probe")
        host, port = args[0], int(args[1])
        url, scroll = args[i + 1], int(args[i + 2])
        rest = args[i + 3:]
        depth = 0
        children = "--children" in rest
        rest = [r for r in rest if r != "--children"]
        if "--depth" in rest:
            j = rest.index("--depth")
            depth = int(rest[j + 1])
            rest = rest[:j] + rest[j + 2:]
        points = [[int(v) for v in p.split(",")] for p in rest]
        for row in probe(host, port, url, scroll, points, depth, children):
            print(json.dumps(row, sort_keys=True))
        return

    families, sizes = FAMILIES, SIZES
    positional = []
    while args:
        if args[0] == "--families":
            families = [f.strip() for f in args[1].split(",") if f.strip()]
            args = args[2:]
        elif args[0] == "--sizes":
            sizes = parse_sizes(args[1])
            args = args[2:]
        else:
            positional.append(args[0])
            args = args[1:]

    host = positional[0] if len(positional) > 0 else "127.0.0.1"
    port = int(positional[1]) if len(positional) > 1 else 2828
    json.dump(measure(host, port, families, sizes), sys.stdout, indent=1, sort_keys=True)
    print()


if __name__ == "__main__":
    main()


