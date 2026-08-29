#!/usr/bin/env python3
"""
Generate comparison pages from Chromium's own CSS property database.

    tools/testing/gen_css_matrix.py <chromium-src> <out-dir>

Reads third_party/blink/renderer/core/css/css_properties.json5, keeps every
property whose name touches text or fonts, and writes one page per property
that sweeps the property's own keywords over a multi-script sample line. The
input space comes from the source, so a new keyword in a rebased checkout
lands in the sweep without anyone curating it.

Keywords come from the property's `keywords` list where the database has one;
properties without one get a hand-kept value list below, and a property in
neither is listed in the page for a reader to notice, unswept.

The pages land in <out-dir>/css-matrix-<n>/index.html with 24 properties per
page, and a plan body for them goes to stdout.
"""

import json
import os
import re
import sys

SAMPLE = ("Wiki 013 fi ffl AV To النص אב "
          "漢字かな 한글 ไทย "
          "क्षि")

PATTERN = re.compile(
    r"font|text|letter|word|line-height|hyphen|tab-size|ruby|writing-mode"
    r"|orientation")

# Value sweeps for properties the database gives no keyword list.
VALUES = {
    "font-size": ["9px", "11px", "13px", "13.34px", "16px", "21px", "24px",
                  "32px", "64px"],
    "font-weight": ["100", "300", "400", "500", "600", "700", "900"],
    "font-stretch": ["50%", "75%", "100%", "125%", "200%"],
    "font-size-adjust": ["none", "0.5", "0.545"],
    "letter-spacing": ["normal", "-0.5px", "0.5px", "1.3px"],
    "word-spacing": ["normal", "4px", "-2px"],
    "line-height": ["normal", "1", "1.15", "20px"],
    "tab-size": ["8", "4", "2"],
    "font-feature-settings": ['normal', '"liga" 0', '"smcp"', '"onum"',
                              '"ss01"', '"kern" 0'],
    "font-variation-settings": ['normal', '"wght" 300', '"wght" 650'],
    "font-family": ["Arial", "Times New Roman", "Segoe UI", "monospace"],
    "font-palette": ["normal", "light", "dark"],
    "hyphenate-limit-chars": ["auto", "10 3 4"],
    "hyphenate-character": ['auto', '"-"'],
    "text-indent": ["0", "24px", "-12px"],
    "text-underline-offset": ["auto", "2px", "-1px"],
    "text-decoration-thickness": ["auto", "from-font", "3px"],
    "word-break": ["normal", "break-all", "keep-all", "break-word"],
    "text-shadow": ["none", "1px 1px 0 #888", "0 0 3px #00f"],
}

PAGE_HEAD = """<!doctype html>
<meta charset="utf-8">
<title>css matrix %d</title>
<style>
  body { font-family: "Segoe UI"; font-size: 16px; margin: 8px; }
  .row { white-space: nowrap; overflow: hidden; height: 26px; }
  .label { display: inline-block; width: 340px; font: 11px Consolas; }
</style>
"""


def keyword_lists(src):
    path = os.path.join(src, "third_party/blink/renderer/core/css/"
                        "css_properties.json5")
    text = open(path, encoding="utf-8").read()
    out = {}
    # The file is JSON5; entries are close enough to walk with a pattern:
    # a name followed, before the next name, by an optional keywords list.
    entries = re.split(r'\n\s*name:\s*"', text)[1:]
    for entry in entries:
        name = entry[:entry.index('"')]
        m = re.search(r"keywords:\s*\[([^\]]*)\]", entry)
        if m:
            out[name] = re.findall(r'"([^"]+)"', m.group(1))
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    src, out_dir = sys.argv[1], sys.argv[2]
    keywords = keyword_lists(src)
    props = sorted(n for n in keywords if PATTERN.search(n)
                   and not n.startswith("-"))
    for extra in VALUES:
        if extra not in props:
            props.append(extra)
    props.sort()

    page = 0
    rows = []
    plan = []

    def flush():
        nonlocal page, rows
        if not rows:
            return
        page += 1
        directory = os.path.join(out_dir, "css-matrix-%d" % page)
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(directory, "index.html"), "w",
                  encoding="utf-8") as handle:
            handle.write(PAGE_HEAD % page)
            handle.write("\n".join(rows))
        plan.append("page css-matrix-%d/index.html 0" % page)
        rows = []

    swept = 0
    for prop in props:
        values = VALUES.get(prop) or keywords.get(prop) or []
        values = [v for v in values if "!" not in v][:12]
        if not values:
            rows.append('<div class="row"><span class="label">%s'
                        '</span>(unswept)</div>' % prop)
            continue
        for value in values:
            rows.append(
                '<div class="row"><span class="label">%s: %s</span>'
                '<span style="%s: %s">%s</span></div>'
                % (prop, value, prop, value, SAMPLE))
            swept += 1
        if len(rows) >= 24:
            flush()
    flush()

    print("\n".join(plan))
    print("%d properties, %d property-value rows over %d pages"
          % (len(props), swept, page), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
