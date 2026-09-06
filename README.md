# libdwritecore

This repo provides a build of DirectWrite for Linux, with the complete ClearType pipeline,
and an `LD_PRELOAD` interposer that puts it under applications that draw their text with FreeType.

Microsoft ships DWriteCore, a cross-platform DirectWrite engine, as part of the
Windows App SDK, and only publishes it for Windows. The Microsoft Office Android
APK happens to carry a Bionic build of that same engine. This project takes
that build and makes it usable on Linux.

Two libraries come out of it.

| File                     | What it is                                                                                                                                                                                     |
|--------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `build/libdwritecore.so` | DirectWrite itself, as one self-contained file. `include/` mirrors the Windows App SDK headers, so DirectWrite code compiles unchanged.                                                        |
| `build/libcleartype.so`  | The FreeType interposer, which is what lets ordinary Linux applications render through the ClearType rasterizer. Also contains the parity code for Firefox and for Chromium, Electron and CEF. |

## Build

x86-64 only for now.

### Prerequisites

A C++20 compiler, CMake 3.16 or newer, Python 3, and FreeType 2.10 or newer
with its development headers. GCC 10 and Clang 14 are the oldest tested.

```sh
# Debian, Ubuntu
sudo apt install build-essential cmake python3 libfreetype-dev

# Fedora, RHEL
sudo dnf install gcc-c++ cmake python3 freetype-devel

# Arch
sudo pacman -S --needed base-devel cmake python freetype2

# openSUSE
sudo zypper install gcc-c++ cmake python3 freetype2-devel
```

Configuring stops if the FreeType headers are absent.
`-DDWRITECORE_BUILD_CLEARTYPE=OFF` builds everything else without them.

```sh
cmake -S . -B build
cmake --build build
```

## ClearType for Linux applications

`libcleartype.so` sits in front of FreeType. When an application asks FreeType
to rasterize a glyph as a subpixel (LCD) bitmap, this library rasterizes the
same glyph through DWriteCore's ClearType pipeline and returns that instead.
Every other FreeType call is forwarded untouched, so glyph choice, advances and
layout still come from the application's own measurements.

### Prerequisites

**Subpixel rendering has to be on.** Nothing asks FreeType for an LCD bitmap
otherwise, and there is then nothing to intercept. To check your own setting:

```sh
fc-match -f '%{rgba}\n' sans   # 1 rgb, 2 bgr, 3 vrgb, 4 vbgr; 0 unknown, 5 none
```

`0` or `5` means it is off. This turns it on for every application, on any
distribution:

```sh
mkdir -p ~/.config/fontconfig/conf.d && \
printf '%s\n' \
  '<?xml version="1.0"?>' \
  '<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">' \
  '<fontconfig>' \
  '  <match target="font">' \
  '    <edit name="antialias" mode="assign"><bool>true</bool></edit>' \
  '    <edit name="rgba"      mode="assign"><const>rgb</const></edit>' \
  '    <edit name="lcdfilter" mode="assign"><const>lcddefault</const></edit>' \
  '  </match>' \
  '</fontconfig>' \
  > ~/.config/fontconfig/conf.d/99-subpixel.conf && \
fc-cache -r
```

On GNOME and on KDE Plasma, set it there too:

```sh
# GNOME
gsettings set org.gnome.desktop.interface font-antialiasing 'rgba'

# KDE Plasma
kwriteconfig6 --file kdeglobals --group General --key XftSubPixel rgb
```

Applications pick the new setting up the next time they start.

### Usage

```sh
LD_PRELOAD=$PWD/build/libcleartype.so <app>
```

Just a single `LD_PRELOAD` is enough.
This will cause any calls for glyph rasterization on FreeType to go through DirectWrite instead.

