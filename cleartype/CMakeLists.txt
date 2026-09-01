find_package(Threads REQUIRED)

# The Chromium half needs an assembler for its vtable thunks.
enable_language(ASM)

# OFF leaves the Chromium patch out altogether. ON compiles it in; it still
# applies only where its targets are found, which each patch decides for
# itself, and only when CLEARTYPE_CHROMIUM says so at run time.
option(CLEARTYPE_CHROMIUM_PARITY
  "Reproduce Chromium-on-Windows font selection, metrics and rasterization" ON)

# Chromium, Electron and the Electron applications. Gecko is patched through
# libxul_patch.cpp instead; these reach Skia's scaler context. See
# src/chromium/vtable_patch.cpp.
set(cleartype_chromium_sources
  src/chromium/vtable_patch.cpp
  src/chromium/windows_path.cpp
  src/chromium/typeface_bridge.cpp
  src/chromium/font_facts.cpp
  src/chromium/dwrite_raster.cpp
  src/chromium/parity_gate.cpp
  src/chromium/render_params.cpp
  src/chromium/system_fonts.cpp
  src/chromium/fallback_order.cpp
  src/chromium/family_match.cpp
  src/chromium/bold_fallback.cpp
  src/chromium/bold_shaping.cpp
  src/chromium/hb_abi.cpp
  src/chromium/generic_families.cpp
  src/chromium/fallback_hook.cpp
  src/chromium/static_fontconfig.cpp
  src/chromium/cef_system_font.cpp
  src/chromium/bold_weight.cpp
  src/chromium/colr_outline.cpp
  src/chromium/weight_style.cpp
  src/chromium/hook_thunk.S)
if(NOT CLEARTYPE_CHROMIUM_PARITY)
  set(cleartype_chromium_sources)
endif()

add_library(cleartype SHARED
  src/freetype.cpp
  src/libxul_patch.cpp
  src/fontconfig.cpp
  src/prefs.cpp
  src/parity_mode.cpp
  ${cleartype_chromium_sources})

# OFF leaves the parity paths out altogether, for a plain FreeType-to-
# DirectWrite interposer. ON compiles them in; they still apply only to a Gecko
# process, which parity_mode.cpp decides at runtime.
option(CLEARTYPE_FIREFOX_PARITY
  "Reproduce Firefox-on-Windows glyph advances, metrics and synthetic oblique" ON)

target_compile_definitions(cleartype
  PRIVATE CLEARTYPE_FIREFOX_PARITY=$<BOOL:${CLEARTYPE_FIREFOX_PARITY}>
  PRIVATE CLEARTYPE_CHROMIUM_PARITY=$<BOOL:${CLEARTYPE_CHROMIUM_PARITY}>
  # IDWriteFactory5 for the in-memory font file loader and IDWriteFactory2 for
  # the grid-fit and antialias arguments to CreateGlyphRunAnalysis are gated in
  # dwrite_3.h behind DWRITE_CORE or a Windows 10 NTDDI version.
  PRIVATE DWRITE_CORE=1)

target_link_libraries(cleartype
  PRIVATE dwritecore ${DWRITECORE_FREETYPE} Threads::Threads ${CMAKE_DL_LIBS})

target_include_directories(cleartype
  PRIVATE "${CMAKE_BINARY_DIR}/generated"
  # src/chromium reaches windows_fonts.h and parity_mode.h beside it.
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PRIVATE "${CMAKE_SOURCE_DIR}/include")
add_dependencies(cleartype dwritecore_version)

set_target_properties(cleartype PROPERTIES
  OUTPUT_NAME cleartype
  # Next to libdwritecore.so, which this library has in its DT_NEEDED.
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  # Find libdwritecore.so next to this file, or one directory up, so the pair
  # keeps working wherever it is copied. CMake still appends the build-tree
  # path, so a run from the build directory works either way.
  BUILD_RPATH "\$ORIGIN;\$ORIGIN/.."
  INSTALL_RPATH "\$ORIGIN"
  # A preloaded library's exports go to the front of the entire process's
  # symbol lookup scope, so everything but the interposed FreeType entry
  # points is kept out of the dynamic symbol table entirely.
  LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cleartype.map")

dwritecore_split_debug(cleartype)

# Logging only: whether LCD calls reach FreeType at all, which face constructor
# is in use, whether a transform is set.
add_library(cleartype_probe SHARED probes/probe.c)
target_link_libraries(cleartype_probe PRIVATE ${DWRITECORE_FREETYPE} ${CMAKE_DL_LIBS})
set_target_properties(cleartype_probe PROPERTIES OUTPUT_NAME cleartype-probe)

# Logging only: which of cairo's five text entry points an application uses.
# Attaching to the wrong one fails silently. Skipped when cairo development
# files are absent.
pkg_check_modules(CAIRO IMPORTED_TARGET cairo)
if(TARGET PkgConfig::CAIRO)
  add_library(cleartype_cairo_probe SHARED probes/cairo_probe.c)
  target_link_libraries(cleartype_cairo_probe PRIVATE PkgConfig::CAIRO ${CMAKE_DL_LIBS})
  set_target_properties(cleartype_cairo_probe PROPERTIES
    OUTPUT_NAME cleartype-cairo-probe
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# Fall-through contract test. Must be run under real LD_PRELOAD, and must pass
# both normally and with CLEARTYPE_FORCE_FALLBACK=1. Not a ctest case, because
# ctest does not set up LD_PRELOAD per test.
add_executable(cleartype_test_fallback tests/test_fallback.c)
target_link_libraries(cleartype_test_fallback PRIVATE ${DWRITECORE_FREETYPE})

# Visual check: renders a line of text through plain FreeType to a PPM. Run it
# with and without LD_PRELOAD and compare the two images.
add_executable(cleartype_sample tests/sample.c)
target_link_libraries(cleartype_sample PRIVATE ${DWRITECORE_FREETYPE})
