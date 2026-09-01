#!/usr/bin/env python3
"""
The parts of a viewport capture that do not depend on which browser it is.

capture_viewport.py drives Firefox over Marionette; capture_viewport_cdp.py
drives Chromium and Electron over the DevTools protocol. What they have in
common is everything that matters to the comparison:

  the JavaScript          the same page state has to be reached on both sides,
                          so the snippets that set it live here and not in
                          either driver.
  the size convergence    the difference between outer and inner size is not
                          known until the window exists, so the size is set,
                          re-read, and set again until it holds.
  the sentinel handshake  the caller takes an OS-level screenshot of a whole
                          screen or a whole guest display, so it has to be told
                          when the page is ready and when the marker is gone.

A driver supplies four operations and gets the choreography from Capture.

The JavaScript is written for Marionette's executeScript, which wraps a body
in a function and passes `arguments`. CdpBrowser wraps it the same way, so one
copy serves both.
"""

import abc
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
import urllib.request

# ---------------------------------------------------------------------------
# The page state. Shared, because the comparison is only meaningful if both
# sides were put into the same one.
# ---------------------------------------------------------------------------

INNER = ("return [window.innerWidth, window.innerHeight, "
         "window.outerWidth, window.outerHeight];")

# readyState reaches complete before font loading has settled, and a capture
# taken in between catches the page laid out with the fonts it had at the time.
PAGE_LOADED = """
return document.readyState === 'complete' && document.fonts.status === 'loaded';
"""

# Fonts loaded and no pending layout. document.fonts.ready alone is not
# enough, since it resolves before the first paint that uses them.
VIEWPORT_READY = """
return document.fonts.status === 'loaded' &&
       Math.abs(window.scrollY - arguments[0]) <= 1;
"""

SHAPE = ("return document.documentElement.scrollHeight + 'x' +"
         "       document.documentElement.scrollWidth;")

# An eight-pixel magenta square at the viewport origin, so a screenshot of a
# whole screen can be cropped to the viewport. Scrollbars are hidden because
# their width differs between platforms and would shift the content.
MARK = """
document.documentElement.style.scrollbarWidth = 'none';
const d = document.createElement('div');
d.id = '__dwc_origin_mark';
// all:initial first, because the page's own rules reach a bare div. A
// `div { margin: 0 20px }` moves the marker, since left:0 positions the
// margin box, and a border grows it; the crop then starts in the wrong
// place or off the window entirely.
// Two off-primary colors in vertical stripes, magenta on columns 0-3 and
// green on 4-7. Neither half alone is enough: rgb(255,0,255) is `magenta`,
// `fuchsia` and `#f0f` at once, and a page painting it in the corner captures
// the origin (direction-upright-002.html has 2478 px of it), while any single
// color is a range a tolerant test lets a page paint inside. Stripes rather
// than a checker because the geometry is explicit: a conic gradient starts at
// twelve o'clock, so its first quadrant is the top right and the detected
// corner comes out four rows low.
d.style.cssText = 'all:initial;position:fixed;left:0;top:0;width:8px;height:8px;'
                + 'z-index:2147483647;'
                + 'background:linear-gradient(90deg,'
                + 'rgb(253,3,251) 0 50%,rgb(3,251,3) 50% 100%);';
document.documentElement.appendChild(d);
document.documentElement.style.scrollBehavior = 'auto';
window.scrollTo({top: arguments[0], left: 0, behavior: 'instant'});
return [window.innerWidth, window.innerHeight, window.scrollY,
        document.documentElement.scrollHeight];
"""

UNMARK = ("const d = document.getElementById('__dwc_origin_mark'); "
          "if (d) d.remove(); return 1;")

# An extra rule sheet, applied to both sides of a comparison to take one
# suspected cause out of the picture. `font-kerning: none` prices what the two
# sides' kerning is worth, `font-weight: 400` what their bold is. The sheet
# goes in last, so a rule the caller marks !important beats the page's own.
EXTRA_CSS = """
const s = document.createElement('style');
s.id = '__dwc_extra_css';
s.textContent = arguments[0];
document.head.appendChild(s);
return 1;
"""

