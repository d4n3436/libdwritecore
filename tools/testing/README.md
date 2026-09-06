# Testing tools

Measurement and verification. Nothing here is part of the build and nothing
here runs unless invoked by hand. The generators one level up in `tools/` write
committed source; the two here that do the same, `gen_script_rows.py` and
`dwrite_fallback_probe.cpp`, are kept beside the harness that checks their
rows.

## What a comparison is made of

The question these tools answer is whether a browser on Linux, with the shim
loaded, draws the same pixels as the same browser on Windows. Everything below
uses these words for the parts of that.

**shim** is `build/libcleartype.so`, preloaded into the Linux browser so it
renders text through DWriteCore in place of the system FreeType stack. The
Windows side runs without it, since there it is what the shim imitates.

**corpus** is a directory of saved web pages, served over HTTP to both sides.
They are frozen copies, so the two machines render identical bytes on a page
that cannot change between the runs and cannot reach the network.
`save_page.py` freezes a URL into one, and the generators below write pages
that exercise a particular decision.

**side** is one browser in the comparison, and the machine it runs on. A
comparison has two, the Linux one under test and the Windows one in a libvirt
guest that is the reference.

**cell** is one page at one scroll offset, and it is the unit that gets
captured, compared and reported. A plan of 300 pages with a few offsets each
comes to a few thousand cells.

**plan** is a text file naming the size, the scale, the two sides and the
pages, so a run can be repeated by someone who has only that file.

**shard** is one of several browsers a side spreads its cells across, to
finish a large plan sooner. Shards are one side, and their results are pooled.

**sweep** is one run of a whole plan past both sides. The other way to ask a
question here is the census, which enumerates the decisions a renderer made
about fonts; a sweep photographs what it drew.

**backend** is how a side's screen gets photographed, since Firefox cannot
take a shot of itself. An X display, a guest's emulated display, or a
window inside a guest read over TCP. A Chromium side hands back its own
viewport and photographs no screen.

**marker** is an eight-pixel magenta square drawn at the viewport's top left
corner. A screenshot covers a whole screen, and the marker is what says which
pixel of it the viewport starts at, so a Firefox cell is shot twice, once with
the marker and once without, and only the clean shot is compared. A Chromium
side asks the browser for its viewport and needs none of this.

A sweep never writes those shots down. Each side hands its frame to the
comparison over a socket, the pair meets in memory and only the report
survives. Saving them was for reading them by eye, which nothing does now.
`capture_viewport.sh` still writes a pair on request, since the tools further
down want pixels to work on.

**accept file** is a list of cells whose difference has been traced to
something outside the shim. They are still captured, compared and printed,
marked `accepted`, and left out of the headline figure.

**control run** is the same run with the shim not loaded. A number means
something only as the difference between the two.

## The comparison

The same four steps run a Firefox comparison and a Chromium one, and only the
browser each side starts differs.

### 1. Serve the corpus

Both sides read the same pages over HTTP, so the bytes are identical and no
page reaches the network. The Firefox corpus is served on 8080 and the
Chromium one on 8744.

    cd <corpus> && python3 -m http.server 8080 --bind 0.0.0.0

The Firefox guest is staged from `STAGE_DIR` below. The Electron guest fetches
from the host over the libvirt bridge, so nothing is pushed into it.

### 2. Start both sides

Firefox:

    run_parity_firefox.sh start --port 2828 --display :99 \
        --size 3200x2400 --scale 1.0 --scheme light

    STAGE_DIR=<corpus> STAGE_URL=http://<host-ip>:8080 \
    run_parity_firefox.sh guest win11 --port 2929,2931 \
        --scale 1.0 --scheme light

Chromium and Electron:

    DWC_THEME=light run_electron_side.sh start electron-app about:blank 1920 1080 \
        --port 9222 --display :96 --binary <electron> \
        --preload $PWD/build/libcleartype.so

    run_electron_side.sh guest win11 --port 9223

What each side insists on:

* Firefox takes `--scale` and `--scheme`, and both must match across the two
  sides. Each is a Gecko pref, so the sweep asks every side what it is at and
  refuses one that is not at the plan's value.
* Electron has neither. The size is positional and the app sizes its own
  window in `electron-app/main.js`, because Electron does not implement
  Chromium's Browser domain, so the DevTools side verifies the size instead of
  setting it. The guest's `run.cmd` pins 1920x1080, and another size means
  editing it.
* `DWC_THEME` defaults to **dark**. A light run has to say so, and it has to
  agree with `DWC_COLOR_SCHEME` on the capture side, or the mismatch is
  silent.
