cmake_minimum_required(VERSION 3.16)
project(dwritecore_shim VERSION 0.1.0 LANGUAGES C CXX)

# The implementation is an Android x86-64 build, and bionic-compat/ is written
# against that ABI - R_X86_64_* relocations and the %fs:0x28 stack guard. There
# is nothing to fall back to on another architecture, so say so here rather
# than at link time, where the failure is a wall of undefined symbols.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR
    "Only x86-64 is currently supported: the vendored implementation is an Android "
    "x86-64 build. This is ${CMAKE_SYSTEM_PROCESSOR}.")
endif()

# Every generator this build runs is Python. Finding the interpreter beats a
# bare `python3`, which is not on PATH everywhere and cannot be pointed
# elsewhere.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# fontconfig here and freetype further down both need it, and asking twice
# does nothing but repeat the search.
find_package(PkgConfig QUIET)

# Generated once here so the first build has it, and again on every build so
# the git revision follows the tree.
set(DWRITECORE_VERSION_HEADER "${CMAKE_BINARY_DIR}/generated/cleartype_version.h")
set(DWRITECORE_VERSION_COMMAND "${CMAKE_COMMAND}"
  "-DSRC=${CMAKE_SOURCE_DIR}" "-DOUT=${DWRITECORE_VERSION_HEADER}"
  "-DVERSION=${PROJECT_VERSION}"
  -P "${CMAKE_SOURCE_DIR}/cmake/version.cmake")
execute_process(COMMAND ${DWRITECORE_VERSION_COMMAND})
add_custom_target(dwritecore_version ALL
  COMMAND ${DWRITECORE_VERSION_COMMAND}
  BYPRODUCTS "${DWRITECORE_VERSION_HEADER}"
  VERBATIM)

# So a stripped library still names the debug file that describes it.
add_link_options(-Wl,--build-id=sha1)

# C++20 for the designated initializers. There are about 2,400 of them, most
# in the generated data tables, and under C++17 every one is a compiler
# extension that -Wpedantic reports. Nothing else here needs C++20, and no
# C++20 library feature is used.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# So clangd can resolve the DirectWrite and FreeType headers. Without a
# compile database it parses these files with default flags, fails to find
# dwrite_core.h and ft2build.h, and silently degrades every unknown type to
# implicit int, which it then answers hover and go-to-definition from.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings, on by default
set(DWC_WARNINGS -Wall -Wextra -Wpedantic)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND DWC_WARNINGS -Weverything
    # Warn about using anything newer than an old standard. This project is
    # C++20 and C23 by choice; these fire on the choice, not on a defect.
    -Wno-c++98-compat -Wno-c++98-compat-pedantic
    -Wno-pre-c++17-compat -Wno-pre-c++20-compat -Wno-pre-c++20-compat-pedantic
    -Wno-pre-c11-compat -Wno-c++20-compat
    # A destructor slot in a COM interface would move every method after it.
    # tests/vtable_test.cpp checks 1819 of those slots.
    -Wno-non-virtual-dtor -Wno-delete-non-abstract-non-virtual-dtor
    # bionic-compat/ *is* libc: the names are reserved because they are the
    # ones being replaced. Same reason CppUseInternalLinkage is off.
    -Wno-reserved-identifier -Wno-reserved-macro-identifier
    # Would mean std::span across a raw-pointer COM ABI.
    -Wno-unsafe-buffer-usage -Wno-unsafe-buffer-usage-in-libc-call
    -Wno-unsafe-buffer-usage-in-container -Wno-unsafe-buffer-usage-in-format-attr-call
    # The loader hooks: the constructors in cleartype/src/prefs.cpp,
    # cleartype/src/freetype.cpp and bionic-compat/.
    -Wno-global-constructors -Wno-exit-time-destructors
    # Structure layout, and a style preference about switch defaults.
    -Wno-padded -Wno-switch-default
    # Declaration-after-statement is a C89 rule and this is C23. c++-keyword
    # fires on C code naming C types that C++ made keywords - wmemchr's
    # wchar_t. nrvo asks for a copy elision that cannot happen: the locals it
    # names are passed by address to a fill-in function first.
    -Wno-declaration-after-statement -Wno-c++-keyword -Wno-nrvo
    # Asks for every enumerator of a third-party enum to be spelled out even
    # where `default:` is the right answer, and contradicts
    # -Wcovered-switch-default. -Wswitch, from -Wall, still catches a switch
    # that handles neither.
    -Wno-switch-enum
    # Interfaces defined inline in a header have no other translation unit to
    # anchor a vtable in; that is what a mirrored SDK header is.
    -Wno-weak-vtables)