# Resolves after the next paint, so the screenshot is taken of a frame that
# includes whatever was just changed.
# Two frames is enough for layout, but not for an image that decodes on
# another thread: its first painted frame can land after the capture, which
# reads as a small blank patch on the slower side. Decoding is awaited first,
# then the frames.
PAINTED = """
const done = arguments[arguments.length - 1];
const decoded = Array.from(document.images)
    .filter(i => i.currentSrc)
    .map(i => i.decode().catch(() => {}));
// A background image decodes off the main thread and is not in
// document.images, so there is nothing to await for it. Its first painted
// frame can land after a two-frame settle, which reads as a blank patch on
// whichever side is busier. Frames are counted until three in a row arrive
// within a vsync interval of each other, which is the compositor having
// caught up. A 60 Hz tick is 16.7 ms, so the bound has to sit above that; a
// side still rastering skips frames and shows 33 ms and up.
//
// The deadline is measured from the moment this starts. A frame timestamp is
// never ahead of performance.now(), so comparing the two against each other
// gives a value that is at best zero and the wait never ends; a machine busy
// enough to miss the 12 ms mark forever is exactly when the escape is needed.
Promise.all(decoded).then(() => {
  const start = performance.now();
  let quiet = 0;
  let last = start;
  let settled = false;
  const finish = () => { if (!settled) { settled = true; done(1); } };
  const tick = (now) => {
    quiet = (now - last) < 22 ? quiet + 1 : 0;
    last = now;
    if (quiet >= 3 || performance.now() - start > 400) { finish(); return; }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
  // A window nothing composites never gets a frame, and a browser running
  // where no desktop is drawn is exactly that. The timer is the only thing
  // that ends the wait there.
  setTimeout(finish, 600);
});
"""

POLL = 0.02
SETUP_TIMEOUT = 20.0
WORK_TIMEOUT = 240.0


# ---------------------------------------------------------------------------
# A minimal DevTools client.
#
# Neither websockets nor websocket-client is present, and the protocol
# needed here is small. One connection, text frames, no extensions, no
# continuation.
# Everything below is RFC 6455 for that case only, and will not serve as a
# general client.
# ---------------------------------------------------------------------------

class WebSocket:
    def __init__(self, url, timeout=30.0):
        if not url.startswith("ws://"):
            raise ValueError("only ws:// is supported, got %r" % url)
        rest = url[len("ws://"):]
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.sock = socket.create_connection((host, int(port or 80)), timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        # DevTools refuses an upgrade whose Host is not a loopback name, to
        # stop a page reaching the debugger by DNS rebinding. It reads the
        # header rather than the peer address, so reaching a browser on another
        # machine means saying localhost while dialling its real address.
        sent_host = "localhost:%s" % (port or "80")
        self.sock.sendall((
            "GET /%s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" % (path, sent_host, key)).encode())
        header = self._read_until(b"\r\n\r\n")
        if b"101" not in header.split(b"\r\n")[0]:
            raise RuntimeError("websocket upgrade refused: %r" % header[:120])
        accept = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        if accept.encode() not in header:
            raise RuntimeError("websocket accept key did not match")
        self.buf = b""

    def _read_until(self, marker):
        data = b""
        while marker not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed during handshake")
            data += chunk
        return data

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        n = len(payload)
        if n < 126:
            header = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < (1 << 16):
            header = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        self.sock.sendall(header + mask + masked)

    def recv(self):
        while True:
            b0, b1 = struct.unpack("!BB", self._recv_exact(2))
            opcode = b0 & 0x0F
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._recv_exact(8))[0]
            payload = self._recv_exact(length)
            if opcode == 0x8:
                raise RuntimeError("websocket closed by peer")
            if opcode == 0x9:                       # ping
                self.sock.sendall(struct.pack("!BB", 0x8A, 0))
                continue
            if opcode in (0x1, 0x2):
                return payload.decode("utf-8", "replace")

    @abc.abstractmethod
    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Browsers.
# ---------------------------------------------------------------------------

class Browser(abc.ABC):
    """What Capture needs from whichever browser is being driven."""

    # Which protocol this speaks, for the few places that need to know.
    driver = "cdp"

    @abc.abstractmethod
    def navigate(self, url):
        raise NotImplementedError

    @abc.abstractmethod
    def script(self, body, args=()):
        """Run a Marionette-style body (uses `return` and `arguments`)."""
        raise NotImplementedError

    @abc.abstractmethod
    def script_async(self, body):
        """Run a body whose last argument is a completion callback."""
        raise NotImplementedError

    @abc.abstractmethod
    def set_window_rect(self, width, height):
        """Resize. False if this browser cannot, and the size must already hold."""
        raise NotImplementedError

    def close(self):
        pass