* `--binary` pins the Electron build and has no default. `--no-sandbox` is
  needed for one unpacked from a release archive, which has no setuid
  `chrome-sandbox` and exits before DevTools opens.

For several browsers a side, give the port flag a comma-separated list. A
Marionette browser is photographed from its screen, so each needs a display of
its own and Marionette ports go two apart, each taking `PORT+1` for its
capture server. Start them one at a time: bringing them all up at once peaks
far above what they then hold.

### 3. Write a plan

    size 1199 579
    scale 1.25
    scheme light
    side linux x11::99,x11::97 127.0.0.1 2828,2830 http://127.0.0.1:8080
    side win   guest:192.168.122.206 192.168.122.206 2929,2931 http://192.168.122.1:8080
    accept firefox-residue.accept
    page wt/external/wpt/css/css-fonts/font-variant-position.html 0
    page long/index.html 0 1000 4600

A Chromium plan is the same with `cdp` as the side's sixth field:

    size 1920 1080
    side f  x11::96       127.0.0.1       9222,9225 http://127.0.0.1:8744     cdp
    side fW libvirt:win11 192.168.122.206 9223      http://192.168.122.1:8744 cdp

`size` is in CSS pixels; captures are `scale` times it. A side names its
capture backend, the host and port its browser listens on, and the URL prefix
*that machine* reaches the server by, which differs when one side is a guest.
On a `cdp` side the backend is still parsed and has to be well formed, but
nothing photographs a screen there.

A comma list is shards of one side, not a second browser to compare against.
All of a side's shards must be the same build, and `compare_pages.sh` walks
every port of both sides to check it, refusing the sweep and naming the odd
one out.

`accept` lists page globs whose difference is known not to be the shim's;
those cells are still captured and printed, marked `accepted`, and kept out of
the worst-channel figure. Accept files are not interchangeable between tools:
`compare_pages.sh` takes page paths, `font_census.py --accept` takes cell
keys. `firefox-residue.accept` and `widget-chrome.accept` are the page sweep's;
`fallback.accept`, `fallback-cef.accept` and `subpixel-metrics.accept` are the
census's. `compare_pages.sh --help` has the rest.

Copy `parity.plan.example` to start. A `*.plan` is gitignored, since it names
machines and a corpus that only exist where it was written.

### 4. Run it

    compare_pages.sh <plan>

    page                          scrollY   identical  max diff
    css-fonts/font-variant-position.html 0  100.0000%         0

    7376 of 7452 identical, 75 accepted
    7452 cells in 413s, worst channel difference 0, 1 failed

The worst channel difference is the largest single red, green or blue step
between the two sides, over every cell that is not accepted. Zero of those
with every cell accounted for is parity, and the count has to add up: the
identical, the accepted and any that failed to capture together make the
total. A cell that is neither identical nor `accepted` is the only thing to
look at.

Firefox is photographed from its screen through the marker handshake;
Chromium and Electron are read back with `Page.captureScreenshot`, which
returns the viewport with no marker and no screen to crop. The two routes are
not byte-identical, so both sides of one comparison must use the same one, and
the CDP route is pinned to a device pixel ratio of 1. Scaled sweeps are a
Marionette-only route.

`DWC_COMPARE_WORKERS` sizes the comparison pool (default 16); lower it when
memory is short. `--keep` retains what a run wrote, `--clusters N` groups the
differing pixels, `--css RULES` adds a rule sheet to both sides at once.

## Before the numbers mean anything

* **Same build on both sides, shards included.** For Firefox,
  `readprefs_marionette.py <host> <port> --graphics`: the WebRender feature
  status and source revision must agree, and a GPU against a software
  WebRender is not a comparison. For Electron, `--binary` pins it. Either way
  the sweep checks every shard before it starts.
* **Byte-identical font files**, not the same family at the same version.
* **The shim is stated deliberately.** A shell that already has it preloaded
  hands it to every browser it starts, so a run given no `--preload` is not
  the control it looks like. Confirm against `/proc/<pid>/maps`.
* **The port is free first.** A browser still holding it answers for the one
  about to start, and the pidfile and the mapped library both describe the new
  process, so nothing downstream catches it.
* **Windowed, never headless**, and occlusion throttling off. Headless Firefox
  answers `sans-serif` for every system font, so `system-ui` cannot be
  measured at all; a sharded Electron sweep puts two windows on one screen and
  a covered window's rAF is throttled to a stop.