else()
  list(APPEND DWC_WARNINGS
    -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion -Wformat=2
    -Wcast-qual -Wcast-align -Wundef -Wredundant-decls -Wmissing-declarations
    -Wlogical-op -Wduplicated-cond -Wduplicated-branches -Wnull-dereference
    -Wfloat-equal -Wmissing-noreturn -Wunused-macros)
  # No -Wuseless-cast: every one it reports here is a cast that makes a printf
  # argument match its conversion - (int)getpid() for %d - or one that crosses
  # the bionic/host ABI. They are useless only where the typedef happens to
  # match, and dropping them would be a portability defect that -Wformat
  # reports on the platforms where it does not.
  # No -Wswitch-enum, for the same reason as in the clang set above.
endif()

# C++-only: gcc reports these as "valid for C++ but not for C" on a C file.
add_compile_options(${DWC_WARNINGS}
  "$<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# The shim
#
# Named libdwritecore.so so it can stand in for the stock library, which
# exports only two symbols. It forwards to a SONAME-renamed copy of the real
# implementation produced by tools/make_impl.py.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Debug info, kept out of the library it describes.
#
# Off by default. A shipped build then carries no DWARF, and the symbol table
# alone answers a crash address with a function name and no line number.
# Turning this on gives the same optimized code, the DWARF beside it in
# <library>.debug, and a shipped file smaller than before, since the split
# takes the symbol table with it.
#
# -g is added rather than required from the build type, so Release stays
# Release. It changes what the compiler records, never what it emits.
#
# GDB, addr2line and the crash reporters read .gnu_debuglink and look in the
# library's own directory first, so keeping the two side by side is all it
# takes. A distribution that wants them under /usr/lib/debug/.build-id can
# move them there; the build id is already in both files.
#
# Called from the directory that created the target, which is where a
# POST_BUILD command can attach.
# ---------------------------------------------------------------------------

option(DWRITECORE_SPLIT_DEBUG
       "Write each library's DWARF to <library>.debug and strip the library" OFF)

function(dwritecore_split_debug target)
  if(NOT DWRITECORE_SPLIT_DEBUG)
    return()
  endif()
  if(NOT CMAKE_OBJCOPY OR NOT CMAKE_STRIP)
    message(WARNING "objcopy or strip not found; leaving the debug info in ${target}")
    return()
  endif()
  target_compile_options(${target} PRIVATE -g)
  # -R .gnu_debuglink so that rebuilding over a previous split does not leave
  # the old link in place beside the new one.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug
            "$<TARGET_FILE:${target}>" "$<TARGET_FILE:${target}>.debug"
    COMMAND "${CMAKE_STRIP}" -g -x -R .gnu_debuglink "$<TARGET_FILE:${target}>"
    COMMAND "${CMAKE_OBJCOPY}" "--add-gnu-debuglink=$<TARGET_FILE_NAME:${target}>.debug"
            "$<TARGET_FILE:${target}>"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
    COMMENT "Splitting debug info out of ${target}"
    VERBATIM)
endfunction()

