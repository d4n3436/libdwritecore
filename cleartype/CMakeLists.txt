find_package(Threads REQUIRED)

add_library(cleartype SHARED
  src/freetype.cpp
  src/libxul_patch.cpp
  src/fontconfig.cpp
  src/prefs.cpp
  src/parity_mode.cpp)

# OFF leaves the parity paths out altogether, for a plain FreeType-to-
# DirectWrite interposer. ON compiles them in; they still apply only to a Gecko
# process, which parity_mode.cpp decides at runtime.
option(DWRITECORE_FIREFOX_PARITY
  "Reproduce Firefox-on-Windows glyph advances, metrics and synthetic oblique" ON)

target_compile_definitions(cleartype
  PRIVATE DWRITECORE_FIREFOX_PARITY=$<BOOL:${DWRITECORE_FIREFOX_PARITY}>)

target_link_libraries(cleartype
  PRIVATE dwritecore ${DWRITECORE_FREETYPE} Threads::Threads ${CMAKE_DL_LIBS})

target_include_directories(cleartype PRIVATE "${CMAKE_BINARY_DIR}/generated")
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