* **The X side needs a window manager**, or the window never maps and the
  screenshot is a blank rectangle. `minwm.py` is one. Start Firefox with
  `WAYLAND_DISPLAY` unset and `GDK_BACKEND=x11`. The Electron guest runs in
  the services session with no desktop at all, which is why its capture has to
  be the direct route.
* **A control run.** Repeat with `--no-shim`. If the number is unchanged, the
  difference is not the shim's.
* **More than one run.** Rendering has been nondeterministic per process
  start, and a residue has been bisected on single-run arms and then retracted.

## When a cell differs

Work down until the difference has a name. The last three ask the browsers
themselves and need nothing on disk. The first two read a captured pair, and a
sweep leaves none, so `capture_viewport.sh` is what makes one.

    capture_viewport.sh <backend> <prefix> <host> <port> <url> <w> <h>
    compare_viewport.py <w> <h> <prefix>_marked.png <prefix>_clean.png \
                                <other>_marked.png <other>_clean.png --bands 24
    compare_viewport.py … --rows 0,820  # judge the part above an offset
    charpos.py <A> <B> --page <path> --prefixes <urlA> <urlB>
    attribute_diff.py <url> --a <A> --b <B> --backend-a <SPEC> --backend-b <SPEC>
    font_census.py …                    # which decision, not which pixel

`--bands` reports the vertical shift that best aligns each band: zero means
the pixels disagree, one or two means the content moved, and the row where it
changes is the place to look. `charpos.py` diffs every character's rect, which
separates metrics from raster in one shot. `attribute_diff.py` charges each
differing pixel to the innermost element covering it, capturing both sides
itself so the pixels and the boxes come from one load. The census enumerates
the decisions a renderer made; the sweep photographs what it drew.

Re-run a suspect cell on its own before believing it. A cell that is identical
alone and differs in a sweep is the harness under load, not the shim.

## Tools

| Tool | What it answers |
|---|---|
| `run_parity_firefox.sh` | Starts a parity Firefox here under Xvfb or in a libvirt guest. `--no-shim`, `--no-prefs` give the control runs. |
| `run_electron_side.sh` + `electron-app/` | The same for Chromium and Electron. The guest copy runs in the services session, so its capture is the direct route. |
| `compare_pages.sh` | Runs a whole plan past both sides and tabulates it. |
| `sweep_pages_marionette.py`, `sweep_pages_cdp.py` | One side of a plan. Called by `compare_pages.sh`. |
| `capture_viewport.sh` + `.py` | One cell by hand: sizes the browser, marks the origin, takes the two shots. `--driver marionette\|cdp`. |
| `screen_grab.py` | Photographs a screen: `x11:<display>`, `libvirt:<domain>`, `guest:<host>`. Shared by the sweep and the single cell. |
| `viewport_protocol.py` | The page state, size convergence and marker handshake both drivers share. |
| `compare_viewport.py` | How far apart two captures are. Pairs the sweep's frames in memory, and diffs a saved pair from the command line. |
| `charpos.py` | Per-character rects from both sides, diffed. Metrics against raster. |
| `attribute_diff.py` | Which element each differing pixel belongs to. |
| `font_census.py`, `census_per_language.sh` | Every font decision a renderer made, enumerated. The census restarts both browsers per language, since a renderer caches fallback by character. |
| `readprefs_marionette.py` | A running Firefox's *effective* prefs. StaticPrefs are compiled in and appear in no file. |
| `verify_vtable.py` | Does the factory vtable in the binary match the headers? |
| `dwrite_fallback_probe.cpp` | What DWriteCore's own fallback answers. The only source for the measured rows of `kScripts`. |
| `save_page.py` | Freezes a URL into a directory that renders the same twice, offline. |
| `gen_stress_page.py` | A text stress page: color, background, font, size, weight, slant, script. |
| `gen_css_matrix.py` | Comparison pages from Chromium's own CSS property database. |
| `gen_web_tests_plan.py` | Turns Chromium's web tests into plan `page` lines. |
| `gen_script_rows.py` | The fallback script table, from ICU's data. |
| `stress_leaks.sh` + `stress_leaks_ft.c` | Every FreeType stress mode under valgrind at two scales. |
| `marionette.py`, `minwm.py`, `vmexec.py`, `wincap.c`, `wincap_size.py` | Plumbing: the Marionette client, a minimal window manager, the guest agent, and the guest's capture server. |

Needs: Firefox with Marionette, an Electron build for the Chromium side, Xvfb
and python-xlib, Pillow and NumPy, libvirt and a guest with qemu-guest-agent
for the Windows side, valgrind for the leak runs, beautifulsoup4 for
`save_page.py`.
