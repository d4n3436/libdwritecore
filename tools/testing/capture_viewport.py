#!/usr/bin/env python3
"""
Put one browser into an exactly known state and hold it there for a screenshot.

    tools/testing/capture_viewport.py <host> <port> <url> <width> <height> \
                                      <tag> [--wait S] [--scroll Y]

Drives one Firefox over Marionette: sets the *inner* size to exactly
width x height, loads the url, waits for its fonts, scrolls to a given offset,
and draws an eight-pixel magenta square at the viewport origin. Then it writes
"<tag>.marked", waits for that file to be deleted, removes the square, writes
"<tag>.clean", and waits again.

The handshake is what makes an OS-level screenshot usable. The caller takes its
own screenshot - of a whole screen, a whole virtual framebuffer, a virtual
machine's display - and needs to know which pixel of it is the viewport's
origin. The marker says so, and comparing the *clean* shot of each side means
the marker itself is never part of what is compared. Sentinel files rather
than sleeps: the caller takes the shot when the page is actually ready, and
releases the next stage when the shot is actually taken.

Set the inner size, not the window size. Window chrome differs between
platforms and between window managers, so two machines told to use the same
window size render different amounts of page.

Nothing here sleeps for a fixed time. Every wait has an observable it stands
on:

    the window manager applying a resize   ->  innerWidth/innerHeight report it,
                                               and keep reporting it
    the page loading                       ->  readyState and document.fonts
    lazy images arriving after the scroll  ->  complete, for the ones now in
                                               the viewport and only those
    layout settling after that             ->  scrollHeight stops changing
    the compositor putting it on screen    ->  the screenshot stops changing,
                                               which only the caller can see and
                                               is therefore the caller's job

--wait is a deadline for the second and third of those, not a duration to sit
through; a local page normally clears them in well under a second.

Run one of these per machine, with capture_viewport.sh around each.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from marionette import Marionette

INNER = "return [window.innerWidth, window.innerHeight, window.outerWidth, window.outerHeight];"

# Two waits, either side of the scroll, because they are different questions.
#
# Before scrolling: the document is loaded and the fonts are resolved. The load
# event covers every eager resource, which is what readyState says.
#
# After scrolling: the images that are *now in the viewport* have arrived, and
# the layout has stopped moving. This half exists because of lazy loading. An
# img that has never been scrolled into view reports complete === false for
# ever, so waiting for all of them before scrolling never returns; waiting for
# none of them photographs a half-decoded image.
PAGE_LOADED = """
const cb = arguments[arguments.length - 1];
const deadline = Date.now() + arguments[0] * 1000;
function ready() { return document.readyState === 'complete'; }
function done() { document.fonts.ready.then(() => cb(['loaded'])); }
if (ready()) { done(); }
else {
  const poll = setInterval(() => {
    if (ready()) { clearInterval(poll); done(); }
    else if (Date.now() > deadline) { clearInterval(poll); cb(['deadline']); }
  }, 25);
}
"""

# The viewport size is part of what has to settle, not just the height: a
# window manager can still be adjusting the window when the first page loads,
# so a resize that lands late has to invalidate the layout measured before it.
#
# The settling loop runs on rAF, not on a timer. rAF is the layout and paint
# clock, so "unchanged on three consecutive frames" describes the thing being
# waited for and not the wall clock.
VIEWPORT_READY = """
const cb = arguments[arguments.length - 1];
const deadline = Date.now() + arguments[0] * 1000;

function pendingInView() {
  let n = 0;
  for (const img of document.images) {
    if (img.complete) { continue; }
    const r = img.getBoundingClientRect();
    if (r.bottom > 0 && r.top < window.innerHeight &&
        r.right > 0 && r.left < window.innerWidth) { n++; }
  }
  return n;
}

function shape() {
  return document.documentElement.scrollHeight + 'x' + window.innerWidth +
         'x' + window.innerHeight + 'x' + window.scrollY + 'x' + pendingInView();
}