Behavior is tuned with `CLEARTYPE_*` environment variables, listed under [Options](#options).

To autoload the interposer at boot, see [Installation](#installation).

## Parity mode

Rasterization alone makes text look like Windows. Parity mode also makes it
**lay out** like Windows, by handing the application the font metrics and advances
that DirectWrite computes there, along with the preferences the Windows build
runs with, achieving 100% accuracy versus Windows's own DirectWrite rendering.

Parity mode currently targets Gecko (Firefox), Chromium and the applications
built on Electron and CEF. The library works out which one it is running under
and applies that engine's parity behavior and nothing else, so one file is a
Firefox parity shim under Firefox, a Chromium parity shim under an Electron
application, and a plain interposer everywhere else.

It is verified against Firefox, Electron 43 in both its statically and
dynamically linked builds, and CEF 144, each measured against a Windows machine
running the same build.

### Prerequisites

**The Windows fonts.** Both paths ask for 23 specific families by name, and
without them, the browser substitutes whatever is installed and the text will
not match Windows.
Plain interposition needs none of this and works with the fonts you may already have.

You can auto-install them with the following command:

```sh
tools/winfonts.sh
```

It reads Microsoft's Windows 11 evaluation image over range requests, pulling
only the bytes the fonts occupy, and writes them straight into your font
directory. Around 190 MB is fetched, held in a tmpfs, and never written to disk.

Running it again fetches only what is missing, and does nothing at all when
every font is already there.

Needs `curl`, `od`, `awk`, `wimlib-imagex` (packaged as `wimtools`,
`wimlib-utils` or `wimlib`) and GNU `dd`.

### Usage

```sh
# Close Firefox first. Launching a second copy hands the window to the
# already running one, which has no preload, and nothing changes.
LD_PRELOAD=$PWD/build/libcleartype.so firefox

LD_PRELOAD=$PWD/build/libcleartype.so chromium
LD_PRELOAD=$PWD/build/libcleartype.so ./my-electron-app
```

Under Firefox, the ~150 preferences this needs travel in `MOZ_DEFAULT_PREFS`,
which Firefox reads as pref data and not as a file name, so no profile is
touched and no `user.js` is needed. They arrive as defaults, so anything you
have already set yourself is not overridden.

Two things sit outside it. DWriteCore matches DirectWrite's rasterization up to
Windows 11 22H2, and Microsoft changed that in 24H2, so a diagonal can land a
subpixel away from what a stock 24H2 machine draws. And a Linux Firefox and a
Windows one differ by one step on antialiased edges with no text on the page at
all, being one WebRender source built by two compilers, with picture cache tiles
512px wide on Windows against 1024px everywhere else.

To autoload the interposer at boot, see the section below.

## Installation

```sh
cmake --install build --prefix ~/.local
```

Both libraries are stored in `~/.local/lib/dwritecore/`.

### Autoload for your whole session

`LD_PRELOAD` has to be set before the programs it applies to start, which means
the session and not a shell. Every distribution running systemd reads
`~/.config/environment.d/` when the user session begins:

```sh
mkdir -p ~/.config/environment.d
printf 'LD_PRELOAD=%s/.local/lib/dwritecore/libcleartype.so\n' "$HOME" \
    > ~/.config/environment.d/50-cleartype.conf
```

Log out and back in to apply it. Every program in the session will then draw its text through DWriteCore.

### Autoload for one program

```sh
mkdir -p ~/.local/share/applications
cp /usr/share/applications/firefox.desktop ~/.local/share/applications/
sed -i "s|^Exec=|Exec=env LD_PRELOAD=$HOME/.local/lib/dwritecore/libcleartype.so |" \
    ~/.local/share/applications/firefox.desktop
```

### Checking it worked

```sh
grep -c libcleartype /proc/$(pidof -s firefox)/maps
```

Anything above zero means the library is mapped into that process.

## Options

Compile-time, given to `cmake` as `-D<name>=<value>`:

| Option                       | Default                                       | Effect                                                                                                                                      |
|------------------------------|-----------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------|
| `DWRITECORE_BUILD_CLEARTYPE` | `ON`                                          | Build `libcleartype.so`. Needs FreeType.                                                                                                    |
| `CLEARTYPE_FIREFOX_PARITY`   | `ON`                                          | Compile the Gecko parity path in.                                                                                                           |
| `CLEARTYPE_CHROMIUM_PARITY`  | `ON`                                          | Compile the Chromium, Electron and CEF parity path in. Needs an assembler for its vtable thunks.                                            |
| `DWRITECORE_EMBED_IMPL`      | `ON`                                          | Carry the implementation and the Bionic stubs inside `libdwritecore.so`. `OFF` leaves them beside it, which is friendlier while developing. |
| `DWRITECORE_ORIGINAL`        | `third_party/office-android/libdwritecore.so` | The stock implementation to build against.                                                                                                  |
| `DWRITECORE_INSTALL_DIR`     | `${CMAKE_INSTALL_LIBDIR}/dwritecore`          | Where `cmake --install` puts both libraries.                                                                                                |
| `DWRITECORE_SPLIT_DEBUG`     | `OFF`                                         | Write each library's debug info to `<library>.debug` beside it and strip the library. A debugger finds it through `.gnu_debuglink`.         |

Read from the environment by `libcleartype.so` at startup:

| Variable                           | Default                       | Effect                                                                                                                                                                                                                                             |
|------------------------------------|-------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `CLEARTYPE=0`                      | on                            | Disable entirely; every call goes to FreeType.                                                                                                                                                                                                     |
| `CLEARTYPE_FIREFOX=0`              | on                            | Leave a Gecko process's advances and metrics alone. Rasterization is unaffected; that is `CLEARTYPE` above. Inert outside Gecko either way.                                                                                                        |
| `CLEARTYPE_CHROMIUM=0`             | on                            | Leave a Chromium process alone. Inert outside Chromium either way.                                                                                                                                                                                 |
| `CLEARTYPE_RENDERING_MODE`         | `auto`                        | `auto` asks DirectWrite per font and size. `gdi-classic`, `gdi-natural`, `natural`, `natural-symmetric` and `aliased` pin one mode for everything.                                                                                                 |
| `CLEARTYPE_MEASURING_MODE`         | follows the rendering mode    | `natural`, `gdi-classic`, `gdi-natural`.                                                                                                                                                                                                           |
| `CLEARTYPE_GRID_FIT`               | unset                         | `enabled`, `disabled`, `default`.                                                                                                                                                                                                                  |
| `CLEARTYPE_SUBPIXEL_POSITIONING=0` | on                            | Ignore the caller's fractional pen position.                                                                                                                                                                                                       |
| `CLEARTYPE_WINDOWS_METRICS=0`      | on                            | Leave FreeType's metrics and advances alone, for rasterization parity only. Inert outside a Gecko process.                                                                                                                                         |
| `CLEARTYPE_ALPHA_GAMMA`            | `1.0` (off)                   | Reshape the coverage curve, for a caller with no preblend of its own. Not for Firefox.                                                                                                                                                             |
| `CLEARTYPE_FONTCONFIG`             | on under Gecko, off elsewhere | Answer fontconfig's rendering queries from this library. The default differs because parity mode's answers describe Windows, while outside it they would overrule your own settings. Chromium states its own render params and does not need this. |
| `CLEARTYPE_LIBXUL_PATCH=0`         | on                            | Leave libxul's own code alone. Inert outside a Gecko process.                                                                                                                                                                                      |
| `CLEARTYPE_CHROMIUM_PATCH=0`       | on                            | Leave Skia's scaler context alone, for the parity work that reaches Chromium by other routes only. Inert outside Chromium.                                                                                                                         |
| `CLEARTYPE_PREFS=0`                | on                            | Do not set `MOZ_DEFAULT_PREFS`; Firefox starts on its own Linux prefs. Inert outside a Gecko process.                                                                                                                                              |
| `CLEARTYPE_FORCE_FALLBACK=1`       | off                           | Send every glyph down the FreeType fallback path, on input DirectWrite would have handled. For the interposer's own tests.                                                                                                                         |
| `CLEARTYPE_LOG`                    | unset                         | Append diagnostics to `<path>.<pid>`, one file per process. `-` writes to stderr, which is what a sandboxed content process can reach.                                                                                                             |
| `CLEARTYPE_LOG_FAMILY`             | unset                         | Restrict the log to one family name.                                                                                                                                                                                                               |
| `CLEARTYPE_CENSUS_SECONDS`         | `10`                          | How often the log gets a census of every table this library keeps. Only written while `CLEARTYPE_LOG` is set.                                                                                                                                      |
| `DWC_FAULT_REPORT=1`               | off                           | Print a stack for a renderer fault, for a crash a core dump cannot explain. Chromium only.                                                                                                                                                         |

Read by `libdwritecore.so` at startup:

| Variable                    | Effect                                                                                             |
|-----------------------------|----------------------------------------------------------------------------------------------------|
| `DWRITECORE_SYSTEM_FONTS=0` | Return the implementation's own factory unwrapped, with no system font collection and no fallback. |
| `DWRITECORE_IMPL_PATH`      | Load the implementation from this path. Only meaningful in a `DWRITECORE_EMBED_IMPL=OFF` build.    |
| `DWRITECORE_BIONIC_DIR`     | Load the Bionic stubs from a specific directory. Likewise.                                         |

## Using the DirectWrite API

Same as regular DWriteCore.

```c++
#include "dwrite_core.h"

IUnknown* unknown = nullptr;
DWriteCoreCreateFactory(DWRITE_FACTORY_TYPE_SHARED, DWRITE_UUIDOF(IDWriteFactory), &unknown);
auto* factory = static_cast<IDWriteFactory*>(unknown);

factory->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, mode, measuring, 0, 0, &analysis);
analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, buffer, size);
```

### The system font collection

The shipped library can open font files but cannot find them. It imports `open`,
`read`, `stat` and `mmap`, and no directory enumeration at all. On Windows a
platform layer supplies the list of installed fonts; on Linux nothing does, so
`GetSystemFontCollection` would answer `S_OK` with zero families and no text
would lay out.

So `DWriteCoreCreateFactory` returns a proxy over the real factory. It forwards
every method untouched except the system font getters, which answer from
fontconfig's list, and the two `CreateTextFormat` overloads, whose optional
collection argument would otherwise pick up the empty one.

## Tests

`ctest --test-dir build` runs five. They find their fonts through fontconfig,
and where nothing suitable is installed they report **Skipped** rather than
passing without running.

| Test             | What it proves                                                                                                                        |
|------------------|---------------------------------------------------------------------------------------------------------------------------------------|
| `abi_check`      | Struct layouts, including the one that fails silently: `WCHAR` is 2 bytes, never Linux's 4-byte `wchar_t`.                            |
| `vtable_test`    | Calls every method of all 103 interfaces against a mock vtable and checks each lands on the expected slot, 1819 of them.              |
| `cleartype_test` | End to end. Loads the real implementation through the Bionic layer and rasterizes a glyph run.                                        |
| `thread_test`    | 8 threads through the full pipeline against one shared factory, which is what exercises DWriteCore's internal caches and their locks. |
| `shaping_test`   | Script and bidi analysis, glyph mapping, placement and rasterization across six samples.                                              |

`vtable_test` is generated from the same headers it checks, so on its own it
only proves the generator is coherent. `tools/testing/verify_vtable.py` closes
that by reading the real factory vtable out of the binary, and two slots were
disassembled and matched against their declared signatures.

`shaping_test` goes deepest. It is also the only place DWriteCore calls back
into interfaces the caller implements, so the ABI is exercised in both
directions.

| Sample         | Chars → glyphs | Result                                                    |
|----------------|----------------|-----------------------------------------------------------|
| Arabic (Naskh) | 13 → 17        | GSUB applied, bidi level 1                                |
| Arabic (Kufi)  | 12 → 13        | GSUB applied, bidi level 1                                |
| Hebrew         | 9 → 9          | identical, as expected, RTL but not cursive; bidi level 1 |
| Chinese        | 6 → 6          | GSUB applied                                              |
| Japanese       | 7 → 7          | split into 2 script runs (Hiragana / Han)                 |
| Devanagari     | 13 → 11        | conjuncts formed, fewer glyphs than input                 |

Devanagari is the strongest signal. 13 characters collapsing to 11 glyphs means
conjunct substitution and reordering actually ran, and the rendered output shows
a continuous shirorekha across the letters.

The interposer's own tests need a real `LD_PRELOAD` and so are run by hand:

```sh
FONT=$(fc-match -f '%{file}' 'DejaVu Sans')

# Both must exit 0. The second forces the FreeType fallback path on input
# DirectWrite would have handled, which is what proves the fallback works.
LD_PRELOAD=$PWD/build/libcleartype.so ./build/cleartype/cleartype_test_fallback "$FONT"
CLEARTYPE_FORCE_FALLBACK=1 LD_PRELOAD=$PWD/build/libcleartype.so \
  ./build/cleartype/cleartype_test_fallback "$FONT"

# Renders the same line both ways, for comparing them.
./build/cleartype/cleartype_sample $FONT freetype.ppm
LD_PRELOAD=$PWD/build/libcleartype.so ./build/cleartype/cleartype_sample $FONT dwrite.ppm
```

## Layout

```
cleartype/         the FreeType LD_PRELOAD interposer
  src/             the interposer itself, built into libcleartype.so
    chromium/      the Chromium, Electron and CEF parity path
  probes/          logging-only libraries, for checking what an application does
  tests/           run by hand under a real LD_PRELOAD, not by ctest
third_party/       vendored Microsoft material - see third_party/README.md
  windows-app-sdk/ the six DirectWrite headers
  windows-sdk/     the Windows SDK subset they depend on
  office-android/  libdwritecore.so, from the Office APK
include/           mirrored DirectWrite + Windows SDK headers. dwrite_core.h is
                   the entry point, as it is on Windows
src/               loader and the factory proxy that supplies the system font
                   collection
bionic-compat/     bionic libc/libm/libdl/liblog stubs, which is what lets an
                   Android build run on glibc
tools/             header mirrors, API generator, binary analysis, winfonts.sh
  testing/         measurement and verification, run by hand. See its README
tests/             ABI, vtable slot, end-to-end ClearType and concurrency tests
```

### Regenerating the checked-in sources

Needed only when the vendored input changes. Each script reads `third_party/`
and rewrites files already in the tree.

```sh
python3 tools/mirror_headers.py      # the DirectWrite headers
python3 tools/mirror_sdk_headers.py  # the Windows SDK headers they need
python3 tools/gen_vtable_test.py     # the vtable slot test
python3 tools/gen_bionic_maps.py     # the Bionic stub version scripts
python3 tools/gen_factory_proxy.py   # the factory proxy
python3 tools/gen_fallback_data.py   # the per-script fallback table
```

Two more read inputs that are not in `third_party/`:

```sh
# The parity data, from a Firefox tree. --check (the default) verifies.
python3 tools/gen_firefox_parity.py --fetch --tree <dir> --write

# DirectWrite's own fallback table, from a Windows DWrite.dll or DWriteCore.dll.
# There is no copy of either in this repository.
python3 tools/extract_dwrite_fallback.py --write <path-to-dll>
```

Every path into `third_party/` is resolved in `tools/paths.py`, and
`DWRITECORE_THIRD_PARTY` points the tools at a copy held elsewhere.

## License

MIT, see [LICENSE](LICENSE). It covers this repository's own code and not
`third_party/`.