class MarionetteBrowser(Browser):
    """Firefox, over Marionette.

    The scripts the tools here run are already written the way Marionette
    passes them: arguments come in as `arguments[n]` and the value comes back
    from a `return`, so only the transport differs.
    """

    driver = "marionette"

    def __init__(self, host, port, timeout=30.0):
        import marionette
        self.m = marionette.Marionette(host, port, timeout=int(timeout))
        self.m.start("content")

    def navigate(self, url):
        self.m.call("WebDriver:Navigate", {"url": url})

    def script(self, body, args=()):
        return self.m.script(body, list(args))

    def script_async(self, body):
        return self.m.script_async(body)

    def set_window_rect(self, width, height):
        self.m.call("WebDriver:SetWindowRect",
                    {"width": width, "height": height, "x": 0, "y": 0})
        return True

    def close(self):
        self.m.close()


def open_browser(spec, timeout=30.0):
    """A browser from `[driver:]host:port`, DevTools when no driver is named."""
    driver, _, rest = spec.partition(":")
    if driver == "marionette":
        host, _, port = rest.partition(":")
        return MarionetteBrowser(host, int(port or 2828), timeout)
    if driver == "cdp":
        spec = rest
    host, _, port = spec.partition(":")
    return CdpBrowser(host, int(port or 9222), timeout)


class CdpBrowser(Browser):
    """Chromium and Electron, over the DevTools protocol.

    One socket, two scopes. Browser.setWindowBounds only exists on the
    browser-level connection, and Runtime/Page only inside a page, so this
    connects to the browser endpoint and attaches to the page target flat:
    browser methods go with no sessionId, page methods carry one.
    """

    def __init__(self, host, port, timeout=30.0):
        # noinspection HttpUrlsUsage
        # DevTools serves this endpoint over plain HTTP and offers no TLS; the
        # port is bound to loopback and reached through a forward.
        base = "http://%s:%d" % (host, port)
        version = self._get_json(base + "/json/version", timeout)
        self.ws = WebSocket(version["webSocketDebuggerUrl"], timeout)
        self.next_id = 1
        self.session = None

        target = self._first_page(base, timeout)
        self.session = self.call("Target.attachToTarget",
                                 {"targetId": target["id"], "flatten": True},
                                 session=False)["sessionId"]
        self.call("Page.enable")
        self.call("Runtime.enable")
        # A fixture that changed on disk is otherwise served out of the
        # browser's own cache, and only on the side that already had it, so the
        # two machines compare different bytes and the difference reads as a
        # rendering one.
        self.call("Network.enable")
        self.call("Network.setCacheDisabled", {"cacheDisabled": True})
        # Electron implements only part of the Browser domain: window
        # management there belongs to BrowserWindow, not Chromium, so
        # Browser.getWindowForTarget is simply absent. When it is, the window
        # is expected to have been sized by whoever launched the browser and
        # the convergence below verifies rather than sets it.
        try:
            self.window_id = self.call("Browser.getWindowForTarget",
                                       {"targetId": target["id"]},
                                       session=False)["windowId"]
        except RuntimeError:
            self.window_id = None

    @staticmethod
    def _get_json(url, timeout):
        end = time.time() + timeout
        while True:
            try:
                with urllib.request.urlopen(url, timeout=5) as r:
                    return json.load(r)
            except OSError:
                if time.time() > end:
                    raise
                time.sleep(0.2)

    def _first_page(self, base, timeout):
        end = time.time() + timeout
        while True:
            targets = self._get_json(base + "/json/list", timeout)
            pages = [t for t in targets if t.get("type") == "page"]
            if pages:
                return pages[0]
            if time.time() > end:
                raise RuntimeError("no page target appeared")
            time.sleep(0.2)

    def call(self, method, params=None, session=True):
        self.next_id += 1
        ident = self.next_id
        message = {"id": ident, "method": method, "params": params or {}}
        if session and self.session:
            message["sessionId"] = self.session
        self.ws.send(json.dumps(message))
        while True:
            reply = json.loads(self.ws.recv())
            if reply.get("id") != ident:
                continue                      # an event, or an earlier reply
            if "error" in reply:
                raise RuntimeError("%s: %s" % (method, reply["error"]))
            return reply.get("result", {})

    def _evaluate(self, expression, await_promise):
        result = self.call("Runtime.evaluate", {
            "expression": expression,
            "returnByValue": True,
            "awaitPromise": await_promise,
        })
        if result.get("exceptionDetails"):
            raise RuntimeError("script failed: %s"
                               % json.dumps(result["exceptionDetails"])[:300])
        return result.get("result", {}).get("value")

    def navigate(self, url):
        self.call("Page.navigate", {"url": url})

    def script(self, body, args=()):
        # Marionette wraps the body in a function and supplies `arguments`;
        # do the same so one copy of the JavaScript serves both drivers.
        return self._evaluate(
            "(function(){%s}).apply(null, %s)" % (body, json.dumps(list(args))),
            False)

    def script_async(self, body):
        return self._evaluate(
            "new Promise(function(resolve){"
            "  (function(){%s}).apply(null, [resolve]);"
            "})" % body, True)

    def set_window_rect(self, width, height):
        if self.window_id is None:
            return False          # cannot resize; the caller must have sized it
        self.call("Browser.setWindowBounds", {
            "windowId": self.window_id,
            "bounds": {"left": 0, "top": 0, "width": width, "height": height,
                       "windowState": "normal"},
        }, session=False)
        return True

    def close(self):
        self.ws.close()