add_library(dwritecore SHARED
  src/dwritecore_shim.cpp
  # The system font collection. The shipped implementation cannot enumerate
  # fonts - it has no readdir - so DWriteCoreCreateFactory hands back a proxy
  # that answers the system-font getters from fontconfig's list instead.
  # fontconfig is dlopen'd, not linked, so the single-file packaging holds.
  src/factory_proxy_forward.cpp
  src/factory_proxy_overrides.cpp
  src/system_fonts.cpp
  src/system_fallback.cpp
  src/fallback_data.cpp
)

# SYSTEM: include/ mirrors the Windows SDK headers, which are Microsoft's code
# and are kept byte-comparable with them, so a warning there is not something
# this project can act on. They account for many of the
# warnings the set below turns on, all of them in the verbatim headers
# (winnt.h, guiddef.h, winerror.h, dwrite_*.h, minwindef.h, rpc.h,
# unknwn.h) and none in the reconstructed parts of compat.h.
target_include_directories(dwritecore SYSTEM PUBLIC include)
target_include_directories(dwritecore PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(dwritecore dwritecore_version)
target_link_libraries(dwritecore PRIVATE dl pthread)

set_target_properties(dwritecore PROPERTIES
  OUTPUT_NAME dwritecore
  # No SOVERSION: the file must be exactly libdwritecore.so.
  NO_SONAME OFF
  # The five entry points and nothing else - see src/dwritecore.map for why
  # the rest must not be in the dynamic symbol table.
  LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/dwritecore.map"
)

# ---------------------------------------------------------------------------
# SONAME-renamed copy of the stock implementation.
# ---------------------------------------------------------------------------

set(DWRITECORE_ORIGINAL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/office-android/libdwritecore.so"
    CACHE FILEPATH "Path to the stock libdwritecore.so")

if(EXISTS "${DWRITECORE_ORIGINAL}")
  add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/libdwcoreimpl.so"
    COMMAND ${CMAKE_COMMAND} -E echo "Renaming DT_SONAME of the stock implementation"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_impl.py"
            "${DWRITECORE_ORIGINAL}" "${CMAKE_BINARY_DIR}/libdwcoreimpl.so"
    DEPENDS "${DWRITECORE_ORIGINAL}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_impl.py"
    COMMENT "Producing libdwcoreimpl.so"
    VERBATIM
  )
  add_custom_target(dwritecore_impl ALL
    DEPENDS "${CMAKE_BINARY_DIR}/libdwcoreimpl.so")
else()
  message(FATAL_ERROR
    "The stock implementation was not found at:\n"
    "    ${DWRITECORE_ORIGINAL}\n"
    "It ships in third_party/office-android/. Restore it, or point "
    "-DDWRITECORE_ORIGINAL= at a copy.")
endif()

# ---------------------------------------------------------------------------
# bionic compatibility stubs
#
# libdwritecore.so is an Android build: it needs liblog/libc/libm/libdl with
# bionic SONAMEs, and every import is versioned @LIBC, which glibc can never
# satisfy. These four stubs carry the right SONAMEs and declare that version.
#
# The shim loads them by absolute path with RTLD_LOCAL, so their pthread
# symbols cannot interpose on the host program's own threading.
# ---------------------------------------------------------------------------

function(add_bionic_stub name soname source map)
  add_library(${name} SHARED ${source})
  target_include_directories(${name} PRIVATE bionic-compat)
  # -fno-builtin: these files define memset/memcpy/strlen, and without it the
  # compiler rewrites each body into a call to itself.
  target_compile_options(${name} PRIVATE -fno-builtin -fno-lto -Wno-attributes)
  target_link_libraries(${name} PRIVATE dl)
  set_target_properties(${name} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bionic"
    OUTPUT_NAME ${name}
    PREFIX "" SUFFIX ""
    LINK_FLAGS "-Wl,-soname,${soname} -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/bionic-compat/${map}")
  set_target_properties(${name} PROPERTIES OUTPUT_NAME "${soname}")
endfunction()

