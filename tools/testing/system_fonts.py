#!/usr/bin/env python3
"""
Report the CSS system fonts a running Firefox resolves.

    tools/testing/system_fonts.py <host> <port> [--against <host> <port>]

Every one of the ten `font: <keyword>` shorthands maps to a LookAndFeel FontID,
and this asks the browser what each one computed to - family, pixel size,
weight, style. That is the whole of what a platform's UI font choice can change
inside a page, and `system-ui` is one of them by another name:
gfxPlatformFontList::ResolveGenericFontNames appends whatever LookAndFeel
reports for StyleSystemFont::Menu, quotes trimmed.

The ten are not one value. On GTK they come from four different style contexts
(nsLookAndFeel::PerThemeData::GetFont: Menu and -moz-pull-down-menu from the
menu context, -moz-field and -moz-list from an entry, -moz-button from a
button, and caption/icon/message-box/small-caption/status-bar from a label), so
a change that moves one need not move the others. Anything setting them from
prefs has to set all ten, because nsXPLookAndFeel::GetFontValue consults
ui.font.<id> per ID and falls back to the platform only for the ones with no
pref - and a missing .size does not fall back at all, it clamps to 16px.

--against measures a second browser and prints only the fonts that differ,
which is the same shape as readprefs_marionette.py --against.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from marionette import Marionette

# The six `font:` keywords content CSS can actually set. The other four FontIDs
# - -moz-field, -moz-list, -moz-button, -moz-pull-down-menu - are UA-only
# keywords: assigning them from content silently fails and leaves the initial
# serif/16px, which is what makes them easy to believe are broken when they are
# only unreachable. They are measured through the elements forms.css applies
# them to instead (font: -moz-field at forms.css:94, -moz-list at 208,
# -moz-button at 502).
KEYWORDS = ["caption", "icon", "menu", "message-box", "small-caption",
            "status-bar"]

CONTROLS = [("input", "-moz-field"), ("select", "-moz-list"),
            ("button", "-moz-button"), ("textarea", "-moz-field")]

PROBE = """
const out = {};
const el = document.createElement('span');
el.textContent = 'x';
document.documentElement.appendChild(el);
for (const kw of arguments[0]) {
  el.style.font = '';
  el.style.font = kw;
  if (!el.style.font) { out[kw] = ['(not settable from content)', '', '', '']; continue; }
  const cs = getComputedStyle(el);
  out[kw] = [cs.fontFamily, cs.fontSize, cs.fontWeight, cs.fontStyle];
}
el.remove();
for (const [tag, which] of arguments[1]) {
  const c = document.createElement(tag);
  document.documentElement.appendChild(c);
  const cs = getComputedStyle(c);
  out[tag + ' (' + which + ')'] = [cs.fontFamily, cs.fontSize, cs.fontWeight,
                                   cs.fontStyle];
  c.remove();
}
return out;
"""


def measure(host, port):
    m = Marionette(host, int(port), timeout=60)
    m.start("content")
    m.call("WebDriver:Navigate", {"url": "about:blank"})
    return m.script(PROBE, [KEYWORDS, CONTROLS])


def line(kw, v):
    family, size, weight, style = v
    trailing = "  %s %s %s" % (size, weight, style) if size else ""
    return "%-24s %s%s" % (kw, family, trailing)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__.strip())
    got = measure(args[0], args[1])

    if "--against" in args:
        at = args.index("--against")
        other = measure(args[at + 1], args[at + 2])
        differ = [k for k in got if got[k] != other.get(k)]
        for k in got:
            if k in differ:
                print("- " + line(k, got[k]))
                print("+ " + line(k, other[k]))
        print("\n%d of %d differ" % (len(differ), len(got)))
        sys.exit(1 if differ else 0)

    for k, v in got.items():
        print(line(k, v))


if __name__ == "__main__":
    main()