# ---------------------------------------------------------------------------
# The choreography.
# ---------------------------------------------------------------------------

def await_condition(browser, body, deadline, what, args=()):
    end = time.time() + deadline
    while True:
        if browser.script(body, args):
            return
        if time.time() > end:
            sys.exit(what)
        time.sleep(POLL)


def converge_inner_size(browser, want_w, want_h, deadline):
    """Set the window until the *inner* size holds at exactly want_w x want_h.

    This converges instead of computing. The difference between outer and
    inner is not known until the window exists, and some window managers
    change it once more after the first resize. Re-reading the size is itself
    the wait: each read is a round trip through the browser's main thread, so
    it drains queued resize events. The answer has to come back right several
    times running, because arriving at a size once and holding it are
    different things.
    """
    end = time.time() + deadline
    agreed = 0
    forced = False
    misses = 0
    while agreed < 4:
        iw, ih, ow, oh = browser.script(INNER)
        if (iw, ih) == (want_w, want_h):
            agreed += 1
            continue
        agreed = 0
        if time.time() > end:
            sys.exit("window never held %dx%d inner (last %dx%d)"
                     % (want_w, want_h, iw, ih))
        # A resize call that is accepted but never lands, which is what a
        # window without a cooperating manager does, forces the layout
        # viewport the same way a refused one does.
        misses += 1
        if misses > 6 and not forced and hasattr(browser, "call"):
            browser.call("Emulation.setDeviceMetricsOverride",
                         {"width": want_w, "height": want_h,
                          "deviceScaleFactor": 1, "mobile": False})
            forced = True
            continue
        if forced:
            time.sleep(POLL)
            continue
        if browser.set_window_rect(want_w + max(ow - iw, 0),
                                   want_h + max(oh - ih, 0)) is False:
            # An app that sizes its own window answers False. Electron's does
            # and already holds the size, so this is only reached where the
            # two sides disagree: cefsimple opens 800x600 and cefclient 784x485,
            # and comparing those compares two different pages. The layout
            # viewport is overridden instead, which is what the screenshot is
            # taken of.
            if forced or not hasattr(browser, "call"):
                time.sleep(POLL)
                continue
            browser.call("Emulation.setDeviceMetricsOverride",
                         {"width": want_w, "height": want_h,
                          "deviceScaleFactor": 1, "mobile": False})
            forced = True
            continue
        # Clamped: a window that is not yet mapped reports placeholder outer
        # values, and the subtraction would then ask for a negative size.
        # Asking for the inner size itself is wrong by exactly the chrome and
        # is corrected on the next turn of this loop.


def wait_for_removal(path, deadline=WORK_TIMEOUT):
    end = time.time() + deadline
    while os.path.exists(path):
        if time.time() > end:
            sys.exit("%s was never removed; the caller did not take its shot" % path)
        time.sleep(POLL)


def park_pointer(browser):
    """Move the pointer to the top-left corner of the viewport.

    Chromium keeps :hover wherever the pointer last sat, so a machine whose
    pointer happens to rest on a link captures it underlined and in its hover
    color. The corner is page background on both sides. Only the CDP driver can
    send input, and the WebDriver one leaves its pointer outside the window.
    """
    if not hasattr(browser, "call"):
        return
    browser.call("Input.dispatchMouseEvent",
                 {"type": "mouseMoved", "x": 1, "y": 1, "buttons": 0})


def hide_scrollbars(browser):
    """Take the scrollbars out of the picture before anything is measured.

    Chromium draws overlay scrollbars inside the viewport and over the content,
    so they land in the crop. One stays drawn while the pointer is over it or a
    scroll is still fading, and that is not the same moment on two machines, so
    a thumb counts as a difference wherever it is caught. They are window
    furniture and are hidden instead of compared. Overlay scrollbars take no
    layout space, so nothing moves. Only the CDP driver can do this.
    """
    if not hasattr(browser, "call"):
        return
    browser.call("Emulation.setScrollbarsHidden", {"hidden": True})