add_bionic_stub(bionic_c   libc.so   bionic-compat/libc_compat.c   libc.map)
add_bionic_stub(bionic_m   libm.so   bionic-compat/libm_compat.c   libm.map)
add_bionic_stub(bionic_dl  libdl.so  bionic-compat/libdl_compat.c  libdl.map)
add_bionic_stub(bionic_log liblog.so bionic-compat/liblog_compat.c liblog.map)

add_dependencies(dwritecore bionic_c bionic_m bionic_dl bionic_log)
# Both this target (through dwc_blobs.S) and dwritecore_impl consume
# libdwcoreimpl.so, which a custom command produces. With a Makefile generator
# each target carries its own copy of that rule, so under -j they can both run
# make_impl.py at once, over the same output file, and one of them reads it
# half-written. Ordering them is what makes a parallel build deterministic.
if(TARGET dwritecore_impl)
  add_dependencies(dwritecore dwritecore_impl)
endif()

# ---------------------------------------------------------------------------
# Single-file packaging
#
# With this on (the default) the implementation and the four bionic stubs are
# embedded in libdwritecore.so and loaded from an anonymous in-memory file at
# run time, so only one file has to ship. Turn it off to keep them as separate
# files beside the library, which is friendlier while developing: the
# implementation can then be swapped with DWRITECORE_IMPL_PATH without
# relinking, and a debugger can map the code back to a file.
# ---------------------------------------------------------------------------

option(DWRITECORE_EMBED_IMPL
       "Embed the implementation and bionic stubs into libdwritecore.so" ON)

if(DWRITECORE_EMBED_IMPL AND EXISTS "${DWRITECORE_ORIGINAL}")
  set(DWC_BLOBS "${CMAKE_BINARY_DIR}/dwc_blobs.S")

  set(DWC_BLOB_NAMES impl libc libm libdl liblog)
  set(DWC_BLOB_FILES "${CMAKE_BINARY_DIR}/libdwcoreimpl.so")
  foreach(stub bionic_c bionic_m bionic_dl bionic_log)
    get_target_property(stub_dir ${stub} LIBRARY_OUTPUT_DIRECTORY)
    get_target_property(stub_name ${stub} OUTPUT_NAME)
    list(APPEND DWC_BLOB_FILES "${stub_dir}/${stub_name}")
  endforeach()

  set(DWC_BLOB_TEXT "/* GENERATED by CMakeLists.txt - do not edit. */\n\n")
  string(APPEND DWC_BLOB_TEXT "    .section .rodata.dwc_blobs, \"a\"\n\n")
  list(LENGTH DWC_BLOB_NAMES DWC_BLOB_COUNT)
  math(EXPR DWC_BLOB_LAST "${DWC_BLOB_COUNT} - 1")
  foreach(i RANGE ${DWC_BLOB_LAST})
    list(GET DWC_BLOB_NAMES ${i} name)
    list(GET DWC_BLOB_FILES ${i} file)
    string(APPEND DWC_BLOB_TEXT
      "    .globl dwc_blob_${name}_start\n"
      "    .globl dwc_blob_${name}_end\n"
      "    .balign 16\n"
      "dwc_blob_${name}_start:\n"
      "    .incbin \"${file}\"\n"
      "dwc_blob_${name}_end:\n"
      "    .byte 0\n\n")
  endforeach()

  # file(GENERATE) leaves the file alone when the content has not changed, so
  # configuring again does not set off a rebuild.
  file(GENERATE OUTPUT "${DWC_BLOBS}" CONTENT "${DWC_BLOB_TEXT}")

  enable_language(ASM)
  target_sources(dwritecore PRIVATE "${DWC_BLOBS}")
  target_compile_definitions(dwritecore PRIVATE DWRITECORE_EMBEDDED)
  # The text names the blobs and does not change with them, so what has to be
  # reassembled when one is rebuilt is said here instead.
  set_source_files_properties("${DWC_BLOBS}" PROPERTIES
    LANGUAGE ASM
    GENERATED TRUE
    OBJECT_DEPENDS "${DWC_BLOB_FILES}")
  add_dependencies(dwritecore dwritecore_impl bionic_c bionic_m bionic_dl bionic_log)
