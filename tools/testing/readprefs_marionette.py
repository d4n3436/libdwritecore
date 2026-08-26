#!/usr/bin/env python3
"""
Read a running Firefox's *effective* pref values over Marionette.

    firefox -marionette -remote-allow-system-access -profile <dir> about:blank &
    tools/testing/readprefs_marionette.py 127.0.0.1 2828 gfx.font_rendering.
    tools/testing/readprefs_marionette.py 127.0.0.1 2828 --graphics
    tools/testing/readprefs_marionette.py 127.0.0.1 2828 --systemfont
    tools/testing/readprefs_marionette.py 127.0.0.1 2828 font. \
        --against <windows-host> 2929
    tools/testing/readprefs_marionette.py 127.0.0.1 2828 font. \
        --against <windows-host> 2929 --as-userjs

Why this and not greprefs.js: the interesting graphics prefs are StaticPrefs,
compiled into libxul from StaticPrefList.yaml rather than shipped as text, so
they appear in neither omni.ja nor prefs.js. Marionette runs privileged chrome
JS, which can ask the pref service directly - and it works the same way on
Windows, so the two platforms can be compared with one script.

--graphics answers a different question, and one worth asking before trusting
any comparison between two machines: which rasterizer is actually drawing.
Firefox falls back to software WebRender whenever hardware compositing is
blocklisted, and two machines that disagree about that are not comparable at
all - while two that agree are running the same software rasterizer, and a
difference between them is a difference in how it was built rather than in
what it was asked to do. It prints the adapter, the WebRender feature status,
and the source revision, all of which have to match before the pixels mean
anything.

Marionette framing is "<byte length>:<json>", and every command is
[0, id, name, params].
"""

import json
import socket
import sys


class Marionette:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=30)
        self.buf = b""
        self.next_id = 1
        self.recv()                       # server handshake

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


SCRIPT = """
const prefix = arguments[0];
const out = {};
for (const name of Services.prefs.getChildList(prefix)) {
  const kind = Services.prefs.getPrefType(name);
  let value;
  try {
    if (kind === Services.prefs.PREF_INT)         value = Services.prefs.getIntPref(name);
    else if (kind === Services.prefs.PREF_BOOL)   value = Services.prefs.getBoolPref(name);
    else if (kind === Services.prefs.PREF_STRING) value = Services.prefs.getStringPref(name);
    else value = null;
  } catch (e) { value = "<" + e.name + ">"; }
  out[name] = { value: value, userSet: Services.prefs.prefHasUserValue(name) };
}
return out;
"""


GRAPHICS = """
const out = {features: {}, adapter: {}};
const gfxInfo = Cc["@mozilla.org/gfx/info;1"].getService(Ci.nsIGfxInfo);
try {
  for (const f of gfxInfo.getFeatureLog().features || []) {
    out.features[f.name] = f.status;
  }
} catch (e) { out.featuresError = "" + e; }
for (const k of ["adapterDescription", "adapterVendorID", "adapterDeviceID",
                 "adapterDriverVersion"]) {
  try { out.adapter[k] = gfxInfo[k]; } catch (e) {}
}
try {
  const { AppConstants } = ChromeUtils.importESModule("resource://gre/modules/AppConstants.sys.mjs");
  out.sourceRevision = "" + AppConstants.SOURCE_REVISION_URL;
} catch (e) {}
try {
  out.build = Services.appinfo.version + " " + Services.appinfo.appBuildID
            + " " + Services.appinfo.XPCOMABI;
} catch (e) {}
return out;
"""

SYSTEM_FONT = """
// The families behind the CSS system keywords. `system-ui` has no pref of its
// own on any platform: gfxPlatformFontList::ResolveGenericFontNames appends
// whatever LookAndFeel reports for the Menu font, so on Windows that is the
// shell's menu font and on GTK it is the desktop's. Comparing the two is how
// a system-ui mismatch gets a name instead of a symptom.
const out = {};
for (const which of ["Caption", "Icon", "Menu", "MessageBox", "SmallCaption",
                     "StatusBar"]) {
  try {
    const name = {}, style = {};
    if (LookAndFeel.getFont(which, name, style) !== false) {
      out[which] = "" + (name.value !== undefined ? name.value : name);
    }
  } catch (e) { out[which] = "<" + e.name + ">"; }
}
try {
  const el = document.createElement("div");
  el.style.font = "menu";
  document.documentElement.appendChild(el);
  out["computed font shorthand for 'menu'"] = getComputedStyle(el).fontFamily;
  el.remove();
} catch (e) { out.computedError = "" + e; }
return out;
"""