def pin_color_scheme(browser):
    """Hold both sides to the same prefers-color-scheme.

    Which one an app reports is a property of the app and its desktop, not of
    the font stack: cefsimple on a bare X server answers dark and cefclient on
    the guest answers light, which makes every pixel of every page differ.
    Light is pinned because that is what both Electrons already report. Only
    the CDP driver can do this.
    """
    if not hasattr(browser, "call"):
        return
    browser.call("Emulation.setEmulatedMedia",
                 {"features": [{"name": "prefers-color-scheme", "value": "light"}]})


# The scroll part of MARK, without the marker. Scrollbars are already hidden
# session-wide through Emulation.setScrollbarsHidden, so this writes no style,
# since a per-page style write on the root element costs a full recalc on a
# large document. Returns how far the page can scroll at all, so an offset
# past its end is refused at once instead of waited on until the deadline.
SETTLE = """
window.scrollTo({top: arguments[0], left: 0, behavior: 'instant'});
return Math.max(0, document.documentElement.scrollHeight - window.innerHeight);
"""


def capture_direct(browser, url, want_w, want_h, out_png, scroll=0,
                   deadline=WORK_TIMEOUT, css=None):
    """Put the page into the compared state and read the frame back over CDP.

    Page.captureScreenshot returns exactly the viewport, so there is no
    marker to plant, no whole-screen photograph and no crop. It reads back
    the compositor's frame, which is not byte-identical to a photograph of
    the screen, so both sides of a comparison must be captured the same way
    and the two routes are never mixed.

    CDP only. The Marionette route keeps the marker handshake in capture().
    """
    browser.navigate(url)
    await_condition(browser, PAGE_LOADED, deadline, "the page never finished loading")
    if css:
        browser.script(EXTRA_CSS, [css])
        await_condition(browser, PAGE_LOADED, deadline,
                        "the page never settled after the extra css")
    max_scroll = browser.script(SETTLE, [scroll])
    if scroll > max_scroll + 1:
        raise RuntimeError("scroll %d is past the page, which ends at %d"
                           % (scroll, max_scroll))
    await_condition(browser, VIEWPORT_READY, deadline,
                    "the viewport never settled after scrolling", [scroll])
    # A navigation re-derives :hover from where the machine's own pointer
    # sits, and whatever is under it renders hovered until the pointer is
    # moved again, so the park is per page.
    park_pointer(browser)
    # The frame heuristics above cannot prove that everything painted: a
    # background image decodes with nothing to await and its first frame can
    # land after any fixed number of quiet ones. Two consecutive captures
    # with the same bytes can. The wait between tries is the paint settle
    # itself, and a page that never stabilizes keeps its newest frame once
    # the window closes, which is what a single capture did anyway.
    # optimizeForSpeed trades PNG size for encode time. Lossless either way,
    # and the encode is the whole cost of a busy frame without it.
    stable_end = time.time() + 5.0
    data = None
    while True:
        browser.script_async(PAINTED)
        shot = browser.call("Page.captureScreenshot",
                            {"format": "png", "fromSurface": True,
                             "optimizeForSpeed": True})
        fresh = base64.b64decode(shot["data"])
        if fresh == data or time.time() > stable_end:
            data = fresh
            break
        data = fresh
    with open(out_png, "wb") as handle:
        handle.write(data)


def capture(browser, url, want_w, want_h, tag, scroll=0, deadline=WORK_TIMEOUT,
            css=None):
    """Put the browser into the compared state and hand off for two shots.

    Writes "<tag>.marked" once the page is ready with the origin marker in
    place, waits for the caller to delete it, removes the marker, writes
    "<tag>.clean", and waits again. The caller screenshots between those.

    `css` is an extra rule sheet, for isolating a cause. It has to be the same
    on both sides or the comparison stops meaning anything.
    """
    browser.navigate("about:blank")
    hide_scrollbars(browser)
    converge_inner_size(browser, want_w, want_h, SETUP_TIMEOUT)
    browser.navigate(url)
    await_condition(browser, PAGE_LOADED, deadline, "the page never finished loading")
    if css:
        browser.script(EXTRA_CSS, [css])
        await_condition(browser, PAGE_LOADED, deadline,
                        "the page never settled after the extra css")

    state = browser.script(MARK, [scroll])
    await_condition(browser, VIEWPORT_READY, deadline,
                    "the viewport never settled after scrolling", [scroll])
    park_pointer(browser)
    browser.script_async(PAINTED)
    print(json.dumps({"inner": state}), flush=True)

    open(tag + ".marked", "w").write("1")
    wait_for_removal(tag + ".marked", deadline)
    browser.script(UNMARK)
    browser.script_async(PAINTED)
    open(tag + ".clean", "w").write("1")
    wait_for_removal(tag + ".clean", deadline)
    return state