endif()

# ---------------------------------------------------------------------------
# ABI conformance checks and the mock-vtable test.
#
# The mock implements the COM interfaces by hand with explicit vtable slot
# numbers and asserts that a call through each interface reaches the slot the
# headers' declaration order predicts. That is what validates the vtable
# reconstruction without needing to run the bionic-linked implementation.
# ---------------------------------------------------------------------------

enable_testing()

# ---------------------------------------------------------------------------
# Fonts for the tests
#
# The three tests that need a real font ask fontconfig for it, because the
# layout under /usr/share/fonts is a packaging decision and differs per
# distribution. Where fontconfig is missing, or has nothing suitable, they exit
# 77, which SKIP_RETURN_CODE below turns into ctest's Skipped state - visible
# in the summary, unlike a pass for a test that never ran.
# ---------------------------------------------------------------------------

if(PkgConfig_FOUND)
  pkg_check_modules(FONTCONFIG IMPORTED_TARGET fontconfig)
endif()
if(NOT TARGET PkgConfig::FONTCONFIG)
  message(STATUS
    "fontconfig not found; the cleartype, thread and shaping tests will "
    "report Skipped rather than running")
endif()

function(dwc_font_test target)
  target_include_directories(${target} PRIVATE tests)
  if(TARGET PkgConfig::FONTCONFIG)
    target_link_libraries(${target} PRIVATE PkgConfig::FONTCONFIG)
    target_compile_definitions(${target} PRIVATE HAVE_FONTCONFIG)
  endif()
  set_tests_properties(${target} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

add_executable(abi_check tests/abi_check.cpp)
target_link_libraries(abi_check PRIVATE dwritecore)
add_test(NAME abi_check COMMAND abi_check)

add_executable(vtable_test tests/vtable_test.cpp)
target_link_libraries(vtable_test PRIVATE dwritecore)
add_test(NAME vtable_test COMMAND vtable_test)

# End-to-end: loads the real implementation through the bionic stubs and
# rasterizes a glyph run. Reports Skipped, not Passed, when fontconfig
# finds no font to run it with.
add_executable(cleartype_test tests/cleartype_test.cpp)
target_link_libraries(cleartype_test PRIVATE dwritecore)
add_test(NAME cleartype_test COMMAND cleartype_test)
dwc_font_test(cleartype_test)

# Concurrency stress for the futex-based bionic locks.
add_executable(thread_test tests/thread_test.cpp)
target_link_libraries(thread_test PRIVATE dwritecore pthread)
add_test(NAME thread_test COMMAND thread_test)
dwc_font_test(thread_test)

# Complex script shaping: Arabic, CJK, Hebrew, Devanagari. Also the only test
# where DWriteCore calls back into caller-implemented COM interfaces.
add_executable(shaping_test tests/shaping_test.cpp)
target_link_libraries(shaping_test PRIVATE dwritecore)
add_test(NAME shaping_test COMMAND shaping_test)
dwc_font_test(shaping_test)

# ---------------------------------------------------------------------------
# The FreeType interposer
#
# Optional, needs FreeType. Built by default when it is present.
# ---------------------------------------------------------------------------

dwritecore_split_debug(dwritecore)

option(DWRITECORE_BUILD_CLEARTYPE "Build the FreeType LD_PRELOAD interposer" ON)

if(DWRITECORE_BUILD_CLEARTYPE)
  # pkg-config first, since freetype2.pc carries the flags the library was
  # built with. CMake's own finder second, so a machine that has the headers
  # but not pkg-config still builds.
  if(PkgConfig_FOUND)
    pkg_check_modules(FREETYPE IMPORTED_TARGET freetype2)
  endif()
  if(TARGET PkgConfig::FREETYPE)
    set(DWRITECORE_FREETYPE PkgConfig::FREETYPE)
  else()
    find_package(Freetype)
    if(TARGET Freetype::Freetype)
      set(DWRITECORE_FREETYPE Freetype::Freetype)
    endif()
  endif()

  # This option is on by default, so reaching here with nothing to build
  # against is a configuration error and not a thing to carry on past.
  if(NOT DWRITECORE_FREETYPE)
    message(FATAL_ERROR
      "FreeType not found. libcleartype.so needs its development headers, "
      "which most distributions package apart from the library itself; "
      "README.md names the package. Configure with "
      "-DDWRITECORE_BUILD_CLEARTYPE=OFF to build everything else without it.")
  endif()

  # freetype2.pc carries a libtool version and not the release number, so the
  # one thing the shim needs from a recent FreeType is compiled for instead.
  # FT_GlyphSlot gained glyph_index in 2.10, where the field had been called
  # reserved, and cleartype/src/freetype.cpp reads it on every glyph load.
  # Without this the build reaches that file and stops with an error naming a
  # struct member, which says nothing about what to install.
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state()
  if(FREETYPE_INCLUDE_DIRS)
    set(CMAKE_REQUIRED_INCLUDES ${FREETYPE_INCLUDE_DIRS})
  endif()
  check_cxx_source_compiles("
#include <ft2build.h>
#include FT_FREETYPE_H
int main() { FT_GlyphSlotRec slot; (void)slot.glyph_index; return 0; }
" DWRITECORE_FREETYPE_2_10)
  cmake_pop_check_state()

  if(NOT DWRITECORE_FREETYPE_2_10)
    message(FATAL_ERROR
      "FreeType is too old. libcleartype.so needs 2.10 or newer, which is "
      "where FT_GlyphSlot gained glyph_index. Configure with "
      "-DDWRITECORE_BUILD_CLEARTYPE=OFF to build everything else without it.")
  endif()
  add_subdirectory(cleartype)
endif()

# ---------------------------------------------------------------------------
# The Chromium/Electron vtable-patch prototype
#
# Off by default as it targets a mechanism (see chromium/src/vtable_patch.cpp),
# not a finished parity path, and its own vtable-slot selection is specific
# to the fontations_ffi bridge that only exists in a Chromium/Electron build.
# ---------------------------------------------------------------------------

option(DWRITECORE_BUILD_CHROMIUM_PROTOTYPE
  "Build the experimental Chromium/Electron vtable-patch prototype" OFF)

if(DWRITECORE_BUILD_CHROMIUM_PROTOTYPE)
  enable_language(ASM)
  add_subdirectory(chromium)
endif()

# ---------------------------------------------------------------------------
# Installing.
#
# Both files go into one directory: the shim has libdwritecore.so in its
# DT_NEEDED and finds it through $ORIGIN, so the pair works from wherever it is
# put and from nowhere else. The build tree additionally carries its own
# absolute path in RUNPATH, which is right for running out of the build
# directory and wrong for anything installed, so the installed copies drop it.
#
# There is no CMAKE_INSTALL_PREFIX default. The probe, the samples and the
# tests are not installed.
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

set(DWRITECORE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/dwritecore"
    CACHE STRING "Directory under the prefix that both libraries are installed into")



install(TARGETS dwritecore
        LIBRARY DESTINATION "${DWRITECORE_INSTALL_DIR}")

if(TARGET cleartype)
  install(TARGETS cleartype
          LIBRARY DESTINATION "${DWRITECORE_INSTALL_DIR}")
endif()

if(DWRITECORE_SPLIT_DEBUG)
  install(FILES "$<TARGET_FILE:dwritecore>.debug"
          DESTINATION "${DWRITECORE_INSTALL_DIR}")
  if(TARGET cleartype)
    install(FILES "$<TARGET_FILE:cleartype>.debug"
            DESTINATION "${DWRITECORE_INSTALL_DIR}")
  endif()
endif()
