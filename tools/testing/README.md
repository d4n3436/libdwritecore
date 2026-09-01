# Testing tools

Measurement and verification. Nothing here is part of the build, nothing here
writes a file that is committed, and nothing here runs unless invoked by hand.
The tools that do produce committed output stay one level up in `tools/`,
alongside `paths.py` and the one the build itself calls (`make_impl.py`).

| Tool                                     | What it answers                                                                                                                                                                    | Needs                                                     |
|------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------|
| `verify_vtable.py`                       | Does the factory vtable in the binary match the headers? Reads the real vtable, which `vtable_test.cpp` cannot.                                                                    | Nothing                                                   |
| `save_page.py`                           | Turns a live URL into a directory that renders the same twice, offline.                                                                                                            | beautifulsoup4                                            |
| `gen_stress_page.py`                     | Emits a text rendering stress page: background x text color x font x size x weight and slant x script, every cell labeled.                                                         | Nothing                                                   |
| `capture_viewport.py` + `.sh`            | Puts one browser at an exact inner size, scroll offset and marked viewport origin, and photographs it.                                                                             | Firefox with Marionette; ImageMagick or libvirt           |
| `compare_viewport.py`                    | How far apart are two of those captures? Crops both to the marker and diffs.                                                                                                       | Pillow, NumPy                                             |
| `compare_pages.sh`                       | Runs a whole page set past both browsers and tabulates it, a percentage and a worst-channel figure per cell.                                                                       | both browsers, plus what `capture_viewport.sh` needs      |
| `charpos.py`                            | Are the glyphs in the same places? Per-character Range rects from both browsers, diffed, which separates advances and metrics from rasterization.                                  | both browsers                                             |
| `run_parity_firefox.sh`                  | Starts a browser with the parity fontconfig and the parity prefs, locally under Xvfb or in a guest. `--no-shim` and `--no-prefs` give the control runs.                            | Xvfb, Firefox, python-xlib; libvirt for the guest half    |
| `readprefs_marionette.py`                | What are a running Firefox's *effective* pref values? StaticPrefs are compiled into libxul and appear in no text file.                                                             | Firefox with Marionette                                   |
| `stress_leaks.sh` + `stress_leaks_ft.c`  | Every FreeType-side stress mode under valgrind at two scales; the scale comparison separates a leak from a cache.                                                                  | valgrind                                                  |
| `minwm.py`                               | Makes a bare X server usable for capture: maps windows and grants resize requests.                                                                                                 | Xvfb, python-xlib                                         |
| `vmexec.py`                              | Runs a command inside a libvirt guest through the QEMU guest agent.                                                                                                                | libvirt, a guest with qemu-guest-agent                    |
| `marionette.py`                          | The Marionette client the other tools share. Not run directly.                                                                                                                     | Nothing                                                   |
| `parity.plan.example`                    | The plan format `compare_pages.sh` reads. Copy to `parity.plan` and fill in your machines; the copy is gitignored.                                                                 | -                                                         |

`vmexec.py` is the only one that needs a Windows guest. The rest are
Linux-side, and the Marionette tools take a host and port, so they work against
either side of a comparison.

## Comparing two machines on the same page

    save_page.py <url> <dir>                  # once, so both sides get one page
    (cd <dir> && python3 -m http.server 8080 --bind 0.0.0.0)

    run_parity_firefox.sh start --port 2828
    run_parity_firefox.sh guest <domain> --port 2929
    cp parity.plan.example parity.plan   # then fill in your machines
    compare_pages.sh parity.plan

The plan names the size, the two sides and the pages; `compare_pages.sh --help`
shows the format. One cell at a time is still available underneath:

    capture_viewport.sh x11:<display>      linux   <host> <port> <url> 2200 1150 --scroll 0
    capture_viewport.sh libvirt:<domain>   win     <host> <port> <url> 2200 1150 --scroll 0
    compare_viewport.py 2200 1150 linux_marked.png linux_clean.png \
                                  win_marked.png   win_clean.png

Four things have to be true before the numbers mean anything:

* **Both browsers must be drawing with the same rasterizer.** Run
  `readprefs_marionette.py <host> <port> --graphics` on each and check that the
  WebRender feature status and the source revision agree. A machine on a GPU
  and a machine on software WebRender are not comparable.
* **The font files must be byte-identical**, not merely the same family at the
  same version. Compare hashes.
* **Measure system fonts in a windowed browser, never headless.** A headless
  Firefox substitutes `HeadlessLookAndFeel`, which answers `sans-serif` for
  every system font whatever the desktop says, so `system-ui` cannot be
  measured headless at all. `--systemfont` prints what the browser resolved.
* **The X side needs a window manager**, or Firefox never maps its window,
  `WebDriver:SetWindowRect` is ignored, and the screenshot is a uniform
  rectangle; `minwm.py` exists for that. Launch Firefox with `WAYLAND_DISPLAY`
  unset and `GDK_BACKEND=x11`, or a browser started from inside a Wayland
  session connects to that session instead of the X display being photographed.

## Narrowing a difference down to one character

A page comparison that comes back bad is usually not bad everywhere. Work down:

    compare_viewport.py ... --bands 24        # is this a layout offset or pixels?
    compare_viewport.py ... --rows 0,820      # judge the part above an offset
    charpos.py <sideA> <sideB> --page <path> --prefixes <urlA> <urlB>
    charpos.py <sideA> <sideB> --family F --size N --style S --weight W

`--bands` reports, per horizontal band, the vertical shift that aligns it best.
A band whose best shift is zero disagrees about pixels; one that jumps to 100%
at a shift of one or two disagrees about where the content is, and the row
where the best shift changes is the only place worth looking. `--rows` then
measures the part above it, which is the part where rasterization can still be
judged.

`charpos.py` reads every character's Range rect on both sides and diffs them,
which separates metrics from raster in one shot: characters that differ moved,
and a page whose characters all agree diverges in rasterization alone. The
second form styles one probe string instead of loading a page, which answers a
single family, weight and slant combination directly.

One check to run before blaming the shim: repeat with `LD_PRELOAD` unset. If
the number is the same without it, the difference is not the shim's.