function settle(last, repeats) {
  const now = shape();
  if (Date.now() > deadline) { cb(['deadline', now]); return; }
  const same = (now === last && now.endsWith('x0')) ? repeats + 1 : 0;
  if (same >= 3) { cb(['settled', now]); return; }
  requestAnimationFrame(() => settle(now, same));
}
settle(null, 0);
"""

SHAPE = ("return document.documentElement.scrollHeight + 'x' +"
         " window.innerWidth + 'x' + window.innerHeight + 'x' + window.scrollY;")

# scrollbarWidth first, and it is not a cosmetic choice. Firefox's overlay
# scrollbar is painted over the content rather than beside it - innerWidth and
# documentElement.clientWidth are both 2200 with it on screen, and turning it
# off changes neither, nor the page height, on either platform - so it is
# chrome that happens to sit inside the rectangle being photographed. It also
# fades out on a timer after a scroll, so a shot taken early catches the thumb
# mid-fade and two machines disagree over a hundred pixels of widget. Removing
# it is better than waiting the fade out, because it is not part of the
# rendering under comparison in the first place.
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

# Park the pointer somewhere harmless before the shot.
#
# Firefox shows the target of a hovered link in a panel over the bottom-left of
# the page. It is chrome, so nothing in the content process can remove it, it
# slides in rather than appearing, and it is drawn wherever the pointer happens
# to have been left - which on a machine nobody has touched is wherever the
# last run left it. Two captures of the same page, same browser, same
# configuration, differed by 6378 pixels in a 21-row band at the bottom because
# of it, and the band read as a text difference in the comparison.
#
# The pointer cannot be moved outside the viewport through WebDriver, so it
# goes to the first corner that is not over a link. Both machines are driven
# the same way and land on the same content coordinate, so whatever :hover
# effect it has is the same on both.
SAFE_POINT = """
const w = window.innerWidth, h = window.innerHeight;
function overLink(x, y) {
  let e = document.elementFromPoint(x, y);
  while (e) {
    if (e.tagName === 'A' || e.tagName === 'AREA') { return true; }
    e = e.parentElement;
  }
  return false;
}
for (const p of [[w - 4, 4], [4, 4], [w - 4, h - 4], [4, h - 4]]) {
  if (!overLink(p[0], p[1])) { return p; }
}
return [w - 4, 4];
"""

UNMARK = "const d = document.getElementById('__dwc_origin_mark'); if (d) d.remove(); return 1;"

# Returns once the change has been through a paint. This does not promise the
# compositor has put it on the screen - nothing inside the content process can
# promise that - which is why the caller checks the screenshot itself. It does
# mean the caller is never photographing a frame the change had not reached.
PAINTED = """
const cb = arguments[arguments.length - 1];
requestAnimationFrame(() => requestAnimationFrame(() => cb(1)));
"""

# Sentinels are a handshake between two processes on one machine, so the only
# thing this interval costs is latency. At one second it cost about half a
# second per stage, four stages per compared cell, on every cell of a sweep.
POLL = 0.02

# What one Marionette reply may take. main() sets each of these in turn.
SETUP_TIMEOUT = 20.0
WORK_TIMEOUT = 240.0


def await_condition(m, script, deadline, complaint):
    """Run one of the async scripts above and report a deadline as a warning.

    A deadline is not fatal: the capture still happens, and the caller would
    rather have a screenshot with a note attached than nothing at all. It goes
    to stderr so it cannot be mistaken for the JSON on stdout.
    """
    answer = m.call("WebDriver:ExecuteAsyncScript",
                    {"script": script, "args": [deadline]})
    if isinstance(answer, dict) and "value" in answer:
        answer = answer["value"]
    if answer and answer[0] == "deadline":
        print("%s within %gs: %s" % (complaint, deadline, answer[1:]),
              file=sys.stderr, flush=True)
    return answer


def park_pointer(m):
    """Move the mouse off any link. A failure here is not worth a failed run."""
    try:
        x, y = m.script(SAFE_POINT)
        m.call("WebDriver:PerformActions", {"actions": [{
            "type": "pointer", "id": "mouse",
            "parameters": {"pointerType": "mouse"},
            "actions": [{"type": "pointerMove", "duration": 0, "x": x, "y": y}],
        }]})
    except Exception as exc:                       # noqa: BLE001
        print("could not park the pointer: %s" % exc, file=sys.stderr, flush=True)


def wait_for_removal(path):
    while os.path.exists(path):
        time.sleep(POLL)


def main():
    args = sys.argv[1:]
    if len(args) < 6:
        sys.exit(__doc__.strip())
    host, port, url = args[0], int(args[1]), args[2]
    want_w, want_h, tag = int(args[3]), int(args[4]), args[5]
    deadline = 30.0
    scroll = 0
    rest = args[6:]
    while rest:
        if rest[0] == "--wait":
            deadline = float(rest[1]); rest = rest[2:]
        elif rest[0] == "--scroll":
            scroll = int(rest[1]); rest = rest[2:]
        else:
            sys.exit("unknown argument: " + rest[0])

    # Two timeouts, because the two halves fail on different scales. Reaching a
    # browser that is up and asking it for a session is a handful of
    # milliseconds, so a short ceiling here turns a wedged one into a prompt
    # error. Loading a page and waiting for it to settle is the slow half and
    # keeps the generous one.
    m = Marionette(host, port, timeout=SETUP_TIMEOUT)
    m.start("content")
    m.wait(WORK_TIMEOUT)
    m.call("WebDriver:Navigate", {"url": "about:blank"})

    # This converges instead of computing. The difference between outer and
    # inner is not known until the window exists, and on some window managers
    # it changes once more after the first resize. Re-reading the size *is* the wait - each
    # read is a round trip through the browser's main thread, so it also drains
    # whatever resize events were queued - and the size has to come back right
    # several times running before it counts, because arriving at it once and
    # arriving at it for good are different things.
    end = time.time() + deadline
    agreed = 0
    while agreed < 4:
        iw, ih, ow, oh = m.script(INNER)
        if (iw, ih) == (want_w, want_h):
            agreed += 1
            continue
        agreed = 0
        if time.time() > end:
            sys.exit("window never held %dx%d inner (last %dx%d)"
                     % (want_w, want_h, iw, ih))
        # Clamped, because the chrome difference is not always reportable. A
        # window that is minimized, or whose desktop session has no visible
        # window yet, answers outerWidth/outerHeight with placeholder values -
        # a guest measured 2560x1307 inner against 160x28 outer - and the
        # subtraction then asks for a negative height, which Marionette
        # rejects outright. Treating an impossible difference as zero asks for
        # the inner size itself, which is wrong by exactly the chrome and is
        # corrected on the next turn of this loop, so nothing fails here.
        m.call("WebDriver:SetWindowRect",
               {"x": 0, "y": 0,
                "width": want_w + max(ow - iw, 0),
                "height": want_h + max(oh - ih, 0)})

    m.call("WebDriver:Navigate", {"url": url})
    await_condition(m, PAGE_LOADED, deadline, "the page never finished loading")

    state = m.script(MARK, [scroll])
    # noinspection PyTypeChecker
    park_pointer(m)
    await_condition(m, VIEWPORT_READY, deadline,
                    "the viewport never settled after scrolling")
    m.call("WebDriver:ExecuteAsyncScript", {"script": PAINTED, "args": []})
    before = m.script(SHAPE)
    print('{"inner": %s}' % state, flush=True)

    open(tag + ".marked", "w").write("1")
    wait_for_removal(tag + ".marked")
    m.script(UNMARK)
    m.call("WebDriver:ExecuteAsyncScript", {"script": PAINTED, "args": []})
    open(tag + ".clean", "w").write("1")
    wait_for_removal(tag + ".clean")

    # Checked, not assumed. Everything above waits for a condition, but one
    # thing cannot be waited for from in here: Firefox builds part of its
    # platform font list off the main thread, and a page laid out before that
    # finishes reflows when it arrives - 16px on one sampler page, half a
    # second after the load event, on the first load of a fresh browser only.
    # run_parity_firefox.sh warms the browser so it does not happen during a
    # measurement; this makes the remaining case loud instead of silent, since
    # a capture that straddles a reflow looks like a perfectly good screenshot
    # of the wrong layout.
    after = m.script(SHAPE)
    if after != before:
        print("layout changed under the capture: %s -> %s" % (before, after),
              file=sys.stderr, flush=True)
        sys.exit(3)


if __name__ == "__main__":
    main()