INTERESTING = ("HW_COMPOSITING", "D3D11_COMPOSITING", "OPENGL_COMPOSITING",
               "WEBRENDER", "WEBRENDER_COMPOSITOR", "WEBRENDER_ANGLE")


def unwrap(result):
    return result["value"] if isinstance(result, dict) and "value" in result else result


def show_graphics(m):
    info = unwrap(m.call("WebDriver:ExecuteScript", {"script": GRAPHICS, "args": []}))
    print("build           %s" % info.get("build", "?"))
    print("source          %s" % info.get("sourceRevision", "?"))
    for k in sorted(info.get("adapter", {})):
        print("%-15s %s" % (k, info["adapter"][k]))
    for name in INTERESTING:
        if name in info.get("features", {}):
            print("%-15s %s" % (name, info["features"][name]))


def show_system_font(m):
    info = unwrap(m.call("WebDriver:ExecuteScript", {"script": SYSTEM_FONT, "args": []}))
    for k in sorted(info):
        print("%-38s %s" % (k, info[k]))


def read_prefs(m, prefix):
    return unwrap(m.call("WebDriver:ExecuteScript",
                         {"script": SCRIPT, "args": [prefix]}))


def show_prefs(m, prefix):
    prefs = read_prefs(m, prefix)
    for name in sorted(prefs):
        entry = prefs[name]
        print("%-58s %-18s %s" % (name, entry["value"],
                                  "(user set)" if entry["userSet"] else ""))


def show_diff(a, b, prefix, as_userjs=False):
    """Only the prefs the two browsers disagree about.

    This is the cheapest parity check there is, and it has found more than the
    expensive ones: font.size.monospace.x-western is 13 on Windows and 12 on
    Linux, which silently renders every <pre> on the web a point smaller here.
    A pref that differs is a difference the pages will show, whatever the
    rasterizer does.
    """
    left = read_prefs(a, prefix)
    right = read_prefs(b, prefix)
    names = sorted(set(left) | set(right))
    shown = 0
    for name in names:
        missing = object()
        x = left.get(name, {}).get("value", missing)
        y = right.get(name, {}).get("value", missing)
        if x == y:
            continue
        shown += 1
        if as_userjs:
            # The reference side's value, as a line for a user.js. A pref the
            # reference does not have at all becomes an empty string, which is
            # how Firefox behaves when the pref is absent: ParseFontList gets
            # nothing out of it and the list is empty either way.
            if y is missing:
                literal = '""'
            elif isinstance(y, bool):
                literal = "true" if y else "false"
            elif isinstance(y, int):
                literal = str(y)
            else:
                literal = '"%s"' % y
            print('user_pref("%s", %s);  // was %s here'
                  % (name, literal,
                     "unset" if x is missing else x))
        else:
            print("%-52s %-20s %s"
                  % (name, "<absent>" if x is missing else x,
                     "<absent>" if y is missing else y))
    if not as_userjs:
        print("# %d of %d prefs under %r differ" % (shown, len(names), prefix))


def connect(host, port):
    m = Marionette(host, port)
    m.call("WebDriver:NewSession", {"capabilities": {}})
    m.call("Marionette:SetContext", {"value": "chrome"})
    return m


def main():
    flags = sys.argv[1:]
    graphics = "--graphics" in flags
    systemfont = "--systemfont" in flags
    as_userjs = "--as-userjs" in flags
    against = None
    if "--against" in flags:
        i = flags.index("--against")
        if len(flags) < i + 3:
            sys.exit("--against needs a host and a port")
        against = (flags[i + 1], int(flags[i + 2]))
        del flags[i:i + 3]
    args = [a for a in flags
            if a not in ("--graphics", "--systemfont", "--as-userjs")]
    host = args[0] if len(args) > 0 else "127.0.0.1"
    port = int(args[1]) if len(args) > 1 else 2828
    prefix = args[2] if len(args) > 2 else "gfx.font_rendering."

    m = connect(host, port)
    if against:
        show_diff(m, connect(*against), prefix, as_userjs)
    elif graphics:
        show_graphics(m)
    elif systemfont:
        show_system_font(m)
    else:
        show_prefs(m, prefix)


if __name__ == "__main__":
    main()
