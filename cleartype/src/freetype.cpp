//+--------------------------------------------------------------------------
//
//  freetype.cpp - libcleartype.so
//
//  An LD_PRELOAD interposer that replaces FreeType's LCD rasterization with
//  DWriteCore's. Every FT_Render_Glyph(slot, FT_RENDER_MODE_LCD) is answered
//  by re-rasterizing that glyph through IDWriteGlyphRunAnalysis::
//  CreateAlphaTexture with DWRITE_TEXTURE_CLEARTYPE_3x1, handed back in the
//  FT_PIXEL_MODE_LCD layout the caller already composites. Every other render
//  mode is forwarded untouched.
//
//  Every failure falls through to the real FT_Render_Glyph - unknown face,
//  unsupported font, allocation failure, any failed HRESULT - and slot->bitmap
//  is never left half-written.
//
//----------------------------------------------------------------------------

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_OUTLINE_H
#include FT_GLYPH_H
#include FT_SYNTHESIS_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SYSTEM_H

#include "cleartype_version.h"
#include "dwrite_core.h"
#include "dwritecore_shim.h"
#include "parity_mode.h"
#include "shim_exports.h"

#if CLEARTYPE_FIREFOX_PARITY
#  include "firefox_parity_data.h"
#endif

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <vector>

// Everything here hangs off GetFactories(), which reaches the implementation
// through dlopen and dlsym (GetImpl in src/dwritecore_shim.cpp). Data flow
// analysis has no model for that, so it reads the handle as null and concludes
// the factory always is too, which makes the whole rasterizer look dead.
// ReSharper disable CppDFAConstantParameter
// ReSharper disable CppDFANullDereference
// ReSharper disable CppDFAUnreachableCode
// ReSharper disable CppDFAUnreachableFunctionCall
// ReSharper disable CppDFAConstantConditions
// ReSharper disable CppDFAConstantFunctionResult
// ReSharper disable CppDFAUnusedValue

// FreeType's handles - FT_Face, FT_Library, FT_GlyphSlot, FT_Size_Request -
// are typedefs for pointers, so const-qualifying a parameter of one qualifies
// the handle and not what it points at, which clang-tidy's
// misc-misplaced-const then reports. The locals holding a resolved ft_*_fn are
// pointers too.
// ReSharper disable CppParameterMayBeConst
// ReSharper disable CppLocalVariableMayBeConst

// Every comparison this reports asks whether a value is exactly a particular
// one - is the scale exactly 1, is the size exactly 0, is there any subpixel
// offset at all - and one of them is Firefox's own == against a descent.
#pragma GCC diagnostic ignored "-Wfloat-equal"

// The type of an atomic is what says how the field is shared, so it is spelled
// out rather than deduced.
// ReSharper disable CppTemplateArgumentsCanBeDeduced

// What is left groups an addition inside a mask, a shift inside a comparison
// or one ternary inside another, where the precedence is the thing a reader
// should not have to recall.
// ReSharper disable CppRedundantParentheses

// Each block here reads against the Firefox and Skia source it was translated
// from, and both of these would reshape it away from the original.
// ReSharper disable CppTooWideScopeInitStatement
// ReSharper disable CppUseStructuredBinding

namespace {

// ---------------------------------------------------------------------------
// Real FreeType entry points.
//
// Resolved through RTLD_NEXT, which is the right direction only because this
// library is genuinely preloaded. A library brought in later by dlopen() sits
// after the executable's own dependencies in the search order, so the same
// lookup from there finds the wrong thing, or nothing. The shim also links
// libfreetype directly, which guarantees the object is in the link map even
// for a host that would otherwise dlopen it.
//
// That guarantee covers the link map, not the global scope. A host that
// dlopens its own copy without RTLD_GLOBAL, which is the default, keeps it
// out of the scope RTLD_NEXT walks, and the answer is then null for a call
// this library has already intercepted. fontconfig.cpp carries the same note,
// and there it crashes a JVM. Asking the object directly is the fallback.
// ---------------------------------------------------------------------------

// One walk of the link map, looking for an importer of FreeType that is not
// this library. Split out so it can be asked more than once; see
// HostUsesThisFreeType below for why once is not enough.
bool LinkMapImportsFreeType()
{
    // Any image in the link map that imports FreeType, other than this
    // one. dlsym cannot answer, since this library exports FT_Load_Glyph
    // and the global scope therefore always has one.
    //
    // The main image alone is not enough. Firefox's executable is a
    // launcher whose DT_NEEDED lists only libc and libstdc++, while
    // libxul.so carries the FreeType dependency, so asking the main image
    // stands the rasterizer down in every Gecko process.
    Dl_info self{};
    const void* self_base = nullptr;
    if (dladdr(reinterpret_cast<const void*>(&LinkMapImportsFreeType), &self) != 0) {
        self_base = self.dli_fbase;
    }

    struct Ask
    {
        const void* self_base;
        bool found;
    } ask{ .self_base = self_base, .found = false };

    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            auto* a = static_cast<Ask*>(data);
            if (a->self_base != nullptr &&
                reinterpret_cast<const void*>(info->dlpi_addr) == a->self_base) {
                return 0;           // this library's own dependency
            }
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                if (info->dlpi_phdr[i].p_type != PT_DYNAMIC) {
                    continue;
                }
                const auto* dyn = reinterpret_cast<const ElfW(Dyn)*>(
                    info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
                const char* strtab = nullptr;
                for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
                    if (d->d_tag == DT_STRTAB) {
                        strtab = reinterpret_cast<const char*>(d->d_un.d_ptr);
                    }
                }
                if (strtab == nullptr) {
                    continue;
                }
                for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
                    if (d->d_tag == DT_NEEDED &&
                        std::strncmp(strtab + d->d_un.d_val, "libfreetype", 11) == 0) {
                        a->found = true;
                        return 1;
                    }
                }
            }
            return 0;
        },
        &ask);
    return ask.found;
}

// Whether the FreeType this library interposes is the one the host draws
// with. A build that compiles FreeType in imports none of its symbols, so the
// host's calls never reach these entry points and the only callers left are
// other libraries in the process drawing their own widgets. Serving those
// rasterizes text the host never asked about, against face state that belongs
// to nobody, so the interposer stands down instead.
//
// True once anything in the process has been seen to import FreeType. Only a
// true is ever stored: the answer can go from false to true when the host
// dlopens a library, and never the other way.
std::atomic<bool> g_host_uses_freetype{false};
std::atomic<unsigned> g_host_scan_calls{0};

bool HostUsesThisFreeType()
{
    if (g_host_uses_freetype.load(std::memory_order_acquire)) {
        return true;
    }
    // The first scan runs while this library loads, before the host has opened
    // anything of its own, so a launcher that dlopens its shared library later
    // has no importer in the link map yet. ParityActive answers from the
    // installation layout instead, which is known at that point, and is itself
    // monotone.
    if (dwcft::ParityActive()) {
        g_host_uses_freetype.store(true, std::memory_order_release);
        return true;
    }
    // Nothing said yes yet, so ask the link map again - it is a different link
    // map every time the host opens something. Walking it costs the loader
    // lock, so the answer is only re-sought on calls 1, 2, 4, 8 and so on: a
    // process that really has no FreeType importer settles into doing almost
    // nothing, while one that gains an importer at any point still finds it.
    const unsigned n = g_host_scan_calls.fetch_add(1, std::memory_order_relaxed);
    if ((n & (n + 1)) != 0) {
        return false;
    }
    if (!LinkMapImportsFreeType()) {
        return false;
    }
    g_host_uses_freetype.store(true, std::memory_order_release);
    return true;
}

// Whether the interposer should do anything at all in this process.
//
// The master switch is an environment variable and cannot change under a
// running process; the other half can, so it is asked every time.
//
// Settles the options on the way past. Every gated entry point asks this
// first, and the first read of the options is what opens CLEARTYPE_LOG, so
// doing it here keeps the log open even for a call that declines.
struct Options;
const Options& GetOptions();

bool InterposerWanted()
{
    (void)GetOptions();
    static const bool enabled = dwcft::Enabled();
    return enabled && HostUsesThisFreeType();
}

// Same rule as DEFINE_REAL: a failed open is retried rather than remembered,
// since the library can arrive after the first ask.
void* FreeTypeSymbol(const char* name)
{
    static std::atomic<void*> library{nullptr};
    void* handle = library.load(std::memory_order_acquire);
    if (handle == nullptr) {
        handle = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        if (handle == nullptr) {
            handle = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
        }
        if (handle != nullptr) {
            library.store(handle, std::memory_order_release);
        }
    }
    return handle != nullptr ? dlsym(handle, name) : nullptr;
}

using ft_render_glyph_fn = FT_Error (*)(FT_GlyphSlot, FT_Render_Mode);
using ft_new_face_fn = FT_Error (*)(FT_Library, const char*, FT_Long, FT_Face*);
using ft_new_memory_face_fn = FT_Error (*)(FT_Library, const FT_Byte*, FT_Long, FT_Long, FT_Face*);
using ft_open_face_fn = FT_Error (*)(FT_Library, const FT_Open_Args*, FT_Long, FT_Face*);
using ft_reference_face_fn = FT_Error (*)(FT_Face);
using ft_done_face_fn = FT_Error (*)(FT_Face);
using ft_done_freetype_fn = FT_Error (*)(FT_Library);
using ft_set_transform_fn = void (*)(FT_Face, FT_Matrix*, FT_Vector*);
using ft_outline_translate_fn = void (*)(const FT_Outline*, FT_Pos, FT_Pos);
using ft_outline_done_fn = FT_Error (*)(FT_Library, FT_Outline*);
using ft_done_glyph_fn = void (*)(FT_Glyph);
using ft_glyphslot_embolden_fn = void (*)(FT_GlyphSlot);
using ft_glyphslot_oblique_fn = void (*)(FT_GlyphSlot);
using ft_outline_embolden_fn = FT_Error (*)(FT_Outline*, FT_Pos);
using ft_outline_embolden_xy_fn = FT_Error (*)(FT_Outline*, FT_Pos, FT_Pos);
using ft_outline_transform_fn = void (*)(const FT_Outline*, const FT_Matrix*);
using ft_load_glyph_fn = FT_Error (*)(FT_Face, FT_UInt, FT_Int32);
using ft_load_char_fn = FT_Error (*)(FT_Face, FT_ULong, FT_Int32);
using ft_outline_get_bitmap_fn = FT_Error (*)(FT_Library, FT_Outline*, const FT_Bitmap*);
using ft_outline_decompose_fn = FT_Error (*)(FT_Outline*, const FT_Outline_Funcs*, void*);
using ft_outline_get_cbox_fn = void (*)(const FT_Outline*, FT_BBox*);
using ft_set_char_size_fn = FT_Error (*)(FT_Face, FT_F26Dot6, FT_F26Dot6, FT_UInt, FT_UInt);
using ft_set_pixel_sizes_fn = FT_Error (*)(FT_Face, FT_UInt, FT_UInt);
using ft_request_size_fn = FT_Error (*)(FT_Face, FT_Size_Request);
using ft_get_sfnt_table_fn = void* (*)(FT_Face, FT_Sfnt_Tag);
using ft_load_sfnt_table_fn = FT_Error (*)(FT_Face, FT_ULong, FT_Long, FT_Byte*, FT_ULong*);
using ft_mulfix_fn = FT_Long (*)(FT_Long, FT_Long);

// ---------------------------------------------------------------------------
// Which FreeType entry points are interposed, and why
//
//   FT_Render_Glyph          rasterization, for callers that let FreeType own
//   FT_Outline_Get_Bitmap    the bitmap, and for those that allocate and place
//                            it themselves (WebRender). LCD only.
//   FT_Load_Glyph            which glyph of which face an outline holds, which
//   FT_Load_Char             FT_Outline_Get_Bitmap is not told; clears the
//                            slot's pending translation before the load, since
//                            a slot outlives the glyph in it, and corrects
//                            linearHoriAdvance after it.
//   FT_New_Face              each face's font file - a path, or the bytes for
//   FT_New_Memory_Face       a memory face. FT_Face exposes neither.
//   FT_Open_Face
//   FT_Reference_Face        mirror FreeType's own refcount, so a face is
//   FT_Done_Face             forgotten only when it is really gone.
//   FT_Done_FreeType         sweep faces destroyed without FT_Done_Face.
//   FT_Set_Transform         detect a reshape, recover the exact matrix.
//   FT_Outline_Translate     recover the caller's subpixel pen phase.
//   FT_GlyphSlot_Embolden    detect synthetic bold and italic, which reshape
//   FT_GlyphSlot_Oblique     the outline in place, and turn them back into
//   FT_Outline_Embolden      DWRITE_FONT_SIMULATIONS.
//   FT_Outline_EmboldenXY
//   FT_Outline_Transform
//   FT_Set_Char_Size         rewrite size->metrics with Windows' rounding. All
//   FT_Set_Pixel_Sizes       three, because each recomputes those fields from
//   FT_Request_Size          scratch.
//   FT_Get_Sfnt_Table        a per-size OS/2 copy; the font is never modified,
//                            so the raw-table path shaping uses is unaffected.
//   FT_MulFix                only watched. Gecko computes a synthetic-bold
//                            strength with it, which is the one sign a load
//                            carries that the instance asking for the glyph is
//                            emboldened; see ApplyWindowsBoldAdvance.
//
// Variable fonts need no entry point of their own, since the axis position is
// read with FT_Get_Var_Design_Coordinates at render time.
//
// Nothing else is exported: cleartype.map keeps every other name out of the
// dynamic symbol table, because a preloaded library's symbols go to the front
// of the process's lookup scope.
// ---------------------------------------------------------------------------

// One resolution per name, kept only once it succeeds.
//
// A null is never stored. dlsym(RTLD_NEXT) can be asked before the host's
// FreeType is in the link map, and a caller handed a null real function has
// nothing to fall through to, so caching that answer would leave every later
// call failing for the rest of the process with nothing said about it. Storing
// only a non-null result makes the resolution monotone: a call after the
// library arrives picks the symbol up. Two threads racing store the same
// address, so the plain atomic needs no init guard.
#define DEFINE_REAL(type, name)                             \
    type real_##name()                                      \
    {                                                       \
        static std::atomic<type> resolved{nullptr};         \
        type fn = resolved.load(std::memory_order_acquire); \
        if (fn != nullptr) {                                \
            return fn;                                      \
        }                                                   \
        fn = reinterpret_cast<type>(                        \
            dlsym(RTLD_NEXT, #name));                       \
        if (fn == nullptr) {                                \
            fn = reinterpret_cast<type>(                    \
                FreeTypeSymbol(#name));                     \
        }                                                   \
        if (fn != nullptr) {                                \
            resolved.store(fn, std::memory_order_release);  \
        }                                                   \
        return fn;                                          \
    }

DEFINE_REAL(ft_render_glyph_fn, FT_Render_Glyph)
DEFINE_REAL(ft_new_face_fn, FT_New_Face)
DEFINE_REAL(ft_new_memory_face_fn, FT_New_Memory_Face)
DEFINE_REAL(ft_open_face_fn, FT_Open_Face)
DEFINE_REAL(ft_reference_face_fn, FT_Reference_Face)
DEFINE_REAL(ft_done_face_fn, FT_Done_Face)
DEFINE_REAL(ft_done_freetype_fn, FT_Done_FreeType)
DEFINE_REAL(ft_set_transform_fn, FT_Set_Transform)
DEFINE_REAL(ft_outline_translate_fn, FT_Outline_Translate)
DEFINE_REAL(ft_outline_done_fn, FT_Outline_Done)
DEFINE_REAL(ft_done_glyph_fn, FT_Done_Glyph)
DEFINE_REAL(ft_glyphslot_embolden_fn, FT_GlyphSlot_Embolden)
DEFINE_REAL(ft_glyphslot_oblique_fn, FT_GlyphSlot_Oblique)
DEFINE_REAL(ft_outline_embolden_fn, FT_Outline_Embolden)
DEFINE_REAL(ft_outline_embolden_xy_fn, FT_Outline_EmboldenXY)
DEFINE_REAL(ft_outline_transform_fn, FT_Outline_Transform)
DEFINE_REAL(ft_load_glyph_fn, FT_Load_Glyph)
DEFINE_REAL(ft_load_char_fn, FT_Load_Char)
DEFINE_REAL(ft_outline_get_bitmap_fn, FT_Outline_Get_Bitmap)
DEFINE_REAL(ft_outline_decompose_fn, FT_Outline_Decompose)
DEFINE_REAL(ft_outline_get_cbox_fn, FT_Outline_Get_CBox)
DEFINE_REAL(ft_set_char_size_fn, FT_Set_Char_Size)
DEFINE_REAL(ft_set_pixel_sizes_fn, FT_Set_Pixel_Sizes)
DEFINE_REAL(ft_request_size_fn, FT_Request_Size)
DEFINE_REAL(ft_get_sfnt_table_fn, FT_Get_Sfnt_Table)
DEFINE_REAL(ft_mulfix_fn, FT_MulFix)
// Not interposed - only read, to answer the embedded-bitmap question below.
#if CLEARTYPE_FIREFOX_PARITY
DEFINE_REAL(ft_load_sfnt_table_fn, FT_Load_Sfnt_Table)
#endif

#undef DEFINE_REAL

// FreeType's rounded 16.16 multiply, for the shim's own arithmetic. Always
// through the real function: this file interposes FT_MulFix to watch for the
// one Gecko computes a synthetic-bold strength with, and a call from in here
// would be mistaken for it.
FT_Long MulFix(const FT_Long a, const FT_Long b)
{
    ft_mulfix_fn real = real_FT_MulFix();
    if (real != nullptr) {
        return real(a, b);
    }
    // What src/base/ftcalc.c does: the sign is taken out first, so the
    // rounding goes away from zero on both sides.
    const uint64_t magnitude =
        static_cast<uint64_t>(a < 0 ? -a : a) * static_cast<uint64_t>(b < 0 ? -b : b);
    const FT_Long product = static_cast<FT_Long>((magnitude + 0x8000U) >> 16);
    return (a < 0) != (b < 0) ? -product : product;
}

// ---------------------------------------------------------------------------
// Firefox parity.
//
// Everything under CLEARTYPE_FIREFOX_PARITY (opt-out: build with
// -DCLEARTYPE_FIREFOX_PARITY=0 for a plain FreeType-to-DirectWrite
// interposer) reproduces what Firefox 154.0 does on Windows, translated from
// the Firefox source at mozilla-release 9ce1ee6baeb9a3c326dbd180bdece65d8fc2eadc
// (tag FIREFOX_154_0_RELEASE). Each site names the Firefox file and function
// it was translated from; paths are relative to the Firefox tree:
//
//   gfx/thebes/gfxDWriteFonts.cpp, gfxDWriteFontList.cpp       Windows side
//   gfx/thebes/gfxFT2FontBase.cpp, gfxFcPlatformFontList.cpp   Linux side
//   gfx/thebes/gfxFont.cpp, gfxHarfBuzzShaper.cpp               shared
//   gfx/2d/ScaledFontDWrite.cpp, ScaledFontFontconfig.cpp, DWriteSettings.cpp
//   gfx/wr/wr_glyph_rasterizer/src/platform/{windows,unix}/font.rs
//   gfx/wr/wr_glyph_rasterizer/src/{rasterizer,gamma_lut}.rs
//   gfx/wr/webrender_api/src/font.rs
//   gfx/src/nsFontMetrics.cpp, layout/painting/nsCSSRendering.cpp
// ---------------------------------------------------------------------------

#ifndef CLEARTYPE_FIREFOX_PARITY
#  define CLEARTYPE_FIREFOX_PARITY 1
#endif

// ---------------------------------------------------------------------------
// Logging.
//
// Off unless CLEARTYPE_LOG names a file, which it writes to instead of
// stderr because GUI applications routinely reopen their inherited stdio on
// /dev/null when they detach from the launching terminal. A shell redirection
// then captures nothing from a process that is very much alive and rendering.
// ---------------------------------------------------------------------------

// A descriptor, not a FILE*. stdio locks per call, so a thread formatting a
// message with several fprintf calls can be interrupted between them and the
// lines tear. Each message is formatted into a stack buffer and handed to one
// write(2) instead, which O_APPEND makes atomic against the others.
int g_log_fd = -1;

// Kept in .rodata whether or not logging is on, so strings(1) answers offline.
[[gnu::used]] constexpr char g_version[] = CLEARTYPE_VERSION_STRING;

// Kept for the "is logging on?" checks that read better as a pointer test.
bool LogEnabled() { return g_log_fd >= 0; }

// A variadic template would satisfy DCL50-CPP but cost more than it
// buys here: the format attribute below gives the compiler real
// checking of the format string against its arguments at every call
// site, and a parameter pack forwarding to vsnprintf cannot carry it.
// NOLINTNEXTLINE(cert-dcl50-cpp)
void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// NOLINTNEXTLINE(cert-dcl50-cpp)  -- see the declaration above
void LogLine(const char* fmt, ...)
{
    if (g_log_fd < 0) {
        return;
    }
    // Comfortably above the longest message this file emits; a longer one is
    // truncated rather than split, because a split is the thing being fixed.
    char line[1024];
    const int used = std::snprintf(line, sizeof(line), "[cleartype %d] ", static_cast<int>(getpid()));
    if (used < 0 || static_cast<size_t>(used) >= sizeof(line)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    const int body = std::vsnprintf(line + used, sizeof(line) - static_cast<size_t>(used) - 1, fmt, args);
    va_end(args);
    if (body < 0) {
        return;
    }
    size_t length = static_cast<size_t>(used) + static_cast<size_t>(body);
    if (length > sizeof(line) - 2) {
        length = sizeof(line) - 2;   // room for the newline
    }
    line[length++] = '\n';

    // Short writes are possible in principle; a partial line is still better
    // than a lost one, and EINTR is worth retrying.
    size_t written = 0;
    while (written < length) {
        const ssize_t n = write(g_log_fd, line + written, length - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        written += static_cast<size_t>(n);
    }
}

// The faces a trace is being kept for.
//
// Summing a glyph's texture and formatting a line for it is real work in the
// drawing thread, and paid on every glyph it comes to more than the writing
// does. A run that behaves differently while it is watched says nothing about
// the run that is not, so CLEARTYPE_LOG_FAMILY narrows the per-glyph lines to
// the faces in question and leaves the rest of a page costing what it costs
// with no log at all.
char g_log_family[128] = "";

bool LogFaceWanted(FT_Face face)
{
    if (g_log_family[0] == '\0') {
        return true;
    }
    return face != nullptr && face->family_name != nullptr &&
           std::strstr(face->family_name, g_log_family) != nullptr;
}

// Reports, at most once per face, why that face's glyphs are going to real
// FreeType instead.
void LogSkipOnce(FT_Face face, const char* reason)
{
    if (!LogEnabled()) {
        return;
    }
    // Bounded, because the key is an FT_Face address that a later face can be
    // allocated at, so the set has no natural end and a stale entry would
    // silence a warning about a different face.
    constexpr size_t kMaxReported = 256;
    static std::vector<FT_Face> reported;
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&mutex);
    bool known = false;
    for (FT_Face seen : reported) {
        if (seen == face) {
            known = true;
            break;
        }
    }
    if (!known) {
        if (reported.size() >= kMaxReported) {
            reported.erase(reported.begin());
        }
        reported.push_back(face);
    }
    pthread_mutex_unlock(&mutex);

    if (!known) {
        LogLine("face %p \"%s\": falling through to FreeType - %s", reinterpret_cast<void*>(face),
                face != nullptr && face->family_name != nullptr ? face->family_name : "?",
                reason);
    }
}

// ---------------------------------------------------------------------------
// Options, resolved once.
//
// Not read per glyph: a single window repaint is thousands of calls through
// here, and getenv() walks the whole environment block every time.
// ---------------------------------------------------------------------------

struct Options
{
    // Whether the interposer runs at all is not here. It can go from false to
    // true when the host opens a library, and these are settled once by
    // pthread_once, so it is asked per call through InterposerWanted instead -
    // the same reason WindowsMetrics below is a call and not a field.
    //
    // Unset means "ask DirectWrite", which is what Windows does.
    bool rendering_mode_forced = false;
    DWRITE_RENDERING_MODE rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL;
    DWRITE_MEASURING_MODE measuring_mode = DWRITE_MEASURING_MODE_NATURAL;
    bool grid_fit_forced = false;
    DWRITE_GRID_FIT_MODE grid_fit_mode = DWRITE_GRID_FIT_MODE_DEFAULT;
    bool subpixel_positioning = true;
    bool alpha_gamma_enabled = false;
    bool windows_metrics = false;

    BYTE alpha_lut[256] = {};
};

Options g_options;
pthread_once_t g_options_once = PTHREAD_ONCE_INIT;

bool EnvIsOff(const char* name, const bool fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return !(std::strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0 ||
             strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0);
}

void InitOptions()
{
    if (const char* path = std::getenv("CLEARTYPE_LOG")) {
        // One file per process, as probe.c and cairo_probe.c already do. The
        // file is opened for append, so processes sharing one path interleave
        // their lines, and no count or ordering in the result can be
        // attributed to a process.
        if (std::strcmp(path, "-") == 0) {
            // stderr, for processes that cannot create files (a sandboxed
            // content process); the pid in every line keeps them apart.
            g_log_fd = dup(2);
        } else {
            char named[4096];
            (void)std::snprintf(named, sizeof(named), "%s.%d", path, static_cast<int>(getpid()));
            // O_APPEND is what makes concurrent writes atomic; O_CLOEXEC keeps
            // the descriptor out of anything this process spawns.
            g_log_fd = open(named, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        }
        if (const char* family = std::getenv("CLEARTYPE_LOG_FAMILY")) {
            (void)std::snprintf(g_log_family, sizeof(g_log_family), "%s", family);
        }
        LogLine("%s", g_version);
    }

    g_options.subpixel_positioning = EnvIsOff("CLEARTYPE_SUBPIXEL_POSITIONING", true);

    // "auto" - the default - leaves the mode to DirectWrite, which decides
    // per font and per size from the font's gasp table and the rendering
    // params, as Windows does. Anything else pins the mode for every font and
    // size, which is useful for comparing modes and is not what Windows shows.
    if (const char* mode = std::getenv("CLEARTYPE_RENDERING_MODE")) {
        g_options.rendering_mode_forced = true;
        if (strcasecmp(mode, "auto") == 0) {
            g_options.rendering_mode_forced = false;
        } else if (strcasecmp(mode, "aliased") == 0) {
            g_options.rendering_mode = DWRITE_RENDERING_MODE_ALIASED;
        } else if (strcasecmp(mode, "gdi-classic") == 0) {
            g_options.rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_GDI_CLASSIC;
            g_options.measuring_mode = DWRITE_MEASURING_MODE_GDI_CLASSIC;
        } else if (strcasecmp(mode, "gdi-natural") == 0) {
            g_options.rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_GDI_NATURAL;
            g_options.measuring_mode = DWRITE_MEASURING_MODE_GDI_NATURAL;
        } else if (strcasecmp(mode, "natural") == 0) {
            g_options.rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL;
        } else if (strcasecmp(mode, "natural-symmetric") == 0) {
            g_options.rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;
        } else {
            g_options.rendering_mode_forced = false;
            LogLine("unknown CLEARTYPE_RENDERING_MODE=\"%s\", asking DirectWrite instead", mode);
        }
    }

    // NATURAL is the default, and it is what Firefox uses for all antialiased
    // text. dwrite_measure_mode() in platform/windows/font.rs answers
    // GDI_CLASSIC for FontRenderMode::Mono and NATURAL for Alpha and Subpixel.
    //
    // It answers GDI_CLASSIC for FontInstanceFlags::FORCE_GDI too, which also
    // drops SUBPIXEL_POSITION. gfxDWriteFont sets that flag for a list of
    // families below gfx.font_rendering.cleartype_params
    // .force_gdi_classic_max_size, from family names on the Windows side, so
    // nothing on Linux can key off it. A page using one of those families at a
    // small size is a known divergence, and asking for the same treatment here
    // means setting it by hand, below.
    //
    // Set after the rendering mode so an explicit request wins over the
    // measuring mode the GDI rendering modes imply above.
    if (const char* mode = std::getenv("CLEARTYPE_MEASURING_MODE")) {
        if (strcasecmp(mode, "natural") == 0) {
            g_options.measuring_mode = DWRITE_MEASURING_MODE_NATURAL;
        } else if (strcasecmp(mode, "gdi-classic") == 0) {
            g_options.measuring_mode = DWRITE_MEASURING_MODE_GDI_CLASSIC;
        } else if (strcasecmp(mode, "gdi-natural") == 0) {
            g_options.measuring_mode = DWRITE_MEASURING_MODE_GDI_NATURAL;
        }
    }

    // Likewise resolved by DirectWrite unless pinned:
    // GetRecommendedRenderingMode's full form returns the grid-fit mode
    // alongside the rendering mode, and the two belong together.
    if (const char* mode = std::getenv("CLEARTYPE_GRID_FIT")) {
        g_options.grid_fit_forced = true;
        if (strcasecmp(mode, "enabled") == 0) {
            g_options.grid_fit_mode = DWRITE_GRID_FIT_MODE_ENABLED;
        } else if (strcasecmp(mode, "disabled") == 0) {
            g_options.grid_fit_mode = DWRITE_GRID_FIT_MODE_DISABLED;
        } else if (strcasecmp(mode, "default") == 0) {
            g_options.grid_fit_mode = DWRITE_GRID_FIT_MODE_DEFAULT;
        } else {
            g_options.grid_fit_forced = false;
        }
    }

    // Line box parity. Windows rounds DirectWrite's vertical metrics, and
    // those are not the metrics FreeType reports. See FT_Set_Char_Size below.
    //
    // Set it to 0 for rasterization parity alone. The rewrite touches
    // size->metrics and the OS/2 line gap on a shared FT_Face, so every
    // consumer of that face in the process sees numbers made right for one
    // code path.
#if CLEARTYPE_FIREFOX_PARITY
    g_options.windows_metrics = EnvIsOff("CLEARTYPE_WINDOWS_METRICS", true);
#endif

    // An approximation, off by default, and it should stay off for Firefox.
    // DWrite's alpha texture is raw filtered coverage, and the gamma and
    // contrast that belong with it are applied by whoever blends it against
    // the text color, which FT_Render_Glyph never sees. Firefox already does
    // that step itself, through wr_glyph_rasterizer's GammaLut in
    // preblend(pixels, font.color), with the Windows numbers that two of the
    // prefs in prefs.cpp set; applying the curve here too would apply it
    // twice. This knob is for a caller that has no such step of its own.
    if (const char* gamma = std::getenv("CLEARTYPE_ALPHA_GAMMA")) {
        // strtod, since atof reads a misspelled value as zero and would leave
        // the knob looking set while doing nothing. An unparsable value falls
        // to the range check below, which rejects 0.
        char* gamma_end = nullptr;
        float parsed = static_cast<float>(std::strtod(gamma, &gamma_end));
        if (gamma_end == gamma || *gamma_end != '\0') {
            parsed = 1.0f;  // not a number: behave as though it were unset
        }
        if (parsed >= 0.25f && parsed <= 4.0f && parsed != 1.0f) {
            for (int i = 0; i < 256; ++i) {
                const float v = std::pow(static_cast<float>(i) / 255.0f, 1.0f / parsed) * 255.0f + 0.5f;
                g_options.alpha_lut[i] = static_cast<BYTE>(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v);
            }
            g_options.alpha_gamma_enabled = true;
        } else if (parsed != 1.0f) {
            LogLine("ignoring out-of-range CLEARTYPE_ALPHA_GAMMA=\"%s\"", gamma);
        }
    }
}

const Options& GetOptions()
{
    pthread_once(&g_options_once, InitOptions);
    return g_options;
}

// The metrics work reproduces what Firefox on Windows measures, so it applies
// to Gecko and to nothing else. Asked per call, since the options are settled
// on their first read, which can be long before libxul is mapped.
//
// Not itself gated, because FT_Get_Sfnt_Table is compiled either way and asks
// this. With the parity build off, ParityActive is a constant false and the
// whole thing folds away.
bool WindowsMetrics()
{
    return dwcft::ParityActive() && GetOptions().windows_metrics;
}

}  // namespace

// The face whose sfnt tables were last handed out on this thread.
static thread_local FT_Face g_last_sfnt_face = nullptr;


#if CLEARTYPE_FIREFOX_PARITY
// The size gfxFT2FontBase::InitMetrics is measuring at, read out of the font
// itself by libxul_patch.cpp. The face alone does not name an instance, since
// several fonts share one face at different sizes and the face carries
// whichever size was installed last.
static thread_local FT_Fixed g_claimed_em_26_6 = 0;
// The same size before it was quantized, or 0. See the claimed-size table.
static thread_local double g_claimed_em_px = 0.0;
#endif





// This thread's name, recorded on the way in. See ThisThreadName's comment for
// why it is caught here rather than read back with prctl(PR_GET_NAME).
namespace {
thread_local char g_thread_name[16 + 1];
thread_local bool g_thread_named = false;
}  // namespace

static const char* ThisThreadName()
{
    return g_thread_named ? g_thread_name : nullptr;
}

// The same log libxul_patch.cpp writes to, so a patch report and a
// rasterization report land in one file in the order they happened.
// Preformatted by the caller, since a variadic function cannot be forwarded
// without repeating its format attribute.
// Interposed only to be told the name; the real call still happens and its
// result is handed back untouched. Only a thread naming itself is recorded,
// since pthread_setname_np can name another thread and that answer belongs to
// the thread it names.
extern "C" __attribute__((visibility("default")))
int pthread_setname_np(const pthread_t thread, const char* name)
{
    static int (*real)(pthread_t, const char*) = nullptr;
    if (real == nullptr) {
        real = reinterpret_cast<int (*)(pthread_t, const char*)>(
            dlsym(RTLD_NEXT, "pthread_setname_np"));
        if (real == nullptr) {
            return ENOSYS;
        }
    }
    // glibc declares this entry point __nonnull((2)) and this definition
    // matches, so the compiler assumes the parameter is never null and deletes
    // a plain `name != nullptr` test, sending the caller straight into
    // strncpy. Routing the pointer through a volatile object takes the
    // assumption away: the value read back is not the parameter as far as the
    // optimizer is concerned.
    const char* volatile name_slot = name;
    const char* const observed = name_slot;
    const int result = real(thread, name);
    if (result == 0 && observed != nullptr && pthread_equal(thread, pthread_self())) {
        std::strncpy(g_thread_name, observed, sizeof(g_thread_name) - 1);
        g_thread_name[sizeof(g_thread_name) - 1] = '\0';
        g_thread_named = true;
    }
    return result;
}

extern "C" void CleartypeLogLine(const char* message)
{
    GetOptions();                        // opens the log, if it is wanted
    if (LogEnabled() && message != nullptr) {
        LogLine("%s", message);
    }
}

namespace {

// ---------------------------------------------------------------------------
// The DWrite factory, created once on first use.
// ---------------------------------------------------------------------------

// Returned together, and only ever read under g_factory_mutex. A caller
// reading the globals itself could observe a published factory alongside a
// half-written factory2, which is what two threads reaching their first LCD
// glyph at once would produce.
struct Factories
{
    IDWriteFactory* factory = nullptr;
    IDWriteFactory2* factory2 = nullptr;  // optional; enables grid-fit control
};

Factories g_factories;
bool g_factory_failed = false;
pthread_mutex_t g_factory_mutex = PTHREAD_MUTEX_INITIALIZER;

Factories GetFactories()
{
    pthread_mutex_lock(&g_factory_mutex);
    if (g_factories.factory == nullptr && !g_factory_failed) {
        IUnknown* unknown = nullptr;
        const HRESULT hr = DWriteCoreCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, DWRITE_UUIDOF(IDWriteFactory), &unknown);
        if (SUCCEEDED(hr) && unknown != nullptr) {
            Factories created;
            created.factory = reinterpret_cast<IDWriteFactory*>(unknown);

            void* factory2 = nullptr;
            const GUID iid2 = DWRITE_UUIDOF(IDWriteFactory2);
            if (SUCCEEDED(unknown->QueryInterface(iid2, &factory2))) {
                created.factory2 = static_cast<IDWriteFactory2*>(factory2);
            }
            g_factories = created;
            LogLine("DWriteCore factory created (IDWriteFactory2 %s)",
                    created.factory2 ? "available" : "unavailable");
        } else {
            g_factory_failed = true;
            const char* why = DWriteCoreShimGetLastLoadError();
            LogLine("DWriteCoreCreateFactory failed hr=0x%08X%s%s",
                    static_cast<unsigned>(hr), why ? ": " : "", why ? why : "");
        }
    }
    const Factories snapshot = g_factories;
    pthread_mutex_unlock(&g_factory_mutex);
    return snapshot;
}

#if CLEARTYPE_FIREFOX_PARITY
// Created when this library is loaded, not on first use: gfxFT2FontBase::
// InitMetrics runs in Firefox's content processes, whose sandbox
// (security/sandbox/linux) starts after process startup and from then on
// denies the dlopen of the DWriteCore implementation that the first
// DWriteCoreCreateFactory would perform.
__attribute__((constructor)) void CreateFactoryAtLoad()
{
    // Gecko only. Doing this everywhere would initialize DWriteCore in every
    // process that loads this library, which is most of the cost a plain build
    // avoids; ParityActive answers from the installation layout here, since
    // libxul is not mapped yet in a content process this early.
    if (dwcft::ParityActive() && InterposerWanted()) {
        GetFactories();
    }
}
#endif

// The rendering params GetRecommendedRenderingMode consults. On Windows
// these come from the per-monitor ClearType tuner settings in the registry;
// DWriteCore's defaults are DirectWrite's documented ones (gamma 1.8,
// enhanced contrast 0.5, ClearType level 1.0, RGB), and there is nothing on
// this platform to read instead.
//
// Created under g_factory_mutex, then only read. It is never released: it
// lives as long as the factory does, and the process exits with both.
//
// Lock order, since these are plain mutexes and nothing enforces it. The face
// lock is taken first and the factory lock nested inside it, which is what
// this function does when it is called from under g_faces_mutex. Taking them
// the other way around anywhere would deadlock.
IDWriteRenderingParams* g_rendering_params = nullptr;

IDWriteRenderingParams* GetDefaultRenderingParams()
{
    pthread_mutex_lock(&g_factory_mutex);
    if (g_rendering_params == nullptr && g_factories.factory != nullptr) {
        IDWriteRenderingParams* params = nullptr;
        if (SUCCEEDED(g_factories.factory->CreateRenderingParams(&params))) {
            g_rendering_params = params;
            LogLine("rendering params: gamma=%.2f contrast=%.2f clearTypeLevel=%.2f geometry=%d",
                    static_cast<double>(params->GetGamma()),
                    static_cast<double>(params->GetEnhancedContrast()),
                    static_cast<double>(params->GetClearTypeLevel()),
                    static_cast<int>(params->GetPixelGeometry()));
        }
    }
    IDWriteRenderingParams* params = g_rendering_params;
    pthread_mutex_unlock(&g_factory_mutex);
    return params;
}

const char* RenderingModeName(const DWRITE_RENDERING_MODE mode)
{
    switch (mode) {
    case DWRITE_RENDERING_MODE_ALIASED:                    return "ALIASED";
    case DWRITE_RENDERING_MODE_CLEARTYPE_GDI_CLASSIC:      return "CLEARTYPE_GDI_CLASSIC";
    case DWRITE_RENDERING_MODE_CLEARTYPE_GDI_NATURAL:      return "CLEARTYPE_GDI_NATURAL";
    case DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL:          return "CLEARTYPE_NATURAL";
    case DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC:return "CLEARTYPE_NATURAL_SYMMETRIC";
    case DWRITE_RENDERING_MODE_OUTLINE:                    return "OUTLINE";
    default:                                               return "DEFAULT";
    }
}

// ---------------------------------------------------------------------------
// Face table.
//
// One entry per live FT_Face, holding what FT_Face itself does not expose
// and what DWrite needs: the font file (path, or the bytes for a memory
// face), the face index, the transform last handed to FT_Set_Transform, and
// a lazily created IDWriteFontFace cached for the face's lifetime.
//
// A plain mutex: DWriteCore never calls into FreeType, so nothing can re-enter
// this file's own interposers while the lock is held.
//
// An FT_Face outlives its first FT_Done_Face: FreeType reference-counts them,
// and FT_Done_Face only destroys the face when the count reaches zero. Qt6
// relies on this (libQt6Gui imports FT_Reference_Face), so the count is
// mirrored here, with FT_Reference_Face interposed alongside the constructors.
// ---------------------------------------------------------------------------

// An IDWriteFontFace bakes in both its simulations and, for a variable font,
// its axis position - so one FT_Face can need several of them: regular and
// synthetically bold, weight 400 and weight 600. They are cached by the pair.
//
// `face == nullptr` records a creation that failed, so it is not retried for
// every glyph.
struct CachedDWriteFace
{
    IDWriteFontFace* face = nullptr;
    DWRITE_FONT_SIMULATIONS simulations = DWRITE_FONT_SIMULATIONS_NONE;
    std::vector<DWRITE_FONT_AXIS_VALUE> axes;  // empty: not a variable face
};

bool SameAxes(const std::vector<DWRITE_FONT_AXIS_VALUE>& a,
              const std::vector<DWRITE_FONT_AXIS_VALUE>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].axisTag != b[i].axisTag || a[i].value != b[i].value) {
            return false;
        }
    }
    return true;
}

// What DirectWrite recommends for this face at one em size. Cached because
// GetRecommendedRenderingMode reads the font's gasp table, which is not work
// to repeat thousands of times per repaint, and because sizes change far more
// rarely than glyphs do.
struct CachedMode
{
    // The whole key, which holds the em size bit for bit. The recommendation
    // Windows asks for depends on the em size, the measuring mode and the
    // font's gasp table, and on nothing else. See ResolveModeLocked.
    // A gasp range ends on a whole ppem, so two sizes a thousandth apart can
    // fall on opposite sides of one, and a transformed run whose em is a hair
    // under a whole pixel shares no answer with a plain run at that pixel.
    uint32_t em_size_bits = 0;
    DWRITE_RENDERING_MODE rendering_mode = DWRITE_RENDERING_MODE_DEFAULT;
    DWRITE_GRID_FIT_MODE grid_fit_mode = DWRITE_GRID_FIT_MODE_DEFAULT;
    bool answered = false;  // false: GetRecommendedRenderingMode failed
};

#if CLEARTYPE_FIREFOX_PARITY
// One EBLC bitmapSizeTable / EBSC bitmapScaleTable entry, as
// gfxDWriteFont::HasBitmapStrikeForSize reads them.
struct EblcStrike
{
    unsigned ppem_x;
    unsigned ppem_y;
    uint16_t first;
    uint16_t last;
};

struct EbscStrike
{
    unsigned ppem_x;
    unsigned ppem_y;
};

// gfxFont::Metrics (gfx/thebes/gfxFont.h), as gfxDWriteFont::ComputeMetrics
// fills it, after gfxFont::SanitizeMetrics.
struct WinMetrics
{
    double xHeight;
    double capHeight;
    double strikeoutSize;
    double strikeoutOffset;
    double underlineSize;
    double underlineOffset;
    double internalLeading;
    double externalLeading;
    double emHeight;
    double emAscent;
    double emDescent;
    double maxHeight;
    double maxAscent;
    double maxDescent;
    double maxAdvance;
    double aveCharWidth;
    double spaceWidth;
    double zeroWidth;
    double ideographicWidth;
};

// One gfxDWriteFont: what gfxDWriteFont::ComputeMetrics leaves behind for one
// (font face, mAdjustedSize), with the IDWriteFontFace it measured with.
struct WinInstance
{
    FT_Fixed size_26_6;             // the requested size, half the cache key
    // The axis position the face below was built at, and the other half of
    // the key. A variable family draws several weights at one pixel size, and
    // each weight is a separate IDWriteFontFace. Keyed on the size alone, the
    // first weight drawn at a size answers for every later one, which measures
    // glyphs through a face for the wrong axis position.
    std::vector<DWRITE_FONT_AXIS_VALUE> axes;
    bool valid;
    // The size this instance was asked for, which is mAdjustedSize before the
    // bitmap-strike rounding moves it. size_26_6 holds the same value on a
    // 26.6 grid, too coarse to rebuild a conversion factor from.
    double requested_size;
    double adjusted_size;           // mAdjustedSize
    float funits_conv;              // mFUnitsConvFactor
    bool use_subpixel_positions;    // mUseSubpixelPositions
    bool bitmap_font;               // IsCJKFont() && HasBitmapStrikeForSize()
    bool bad_underline;             // gfxFontEntry::mIsBadUnderlineFont
    bool descent_fold;              // see ApplyWindowsMetrics
    // FreeType's 16.16 x_scale while this size was the one set on the face,
    // which is what gfxFT2FontBase::InitMetrics turns into mFUnitsConvFactor.
    // Recorded here because the face carries one size at a time and the next
    // font to ask for a different one overwrites it. Zero until seen.
    FT_Fixed ft_x_scale;
    // mFontFace, held as a counted reference. The face cache below is
    // bounded and releases its oldest entry, while this cache is
    // bounded separately and larger, so an instance can outlive the cache
    // entry it was built from. Without a reference of its own it would be
    // measuring glyphs through a freed face.
    IDWriteFontFace* dwrite_face;
    DWRITE_FONT_METRICS font_metrics;
    WinMetrics metrics;
};
#endif  // CLEARTYPE_FIREFOX_PARITY

struct FaceEntry
{
    FT_Face face = nullptr;
    FT_Library library = nullptr;   // for the FT_Done_FreeType sweep
    std::string path;               // empty for a memory face
    const FT_Byte* memory = nullptr;  // non-null for a memory face
    FT_Long memory_size = 0;
    FT_Long face_index = 0;
    int refcount = 1;
    // Properties of the file, answered once; -1 until asked.
    int is_cjk = -1;
    bool strikes_read = false;
#if CLEARTYPE_FIREFOX_PARITY
    std::vector<EblcStrike> eblc;
    std::vector<EbscStrike> ebsc;
    int has_colr = -1;
    int is_hinted = -1;
    std::vector<WinInstance> instances;
#endif
    // The em size the caller last asked for, in 26.6 pixels, or 0. Not the
    // same as the one FreeType then scales by: head.flags bit 3 ("force ppem
    // to integer values") makes tt_size_reset recompute x_scale and y_scale
    // from the rounded ppem, and DirectWrite honors no such thing.
    FT_Fixed requested_em_26_6 = 0;
    // Tracks only FT_Set_Transform's *matrix*. Its delta needs no field: see
    // the interposer for why. The matrix itself is kept, not just whether it
    // was identity, because it is carried into the DWrite call as a
    // DWRITE_MATRIX rather than being a reason to decline the face.
    bool transform_identity = true;
    // Kept beside the other flags rather than next to the pointer each
    // one describes: interleaved with them, every bool cost a whole word.
    bool file_failed = false;
    bool resource_failed = false;
    FT_Matrix transform_matrix = { .xx = 0x10000, .xy = 0, .yx = 0, .yy = 0x10000 };
    // FT_Set_Transform's delta, which FreeType applies inside FT_Load_Glyph.
    // Kept because the translate it makes there is no longer recorded.
    FT_Vector transform_delta = { .x = 0, .y = 0 };
    DWRITE_FONT_FACE_TYPE dwrite_face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    // Created once and reused by both face paths.
    IDWriteFontFile* dwrite_file = nullptr;
    // Only for variable faces: the object that can produce a face at an
    // arbitrary axis position.
    IDWriteFontResource* dwrite_resource = nullptr;
    std::vector<CachedDWriteFace> dwrite_faces;
    std::vector<CachedMode> modes;
};

std::vector<FaceEntry> g_faces;
pthread_mutex_t g_faces_mutex = PTHREAD_MUTEX_INITIALIZER;

FaceEntry* FindFaceLocked(FT_Face face)
{
    for (FaceEntry& entry : g_faces) {
        if (entry.face == face) {
            return &entry;
        }
    }
    return nullptr;
}

// Drops the entry at `index`, releasing its cached DWrite face. Called with
// g_faces_mutex held.
void ForgetSfntCopiesLocked(FT_Face face);
size_t SfntCopyCountLocked();
// Defined with the memory font cache, whose entries it counts down.
void ForgetMemoryFontLocked(const IDWriteFontFile* file);
// Defined with the side tables, all of which it reads.
void LogTableCensus(const char* when);
void MaybeLogTableCensus();

// Defined with the outline-owner table further down. Takes g_owners_mutex, so
// it fixes the lock order as g_faces_mutex then g_owners_mutex; nothing takes
// them the other way around.
void ForgetOutlineOwners(FT_Face face);
// Defined with the installed-bitmap table further down. Takes
// g_tracked_mutex under g_faces_mutex; nothing takes them the other way.
void FreeTrackedBuffersForFace(FT_Face face);
// Defined with the pending-outline table. Takes g_shifts_mutex under
// g_faces_mutex; nothing takes them the other way.
void ForgetPendingOutlines(FT_Face face);
#if CLEARTYPE_FIREFOX_PARITY
// Defined with the claimed-size table. Takes g_claimed_mutex under
// g_faces_mutex; nothing takes them the other way.
void ForgetClaimedSizes(FT_Face face);
#endif

void EraseFaceLocked(const size_t index)
{
    // The owner table is keyed on &face->glyph->outline, an address inside the
    // glyph slot, which is freed along with the face. An entry left behind
    // keeps naming a dead face. The next allocation to land on that address
    // matches it and resurrects the dead pointer, and that allocation can be
    // another face's slot or a heap FT_Outline from FT_Glyph_Copy.
    ForgetOutlineOwners(g_faces[index].face);
    FreeTrackedBuffersForFace(g_faces[index].face);
    ForgetPendingOutlines(g_faces[index].face);
    ForgetSfntCopiesLocked(g_faces[index].face);
#if CLEARTYPE_FIREFOX_PARITY
    ForgetClaimedSizes(g_faces[index].face);
#endif
    std::vector<IDWriteFontFace*> faces;
    for (const CachedDWriteFace& cached : g_faces[index].dwrite_faces) {
        if (cached.face != nullptr) {
            faces.push_back(cached.face);
        }
    }
#if CLEARTYPE_FIREFOX_PARITY
    for (const WinInstance& inst : g_faces[index].instances) {
        if (inst.dwrite_face != nullptr) {
            faces.push_back(inst.dwrite_face);
        }
    }
#endif
    IDWriteFontFile* file = g_faces[index].dwrite_file;
    IDWriteFontResource* resource = g_faces[index].dwrite_resource;

    g_faces[index] = g_faces.back();
    g_faces.pop_back();

    for (IDWriteFontFace* face : faces) {
        face->Release();
    }
    if (resource != nullptr) {
        resource->Release();
    }
    if (file != nullptr) {
        ForgetMemoryFontLocked(file);
        file->Release();
    }
}

void RecordFace(FT_Library library, FT_Face face, const char* path, const FT_Byte* memory,
                const FT_Long memory_size, const FT_Long face_index)
{
    if (!InterposerWanted()) {
        return;
    }
    GetOptions();  // opens the log, if it is wanted
    LogLine("recorded face %p: %s index=%ld", reinterpret_cast<void*>(face),
            path != nullptr ? path : memory != nullptr ? "(memory)" : "(custom stream)",
            static_cast<long>(face_index));

    pthread_mutex_lock(&g_faces_mutex);
    // A face address can legitimately be reused once the previous face at it
    // is gone, and an application is free to open the same file twice; either
    // way the newest record is the correct one.
    for (size_t i = 0; i < g_faces.size(); ++i) {
        if (g_faces[i].face == face) {
            EraseFaceLocked(i);
            break;
        }
    }

    FaceEntry entry;
    entry.face = face;
    entry.library = library;
    entry.path = path != nullptr ? path : "";
    entry.memory = memory;
    entry.memory_size = memory_size;
    entry.face_index = face_index;
    g_faces.push_back(entry);
    pthread_mutex_unlock(&g_faces_mutex);
}

void ReferenceFace(FT_Face face)
{
    pthread_mutex_lock(&g_faces_mutex);
    if (FaceEntry* entry = FindFaceLocked(face)) {
        entry->refcount++;
    }
    pthread_mutex_unlock(&g_faces_mutex);
}

// Mirrors FT_Done_Face: one reference goes away, and the face is only
// forgotten when the last one does.
void ReleaseFace(FT_Face face)
{
    pthread_mutex_lock(&g_faces_mutex);
    for (size_t i = 0; i < g_faces.size(); ++i) {
        if (g_faces[i].face == face) {
            if (--g_faces[i].refcount <= 0) {
                LogLine("forgetting face %p", reinterpret_cast<void*>(face));
                EraseFaceLocked(i);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_faces_mutex);
}

// FT_Done_FreeType destroys a library's remaining faces without routing any
// of them through FT_Done_Face, so without this sweep their entries - and
// the DWrite faces they cache - would outlive them.
void ForgetLibrary(FT_Library library)
{
    pthread_mutex_lock(&g_faces_mutex);
    for (size_t i = g_faces.size(); i-- > 0;) {
        if (g_faces[i].library == library) {
            EraseFaceLocked(i);
        }
    }
    pthread_mutex_unlock(&g_faces_mutex);
}


std::u16string ToUtf16(const std::string& utf8)
{
    // Font paths on Linux are bytes, and DWriteCore wants UTF-16. Decoding
    // by hand keeps this free of any locale or iconv dependency; a path
    // that is not valid UTF-8 yields no font file and the glyph falls
    // through, which is the right outcome for a path DWrite could not have
    // opened anyway.
    std::u16string out;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        unsigned int cp;
        size_t extra;
        unsigned int lowest;
        if (c < 0x80) {
            cp = c;
            extra = 0;
            lowest = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
            lowest = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
            lowest = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
            lowest = 0x10000;
        } else {
            return std::u16string();
        }
        if (i + extra >= utf8.size()) {
            return std::u16string();
        }
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return std::u16string();
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // An overlong form, a surrogate or anything past U+10FFFF is not a
        // path either, and the last of those would put two low surrogates in
        // the result.
        if (cp < lowest || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return std::u16string();
        }
        i += extra + 1;

        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

// The loader that backs memory faces, created on the first one and kept.
//
// Every reference costs a copy of the font until the loader itself is
// destroyed. The loader appends to a vector and keys each font file by that
// element's index, which CreateStreamFromKey bounds-checks and reads, so
// nothing can be removed without renumbering everything after it. All three
// implementations work that way: the Windows DWrite.dll, the redistributable
// DWriteCore, and this one. Tearing the loader down does return everything,
// but a live font file holds a reference on it, so that is only safe when no
// font file is live at all.
//
// Memory faces go through CustomFontFileLocked instead, which serves the
// caller's own bytes from a stream and can answer a key it no longer has,
// giving the memory back per font. This loader is the fallback for a build
// that will not take such a stream.
//
// Register and UnregisterFontFileLoader ignore their argument here and return
// S_OK. The Register call below is kept for a build that implements it.
IDWriteInMemoryFontFileLoader* g_memory_loader = nullptr;
bool g_memory_loader_failed = false;

// One DWrite font file per distinct block of font bytes, for as long as some
// face wants it.
//
// Handing the same font over twice costs a second copy, so faces alive at the
// same time share one. A host loading web fonts calls FT_New_Memory_Face over
// and over for the same bytes, which is where the copies pile up.
//
// The key is the content, not the address. A buffer the caller freed can come
// back for a different font at the same address.
//
// An entry goes when its last face does; see ForgetMemoryFontLocked. A font
// revisited after its faces went is copied again.
//
// Guarded by g_faces_mutex, like the FaceEntry fields it serves.
struct FontDigest
{
    uint64_t forward;
    uint64_t backward;
    bool operator==(const FontDigest& other) const
    {
        return forward == other.forward && backward == other.backward;
    }
};

struct MemoryFontFile
{
    FT_Long size;
    FontDigest hash;
    IDWriteFontFile* file;
    // The bytes behind the file, or 0 when the in-memory loader was used
    // instead and there is nothing of ours to free. See CustomFontFileLocked.
    uint64_t blob;
    // Faces holding `file`. The entry goes when the last one does.
    int uses;
};
// The vector itself is never destroyed, whatever it holds at exit. A static
// destructor running then would drop the last pointer to each entry's COM
// object without releasing it, which valgrind reports as a leak.
// Heap-allocated once and left, the way src/dwritecore_shim.cpp keeps its
// proxy cache.
std::vector<MemoryFontFile>& MemoryFontFiles()
{
    static auto* const files = new std::vector<MemoryFontFile>();
    return *files;
}

// FNV-1a in both directions, which is two independent 64-bit values over the
// same pass. Reading the whole font costs about what the copy this avoids
// would have, and anything less than the whole font is a key that two
// different fonts can share.
//
// The pair exists because a match here hands a caller a font file built from
// other bytes, and every glyph of its face then comes from the wrong font.
// Where the bytes were kept they are compared outright; the second value is
// what stands in for that on the path where DirectWrite copied them and there
// is nothing left to compare against.
FontDigest HashFontBytes(const FT_Byte* data, const FT_Long size)
{
    FontDigest digest{.forward = 1469598103934665603ULL, .backward = 14695981039346656037ULL};
    for (FT_Long i = 0; i < size; ++i) {
        digest.forward ^= data[i];
        digest.forward *= 1099511628211ULL;
        digest.backward ^= data[size - 1 - i];
        digest.backward *= 1099511628211ULL;
    }
    return digest;
}

// True when the blob still holding a cached font's bytes matches these.
bool SameFontBytes(uint64_t blob_id, const FT_Byte* data, FT_Long size);

IDWriteInMemoryFontFileLoader* GetMemoryLoader(IDWriteFactory* factory)
{
    if (g_memory_loader != nullptr || g_memory_loader_failed) {
        return g_memory_loader;
    }

    IDWriteFactory5* factory5 = nullptr;
    const GUID iid5 = DWRITE_UUIDOF(IDWriteFactory5);
    HRESULT hr = factory->QueryInterface(iid5, reinterpret_cast<void**>(&factory5));
    if (SUCCEEDED(hr) && factory5 != nullptr) {
        IDWriteInMemoryFontFileLoader* loader = nullptr;
        hr = factory5->CreateInMemoryFontFileLoader(&loader);
        if (SUCCEEDED(hr) && loader != nullptr) {
            hr = factory->RegisterFontFileLoader(loader);
            if (SUCCEEDED(hr)) {
                g_memory_loader = loader;
            } else {
                loader->Release();
            }
        }
        factory5->Release();
    }

    if (g_memory_loader == nullptr) {
        g_memory_loader_failed = true;
        LogLine("no in-memory font file loader (hr=0x%08X); memory faces will fall through",
                static_cast<unsigned>(hr));
    }
    return g_memory_loader;
}

// COM's own IID, which no DirectWrite header declares.
constexpr GUID kIID_IUnknown =
    { .Data1 = 0x00000000, .Data2 = 0x0000, .Data3 = 0x0000,
      .Data4 = { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

// One font's bytes, owned by the shim and counted: the table below holds a
// reference and every stream DWrite still has holds another. Dropping the
// table's reference therefore frees the bytes once DWrite is done with them
// and not before, which is what lets a font be forgotten at all.
//
// The bytes are copied because the buffer stays the caller's: FT_New_Memory_Face
// does not take ownership, and a host that freed it while DWrite still held a
// stream would be read after the free.
struct FontBlob
{
    std::vector<FT_Byte> bytes;
    long refs;
};

// The blob table has its own lock, and nothing else is taken while it is
// held. It has to: DWrite calls the loader back from inside Analyze, on this
// thread, with g_faces_mutex already held, and releases streams from its own
// threads afterwards.
pthread_mutex_t g_blobs_mutex = PTHREAD_MUTEX_INITIALIZER;

std::map<uint64_t, FontBlob*>& FontBlobs()
{
    static auto* const blobs = new std::map<uint64_t, FontBlob*>();
    return *blobs;
}

// Ids are never reused, so a key DWrite kept past the font's life finds
// nothing rather than finding the wrong font.
uint64_t g_next_blob_id = 1;

void ReleaseBlob(FontBlob* const blob)
{
    pthread_mutex_lock(&g_blobs_mutex);
    const bool last = --blob->refs == 0;
    pthread_mutex_unlock(&g_blobs_mutex);
    if (last) {
        // Unreachable at zero: the table's own reference is the one dropped
        // last, and it is dropped only after the id is erased.
        delete blob;
    }
}

// The table gives up its reference to `id`. Any stream DWrite still holds
// keeps the bytes alive on its own.
bool SameFontBytes(const uint64_t blob_id, const FT_Byte* data, const FT_Long size)
{
    bool same = false;
    pthread_mutex_lock(&g_blobs_mutex);
    const auto it = FontBlobs().find(blob_id);
    if (it != FontBlobs().end() && it->second != nullptr &&
        it->second->bytes.size() == static_cast<size_t>(size)) {
        same = std::memcmp(it->second->bytes.data(), data, static_cast<size_t>(size)) == 0;
    }
    pthread_mutex_unlock(&g_blobs_mutex);
    return same;
}

void ForgetBlob(const uint64_t id)
{
    pthread_mutex_lock(&g_blobs_mutex);
    const auto found = FontBlobs().find(id);
    FontBlob* const blob = found == FontBlobs().end() ? nullptr : found->second;
    if (blob != nullptr) {
        FontBlobs().erase(found);
    }
    pthread_mutex_unlock(&g_blobs_mutex);
    if (blob != nullptr) {
        ReleaseBlob(blob);
    }
}

// One font's bytes, as DWrite reads them. A fragment is a pointer into the
// blob, so releasing one costs nothing and no fragment can outlive the
// stream that handed it out.
class MemoryFontStream final : public IDWriteFontFileStream
{
public:
    // Takes over the reference its caller counted on the blob.
    explicit MemoryFontStream(FontBlob* const blob) : blob_(blob) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, kIID_IUnknown) ||
            IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFontFileStream))) {
            AddRef();
            *object = static_cast<IDWriteFontFileStream*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(__atomic_add_fetch(&refs_, 1, __ATOMIC_ACQ_REL));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const long remaining = __atomic_sub_fetch(&refs_, 1, __ATOMIC_ACQ_REL);
        if (remaining == 0) {
            // Exactly the type this is, since the destructor is private and
            // this is the only line that reaches it. A virtual destructor
            // would put a slot in the COM vtable and move every method after
            // it, as src/factory_proxy_overrides.cpp says at more length.
            delete this;
        }
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE ReadFileFragment(const void** start, const UINT64 offset,
                                               const UINT64 size, void** context) override
    {
        if (start == nullptr || context == nullptr) {
            return E_POINTER;
        }
        *start = nullptr;
        *context = nullptr;
        // The header asks implementations to check this rather than trust it.
        // Written as a subtraction so that a size near the top of the range
        // cannot wrap the sum.
        const uint64_t total = blob_->bytes.size();
        if (offset > total || size > total - offset) {
            return E_FAIL;
        }
        *start = blob_->bytes.data() + offset;
        return S_OK;
    }

    void STDMETHODCALLTYPE ReleaseFileFragment(void*) override {}

    HRESULT STDMETHODCALLTYPE GetFileSize(UINT64* const size) override
    {
        if (size == nullptr) {
            return E_POINTER;
        }
        *size = blob_->bytes.size();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64* const time) override
    {
        if (time == nullptr) {
            return E_POINTER;
        }
        // Nothing writes these bytes, and DWrite only compares the value
        // against itself.
        *time = 0;
        return S_OK;
    }

private:
    ~MemoryFontStream() { ReleaseBlob(blob_); }

    FontBlob* blob_;
    long refs_ = 1;
};

// Hands DWrite a stream for a key it was given. One per process and never
// released: CreateCustomFontFileReference is passed the loader directly, so
// nothing has to be able to look it up, and this build's
// RegisterFontFileLoader ignores what it is handed.
// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
// A COM object, whose lifetime is Release's and not delete's, and which this
// one never ends. A virtual destructor would add slots to a vtable DirectWrite
// reads and would still never run.
class MemoryFontLoader final : public IDWriteFontFileLoader
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, kIID_IUnknown) ||
            IsEqualGUID(riid, DWRITE_UUIDOF(IDWriteFontFileLoader))) {
            AddRef();
            *object = static_cast<IDWriteFontFileLoader*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(__atomic_add_fetch(&refs_, 1, __ATOMIC_ACQ_REL));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        // Never freed, whatever the count says.
        return static_cast<ULONG>(__atomic_sub_fetch(&refs_, 1, __ATOMIC_ACQ_REL));
    }

    HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void* const key, const UINT32 key_size,
                                                  IDWriteFontFileStream** const stream) override
    {
        if (key == nullptr || stream == nullptr) {
            return E_POINTER;
        }
        *stream = nullptr;
        if (key_size != sizeof(uint64_t)) {
            return E_INVALIDARG;
        }
        uint64_t id = 0;
        std::memcpy(&id, key, sizeof id);

        pthread_mutex_lock(&g_blobs_mutex);
        const auto found = FontBlobs().find(id);
        FontBlob* const blob = found == FontBlobs().end() ? nullptr : found->second;
        if (blob != nullptr) {
            ++blob->refs;
        }
        pthread_mutex_unlock(&g_blobs_mutex);

        if (blob == nullptr) {
            // The font was forgotten while DWrite still had the key. Ids are
            // never reused, so this can only ever be a font that is gone -
            // never a different one - and failing here costs that one face.
            return E_INVALIDARG;
        }
        *stream = new MemoryFontStream(blob);
        return S_OK;
    }

private:
    long refs_ = 1;
};

MemoryFontLoader* MemoryFontLoaderInstance(IDWriteFactory* factory)
{
    static MemoryFontLoader* const loader = [factory] {
        auto* const made = new MemoryFontLoader();
        // A no-op on this build, kept for one that implements it. See
        // GetMemoryLoader.
        (void)factory->RegisterFontFileLoader(made);
        return made;
    }();
    return loader;
}

// A font file over a copy of `data` that the shim owns and can free.
//
// DWrite reads through the stream above and keeps a bounded number of them, so
// what a process holds stops growing however many fonts it goes on to load.
// The in-memory loader instead keeps every font for the life of the loader.
//
// Returns nullptr if this build will not read a stream the caller supplies,
// and the caller falls back to the loader. Called with g_faces_mutex held.
IDWriteFontFile* CustomFontFileLocked(IDWriteFactory* factory, const FT_Byte* data,
                                      const FT_Long size, uint64_t* const id_out)
{
    *id_out = 0;
    // Copies the whole font with g_faces_mutex held, so an exception would
    // unwind past the manual unlock below and leave the lock held for the life
    // of the process. Answering null sends the caller to the loader, which is
    // where it goes when this build will not read a stream at all.
    FontBlob* blob = nullptr;
    try {
        blob = new FontBlob{ .bytes = std::vector(data, data + size), .refs = 1 };
    } catch (const std::bad_alloc&) {
        LogLine("no memory for a %ld-byte font copy; using the loader instead",
                static_cast<long>(size));
        return nullptr;
    }

    pthread_mutex_lock(&g_blobs_mutex);
    const uint64_t id = g_next_blob_id++;
    FontBlobs()[id] = blob;
    pthread_mutex_unlock(&g_blobs_mutex);

    IDWriteFontFile* file = nullptr;
    HRESULT hr = factory->CreateCustomFontFileReference(&id, sizeof id,
                                                        MemoryFontLoaderInstance(factory), &file);
    if (SUCCEEDED(hr) && file != nullptr) {
        // Analyze is what reads the stream, so it doubles as the test of
        // whether this build honors a loader of ours at all.
        BOOL supported = FALSE;
        DWRITE_FONT_FILE_TYPE file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
        DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
        UINT32 face_count = 0;
        hr = file->Analyze(&supported, &file_type, &face_type, &face_count);
        if (SUCCEEDED(hr) && supported) {
            *id_out = id;
            return file;
        }
        file->Release();
    }
    LogLine("memory font: DWrite would not read a stream of ours (hr=0x%08X), "
            "handing the bytes over instead", static_cast<unsigned>(hr));
    ForgetBlob(id);
    return nullptr;
}

// A font file over `data`, from the cache when the same bytes are already
// registered. Adds the caller's reference either way.
IDWriteFontFile* MemoryFontFileLocked(IDWriteFactory* factory, const FT_Byte* data,
                                      const FT_Long size)
{
    if (data == nullptr || size <= 0) {
        return nullptr;
    }
    const FontDigest hash = HashFontBytes(data, size);
    for (MemoryFontFile& cached : MemoryFontFiles()) {
        if (cached.size != size || cached.hash != hash) {
            continue;
        }
        // The bytes decide, not the hash. Two distinct fonts of one length
        // whose 64-bit values collide would otherwise share a file, and every
        // glyph of the second would be drawn from the first. This runs once
        // per face and reads bytes HashFontBytes has just walked.
        if (cached.blob != 0 && !SameFontBytes(cached.blob, data, size)) {
            continue;
        }
        cached.file->AddRef();
        cached.uses++;
        return cached.file;
    }

    uint64_t blob = 0;
    IDWriteFontFile* file = CustomFontFileLocked(factory, data, size, &blob);
    if (file == nullptr) {
        IDWriteInMemoryFontFileLoader* loader = GetMemoryLoader(factory);
        if (loader == nullptr) {
            return nullptr;
        }
        // A null owner object makes DWrite copy the data. The font file's
        // lifetime is then independent of the caller's buffer, and DWrite can
        // never read a buffer the application has already freed.
        //
        // Passing an owner takes the branch that does not copy; the loader
        // holds a reference to it instead. That saves nothing here, because
        // the loader outlives the face, so the owner would have to keep the
        // bytes alive itself.
        const HRESULT hr = loader->CreateInMemoryFontFileReference(
            factory, data, static_cast<UINT32>(size), nullptr, &file);
        if (FAILED(hr) || file == nullptr) {
            return nullptr;
        }
    }
    LogLine("memory font (%ld bytes) handed over %s", static_cast<long>(size),
            blob != 0 ? "as a stream" : "as a copy");
    file->AddRef();   // the cache's reference
    MemoryFontFiles().push_back(
        MemoryFontFile{.size = size, .hash = hash, .file = file, .blob = blob, .uses = 1});
    return file;
}

// One face is done with `file`. Called with g_faces_mutex held, and before
// the face's own reference goes, so the entry cannot be looking at a pointer
// that has already been freed.
//
// Does nothing for a file the cache never held - a face opened by path - so
// the caller need not know which kind it has.
void ForgetMemoryFontLocked(const IDWriteFontFile* file)
{
    std::vector<MemoryFontFile>& files = MemoryFontFiles();
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].file != file) {
            continue;
        }
        if (--files[i].uses > 0) {
            return;
        }
        if (files[i].blob == 0) {
            // The in-memory loader's copy, which nothing gives back - see
            // GetMemoryLoader. The entry stays so that the next face for
            // these bytes reuses it rather than paying for a second copy.
            return;
        }
        const uint64_t blob = files[i].blob;
        IDWriteFontFile* const cached = files[i].file;
        files[i] = files.back();
        files.pop_back();
        // The cache's reference first, so DWrite can drop its stream; the
        // bytes then go with that stream, or right here if it already has.
        cached->Release();
        ForgetBlob(blob);
        return;
    }
}

IDWriteFontFile* GetFontFileLocked(FaceEntry* entry, IDWriteFactory* factory)
{
    if (entry->dwrite_file != nullptr || entry->file_failed) {
        return entry->dwrite_file;
    }
    entry->file_failed = true;  // cleared on success

    IDWriteFontFile* file = nullptr;
    HRESULT hr = E_FAIL;
    if (!entry->path.empty()) {
        const std::u16string wide = ToUtf16(entry->path);
        if (wide.empty()) {
            LogLine("face %p: path is not valid UTF-8, cannot pass to DWrite", reinterpret_cast<void*>(entry->face));
            return nullptr;
        }
        hr = factory->CreateFontFileReference(wide.c_str(), nullptr, &file);
    } else if (entry->memory != nullptr && entry->memory_size > 0) {
        file = MemoryFontFileLocked(factory, entry->memory, entry->memory_size);
        hr = file != nullptr ? S_OK : E_FAIL;
    }
    // A failed call can still hand back an object, and the fallback below
    // overwrites the variable holding it.
    if (FAILED(hr) && file != nullptr) {
        file->Release();
        file = nullptr;
    }
    if ((FAILED(hr) || file == nullptr) && entry->face != nullptr &&
        entry->face->stream != nullptr && entry->face->stream->read == nullptr &&
        entry->face->stream->base != nullptr && entry->face->stream->size > 0) {
        // The file could not be opened by path. A sandboxed process does
        // that, and so does a path FreeType accepts and DWrite cannot use.
        // FreeType holds the whole file in memory either way, as an mmap or a
        // read-in buffer, so the same bytes are handed over from there.
        file = MemoryFontFileLocked(factory, entry->face->stream->base,
                                    static_cast<FT_Long>(entry->face->stream->size));
        hr = file != nullptr ? S_OK : E_FAIL;
        if (file != nullptr) {
            LogLine("face %p: font file opened from FreeType's stream (%lu bytes)",
                    reinterpret_cast<void*>(entry->face), entry->face->stream->size);
        }
    }
    if (FAILED(hr) || file == nullptr) {
        LogLine("face %p: no font file reference, hr=0x%08X", reinterpret_cast<void*>(entry->face), static_cast<unsigned>(hr));
        return nullptr;
    }

    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 face_count = 0;
    hr = file->Analyze(&supported, &file_type, &face_type, &face_count);
    if (FAILED(hr) || !supported) {
        LogLine("face %p: DWrite does not support this font (hr=0x%08X, fileType=%d)",
                reinterpret_cast<void*>(entry->face), static_cast<unsigned>(hr), static_cast<int>(file_type));
        ForgetMemoryFontLocked(file);
        file->Release();
        return nullptr;
    }

    entry->dwrite_file = file;
    entry->dwrite_face_type = face_type;
    entry->file_failed = false;
    return file;
}

// The object that can produce a face at an arbitrary axis position. Only a
// variable face needs one. Called with g_faces_mutex held.
IDWriteFontResource* GetFontResourceLocked(FaceEntry* entry, IDWriteFactory* factory)
{
    if (entry->dwrite_resource != nullptr || entry->resource_failed) {
        return entry->dwrite_resource;
    }
    entry->resource_failed = true;

    IDWriteFontFile* file = GetFontFileLocked(entry, factory);
    if (file == nullptr) {
        return nullptr;
    }

    IDWriteFactory6* factory6 = nullptr;
    const GUID iid6 = DWRITE_UUIDOF(IDWriteFactory6);
    HRESULT hr = factory->QueryInterface(iid6, reinterpret_cast<void**>(&factory6));
    if (FAILED(hr) || factory6 == nullptr) {
        LogSkipOnce(entry->face, "no IDWriteFactory6, so a variable face cannot be positioned");
        return nullptr;
    }

    IDWriteFontResource* resource = nullptr;
    hr = factory6->CreateFontResource(file, static_cast<UINT32>(entry->face_index & 0xFFFF), &resource);
    factory6->Release();
    if (FAILED(hr) || resource == nullptr) {
        LogLine("face %p: CreateFontResource failed hr=0x%08X", reinterpret_cast<void*>(entry->face), static_cast<unsigned>(hr));
        return nullptr;
    }

    entry->dwrite_resource = resource;
    entry->resource_failed = false;
    return resource;
}

// Builds the IDWriteFontFace for one (simulations, axis position) pair and
// caches it. Called with g_faces_mutex held. Returns null on any failure and
// remembers that, so a font DWrite cannot open is not retried per glyph.
IDWriteFontFace* GetDWriteFaceLocked(FaceEntry* entry, IDWriteFactory* factory,
                                     const DWRITE_FONT_SIMULATIONS simulations,
                                     const std::vector<DWRITE_FONT_AXIS_VALUE>& axes)
{
    for (const CachedDWriteFace& cached : entry->dwrite_faces) {
        if (cached.simulations == simulations && SameAxes(cached.axes, axes)) {
            return cached.face;  // null here means "tried, failed, do not retry"
        }
    }

    IDWriteFontFace* dwrite_face = nullptr;
    HRESULT hr = E_FAIL;

    if (axes.empty()) {
        // Not `const*`: its address goes to CreateFontFace, which takes
        // IDWriteFontFile* const*, and const-qualifying the pointee makes
        // that a different type. misc-const-correctness asks for it anyway.
        // NOLINTNEXTLINE(misc-const-correctness)
        IDWriteFontFile* file = GetFontFileLocked(entry, factory);
        if (file != nullptr) {
            hr = factory->CreateFontFace(entry->dwrite_face_type, 1, &file, static_cast<UINT32>(entry->face_index & 0xFFFF), simulations, &dwrite_face);
        }
    } else {
        IDWriteFontResource* resource = GetFontResourceLocked(entry, factory);
        if (resource != nullptr) {
            IDWriteFontFace5* face5 = nullptr;
            hr = resource->CreateFontFace(simulations, axes.data(), static_cast<UINT32>(axes.size()), &face5);
            dwrite_face = reinterpret_cast<IDWriteFontFace*>(face5);
        }
    }

    if (FAILED(hr) || dwrite_face == nullptr) {
        LogLine("face %p: CreateFontFace failed hr=0x%08X", reinterpret_cast<void*>(entry->face), static_cast<unsigned>(hr));
        dwrite_face = nullptr;
    } else {
        char described[160] = "";
        int used = 0;
        for (const DWRITE_FONT_AXIS_VALUE& axis : axes) {
            char tag[5] = {};
            std::memcpy(tag, &axis.axisTag, 4);
            used += std::snprintf(described + used, sizeof(described) - static_cast<size_t>(used), "%s%s %g",
                                  used ? " " : ", ", tag, static_cast<double>(axis.value));
            if (used >= static_cast<int>(sizeof(described)) - 24) {
                break;
            }
        }
        LogLine("face %p: DWrite face ready (%s, index %ld%s%s%s)", reinterpret_cast<void*>(entry->face),
                entry->path.empty() ? "in-memory" : entry->path.c_str(),
                static_cast<long>(entry->face_index & 0xFFFF),
                simulations & DWRITE_FONT_SIMULATIONS_BOLD ? ", simulated bold" : "",
                simulations & DWRITE_FONT_SIMULATIONS_OBLIQUE ? ", simulated oblique" : "",
                described);
    }

    // Bounded: a page can ask for many weights of one variable family, and
    // each is a separate face. Well past what any real page uses.
    if (entry->dwrite_faces.size() >= 24) {
        if (entry->dwrite_faces.front().face != nullptr) {
            entry->dwrite_faces.front().face->Release();
        }
        entry->dwrite_faces.erase(entry->dwrite_faces.begin());
    }
    entry->dwrite_faces.push_back(
        CachedDWriteFace{.face = dwrite_face, .simulations = simulations, .axes = axes});
    return dwrite_face;
}

// A FreeType 16.16 matrix as a DWRITE_MATRIX, with the pixel aspect folded in.
//
// The two spaces disagree about which way y grows: FreeType's grows upward,
// DirectWrite's device space grows downward. A linear map changes basis under
// that flip by conjugation with diag(1, -1), which negates exactly the two
// off-diagonal terms and leaves the diagonal alone. Concretely, a rightward
// lean in FreeType becomes a leftward one in DWrite unless the shear flips
// sign; get it wrong and slanted text is drawn the wrong way round.
//
// DWRITE_MATRIX is row-vector (x' = x*m11 + y*m21), FT_Matrix is column
// (x' = x*xx + y*xy), so the off-diagonal terms also swap position.
//
// The aspect goes on the two terms the x char size scaled. A caller that
// hands FreeType two char sizes has already divided its matrix by both, one
// per column, where the DWRITE_MATRIX Windows builds is divided by the y size
// throughout: SkScalerContext_CairoFT::computeShapeMatrix normalizes by major
// and minor and SkScalerContext_win_dw's fSkXform is the whole matrix over
// scale.fY, and platform/unix/font.rs and platform/windows/font.rs split the
// same way around compute_scale. Multiplying the x column by the ratio of the
// two sizes puts it back on the y size's footing. In a row-vector matrix that
// column is m11 and m12; the ratio is one unless the two sizes differ, which
// is why only a shape with a zero diagonal shows it.
DWRITE_MATRIX ToDWriteMatrix(const FT_Matrix& m, const float x_over_y)
{
    DWRITE_MATRIX out = {};
    out.m11 = static_cast<FLOAT>(static_cast<double>(m.xx) / 65536.0) * x_over_y;
    out.m12 = static_cast<FLOAT>(static_cast<double>(-m.yx) / 65536.0) * x_over_y;
    out.m21 = static_cast<FLOAT>(static_cast<double>(-m.xy) / 65536.0);
    out.m22 = static_cast<FLOAT>(static_cast<double>(m.yy) / 65536.0);
    return out;
}

// FIREFOX PARITY. Recover the exact synthetic-oblique skew from the 16.16 one
// FreeType was given. Gecko's Windows code passes the skew through the
// DirectWrite API as a float and reaches this shim as a fixed-point matrix,
// and the two are not the same number:
//
//   webrender_api/src/font.rs   SyntheticItalics::to_skew()
//       self.to_radians().tan()                  // f32, angle in 1/256 degree
//   platform/windows/font.rs    DWRITE_MATRIX { m21: shape.skew_x, .. }
//   platform/unix/font.rs       xy: (shape.skew_x * -65536.0) as FT_Fixed
//
// tan(14 degrees) is 16339.96 in 16.16, so the default oblique arrives as
// 16339, and slanting the outline by that instead of by the float shifts
// coverage across the glyph.
//
// It is recoverable because the angle is not free: SyntheticItalics carries an
// i16 count of 1/256 degrees, so only 45569 skews can be sent. Take the arc
// tangent, round to the nearest such angle, recompute the tangent Firefox's
// way, and require the result to truncate back to exactly the value received.
// A matrix that did not come from this computation is then left alone. Only an
// otherwise-identity matrix with a horizontal shear is considered.
//
// Every number Firefox quantizes on the way into FreeType and leaves
// unquantized on Windows is one of these. The rest are in
// platform/unix/font.rs
// and are either exact for upright horizontal text (xx, yy, ft_delta, the
// subpixel dx, the cbox shift) or recovered elsewhere - see ExactEmSize for the
// char size.
// Whether a recovered shear arrived on its own or behind the quarter turn
// vertical text is drawn under.
enum class ObliqueForm { None, Upright, QuarterTurn };

#if CLEARTYPE_FIREFOX_PARITY

float SkewForAngle(const int angle_256)
{
    // f32 throughout, as SyntheticItalics is: degrees, then radians through an
    // f32 pi/180, then tanf.
    const float degrees = static_cast<float>(angle_256) / 256.0f;
    const float radians = degrees * (static_cast<float>(M_PI) / 180.0f);
    return tanf(radians);
}

ObliqueForm ExactObliqueSkew(const FT_Matrix& m, float* skew)
{
    // Upright text sends the shear on its own. Vertical text sends the same
    // shear behind the quarter turn the glyph is drawn under, and the product
    // [[0,1],[-1,0]] * [[1,s],[0,1]] carries it in yy.
    ObliqueForm form;
    FT_Fixed sent;
    if (m.xx == 0x10000 && m.yy == 0x10000 && m.yx == 0 && m.xy != 0) {
        form = ObliqueForm::Upright;
        sent = m.xy;
    } else if (m.xx == 0 && m.xy == 0x10000 && m.yx == -0x10000 && m.yy != 0) {
        form = ObliqueForm::QuarterTurn;
        sent = -m.yy;
    } else {
        return ObliqueForm::None;
    }
    const double received = static_cast<double>(sent) / 65536.0;
    const double angle = std::atan(received) * (180.0 / M_PI) * 256.0;
    const long nearest = static_cast<long>(std::floor(angle + 0.5));
    for (long candidate = nearest - 1; candidate <= nearest + 1; ++candidate) {
        if (candidate == 0 || candidate < -89L * 256 || candidate > 89L * 256) {
            continue;
        }
        const float value = SkewForAngle(static_cast<int>(candidate));
        // `as FT_Fixed` in Rust truncates toward zero; the multiply by 65536 is
        // exact in f32, so doing it in double here cannot disagree.
        const FT_Fixed round_trip = static_cast<FT_Fixed>(std::trunc(static_cast<double>(value) * 65536.0));
        if (round_trip == sent) {
            *skew = value;
            return form;
        }
    }
    return ObliqueForm::None;
}

#else  // !CLEARTYPE_FIREFOX_PARITY

// Without Firefox to reproduce there is no sender whose quantization is known,
// so the matrix is whatever the caller sent.
inline ObliqueForm ExactObliqueSkew(const FT_Matrix&, float*) { return ObliqueForm::None; }

#endif

// Within one 16.16 step, because that is how a caller writes 1.
//
// FT_Matrix is 16.16, so 1 is 0x10000. Firefox hands this path 0xFFFF on both
// diagonals for text it is not scaling at all, which is one step short of
// exact.
// Windows never sees that value, since platform/windows/font.rs builds its
// shape from font.transform.invert_scale(y_scale, y_scale), the same scale on
// both axes, which is exactly 1 and is dropped as an identity.
//
// The gap is 15 parts per million, too small to move an advance or a bounding
// box and still enough to shift a stem's coverage by one step when the stem
// edge sits near a boundary. So a matrix this close to identity is identity,
// and no transform goes to DirectWrite.
bool MatrixIsIdentity(const FT_Matrix& m)
{
    auto within_one = [](const FT_Fixed v, const FT_Fixed want) {
        return v >= want - 1 && v <= want + 1;
    };
    return within_one(m.xx, 0x10000) && within_one(m.xy, 0) &&
           within_one(m.yx, 0) && within_one(m.yy, 0x10000);
}

#if CLEARTYPE_FIREFOX_PARITY

// The TTAG_* spellings live in a private FreeType header.
constexpr FT_ULong kTagEBLC = FT_MAKE_TAG('E', 'B', 'L', 'C');
constexpr FT_ULong kTagEBSC = FT_MAKE_TAG('E', 'B', 'S', 'C');
constexpr FT_ULong kTagCOLR = FT_MAKE_TAG('C', 'O', 'L', 'R');

bool ReadSfntTable(FT_Face face, const FT_ULong tag, std::vector<FT_Byte>* out)
{
    ft_load_sfnt_table_fn real = real_FT_Load_Sfnt_Table();
    if (real == nullptr) {
        return false;
    }
    FT_ULong length = 0;
    if (real(face, tag, 0, nullptr, &length) != 0 || length == 0) {
        return false;
    }
    out->resize(length);
    return real(face, tag, 0, out->data(), &length) == 0;
}

uint32_t ReadU32(const std::vector<FT_Byte>& t, const size_t at)
{
    return (static_cast<uint32_t>(t[at]) << 24) | (static_cast<uint32_t>(t[at + 1]) << 16) |
           (static_cast<uint32_t>(t[at + 2]) << 8) | static_cast<uint32_t>(t[at + 3]);
}

uint16_t ReadU16(const std::vector<FT_Byte>& t, const size_t at)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(t[at]) << 8) | static_cast<uint16_t>(t[at + 1]));
}

// gfxFontEntry::HasFontTable, as gfxFT2FontEntryBase::FaceHasTable answers it:
// FT_Load_Sfnt_Table reports a non-zero length.
bool FaceHasTable(FT_Face face, const FT_ULong tag)
{
    ft_load_sfnt_table_fn real = real_FT_Load_Sfnt_Table();
    if (real == nullptr) {
        return false;
    }
    FT_ULong length = 0;
    return real(face, tag, 0, nullptr, &length) == 0 && length > 0;
}

// nsMathUtils.h NS_lround.
//
// Gecko spells this as a bare cast of x +/- 0.5, which is the same number:
// truncation toward zero of a non-negative value is floor, and of a negative
// value is ceil, so each branch keeps the sign it already had. The cast of a
// sum is what bugprone-incorrect-roundings flags.
int32_t NSlround(const double x)
{
    return x >= 0.0 ? static_cast<int32_t>(std::floor(x + 0.5)) : static_cast<int32_t>(std::ceil(x - 0.5));
}

// gfx/thebes/gfxDWriteFontList.cpp gfxDWriteFontEntry::IsCJKFont: the
// OS/2 ulCodePageRange1 bits for codepages 932, 936, 949, 950 and 1361.
bool FaceIsCJKLocked(FaceEntry* entry)
{
    if (entry->is_cjk >= 0) {
        return entry->is_cjk != 0;
    }
    entry->is_cjk = 0;
    ft_get_sfnt_table_fn real = real_FT_Get_Sfnt_Table();
    if (real != nullptr) {
        const auto* os2 = static_cast<const TT_OS2*>(real(entry->face, FT_SFNT_OS2));
        if (os2 != nullptr && os2->version != 0xFFFF) {
            constexpr uint32_t kCJKCodePageBits =
                (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21);
            if ((os2->ulCodePageRange1 & kCJKCodePageBits) != 0) {
                entry->is_cjk = 1;
            }
        }
    }
    return entry->is_cjk != 0;
}

// gfx/thebes/gfxDWriteFonts.cpp gfxDWriteFont::HasBitmapStrikeForSize. The
// tables are read once; the per-size answer walks them the way that function
// does: the first EBLC bitmapSizeTable whose ppemX and ppemY both equal the
// size decides, and counts only if it spans four or more glyphs; failing
// that, any EBSC bitmapScaleTable at the size counts.
void ReadStrikesLocked(FaceEntry* entry)
{
    if (entry->strikes_read) {
        return;
    }
    entry->strikes_read = true;

    std::vector<FT_Byte> table;
    // EBLCHeader (8 bytes) then numSizes BitmapSizeTables of 48 bytes each;
    // startGlyphIndex at +40, endGlyphIndex at +42, ppemX at +44, ppemY at +45.
    if (ReadSfntTable(entry->face, kTagEBLC, &table) && table.size() >= 8 &&
        ReadU32(table, 0) == 0x00020000) {
        const uint32_t count = ReadU32(table, 4);
        if (count <= 0xFFFF && table.size() >= 8 + static_cast<size_t>(count) * 48) {
            for (uint32_t i = 0; i < count; ++i) {
                const size_t at = 8 + static_cast<size_t>(i) * 48;
                EblcStrike strike;
                strike.first = ReadU16(table, at + 40);
                strike.last = ReadU16(table, at + 42);
                strike.ppem_x = table[at + 44];
                strike.ppem_y = table[at + 45];
                entry->eblc.push_back(strike);
            }
        }
    }
    // EBSCHeader (8 bytes) then numSizes BitmapScaleTables of 28 bytes each;
    // ppemX at +24, ppemY at +25.
    if (ReadSfntTable(entry->face, kTagEBSC, &table) && table.size() >= 8 &&
        ReadU32(table, 0) == 0x00020000) {
        const uint32_t count = ReadU32(table, 4);
        if (count <= 0xFFFF && table.size() >= 8 + static_cast<size_t>(count) * 28) {
            for (uint32_t i = 0; i < count; ++i) {
                const size_t at = 8 + static_cast<size_t>(i) * 28;
                EbscStrike strike;
                strike.ppem_x = table[at + 24];
                strike.ppem_y = table[at + 25];
                entry->ebsc.push_back(strike);
            }
        }
    }
}

bool HasBitmapStrikeForSizeLocked(FaceEntry* entry, const uint32_t size)
{
    ReadStrikesLocked(entry);
    for (const EblcStrike& strike : entry->eblc) {
        if (strike.ppem_x == size && strike.ppem_y == size) {
            if (strike.last >= static_cast<uint32_t>(strike.first) + 3) {
                return true;
            }
            break;
        }
    }
    for (const EbscStrike& strike : entry->ebsc) {
        if (strike.ppem_x == size && strike.ppem_y == size) {
            return true;
        }
    }
    return false;
}

// gfxDWriteFont::ComputeMetrics:
//   fe->IsCJKFont() && HasBitmapStrikeForSize(NS_lround(mAdjustedSize))
// and gfxDWriteFont::GetScaledFont, which sends the same predicate to
// WebRender as FontInstanceFlags::EMBEDDED_BITMAPS when the system rendering
// mode is DWRITE_RENDERING_MODE_DEFAULT. Called with g_faces_mutex held.
bool IsBitmapFontLocked(FaceEntry* entry, const double adjusted_size)
{
    if (entry == nullptr || !WindowsMetrics() || !(adjusted_size > 0.0)) {
        return false;
    }
    return FaceIsCJKLocked(entry) &&
           HasBitmapStrikeForSizeLocked(entry, static_cast<uint32_t>(NSlround(adjusted_size)));
}

// gfxDWriteFontEntry::HasFontTable(TRUETYPE_TAG('C','O','L','R')), cached.
bool FaceHasCOLRLocked(FaceEntry* entry)
{
    if (entry->has_colr < 0) {
        entry->has_colr = FaceHasTable(entry->face, kTagCOLR) ? 1 : 0;
    }
    return entry->has_colr != 0;
}

#else  // !CLEARTYPE_FIREFOX_PARITY

inline bool IsBitmapFontLocked(FaceEntry*, double) { return false; }
inline bool FaceHasCOLRLocked(FaceEntry*) { return false; }

#endif

// The rendering mode platform/windows/font.rs asks DirectWrite for when no
// flag pins one:
//
//   dwrite_render_mode():
//     font_face.get_recommended_rendering_mode_default_params(em_size, 1.0,
//                                                             measure_mode)
//     if mode == DWRITE_RENDERING_MODE_OUTLINE { CLEARTYPE_NATURAL_SYMMETRIC }
//
// dwrote's get_recommended_rendering_mode_default_params passes the factory's
// default IDWriteRenderingParams (third_party/rust/dwrote/src/font_face.rs),
// which is what GetDefaultRenderingParams() returns here. The newer
// IDWriteFontFace2 overloads are not used by WebRender and are not used here.
//
// Cached per (face, em size): the answer depends on the em size and the font's
// gasp table, and on nothing else. Called with g_faces_mutex held.
//
// Two of the modes it can return have no DWRITE_TEXTURE_CLEARTYPE_3x1 to
// give, and glyphs at those sizes fall through to FreeType:
//
//   ALIASED   bilevel, and lives in a 1x1 texture. Asking for a 3x1 texture
//             succeeds and returns empty bounds.
//   OUTLINE   above 96px DirectWrite stops rasterizing and hands back
//             geometry for the caller to fill. Asking for a 3x1 texture in
//             this mode divides by zero inside DWriteCore, which the Windows
//             build does as well, so never pass it.
CachedMode ResolveModeLocked(FaceEntry* entry, IDWriteFontFace* dwrite_face, const float em_size,
                             const Options& options)
{
    uint32_t key = 0;
    std::memcpy(&key, &em_size, sizeof(key));
    for (const CachedMode& cached : entry->modes) {
        if (cached.em_size_bits == key) {
            return cached;
        }
    }

    CachedMode resolved;
    resolved.em_size_bits = key;
    resolved.rendering_mode = DWRITE_RENDERING_MODE_DEFAULT;
    resolved.grid_fit_mode = options.grid_fit_mode;
    resolved.answered = false;

    DWRITE_RENDERING_MODE mode = DWRITE_RENDERING_MODE_DEFAULT;
    const HRESULT hr = dwrite_face->GetRecommendedRenderingMode(em_size, 1.0f, options.measuring_mode, GetDefaultRenderingParams(), &mode);
    if (SUCCEEDED(hr)) {
        if (mode == DWRITE_RENDERING_MODE_OUTLINE) {
            mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;
        }
        resolved.rendering_mode = mode;
        resolved.answered = true;
        LogLine("face %p at %.2fpx: DirectWrite recommends %s",
                reinterpret_cast<void*>(entry->face), static_cast<double>(em_size),
                RenderingModeName(mode));
    } else {
        LogLine("face %p at %.2fpx: GetRecommendedRenderingMode hr=0x%08X, falling through",
                reinterpret_cast<void*>(entry->face), static_cast<double>(em_size),
                static_cast<unsigned>(hr));
    }

    // Bounded: a face is drawn at a handful of sizes.
    if (entry->modes.size() >= 32) {
        entry->modes.erase(entry->modes.begin());
    }
    entry->modes.push_back(resolved);
    return resolved;
}

// ---------------------------------------------------------------------------
// Subpixel phase.
//
// Cairo (and WebRender, and anything else following Skia's convention)
// quantizes a glyph's true horizontal position to quarter-pixel phases and
// bakes that phase into the outline with FT_Outline_Translate immediately
// before calling FT_Render_Glyph. This shim rasterizes from the font file
// rather than from the caller's outline, so it would otherwise discard that
// phase, snapping every glyph to a whole pixel and visibly changing the
// spacing the application laid out.
//
// Recording the translate is exact. It is the value the caller asked for,
// not a reconstruction, and DWrite takes it directly as a float
// baselineOriginX with no outline to translate.
//
// The render that follows consumes the entry. A translate is used by exactly
// the next render of that outline, or by nothing, so a stale phase cannot
// survive into a later glyph.
// ---------------------------------------------------------------------------

// Also carries how the outline was *reshaped* since it was loaded.
//
// A toolkit fakes a missing bold or italic face by thickening or slanting the
// outline in place, through FT_GlyphSlot_Embolden, FT_GlyphSlot_Oblique or
// FT_Outline_Transform. It does not load a different font file, and it does
// not go through FT_Set_Transform. Qt6 and Cairo both work this way.
//
// This shim re-rasterizes from the font file by glyph index, so none of that
// reshaping is visible in what it produces. Left alone it would render
// emboldened text at the regular weight.
//
// Windows draws a family with no real bold or italic file through
// DWRITE_FONT_SIMULATIONS_BOLD and _OBLIQUE, applied by the rasterizer when
// the face is created, so the reshape is classified and turned back into those
// flags. Any other linear map rides through as a DWRITE_MATRIX; only a
// singular one is declined, which is what `unrepresentable` records.
struct PendingOutline
{
    const FT_Outline* outline;
    FT_Pos dx;
    FT_Pos dy;
    // synthesize_italics' origin shift for vertical text, in device pixels,
    // recovered rather than read back from the 26.6 delta FreeType was given.
    // Zero when there is none or it could not be recovered, in which case the
    // delta is in dx and dy like any other translate.
    double delta_x;
    double delta_y;
    DWRITE_FONT_SIMULATIONS simulations;
    bool unrepresentable;
    bool has_matrix;
    FT_Matrix matrix;
    // The face whose glyph slot `outline` points into, once a load has said
    // so; null for a bare outline the caller owns. Only used to drop the
    // entry when that face goes away - see ForgetPendingOutlines.
    FT_Face face;
};

std::vector<PendingOutline> g_shifts;
pthread_mutex_t g_shifts_mutex = PTHREAD_MUTEX_INITIALIZER;

// Finds or creates the entry for `outline`. Called with g_shifts_mutex held.
PendingOutline* PendingOutlineLocked(const FT_Outline* outline)
{
    for (PendingOutline& pending : g_shifts) {
        if (pending.outline == outline) {
            return &pending;
        }
    }
    // Bounded, because consume-on-render is not a guarantee of consumption:
    // a caller that touches an outline and then renders it in some mode other
    // than LCD leaves its entry behind forever, keyed by a pointer a later
    // glyph slot could be allocated at. Live entries number one per glyph
    // slot mid-render; anything approaching this is already leakage.
    if (g_shifts.size() >= 64) {
        const PendingOutline& oldest = g_shifts.front();
        if (oldest.dx != 0 || oldest.dy != 0 || oldest.has_matrix ||
            oldest.simulations != DWRITE_FONT_SIMULATIONS_NONE || oldest.unrepresentable) {
            LogLine("pending outline table full; dropping %p with shift %ld,%ld",
                    static_cast<const void*>(oldest.outline), static_cast<long>(oldest.dx),
                    static_cast<long>(oldest.dy));
        }
        g_shifts.erase(g_shifts.begin());
    }
    constexpr FT_Matrix identity = { .xx = 0x10000, .xy = 0, .yx = 0, .yy = 0x10000 };
    g_shifts.push_back(PendingOutline{.outline = outline, .dx = 0, .dy = 0, .delta_x = 0.0,
                                      .delta_y = 0.0,
                                      .simulations = DWRITE_FONT_SIMULATIONS_NONE,
                                      .unrepresentable = false, .has_matrix = false,
                                      .matrix = identity, .face = nullptr});
    return &g_shifts.back();
}

// The reshape maps onto a DirectWrite simulation; flags accumulate, so a
// glyph that is both emboldened and slanted asks for both.
void RecordSimulation(const FT_Outline* outline, const DWRITE_FONT_SIMULATIONS simulation)
{
    if (!InterposerWanted()) {
        return;
    }
    pthread_mutex_lock(&g_shifts_mutex);
    PendingOutline* pending = PendingOutlineLocked(outline);
    pending->simulations =
        static_cast<DWRITE_FONT_SIMULATIONS>(static_cast<int>(pending->simulations) | static_cast<int>(simulation));
    pthread_mutex_unlock(&g_shifts_mutex);
}

// 16.16 fixed-point matrix product, in FreeType's own convention:
//     x' = (x * xx + y * xy) >> 16
//     y' = (x * yx + y * yy) >> 16
// `second` is applied after `first`. Done here so this file needs no further
// symbol from libfreetype.
FT_Matrix MatrixConcat(const FT_Matrix& first, const FT_Matrix& second)
{
    FT_Matrix out;
    out.xx = MulFix(first.xx, second.xx) + MulFix(first.yx, second.xy);
    out.xy = MulFix(first.xy, second.xx) + MulFix(first.yy, second.xy);
    out.yx = MulFix(first.xx, second.yx) + MulFix(first.yx, second.yy);
    out.yy = MulFix(first.xy, second.yx) + MulFix(first.yy, second.yy);
    return out;
}

bool MatrixIsSingular(const FT_Matrix& m)
{
    // A singular matrix collapses the outline to a line or a point, which
    // DirectWrite has no way to draw and FreeType does by flattening it, so
    // that one really does fall through.
    //
    // The determinant is computed in 64 bits. FT_MulFix rounds its 16.16
    // product, which reads every determinant below 0.5/65536 as zero and would
    // call a uniform scale of about 1/362 singular.
    const int64_t det = static_cast<int64_t>(m.xx) * m.yy - static_cast<int64_t>(m.xy) * m.yx;
    return det == 0;
}

// The reshape is a linear map with no simulation to match it. It accumulates,
// so a caller that transforms an outline twice gets the product.
void RecordMatrix(const FT_Outline* outline, const FT_Matrix& matrix)
{
    if (!InterposerWanted()) {
        return;
    }
    pthread_mutex_lock(&g_shifts_mutex);
    PendingOutline* pending = PendingOutlineLocked(outline);
    pending->matrix = pending->has_matrix ? MatrixConcat(pending->matrix, matrix) : matrix;
    pending->has_matrix = true;
    pthread_mutex_unlock(&g_shifts_mutex);
}

// The reshape has no simulation to map to; the glyph falls through.
void RecordUnrepresentable(const FT_Outline* outline)
{
    if (!InterposerWanted()) {
        return;
    }
    pthread_mutex_lock(&g_shifts_mutex);
    PendingOutlineLocked(outline)->unrepresentable = true;
    pthread_mutex_unlock(&g_shifts_mutex);
}

void RecordShift(const FT_Outline* outline, const FT_Pos dx, const FT_Pos dy)
{
    if (!InterposerWanted()) {
        return;
    }
    pthread_mutex_lock(&g_shifts_mutex);
    PendingOutline* pending = PendingOutlineLocked(outline);
    pending->dx += dx;
    pending->dy += dy;
    pthread_mutex_unlock(&g_shifts_mutex);
}

#if CLEARTYPE_FIREFOX_PARITY
// The same, for a translate whose exact value is known in device pixels
// rather than 26.6. Device space is y-down, so no sign flip here.
void RecordExactDelta(const FT_Outline* outline, const double dx, const double dy)
{
    if (!InterposerWanted()) {
        return;
    }
    pthread_mutex_lock(&g_shifts_mutex);
    PendingOutline* pending = PendingOutlineLocked(outline);
    pending->delta_x += dx;
    pending->delta_y += dy;
    pthread_mutex_unlock(&g_shifts_mutex);
}
#endif  // CLEARTYPE_FIREFOX_PARITY

// Reads and clears whatever was recorded for `outline`. What is recorded
// applies to exactly the next render of that outline, or to nothing.
PendingOutline TakeOutlineState(const FT_Outline* outline)
{
    PendingOutline state{.outline = outline, .dx = 0, .dy = 0, .delta_x = 0.0, .delta_y = 0.0,
                         .simulations = DWRITE_FONT_SIMULATIONS_NONE,
                         .unrepresentable = false,
                         .has_matrix = false, .matrix = {}, .face = nullptr};
    pthread_mutex_lock(&g_shifts_mutex);
    for (size_t i = 0; i < g_shifts.size(); ++i) {
        if (g_shifts[i].outline == outline) {
            state = g_shifts[i];
            g_shifts[i] = g_shifts.back();
            g_shifts.pop_back();
            break;
        }
    }
    pthread_mutex_unlock(&g_shifts_mutex);
    return state;
}

// The same, without consuming the entry: for a measurement that precedes
// the render the entry belongs to.
PendingOutline PeekOutlineState(const FT_Outline* outline)
{
    PendingOutline state{.outline = outline, .dx = 0, .dy = 0, .delta_x = 0.0, .delta_y = 0.0,
                         .simulations = DWRITE_FONT_SIMULATIONS_NONE,
                         .unrepresentable = false,
                         .has_matrix = false, .matrix = {}, .face = nullptr};
    pthread_mutex_lock(&g_shifts_mutex);
    for (size_t i = 0; i < g_shifts.size(); ++i) {
        if (g_shifts[i].outline == outline) {
            state = g_shifts[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_shifts_mutex);
    return state;
}

// Says which face's slot this outline lives in. Called after a load, because
// that is the first moment the association is known. The entry itself often
// exists already, created by the FT_Outline_Translate that FreeType performs
// inside FT_Load_Glyph to apply an FT_Set_Transform delta.
void SetPendingOutlineFace(const FT_Outline* outline, FT_Face face)
{
    if (!InterposerWanted()) {
        return;
    }
    pthread_mutex_lock(&g_shifts_mutex);
    for (PendingOutline& pending : g_shifts) {
        if (pending.outline == outline) {
            pending.face = face;
            break;
        }
    }
    pthread_mutex_unlock(&g_shifts_mutex);
}

// Consume-on-render does not guarantee consumption - an outline touched and
// then rendered in a mode this shim declines keeps its entry - so the entries
// a dying face owns are dropped here. Left to age out, they would sit in a
// table keyed by an address the next glyph slot may be allocated at.
void ForgetPendingOutlines(FT_Face face)
{
    pthread_mutex_lock(&g_shifts_mutex);
    for (size_t i = g_shifts.size(); i-- > 0;) {
        if (g_shifts[i].face == face) {
            g_shifts[i] = g_shifts.back();
            g_shifts.pop_back();
        }
    }
    pthread_mutex_unlock(&g_shifts_mutex);
}

// Drops anything recorded for `outline` without applying it.
//
// A glyph slot is reused for every glyph loaded into it, so the outline
// pointer is stable for the life of the face while the glyph in it is not.
// TakeOutlineState clears an entry only when that outline is rasterized
// through this shim. A caller that translates an outline and then renders it
// in another mode, or never renders it at all, leaves the entry behind. The
// next glyph loaded into that slot then inherits a translation meant for a
// different glyph, which rasterizes correctly and lands outside the bitmap the
// caller sized for it: a letter missing from the page with its advance intact.
// WebRender caches rasterized glyphs, so only the first rasterization in a
// process can go wrong, which makes it intermittent.
void DiscardOutlineState(const FT_Outline* outline)
{
    pthread_mutex_lock(&g_shifts_mutex);
    for (size_t i = 0; i < g_shifts.size(); ++i) {
        if (g_shifts[i].outline == outline) {
            g_shifts[i] = g_shifts.back();
            g_shifts.pop_back();
            break;
        }
    }
    pthread_mutex_unlock(&g_shifts_mutex);
}

// ---------------------------------------------------------------------------
// Outline ownership.
//
// FT_Outline_Get_Bitmap - the entry point WebRender-style rasterizers use
// instead of FT_Render_Glyph - is handed a bare FT_Outline and a bitmap to
// fill. No face, no glyph index, which is everything DirectWrite needs. But
// the outline it is given is always the one FT_Load_Glyph just filled in, so
// watching the loads is enough to say which glyph of which face an outline
// is.
//
// Not consume-once, unlike the translate table: the outline stays valid, and
// gets rasterized, until the next load into that slot overwrites it.
// ---------------------------------------------------------------------------

struct OutlineOwner
{
    const FT_Outline* outline;
    FT_Face face;
    FT_UInt glyph_index;
};

std::vector<OutlineOwner> g_owners;
pthread_mutex_t g_owners_mutex = PTHREAD_MUTEX_INITIALIZER;

void RecordOutlineOwner(FT_Face face, const FT_UInt glyph_index)
{
    if (!dwcft::Enabled() || face == nullptr || face->glyph == nullptr) {
        return;
    }
    const FT_Outline* outline = &face->glyph->outline;

    pthread_mutex_lock(&g_owners_mutex);
    for (OutlineOwner& owner : g_owners) {
        if (owner.outline == outline) {
            owner.face = face;
            owner.glyph_index = glyph_index;
            pthread_mutex_unlock(&g_owners_mutex);
            return;
        }
    }
    // One entry per live glyph slot, which is one per face; the cap only
    // matters if a host churns through faces without ever rasterizing.
    if (g_owners.size() >= 128) {
        g_owners.erase(g_owners.begin());
    }
    g_owners.push_back(OutlineOwner{.outline = outline, .face = face,
                                    .glyph_index = glyph_index});
    pthread_mutex_unlock(&g_owners_mutex);
}

// Drops every entry naming `face`. Matched by face rather than by outline
// because the slot this face owned may already be freed, so face->glyph must
// not be dereferenced here.
void ForgetOutlineOwners(FT_Face face)
{
    pthread_mutex_lock(&g_owners_mutex);
    for (size_t i = g_owners.size(); i-- > 0;) {
        if (g_owners[i].face == face) {
            g_owners[i] = g_owners.back();
            g_owners.pop_back();
        }
    }
    pthread_mutex_unlock(&g_owners_mutex);
}

bool FindOutlineOwner(const FT_Outline* outline, FT_Face* face, FT_UInt* glyph_index)
{
    bool found = false;
    pthread_mutex_lock(&g_owners_mutex);
    for (const OutlineOwner& owner : g_owners) {
        if (owner.outline == outline) {
            *face = owner.face;
            *glyph_index = owner.glyph_index;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_owners_mutex);
    return found;
}

// ---------------------------------------------------------------------------
// Installed-bitmap side table.
//
// FT_Load_Glyph resets slot->bitmap.buffer to null while preparing the slot
// for the next glyph, without freeing whatever was there, regardless of any
// ownership flag; FT_Bitmap_Done() and FT_GlyphSlot_Own_Bitmap() do not
// change that. By the time this code runs there is no pointer left in the
// slot to free, so the buffer is remembered here and freed here.
//
// Bounded by the number of live glyph slots - one per FT_Face - not by the
// number of glyphs rendered.
// ---------------------------------------------------------------------------

struct TrackedBuffer
{
    FT_GlyphSlot slot;
    // The face the slot belongs to, so the buffer can still be found once the
    // face is being destroyed: EraseFaceLocked may run when the slot is
    // already gone, and reading slot->face there would be a use after free.
    FT_Face face;
    unsigned char* buffer;
};

std::vector<TrackedBuffer> g_tracked;
pthread_mutex_t g_tracked_mutex = PTHREAD_MUTEX_INITIALIZER;

void FreeTrackedBuffer(FT_GlyphSlot slot)
{
    unsigned char* buffer = nullptr;
    pthread_mutex_lock(&g_tracked_mutex);
    for (size_t i = 0; i < g_tracked.size(); ++i) {
        if (g_tracked[i].slot == slot) {
            buffer = g_tracked[i].buffer;
            g_tracked[i] = g_tracked.back();
            g_tracked.pop_back();
            break;
        }
    }
    pthread_mutex_unlock(&g_tracked_mutex);
    std::free(buffer);
}

// The last bitmap this shim installed on a face's slot is still allocated
// when the face is destroyed: nothing renders through that slot again, so
// InstallBitmap's free-the-previous-one never runs for it. One buffer per
// face that ever rasterized, which is a leak for any host that opens faces
// over a long session.
void FreeTrackedBuffersForFace(FT_Face face)
{
    std::vector<unsigned char*> buffers;
    pthread_mutex_lock(&g_tracked_mutex);
    for (size_t i = g_tracked.size(); i-- > 0;) {
        if (g_tracked[i].face == face) {
            buffers.push_back(g_tracked[i].buffer);
            g_tracked[i] = g_tracked.back();
            g_tracked.pop_back();
        }
    }
    pthread_mutex_unlock(&g_tracked_mutex);
    for (unsigned char* buffer : buffers) {
        std::free(buffer);
    }
}

void TrackBuffer(FT_GlyphSlot slot, unsigned char* buffer)
{
    pthread_mutex_lock(&g_tracked_mutex);
    g_tracked.push_back(TrackedBuffer{.slot = slot, .face = slot->face,
                                      .buffer = buffer});
    pthread_mutex_unlock(&g_tracked_mutex);
}

// ---------------------------------------------------------------------------
// Rendering.
// ---------------------------------------------------------------------------

// The axis position FreeType is currently rendering this face at, in the
// form DirectWrite wants.
//
// Neither the tag encoding nor the axis list can be taken across as-is:
//
//   - The tag encodings are byte-reversed. FreeType packs 'wght' big-endian
//     (FT_MAKE_TAG), DirectWrite packs it little-endian
//     (DWRITE_MAKE_FONT_AXIS_TAG), so the same axis is 0x77676874 on one
//     side and 0x74686777 on the other.
//   - DirectWrite reports *more* axes than the font has: it synthesizes the
//     standard wdth/ital/slnt alongside the font's own. Segoe UI Variable
//     has 2 axes by FreeType's count and 5 by DirectWrite's.
//   - The orders differ too - FreeType lists wght first for that font,
//     DirectWrite lists opsz first.
//
// So axes are matched by tag and never by index, and only the ones FreeType
// actually reports are sent; DirectWrite fills in the rest from its
// defaults.
//
// Returns an empty vector for a face that is not being varied, which selects
// the plain CreateFontFace path.
std::vector<DWRITE_FONT_AXIS_VALUE> GetAxisValues(FT_Face face)
{
    std::vector<DWRITE_FONT_AXIS_VALUE> axes;
    if ((face->face_flags & FT_FACE_FLAG_MULTIPLE_MASTERS) == 0) {
        return axes;
    }

    FT_MM_Var* mm = nullptr;
    if (FT_Get_MM_Var(face, &mm) != 0 || mm == nullptr) {
        return axes;
    }
    std::vector<FT_Fixed> coords(mm->num_axis);
    if (FT_Get_Var_Design_Coordinates(face, mm->num_axis, coords.data()) == 0) {
        for (FT_UInt i = 0; i < mm->num_axis; ++i) {
            DWRITE_FONT_AXIS_VALUE value;
            value.axisTag = static_cast<DWRITE_FONT_AXIS_TAG>(__builtin_bswap32(static_cast<uint32_t>(mm->axis[i].tag)));
            value.value = static_cast<FLOAT>(static_cast<double>(coords[i]) / 65536.0);
            axes.push_back(value);
        }
    }
    FT_Done_MM_Var(face->glyph->library, mm);
    return axes;
}

// The pixel size a char-size request works out to, in 26.6, the way
// FT_Request_Metrics computes it before rounding to whole pixels.
FT_Fixed PixelSize26_6(const FT_F26Dot6 char_size, const FT_UInt resolution)
{
    if (char_size <= 0) {
        return 0;
    }
    const long res = resolution != 0 ? static_cast<long>(resolution) : 72L;
    return char_size * res / 72L;
}

// Remembers what the caller asked for, so the integer-ppem snapping FreeType
// may have applied afterwards can be undone. Called from every entry point
// that sets a size, including with 0, which clears a stale value.
void RecordRequestedEmSize(FT_Face face, const FT_Fixed pixel_size_26_6)
{
    if (!InterposerWanted()) {
        return;
    }
    pthread_mutex_lock(&g_faces_mutex);
    if (FaceEntry* entry = FindFaceLocked(face)) {
        entry->requested_em_26_6 = pixel_size_26_6;
    }
    pthread_mutex_unlock(&g_faces_mutex);
}

#if CLEARTYPE_FIREFOX_PARITY
FT_Fixed RequestedEmSize(FT_Face face)
{
    FT_Fixed requested = 0;
    pthread_mutex_lock(&g_faces_mutex);
    if (const FaceEntry* entry = FindFaceLocked(face)) {
        requested = entry->requested_em_26_6;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return requested;
}
#endif

// The em size DirectWrite is asked for, and the pixel aspect that could not
// be folded into it.
//
// Taken from the 16.16 scale factors, not from x_ppem and y_ppem, which are
// whole pixels. FT_MulFix(units_per_EM, x_scale) is the scaled em in 26.6.

#if CLEARTYPE_FIREFOX_PARITY
// Undo the quantization Firefox applies on the way into FreeType.
//
// The size Firefox lays out and rasterizes with is a CSS computed font-size,
// which is an nscoord: gfx/src/nsFontMetrics.cpp builds the gfxFontStyle with
// `gfxFloat(aFont.size.ToAppUnits()) / mP2A`, mP2A being
// AppUnitsPerDevPixel(), 60 on an unscaled display. Both FreeType callers
// quantize that to 26.6 on the way in:
//
//   gfxFT2FontBase::LockFTFace:   charSize = mFTSize * 64.0 + 0.5  (truncated)
//   platform/unix/font.rs:        (req_size * x_scale * 64.0 + 0.5) as FT_F26Dot6
//                                 with req_size = font.size.to_f64_px(), an f32
//
// The Windows consumers use the unquantized value. mFUnitsConvFactor is
// float(mAdjustedSize / designUnitsPerEm) in gfxDWriteFont::ComputeMetrics,
// and platform/windows/font.rs passes `fontEmSize: size` as an f32.
//
// So the original is recovered as the n/60 whose quantization reproduces the
// received value, in either of the two arithmetics above. A size that came
// from neither is left as FreeType reported it. AppUnitsPerDevPixel is
// assumed to be 60, the unscaled value at device pixel ratio 1.
constexpr int kAppUnitsPerCSSPixel = 60;

// App units per device pixel, as this presentation counts them.
//
// AppUnitsPerCSSPixel is always 60; this is 60 only while a device pixel is a
// CSS pixel, and 48, 40 or 30 at a device pixel ratio of 1.25, 1.5 or 2. It
// stays 60 until the patch in libxul_patch.cpp reads one out of a shaped
// text, so a build where that never happens behaves exactly as it did before.
std::atomic<int> g_app_units_per_dev_px{60};

bool ExactEmSize(const FT_Fixed em_size_26_6, double* exact)
{
    if (em_size_26_6 <= 0) {
        return false;
    }
    // How many app units this presentation puts in a device pixel. Sixty while
    // a device pixel is a CSS pixel, and 48, 40 or 30 at a ratio of 1.25, 1.5
    // or 2, which is the grid the size was laid out on. Recovering against 60
    // on a scaled display lands on a size a fraction away from the one Windows
    // measures and rasterizes, and a fraction is enough where the outline is
    // grid-fitted.
    const int per_px = g_app_units_per_dev_px.load(std::memory_order_relaxed);
    const long app_units =
        static_cast<long>(std::floor(static_cast<double>(em_size_26_6) * per_px / 64.0 + 0.5));
    if (app_units <= 0) {
        return false;
    }
    const double px = static_cast<double>(app_units) / per_px;
    // Both are floor for the same reason as above: app_units > 0 was checked
    // just now, so px is positive and truncation and floor agree.
    const FT_Fixed layout_quantized = static_cast<FT_Fixed>(std::floor(px * 64.0 + 0.5));
    // Narrowed to f32 the way WebRender holds the size, then multiplied in
    // double: the promotion is the arithmetic that was already happening,
    // spelled out.
    const FT_Fixed webrender_quantized = static_cast<FT_Fixed>(
        std::floor(static_cast<double>(static_cast<float>(px)) * 64.0 + 0.5));
    if (layout_quantized != em_size_26_6 && webrender_quantized != em_size_26_6) {
        return false;
    }
    *exact = px;
    return true;
}

// The size the caller asked for, as (req_size * scale * 64.0 + 0.5) reaches
// FT_Set_Char_Size. Rust's `as` truncates toward zero and every size here is
// positive, so this is the floor.
FT_Fixed CharSize26_6(const double px)
{
    return static_cast<FT_Fixed>(std::trunc(px * 64.0 + 0.5));
}

// gfxFontStyle::AdjustForSubSuperscript, forwards.
//
// The size is in device pixels and the thresholds are CSS pixels, so the
// source converts before it compares:
//
//     float cssSize = size * aAppUnitsPerDevPixel / AppUnitsPerCSSPixel();
//
// The two are equal only at a device pixel ratio of one. Above it the device
// size is the larger number and comparing it against the thresholds picks a
// different ratio, which is a different em and a different glyph. Narrowed to
// f32 the way the source narrows it.
double SubSuperSize(const double size)
{
    const int per_px = g_app_units_per_dev_px.load(std::memory_order_relaxed);
    const auto css = static_cast<double>(static_cast<float>(
        size * static_cast<double>(per_px) / static_cast<double>(kAppUnitsPerCSSPixel)));
    if (css < 20.0) {
        return size * 0.82;
    }
    if (css >= 45.0) {
        return size * 0.667;
    }
    const double t = (css - 20.0) / (45.0 - 20.0);
    return size * ((1.0 - t) * 0.82 + t * 0.667);
}

// The size behind a 26.6 value that no whole app unit produces.
//
// A sub- or superscript run is drawn at 0.82, 0.667 or an interpolation of the
// size around it, and small capitals at 0.8, so the product is not a whole app
// unit and ExactEmSize cannot invert the rounding of it. The reduction is
// inverted by search instead, over the whole app units it could have started
// from, which the ratio bounds. Where the size is Gecko's own the claimed-size
// table answers first and this is never reached; it is for the process that
// rasterizes, which holds no gfxFont to claim from.
//
// Two starting sizes can land on one 26.6 value (16 px reduced by 0.82 and
// 16.4 px by 0.8 both give 13.12), so what has to be unique is the size
// answered with, not the one behind it. Two that disagree answer nothing and
// FreeType's own value stands.
bool ReducedEmSize(const FT_Fixed em_size_26_6, double* exact)
{
    if (em_size_26_6 <= 0) {
        return false;
    }
    // The search runs over every app unit the reduction could have started
    // from, thousands of them at a large size, and every glyph in a run asks
    // about the same size. Remembering the last answer turns a run into one
    // search.
    thread_local FT_Fixed memo_size = 0;
    thread_local double memo_px = 0.0;
    if (memo_size == em_size_26_6) {
        if (!(memo_px > 0.0)) {
            return false;
        }
        *exact = memo_px;
        return true;
    }
    memo_size = em_size_26_6;
    memo_px = 0.0;

    // The size the reduction started from is gfxFontStyle::size, which
    // nsFontMetrics divides out of nsFont::size by the app units in a device
    // pixel, so that is the grid it sits on. Searching the CSS grid instead
    // steps finer than the sizes Gecko can hold on a scaled display, and two
    // neighbours then reduce onto one 26.6 value and the search calls it
    // ambiguous.
    const int per_px = g_app_units_per_dev_px.load(std::memory_order_relaxed);
    const double got = static_cast<double>(em_size_26_6) / 64.0;
    const auto lo = static_cast<long>(std::floor(got * per_px / 0.82));
    const auto hi = static_cast<long>(std::ceil(got * per_px / 0.667)) + 1;
    // The two reductions are searched apart. A sub- or superscript size and a
    // small-capital one can quantize alike from different starting sizes, and
    // read together they cancel and leave FreeType's own value, which is
    // neither of them. Each is asked to name one size on its own, and the
    // sub- or superscript answer stands where both do, since that is the
    // reduction that put the size off the app unit grid ExactEmSize inverts.
    double answers[2] = {0.0, 0.0};
    bool found[2] = {false, false};
    bool ambiguous[2] = {false, false};
    for (long units = lo > 1 ? lo : 1; units <= hi; ++units) {
        const double size = static_cast<double>(units) / per_px;
        const double reduced[2] = { SubSuperSize(size), size * 0.8 };
        for (unsigned which = 0; which < 2; ++which) {
            const double px = reduced[which];
            if (CharSize26_6(px) != em_size_26_6) {
                continue;
            }
            // Two starting sizes can reach one size by different arithmetic
            // and land a double's last bit apart, which is one size and not
            // two. run.fontEmSize is a float and both narrow onto it.
            if (found[which] &&
                static_cast<float>(px) != static_cast<float>(answers[which])) {
                ambiguous[which] = true;
                continue;
            }
            answers[which] = px;
            found[which] = true;
        }
    }
    for (unsigned which = 0; which < 2; ++which) {
        if (found[which] && !ambiguous[which]) {
            memo_px = answers[which];
            *exact = answers[which];
            return true;
        }
    }
    return false;
}

// The unquantized sizes Gecko has named for a face.
//
// gfxFT2FontBase::LockFTFace rounds gfxFont::mAdjustedSize into
// FT_Set_Char_Size's 26.6, and ExactEmSize can only invert that where the size
// was a whole app unit. A sub- or superscript run is not one.
// gfxFontStyle::AdjustForSubSuperscript multiplies by 0.82, 0.667 or an
// interpolation between them, so 16 px becomes 13.12 and arrives as 840. The
// size itself does reach this library, as mFTSize through the InitMetrics hook
// in libxul_patch.cpp, so it is kept here and matched back by the 26.6 value
// it quantizes to.
//
// One face carries several at once, since a superscript run and the text
// around it share it, and a size is asked for when a glyph is drawn rather
// than when it is claimed. A face keeps its own row, so claiming a size for
// one face never drops another's, and a row is bounded and drops its oldest
// entry. Rows are taken in turn once they run out, which loses the sizes of
// whichever face has gone longest without one.
constexpr size_t kClaimedFaces = 64;
constexpr size_t kClaimedPerFace = 16;

struct ClaimedSize
{
    FT_Fixed size_26_6;
    double px;
    // The mFTSize the font paired with this size, as a char size. They are the
    // same value except where FindClosestSize moved it. See AdjustedSizeForFace.
    FT_Fixed ft_26_6;
    // Whether a font named this as its own mAdjustedSize. A size recorded as
    // the mFTSize a font settled on is the face's size and not a font's, and
    // names nothing to measure at.
    bool from_adjusted;
    // When the face was last set to this size. See TouchClaimedSize.
    unsigned long long used;
};

struct ClaimedFace
{
    FT_Face face;
    size_t next;
    ClaimedSize sizes[kClaimedPerFace];
};

ClaimedFace g_claimed_faces[kClaimedFaces] = {};
size_t g_claimed_next_face = 0;
unsigned long long g_claim_clock = 0;
pthread_mutex_t g_claimed_mutex = PTHREAD_MUTEX_INITIALIZER;

// Both take g_claimed_mutex.
ClaimedFace* FindClaimedFaceLocked(FT_Face face)
{
    for (ClaimedFace& row : g_claimed_faces) {
        if (row.face == face) {
            return &row;
        }
    }
    return nullptr;
}

ClaimedFace* ClaimRowLocked(FT_Face face)
{
    if (ClaimedFace* row = FindClaimedFaceLocked(face)) {
        return row;
    }
    for (ClaimedFace& row : g_claimed_faces) {
        if (row.face == nullptr) {
            row.face = face;
            return &row;
        }
    }
    ClaimedFace* row = &g_claimed_faces[g_claimed_next_face];
    g_claimed_next_face = (g_claimed_next_face + 1) % std::size(g_claimed_faces);
    *row = ClaimedFace{};
    row->face = face;
    return row;
}

void RecordClaimedSize(FT_Face face, const double px, const double ft_px,
                       const bool from_adjusted)
{
    if (face == nullptr || !(px > 0.0) || !(px < 65536.0)) {
        return;
    }
    const FT_Fixed size_26_6 = CharSize26_6(px);
    const FT_Fixed ft_26_6 = ft_px > 0.0 && ft_px < 65536.0 ? CharSize26_6(ft_px) : size_26_6;
    pthread_mutex_lock(&g_claimed_mutex);
    ClaimedFace* row = ClaimRowLocked(face);
    for (ClaimedSize& slot : row->sizes) {
        if (slot.px > 0.0 && slot.size_26_6 == size_26_6) {
            slot.px = px;
            slot.ft_26_6 = ft_26_6;
            // Only ever raised. A font that named this size as its own is
            // still one when the same value arrives again as a face's size.
            slot.from_adjusted = slot.from_adjusted || from_adjusted;
            slot.used = ++g_claim_clock;
            pthread_mutex_unlock(&g_claimed_mutex);
            return;
        }
    }
    row->sizes[row->next] = ClaimedSize{ .size_26_6 = size_26_6, .px = px,
                                         .ft_26_6 = ft_26_6, .from_adjusted = from_adjusted,
                                         .used = ++g_claim_clock };
    row->next = (row->next + 1) % kClaimedPerFace;
    pthread_mutex_unlock(&g_claimed_mutex);
}

// Notes that a face has been set to a size it was claimed at.
//
// gfxFT2FontBase::LockFTFace sets the face to mFTSize with equal width and
// height whenever another owner had it last, so a font still on the page keeps
// asking for its size and one whose page is gone stops. A face outlives both,
// and this is what separates the sizes it still carries.
void TouchClaimedSize(FT_Face face, const FT_Fixed size_26_6)
{
    if (face == nullptr || size_26_6 <= 0) {
        return;
    }
    pthread_mutex_lock(&g_claimed_mutex);
    if (ClaimedFace* row = FindClaimedFaceLocked(face)) {
        for (ClaimedSize& slot : row->sizes) {
            if (slot.px > 0.0 && slot.size_26_6 == size_26_6) {
                slot.used = ++g_claim_clock;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_claimed_mutex);
}

bool ClaimedSizeFor(FT_Face face, const FT_Fixed size_26_6, double* px)
{
    bool found = false;
    pthread_mutex_lock(&g_claimed_mutex);
    if (const ClaimedFace* row = FindClaimedFaceLocked(face)) {
        for (const ClaimedSize& slot : row->sizes) {
            if (slot.px > 0.0 && slot.size_26_6 == size_26_6) {
                *px = slot.px;
                found = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_claimed_mutex);
    return found;
}

// The size WebRender is rasterizing at, taken from its own font instance.
//
// A page's fonts are laid out in the content process, where PreClaimOwnSize
// records the size gfxFont settled on. WebRender rasterizes in the parent
// process, which has no gfxFont for them, so the size arrives there only as
// FT_Set_Char_Size's 26.6 value. A whole app unit and the sub- or superscript
// reduction of another size can round onto one of those, and the inversion
// then has two answers with nothing to choose between them.
// libxul_patch.cpp names the instance ahead of each glyph, and the size the
// instance holds is the one Gecko sent.
thread_local const void* g_wr_instance = nullptr;

// How far into the instance to look. WebRender's FontInstance carries Rust's
// own field order, so where the size sits is found rather than assumed.
constexpr size_t kWrInstanceScan = 64;
// How many distinct sizes have to agree before one offset is believed.
constexpr unsigned kWrSizeAgreements = 4;
std::atomic<int> g_wr_size_offset{-1};
uint32_t g_wr_size_candidates = ~0u;
unsigned g_wr_size_agreed = 0;
FT_Fixed g_wr_size_last = 0;
pthread_mutex_t g_wr_size_mutex = PTHREAD_MUTEX_INITIALIZER;

extern "C" void CleartypeNoteWebRenderInstance(const void* instance)
{
    g_wr_instance = instance;
}

// The instance's own size, where it accounts for the size FreeType was set to.
//
// Reading it also spends it: an instance names one glyph load, and a size that
// arrived from anywhere else must not be answered with the last one seen.
double WebRenderInstanceSize(const FT_Fixed char_size)
{
    const void* instance = g_wr_instance;
    g_wr_instance = nullptr;
    if (instance == nullptr || char_size <= 0) {
        return 0.0;
    }
    const auto* bytes = static_cast<const unsigned char*>(instance);
    const int known = g_wr_size_offset.load(std::memory_order_relaxed);
    if (known >= 0) {
        float px = 0.0f;
        std::memcpy(&px, bytes + known, sizeof(px));
        return CharSize26_6(px) == char_size ? static_cast<double>(px) : 0.0;
    }

    uint32_t here = 0;
    for (size_t off = 0; off + sizeof(float) <= kWrInstanceScan; off += sizeof(float)) {
        float px = 0.0f;
        std::memcpy(&px, bytes + off, sizeof(px));
        if (px > 0.0f && px < 65536.0f && CharSize26_6(px) == char_size) {
            here |= 1u << (off / sizeof(float));
        }
    }
    // A transformed instance is set to a size no field of it holds on its own,
    // and says nothing about where the size sits.
    if (here == 0) {
        return 0.0;
    }
    pthread_mutex_lock(&g_wr_size_mutex);
    g_wr_size_candidates &= here;
    // Distinct sizes are what separate the size from a field that happens to
    // agree with it at one of them.
    if (char_size != g_wr_size_last) {
        g_wr_size_last = char_size;
        ++g_wr_size_agreed;
    }
    if (g_wr_size_agreed >= kWrSizeAgreements && g_wr_size_candidates != 0 &&
        (g_wr_size_candidates & (g_wr_size_candidates - 1)) == 0) {
        const auto slot = static_cast<unsigned>(__builtin_ctz(g_wr_size_candidates));
        g_wr_size_offset.store(static_cast<int>(slot * sizeof(float)),
                               std::memory_order_relaxed);
    }
    pthread_mutex_unlock(&g_wr_size_mutex);
    return 0.0;
}

// Every distinct size the table holds for a face.
size_t ClaimedSizesFor(FT_Face face, double* out, unsigned long long* used, const size_t room)
{
    size_t found = 0;
    pthread_mutex_lock(&g_claimed_mutex);
    if (const ClaimedFace* row = FindClaimedFaceLocked(face)) {
        for (const ClaimedSize& slot : row->sizes) {
            if (!(slot.px > 0.0) || found == room) {
                continue;
            }
            bool already = false;
            for (size_t i = 0; i < found; ++i) {
                already = already || out[i] == slot.px;
            }
            if (!already) {
                out[found] = slot.px;
                used[found] = slot.used;
                ++found;
            }
        }
    }
    pthread_mutex_unlock(&g_claimed_mutex);
    return found;
}

// The size Gecko is measuring at, where the face is set to another one.
//
// gfxFT2FontBase::FindClosestSize clamps mFTSize to a pixel and to a bitmap
// strike, and GetFTGlyphExtents scales what it reads back by
// GetAdjustedSize() / mFTSize afterwards. Windows measures at the adjusted
// size itself, and the two are not one scale apart: gfxFont::PostShapingFixup
// adds a tracking whose floor is a quarter of a pixel however small the size
// is, and a whole app unit is a coarser step than the advances under a pixel
// differ by.
//
// The pair is recorded together, so a claimed size whose mFTSize is the one the
// face carries names the size the font behind it is really drawing at. A face
// measured at a pixel and at a fraction of one carries a claim for each, and
// both name the same mFTSize, so the most recent stands; a font at the size the
// face is already set to leaves nothing to answer.
bool AdjustedSizeForFace(FT_Face face, const FT_Fixed ft_26_6, double* adjusted)
{
    double best = 0.0;
    pthread_mutex_lock(&g_claimed_mutex);
    if (const ClaimedFace* row = FindClaimedFaceLocked(face)) {
        unsigned long long newest = 0;
        for (const ClaimedSize& slot : row->sizes) {
            if (!(slot.px > 0.0) || !slot.from_adjusted || slot.ft_26_6 != ft_26_6) {
                continue;
            }
            if (best == 0.0 || slot.used > newest) {
                best = slot.px;
                newest = slot.used;
            }
        }
    }
    pthread_mutex_unlock(&g_claimed_mutex);
    if (!(best > 0.0) || CharSize26_6(best) == ft_26_6) {
        return false;
    }
    *adjusted = best;
    return true;
}

// FT_F26Dot6(fScaleY * 64.0f + 0.5f) in SkScalerContext_CairoFT::Lock. The
// cast truncates and the arithmetic is single precision.
FT_Fixed SkiaCharSize26_6(const float px)
{
    // NOLINTNEXTLINE(bugprone-incorrect-roundings)  -- the truncation is the mirror
    return static_cast<FT_Fixed>(px * 64.0f + 0.5f);
}

// The one product of a size and a whole number of 1024ths that quantizes to
// the received value, if the size names exactly one. `units` is the number of
// 1024ths, which is what separates two sizes that both name one. See
// ScaledEmSize.
bool ProductOfSize(const double size, const FT_Fixed em_size_26_6, double* px,
                   long* units = nullptr)
{
    if (!(size > 0.0)) {
        return false;
    }
    const double estimate = static_cast<double>(em_size_26_6) * 16.0 / size;
    // Past this a 1024th is finer than the float holding the product, so the
    // check below cannot separate two of them.
    if (!(estimate > 0.0) || estimate > 16777216.0) {
        return false;
    }
    // sk_relax rounds and the 26.6 value rounds again, so the 1024th the
    // received value implies can be one out either way.
    const long seed = lround(estimate);
    bool found = false;
    long answer_units = 0;
    double answer = 0.0;
    for (long step = -1; step <= 1; ++step) {
        const long units_here = seed + step;
        if (units_here <= 0) {
            continue;
        }
        const float product = static_cast<float>(units_here) / 1024.0f * static_cast<float>(size);
        if (SkiaCharSize26_6(product) != em_size_26_6) {
            continue;
        }
        if (found && static_cast<double>(product) != answer) {
            return false;
        }
        answer = static_cast<double>(product);
        answer_units = units_here;
        found = true;
    }
    if (found) {
        *px = answer;
        if (units != nullptr) {
            *units = answer_units;
        }
    }
    return found;
}

// The em behind a 26.6 value that Skia scaled by a device matrix.
//
// SkScalerContext::MakeRecAndEffects puts the device matrix into
// SkScalerContextRec::fPost2x2 through sk_relax, which rounds every term to a
// 1024th, and leaves fTextSize the font's own size. getSingleMatrix multiplies
// the two, so a uniform device scale reaches SkScalerContext_CairoFT::Lock as
// fTextSize times a whole number of 1024ths, and SkScalerContext_win_dw reads
// the same rec and rasterizes at that same product. The product is the answer
// and FT_Set_Char_Size's 26.6 rounding of it is all that has to be undone.
//
// The product is no longer a whole app unit, and a 26.6 step is wider than an
// app unit, so ExactEmSize almost always finds one to answer with and it is
// the wrong size. The search here runs over the sizes Gecko has named for the
// face instead, which are the fTextSize the product could have been built
// from, and over the 1024ths around the one the received value implies.
//
// A face outlives the fonts that name sizes for it, so its row can hold
// several sizes whose products quantize alike, and the one nearest the value
// received answers.
//
// Under 16 px of size a 1024th is finer than a 26.6 step, so two neighboring
// 1024ths of one size quantize alike as well. Nothing else in
// FT_Set_Char_Size carries which of them it was, and the two answers are half
// a level of coverage apart on a color glyph's layers, so the search declines
// and the inversions below answer instead.
//
// The largest of these scales is a color glyph's. gfxFont::RenderColorGlyph
// draws one into a surface scaled by two and COLRFonts walks a paint graph
// whose transform nodes scale again, so the layers of a 16 px emoji are
// rasterized at over a hundred pixels.

// The size Skia asks a scaler for when it wants a glyph's path.
//
// SkFont::getPaths hands the font to setupForAsPaths before anything else,
// which replaces the size with SkFontPriv::kCanonicalTextSizeForPaths and
// returns the ratio for the caller to scale the finished path by, and
// SkStrikeSpec::MakeWithNoDevice then builds the strike under no device
// matrix. So every path request reaches the scaler as this size with the axes
// equal, whatever size the font is drawn at, and nothing about it came from a
// device scale for the search below to recover.
//
// ScaledFontBase::GetPathForGlyphs is that call, and COLRFonts takes it for
// every gradient layer's clip and for the box RenderColorGlyph rounds out, so
// an emoji reaches it many times per glyph.
constexpr double kSkiaPathSize = 64.0;

// Whether a received char size is that one.
bool IsSkiaPathSize(const FT_Fixed em_size_26_6, const float x_over_y)
{
    return em_size_26_6 == SkiaCharSize26_6(static_cast<float>(kSkiaPathSize)) &&
           x_over_y >= 0.9999f && x_over_y <= 1.0001f;
}

// What the Skia scaler drawing on this thread was built with, or nothing.
// See CleartypeSkiaScaler.
struct SkiaScalerRec
{
    double text_size;
    double pre_scale_x;
    double pre_skew_x;
    double post[4];
    bool valid;
};
thread_local SkiaScalerRec g_skia_rec = {};

// Its size alone, which is all the search over products needs.
thread_local double g_skia_text_size = 0.0;

// True when the search named a product, which it writes to `exact`.
bool ScaledEmSize(FT_Face face, const FT_Fixed em_size_26_6, double* exact)
{
    if (em_size_26_6 <= 0) {
        return false;
    }
    // The scaler's own fTextSize, where a hook has read one out. Every size
    // below is a guess at that value, so nothing here can improve on it.
    if (g_skia_text_size > 0.0 &&
        ProductOfSize(g_skia_text_size, em_size_26_6, exact)) {
        return true;
    }
    // Every size the face's row can hold, so which of them answers does not
    // depend on the order they were recorded in.
    double claimed[kClaimedPerFace];
    unsigned long long used[kClaimedPerFace];
    const size_t count = ClaimedSizesFor(face, claimed, used, std::size(claimed));
    bool found = false;
    double answer = 0.0;
    unsigned long long answer_used = 0;
    for (size_t i = 0; i < count; ++i) {
        double px = 0.0;
        if (!ProductOfSize(claimed[i], em_size_26_6, &px)) {
            continue;
        }
        // The size the face has been set to more recently answers. It is the
        // one a font on the page is still drawing at, the other belonging to a
        // page that is gone.
        if (found && px != answer && used[i] < answer_used) {
            continue;
        }
        answer = px;
        answer_used = used[i];
        found = true;
    }
    if (!found) {
        return false;
    }
    *exact = answer;
    return true;
}

// The ratio of a face's two char sizes, with the 26.6 rounding taken out of
// both.
//
// ToDWriteMatrix folds the ratio into the terms the x char size scaled, so it
// has to be the ratio of the sizes themselves. computeShapeMatrix takes major
// as the length of the shape's x column, and sk_relax leaves that column a
// pair of whole 1024ths of the size, so the length is a square root and only
// its dominant term is a product the search can name.
//
// The two are the same value to the 26.6 the face received whenever the other
// term is small enough: the length exceeds the dominant term by the square of
// their ratio over two, which under half a 26.6 step is a difference the
// received value cannot carry. A column that far from an axis is a real
// rotation, where the length is a square root the search would answer with a
// stray product, so those keep the ratio of the received sizes.
//
// The two sizes are equal for every shape that scales the axes alike, so
// nothing is done for those and the ratio stays one.
bool ExactAspect(FT_Face face, const FT_Matrix& shape, const double em_size, float* x_over_y)
{
    if (!(em_size > 0.0) || face->size == nullptr || face->units_per_EM == 0) {
        return false;
    }
    const FT_Fixed x_26_6 = MulFix(face->units_per_EM, face->size->metrics.x_scale);
    const FT_Fixed y_26_6 = MulFix(face->units_per_EM, face->size->metrics.y_scale);
    if (x_26_6 <= 0 || x_26_6 == y_26_6) {
        return false;
    }
    const double column_x = std::fabs(static_cast<double>(shape.xx));
    const double column_y = std::fabs(static_cast<double>(shape.yx));
    const double length = std::hypot(column_x, column_y);
    if (!(length > 0.0)) {
        return false;
    }
    const double excess = (length - std::max(column_x, column_y)) / length;
    if (!(excess * (static_cast<double>(x_26_6) / 64.0) < 1.0 / 128.0)) {
        return false;
    }
    double x_exact = 0.0;
    if (!ClaimedSizeFor(face, x_26_6, &x_exact) && !ScaledEmSize(face, x_26_6, &x_exact)) {
        return false;
    }
    *x_over_y = static_cast<float>(x_exact / em_size);
    return true;
}

// A face's slots, dropped with the face. The next allocation to land on that
// address would otherwise inherit its sizes.
void ForgetClaimedSizes(FT_Face face)
{
    pthread_mutex_lock(&g_claimed_mutex);
    if (ClaimedFace* row = FindClaimedFaceLocked(face)) {
        *row = ClaimedFace{};
    }
    pthread_mutex_unlock(&g_claimed_mutex);
}

// Defined with the WebRender search below, which asks the same question of a
// shape.
bool IsSignedPermutation(const FT_Matrix& m);

// The em behind a char size Skia scaled by a shape.
//
// sk_relax rounds every term of SkScalerContextRec::fPost2x2 to a 1024th, so
// getSingleMatrix is fTextSize times a matrix of whole 1024ths.
// computeShapeMatrix takes major as the length of that matrix's first column
// and minor as its determinant over that length, and a length is a square
// root, which no single 1024th reaches. The search over products therefore
// finds nothing for a rotated layer and the app unit inversion below it has
// nothing to invert, so the em comes out a thousandth of a pixel from the one
// Windows rasterizes at, which is a level of coverage on every edge.
//
// The integers are recoverable. Each column reaches FreeType divided by its
// own scale, so its direction names the pair and the received 26.6 size names
// which pair it is, exactly as the WebRender search below does it. The one
// thing neither search can guess is the size itself, and for this route
// CleartypeSkiaScaler carries it here from the scaler.
//
// SkiaScalerRun answers the same question without any of this, out of the rec
// the same hook reads, and stands ahead of it. This is what is left for a
// build whose scaler vtable was never found.

// The scale and the transform SkScalerContext_win_dw hands DirectWrite, for a
// device matrix whose terms are known exactly.
//
// SkScalerContextRec::getSingleMatrix multiplies the size into the relaxed
// 2x2, computeMatrices removes the rotation by a Givens rotation and takes the
// scale as what is left on the diagonal, and the remainder is the matrix
// divided by it. Every step is in float, and the order matters: the same
// quantities reached through the 16.16 shape FreeType was handed are a part in
// 65536 out, which is a level of coverage on an edge a hundred pixels from the
// origin.
struct WindowsShapedRun
{
    float em_size;
    float m11, m12, m21, m22;
};

// The total matrix SkScalerContextRec::getSingleMatrix builds, out of the five
// fields it is made of.
//
// getLocalMatrix is SkFontPriv::MakeTextMatrix, a scale by the size and the
// pre-scale with the pre-skew posted onto it, and getMatrixFrom2x2 is the
// relaxed device matrix. postConcat puts the device matrix on the left, and
// SkMatrix multiplies through sdot, which is a float multiply and add.
void SkiaTotalMatrix(const SkiaScalerRec& rec, float* a00, float* a01, float* a10, float* a11)
{
    const auto size = static_cast<float>(rec.text_size);
    const float l00 = size * static_cast<float>(rec.pre_scale_x);
    const float l01 = static_cast<float>(rec.pre_skew_x) * size;
    const float l11 = size;
    const auto d00 = static_cast<float>(rec.post[0]);
    const auto d01 = static_cast<float>(rec.post[1]);
    const auto d10 = static_cast<float>(rec.post[2]);
    const auto d11 = static_cast<float>(rec.post[3]);
    // Row zero of the local matrix has no y term, so the two products that
    // would use it are dropped rather than written as multiplies by zero.
    *a00 = d00 * l00;
    *a01 = d00 * l01 + d01 * l11;
    *a10 = d10 * l00;
    *a11 = d10 * l01 + d11 * l11;
}

// Whether the relaxed device matrix is the identity, which is every glyph a
// page draws at its own size. Those reach FreeType as the size itself and are
// already exact, so nothing below is asked of them.
bool SkiaDeviceIsIdentity(const SkiaScalerRec& rec)
{
    return rec.post[0] == 1.0 && rec.post[1] == 0.0 && rec.post[2] == 0.0 && rec.post[3] == 1.0 &&
           rec.pre_scale_x == 1.0 && rec.pre_skew_x == 0.0;
}

bool WindowsRunForMatrix(const float a00, const float a10, const float a01, const float a11,
                         WindowsShapedRun* out)
{
    // computeMatrices only removes a rotation from a matrix that has one.
    const bool skewed_or_flipped = a01 != 0.0f || a10 != 0.0f || a00 < 0.0f || a11 < 0.0f;
    float ga_scale_x = a00;
    float ga_scale_y = a11;
    if (skewed_or_flipped) {
        float cos_g = 1.0f;
        float sin_g = 0.0f;
        // SkComputeGivensRotation over the point A maps the horizontal
        // baseline to, which is column one.
        if (a10 == 0.0f) {
            cos_g = std::copysign(1.0f, a00);
            sin_g = 0.0f;
        } else if (a00 == 0.0f) {
            cos_g = 0.0f;
            sin_g = -std::copysign(1.0f, a10);
        } else if (std::fabs(a10) > std::fabs(a00)) {
            const float t = a00 / a10;
            const float u = std::copysign(std::sqrt(1.0f + t * t), a10);
            sin_g = -1.0f / u;
            cos_g = -sin_g * t;
        } else {
            const float t = a10 / a00;
            const float u = std::copysign(std::sqrt(1.0f + t * t), a00);
            cos_g = 1.0f / u;
            sin_g = -cos_g * t;
        }
        ga_scale_x = cos_g * a00 - sin_g * a10;
        ga_scale_y = sin_g * a01 + cos_g * a11;
    }
    // A matrix this flat draws nothing, and computeMatrices answers a scale of
    // one and a pair of zero matrices for it rather than a size.
    constexpr float kNearlyZero = 1.0f / 4096.0f;
    if (!std::isfinite(ga_scale_x) || !std::isfinite(ga_scale_y) ||
        std::fabs(ga_scale_x) <= kNearlyZero || std::fabs(ga_scale_y) <= kNearlyZero) {
        return false;
    }
    // PreMatrixScale::kVertical takes both scales from the diagonal's second
    // term, and sA is the matrix with that taken out. Which of the three ways
    // it is taken out decides the last bit of every term, so all three are
    // here.
    const float scale = std::fabs(ga_scale_y);
    out->em_size = scale;
    if (!skewed_or_flipped && a00 == a11) {
        out->m11 = 1.0f;
        out->m12 = 0.0f;
        out->m21 = 0.0f;
        out->m22 = 1.0f;
    } else if (!skewed_or_flipped) {
        out->m11 = a00 / scale;
        out->m12 = 0.0f;
        out->m21 = 0.0f;
        out->m22 = 1.0f;
    } else {
        // Through the reciprocal SkScalarInvert makes of it, which is not the
        // same last bit as a divide.
        const float inv = 1.0f / scale;
        out->m11 = a00 * inv;
        out->m12 = a10 * inv;
        out->m21 = a01 * inv;
        out->m22 = a11 * inv;
    }
    return true;
}

// The two char sizes and the shape SkScalerContext_CairoFT::Lock would have
// handed FreeType, out of the same matrix. computeShapeMatrix works in double,
// narrows the two scales to SkScalar for the request, and normalizes the
// matrix by their reciprocals for the shape; a matrix that only scales the
// axes gets no shape at all and the face keeps the identity.
bool SkiaFaceStateForMatrix(const float a00, const float a01, const float a10, const float a11,
                            FT_Fixed* x_26_6, FT_Fixed* y_26_6, FT_Matrix* shape)
{
    const double scale_x = static_cast<double>(a00), skew_x = static_cast<double>(a01),
                 skew_y = static_cast<double>(a10), scale_y = static_cast<double>(a11);
    const double det = scale_x * scale_y - skew_y * skew_x;
    if (!std::isfinite(det)) {
        return false;
    }
    double major = det != 0.0 ? std::hypot(scale_x, skew_y) : 0.0;
    double minor = major != 0.0 ? std::fabs(det) / major : 0.0;
    major = std::max(major, 1.0);
    minor = std::max(minor, 1.0);
    *x_26_6 = SkiaCharSize26_6(static_cast<float>(major));
    *y_26_6 = SkiaCharSize26_6(static_cast<float>(minor));

    constexpr FT_Matrix identity = { .xx = 0x10000, .xy = 0, .yx = 0, .yy = 0x10000 };
    *shape = identity;
    const bool have_shape = a01 != 0.0f || a10 != 0.0f || a00 < 0.0f || a11 < 0.0f;
    if (!have_shape) {
        return true;
    }
    // preScale takes the reciprocals as doubles and multiplies the columns by
    // them as floats, and SkScalarToFixed truncates the 16.16 product.
    const auto inv_major = static_cast<float>(1.0 / major);
    const auto inv_minor = static_cast<float>(1.0 / minor);
    const auto fixed = [](const float v) {
        return static_cast<FT_Fixed>(v * 65536.0f);
    };
    shape->xx = fixed(a00 * inv_major);
    shape->yx = fixed(-(a10 * inv_major));
    shape->xy = fixed(-(a01 * inv_minor));
    shape->yy = fixed(a11 * inv_minor);
    return true;
}

// The em and the transform Windows rasterizes this glyph with, out of the
// scaler's own fields rather than out of what survived the trip through
// FreeType.
//
// The char sizes and the shape the matrix implies both have to be the ones the
// face was actually set to. Two layers of one color glyph can share a char
// size and differ only in their rotation, so the sizes alone do not say
// whether this rec belongs to the glyph being drawn or to a scaler that ran
// earlier on this thread. The shape settles it.
bool SkiaScalerRun(FT_Face face, double* em_size, float* x_over_y, WindowsShapedRun* run)
{
    if (!g_skia_rec.valid || face->size == nullptr || face->units_per_EM == 0) {
        return false;
    }
    // A glyph drawn at the page's own size reaches FreeType as that size and
    // needs none of this.
    if (SkiaDeviceIsIdentity(g_skia_rec)) {
        return false;
    }
    float a00 = 0.0f, a01 = 0.0f, a10 = 0.0f, a11 = 0.0f;
    SkiaTotalMatrix(g_skia_rec, &a00, &a01, &a10, &a11);
    FT_Fixed want_x = 0, want_y = 0;
    FT_Matrix want_shape = {};
    if (!SkiaFaceStateForMatrix(a00, a01, a10, a11, &want_x, &want_y, &want_shape)) {
        return false;
    }
    if (want_x != MulFix(face->units_per_EM, face->size->metrics.x_scale) ||
        want_y != MulFix(face->units_per_EM, face->size->metrics.y_scale)) {
        return false;
    }
    FT_Matrix have_shape = {};
    pthread_mutex_lock(&g_faces_mutex);
    if (const FaceEntry* entry = FindFaceLocked(face)) {
        constexpr FT_Matrix identity = { .xx = 0x10000, .xy = 0, .yx = 0, .yy = 0x10000 };
        have_shape = entry->transform_identity ? identity : entry->transform_matrix;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    // One unit of slack either way, because a mirror of a float multiply and a
    // truncation is not obliged to land on the same last bit.
    const auto near_enough = [](const FT_Fixed a, const FT_Fixed b) {
        return a - b <= 1 && b - a <= 1;
    };
    if (!near_enough(want_shape.xx, have_shape.xx) || !near_enough(want_shape.xy, have_shape.xy) ||
        !near_enough(want_shape.yx, have_shape.yx) || !near_enough(want_shape.yy, have_shape.yy)) {
        return false;
    }
    if (!WindowsRunForMatrix(a00, a10, a01, a11, run)) {
        return false;
    }
    // The transform carries both scales, so the ratio the aspect search would
    // estimate is already in it.
    *em_size = static_cast<double>(run->em_size);
    *x_over_y = 1.0f;
    return true;
}

bool SkiaShapedEmSize(const double size, const FT_Matrix& m, const FT_Fixed x_26_6,
                      const FT_Fixed y_26_6, double* em_size, float* x_over_y,
                      WindowsShapedRun* run)
{
    if (!(size > 0.0) || x_26_6 <= 0 || y_26_6 <= 0) {
        return false;
    }
    // A shape that only permutes and flips the axes is already exact in
    // 1024ths, and its scales are a pair of ones, so there is nothing to
    // recover and an estimate here would name a transform a thousandth off.
    if (IsSignedPermutation(m)) {
        return false;
    }
    // The two columns in SkMatrix's terms. computeShapeMatrix writes the FT
    // matrix with the y axis flipped, which is the same convention
    // unix/font.rs uses.
    const double a = static_cast<double>(m.xx) / 65536.0;
    const double b = -static_cast<double>(m.yx) / 65536.0;
    const double c = -static_cast<double>(m.xy) / 65536.0;
    const double d = static_cast<double>(m.yy) / 65536.0;

    const double x_px = static_cast<double>(x_26_6) / 64.0;
    const double y_px = static_cast<double>(y_26_6) / 64.0;
    const double x_est = x_px / size;
    const double y_est = y_px / size;
    const long seed_k1 = lround(1024.0 * a * x_est);
    const long seed_k2 = lround(1024.0 * b * x_est);

    // The seed before its neighbors, since a neighbor can satisfy every check
    // while naming a different transform.
    static constexpr long kNudge[3] = { 0, -1, 1 };
    for (const long d1 : kNudge) {
        for (const long d2 : kNudge) {
            const long k1 = seed_k1 + d1, k2 = seed_k2 + d2;
            if (k1 == 0 && k2 == 0) {
                continue;
            }
            const double h = std::hypot(static_cast<double>(k1), static_cast<double>(k2));
            const double x_scale = h / 1024.0;
            if (SkiaCharSize26_6(static_cast<float>(size * x_scale)) != x_26_6) {
                continue;
            }
            const long seed_k3 = lround(1024.0 * y_est * c);
            const long seed_k4 = lround(1024.0 * y_est * d);
            for (const long d3 : kNudge) {
                for (const long d4 : kNudge) {
                    const long k3 = seed_k3 + d3, k4 = seed_k4 + d4;
                    const long det = k1 * k4 - k2 * k3;
                    if (det == 0) {
                        continue;
                    }
                    const double y_scale = std::fabs(static_cast<double>(det)) / (1024.0 * h);
                    if (SkiaCharSize26_6(static_cast<float>(size * y_scale)) != y_26_6) {
                        continue;
                    }
                    // Both columns have to read back as the shape FreeType was
                    // handed, or these were the wrong integers.
                    const double back_a = static_cast<double>(k1) / (1024.0 * x_scale);
                    const double back_b = static_cast<double>(k2) / (1024.0 * x_scale);
                    const double back_c = static_cast<double>(k3) / (1024.0 * y_scale);
                    const double back_d = static_cast<double>(k4) / (1024.0 * y_scale);
                    constexpr double kBack = 3.0 / 65536.0;
                    if (std::fabs(back_a - a) > kBack || std::fabs(back_b - b) > kBack ||
                        std::fabs(back_c - c) > kBack || std::fabs(back_d - d) > kBack) {
                        continue;
                    }
                    // The scale and the remainder as Windows reaches them, out
                    // of the same four integers.
                    const auto term = [size](const long k) {
                        return static_cast<float>(size) * (static_cast<float>(k) / 1024.0f);
                    };
                    if (!WindowsRunForMatrix(term(k1), term(k2), term(k3), term(k4), run)) {
                        continue;
                    }
                    *em_size = static_cast<double>(run->em_size);
                    *x_over_y = 1.0f;
                    return true;
                }
            }
        }
    }
    return false;
}

// The em size Windows rasterizes a transformed glyph at.
//
// platform/windows/font.rs get_glyph_parameters passes
// `font.size.to_f64_px() * y_scale`, with y_scale from
// FontTransform::compute_scale, so a rotated glyph is drawn at the requested
// size times a factor that is rarely 1. The Linux backend spends that same
// product on FT_Set_Char_Size, which is 26.6, and ExactEmSize then reads it
// back as though the factor were 1 and answers with the nearest CSS size,
// which is up to a 128th of a pixel out.
//
// Every term of the product is constrained. FontTransform::quantize rounds
// the transform to 1024ths before the glyph cache, so its first column is a
// pair of integers over 1024. compute_scale makes x_scale that column's
// length, and the shape reaches FreeType divided by that length, so the
// column arrives as a unit vector and the pair follows from an estimate good
// to a part in a thousand. The requested size is a whole app unit over 60.
// y_scale is the determinant over x_scale, another integer over 1024 times
// that same length.
//
// Every step is checked against the 26.6 sizes FreeType was asked for, so a
// transform that did not come from WebRender fails one and keeps the size
// FreeType reported.

// The transform WebRender handed down, as the search recovered it: its four
// terms over 1024, the size it was asked for, and the two scales
// compute_scale split out of it.
struct RecoveredTransform
{
    long k1, k2, k3, k4;
    double req;
    double x_scale;
    double y_scale;
    double em_size;
    // synthesize_italics' skew, and which column it moved. Zero and false for
    // an upright glyph.
    float skew;
    bool vertical;
};

// One pass of the search, over a shape whose column two is already free of
// any synthetic oblique.
bool SearchTransformedEmSize(const double a, const double b, const double c, const double d,
                             const FT_Fixed x_26_6, const FT_Fixed y_26_6, const double seed_px,
                             RecoveredTransform* out)
{
    const double x_px = static_cast<double>(x_26_6) / 64.0;
    const double y_px = static_cast<double>(y_26_6) / 64.0;
    const double x_est = x_px / seed_px;
    const long seed_k1 = lround(1024.0 * a * x_est);
    const long seed_k2 = lround(1024.0 * b * x_est);

    // The seed is tried before its neighbors. A neighbor can satisfy every
    // check while naming a different transform, so the estimate wins ties.
    static constexpr long kNudge[3] = { 0, -1, 1 };
    for (const long d1 : kNudge) {
        for (const long d2 : kNudge) {
            const long k1 = seed_k1 + d1, k2 = seed_k2 + d2;
            if (k1 == 0 && k2 == 0) {
                continue;
            }
            const double h = std::hypot(static_cast<double>(k1), static_cast<double>(k2));
            const double x_scale = h / 1024.0;
            const double req_est = x_px / x_scale;
            for (const long dn : kNudge) {
                const long app_units = lround(req_est * 60.0) + dn;
                if (app_units <= 0) {
                    continue;
                }
                // font.size is an f32 and to_f64_px only widens it.
                const double req =
                    static_cast<double>(static_cast<float>(static_cast<double>(app_units) / 60.0));
                if (CharSize26_6(req * x_scale) != x_26_6) {
                    continue;
                }
                // The vertical size is known only to a 128th of a pixel,
                // which spans far too many determinants to search. Column two
                // is put on the 1024th grid instead and the determinant
                // follows from it.
                const double y_est = y_px / req;
                const long seed_k3 = lround(1024.0 * y_est * c);
                const long seed_k4 = lround(1024.0 * y_est * d);
                for (const long d3 : kNudge) {
                    for (const long d4 : kNudge) {
                        const long k3 = seed_k3 + d3, k4 = seed_k4 + d4;
                        const long det = k1 * k4 - k2 * k3;
                        if (det == 0) {
                            continue;
                        }
                        const double y_scale =
                            std::fabs(static_cast<double>(det)) / (1024.0 * h);
                        if (CharSize26_6(req * y_scale) != y_26_6) {
                            continue;
                        }
                        // Both columns have to read back as the shape FreeType
                        // was handed, or these were the wrong integers.
                        const double back_a = static_cast<double>(k1) / (1024.0 * x_scale);
                        const double back_b = static_cast<double>(k2) / (1024.0 * x_scale);
                        const double back_c = static_cast<double>(k3) / (1024.0 * y_scale);
                        const double back_d = static_cast<double>(k4) / (1024.0 * y_scale);
                        constexpr double kBack = 3.0 / 65536.0;
                        if (std::fabs(back_a - a) > kBack || std::fabs(back_b - b) > kBack ||
                            std::fabs(back_c - c) > kBack || std::fabs(back_d - d) > kBack) {
                            continue;
                        }
                        out->k1 = k1;
                        out->k2 = k2;
                        out->k3 = k3;
                        out->k4 = k4;
                        out->req = req;
                        out->x_scale = x_scale;
                        out->y_scale = y_scale;
                        out->em_size = static_cast<double>(static_cast<float>(req * y_scale));
                        out->skew = 0.0f;
                        out->vertical = false;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// Rounds `guess` to the nearest skew SyntheticItalics can hold, takes that
// much of one column back out of the other, and searches the shape that
// leaves. `vertical` picks which column carries the skew, matching the two
// branches of FontTransform::synthesize_italics.
bool RetryWithoutSkew(const double guess, const double a, const double b, const double c,
                      const double d, const bool vertical, const FT_Fixed x_26_6,
                      const FT_Fixed y_26_6, const double seed_px, RecoveredTransform* out)
{
    const double angle = std::atan(guess) * (180.0 / M_PI) * 256.0;
    if (!(std::fabs(angle) < 89.0 * 256.0)) {
        return false;
    }
    const long nearest = static_cast<long>(std::floor(angle + 0.5));
    for (long candidate = nearest - 1; candidate <= nearest + 1; ++candidate) {
        if (candidate == 0 || candidate < -89L * 256 || candidate > 89L * 256) {
            continue;
        }
        const float exact = SkewForAngle(static_cast<int>(candidate));
        const double skew = static_cast<double>(exact);
        const bool found =
            vertical ? SearchTransformedEmSize(a - skew * c, b - skew * d, c, d, x_26_6, y_26_6,
                                               seed_px, out)
                     : SearchTransformedEmSize(a, b, c + skew * a, d + skew * b, x_26_6, y_26_6,
                                               seed_px, out);
        if (found) {
            out->skew = exact;
            out->vertical = vertical;
            return true;
        }
    }
    return false;
}

// True when the shape only permutes the glyph's axes and flips their signs,
// which leaves every term exactly 0 or plus or minus one.
bool IsSignedPermutation(const FT_Matrix& m)
{
    constexpr FT_Fixed kOne = 0x10000;
    const bool diagonal = (m.xx == kOne || m.xx == -kOne) && (m.yy == kOne || m.yy == -kOne) &&
                          m.xy == 0 && m.yx == 0;
    const bool swapped = (m.xy == kOne || m.xy == -kOne) && (m.yx == kOne || m.yx == -kOne) &&
                         m.xx == 0 && m.yy == 0;
    return diagonal || swapped;
}

bool ExactTransformedEmSize(const FT_Matrix& m, const FT_Fixed x_26_6, const FT_Fixed y_26_6,
                            const double seed_px, RecoveredTransform* out)
{
    if (x_26_6 <= 0 || y_26_6 <= 0 || !(seed_px > 0.0)) {
        return false;
    }
    // Such a shape is already exact in 1024ths, so compute_scale answers a
    // pair of ones and the em is the size FreeType was asked for. The search
    // below seeds column one from the 26.6 size over the reported one, and at
    // a size that is not a whole number of quarter pixels that ratio rounds a
    // 1024th away from unity, which names a transform a thousandth off and an
    // em with it.
    if (IsSignedPermutation(m)) {
        return false;
    }
    // A synthetic oblique on its own leaves font.transform the identity, so
    // compute_scale answers a pair of ones and the em is the size FreeType was
    // asked for. The skew takes column two off the 1024th grid, and the search
    // below then moves column one and names a size a fraction of a pixel from
    // the one WebRender sent.
    if (float oblique = 0.0f; ExactObliqueSkew(m, &oblique) == ObliqueForm::Upright) {
        return false;
    }
    // Column one of the shape, in FontTransform's terms. unix/font.rs negates
    // skew_y on its way into FT_Matrix, and the synthetic oblique only ever
    // adds a multiple of this column to the other one, so it arrives untouched
    // whether or not the glyph is slanted.
    const double a = static_cast<double>(m.xx) / 65536.0;
    const double b = -static_cast<double>(m.yx) / 65536.0;
    // Column two, which does carry the oblique.
    const double c = -static_cast<double>(m.xy) / 65536.0;
    const double d = static_cast<double>(m.yy) / 65536.0;

    // compute_scale makes x_scale the length of column one, and the shape is
    // the transform divided by it, so that column arrives as a unit vector.
    // Any other shape, such as Skia's own or one swap_xy has permuted, is left
    // alone. A synthetic oblique moves one column by a multiple of the other,
    // which leaves the determinant and therefore y_scale alone but takes the
    // column it moved off the 1024th grid, so it has to come back out before
    // the search can find any integers. SyntheticItalics only ever holds the
    // tangent of a 256th of a degree, so rounding to the nearest such angle
    // gives back the f32 Windows was handed.
    //
    // Which column moved says how to estimate the skew, and both estimates
    // are the same projection. The transform WebRender started from is a
    // rotation and a scale, so its two columns are perpendicular and whatever
    // one column holds of the other's direction is the skew and nothing else.
    constexpr double kUnit = 8.0 / 65536.0;
    const double along = a * c + b * d;
    if (std::fabs(std::hypot(a, b) - 1.0) <= kUnit) {
        if (SearchTransformedEmSize(a, b, c, d, x_26_6, y_26_6, seed_px, out)) {
            return true;
        }
        // Horizontal text: the skew multiplies column one into column two.
        if (RetryWithoutSkew(-along, a, b, c, d, false, x_26_6, y_26_6, seed_px, out)) {
            return true;
        }
    }
    // Vertical text: the skew multiplies column two into column one instead,
    // so column one is the one that no longer has unit length. Taking the skew
    // back out has to restore that length, which is a quadratic. Its vertex is
    // the projection, and where the two columns are perpendicular that is the
    // answer and the two roots are a square root's worth of noise either side
    // of it; where they are not, one of the roots is the answer and the vertex
    // is not. All three are cheap to try.
    const double len2 = c * c + d * d;
    if (!(len2 > 0.0)) {
        return false;
    }
    const double vertex = along / len2;
    const double disc = vertex * vertex - (a * a + b * b - 1.0) / len2;
    const double root = disc > 0.0 ? std::sqrt(disc) : 0.0;
    for (const double guess : { vertex, vertex - root, vertex + root }) {
        if (RetryWithoutSkew(guess, a, b, c, d, true, x_26_6, y_26_6, seed_px, out)) {
            return true;
        }
    }
    return false;
}

// synthesize_italics' origin shift, which only vertical text has. The skew is
// applied about the middle of the glyph rather than its edge, and the shift
// that takes reaches Windows as the DWRITE_MATRIX's dx and dy while
// unix/font.rs spends it on FT_Set_Transform's 26.6 delta.
void SyntheticItalicOffset(const RecoveredTransform& r, double* dx, double* dy)
{
    const auto inv = static_cast<float>(1.0 / r.y_scale);
    const float skew_x = static_cast<float>(r.k3) / 1024.0f * inv;
    const float scale_y = static_cast<float>(r.k4) / 1024.0f * inv;
    const double ty = -(r.req * r.y_scale) * 0.5 * static_cast<double>(r.skew);
    *dx = static_cast<double>(skew_x) * ty;
    *dy = static_cast<double>(scale_y) * ty;
}

// The shape platform/windows/font.rs hands DirectWrite, from the four
// quantized terms behind the one FreeType was handed.
//
// get_glyph_parameters divides both columns of the transform by y_scale, where
// platform/unix/font.rs divides column one by x_scale instead, and it keeps
// the result in float where FreeType takes 16.16. A term reached through that
// 16.16 is a part in 65536 out, which is a level of coverage on the edge of a
// glyph the transform has carried away from the origin. Both differences go
// away here: the terms are rebuilt from the integers and divided the way
// Windows divides them, and the synthetic oblique is put back onto that.
void WindowsShapeFromRecovered(const RecoveredTransform& r, WindowsShapedRun* run)
{
    const auto inv = static_cast<float>(1.0 / r.y_scale);
    float scale_x = static_cast<float>(r.k1) / 1024.0f * inv;
    float skew_y = static_cast<float>(r.k2) / 1024.0f * inv;
    float skew_x = static_cast<float>(r.k3) / 1024.0f * inv;
    float scale_y = static_cast<float>(r.k4) / 1024.0f * inv;
    // FontTransform::synthesize_italics, whose two forms move a different
    // column by a multiple of the other.
    if (r.skew != 0.0f) {
        if (r.vertical) {
            scale_x += skew_x * r.skew;
            skew_y += scale_y * r.skew;
        } else {
            skew_x -= scale_x * r.skew;
            scale_y -= skew_y * r.skew;
        }
    }
    run->em_size = static_cast<float>(r.em_size);
    run->m11 = scale_x;
    run->m12 = skew_y;
    run->m21 = skew_x;
    run->m22 = scale_y;
}
#endif  // CLEARTYPE_FIREFOX_PARITY

// The size FreeType is actually scaling outlines by, snapping and all, in 26.6
// fixed point.
//
// Not x_ppem/y_ppem: those are FreeType's rounded display values, and Qt and
// fontconfig routinely ask for fractional sizes (HiDPI, point sizes at 96dpi).
// An 11pt-at-96dpi request rasterized at a rounded 15px would silently
// mis-size every glyph against the advances the text was laid out with.
//
// FT_MulFix(units_per_EM, x_scale) yields 26.6, not 16.16, despite x_scale
// being 16.16: it is calibrated to take a design-unit coordinate straight to
// a 26.6 pixel one. (Using <<16 here disables this on every input.)
#if CLEARTYPE_FIREFOX_PARITY
// Put the pixel aspect back on the grid WebRender rounded it to.
//
// FontTransform::quantize in wr_glyph_rasterizer/src/rasterizer.rs rounds
// every term of the glyph transform to a 1024th before the glyph cache is
// touched, and Windows hands that term to DirectWrite as it stands. The Linux
// backend spends it on FT_Set_Char_Size instead, which is 26.6, so the ratio
// of the two axes comes back a rounding of the value Windows kept, exact only
// where the vertical size is a power of two.
//
// The 1024th is taken only when it reproduces the horizontal size that was
// actually asked for, which is what makes a ratio that never came from
// WebRender keep the value FreeType reported.
double SnapPixelAspect(const double raw, const FT_Fixed x_26_6, const FT_Fixed y_26_6)
{
    constexpr double quantize_scale = 1024.0;
    const double snapped = std::floor(raw * quantize_scale + 0.5) / quantize_scale;
    if (!(snapped > 0.0)) {
        return raw;
    }
    const FT_Fixed reproduced =
        static_cast<FT_Fixed>(std::floor(static_cast<double>(y_26_6) * snapped + 0.5));
    return reproduced == x_26_6 ? snapped : raw;
}
#endif  // CLEARTYPE_FIREFOX_PARITY

bool GetScaledEmSize(FT_Face face, double* em_size, float* x_over_y)
{
    if (face->size == nullptr || face->units_per_EM == 0) {
        return false;
    }
    const FT_Fixed x_26_6 = MulFix(face->units_per_EM, face->size->metrics.x_scale);
    const FT_Fixed y_26_6 = MulFix(face->units_per_EM, face->size->metrics.y_scale);
    if (x_26_6 <= 0 || y_26_6 <= 0) {
        return false;
    }
    *em_size = static_cast<double>(y_26_6) / 64.0;
    *x_over_y = static_cast<float>(x_26_6) / static_cast<float>(y_26_6);
    return true;
}

#if CLEARTYPE_FIREFOX_PARITY
// The pixel aspect of `face` on WebRender's 1024th grid, or the ratio
// FreeType reports when it is not on it. See SnapPixelAspect.
float SnappedPixelAspect(FT_Face face)
{
    if (face->size == nullptr || face->units_per_EM == 0) {
        return 1.0f;
    }
    const FT_Fixed x_26_6 = MulFix(face->units_per_EM, face->size->metrics.x_scale);
    const FT_Fixed y_26_6 = MulFix(face->units_per_EM, face->size->metrics.y_scale);
    if (x_26_6 <= 0 || y_26_6 <= 0) {
        return 1.0f;
    }
    const double raw = static_cast<double>(x_26_6) / static_cast<double>(y_26_6);
    return static_cast<float>(SnapPixelAspect(raw, x_26_6, y_26_6));
}

// Replaces `em_size` with the one Windows rasterizes at when `face` carries a
// shape, and fills `run` with the shape Windows passes with it. True when the
// four integers behind that shape were recovered. See ExactTransformedEmSize.
// Takes g_faces_mutex, so nothing already holding it may call this.
// The same for the Skia route, where the size comes from the scaler rather
// than from a whole app unit. Both scales are recovered, so the ratio the
// aspect search would otherwise estimate from the two rounded char sizes is
// exact as well.
bool ApplySkiaShapedEmSize(FT_Face face, double* em_size, float* x_over_y,
                           WindowsShapedRun* run)
{
    if (face->size == nullptr || face->units_per_EM == 0 || !(g_skia_text_size > 0.0)) {
        return false;
    }
    FT_Matrix shape = {};
    bool transformed = false;
    pthread_mutex_lock(&g_faces_mutex);
    if (const FaceEntry* entry = FindFaceLocked(face)) {
        transformed = !entry->transform_identity;
        shape = entry->transform_matrix;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    if (!transformed) {
        return false;
    }
    const FT_Fixed x_26_6 = MulFix(face->units_per_EM, face->size->metrics.x_scale);
    const FT_Fixed y_26_6 = MulFix(face->units_per_EM, face->size->metrics.y_scale);
    return SkiaShapedEmSize(g_skia_text_size, shape, x_26_6, y_26_6, em_size, x_over_y, run);
}

bool ApplyTransformedEmSize(FT_Face face, double* em_size, WindowsShapedRun* run)
{
    if (face->size == nullptr || face->units_per_EM == 0) {
        return false;
    }
    FT_Matrix shape = {};
    bool transformed = false;
    pthread_mutex_lock(&g_faces_mutex);
    if (const FaceEntry* entry = FindFaceLocked(face)) {
        transformed = !entry->transform_identity;
        shape = entry->transform_matrix;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    if (!transformed) {
        return false;
    }
    const FT_Fixed x_26_6 = MulFix(face->units_per_EM, face->size->metrics.x_scale);
    const FT_Fixed y_26_6 = MulFix(face->units_per_EM, face->size->metrics.y_scale);
    RecoveredTransform recovered = {};
    if (!ExactTransformedEmSize(shape, x_26_6, y_26_6, *em_size, &recovered)) {
        return false;
    }
    *em_size = recovered.em_size;
    WindowsShapeFromRecovered(recovered, run);
    return true;
}

// FT_Set_Transform's delta, put back onto the glyph just loaded.
//
// FreeType applies it by translating the slot outline from inside
// FT_Load_Glyph, and that translate is no longer recorded, so the value comes
// from the face instead. It is the origin shift synthetic italics asks for in
// vertical text, which Windows passes as the DWRITE_MATRIX's dx and dy.
void RecordTransformDelta(FT_Face face)
{
    if (face == nullptr || face->glyph == nullptr) {
        return;
    }
    FT_Vector delta = { .x = 0, .y = 0 };
    FT_Matrix shape = {};
    bool transformed = false;
    pthread_mutex_lock(&g_faces_mutex);
    if (const FaceEntry* entry = FindFaceLocked(face)) {
        delta = entry->transform_delta;
        shape = entry->transform_matrix;
        transformed = !entry->transform_identity;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    if (delta.x == 0 && delta.y == 0) {
        return;
    }
    // A delta only ever carries the vertical synthetic oblique's origin
    // shift, which Windows keeps to full precision. Where the transform it
    // came from can be recovered the shift is recomputed from that; where it
    // cannot, the 26.6 value FreeType was given is all there is.
    if (transformed && face->size != nullptr && face->units_per_EM != 0) {
        const FT_Fixed x_26_6 = MulFix(face->units_per_EM, face->size->metrics.x_scale);
        const FT_Fixed y_26_6 = MulFix(face->units_per_EM, face->size->metrics.y_scale);
        RecoveredTransform recovered = {};
        if (x_26_6 > 0 && y_26_6 > 0 &&
            ExactTransformedEmSize(shape, x_26_6, y_26_6, static_cast<double>(y_26_6) / 64.0,
                                   &recovered) &&
            recovered.vertical) {
            double dx = 0.0, dy = 0.0;
            SyntheticItalicOffset(recovered, &dx, &dy);
            RecordExactDelta(&face->glyph->outline, dx, dy);
            return;
        }
    }
    RecordShift(&face->glyph->outline, delta.x, delta.y);
}
#endif  // CLEARTYPE_FIREFOX_PARITY

#if CLEARTYPE_FIREFOX_PARITY
// Defined with the rasterizer the caller belongs to, below.
bool FaceRastersThroughSkia(FT_Face face);
#endif

// The size the caller asked for: Firefox's mAdjustedSize, which is what both
// gfxDWriteFont and platform/windows/font.rs compute from.
//
// `requested` is the face's recorded request. A caller already holding
// g_faces_mutex passes the entry's own copy, because RequestedEmSize takes
// that lock and it is not recursive.
bool GetEmSizeWithRequest(FT_Face face, const FT_Fixed requested, double* em_size, float* x_over_y)
{
    if (!GetScaledEmSize(face, em_size, x_over_y)) {
        return false;
    }
#if CLEARTYPE_FIREFOX_PARITY
    // The recorded request is only trusted when its whole-pixel rounding is
    // the ppem FreeType settled on, which ties it to the size object that is
    // currently installed on the face.
    FT_Fixed size_26_6 = static_cast<FT_Fixed>(std::floor(*em_size * 64.0 + 0.5));
    if (requested > 0 && face->size != nullptr &&
        (requested + 32) >> 6 == static_cast<FT_Fixed>(face->size->metrics.y_ppem)) {
        size_26_6 = requested;
        *em_size = static_cast<double>(requested) / 64.0;
    }
    // A size Gecko named for this face outranks the reconstruction, being the
    // value itself rather than an inversion of its rounding.
    double exact = 0.0;
    // NOLINTNEXTLINE(bugprone-branch-clone)  -- each condition names a different em
    if (ClaimedSizeFor(face, size_26_6, &exact)) {
        *em_size = exact;
    } else if (FaceRastersThroughSkia(face) && IsSkiaPathSize(size_26_6, *x_over_y)) {
        // A product of a named size can quantize to this one, and taking it
        // would answer a path request with a size a 256th out. The size is
        // its own answer.
        *em_size = kSkiaPathSize;
    } else if (FaceRastersThroughSkia(face) && ScaledEmSize(face, size_26_6, &exact)) {
        *em_size = exact;
    } else if (ExactEmSize(size_26_6, &exact) || ReducedEmSize(size_26_6, &exact)) {
        *em_size = exact;
    }
#else
    (void)requested;
#endif
    return true;
}

// The em size an InitMetrics accessor answers at. A claimed size is taken as
// given, since it comes from the font whose metrics are about to be written.
// GetEmSizeWithRequest trusts a recorded request only when it rounds to the
// ppem currently installed, which cannot separate two sizes a fraction apart
// on one face.
#if CLEARTYPE_FIREFOX_PARITY
bool GetEmSizeClaimed(FT_Face face, const FT_Fixed requested, double* em_size,
                      float* x_over_y)
{
    if (g_claimed_em_26_6 > 0) {
        if (!GetScaledEmSize(face, em_size, x_over_y)) {
            return false;
        }
        *em_size = static_cast<double>(g_claimed_em_26_6) / 64.0;
        double exact = 0.0;
        if (g_claimed_em_px > 0.0) {
            *em_size = g_claimed_em_px;
        } else if (ExactEmSize(g_claimed_em_26_6, &exact) ||
                   ReducedEmSize(g_claimed_em_26_6, &exact)) {
            *em_size = exact;
        }
        return true;
    }
    return GetEmSizeWithRequest(face, requested, em_size, x_over_y);
}
#endif

bool GetEmSize(FT_Face face, double* em_size, float* x_over_y)
{
#if CLEARTYPE_FIREFOX_PARITY
    return GetEmSizeWithRequest(face, RequestedEmSize(face), em_size, x_over_y);
#else
    return GetEmSizeWithRequest(face, 0, em_size, x_over_y);
#endif
}

#if CLEARTYPE_FIREFOX_PARITY

// ---------------------------------------------------------------------------
// The Windows font instance.
//
// gfx/thebes/gfxDWriteFonts.cpp gfxDWriteFont::ComputeMetrics, evaluated here
// for the (face, size) FreeType was just asked for, with the same DirectWrite
// calls it makes, so that the FreeType-visible values written back below make
// gfx/thebes/gfxFT2FontBase.cpp gfxFT2FontBase::InitMetrics arrive at the same
// gfxFont::Metrics.
// ---------------------------------------------------------------------------

IDWriteFontFace1* QueryFontFace1(IDWriteFontFace* face)
{
    void* out = nullptr;
    const GUID iid = DWRITE_UUIDOF(IDWriteFontFace1);
    if (FAILED(face->QueryInterface(iid, &out))) {
        return nullptr;
    }
    return static_cast<IDWriteFontFace1*>(out);
}

// gfxDWriteFont::MeasureGlyphWidth. The face is a parameter because one
// instance answers through two of them, its own and the one carrying
// DirectWrite's bold simulation.
double WinMeasureGlyphWidth(const WinInstance& inst, IDWriteFontFace* face, const UINT16 glyph)
{
    IDWriteFontFace1* face1 = QueryFontFace1(face);
    double result = 0.0;
    if (face1 != nullptr) {
        INT32 advance = 0;
        if (inst.use_subpixel_positions) {
            if (SUCCEEDED(face1->GetDesignGlyphAdvances(1, &glyph, &advance, FALSE))) {
                result = static_cast<double>(static_cast<float>(advance) * inst.funits_conv);
            }
        } else {
            // GetMeasuringMode() == DWRITE_MEASURING_MODE_GDI_NATURAL is the
            // useGdiNatural argument; the measuring mode here is NATURAL.
            if (SUCCEEDED(face1->GetGdiCompatibleGlyphAdvances(static_cast<FLOAT>(inst.adjusted_size), 1.0f, nullptr, FALSE, FALSE, 1, &glyph, &advance))) {
                // funits_conv is a float, so the product is one too and NSlround
                // takes a double: the widening happens either way, and clang's
                // -Wdouble-promotion asks for it to be written down.
                // ReSharper disable once CppRedundantCastExpression
                result = static_cast<double>(NSlround(
                    static_cast<double>(static_cast<float>(advance) * inst.funits_conv)));
            }
        }
        face1->Release();
    }
    return result;
}

// gfxFont::GetGlyphAdvance for a horizontal advance: ProvidesGlyphWidths() goes
// through gfxDWriteFont::GetGlyphWidth (NS_lround(MeasureGlyphWidth * 65536.0)),
// otherwise through gfxHarfBuzzShaper::GetGlyphHAdvanceUncached
// (FloatToFixed(FUnitsToDevUnitsFactor() * advanceWidth), float, truncated).
double WinGlyphAdvance(const WinInstance& inst, const UINT16 glyph, const bool has_variations)
{
    // gfxDWriteFont::ProvidesGlyphWidths, with the bold-simulation term taken
    // as false. A load carries no sign of which font instance made it, so the
    // emboldened one is recognized later and corrected by ApplyWindowsBoldAdvance.
    if (!inst.use_subpixel_positions || has_variations) {
        return static_cast<double>(NSlround(
                   WinMeasureGlyphWidth(inst, inst.dwrite_face, glyph) * 65536.0)) / 65536.0;
    }
    IDWriteFontFace1* face1 = QueryFontFace1(inst.dwrite_face);
    double result = 0.0;
    if (face1 != nullptr) {
        INT32 advance = 0;
        if (SUCCEEDED(face1->GetDesignGlyphAdvances(1, &glyph, &advance, FALSE))) {
            const float fixed = 65536 * (inst.funits_conv * static_cast<float>(advance));
            result = static_cast<double>(static_cast<int32_t>(fixed)) / 65536.0;
        }
        face1->Release();
    }
    return result;
}

// Set from a gfxShapedText by the libxul patch. Every advance Gecko stores is
// a whole number of app units, and both the tracking below and the em size
// recovered further up have to land on the same grid Gecko laid the text out
// on.
extern "C" void CleartypeSetAppUnitsPerDevPixel(const int units)
{
    if (units > 0 && units <= 240) {
        g_app_units_per_dev_px.store(units, std::memory_order_relaxed);
    }
}

// gfxFont::GetSyntheticBoldOffset: for size S below a threshold T of 48, the
// glyphs fatten by 0.25 + 3S/4T, and by S/T above it.
double WinSyntheticBoldOffset(const double size)
{
    constexpr double threshold = 48.0;
    return size < threshold ? 0.25 + 0.75 * size / threshold : size / threshold;
}

// The advance for a bold face Firefox on Windows synthesizes itself, with no
// DirectWrite simulation behind it.
//
// gfxDWriteFontEntry::CreateFontInstance keeps DirectWrite's simulation away
// from webfonts and from COLR fonts, and gfxDWriteFont sets mApplySyntheticBold
// for exactly those two, so the face measures as its plain self and
// gfxFont::PostShapingFixup widens it afterwards. That widening is tracking:
// gfxShapedText::ApplyTrackingToClusters adds a whole number of app units,
// NS_round(GetSyntheticBoldOffset() * appUnitsPerDevUnit), to the last glyph of
// each cluster, and only when the metrics say the face is not fixed-pitch.
//
// Two things about it cannot be reached from one glyph's advance. It is a whole
// number of app units, which is 1/60 px for a device pixel that is a CSS pixel
// and something else otherwise, and it lands once per cluster where this lands
// once per glyph. A cluster of several glyphs therefore comes out wider than
// Windows draws it, as it already does with the strength Linux adds.
double WinSyntheticBoldAdvance(const WinInstance& inst, const double plain_advance)
{
    if (!(inst.metrics.maxAdvance > inst.metrics.aveCharWidth)) {
        return plain_advance;
    }
    const int64_t app_units_per_px = g_app_units_per_dev_px.load(std::memory_order_relaxed);
    // Windows rounds twice. SetGlyphsFromRun turns the plain 16.16 advance
    // into whole app units, and ApplyTrackingToClusters then adds NS_round of
    // the offset in app units. The offset goes in here as its nearest 16.16
    // fraction, which keeps the advance kerning sums against intact, then is
    // nudged by single 16.16 steps until the app-unit conversion lands on the
    // sum Windows reaches. A plain advance on an exact half app unit rounds
    // down without the nudge, where Windows rounds it up before the tracking.
    const int64_t base = llround(plain_advance * 65536.0);
    const int64_t base_app_units = (base * app_units_per_px + 0x8000) >> 16;
    if (base_app_units <= 0) {
        return plain_advance;
    }
    const int64_t tracking = NSlround(
        WinSyntheticBoldOffset(inst.adjusted_size) * static_cast<double>(app_units_per_px));
    const int64_t want = base_app_units + tracking;
    int64_t emitted = base + llround(
        static_cast<double>(tracking) * 65536.0 / static_cast<double>(app_units_per_px));
    while (((emitted * app_units_per_px + 0x8000) >> 16) < want) {
        ++emitted;
    }
    while (((emitted * app_units_per_px + 0x8000) >> 16) > want) {
        --emitted;
    }
    return static_cast<double>(emitted) / 65536.0;
}

// The same advance for a face DirectWrite is simulating bold on. The simulation
// bit makes gfxDWriteFont::ProvidesGlyphWidths() true whatever the other two
// terms say, so the advance always comes from gfxDWriteFont::GetGlyphWidth.
// Negative when the simulated face cannot be built. Called with g_faces_mutex
// held.
double WinBoldGlyphAdvanceLocked(FaceEntry* entry, const WinInstance& inst, const UINT16 glyph,
                                 const bool has_variations)
{
    // gfxDWriteFontEntry::CreateFontInstance, with
    // gfx.font_rendering.directwrite.bold_simulation at its default of 1.
    if (entry->memory != nullptr || FaceHasCOLRLocked(entry)) {
        return WinSyntheticBoldAdvance(inst, WinGlyphAdvance(inst, glyph, has_variations));
    }
    const Factories factories = GetFactories();
    if (factories.factory == nullptr) {
        return -1.0;
    }
    IDWriteFontFace* bold =
        GetDWriteFaceLocked(entry, factories.factory, DWRITE_FONT_SIMULATIONS_BOLD, inst.axes);
    if (bold == nullptr) {
        return -1.0;
    }
    return static_cast<double>(NSlround(WinMeasureGlyphWidth(inst, bold, glyph) * 65536.0)) / 65536.0;
}

// The ink top and right of one glyph on the face DirectWrite is simulating bold
// on, in pixels, which is what gfxDWriteFont::GetGlyphBounds measures on
// Windows:
//
//     bounds = (leftSideBearing, topSideBearing - verticalOriginY,
//               advanceWidth - leftSideBearing - rightSideBearing, ...)
//     bounds.Scale(mFUnitsConvFactor)
//
// so the top is verticalOriginY - topSideBearing and the right edge is
// advanceWidth - rightSideBearing, both in design units. False when the
// simulated face cannot be built or the glyph has no metrics. Called with
// g_faces_mutex held.
bool WinBoldGlyphInkLocked(FaceEntry* entry, const WinInstance& inst, const UINT16 glyph,
                           double* top, double* right)
{
    const Factories factories = GetFactories();
    if (factories.factory == nullptr) {
        return false;
    }
    IDWriteFontFace* bold =
        GetDWriteFaceLocked(entry, factories.factory, DWRITE_FONT_SIMULATIONS_BOLD, inst.axes);
    if (bold == nullptr) {
        return false;
    }
    DWRITE_GLYPH_METRICS gm{};
    if (FAILED(bold->GetDesignGlyphMetrics(&glyph, 1, &gm))) {
        return false;
    }
    const double conv = static_cast<double>(inst.funits_conv);
    *top = static_cast<double>(gm.verticalOriginY - gm.topSideBearing) * conv;
    *right = static_cast<double>(static_cast<INT32>(gm.advanceWidth) - gm.rightSideBearing) * conv;
    return true;
}

// gfxFont::GetCharAdvance: -1.0 when the font has no glyph for the character.
double WinCharAdvance(const WinInstance& inst, const UINT32 ch, const bool has_variations)
{
    UINT16 glyph = 0;
    if (FAILED(inst.dwrite_face->GetGlyphIndices(&ch, 1, &glyph)) ||
        glyph == 0) {
        return -1.0;
    }
    return WinGlyphAdvance(inst, glyph, has_variations);
}

// firefox_parity_data.h kBadUnderlineFamilies (modules/libpref/init/all.js
// font.blacklist.underline_offset), the list gfxPlatformFontList::
// LoadBadUnderlineList loads and gfxDWriteFontList applies to a family in
// gfx/thebes/gfxDWriteFontList.cpp (the addFamily lambda of
// AppendFamiliesFromCollection); gfxFcPlatformFontList passes
// badUnderline=false for every family, so the flag is reconstructed here from
// the family name the way BuildKeyNameFromFontName keys it (case-folded).
bool IsBadUnderlineFamily(const char* family)
{
    if (family == nullptr) {
        return false;
    }
    for (const char* candidate : firefox_parity::kBadUnderlineFamilies) {
        if (strcasecmp(candidate, family) == 0) {
            return true;
        }
    }
    return false;
}

// gfx/thebes/gfxFont.cpp gfxFont::SanitizeMetrics, for a font with no
// ascent/descent/line-gap override descriptors and mStyle.systemFont false.
void WinSanitizeMetrics(WinMetrics* m, const bool bad_underline)
{
    m->underlineSize = std::max(1.0, m->underlineSize);
    m->strikeoutSize = std::max(1.0, m->strikeoutSize);
    m->underlineOffset = std::min(m->underlineOffset, -1.0);

    if (m->maxAscent < 1.0) {
        m->underlineSize = 0;
        m->underlineOffset = 0;
        m->strikeoutSize = 0;
        m->strikeoutOffset = 0;
        return;
    }

    if (bad_underline) {
        m->underlineOffset = std::min(m->underlineOffset, -2.0);
        if (m->internalLeading + m->externalLeading > m->underlineSize) {
            m->underlineOffset = std::min(m->underlineOffset, -m->emDescent);
        } else {
            m->underlineOffset =
                std::min(m->underlineOffset, m->underlineSize - m->emDescent);
        }
    } else if (m->underlineSize - m->underlineOffset > m->maxDescent) {
        if (m->underlineSize > m->maxDescent) {
            m->underlineSize = std::max(m->maxDescent, 1.0);
        }
        m->underlineOffset = m->underlineSize - m->maxDescent;
    }

    double halfOfStrikeoutSize = std::floor(m->strikeoutSize / 2.0 + 0.5);
    if (halfOfStrikeoutSize + m->strikeoutOffset > m->maxAscent) {
        if (m->strikeoutSize > m->maxAscent) {
            m->strikeoutSize = std::max(m->maxAscent, 1.0);
            halfOfStrikeoutSize = std::floor(m->strikeoutSize / 2.0 + 0.5);
        }
        const double ascent = std::floor(m->maxAscent + 0.5);
        m->strikeoutOffset = std::max(halfOfStrikeoutSize, ascent / 2.0);
    }

    if (m->underlineSize > m->maxAscent) {
        m->underlineSize = m->maxAscent;
    }
}

// The system font collection, resolved once and kept for the life of the
// process.
//
// Building it is not a DWriteCore-internal operation here.
// FactoryProxy::GetSystemFontCollection enumerates through fontconfig,
// fontconfig scans font files with FreeType, and that comes back as this
// library's own FT_New_Face, which takes g_faces_mutex. Building it under that
// lock is a self-deadlock.
//
// WarmSystemCollection builds it and must be called with no lock held.
// SystemCollectionIfWarm only reads, and answers null while it is not built,
// which costs a face its Arial Bold measurement on the first size of a
// process.
pthread_mutex_t g_collection_mutex = PTHREAD_MUTEX_INITIALIZER;
IDWriteFontCollection* g_collection = nullptr;
bool g_collection_tried = false;

// Never call with g_faces_mutex held.
void WarmSystemCollection()
{
    pthread_mutex_lock(&g_collection_mutex);
    const bool tried = g_collection_tried;
    pthread_mutex_unlock(&g_collection_mutex);
    if (tried) {
        return;
    }

    const Factories factories = GetFactories();
    IDWriteFontCollection* collection = nullptr;
    if (factories.factory != nullptr &&
        FAILED(factories.factory->GetSystemFontCollection(&collection, FALSE))) {
        collection = nullptr;
    }

    pthread_mutex_lock(&g_collection_mutex);
    if (!g_collection_tried) {
        g_collection = collection;
        g_collection_tried = true;
        collection = nullptr;
    }
    pthread_mutex_unlock(&g_collection_mutex);
    // Another thread got there first, so this one's reference is surplus.
    if (collection != nullptr) {
        collection->Release();
    }
}

IDWriteFontCollection* SystemCollectionIfWarm()
{
    pthread_mutex_lock(&g_collection_mutex);
    IDWriteFontCollection* const collection = g_collection;
    pthread_mutex_unlock(&g_collection_mutex);
    return collection;
}

// FORCED INTERCEPTION POINT. gfxDWriteFont::GetFakeMetricsForArialBlack:
// ComputeMetrics measures a non-user-font face whose weight is exactly 900
// and whose entry Name() is "Arial Black" with the DWRITE_FONT_METRICS of the
// Arial family's weight-700 face (gfxPlatformFontList::FindFontForFamily
// ("Arial", weight 700)). With the shared font list (gfx.e10s.font-list.shared)
// a gfxDWriteFontEntry's Name() is the face's PostScript name -
// gfxDWriteFontList::ReadFaceNamesForFamily reads PSNAME_ID - so the literal
// is compared against FT_Get_Postscript_Name here. Gecko's own comparison is
// EqualsLiteral("Arial Black") in gfxDWriteFonts.cpp, with the space, so this
// matches the genuine font and is not a dead branch.
// The Arial face is found through libdwritecore's system font collection,
// which on this platform is built from fontconfig's fonts
// (src/system_fonts.cpp), with IDWriteFontFamily::GetFirstMatchingFont(BOLD,
// NORMAL, NORMAL) as the style match. Called with g_faces_mutex held.
bool WindowsArialBlackMetricsLocked(const FaceEntry* entry, DWRITE_FONT_METRICS* out)
{
    const char* ps_name = FT_Get_Postscript_Name(entry->face);
    if (ps_name == nullptr || std::strcmp(ps_name, "Arial Black") != 0 ||
        entry->memory != nullptr) {
        return false;
    }
    ft_get_sfnt_table_fn real = real_FT_Get_Sfnt_Table();
    const TT_OS2* os2 = real ? static_cast<const TT_OS2*>(real(entry->face, FT_SFNT_OS2)) : nullptr;
    if (os2 == nullptr || os2->version == 0xFFFF || os2->usWeightClass != 900) {
        return false;
    }
    // Borrowed from the cache, never built here. See WarmSystemCollection.
    IDWriteFontCollection* const collection = SystemCollectionIfWarm();
    if (collection == nullptr) {
        return false;
    }
    bool found = false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    IDWriteFontFamily* arial = nullptr;
    if (SUCCEEDED(collection->FindFamilyName(u"Arial", &index, &exists)) &&
        exists &&
        SUCCEEDED(collection->GetFontFamily(index, &arial)) &&
        arial != nullptr) {
        IDWriteFont* bold = nullptr;
        if (SUCCEEDED(arial->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &bold)) &&
            bold != nullptr) {
            // GetFirstMatchingFont returns the closest face it has and never
            // fails, so a machine carrying arial.ttf without arialbd.ttf
            // answers with Arial Regular, or with the bold DirectWrite
            // simulates for it. Either would measure Arial Black against
            // something no Windows machine measures it against.
            IDWriteFontFace* bold_face = nullptr;
            if (bold->GetWeight() >= DWRITE_FONT_WEIGHT_BOLD &&
                (bold->GetSimulations() & DWRITE_FONT_SIMULATIONS_BOLD) == 0 &&
                SUCCEEDED(bold->CreateFontFace(&bold_face)) &&
                bold_face != nullptr) {
                bold_face->GetMetrics(out);
                found = out->designUnitsPerEm != 0;
                bold_face->Release();
            }
            bold->Release();
        }
        arial->Release();
    }
    LogLine("face %p: Arial Black measured with Arial Bold metrics%s", reinterpret_cast<void*>(entry->face),
            found ? "" : " - no Arial Bold face, using its own");
    return found;
}

// Two em sizes are the same instance. 26.6 is the resolution every size in
// here arrives at, and the resolution the instance cache keys on.
bool SizesMatch(const double a, const double b)
{
    return static_cast<FT_Fixed>(std::floor(a * 64.0 + 0.5)) ==
           static_cast<FT_Fixed>(std::floor(b * 64.0 + 0.5));
}

// The size mFUnitsConvFactor belongs to for an instance asked for at `size`.
//
// gfxDWriteFont::ComputeMetrics divides by mAdjustedSize before rounding it
// onto a bitmap strike, so a font drawing from a strike keeps the factor of
// the size it was born at. libxul_patch.cpp writes the rounded size into both
// gfxFont::mAdjustedSize and gfxFT2FontBase::mFTSize, which leaves
// GetFTGlyphExtents scaling by one but also makes every later FreeType call
// carry the rounded size, so an instance gets built for it. The one built for
// the size it rounds from is the same gfxDWriteFont, and its size is the one
// Windows divides by.
//
// Called with g_faces_mutex held.
double ConvSizeLocked(const FaceEntry* entry, const double size)
{
    // A size the face has already been asked for outright is a font of its
    // own, whose factor is that size. Only a size no font asked for can be one
    // the patch wrote in place of the size it rounds from.
    for (const WinInstance& other : entry->instances) {
        if (other.valid && SizesMatch(other.requested_size, size)) {
            return size;
        }
    }
    for (const WinInstance& other : entry->instances) {
        if (other.valid && other.bitmap_font && SizesMatch(other.adjusted_size, size)) {
            return other.requested_size;
        }
    }
    return size;
}

// gfxDWriteFont::ComputeMetrics for one (face, mAdjustedSize). Called with
// g_faces_mutex held. The font-size-adjust recomputation is not reproduced:
// it is already folded into the size Firefox asks FreeType for.
bool ComputeWinInstanceLocked(FaceEntry* entry, const double size, WinInstance* inst)
{
    const Factories factories = GetFactories();
    if (factories.factory == nullptr || !(size > 0.0)) {
        return false;
    }
    IDWriteFontFace* dwrite_face =
        GetDWriteFaceLocked(entry, factories.factory, DWRITE_FONT_SIMULATIONS_NONE, inst->axes);
    if (dwrite_face == nullptr) {
        return false;
    }
    DWRITE_FONT_METRICS fm = {};
    if (!WindowsArialBlackMetricsLocked(entry, &fm)) {
        dwrite_face->GetMetrics(&fm);
    }
    if (fm.designUnitsPerEm == 0) {
        return false;
    }
    const bool has_variations = FT_HAS_MULTIPLE_MASTERS(entry->face) != 0;
    const Options& options = GetOptions();

    dwrite_face->AddRef();
    inst->dwrite_face = dwrite_face;
    inst->font_metrics = fm;
    inst->requested_size = size;
    inst->adjusted_size = size;
    // mFUnitsConvFactor is computed from mAdjustedSize before the bitmap-strike
    // rounding below, and not recomputed after it. The size arriving here can
    // already be that rounded one: libxul_patch.cpp writes it into both of the
    // font's size fields, so every FreeType call the font makes afterwards
    // carries it. The instance built for the size it rounds from holds the
    // factor Windows keeps, so that one answers.
    inst->funits_conv =
        static_cast<float>(ConvSizeLocked(entry, size) / fm.designUnitsPerEm);

    // anAAOption == kAntialiasDefault && UsingClearType() &&
    //     GetMeasuringMode() == DWRITE_MEASURING_MODE_NATURAL
    // UsingClearType() is gfxVars::SystemTextQuality() == CLEARTYPE_QUALITY,
    // the Windows font-smoothing setting; GetMeasuringMode() is
    // DWriteSettings::MeasuringMode(), NATURAL for the DEFAULT system
    // rendering mode (gfx/2d/DWriteSettings.cpp UpdateRenderingMode).
    inst->use_subpixel_positions = options.measuring_mode == DWRITE_MEASURING_MODE_NATURAL;

    inst->bitmap_font = IsBitmapFontLocked(entry, size);
    if (inst->bitmap_font) {
        inst->adjusted_size = NSlround(size);
        inst->use_subpixel_positions = false;
    }
    inst->bad_underline = IsBadUnderlineFamily(entry->face->family_name);
    WinMetrics& m = inst->metrics;
    std::memset(&m, 0, sizeof(m));
    const float f = inst->funits_conv;
    m.xHeight = static_cast<double>(fm.xHeight * f);
    m.capHeight = static_cast<double>(fm.capHeight * f);
    m.maxAscent = static_cast<double>(std::round(fm.ascent * f));
    m.maxDescent = static_cast<double>(std::round(fm.descent * f));
    m.maxHeight = m.maxAscent + m.maxDescent;
    m.emHeight = inst->adjusted_size;
    // gfxDWriteFonts divides unguarded here, so a face whose rounded ascent and
    // descent are both zero yields NaN on Windows. Zero is carried instead, so
    // no NaN reaches the size metrics.
    m.emAscent = m.maxHeight != 0.0 ? m.emHeight * m.maxAscent / m.maxHeight : 0.0;
    m.emDescent = m.emHeight - m.emAscent;

    m.maxAdvance = inst->adjusted_size;
    {
        // The 'hhea' table's advanceWidthMax, through gfxFontEntry::AutoTable.
        const void* data = nullptr;
        UINT32 length = 0;
        void* context = nullptr;
        BOOL exists = FALSE;
        if (SUCCEEDED(dwrite_face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('h', 'h', 'e', 'a'), &data, &length, &context, &exists))) {
            if (exists && data != nullptr && length >= 36) {
                const auto* bytes = static_cast<const uint8_t*>(data);
                const uint16_t advance_width_max = static_cast<uint16_t>((bytes[10] << 8) | bytes[11]);
                m.maxAdvance = static_cast<double>(advance_width_max * f);
            }
            dwrite_face->ReleaseFontTable(context);
        }
    }

    m.internalLeading = std::max(m.maxHeight - m.emHeight, 0.0);
    m.externalLeading = static_cast<double>(std::ceil(fm.lineGap * f));

    {
        constexpr UINT32 ucs = L' ';
        UINT16 glyph = 0;
        if (SUCCEEDED(dwrite_face->GetGlyphIndices(&ucs, 1, &glyph)) &&
            glyph != 0) {
            m.spaceWidth = WinMeasureGlyphWidth(*inst, dwrite_face, glyph);
        } else {
            m.spaceWidth = 0;
        }
    }

    if (inst->use_subpixel_positions) {
        m.aveCharWidth = 0;
        const void* data = nullptr;
        UINT32 length = 0;
        void* context = nullptr;
        BOOL exists = FALSE;
        if (SUCCEEDED(dwrite_face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'), &data, &length, &context, &exists))) {
            if (exists && data != nullptr && length >= 4) {
                const auto* bytes = static_cast<const uint8_t*>(data);
                const int16_t x_avg_char_width = static_cast<int16_t>((bytes[2] << 8) | bytes[3]);
                m.aveCharWidth = static_cast<double>(x_avg_char_width * f);
            }
            dwrite_face->ReleaseFontTable(context);
        }
    }
    if (m.aveCharWidth < 1) {
        m.aveCharWidth = WinCharAdvance(*inst, 'x', has_variations);
        if (m.aveCharWidth < 1) {
            m.aveCharWidth = static_cast<double>(fm.xHeight * f);
        }
    }

    m.zeroWidth = WinCharAdvance(*inst, '0', has_variations);
    m.ideographicWidth = WinCharAdvance(*inst, 0x6C34, has_variations);  // kWaterIdeograph

    m.underlineOffset = static_cast<double>(fm.underlinePosition * f);
    m.underlineSize = static_cast<double>(fm.underlineThickness * f);
    m.strikeoutOffset = static_cast<double>(fm.strikethroughPosition * f);
    m.strikeoutSize = static_cast<double>(fm.strikethroughThickness * f);

    WinSanitizeMetrics(&m, inst->bad_underline);
    inst->valid = true;
    return true;
}


// The instance for this face at this requested size, computed on first use.
// Called with g_faces_mutex held.
// The instance for this size and axis position if one has been built, and
// nothing otherwise. A caller that only reads an instance takes this, since
// building one reads the face's current FreeType state and evicts the oldest
// entry, neither of which belongs to a caller that is merely asking.
WinInstance* FindWinInstanceLocked(FaceEntry* entry, const double size)
{
    const FT_Fixed key = static_cast<FT_Fixed>(std::floor(size * 64.0 + 0.5));
    const std::vector<DWRITE_FONT_AXIS_VALUE> axes = GetAxisValues(entry->face);
    for (WinInstance& inst : entry->instances) {
        if (inst.size_26_6 == key && SameAxes(inst.axes, axes)) {
            return &inst;
        }
    }
    return nullptr;
}

WinInstance* GetWinInstanceLocked(FaceEntry* entry, const double size)
{
    if (WinInstance* found = FindWinInstanceLocked(entry, size)) {
        return found;
    }
    const FT_Fixed key = static_cast<FT_Fixed>(std::floor(size * 64.0 + 0.5));
    const std::vector<DWRITE_FONT_AXIS_VALUE> axes = GetAxisValues(entry->face);
    if (entry->instances.size() >= 32) {
        if (entry->instances.front().dwrite_face != nullptr) {
            entry->instances.front().dwrite_face->Release();
        }
        entry->instances.erase(entry->instances.begin());
    }
    WinInstance inst = {};
    inst.size_26_6 = key;
    inst.axes = axes;
    inst.valid = false;
    if (!ComputeWinInstanceLocked(entry, size, &inst)) {
        inst.valid = false;
        LogLine("face %p at %.4fpx: no Windows font instance (DirectWrite face unavailable)",
                reinterpret_cast<void*>(entry->face), size);
    } else {
        const WinMetrics& m = inst.metrics;
        LogLine("face %p at %.4fpx: windows metrics asc=%g desc=%g em=%g il=%g el=%g space=%g "
                "ave=%g zero=%g uo=%g us=%g so=%g ss=%g%s%s",
                reinterpret_cast<void*>(entry->face), size, m.maxAscent, m.maxDescent, m.emHeight,
                m.internalLeading, m.externalLeading, m.spaceWidth, m.aveCharWidth, m.zeroWidth,
                m.underlineOffset, m.underlineSize, m.strikeoutOffset, m.strikeoutSize,
                inst.bitmap_font ? " bitmap-strike" : "",
                inst.use_subpixel_positions ? "" : " no-subpixel-positions");
    }
    entry->instances.push_back(inst);
    return &entry->instances.back();
}

// ---------------------------------------------------------------------------
// What gfxFT2FontBase::InitMetrics reads, written so that it computes the
// Windows instance's metrics:
//
//   maxAscent  = FLOAT_FROM_26_6(ftMetrics.ascender)
//   maxDescent = -FLOAT_FROM_26_6(ftMetrics.descender)
//   maxAdvance = FLOAT_FROM_26_6(ftMetrics.max_advance)
//   emAscent   = os2->sTypoAscender * yScale      (normalized to emHeight)
//   emDescent  = -os2->sTypoDescender * yScale
//   lineHeight = (sTypoAscender - sTypoDescender + sTypoLineGap) * yScale
//   lineHeight = floor(max(lineHeight, maxHeight) + 0.5)
//   externalLeading = lineHeight - internalLeading - emHeight
//   underlineOffset = post->underlinePosition * yScale   (when non-zero)
//   yScale = FLOAT_FROM_26_6(FLOAT_FROM_16_16(ftMetrics.y_scale))
//
// The OS/2 and post tables come from FT_Get_Sfnt_Table, which this file
// interposes to hand back a copy per face and size. The copy clears
// fsSelection's USE_TYPO_METRICS bit, so maxAscent and maxDescent keep coming
// from the size metrics.
//
// emHeight, internalLeading and aveCharWidth cannot be reached: InitMetrics
// rounds those itself, and no input to it can make them fractional.
// ---------------------------------------------------------------------------

// gfxFT2FontBase::InitMetrics' underline (post->underlinePosition * yScale
// when the face has underline metrics and the value is non-zero, else
// face->underline_position * yScale + 0.5 * underlineSize), with the post
// value SubstitutePost would hand back, then gfxFont::SanitizeMetrics(false)
// over the Linux maxAscent/maxDescent. What nsFontMetrics on Linux folds into
// the descent.
void LinuxUnderlineLocked(FT_Face face, const WinInstance& inst, const double max_ascent,
                          const double max_descent, double* offset, double* size)
{
    const WinMetrics& m = inst.metrics;
    const double y_scale = static_cast<double>(face->size->metrics.y_scale) / 65536.0 / 64.0;
    WinMetrics on_linux = m;
    on_linux.maxAscent = max_ascent;
    on_linux.maxDescent = max_descent;
    double em_height = 0.0;
    float x_over_y = 1.0f;
    GetScaledEmSize(face, &em_height, &x_over_y);
    if (face->underline_position != 0 && face->underline_thickness != 0 && y_scale > 0.0) {
        on_linux.underlineSize = static_cast<double>(face->underline_thickness) * y_scale;
        long post_position = 0;
        if (ft_get_sfnt_table_fn real = real_FT_Get_Sfnt_Table()) {
            if (const auto* post = static_cast<const TT_Postscript*>(real(face, FT_SFNT_POST))) {
                post_position = post->underlinePosition;
            }
        }
        // The candidate SubstitutePost installs when it survives.
        const long candidate = lround(m.underlineOffset / y_scale);
        if (candidate != 0 && candidate >= -32768 && candidate <= 32767) {
            WinMetrics trial = on_linux;
            trial.underlineOffset = static_cast<double>(candidate) * y_scale;
            WinSanitizeMetrics(&trial, false);
            if (std::fabs(trial.underlineOffset - m.underlineOffset) < 1e-3 &&
                std::fabs(trial.underlineSize - m.underlineSize) < 1e-3) {
                post_position = candidate;
            }
        }
        if (post_position != 0) {
            on_linux.underlineOffset = static_cast<double>(post_position) * y_scale;
        } else {
            on_linux.underlineOffset =
                static_cast<double>(face->underline_position) * y_scale + 0.5 * on_linux.underlineSize;
        }
    } else {
        // gfxFT2FontBase divides the unrounded em height here and only rounds
        // emHeight afterwards.
        on_linux.underlineSize = em_height / 14.0;
        on_linux.underlineOffset = -on_linux.underlineSize;
    }
    WinSanitizeMetrics(&on_linux, false);
    *offset = on_linux.underlineOffset;
    *size = on_linux.underlineSize;
}

// gfx/src/nsFontMetrics.cpp:
//   ComputeMaxAscent  = floor(maxAscent + 0.5)
//   ComputeMaxDescent = floor(max(minDescent, maxDescent) + 0.5)
//     minDescent = floor(-fontGroup->GetUnderlineOffset() + 0.5)
//                  + NS_round(underlineSize)
// for this font as the first font of its group.
double ComputeMaxDescent(const double max_descent, const double underline_offset, const double underline_size)
{
    const double offset = std::floor(-underline_offset + 0.5);
    const double size = NSlround(underline_size);
    return std::floor(std::max(offset + size, max_descent) + 0.5);
}

// Rewrites size->metrics after an entry point that set a size.
void ApplyWindowsMetrics(FT_Face face, const char* via)
{
    if (face == nullptr || face->size == nullptr || !FT_IS_SCALABLE(face) ||
        !WindowsMetrics()) {
        return;
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (!GetEmSize(face, &em_size, &x_over_y) || !(em_size > 0.0)) {
        return;
    }
    // Resolve the system font collection before locking. Building it under
    // g_faces_mutex deadlocks; see WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    WinInstance* inst = entry != nullptr ? GetWinInstanceLocked(entry, em_size) : nullptr;
    if (inst == nullptr || !inst->valid) {
        pthread_mutex_unlock(&g_faces_mutex);
        return;
    }
    const WinMetrics& m = inst->metrics;
    FT_Size_Metrics& sm = face->size->metrics;
    // The size on the face is this instance's, so the scale is the one
    // InitMetrics will read for it.
    inst->ft_x_scale = sm.x_scale;
    FT_Pos ascender = static_cast<FT_Pos>(llround(m.maxAscent)) << 6;
    FT_Pos descender = static_cast<FT_Pos>(llround(m.maxDescent)) << 6;

    // nsFontMetrics folds the underline into the descent, and the underline it
    // folds is the one InitMetrics computed on Linux. That differs from the
    // Windows one exactly when SanitizeMetrics' bad-underline branch produced
    // the Windows value, since the flag is never set on Linux.
    //
    // So when Windows' ComputeMaxDescent is one more than its maxDescent and
    // Linux's would not be, half a pixel moves from the ascent to the descent.
    // floor(maxAscent - 0.5 + 0.5) and floor(maxDescent + 0.5 + 0.5) then give
    // the Windows MaxAscent and MaxDescent. maxHeight is unchanged, and with
    // it internalLeading and the lineHeight clamp. SanitizeMetrics(false)
    // leaves an underline that folds the same way.
    const double win_max_descent =
        ComputeMaxDescent(m.maxDescent, m.underlineOffset, m.underlineSize);
    double linux_offset = 0.0;
    double linux_size = 0.0;
    LinuxUnderlineLocked(face, *inst, m.maxAscent, m.maxDescent, &linux_offset, &linux_size);
    const double linux_max_descent = ComputeMaxDescent(m.maxDescent, linux_offset, linux_size);
    inst->descent_fold = false;
    if (linux_max_descent != win_max_descent) {
        if (win_max_descent == m.maxDescent + 1.0 && ascender >= 32) {
            ascender -= 32;
            descender += 32;
            inst->descent_fold = true;
        } else {
            LogLine("face %p at %.4fpx: Windows MaxDescent %g (maxDescent %g) is not "
                    "reproducible; Linux folds to %g",
                    reinterpret_cast<void*>(face), em_size, win_max_descent, m.maxDescent, linux_max_descent);
        }
    }

    sm.ascender = ascender;
    sm.descender = -descender;
    sm.height = static_cast<FT_Pos>(std::floor(m.emHeight + m.internalLeading + m.externalLeading + 0.5)) << 6;
    sm.max_advance = static_cast<FT_Pos>(std::floor(m.maxAdvance * 64.0 + 0.5));
    // `inst` points into entry->instances, which another thread reallocates by
    // caching a new size and destroys by dropping the face, so it is good only
    // while the lock is held.
    const bool folded = inst->descent_fold;
    pthread_mutex_unlock(&g_faces_mutex);
    LogLine("size metrics %s %.4fpx asc=%ld desc=%ld height=%ld%s via %s",
            face->family_name ? face->family_name : "?", em_size, static_cast<long>(sm.ascender),
            static_cast<long>(sm.descender), static_cast<long>(sm.height), folded ? " descent-fold" : "", via);
}

struct SfntKey
{
    FT_Face face;
    FT_Fixed size_26_6;
    bool operator<(const SfntKey& other) const
    {
        if (face != other.face) return face < other.face;
        return size_26_6 < other.size_26_6;
    }
};

std::map<SfntKey, TT_OS2> g_os2_copies;
std::map<SfntKey, TT_Postscript> g_post_copies;

// Keeps the copies for one face bounded. Entries are otherwise held until the
// face goes, and a page animating font-size asks for a new size every frame.
// The lowest surviving size is dropped first, and never the one being handed
// back.
constexpr size_t kMaxSfntCopiesPerFace = 32;

template <typename Copies>
void CapSfntCopiesLocked(Copies& copies, const SfntKey& keep)
{
    const SfntKey lowest{.face = keep.face, .size_26_6 = std::numeric_limits<FT_Fixed>::min()};
    size_t count = 0;
    for (auto it = copies.lower_bound(lowest); it != copies.end() && it->first.face == keep.face;
         ++it) {
        ++count;
    }
    while (count > kMaxSfntCopiesPerFace) {
        auto it = copies.lower_bound(lowest);
        while (it != copies.end() && it->first.face == keep.face &&
               it->first.size_26_6 == keep.size_26_6) {
            ++it;
        }
        if (it == copies.end() || it->first.face != keep.face) {
            break;                           // only the kept size is left
        }
        copies.erase(it);
        --count;
    }
}

#endif  // CLEARTYPE_FIREFOX_PARITY

size_t SfntCopyCountLocked()
{
#if CLEARTYPE_FIREFOX_PARITY
    return g_os2_copies.size() + g_post_copies.size();
#else
    return 0;
#endif
}

// How much every side table is holding, logged when the last library goes
// away. Nothing here is keyed on anything but a face, so once the faces are
// gone the counts are the whole of what this library still owns, and anything
// non-zero has been left behind. Written to the log, since cleartype.map
// exports the interposed entry points and nothing else.
//
// The per-face vectors are summed while g_faces_mutex is held, and the three
// standalone tables are read after it is dropped, in the documented order.
void LogTableCensus(const char* when)
{
    if (!LogEnabled()) {
        return;
    }
    size_t faces = 0;
    size_t dwrite_faces = 0;
    size_t instances = 0;
    pthread_mutex_lock(&g_faces_mutex);
    faces = g_faces.size();
    for (const FaceEntry& entry : g_faces) {
        dwrite_faces += entry.dwrite_faces.size();
#if CLEARTYPE_FIREFOX_PARITY
        instances += entry.instances.size();
#endif
    }
    const size_t sfnt = SfntCopyCountLocked();
    const size_t memfonts = MemoryFontFiles().size();
    size_t memfonts_owned = 0;
    for (const MemoryFontFile& cached : MemoryFontFiles()) {
        memfonts_owned += cached.blob != 0 ? 1 : 0;
    }
    pthread_mutex_unlock(&g_faces_mutex);

    pthread_mutex_lock(&g_tracked_mutex);
    const size_t tracked = g_tracked.size();
    pthread_mutex_unlock(&g_tracked_mutex);

    pthread_mutex_lock(&g_owners_mutex);
    const size_t owners = g_owners.size();
    pthread_mutex_unlock(&g_owners_mutex);

    pthread_mutex_lock(&g_shifts_mutex);
    const size_t shifts = g_shifts.size();
    size_t orphan_shifts = 0;
    for (const PendingOutline& pending : g_shifts) {
        orphan_shifts += pending.face == nullptr ? 1 : 0;
    }
    pthread_mutex_unlock(&g_shifts_mutex);

    LogLine("table census %s: faces=%zu tracked=%zu owners=%zu pending=%zu(%zu bare) "
            "sfnt=%zu dwrite_faces=%zu instances=%zu memfonts=%zu(%zu as streams)",
            when, faces, tracked, owners, shifts, orphan_shifts, sfnt, dwrite_faces,
            instances, memfonts, memfonts_owned);
}

// A census every so often, for a process that runs for hours. Firefox content
// processes rasterize for as long as a tab lives and rarely reach
// FT_Done_FreeType at all.
//
// Gated on a counter first so the clock is not read per glyph, and on the log
// being open at all, so a browser started without CLEARTYPE_LOG pays one
// relaxed increment.
std::atomic<unsigned> g_census_ticks{0};
std::atomic<long> g_census_last{0};

void MaybeLogTableCensus()
{
    if (!LogEnabled()) {
        return;
    }
    if ((g_census_ticks.fetch_add(1, std::memory_order_relaxed) & 0xFF) != 0) {
        return;
    }
    // CLEARTYPE_CENSUS_SECONDS sets the interval; 10 by default, 0 to log one
    // per burst of renders and nothing else.
    static const long interval = [] {
        const char* value = std::getenv("CLEARTYPE_CENSUS_SECONDS");
        return value != nullptr ? std::strtol(value, nullptr, 10) : 10L;
    }();
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long previous = g_census_last.load(std::memory_order_relaxed);
    if (previous != 0 && now.tv_sec - previous < interval) {
        return;
    }
    g_census_last.store(now.tv_sec, std::memory_order_relaxed);
    LogTableCensus("while running");
}

#if CLEARTYPE_FIREFOX_PARITY
// The families the parity path asks for by name: the intersection of
// firefox_parity_data.h with Microsoft's Windows 11 font list. A machine
// without them still renders, since fontconfig substitutes, but a chain that
// falls through to Noto draws something Windows would not have.
//
// The prefs also name families that Windows keeps in a Feature-on-Demand
// package - Meiryo, MS Mincho, Yu Mincho, Batang, Gulim, MingLiU, SimHei and
// the rest - which a stock Windows 11 has no more than this machine does. The
// Ext-B fonts invert that: MingLiU-ExtB ships in the base image while plain
// MingLiU does not.
//
// "Arial Black" is absent even though the interception is about that face,
// because there is no such family to ask for. DirectWrite groups by
// weight-stretch-style, which puts Arial Black inside Arial at weight 900, so
// FindFamilyName("Arial Black") answers no on a machine that has the face, as
// it does on Windows 11. The interception finds the face by PostScript name,
// which is what Firefox on Windows compares.
struct ParityFamily
{
    const WCHAR* wide;
    const char* name;
    // The free face drawn at the same widths, where one exists. Without one a
    // substitution moves every glyph and the two are not comparable at all.
    const char* metric_match;
    // Whether a stock Windows 11 ships a designed bold for this family. Where
    // it does not, Windows synthesizes bold exactly as a Linux machine does,
    // and the absence needs no warning.
    bool windows_bold;
};
// The metric-compatible substitute and the designed-bold flag are meant to be
// scanned down the column.
// ReSharper disable CppUseDesignatedInitializers
constexpr ParityFamily kParityFamilies[] = {
    { u"Arial",               "Arial",               "Liberation Sans" , true  },
    { u"Times New Roman",     "Times New Roman",     "Liberation Serif", true  },
    { u"Courier New",         "Courier New",         "Liberation Mono" , true  },
    { u"Segoe UI",            "Segoe UI",            "Selawik"         , true  },
    { u"Comic Sans MS",       "Comic Sans MS",       nullptr           , true  },
    { u"Consolas",            "Consolas",            nullptr           , true  },
    { u"Tahoma",              "Tahoma",              nullptr           , true  },
    { u"Cambria Math",        "Cambria Math",        nullptr           , false },
    { u"Segoe UI Emoji",      "Segoe UI Emoji",      nullptr           , false },
    { u"Sylfaen",             "Sylfaen",             nullptr           , false },
    { u"Nirmala UI",          "Nirmala UI",          nullptr           , true  },
    { u"Microsoft Himalaya",  "Microsoft Himalaya",  nullptr           , false },
    { u"MS Gothic",           "MS Gothic",           nullptr           , false },
    { u"MS PGothic",          "MS PGothic",          nullptr           , false },
    { u"Yu Gothic",           "Yu Gothic",           nullptr           , true  },
    { u"Malgun Gothic",       "Malgun Gothic",       nullptr           , true  },
    { u"Microsoft YaHei",     "Microsoft YaHei",     nullptr           , true  },
    { u"Microsoft JhengHei",  "Microsoft JhengHei",  nullptr           , true  },
    { u"SimSun",              "SimSun",              nullptr           , false },
    { u"NSimSun",             "NSimSun",             nullptr           , false },
    { u"SimSun-ExtB",         "SimSun-ExtB",         nullptr           , false },
    { u"MingLiU-ExtB",        "MingLiU-ExtB",        nullptr           , false },
    { u"MingLiU_HKSCS-ExtB",  "MingLiU_HKSCS-ExtB",  nullptr           , false },
};
// ReSharper restore CppUseDesignatedInitializers
// Says once which of those this machine does not have.
//
// The log gets all of them. stderr gets only the ones with no
// metric-compatible face, and only from the parent process, since that is the
// one condition nothing else in the shim reports. A machine with the fonts
// prints nothing at all.
// Whether the family at `index` carries a bold face the designer drew.
//
// Two things have to be excluded, and neither shows up as a failed call.
// GetFirstMatchingFont answers with the closest face it has and never fails,
// so a family holding only its regular file answers at the regular weight.
// DirectWrite then fills the gap itself: a family with no bold face still
// offers one at weight 700 with DWRITE_FONT_SIMULATIONS_BOLD set, which is
// algorithmic emboldening and is exactly what this is looking for the absence
// of. The weight alone would report every family as complete.
bool FamilyHasDesignedBold(IDWriteFontCollection* collection, const UINT32 index)
{
    IDWriteFontFamily* family = nullptr;
    if (FAILED(collection->GetFontFamily(index, &family)) || family == nullptr) {
        return true;   // unreadable, so nothing to report
    }
    IDWriteFont* font = nullptr;
    bool designed = true;
    if (SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_BOLD,
                                               DWRITE_FONT_STRETCH_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, &font)) &&
        font != nullptr) {
        designed = font->GetWeight() >= DWRITE_FONT_WEIGHT_BOLD &&
                   (font->GetSimulations() & DWRITE_FONT_SIMULATIONS_BOLD) == 0;
        font->Release();
    }
    family->Release();
    return designed;
}

void WarnOnMissingParityFonts()
{
    static std::atomic reported{false};
    if (reported.load(std::memory_order_relaxed) || !dwcft::ParityActive() ||
        (!LogEnabled() && !dwcft::GeckoParentProcess())) {
        return;
    }

    const Factories factories = GetFactories();
    if (factories.factory == nullptr) {
        return;   // no factory yet; ask again on the next glyph
    }
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(factories.factory->GetSystemFontCollection(&collection, FALSE)) ||
        collection == nullptr) {
        return;
    }

    std::string missing;
    std::string unmatched;
    std::string regular_only;
    unsigned unmatched_count = 0;
    for (const ParityFamily& family : kParityFamilies) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(family.wide, &index, &exists)) && exists) {
            // Present is not the same as complete. Windows draws bold from a
            // designed face, and a family installed as its regular file alone
            // leaves every bold run to synthesis.
            if (family.windows_bold && !FamilyHasDesignedBold(collection, index)) {
                regular_only += regular_only.empty() ? "" : ", ";
                regular_only += family.name;
            }
            continue;
        }
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += family.name;
        if (family.metric_match == nullptr) {
            // Named in full up to a handful; past that only the count, since
            // a machine missing most of them was never set up for parity.
            if (++unmatched_count <= 6) {
                unmatched += unmatched.empty() ? "" : ", ";
                unmatched += family.name;
            } else if (unmatched_count == 7) {
                unmatched += " and more";
            }
        }
    }
    collection->Release();

    // Exchange last, so a thread that loses the race says nothing rather than
    // repeating the line.
    if (reported.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    if (!regular_only.empty()) {
        LogLine("parity: installed without a bold face, so bold text in them is "
                "synthesized where Windows draws a designed face: %s",
                regular_only.c_str());
    }
    if (missing.empty()) {
        LogLine("parity: every family the parity path names is present");
        return;
    }
    LogLine("parity: not in the system font collection, so these will be "
            "substituted and the result will not match Windows: %s",
            missing.c_str());
    if (unmatched_count != 0 && dwcft::GeckoParentProcess()) {
        (void)std::fprintf(
            stderr,
            "cleartype: %u of the %zu fonts parity needs are not installed "
            "and have no metric-compatible substitute (%s) - text will not "
            "match Windows. Install them, or set CLEARTYPE_FIREFOX=0.\n",
            unmatched_count, std::size(kParityFamilies),
            unmatched.c_str());
    }
}

void ForgetSfntCopiesLocked(FT_Face face)
{
    for (auto it = g_os2_copies.begin(); it != g_os2_copies.end();) {
        it = it->first.face == face ? g_os2_copies.erase(it) : std::next(it);
    }
    for (auto it = g_post_copies.begin(); it != g_post_copies.end();) {
        it = it->first.face == face ? g_post_copies.erase(it) : std::next(it);
    }
}

// The per-size OS/2 copy. sTypoAscender : sTypoDescender is the Windows
// maxAscent : maxDescent ratio, and the line gap is chosen so that
// (sTypoAscender - sTypoDescender + sTypoLineGap) * yScale rounds to the
// Windows emHeight + internalLeading + externalLeading.
void* SubstituteOS2(FT_Face face, void* real_table)
{
    const TT_OS2* os2 = static_cast<TT_OS2*>(real_table);
    if (os2 == nullptr || face->size == nullptr || face->units_per_EM == 0 ||
        !WindowsMetrics()) {
        return real_table;
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (!GetEmSize(face, &em_size, &x_over_y) || !(em_size > 0.0)) {
        return real_table;
    }
    // Resolve the system font collection before locking. Building it under
    // g_faces_mutex deadlocks; see WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    const WinInstance* inst = entry != nullptr ? GetWinInstanceLocked(entry, em_size) : nullptr;
    if (inst == nullptr || !inst->valid) {
        pthread_mutex_unlock(&g_faces_mutex);
        return real_table;
    }
    const WinMetrics& m = inst->metrics;
    const double y_scale = static_cast<double>(face->size->metrics.y_scale) / 65536.0 / 64.0;
    const double target = m.emHeight + m.internalLeading + m.externalLeading;
    const long ascent = llround(m.maxAscent);
    const long descent = llround(m.maxDescent);
    bool done = false;
    TT_OS2* result = nullptr;
    if (y_scale > 0.0 && ascent >= 0 && descent >= 0 && ascent + descent > 0) {
        // Design-unit-sized multiples keep the numbers in the range the
        // table's int16 fields hold; the ratio is what matters.
        long k = static_cast<long>(std::floor(1.0 / y_scale + 0.5));
        if (k < 1) k = 1;
        for (int attempt = 0; attempt < 2 && !done; ++attempt) {
            const long asc_units = ascent * k;
            const long desc_units = descent * k;
            const long total_units = static_cast<long>(std::floor(target / y_scale + 0.5));
            const long gap_units = total_units - (asc_units + desc_units);
            if (asc_units <= 32767 && desc_units <= 32767 && gap_units >= -32768 &&
                gap_units <= 32767) {
                SfntKey key;
                key.face = face;
                key.size_26_6 = inst->size_26_6;
                TT_OS2& copy = g_os2_copies[key];
                CapSfntCopiesLocked(g_os2_copies, key);
                copy = *os2;
                copy.sTypoAscender = static_cast<FT_Short>(asc_units);
                copy.sTypoDescender = static_cast<FT_Short>(-desc_units);
                copy.sTypoLineGap = static_cast<FT_Short>(gap_units);
                copy.fsSelection = static_cast<FT_UShort>(copy.fsSelection & ~static_cast<FT_UShort>(1 << 7));
                result = &copy;
                done = true;
            }
            k = 1;
        }
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return done ? reinterpret_cast<void*>(result) : real_table;
}

// gfxFT2FontBase::InitMetrics' own external leading, which is what sits in the
// struct unless gfxFont::SanitizeMetrics replaced it from a line-gap-override
// descriptor. Every input is one this library set, so the answer is exact:
// the size metrics come from ApplyWindowsMetrics and the OS/2 table from
// SubstituteOS2. Called with g_faces_mutex held.
//
//   lineHeight   = size->metrics.height, or the typo trio when OS/2 has one
//   emHeight     = floor(units_per_EM * yScale + 0.5)
//   internal     = floor(maxHeight - emHeight + 0.5)
//   lineHeight   = floor(max(lineHeight, maxHeight) + 0.5)
//   external     = lineHeight - internal - emHeight
double LinuxExternalLeadingLocked(FT_Face face, const WinInstance& inst)
{
    const FT_Size_Metrics& sm = face->size->metrics;
    const double y_scale = static_cast<double>(sm.y_scale) / 65536.0 / 64.0;
    double max_ascent = static_cast<double>(sm.ascender) / 64.0;
    double max_descent = -static_cast<double>(sm.descender) / 64.0;
    double line_height = static_cast<double>(sm.height) / 64.0;
    const double em_scaled = static_cast<double>(face->units_per_EM) * y_scale;

    // The table Gecko read: SubstituteOS2' copy for this size when it made one,
    // and the face's own otherwise.
    const TT_OS2* os2 = nullptr;
    SfntKey key;
    key.face = face;
    key.size_26_6 = inst.size_26_6;
    const auto found = g_os2_copies.find(key);
    if (found != g_os2_copies.end()) {
        os2 = &found->second;
    } else if (ft_get_sfnt_table_fn real = real_FT_Get_Sfnt_Table()) {
        os2 = static_cast<const TT_OS2*>(real(face, FT_SFNT_OS2));
    }
    if (os2 != nullptr && os2->sTypoAscender != 0 && y_scale > 0.0) {
        const double em_ascent = static_cast<double>(os2->sTypoAscender) * y_scale;
        const double em_descent = -static_cast<double>(os2->sTypoDescender) * y_scale;
        line_height = static_cast<double>(os2->sTypoAscender - os2->sTypoDescender +
                                          os2->sTypoLineGap) * y_scale;
        constexpr FT_UShort kUseTypoMetrics = 1 << 7;
        if ((os2->fsSelection & kUseTypoMetrics) != 0 ||
            (max_ascent == 0.0 && max_descent == 0.0)) {
            max_ascent = static_cast<double>(NSlround(em_ascent));
            max_descent = static_cast<double>(NSlround(em_descent));
        }
    }
    const double max_height = max_ascent + max_descent;
    const double em_height = std::floor(em_scaled + 0.5);
    const double internal = std::floor(max_height - em_height + 0.5);
    line_height = std::floor(std::max(line_height, max_height) + 0.5);
    return line_height - internal - em_height;
}

// The per-size post copy, carrying the Windows underlineOffset as Linux reads
// it: InitMetrics takes post->underlinePosition * yScale when the face has
// underline metrics and the value is non-zero. For a bad-underline family the
// value wanted is the one gfxFont::SanitizeMetrics produces on Windows with
// aIsBadUnderlineFont, and InitMetrics' own SanitizeMetrics(false) is run over
// the candidate here to confirm it survives; when it does not, the table is
// left alone.
void* SubstitutePost(FT_Face face, void* real_table)
{
    const TT_Postscript* post = static_cast<TT_Postscript*>(real_table);
    if (post == nullptr || face->size == nullptr || !WindowsMetrics() ||
        face->underline_position == 0 || face->underline_thickness == 0) {
        return real_table;
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (!GetEmSize(face, &em_size, &x_over_y) || !(em_size > 0.0)) {
        return real_table;
    }
    // Resolve the system font collection before locking. Building it under
    // g_faces_mutex deadlocks; see WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    const WinInstance* inst = entry != nullptr ? GetWinInstanceLocked(entry, em_size) : nullptr;
    if (inst == nullptr || !inst->valid) {
        pthread_mutex_unlock(&g_faces_mutex);
        return real_table;
    }
    const WinMetrics& m = inst->metrics;
    const double y_scale = static_cast<double>(face->size->metrics.y_scale) / 65536.0 / 64.0;
    void* result = real_table;
    if (y_scale > 0.0) {
        const long candidate = lround(m.underlineOffset / y_scale);
        if (candidate != 0 && candidate >= -32768 && candidate <= 32767) {
            // gfxFT2FontBase::InitMetrics followed by SanitizeMetrics(false),
            // over the Linux maxAscent/maxDescent, which ApplyWindowsMetrics
            // made equal to the Windows ones.
            WinMetrics on_linux = m;
            if (inst->descent_fold) {
                on_linux.maxAscent -= 0.5;
                on_linux.maxDescent += 0.5;
            }
            on_linux.underlineOffset = static_cast<double>(candidate) * y_scale;
            on_linux.underlineSize = static_cast<double>(face->underline_thickness) * y_scale;
            WinSanitizeMetrics(&on_linux, false);
            if (std::fabs(on_linux.underlineOffset - m.underlineOffset) < 1e-3 &&
                std::fabs(on_linux.underlineSize - m.underlineSize) < 1e-3) {
                SfntKey key;
                key.face = face;
                key.size_26_6 = inst->size_26_6;
                TT_Postscript& copy = g_post_copies[key];
                CapSfntCopiesLocked(g_post_copies, key);
                copy = *post;
                copy.underlinePosition = static_cast<FT_Short>(candidate);
                result = &copy;
            } else if (inst->bad_underline) {
                LogLine("face %p at %.4fpx: bad-underline offset %g not reproducible "
                        "(InitMetrics would clamp it to %g)",
                        reinterpret_cast<void*>(face), em_size, m.underlineOffset, on_linux.underlineOffset);
            }
        }
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return result;
}

#else  // !CLEARTYPE_FIREFOX_PARITY

inline void ApplyWindowsMetrics(FT_Face, const char*) {}
inline void* SubstituteOS2(FT_Face, void* real_table) { return real_table; }
inline void* SubstitutePost(FT_Face, void* real_table) { return real_table; }
void ForgetSfntCopiesLocked(FT_Face) {}

#endif

#if CLEARTYPE_FIREFOX_PARITY

// The glyph ApplyWindowsAdvance last answered for on this thread, so that the
// embolden strength computed for it right afterwards can be recognized. See
// ApplyWindowsBoldAdvance.
struct PendingAdvance
{
    FT_Face face;
    FT_UInt glyph_index;
};
thread_local PendingAdvance g_pending_advance = {};

// The horizontal advance Firefox on Windows hands to HarfBuzz for one glyph,
// in 16.16 (hb_position_t), written into glyph->linearHoriAdvance because
// that is the field gfxFT2FontBase::GetFTGlyphExtents reads on Linux when
// ShouldRoundXOffset() is false (the windows-parity fontconfig turns hinting
// off) and returns as the 16.16 advance that gfxFT2FontBase::GetGlyphWidth
// hands to HarfBuzz in turn.
//
// The Windows producer is one of two arithmetics, selected by
// gfxDWriteFont::ProvidesGlyphWidths():
//
//     return !mUseSubpixelPositions ||
//            (mFontFace->GetSimulations() & DWRITE_FONT_SIMULATIONS_BOLD) ||
//            ((gfxDWriteFontEntry*)(GetFontEntry()))->HasVariations();
//
//   true:  gfxDWriteFont::GetGlyphWidth
//            NS_lround(MeasureGlyphWidth(aGID) * 65536.0)
//          gfxDWriteFont::MeasureGlyphWidth
//            mUseSubpixelPositions ? advance * mFUnitsConvFactor  (float)
//            : NS_lround(gdiCompatibleAdvance * mFUnitsConvFactor)
//   false: gfxHarfBuzzShaper::GetGlyphHAdvanceUncached
//            FloatToFixed(mFont->FUnitsToDevUnitsFactor() * advanceWidth)
//          with #define FloatToFixed(f) (65536 * (f)), evaluated in float and
//          truncated to hb_position_t.
//
// The synthetic-bold simulation term cannot be known at this point. Linux
// decides synthetic bold in the FcPattern, which FreeType never sees, and
// gfxFontconfigFontEntry::CreateFontInstance hands every instance of a family
// the one SharedFTFace the entry holds, so nothing about the load says which
// instance made it. It is taken as false here and corrected in
// ApplyWindowsBoldAdvance, which runs late enough to know.
// The ink box of one glyph in pixels, built the way
// gfx/thebes/gfxDWriteFonts.cpp gfxDWriteFont::GetGlyphBounds builds it:
//
//     bounds = (leftSideBearing, topSideBearing - verticalOriginY,
//               advanceWidth - leftSideBearing - rightSideBearing,
//               advanceHeight - topSideBearing - bottomSideBearing)
//     bounds.Scale(mFUnitsConvFactor)
//
// Design units throughout, so no rasterizer has touched it. FreeType instead
// reports the control box of the scaled outline, which encloses the Bezier
// control points as well as the curve itself and so is never smaller.
struct WinInkBox
{
    double left, top, right, bottom;
};

bool WinGlyphInkBox(IDWriteFontFace* dwrite_face, const float funits_conv, const UINT16 glyph,
                    WinInkBox* box)
{
    DWRITE_GLYPH_METRICS gm{};
    if (dwrite_face == nullptr || FAILED(dwrite_face->GetDesignGlyphMetrics(&glyph, 1, &gm))) {
        return false;
    }
    const double conv = static_cast<double>(funits_conv);
    const auto width = static_cast<INT32>(gm.advanceWidth);
    const auto height = static_cast<INT32>(gm.advanceHeight);
    box->left = static_cast<double>(gm.leftSideBearing) * conv;
    box->top = static_cast<double>(gm.verticalOriginY - gm.topSideBearing) * conv;
    box->right = static_cast<double>(width - gm.rightSideBearing) * conv;
    box->bottom = static_cast<double>(height - gm.verticalOriginY - gm.bottomSideBearing) * conv;
    return true;
}

// Puts that box into the slot, in the shape gfxFT2FontBase::GetFTGlyphExtents
// reads it back out of:
//
//     x = horiBearingX;      x2 = x + width
//     y = -horiBearingY;     y2 = y + height
//
// so the height carries the bearing with it.
// The four edges as 26.6, and whether the box is one the slot will take. An
// empty box is how GetFTGlyphExtents is told to fall back to the font-wide
// ascent and descent for a color glyph with no outline, so a degenerate one is
// left out of the slot.
bool InkBox26Dot6(const WinInkBox& box, FT_Pos* x, FT_Pos* x2, FT_Pos* y, FT_Pos* y2)
{
    *x = static_cast<FT_Pos>(llround(box.left * 64.0));
    *x2 = static_cast<FT_Pos>(llround(box.right * 64.0));
    *y = static_cast<FT_Pos>(llround(box.top * 64.0));
    *y2 = static_cast<FT_Pos>(llround(box.bottom * 64.0));
    return *x2 > *x && *y + *y2 > 0;
}

void SetGlyphInkBox(FT_GlyphSlot slot, const WinInkBox& box)
{
    FT_Pos x = 0, x2 = 0, y = 0, y2 = 0;
    if (InkBox26Dot6(box, &x, &x2, &y, &y2)) {
        slot->metrics.horiBearingX = x;
        slot->metrics.width = x2 - x;
        slot->metrics.horiBearingY = y;
        slot->metrics.height = y + y2;
    }
}

void ApplyWindowsAdvance(FT_Face face)
{
    if (face == nullptr || face->glyph == nullptr || !FT_IS_SCALABLE(face) ||
        face->units_per_EM == 0 || face->size == nullptr || !WindowsMetrics()) {
        return;
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (!GetEmSize(face, &em_size, &x_over_y) || !(em_size > 0.0)) {
        return;
    }

    double adjusted = 0.0;
    if (!AdjustedSizeForFace(face, MulFix(face->units_per_EM, face->size->metrics.y_scale),
                             &adjusted)) {
        adjusted = 0.0;
    }

    // Resolve the system font collection before locking. Building it under
    // g_faces_mutex deadlocks; see WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    const WinInstance* instance =
        entry != nullptr ? GetWinInstanceLocked(entry, em_size) : nullptr;
    if (instance == nullptr || !instance->valid || instance->dwrite_face == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return;
    }
    const UINT16 glyph = static_cast<UINT16>(face->glyph->glyph_index);
    double advance_px = WinGlyphAdvance(*instance, glyph, FT_HAS_MULTIPLE_MASTERS(face) != 0);
    WinInkBox box{};
    const bool have_box = face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
                          WinGlyphInkBox(instance->dwrite_face, instance->funits_conv, glyph, &box);
    // The advance Windows measures at the size the font is really drawing at,
    // put where GetFTGlyphExtents' own scaling brings it back. The ink box
    // above stays the one for the face's own size, which that scaling is right
    // for, and is read first: a second instance moves the vector holding it.
    double back_to_ft = 1.0;
    if (adjusted > 0.0) {
        const WinInstance* at_adjusted = GetWinInstanceLocked(entry, adjusted);
        if (at_adjusted != nullptr && at_adjusted->valid && at_adjusted->dwrite_face != nullptr) {
            const double at = WinGlyphAdvance(*at_adjusted, glyph, FT_HAS_MULTIPLE_MASTERS(face) != 0);
            if (at >= 0.0) {
                advance_px = at;
                back_to_ft = em_size / adjusted;
            }
        }
    }
    pthread_mutex_unlock(&g_faces_mutex);
    if (have_box) {
        SetGlyphInkBox(face->glyph, box);
    }
    // Both arithmetics leave a whole number of 1/65536 px. The scaling back is
    // applied to the finished 16.16, so that GetFTGlyphExtents' rounding of the
    // product lands on the value Windows holds rather than beside it.
    face->glyph->linearHoriAdvance =
        static_cast<FT_Fixed>(llround(static_cast<double>(llround(advance_px * 65536.0)) * back_to_ft));
    g_pending_advance.face = face;
    g_pending_advance.glyph_index = face->glyph->glyph_index;
}

// The advance for a synthetically bold instance, which the load above could
// not recognize.
//
// gfx/thebes/gfxFT2FontBase.cpp gfxFT2FontBase::GetEmboldenStrength returns a
// zero strength and calls nothing when the instance is not emboldened, so an
// FT_MulFix of the face's own em size by its y_scale, arriving on this thread
// with the glyph still in the slot, is the load being answered for an
// emboldened one. gfxFT2FontBase::GetFTGlyphExtents asks for the strength
// before it reads the advance:
//
//     FT_Vector bold = GetEmboldenStrength(face.get());
//     advance = face.get()->glyph->linearHoriAdvance;
//     if (advance) { advance += bold.x << 10; }
//
// so what belongs in the field is the Windows advance less the strength that
// is about to be added to it. Firefox on Windows draws these faces through
// DirectWrite's bold simulation, not the multi-strike synthetic bold
// gfxFont::PostShapingFixup applies, so the Windows advance is the one
// measured through the simulated face.
//
// The other callers of FT_MulFix in libxul that pass these two arguments,
// WebRender's mozilla_glyphslot_embolden_less and Skia's FreeType port, are
// computing the same strength for the same reason. The one that passes
// something else, gfxFT2FontBase.cpp's ScaleRoundDesignUnits, scales an OS/2
// design metric and runs before InitMetrics loads any glyph.
void ApplyWindowsBoldAdvance(const FT_Long a, const FT_Long b, const FT_Long product)
{
    FT_Face face = g_pending_advance.face;
    g_pending_advance.face = nullptr;
    if (face == nullptr) {
        return;
    }
    // Nothing below may dereference `face` until it is known to still be one
    // of ours. The record belongs to this thread, and nothing clears it when
    // FT_Done_Face destroys the face it names. The next FT_MulFix here can
    // arrive much later and belong to another face, which
    // gfxFT2FontBase::InitMetrics does through ScaleRoundDesignUnits on every
    // thread that measures a font, so the fields read below would be freed
    // memory. The face table is the authority on liveness, and FindFaceLocked
    // compares pointers without following them.
    pthread_mutex_lock(&g_faces_mutex);
    const bool face_is_live = FindFaceLocked(face) != nullptr;
    pthread_mutex_unlock(&g_faces_mutex);
    if (!face_is_live || face->glyph == nullptr || face->size == nullptr ||
        face->glyph->glyph_index != g_pending_advance.glyph_index ||
        a != static_cast<FT_Long>(face->units_per_EM) || b != face->size->metrics.y_scale) {
        return;
    }

    // What GetEmboldenStrength returns, which is half of
    // FT_GlyphSlot_Embolden's strength for an outline, matching how much
    // WebRender emboldens outlines, and a whole number of pixels for an
    // embedded bitmap.
    FT_Pos strength = 0;
    if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        strength = product / 48;
    } else {
        strength = product / 24;
        if (face->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
            strength &= -64;
            if (strength == 0) {
                strength = 64;
            }
        }
    }

    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (!GetEmSize(face, &em_size, &x_over_y) || !(em_size > 0.0)) {
        return;
    }
    // See ApplyWindowsAdvance. The tracking Windows adds for a synthetic bold
    // has a floor, so the advance under a pixel is not the advance at the
    // face's own size scaled down.
    double adjusted = 0.0;
    if (!AdjustedSizeForFace(face, MulFix(face->units_per_EM, face->size->metrics.y_scale),
                             &adjusted)) {
        adjusted = 0.0;
    }
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    const WinInstance* instance =
        entry != nullptr ? GetWinInstanceLocked(entry, em_size) : nullptr;
    double advance_px = -1.0;
    double win_ink_top = 0.0;
    double win_ink_right = 0.0;
    bool have_win_ink = false;
    bool win_fattens_outline = true;
    double extents_scale = 1.0;
    if (instance != nullptr && instance->valid && instance->dwrite_face != nullptr) {
        // The two cases WinBoldGlyphAdvanceLocked answers for by tracking.
        // gfxDWriteFontEntry::CreateFontInstance keeps DirectWrite's simulation
        // away from a webfont and from a COLR font, so their outlines are drawn
        // at their plain weight and only the advance moves.
        win_fattens_outline = !(entry->memory != nullptr || FaceHasCOLRLocked(entry));
        if (win_fattens_outline) {
            // The bool is the answer here. A glyph whose ink is all below
            // the baseline, like the underscore, has a negative top and is
            // still a glyph Windows fattens.
            have_win_ink =
                WinBoldGlyphInkLocked(entry, *instance,
                                      static_cast<UINT16>(face->glyph->glyph_index),
                                      &win_ink_top, &win_ink_right);
        }
        advance_px = WinBoldGlyphAdvanceLocked(entry, *instance,
                                               static_cast<UINT16>(face->glyph->glyph_index),
                                               FT_HAS_MULTIPLE_MASTERS(face) != 0);
        if (adjusted > 0.0) {
            const WinInstance* at_adjusted = GetWinInstanceLocked(entry, adjusted);
            if (at_adjusted != nullptr && at_adjusted->valid &&
                at_adjusted->dwrite_face != nullptr) {
                const double at =
                    WinBoldGlyphAdvanceLocked(entry, *at_adjusted,
                                              static_cast<UINT16>(face->glyph->glyph_index),
                                              FT_HAS_MULTIPLE_MASTERS(face) != 0);
                if (at >= 0.0) {
                    advance_px = at;
                    extents_scale = em_size / adjusted;
                }
            }
        }
    }
    pthread_mutex_unlock(&g_faces_mutex);

    // gfxFT2FontBase::GetFTGlyphExtents grows the glyph box by this same
    // strength for every emboldened instance:
    //
    //     y  = -horiBearingY;  y2 = y + height;  y -= bold.y;
    //     x2 = horiBearingX + width;             x2 += bold.x;
    //
    // Where Windows leaves the outline alone there is nothing to grow, so the
    // three fields it reads are taken down by the strength first and the two
    // additions net to nothing. y2 is read before y moves, which is why the
    // height comes down with the bearing instead of up.
    if (!win_fattens_outline && strength > 0 &&
        face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_Glyph_Metrics& gm = face->glyph->metrics;
        gm.horiBearingY -= strength;
        gm.height -= strength;
        gm.width -= strength;
    } else if (have_win_ink && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        // Where Windows does fatten the outline, the glyph top it reports is
        // the one DirectWrite's simulated face carries, and GetFTGlyphExtents
        // reaches its own by adding the strength to a bearing FreeType already
        // rounded. Two roundings land it one or two 26.6 steps away. Placing
        // the bearing so that the sum is the nearest step to the Windows value
        // leaves at most half a step. y2 is read before y moves, so the height
        // follows the bearing to hold the descent still.
        FT_Glyph_Metrics& gm = face->glyph->metrics;
        const FT_Pos want = static_cast<FT_Pos>(llround(win_ink_top * 64.0)) - strength;
        const FT_Pos delta = want - gm.horiBearingY;
        gm.horiBearingY += delta;
        gm.height += delta;
        // The right edge the same way. GetFTGlyphExtents reads
        // x2 = horiBearingX + width and then adds the strength, so the width
        // is placed to land that sum on the Windows edge. The bearing is left
        // where it is, since the left edge is not one of the two the
        // emboldening moves.
        const FT_Pos want_x2 = static_cast<FT_Pos>(llround(win_ink_right * 64.0)) - strength;
        if (want_x2 > gm.horiBearingX) {
            gm.width = want_x2 - gm.horiBearingX;
        }
    }
    if (advance_px < 0.0) {
        return;
    }
    face->glyph->linearHoriAdvance =
        static_cast<FT_Fixed>(
            llround(static_cast<double>(llround(advance_px * 65536.0)) * extents_scale)) -
        (static_cast<FT_Fixed>(strength) << 10);
}

#else  // !CLEARTYPE_FIREFOX_PARITY

inline void ApplyWindowsAdvance(FT_Face) {}
inline void ApplyWindowsBoldAdvance(FT_Long, FT_Long, FT_Long) {}

#endif

// The table is built once in InitOptions, not lazily here: the gamma is
// fixed at startup, and a lazily filled cache would be written from whatever
// thread rendered first while others read it.
void ApplyAlphaGamma(std::vector<BYTE>& texture, const Options& options)
{
    if (!options.alpha_gamma_enabled) {
        return;
    }
    for (BYTE& value : texture) {
        value = options.alpha_lut[value];
    }
}

// Installs `texture` into `slot` as FreeType's own FT_Render_Glyph would
// leave it: bitmap, bitmap_left, bitmap_top and format set, nothing else
// touched. `channels` 3 is FT_PIXEL_MODE_LCD, 1 is FT_PIXEL_MODE_GRAY.
// Returns false without modifying the slot if anything fails.
bool InstallBitmap(FT_GlyphSlot slot, const std::vector<BYTE>& texture, const int channels, const int width,
                   const int height, const int left, const int top)
{
    // FreeType pads rows to a multiple of 4 bytes.
    const size_t row_bytes = static_cast<size_t>(width) * static_cast<size_t>(channels);
    // Parenthesized because -Wparentheses asks for it: the addition inside a
    // bitwise operand is the shape that is usually a precedence mistake.
    // ReSharper disable once CppRedundantParentheses
    const size_t pitch = (row_bytes + 3) & ~static_cast<size_t>(3);
    const size_t size = pitch * static_cast<size_t>(height);
    if (texture.size() < row_bytes * static_cast<size_t>(height)) {
        return false;
    }

    auto* buffer = static_cast<unsigned char*>(std::calloc(1, size));
    if (buffer == nullptr) {
        return false;
    }
    for (int y = 0; y < height; ++y) {
        std::memcpy(buffer + static_cast<size_t>(y) * pitch, texture.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
    }

    FreeTrackedBuffer(slot);  // whatever this shim installed on the previous render

    slot->bitmap.rows = static_cast<unsigned int>(height);
    slot->bitmap.width = static_cast<unsigned int>(row_bytes);
    slot->bitmap.pitch = static_cast<int>(pitch);
    slot->bitmap.buffer = buffer;
    slot->bitmap.num_grays = 256;
    slot->bitmap.pixel_mode = channels == 1 ? FT_PIXEL_MODE_GRAY : FT_PIXEL_MODE_LCD;
    slot->bitmap.palette_mode = 0;
    slot->bitmap.palette = nullptr;

    slot->bitmap_left = left;
    slot->bitmap_top = top;
    slot->format = FT_GLYPH_FORMAT_BITMAP;

    TrackBuffer(slot, buffer);
    return true;
}

#if CLEARTYPE_FIREFOX_PARITY

enum class RasterCaller { WebRender, Skia };

// Which Firefox rasterizer is calling, from the thread's name
// (firefox_parity_data.h kWebRenderThreadPrefixes: the rayon pool
// gfx/webrender_bindings/src/bindings.rs names "WRWorker<tag>#<n>", the
// GlyphRasterThread it registers as "WrGlyphRasterizer", and the
// "WRRenderBackend#<n>" thread gfx/wr/wr_glyph_rasterizer/src/rasterizer.rs
// request_glyphs rasterizes on inline). Every other FreeType
// render in Firefox comes through Skia's cairo-FT port
// (gfx/skia/skia/src/ports/SkFontHost_cairo.cpp) - canvas and the other
// DrawTargetSkia consumers - whose Windows counterpart is
// gfx/skia/skia/src/ports/SkScalerContext_win_dw.cpp.
// The name of this thread, or nullptr if it was never named.
//
// Not prctl(PR_GET_NAME): a Firefox content process is exactly where this has
// to work and exactly where that syscall is refused. Gecko's seccomp policy
// (security/sandbox/linux/SandboxFilter.cpp, SandboxPolicyCommon::PrctlPolicy)
// allows PR_SET_VMA, PR_GET_SECCOMP, PR_SET_NAME, PR_SET_DUMPABLE and
// PR_SET_PTRACER, answers PR_CAPBSET_READ with EINVAL, and sends everything
// else to Default(InvalidSyscall()) - which is a reported sandbox violation.
// PR_GET_NAME is allowed only under MOZ_PULSEAUDIO BelowLevel(4), which the
// shipping level is not.
//
// PR_SET_NAME is allowed, though, so the name arrives here on its way in:
// pthread_setname_np is interposed below and records what it was given. Gecko
// names its threads through PR_SetCurrentThreadName and WebRender's rayon pool
// through Rust's Builder::name, both of which land there. A thread nobody
// named has no name to match, which is the same answer PR_GET_NAME would have
// produced for it.

// How many Skia scaler calls this thread is inside.
//
// The hooks over SkScalerContext's generateImage, generatePath and
// generateMetrics bracket their calls with this, so a FreeType render reached
// from one belongs to Skia whatever the thread is named. A blob image is Skia
// drawing on a WebRender worker, where the name says WebRender and the
// library says nothing until an A8 glyph has gone through it.
thread_local int g_in_skia_scaler = 0;

extern "C" void CleartypeEnterSkiaScaler(void) { ++g_in_skia_scaler; }

extern "C" void CleartypeLeaveSkiaScaler(void)
{
    if (g_in_skia_scaler > 0) {
        --g_in_skia_scaler;
    }
}

RasterCaller CurrentRasterCaller()
{
    // Outside Gecko there is no Firefox rasterizer to be, and answering Skia -
    // which is what an unrecognized thread name means - would switch on the
    // paths that reproduce one. WebRender is the answer that leaves them off.
    if (!dwcft::ParityActive()) {
        return RasterCaller::WebRender;
    }
    // A scaler on the stack outranks the thread's name.
    if (g_in_skia_scaler > 0) {
        return RasterCaller::Skia;
    }

    thread_local int cached = -1;
    if (cached < 0) {
        const char* name = ThisThreadName();
        // Nothing has named this thread yet. Gecko names its threads on the
        // way in, through PR_SetCurrentThreadName and rayon's Builder::name,
        // so a name can still arrive; answering Skia now and keeping that
        // answer would give the wrong rasterizer's parameters to a thread that
        // is about to say it is WebRender's. Answer, but do not remember.
        if (name == nullptr) {
            return RasterCaller::Skia;
        }
        cached = 0;
        for (const char* prefix : firefox_parity::kWebRenderThreadPrefixes) {
            if (std::strncmp(name, prefix, 7) == 0) {
                cached = 1;
            }
        }
    }
    return cached == 1 ? RasterCaller::WebRender : RasterCaller::Skia;
}

// True on a thread WebRender rasterizes blob images on, which is where a Skia
// A8 glyph belongs to the blob path rather than to a DrawTargetSkia consumer.
//
// gfx/webrender_bindings/src/moz2d_renderer.rs rasterize_blob runs either on
// the rayon pool ("WRWorker<tag>#<n>") or, when there are too few blobs to be
// worth installing a job, serially on the thread that asked, which is the
// scene builder ("WRSceneBuilder<tag>"). Both were seen answering for the same
// page. Nothing else in Gecko draws Skia glyphs on those threads.
//
// The FreeType library cannot separate these two callers the way it separates
// Skia from WebRender: both are Skia and both use Skia's library. The thread
// is the only signal there is, and it is the subsystem's own name for itself,
// so it says which of Gecko's rasterizers is running and not merely where.
bool OnBlobRasterThread()
{
    static constexpr const char* kBlobThreadPrefixes[] = { "WRWorke", "WRScene", "WRRende" };
    const char* name = ThisThreadName();
    if (name == nullptr) {
        return false;
    }
    for (const char* prefix : kBlobThreadPrefixes) {
        if (std::strncmp(name, prefix, 7) == 0) {
            return true;
        }
    }
    return false;
}

// The same question for one face, which the thread cannot always answer.
//
// Gecko rasterizes a blob image through DrawTargetSkia on WebRender's own
// rayon workers, so a thread named WRWorker is not proof that WebRender is the
// caller, and those glyphs were being given platform/windows/font.rs's
// DirectWrite parameters where Windows gives them SkScalerContext_win_dw's.
//
// The FreeType library settles it. WebRender calls FT_Init_FreeType for itself
// in FontContext::new (platform/unix/font.rs), so the two rasterizers never
// share one, and FT_Outline_Get_Bitmap has no caller but Skia
// (SkScalerContextFTUtils::generateGlyphImage). A library seen there is Skia's
// for the life of the process, and face->glyph->library says which one a face
// belongs to. FT_Render_Glyph cannot be used the same way, since Skia calls it
// too for SkMask::kLCD16_Format.
//
// Slots rather than a map, since a process has one library per rasterizer and
// this is read on every glyph. An unclaimed library leaves the answer to the
// thread name, which is where it was before.
//
// A library is claimed only once a glyph has been drawn through it. Until then
// a blob rasterized on one of WebRender's own workers answers WebRender, and
// for a CJK face carrying an embedded strike at the size in use that draws the
// strike where SkScalerContext_win_dw draws the outline, so the run comes out
// bilevel where Windows antialiases it. A color glyph in the same text run is
// what sends the run through a blob at all, so a plain CJK run is unaffected.
// Each process claims its own libraries.
constexpr int kSkiaLibrarySlots = 4;
std::atomic<void*> g_skia_libraries[kSkiaLibrarySlots];

void RecordSkiaLibrary(FT_Library library)
{
    if (library == nullptr || !dwcft::ParityActive()) {
        return;
    }
    auto* const handle = static_cast<void*>(library);
    for (std::atomic<void*>& slot : g_skia_libraries) {
        void* held = slot.load(std::memory_order_acquire);
        // A lost race leaves the winner's handle in `held`, so the same value
        // arriving on two threads still claims one slot between them.
        if (held == nullptr) {
            slot.compare_exchange_strong(held, handle, std::memory_order_acq_rel);
            held = slot.load(std::memory_order_acquire);
        }
        if (held == handle) {
            return;
        }
    }
}

// A library that has been destroyed belongs to nobody, and there are four
// slots. Left claimed, four libraries coming and going over a long-lived
// process would fill the table, and every library opened after that would be
// taken for WebRender's however many glyphs Skia drew through it. The address
// can also be handed out again by the next FT_Init_FreeType.
void ForgetSkiaLibrary(FT_Library library)
{
    if (library == nullptr) {
        return;
    }
    auto* const handle = static_cast<void*>(library);
    for (std::atomic<void*>& slot : g_skia_libraries) {
        void* held = handle;
        if (slot.compare_exchange_strong(held, nullptr, std::memory_order_acq_rel)) {
            return;
        }
    }
}

bool IsSkiaLibrary(FT_Library library)
{
    if (library == nullptr) {
        return false;
    }
    auto* const handle = static_cast<void*>(library);
    for (const std::atomic<void*>& slot : g_skia_libraries) {
        if (slot.load(std::memory_order_acquire) == handle) {
            return true;
        }
    }
    return false;
}

RasterCaller CallerForFace(FT_Face face)
{
    if (face != nullptr && face->glyph != nullptr && IsSkiaLibrary(face->glyph->library)) {
        return RasterCaller::Skia;
    }
    return CurrentRasterCaller();
}

bool FaceRastersThroughSkia(FT_Face face) { return CallerForFace(face) == RasterCaller::Skia; }

// SkScalerContext_win_dw.cpp get_gasp_range / is_gridfit_only / is_hinted /
// has_bitmap_strike.
struct GaspRange
{
    int min;
    int max;
    int version;
    uint16_t flags;
};

bool GetGaspRange(FT_Face face, const int size, GaspRange* range)
{
    std::vector<FT_Byte> gasp;
    if (!ReadSfntTable(face, FT_MAKE_TAG('g', 'a', 's', 'p'), &gasp) || gasp.size() < 4) {
        return false;
    }
    const uint16_t version = ReadU16(gasp, 0);
    if (version != 0 && version != 1) {
        return false;
    }
    const uint16_t num_ranges = ReadU16(gasp, 2);
    if (num_ranges > 1024 || gasp.size() < 4 + static_cast<size_t>(num_ranges) * 4) {
        return false;
    }
    int min_ppem = -1;
    for (uint16_t i = 0; i < num_ranges; ++i) {
        const int max_ppem = ReadU16(gasp, 4 + static_cast<size_t>(i) * 4);
        if (min_ppem < size && size <= max_ppem) {
            range->min = min_ppem + 1;
            range->max = max_ppem;
            range->version = version;
            range->flags = ReadU16(gasp, 4 + static_cast<size_t>(i) * 4 + 2);
            return true;
        }
        min_ppem = max_ppem;
    }
    return false;
}

bool GaspIsGridfitOnly(const uint16_t flags) { return flags == 0x0001; }
constexpr uint16_t kGaspSymmetricSmoothing = 0x0008;

bool FaceIsHintedLocked(FaceEntry* entry)
{
    if (entry->is_hinted < 0) {
        entry->is_hinted = 0;
        std::vector<FT_Byte> maxp;
        // SkOTTableMaximumProfile::Version::TT is 32 bytes; maxSizeOfInstructions
        // sits at offset 26.
        if (ReadSfntTable(entry->face, FT_MAKE_TAG('m', 'a', 'x', 'p'), &maxp) &&
            maxp.size() >= 32 && ReadU32(maxp, 0) == 0x00010000 && ReadU16(maxp, 26) != 0) {
            entry->is_hinted = 1;
        }
    }
    return entry->is_hinted != 0;
}

// Which of the two routes a blob glyph takes on Windows.
//
// SkScalerContext_DW::generateMetrics sets ScalerContextBits::DW and draws
// from DirectWrite's mask whenever generateDWMetrics answers, and reaches
// ScalerContextBits::PATH only where GetAlphaTextureBounds gives nothing
// back. Hinting does not pick the route: is_hinted picks the rendering mode,
// which SkiaDWParams answers.
bool BlobPrefersDWriteMask(FT_Face face)
{
    if (face == nullptr || face->size == nullptr || face->units_per_EM == 0) {
        return false;
    }
    return true;
}

bool HasBitmapStrikeInRangeLocked(FaceEntry* entry, const GaspRange& range)
{
    ReadStrikesLocked(entry);
    for (const EblcStrike& strike : entry->eblc) {
        if (strike.ppem_x == strike.ppem_y && static_cast<int>(strike.ppem_x) >= range.min &&
            static_cast<int>(strike.ppem_x) <= range.max && strike.last >= static_cast<uint32_t>(strike.first) + 3) {
            return true;
        }
    }
    for (const EbscStrike& strike : entry->ebsc) {
        if (strike.ppem_x == strike.ppem_y && static_cast<int>(strike.ppem_x) >= range.min &&
            static_cast<int>(strike.ppem_x) <= range.max) {
            return true;
        }
    }
    return false;
}

// The rendering decisions of SkScalerContext_DW::SkScalerContext_DW, for a
// DWriteFontTypeface that gfx/2d/ScaledFontDWrite.cpp CreateSkTypeface built
// with the system rendering mode, and an SkFont that
// ScaledFontDWrite::SetupSkFontDrawOptions gave embedded bitmaps when
// UseEmbeddedBitmaps() and whose hinting stays kNormal
// (DWriteFontTypeface::onFilterRec). `real_size` is scale.fY.
struct SkiaDWParams
{
    DWRITE_RENDERING_MODE rendering_mode;
    DWRITE_MEASURING_MODE measuring_mode;
    float size_render;
    DWRITE_GRID_FIT_MODE grid_fit_mode;
    DWRITE_TEXT_ANTIALIAS_MODE antialias_mode;
    DWRITE_TEXTURE_TYPE texture_type;
    bool use_factory2;
};

SkiaDWParams SkiaDWParamsLocked(FaceEntry* entry, const IDWriteFontFace* dwrite_face, const float real_size,
                                const bool a8, const bool embedded_bitmaps, const bool axis_aligned,
                                const bool have_factory2)
{
    SkiaDWParams p;
    // SkScalarRoundToScalar(realTextSize * 64.0f) / 64.0f
    float gdi_size = std::floor(real_size * 64.0f + 0.5f) / 64.0f;
    if (gdi_size == 0.0f) {
        gdi_size = 1.0f;
    }

    bool treat_like_bitmap = false;
    if (embedded_bitmaps) {
        const int bitmap_ppem = static_cast<int>(gdi_size);
        GaspRange range = { .min = bitmap_ppem, .max = bitmap_ppem, .version = 0, .flags = 0 };
        if (GetGaspRange(entry->face, bitmap_ppem, &range)) {
            if (!GaspIsGridfitOnly(range.flags)) {
                range = GaspRange{ .min = bitmap_ppem, .max = bitmap_ppem, .version = 0, .flags = 0 };
            }
        }
        treat_like_bitmap = HasBitmapStrikeInRangeLocked(entry, range);
    }

    DWRITE_RENDERING_MODE typeface_mode = DWRITE_RENDERING_MODE_DEFAULT;
    if (IDWriteRenderingParams* params = GetDefaultRenderingParams()) {
        typeface_mode = params->GetRenderingMode();
    }

    GaspRange range = { .min = 0, .max = 0xFFFF, .version = 0, .flags = 0 };
    if (treat_like_bitmap && axis_aligned) {
        p.size_render = gdi_size;
        p.rendering_mode = DWRITE_RENDERING_MODE_GDI_CLASSIC;
        p.measuring_mode = DWRITE_MEASURING_MODE_GDI_CLASSIC;
    } else if (treat_like_bitmap) {
        p.size_render = gdi_size;
        p.rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;
        p.measuring_mode = DWRITE_MEASURING_MODE_GDI_CLASSIC;
    } else if (real_size > 20.0f || typeface_mode == DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL ||
               typeface_mode == DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC) {
        p.size_render = real_size;
        p.rendering_mode = typeface_mode == DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL
                               ? DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL
                               : DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;
        p.measuring_mode = DWRITE_MEASURING_MODE_NATURAL;
    } else if (GetGaspRange(entry->face, static_cast<int>(std::floor(gdi_size + 0.5f)), &range) &&
               range.version >= 1) {
        p.size_render = real_size;
        p.rendering_mode = (range.flags & kGaspSymmetricSmoothing) == 0
                               ? DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL
                               : DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;
        p.measuring_mode = DWRITE_MEASURING_MODE_NATURAL;
    } else {
        if (FaceIsHintedLocked(entry)) {
            p.size_render = gdi_size;
            p.rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL;
        } else {
            p.size_render = real_size;
            p.rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;
        }
        p.measuring_mode = DWRITE_MEASURING_MODE_NATURAL;
    }

    p.texture_type = DWRITE_TEXTURE_CLEARTYPE_3x1;
    p.antialias_mode = DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE;
    p.grid_fit_mode = DWRITE_GRID_FIT_MODE_ENABLED;
    if (have_factory2 && a8) {
        // kA8_Format without kGenA8FromLCD_Flag.
        p.texture_type = DWRITE_TEXTURE_ALIASED_1x1;
        p.antialias_mode = DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE;
    }

    if (p.measuring_mode != DWRITE_MEASURING_MODE_NATURAL &&
        FaceHasTable(entry->face, FT_MAKE_TAG('C', 'B', 'D', 'T'))) {
        p.measuring_mode = DWRITE_MEASURING_MODE_NATURAL;
    }
    p.use_factory2 = have_factory2 && (p.grid_fit_mode == DWRITE_GRID_FIT_MODE_DISABLED ||
                                       p.antialias_mode == DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    (void)dwrite_face;
    return p;
}

#endif  // CLEARTYPE_FIREFOX_PARITY

// One glyph, rasterized by DirectWrite, in FreeType's own frame of
// reference: `left` is pixels right of the pen origin, `top` is pixels above
// the baseline, both exactly as FT_GlyphSlot's bitmap_left/bitmap_top mean
// them. `channels` is 3 for a ClearType texture and 1 for coverage.
struct DWriteGlyphImage
{
    std::vector<BYTE> texture;
    int channels = 3;
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
};

#if CLEARTYPE_FIREFOX_PARITY

// gfx/wr/wr_glyph_rasterizer/src/rasterizer.rs FontInstance::get_extra_strikes
// for FontInstanceFlags::MULTISTRIKE_BOLD, with x_scale 1.
size_t WinExtraStrikes(const double size)
{
    double bold_offset = size / 48.0;
    if (bold_offset < 1.0) {
        bold_offset = 0.25 + 0.75 * bold_offset;
    }
    return static_cast<size_t>(std::floor(std::max(bold_offset, 1.0) + 0.5));
}

// rasterizer.rs blend_strike_pixel.
BYTE BlendStrikePixel(const BYTE dest, const uint32_t src, const uint32_t src_alpha)
{
    const uint32_t x = src * 255 + static_cast<uint32_t>(dest) * (255 - src_alpha) + 128;
    return static_cast<BYTE>((x + (x >> 8)) >> 8);
}

// rasterizer.rs apply_multistrike_bold / blend_strike, over the texture as
// platform/windows/font.rs hands it in: each channel is an independent
// weight for a subpixel mask, and for the coverage textures this file
// produces every channel equals the alpha, so the premultiplied arm reduces
// to the same arithmetic.
void ApplyMultistrikeBold(std::vector<BYTE>* texture, int* width, const int height, const int channels,
                          const size_t extra_strikes, const double pixel_step)
{
    const size_t src_width = static_cast<size_t>(*width);
    const size_t extra_width = static_cast<size_t>(std::ceil(static_cast<double>(extra_strikes) * pixel_step));
    const size_t dest_width = src_width + extra_width;
    const size_t src_stride = src_width * static_cast<size_t>(channels);
    const size_t dest_stride = dest_width * static_cast<size_t>(channels);
    std::vector<BYTE> dest(dest_stride * static_cast<size_t>(height), 0);
    for (int y = 0; y < height; ++y) {
        std::memcpy(dest.data() + static_cast<size_t>(y) * dest_stride, texture->data() + static_cast<size_t>(y) * src_stride,
                    src_stride);
    }
    for (size_t i = 1; i <= extra_strikes; ++i) {
        const double offset = static_cast<double>(i) * pixel_step;
        const size_t offset_integer = static_cast<size_t>(std::floor(offset)) * static_cast<size_t>(channels);
        const uint32_t offset_fract = static_cast<uint32_t>((offset - std::floor(offset)) * 256.0);
        for (int y = 0; y < height; ++y) {
            const BYTE* src_row = texture->data() + static_cast<size_t>(y) * src_stride;
            BYTE* dest_row = dest.data() + static_cast<size_t>(y) * dest_stride + offset_integer;
            uint32_t prev[4] = {};
            for (size_t x = 0; x < src_width; ++x) {
                for (int c = 0; c < channels; ++c) {
                    const uint32_t px = src_row[x * static_cast<size_t>(channels) + static_cast<size_t>(c)];
                    const uint32_t next = px * offset_fract;
                    const uint32_t offset_px = ((px << 8) - next + prev[static_cast<size_t>(c)] + 128) >> 8;
                    BYTE& d = dest_row[x * static_cast<size_t>(channels) + static_cast<size_t>(c)];
                    d = BlendStrikePixel(d, offset_px, offset_px);
                    prev[static_cast<size_t>(c)] = next;
                }
            }
            if (offset_fract > 0) {
                BYTE* tail = dest_row + src_width * static_cast<size_t>(channels);
                for (int c = 0; c < channels; ++c) {
                    const uint32_t offset_px = (prev[static_cast<size_t>(c)] + 128) >> 8;
                    tail[c] = BlendStrikePixel(tail[c], offset_px, offset_px);
                }
            }
        }
    }
    *texture = std::move(dest);
    *width = static_cast<int>(dest_width);
}

#endif  // CLEARTYPE_FIREFOX_PARITY

// Holds a counted reference for the length of a scope.
//
// CreateGlyphRunAnalysis does not take one on run.fontFace, so the face the
// analysis was built from has to be kept alive by whoever built it. The face
// cache hands out borrowed pointers and evicts, and a face can be dropped
// altogether once g_faces_mutex is released.
struct HoldFace
{
    explicit HoldFace(IDWriteFontFace* f) : face_(f)
    {
        if (face_ != nullptr) {
            face_->AddRef();
        }
    }
    ~HoldFace()
    {
        if (face_ != nullptr) {
            face_->Release();
        }
    }
    HoldFace(const HoldFace&) = delete;
    HoldFace& operator=(const HoldFace&) = delete;

private:
    IDWriteFontFace* face_;
};


// gfx/wr/wr_glyph_rasterizer/src/platform/windows/font.rs
// FontContext::rasterize_glyph, for the glyph in `outline`, with
// FontInstanceFlags as gfx/2d/ScaledFontDWrite.cpp
// ScaledFontDWrite::GetWRFontInstanceOptions would set them. `grayscale`
// is FontRenderMode::Alpha (the caller asked FreeType for
// FT_RENDER_MODE_NORMAL); false is FontRenderMode::Subpixel.
//
// Returns false for any glyph this cannot answer for, with no side effects;
// the caller then lets FreeType render it.
// `measure_only` answers the texture bounds (image->left/top/width/height)
// without creating the texture, and without consuming the outline's pending
// translation, which the render that follows the measurement still needs.
bool RasterizeThroughDWrite(FT_Face face, FT_UInt glyph_index, const FT_Outline* outline,
                            bool grayscale, DWriteGlyphImage* image, bool measure_only = false)
{
    const Options& options = GetOptions();

    // Nothing below may dereference `face` until it is known to still be one
    // of ours. FindOutlineOwner matches on an outline address alone, and can
    // hand back a face that FT_Done_Face already destroyed; GetAxisValues then
    // calls FT_Get_MM_Var on it, which walks the face's freed driver and
    // service pointers and jumps through whatever is left. The face table is
    // the authority on liveness - EraseFaceLocked removes the entry the moment
    // the last reference goes - so ask it first and fall back to FreeType for
    // a face it does not know. A face destroyed on another thread between this
    // check and the work below is still possible; an FT_Face is not thread
    // safe to begin with, and that window is bounded by this call.
    pthread_mutex_lock(&g_faces_mutex);
    const bool face_is_live = FindFaceLocked(face) != nullptr;
    pthread_mutex_unlock(&g_faces_mutex);
    if (!face_is_live) {
        // Logged every time. The line exists to show the stale-entry path
        // being taken, and the face it names is by definition gone.
        LogLine("declined face %p: not in the face table (stale outline owner)",
                reinterpret_cast<void*>(face));
        return false;
    }

    const std::vector<DWRITE_FONT_AXIS_VALUE> axes = GetAxisValues(face);

    double em_size_d = 0.0;
    float x_over_y = 1.0f;
    if (!GetEmSize(face, &em_size_d, &x_over_y)) {
        LogSkipOnce(face, "no usable em size on this face");
        return false;
    }
    // `fontEmSize: size` with size = font.size.to_f64_px() * y_scale as f32,
    // and the shape that goes with it. Both are visible only once the four
    // quantized terms behind the shape are in hand, so a transformed face has
    // them recovered here; the 26.6 char size and the 16.16 matrix are too
    // coarse to carry either. This applies to WebRender's own rasterizer alone.
    // A blob glyph comes through Skia, whose shape is SkScalerContextRec's and
    // never sat on WebRender's 1024th grid.
#if CLEARTYPE_FIREFOX_PARITY
    // The same on the Skia route, where a color glyph's paint graph puts a
    // rotation on nearly every layer.
    bool shaped_em = false;
    WindowsShapedRun shaped_run = {};
    if (dwcft::ParityActive() && CallerForFace(face) == RasterCaller::Skia) {
        // The scaler's own fields first, which need no recovery at all. The
        // search over the shape stands in where no hook has read them, and
        // where the rec belongs to some other scaler.
        shaped_em = SkiaScalerRun(face, &em_size_d, &x_over_y, &shaped_run) ||
                    ApplySkiaShapedEmSize(face, &em_size_d, &x_over_y, &shaped_run);
    }
    if (dwcft::ParityActive() && CallerForFace(face) == RasterCaller::WebRender) {
        shaped_em = ApplyTransformedEmSize(face, &em_size_d, &shaped_run);
    }
#endif
    const float em_size = static_cast<float>(em_size_d);

    const Factories factories = GetFactories();
    IDWriteFactory* factory = factories.factory;
    if (factory == nullptr) {
        return false;
    }

    // Always taken, so the entry is consumed whatever happens next.
    const PendingOutline pending =
        measure_only ? PeekOutlineState(outline) : TakeOutlineState(outline);
    if (pending.unrepresentable) {
        LogSkipOnce(face, "the caller collapsed the outline with a singular matrix");
        return false;
    }

    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        LogSkipOnce(face, "face was not created through an interposed constructor");
        return false;
    }
    const FT_Matrix face_matrix = entry->transform_matrix;
    const bool face_transformed = !entry->transform_identity;
    if (face_transformed && MatrixIsSingular(face_matrix)) {
        pthread_mutex_unlock(&g_faces_mutex);
        LogSkipOnce(face, "FT_Set_Transform set a singular matrix");
        return false;
    }

    // The reshape the caller applied to the outline, and what it becomes.
    //
    // A shear that came through FT_Set_Transform is how platform/unix/font.rs
    // delivers synthetic italics, and get_glyph_parameters() on Windows sends
    // the same skew as a DWRITE_MATRIX, so it is carried as the matrix and no
    // simulation. FT_Outline_Embolden is how unix/font.rs delivers
    // FontInstanceFlags::SYNTHETIC_BOLD (mozilla_glyphslot_embolden_less); on
    // Windows that glyph is drawn from a DWRITE_FONT_SIMULATIONS_BOLD face or
    // with extra strikes, decided below.
    DWRITE_FONT_SIMULATIONS simulations = pending.simulations;
    constexpr FT_Matrix identity = { .xx = 0x10000, .xy = 0, .yx = 0, .yy = 0x10000 };
    FT_Matrix combined = identity;
    if (face_transformed) {
        combined = face_matrix;
        // The shear carries the synthetic oblique, so the oblique simulation
        // would apply it a second time. The bold is a separate reshape and
        // survives, since a face can ask for both.
        simulations = static_cast<DWRITE_FONT_SIMULATIONS>(
            static_cast<int>(simulations) & ~static_cast<int>(DWRITE_FONT_SIMULATIONS_OBLIQUE));
    } else if (pending.has_matrix) {
        combined = pending.matrix;
    }

    bool multistrike = false;
    bool bitmap_font = false;
#if CLEARTYPE_FIREFOX_PARITY
    // gfx/thebes/gfxDWriteFontList.cpp gfxDWriteFontEntry::CreateFontInstance,
    // switching on gfx.font_rendering.directwrite.bold_simulation
    // (firefox_parity_data.h kDirectWriteBoldSimulation): 0 never the
    // DirectWrite simulation, 1 installed fonts only - not webfont resources
    // (mIsDataUserFont, a face loaded from memory) and not COLR fonts -
    // 2 all but COLR. The alternative is multi-strike. WebRender:
    // SYNTHETIC_BOLD -> DWRITE_FONT_SIMULATIONS_BOLD in get_font_face();
    // MULTISTRIKE_BOLD -> apply_multistrike_bold.
    if (dwcft::ParityActive() &&
        (pending.simulations & DWRITE_FONT_SIMULATIONS_BOLD) != 0) {
        const bool data_user_font = entry->memory != nullptr;
        bool use_bold_sim = false;
        switch (firefox_parity::kDirectWriteBoldSimulation) {
        case 0:
            break;
        case 1:
            use_bold_sim = !data_user_font && !FaceHasCOLRLocked(entry);
            break;
        default:
            use_bold_sim = !FaceHasCOLRLocked(entry);
            break;
        }
        if (!use_bold_sim) {
            simulations = simulations & ~DWRITE_FONT_SIMULATIONS_BOLD;
            multistrike = true;
        }
    }
    // is_bitmap_font(): render_mode != Mono && EMBEDDED_BITMAPS. The flag is
    // gfxDWriteFont::GetScaledFont's useEmbeddedBitmap, reconstructed in
    // IsBitmapFontLocked.
    bitmap_font = IsBitmapFontLocked(entry, em_size_d);
#endif

    IDWriteFontFace* dwrite_face = GetDWriteFaceLocked(entry, factory, simulations, axes);
    if (dwrite_face == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return false;  // GetDWriteFaceLocked logs its own reason
    }

    // get_glyph_parameters(): a bitmap font renders with an identity shape and
    // no subpixel offset, everything else with the instance's transform and
    // the glyph's quantized offset; synthetic italics are applied to both.
    // pending.dx is the caller's whole translate, integer pixels included
    // (unix/font.rs moves the outline so its padded cbox starts at 0), and
    // DirectWrite takes it as the matrix's dx; integer pixels do not change a
    // rasterization, so only the phase is dropped for a bitmap font, and only
    // when subpixel positioning is on at all.
    float exact_skew = 0.0f;
    const ObliqueForm oblique_form =
        dwcft::ParityActive() ? ExactObliqueSkew(combined, &exact_skew) : ObliqueForm::None;
    const bool exact_oblique = oblique_form != ObliqueForm::None;
#if CLEARTYPE_FIREFOX_PARITY
    // The face, not the thread: blob images are rasterized through Skia on
    // WebRender's own workers, and those glyphs need SkScalerContext_win_dw's
    // parameters, not platform/windows/font.rs's.
    const bool skia_caller = CallerForFace(face) == RasterCaller::Skia;
#else
    constexpr bool skia_caller = false;
#endif
    // SkScalerContext_DW keeps the transform and the subpixel offset for a
    // bitmap font (kSubpixelPositioning_Flag comes from SkFont::setSubpixel,
    // which ScaledFontDWrite::SetupSkFontDrawOptions sets); only the WebRender
    // path zeroes them.
    const bool webrender_bitmaps = bitmap_font && !skia_caller;
    if (webrender_bitmaps) {
        combined = identity;
    }
    const bool keep_phase = options.subpixel_positioning && !webrender_bitmaps;
    const FT_Pos shift_x = keep_phase ? pending.dx : pending.dx & ~static_cast<FT_Pos>(63);
    const auto offset_x =
        static_cast<float>(static_cast<double>(shift_x) / 64.0 + pending.delta_x);
    const auto offset_y =
        static_cast<float>(-static_cast<double>(pending.dy) / 64.0 + pending.delta_y);

    // create_glyph_analysis(): one glyph, zero advance, zero offset, not
    // sideways, bidi level zero, em size in DIPs.
    const UINT16 dwrite_glyph = static_cast<UINT16>(glyph_index);
    constexpr FLOAT advance = 0.0f;
    constexpr DWRITE_GLYPH_OFFSET offset = {};

    DWRITE_GLYPH_RUN run = {};
    run.fontFace = dwrite_face;
    run.fontEmSize = em_size;
    run.glyphCount = 1;
    run.glyphIndices = &dwrite_glyph;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;

#if CLEARTYPE_FIREFOX_PARITY
    // Only where the shaped search did not already answer with both scales.
    if (skia_caller && !shaped_em) {
        ExactAspect(face, combined, em_size_d, &x_over_y);
    }
#endif
    const bool identity_shape = MatrixIsIdentity(combined);
    const bool square_pixels = x_over_y >= 0.9999f && x_over_y <= 1.0001f;
    DWRITE_MATRIX transform = ToDWriteMatrix(combined, x_over_y);
#if CLEARTYPE_FIREFOX_PARITY
    // Where the four integers behind the shape are known, the terms Windows
    // divides out of them stand instead of the ones the 16.16 shape can carry.
    // On the WebRender route that also puts column one over y_scale, which is
    // the column platform/unix/font.rs divides by x_scale instead.
    if (shaped_em) {
        transform.m11 = shaped_run.m11;
        transform.m12 = shaped_run.m12;
        transform.m21 = shaped_run.m21;
        transform.m22 = shaped_run.m22;
    }
#endif
    if (oblique_form == ObliqueForm::Upright) {
        // webrender_api/src/font.rs SyntheticItalics::to_skew, the f32 tangent
        // Windows receives rather than the 16.16 FreeType received.
        transform = ToDWriteMatrix(identity, x_over_y);
        transform.m21 = -exact_skew * x_over_y;
    } else if (oblique_form == ObliqueForm::QuarterTurn) {
        // The turn is kept and only the shear is put back, which ToDWriteMatrix
        // leaves in m22 for this product.
        transform.m22 = -exact_skew;
    }
    transform.dx = offset_x;
    transform.dy = offset_y;
    const bool need_transform = !identity_shape || exact_oblique || !square_pixels ||
                                offset_x != 0.0f || offset_y != 0.0f;
    const DWRITE_MATRIX* transform_ptr = need_transform ? &transform : nullptr;

    // dwrite_measure_mode(): GDI_CLASSIC for a bitmap font or FORCE_GDI,
    // otherwise NATURAL for Alpha and Subpixel.
    DWRITE_MEASURING_MODE measuring_mode =
        bitmap_font ? DWRITE_MEASURING_MODE_GDI_CLASSIC : options.measuring_mode;

    // dwrite_render_mode(): GDI_CLASSIC for a bitmap font or FORCE_GDI;
    // NATURAL_SYMMETRIC for FORCE_SYMMETRIC, NATURAL for NO_SYMMETRIC - the
    // flags ScaledFontDWrite::GetWRFontInstanceOptions derives from the
    // system rendering mode (DWriteSettings::RenderingMode, which is the
    // default IDWriteRenderingParams' mode unless a pref overrides it); else
    // the recommendation.
    DWRITE_RENDERING_MODE rendering_mode = options.rendering_mode;
    DWRITE_GRID_FIT_MODE grid_fit_mode = options.grid_fit_mode;
    DWRITE_TEXTURE_TYPE texture_type = DWRITE_TEXTURE_CLEARTYPE_3x1;
    bool use_factory2 = options.grid_fit_forced && factories.factory2 != nullptr;
    DWRITE_TEXT_ANTIALIAS_MODE antialias_mode = DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE;
    // Skia always passes its transform; WebRender passes one only when it
    // has something to put in it.
    bool pass_transform = need_transform;
    if (skia_caller) {
#if CLEARTYPE_FIREFOX_PARITY
        const bool axis_aligned =
            !exact_oblique && ((combined.xy == 0 && combined.yx == 0) ||
                               (combined.xx == 0 && combined.yy == 0));
        const SkiaDWParams p =
            SkiaDWParamsLocked(entry, dwrite_face, em_size, grayscale, bitmap_font, axis_aligned,
                               factories.factory2 != nullptr);
        run.fontEmSize = p.size_render;
        measuring_mode = p.measuring_mode;
        if (!options.rendering_mode_forced) {
            rendering_mode = p.rendering_mode;
        }
        if (!options.grid_fit_forced) {
            grid_fit_mode = p.grid_fit_mode;
        }
        texture_type = p.texture_type;
        antialias_mode = p.antialias_mode;
        use_factory2 = p.use_factory2 || use_factory2;
        pass_transform = true;
#endif
    } else if (bitmap_font && !options.rendering_mode_forced) {
        rendering_mode = DWRITE_RENDERING_MODE_GDI_CLASSIC;
    } else if (!options.rendering_mode_forced) {
        DWRITE_RENDERING_MODE system_mode = DWRITE_RENDERING_MODE_DEFAULT;
        if (IDWriteRenderingParams* params = GetDefaultRenderingParams()) {
            system_mode = params->GetRenderingMode();
        }
        if (system_mode == DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC) {
            rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC;
        } else if (system_mode == DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL) {
            rendering_mode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL;
        } else {
            const CachedMode resolved = ResolveModeLocked(entry, dwrite_face, em_size, options);
            if (!resolved.answered) {
                pthread_mutex_unlock(&g_faces_mutex);
                return false;
            }
            rendering_mode = resolved.rendering_mode;
            grid_fit_mode = resolved.grid_fit_mode;
        }
    }
    if (!use_factory2 && texture_type == DWRITE_TEXTURE_ALIASED_1x1) {
        texture_type = DWRITE_TEXTURE_CLEARTYPE_3x1;
    }

    // GlyphRunAnalysis::create(&run, 1.0, transform, render_mode, measure_mode,
    // 0.0, 0.0): the seven-argument overload, which is the one
    // platform/windows/font.rs calls. The IDWriteFactory2 overload is only
    // reached when CLEARTYPE_GRID_FIT pinned a grid-fit mode.
    IDWriteGlyphRunAnalysis* analysis = nullptr;
    HRESULT hr;
    const DWRITE_MATRIX* analysis_transform = pass_transform ? &transform : nullptr;
    if (use_factory2 && factories.factory2 != nullptr) {
        hr = factories.factory2->CreateGlyphRunAnalysis(&run, analysis_transform, rendering_mode, measuring_mode, grid_fit_mode, antialias_mode, 0.0f, 0.0f, &analysis);
    } else {
        hr = factory->CreateGlyphRunAnalysis(&run, 1.0f, analysis_transform, rendering_mode, measuring_mode, 0.0f, 0.0f, &analysis);
    }
    if (FAILED(hr) || analysis == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        LogLine("CreateGlyphRunAnalysis failed hr=0x%08X (glyph %u, em %.2f)", static_cast<unsigned>(hr),
                static_cast<unsigned>(dwrite_glyph), static_cast<double>(em_size));
        return false;
    }

    // dwrite_texture_type(): CLEARTYPE_3x1 for Alpha and Subpixel alike. If
    // its bounds are empty the glyph may not be renderable with ClearType, and
    // an ALIASED analysis with a 1x1 texture is tried instead.
    RECT bounds = {};
    hr = analysis->GetAlphaTextureBounds(texture_type, &bounds);
    if (SUCCEEDED(hr) && (bounds.left == bounds.right || bounds.top == bounds.bottom)) {
        IDWriteGlyphRunAnalysis* analysis2 = nullptr;
        HRESULT hr2 = factory->CreateGlyphRunAnalysis(&run, 1.0f, analysis_transform, DWRITE_RENDERING_MODE_ALIASED, measuring_mode, 0.0f, 0.0f, &analysis2);
        if (SUCCEEDED(hr2) && analysis2 != nullptr) {
            RECT bounds2 = {};
            hr2 = analysis2->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds2);
            if (SUCCEEDED(hr2) && bounds2.left != bounds2.right && bounds2.top != bounds2.bottom) {
                analysis->Release();
                analysis = analysis2;
                texture_type = DWRITE_TEXTURE_ALIASED_1x1;
                bounds = bounds2;
            } else {
                analysis2->Release();
            }
        }
    }
    // ReSharper disable once CppLocalVariableWithNonTrivialDtorIsNeverUsed
    const HoldFace hold(run.fontFace);
    pthread_mutex_unlock(&g_faces_mutex);

    bool produced = false;
    const long width = static_cast<long>(bounds.right) - static_cast<long>(bounds.left);
    const long height = static_cast<long>(bounds.bottom) - static_cast<long>(bounds.top);
    // rasterize_glyph(): empty bounds are LoadFailed, which for FreeType is a
    // glyph with nothing to draw; declining leaves FreeType's own empty bitmap.
    if (measure_only) {
        analysis->Release();
        if (!(SUCCEEDED(hr) && width > 0 && height > 0)) {
            return false;
        }
        image->channels = texture_type == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 3 : 1;
        image->width = static_cast<int>(width);
        image->height = static_cast<int>(height);
        image->left = bounds.left;
        image->top = -bounds.top;
        return true;
    }
    if (SUCCEEDED(hr) && width > 0 && height > 0 && width < 8192 && height < 8192) {
        const int source_channels = texture_type == DWRITE_TEXTURE_CLEARTYPE_3x1 ? 3 : 1;
        // The size guard above admits glyphs large enough for this to fail, and
        // an exception would leave an extern "C" frame into a C caller with no
        // handler. Declining falls through to FreeType, as every other failure
        // in this function does.
        std::vector<BYTE> texture;
        try {
            texture.assign(static_cast<size_t>(width) * static_cast<size_t>(height) *
                               static_cast<size_t>(source_channels),
                           0);
        } catch (const std::bad_alloc&) {
            LogLine("no memory for a %ldx%ldx%d glyph texture; falling through",
                    static_cast<long>(width), static_cast<long>(height), source_channels);
            return false;
        }
        hr = analysis->CreateAlphaTexture(texture_type, &bounds, texture.data(), static_cast<UINT32>(texture.size()));
        if (SUCCEEDED(hr)) {
            // convert_to_bgra(): a 1x1 texture is alpha; a 3x1 texture is a
            // subpixel mask only for FontRenderMode::Subpixel on a non-bitmap
            // font, otherwise "Only take the G channel, as its closest to D2D".
            // SkScalerContext_DW::generateDWImage takes the G channel for an
            // A8 glyph too (RGBToA8) and keeps the three for LCD16 even for a
            // bitmap font.
            int channels = source_channels;
            if (source_channels == 3 && (grayscale || webrender_bitmaps)) {
                std::vector<BYTE> green;
                try {
                    green.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
                } catch (const std::bad_alloc&) {
                    LogLine("no memory to extract the G channel; falling through");
                    return false;
                }
                for (size_t i = 0; i < green.size(); ++i) {
                    green[i] = texture[i * 3 + 1];
                }
                texture.swap(green);
                channels = 1;
            }
            int out_width = static_cast<int>(width);
#if CLEARTYPE_FIREFOX_PARITY
            if (multistrike) {
                // platform/windows/font.rs rasterize_glyph:
                //   let (strike_scale, pixel_step) = if bitmaps { (y_scale, 1.0) }
                //                                    else { (x_scale, y_scale / x_scale) };
                // y_scale is the em size this rasterized at and x_over_y is
                // x_scale over it, so both terms come from the pair already in
                // hand. They part only where the pixels are not square.
                const double strike_scale =
                    bitmap_font ? em_size_d : em_size_d * static_cast<double>(x_over_y);
                const double pixel_step =
                    bitmap_font ? 1.0 : 1.0 / static_cast<double>(x_over_y);
                ApplyMultistrikeBold(&texture, &out_width, static_cast<int>(height), channels,
                                     WinExtraStrikes(strike_scale), pixel_step);
            }
#endif
            // After the strikes, matching platform/windows/font.rs, which
            // blends them from linear coverage and preblends the gamma once
            // the bold is in place.
            ApplyAlphaGamma(texture, options);
            if (LogEnabled() && LogFaceWanted(face)) {
                unsigned long ink = 0;
                for (const BYTE b : texture) {
                    ink += b;
                }
                char shape[64];
                shape[0] = '\0';
                if (transform_ptr != nullptr) {
                    (void)std::snprintf(shape, sizeof(shape), " xform %.8f,%.8f,%.8f,%.8f",
                                  static_cast<double>(transform.m11),
                                  static_cast<double>(transform.m12),
                                  static_cast<double>(transform.m21),
                                  static_cast<double>(transform.m22));
                }
                LogLine("raster glyph %u face %p em %.4f bounds %ldx%ld at (%ld,%ld) "
                        "shift %.3f,%.3f mode %d measure %d texture %s ink %lu%s%s%s%s",
                        static_cast<unsigned>(dwrite_glyph), reinterpret_cast<void*>(face), static_cast<double>(em_size), width, height,
                        static_cast<long>(bounds.left), static_cast<long>(bounds.top),
                        static_cast<double>(offset_x), static_cast<double>(offset_y),
                        static_cast<int>(rendering_mode), static_cast<int>(measuring_mode),
                        source_channels == 3 ? "3x1" : "1x1", ink, shape,
                        bitmap_font ? " bitmap-font" : "", multistrike ? " multistrike" : "",
                        skia_caller ? " skia" : "");
            }
            // DWrite's bounds are device pixels with y growing downward and
            // the origin on the baseline; FreeType's bitmap_top is the top
            // edge measured upward from the same baseline.
            image->texture = std::move(texture);
            image->channels = channels;
            image->width = out_width;
            image->height = static_cast<int>(height);
            image->left = bounds.left;
            image->top = -bounds.top;
            produced = true;
        } else {
            LogLine("CreateAlphaTexture failed hr=0x%08X (glyph %u)", static_cast<unsigned>(hr),
                    static_cast<unsigned>(dwrite_glyph));
        }
    }

    analysis->Release();
    return produced;
}

bool RenderThroughDWrite(FT_GlyphSlot slot, const bool grayscale)
{
    DWriteGlyphImage image;
    if (!RasterizeThroughDWrite(slot->face, slot->glyph_index, &slot->outline, grayscale,
                                &image)) {
        return false;
    }
    // A coverage texture is installed as FT_PIXEL_MODE_GRAY even when the
    // caller asked for FT_RENDER_MODE_LCD: platform/unix/font.rs reads the
    // slot's pixel_mode, and that is how the glyph formats Windows produces
    // for a bitmap font (GlyphFormat::Bitmap) or an ALIASED_1x1 retry
    // (get_alpha_glyph_format()) come out.
    return InstallBitmap(slot, image.texture, image.channels, image.width, image.height,
                         image.left, image.top);
}

// ---------------------------------------------------------------------------
// Re-entry guard.
//
// libfreetype is not built with -Bsymbolic, so its calls to its own exported
// entry points resolve through the GOT, where a preloaded library comes first
// in the lookup. FreeType therefore re-enters this file while it is servicing
// a call this file made, for FT_Load_Glyph, FT_Render_Glyph, FT_Request_Size,
// FT_Outline_Translate, FT_Get_Sfnt_Table and FT_Outline_Get_CBox.
//
// Raised while this library is inside real FreeType. Only FT_Outline_Get_CBox
// reads it, and only to decline: FreeType asks for the control box of the
// outline it is loading or rendering at that moment and builds slot->metrics
// and its own bitmap from the answer, and DirectWrite's texture bounds do not
// answer that question.
//
// Nothing else reads it, so the nested FT_Render_Glyph that real FT_Load_Glyph
// tail-jumps to for FT_LOAD_RENDER still rasterizes through DirectWrite.
// ---------------------------------------------------------------------------
thread_local unsigned g_in_real_freetype = 0;

// The glyph slot outline handed to FreeType, for as long as it is in there.
// FreeType translates and transforms scratch outlines in its own stack frame
// while it works, and those are not the caller's glyph; see ForeignToCaller.
thread_local const FT_Outline* g_loading_outline = nullptr;
#if CLEARTYPE_FIREFOX_PARITY
// The face this thread last loaded a glyph for. Names the font behind a Skia
// scaler, which carries an SkTypeface and no FreeType face of its own.
thread_local FT_Face g_last_loaded_face = nullptr;
#endif

struct InRealFreeType
{
    explicit InRealFreeType(const FT_Outline* slot_outline = nullptr)
        : saved_(g_loading_outline)
    {
        ++g_in_real_freetype;
        if (slot_outline != nullptr) {
            g_loading_outline = slot_outline;
        }
    }
    ~InRealFreeType()
    {
        --g_in_real_freetype;
        g_loading_outline = saved_;
    }
    InRealFreeType(const InRealFreeType&) = delete;
    InRealFreeType& operator=(const InRealFreeType&) = delete;

private:
    const FT_Outline* saved_;
};

// Bumped when an entry point finishes the bookkeeping for a load or a size
// change, so an outer one can tell the nested call FreeType made through the
// GOT already did it. FT_Set_Char_Size and FT_Set_Pixel_Sizes reach the real
// FT_Request_Size that way, and FT_Load_Char reaches the real FT_Load_Glyph.
thread_local unsigned g_size_applied = 0;
thread_local unsigned g_load_applied = 0;

// True for an outline FreeType is working on inside its own frame. The
// autohinter translates one of those several times per hinted load, and an
// entry recorded for it belongs to no face, so nothing can ever remove it and
// the next load at the same stack depth inherits it.
//
// The FT_Set_Transform delta is a translate of the glyph slot's own outline
// and still records, which is what the comment in FT_Load_Glyph describes.
bool ForeignToCaller(const FT_Outline* outline)
{
    return g_in_real_freetype != 0 && outline != g_loading_outline;
}

}  // namespace

// ---------------------------------------------------------------------------
// Interposed entry points.
// ---------------------------------------------------------------------------

extern "C" {

FT_Error FT_New_Face(FT_Library library, const char* path, const FT_Long face_index, FT_Face* aface)
{
    // A face is opened from application context; libxul_patch.cpp keys off it.
    dwcft::NoteOpen(nullptr, __builtin_return_address(0));
    ft_new_face_fn real = real_FT_New_Face();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    const FT_Error error = real(library, path, face_index, aface);
    if (error == 0 && aface != nullptr && *aface != nullptr) {
        RecordFace(library, *aface, path, nullptr, 0, face_index);
    }
    return error;
}

FT_Error FT_New_Memory_Face(FT_Library library, const FT_Byte* base, const FT_Long size,
                            const FT_Long face_index, FT_Face* aface)
{
    dwcft::NoteOpen(nullptr, __builtin_return_address(0));
    ft_new_memory_face_fn real = real_FT_New_Memory_Face();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    const FT_Error error = real(library, base, size, face_index, aface);
    if (error == 0 && aface != nullptr && *aface != nullptr) {
        RecordFace(library, *aface, nullptr, base, size, face_index);
    }
    return error;
}

// Interposing this as well as the two above does not double-record:
// FT_New_Face and FT_New_Memory_Face reach FreeType's shared open path
// through an internal, non-exported call, not through this symbol
// (verified by disassembly on this system's libfreetype). Callers that use
// FT_Open_Face directly - Qt does, for some font sources - would otherwise
// be invisible to the face table.
FT_Error FT_Open_Face(FT_Library library, const FT_Open_Args* args, const FT_Long face_index,
                      FT_Face* aface)
{
    dwcft::NoteOpen(nullptr, __builtin_return_address(0));
    ft_open_face_fn real = real_FT_Open_Face();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    const FT_Error error = real(library, args, face_index, aface);
    if (error == 0 && args != nullptr && aface != nullptr && *aface != nullptr) {
        if ((args->flags & FT_OPEN_PATHNAME) != 0) {
            RecordFace(library, *aface, args->pathname, nullptr, 0, face_index);
        } else if ((args->flags & FT_OPEN_MEMORY) != 0) {
            RecordFace(library, *aface, nullptr, args->memory_base, args->memory_size, face_index);
        } else {
            // FT_OPEN_STREAM: a custom stream with no bytes and no path to
            // hand DWrite. Recorded with neither so the render path stops
            // at the table lookup instead of guessing.
            RecordFace(library, *aface, nullptr, nullptr, 0, face_index);
        }
    }
    return error;
}

// Why the face table mirrors FreeType's reference count instead of treating
// FT_Done_Face as the end of a face's life: Qt6 takes references this way and
// keeps rendering with the face afterwards.
FT_Error FT_Reference_Face(FT_Face face)
{
    ft_reference_face_fn real = real_FT_Reference_Face();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    const FT_Error error = real(face);
    if (error == 0 && face != nullptr) {
        ReferenceFace(face);
    }
    return error;
}

FT_Error FT_Done_Face(FT_Face face)
{
    ft_done_face_fn real = real_FT_Done_Face();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    // The real call first, because it can refuse a stale handle without
    // destroying anything, and dropping the record before knowing that
    // un-registers a face that is still alive. Tearing down afterwards is safe
    // even when the face is gone, because it works from the recorded FaceEntry
    // and uses the FT_Face only as a lookup key.
    const FT_Error error = real(face);
    if (error == 0 && face != nullptr) {
        ReleaseFace(face);
    }
    return error;
}

FT_Error FT_Done_FreeType(FT_Library library)
{
    ft_done_freetype_fn real = real_FT_Done_FreeType();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    // As in FT_Done_Face, sweep only once the real call has accepted.
    const FT_Error error = real(library);
    if (error == 0 && library != nullptr) {
        ForgetLibrary(library);
#if CLEARTYPE_FIREFOX_PARITY
        ForgetSkiaLibrary(library);
#endif
    }
    // After the sweep, so the counts match the label.
    if (error == 0) {
        LogTableCensus("after FT_Done_FreeType");
    }
    return error;
}

// The two halves of this call mean different things and are handled
// differently.
//
// The matrix is a shape change: synthetic oblique, a rotation, a caller-side
// scale. It reaches the DWrite path as a DWRITE_MATRIX, which means converting
// from FreeType's y-up space to DirectWrite's y-down device space. That
// conjugation sign-flips shear and rotation, so getting it wrong renders
// slanted text leaning the wrong way. ToDWriteMatrix does the conversion, and
// cleartype/tests/test_fallback.c checks it against real FreeType.
//
// The delta is only a translation, and it must not decide anything. Qt6 puts
// its subpixel glyph offset here instead of in FT_Outline_Translate, so with
// hinting off - which is what makes Qt position glyphs fractionally at all -
// most glyphs carry a non-zero delta. Declining a face for having one leaves a
// page half rendered by each engine. Only the matrix decides.
//
// The delta itself needs nothing done to it here. FreeType applies it during
// FT_Load_Glyph by calling FT_Outline_Translate, which this file already
// interposes, so it is already in the pending translation by the time a glyph
// is rasterized. Recording it a second time double-applies it.
void FT_Set_Transform(FT_Face face, FT_Matrix* matrix, FT_Vector* delta)
{
    ft_set_transform_fn real = real_FT_Set_Transform();
    if (real == nullptr) {
        return;
    }
    real(face, matrix, delta);
    if (!InterposerWanted()) {
        return;
    }

    // Within a unit of 1/65536 counts as identity. WebRender hands down a
    // device matrix that is identity in intent but arrives as 65535/65536,
    // and treating that as a real transform costs a DWRITE_MATRIX on every
    // glyph of the face and drops its simulation flags for nothing.
    const bool identity_matrix =
        matrix == nullptr ||
        (matrix->xx > 0x10000 - 2 && matrix->xx < 0x10000 + 2 &&
         matrix->yy > 0x10000 - 2 && matrix->yy < 0x10000 + 2 &&
         matrix->xy > -2 && matrix->xy < 2 && matrix->yx > -2 && matrix->yx < 2);

    pthread_mutex_lock(&g_faces_mutex);
    if (FaceEntry* entry = FindFaceLocked(face)) {
        entry->transform_identity = identity_matrix;
        if (matrix != nullptr) {
            entry->transform_matrix = *matrix;
        } else {
            constexpr FT_Matrix identity = { .xx = 0x10000, .xy = 0, .yx = 0, .yy = 0x10000 };
            entry->transform_matrix = identity;
        }
        entry->transform_delta = delta != nullptr ? *delta : FT_Vector{ .x = 0, .y = 0 };
    }
    pthread_mutex_unlock(&g_faces_mutex);
}

// Recorded, then forwarded unchanged - see "Subpixel phase" above. The
// outline still gets translated for real, so the fall-through path renders
// exactly as it would without this shim loaded.
void FT_Outline_Translate(const FT_Outline* outline, const FT_Pos xOffset, const FT_Pos yOffset)
{
    ft_outline_translate_fn real = real_FT_Outline_Translate();
    if (real == nullptr) {
        return;
    }
    real(outline, xOffset, yOffset);
    // A translate FreeType makes on the slot outline while loading it is
    // FreeType's own work and has nothing to do with the caller's subpixel
    // phase. It moves the outline to suit the bitmap about to be built, such
    // as the hinted left phantom point or the light autohinter's grid fit,
    // and DirectWrite grid-fits its own outline, so carrying it across shifts
    // the glyph twice. The one in-load translate that does belong to the
    // caller is FT_Set_Transform's delta, which FT_Load_Glyph puts back from
    // the face entry.
    //
    // Firefox parity only, so the Chromium hosts keep the translate they
    // have.
#if CLEARTYPE_FIREFOX_PARITY
    const bool freetypes_own =
        dwcft::ParityActive() && g_in_real_freetype > 0 && outline == g_loading_outline;
#else
    constexpr bool freetypes_own = false;
#endif
    if (outline != nullptr && (xOffset != 0 || yOffset != 0) && !freetypes_own &&
        !ForeignToCaller(outline)) {
        RecordShift(outline, xOffset, yOffset);
    }
}

// Where an outline the caller owns is destroyed. Whatever was recorded for it
// goes with it, so the table never holds a freed address that could be handed
// back for a glyph slot.

FT_Error FT_Outline_Done(FT_Library library, FT_Outline* outline)
{
    ft_outline_done_fn real = real_FT_Outline_Done();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    const FT_Error error = real(library, outline);
    if (error == 0 && outline != nullptr) {
        DiscardOutlineState(outline);
    }
    return error;
}

void FT_Done_Glyph(FT_Glyph glyph)
{
    ft_done_glyph_fn real = real_FT_Done_Glyph();
    if (real == nullptr) {
        return;
    }
    if (glyph != nullptr && glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        DiscardOutlineState(&reinterpret_cast<FT_OutlineGlyph>(glyph)->outline);
    }
    real(glyph);
}

// Whether this load is the one WebRender takes a glyph's pixels from for a
// face Windows draws from an embedded strike.
//
// gfxDWriteFont::GetScaledFont sends useEmbeddedBitmap to WebRender as
// FontInstanceFlags::EMBEDDED_BITMAPS, and platform/unix/font.rs turns the
// absence of that flag into FT_LOAD_NO_BITMAP. The Linux flag comes from
// fontconfig, which answers for a font and not for a font at a size, so
// src/fontconfig.cpp says no to embeddedbitmap and the strike is put back
// here, for the faces and the sizes IsBitmapFontLocked names.
//
// The format matters as much as the pixels. rasterize_glyph reads the slot's
// format before it rasterizes, and an outline there reaches the batch as
// GlyphFormat::Alpha, which leaves SubpixelDirection::Horizontal and a
// snap_bias of 0.125; a strike gives GlyphFormat::Bitmap, None and 0.5. That
// is a whole pixel of placement for every subpixel phase from 0.5 up.
//
// Skia keeps the transform and the subpixel offset for a bitmap font, so only
// the WebRender path asks for the strike itself.
// Raised while FT_GlyphSlot_Embolden loads a glyph again without its strike,
// so the load below does not hand the strike straight back.
thread_local bool g_suppress_strike = false;

static bool StrikeBelongsToThisLoad(FT_Face face)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (g_suppress_strike || !dwcft::ParityActive() || face == nullptr ||
        face->size == nullptr || CallerForFace(face) == RasterCaller::Skia) {
        return false;
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (!GetEmSize(face, &em_size, &x_over_y)) {
        return false;
    }
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    const bool bitmap_font = entry != nullptr && IsBitmapFontLocked(entry, em_size);
    pthread_mutex_unlock(&g_faces_mutex);
    return bitmap_font;
#else
    (void)face;
    return false;
#endif
}

// Whether a skewed load has to leave the face's strikes alone.
//
// A strike is a bitmap, and skewing one blends its pixels into coverage the
// strike never held. DirectWrite draws an obliqued run from the outline
// instead, so the strike is refused here for as long as the skew is set.
static bool SkewedLoadSkipsStrike(FT_Face face)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (!dwcft::ParityActive() || face == nullptr ||
        (face->face_flags & FT_FACE_FLAG_FIXED_SIZES) == 0) {
        return false;
    }
    pthread_mutex_lock(&g_faces_mutex);
    const FaceEntry* entry = FindFaceLocked(face);
    const bool skewed = entry != nullptr && !entry->transform_identity &&
                        (entry->transform_matrix.xy != 0 || entry->transform_matrix.yx != 0);
    pthread_mutex_unlock(&g_faces_mutex);
    return skewed;
#else
    (void)face;
    return false;
#endif
}

// The synthetic-styling family. Each one reshapes an outline in place, which
// is invisible to a shim that rasterizes from a font file by glyph index -
// see the PendingOutline comment. Each is classified into the DirectWrite
// simulation that means the same thing, so the glyph comes back bold or
// slanted the way Windows would draw it rather than silently regular.
//
// The simulation amounts are DirectWrite's, not FreeType's: the toolkit laid
// the run out with FreeType's emboldened advances.

void FT_GlyphSlot_Embolden(FT_GlyphSlot slot)
{
    ft_glyphslot_embolden_fn real = real_FT_GlyphSlot_Embolden();
    if (real == nullptr) {
        return;
    }
    // A strike, which this function smears one pixel sideways. DirectWrite
    // draws the same glyph through its bold simulation instead, fattening the
    // stems unevenly and antialiasing the edges, and nothing renders a bitmap
    // glyph afterwards for the shim to correct it in.
    //
    // The glyph is loaded again without the strike first. FreeType owns a
    // strike glyph's bitmap and frees it at the next load, so swapping that
    // buffer for one of this shim's is a double free; an outline slot holds no
    // bitmap at all, which is the state InstallBitmap already installs into.
    if (slot != nullptr && slot->face != nullptr &&
        slot->format == FT_GLYPH_FORMAT_BITMAP && StrikeBelongsToThisLoad(slot->face)) {
        const FT_UInt index = slot->glyph_index;
        g_suppress_strike = true;
        const FT_Error error = FT_Load_Glyph(slot->face, index,
                                             FT_LOAD_NO_BITMAP |
                                                 FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH);
        g_suppress_strike = false;
        if (error == 0 && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
            RecordSimulation(&slot->outline, DWRITE_FONT_SIMULATIONS_BOLD);
            if (RenderThroughDWrite(slot, false)) {
                return;
            }
        }
    }
    real(slot);
    if (slot != nullptr) {
        RecordSimulation(&slot->outline, DWRITE_FONT_SIMULATIONS_BOLD);
    }
}

void FT_GlyphSlot_Oblique(FT_GlyphSlot slot)
{
    ft_glyphslot_oblique_fn real = real_FT_GlyphSlot_Oblique();
    if (real == nullptr) {
        return;
    }
    real(slot);
    if (slot != nullptr) {
        RecordSimulation(&slot->outline, DWRITE_FONT_SIMULATIONS_OBLIQUE);
    }
}

FT_Error FT_Outline_Embolden(FT_Outline* outline, const FT_Pos strength)
{
    ft_outline_embolden_fn real = real_FT_Outline_Embolden();
    if (real == nullptr) {
        return FT_Err_Invalid_Argument;
    }
    const FT_Error error = real(outline, strength);
    if (error == 0 && outline != nullptr && strength != 0) {
        RecordSimulation(outline, DWRITE_FONT_SIMULATIONS_BOLD);
    }
    return error;
}

FT_Error FT_Outline_EmboldenXY(FT_Outline* outline, const FT_Pos xstrength, const FT_Pos ystrength)
{
    ft_outline_embolden_xy_fn real = real_FT_Outline_EmboldenXY();
    if (real == nullptr) {
        return FT_Err_Invalid_Argument;
    }
    const FT_Error error = real(outline, xstrength, ystrength);
    if (error == 0 && outline != nullptr && (xstrength != 0 || ystrength != 0)) {
        RecordSimulation(outline, DWRITE_FONT_SIMULATIONS_BOLD);
    }
    return error;
}

// Identity is let through. A toolkit that applies its shape matrix
// unconditionally would otherwise disable the shim for every glyph it draws,
// and an identity matrix changes nothing to reproduce.
//
// A pure horizontal shear has a unit diagonal and no y-dependence on x, which
// is how Cairo builds a synthetic oblique, so it maps to the oblique
// simulation. Any other matrix is a real transform, and only a singular one
// has nothing to stand in for it.
void FT_Outline_Transform(const FT_Outline* outline, const FT_Matrix* matrix)
{
    ft_outline_transform_fn real = real_FT_Outline_Transform();
    if (real == nullptr) {
        return;
    }
    real(outline, matrix);
    if (outline == nullptr || matrix == nullptr) {
        return;
    }
    const bool identity = matrix->xx == 0x10000 && matrix->xy == 0 && matrix->yx == 0 &&
                          matrix->yy == 0x10000;
    const bool shear = matrix->xx == 0x10000 && matrix->yy == 0x10000 && matrix->yx == 0 &&
                       matrix->xy != 0;
    if (identity) {
        return;
    }
    if (shear) {
        RecordSimulation(outline, DWRITE_FONT_SIMULATIONS_OBLIQUE);
    } else if (MatrixIsSingular(*matrix)) {
        RecordUnrepresentable(outline);
    } else {
        RecordMatrix(outline, *matrix);
    }
}

// Gecko is told hintslight so gfxFT2FontBase::GetFTGlyphExtents leaves the
// glyph's ink bounds unsnapped, and it passes that on as FT_LOAD_TARGET_LIGHT.
// The load itself still has to be unhinted. The light autohinter grid-fits
// horiBearingY and height to whole pixels, which is the rounding the
// fontconfig answer just removed, and it moves the outline with them.
// FT_LOAD_NO_HINTING wins over any target mode, so adding it here leaves the
// render mode the caller asked for alone. See IntegerAnswer in
// src/fontconfig.cpp.
static FT_Int32 UnhintedLoadFlags(const FT_Int32 load_flags)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (dwcft::ParityActive()) {
        return load_flags | FT_LOAD_NO_HINTING;
    }
#endif
    return load_flags;
}

// Watched to learn which glyph an outline holds, and to keep the load unhinted
// under the hintslight answer. FT_Load_Char is interposed alongside because its
// internal call to FT_Load_Glyph does not go through this symbol.
FT_Error FT_Load_Glyph(FT_Face face, const FT_UInt glyph_index, const FT_Int32 load_flags)
{
    ft_load_glyph_fn real = real_FT_Load_Glyph();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    FT_Int32 flags = UnhintedLoadFlags(load_flags);
    if ((flags & FT_LOAD_NO_BITMAP) != 0 && StrikeBelongsToThisLoad(face)) {
        flags &= ~FT_LOAD_NO_BITMAP;
    }
    if (SkewedLoadSkipsStrike(face)) {
        flags |= FT_LOAD_NO_BITMAP;
    }
    // Before the load, not after: FreeType applies an FT_Set_Transform delta
    // by calling FT_Outline_Translate from inside FT_Load_Glyph, and that
    // translate does belong to the glyph being loaded. Clearing first drops
    // only what the previous occupant of this slot left behind.
    if (face != nullptr && face->glyph != nullptr) {
        DiscardOutlineState(&face->glyph->outline);
    }
    // ReSharper disable once CppLocalVariableWithNonTrivialDtorIsNeverUsed
    InRealFreeType inside(face != nullptr && face->glyph != nullptr ? &face->glyph->outline
                                                                     : nullptr);
    const FT_Error error = real(face, glyph_index, flags);
    if (error == 0) {
#if CLEARTYPE_FIREFOX_PARITY
        g_last_loaded_face = face;
#endif
        if (face != nullptr && face->glyph != nullptr) {
            SetPendingOutlineFace(&face->glyph->outline, face);
#if CLEARTYPE_FIREFOX_PARITY
            if (dwcft::ParityActive()) {
                RecordTransformDelta(face);
            }
#endif
        }
        RecordOutlineOwner(face, glyph_index);
        ApplyWindowsAdvance(face);
        ++g_load_applied;
    }
    return error;
}

FT_Error FT_Load_Char(FT_Face face, const FT_ULong char_code, const FT_Int32 load_flags)
{
    ft_load_char_fn real = real_FT_Load_Char();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
    if (face != nullptr && face->glyph != nullptr) {
        DiscardOutlineState(&face->glyph->outline);
    }
    const unsigned applied_before = g_load_applied;
    // ReSharper disable once CppLocalVariableWithNonTrivialDtorIsNeverUsed
    InRealFreeType inside(face != nullptr && face->glyph != nullptr ? &face->glyph->outline
                                                                     : nullptr);
    const FT_Error error = real(face, char_code, UnhintedLoadFlags(load_flags));
    if (error == 0 && g_load_applied == applied_before && face != nullptr &&
        face->glyph != nullptr) {
        SetPendingOutlineFace(&face->glyph->outline, face);
#if CLEARTYPE_FIREFOX_PARITY
        if (dwcft::ParityActive()) {
            RecordTransformDelta(face);
        }
#endif
        RecordOutlineOwner(face, face->glyph->glyph_index);
        ApplyWindowsAdvance(face);
    }
    return error;
}

// Only watched, never changed. The one call Gecko makes with a face's own em
// size and y_scale is the synthetic-bold strength for the glyph just loaded,
// which is what says the instance that asked for it is emboldened.
FT_Long FT_MulFix(const FT_Long a, const FT_Long b)
{
    const FT_Long product = MulFix(a, b);
    ApplyWindowsBoldAdvance(a, b, product);
    return product;
}

// The second interception point.
//
// Rasterizers that manage their own glyph atlas - WebRender, and so Firefox
// in its headless and print paths - never call FT_Render_Glyph. They load a
// glyph, allocate a bitmap themselves, and call this to fill it. The bitmap's
// pixel_mode is what says whether subpixel output was asked for.
//
// The placement arithmetic follows FreeType's own convention for this
// function. The target bitmap's bottom-left corner sits at outline coordinate
// (0,0), so bitmap column j covers outline x in [j*64, (j+1)*64) and row i
// covers outline y in [(rows-1-i)*64, (rows-i)*64).
//
// The pen origin is therefore column 0, row `rows`. A DirectWrite glyph whose
// box starts `left` pixels right of the pen and `top` pixels above the
// baseline goes at (left, rows - top). Anything landing outside is clipped,
// which is what real FreeType does with an outline the caller did not
// translate into range.

FT_Error FT_Set_Pixel_Sizes(FT_Face face, const FT_UInt pixel_width, const FT_UInt pixel_height)
{
    ft_set_pixel_sizes_fn real = real_FT_Set_Pixel_Sizes();
    if (real == nullptr) {
        return FT_Err_Unimplemented_Feature;
    }
    const unsigned applied_before = g_size_applied;
    const FT_Error error = real(face, pixel_width, pixel_height);
    if (error == 0 && g_size_applied == applied_before) {
        // Whole pixels by construction, so there is nothing for the exact-size
        // path to recover; record it anyway so a stale value from an earlier
        // FT_Set_Char_Size cannot be picked up.
        RecordRequestedEmSize(face,
                              static_cast<FT_Fixed>(pixel_height != 0 ? pixel_height : pixel_width) << 6);
        ApplyWindowsMetrics(face, "FT_Set_Pixel_Sizes");
    }
    return error;
}

FT_Error FT_Request_Size(FT_Face face, FT_Size_Request req)
{
    ft_request_size_fn real = real_FT_Request_Size();
    if (real == nullptr) {
        return FT_Err_Unimplemented_Feature;
    }
    const FT_Error error = real(face, req);
    if (error == 0) {
        // Only the nominal request carries a size this can use; the others
        // ask FreeType to fit a dimension, and what it picks is its own.
        FT_Fixed requested = 0;
        if (req != nullptr && req->type == FT_SIZE_REQUEST_TYPE_NOMINAL) {
            requested = PixelSize26_6(req->height != 0 ? req->height : req->width,
                                      req->vertResolution != 0 ? req->vertResolution
                                                               : req->horiResolution);
        }
        RecordRequestedEmSize(face, requested);
        // Not while FreeType is mid-load. The bytecode interpreter reaches this
        // entry point through the GOT, and rewriting face->size->metrics there
        // changes them under the interpreter that is reading them.
        if (g_in_real_freetype == 0) {
            ApplyWindowsMetrics(face, "FT_Request_Size");
            ++g_size_applied;
        }
    }
    return error;
}

FT_Error FT_Set_Char_Size(FT_Face face, const FT_F26Dot6 char_width, const FT_F26Dot6 char_height,
                          const FT_UInt horz_resolution, const FT_UInt vert_resolution)
{
    ft_set_char_size_fn real = real_FT_Set_Char_Size();
    if (real == nullptr) {
        return FT_Err_Unimplemented_Feature;
    }
#if CLEARTYPE_FIREFOX_PARITY
    // Ahead of FreeType, because setting the size is what fills the metrics in,
    // through the nested FT_Request_Size that returns early below. The claim
    // has to be in place before that runs, not after it.
    const double wr_px = WebRenderInstanceSize(char_width == char_height ? char_width : 0);
    if (wr_px > 0.0 && face != nullptr && FT_IS_SCALABLE(face)) {
        RecordClaimedSize(face, wr_px, wr_px < 1.0 ? 1.0 : wr_px, /*from_adjusted=*/true);
    }
#endif
    const unsigned applied_before = g_size_applied;
    const FT_Error error = real(face, char_width, char_height, horz_resolution, vert_resolution);
    if (error != 0) {
        return error;
    }
    if (g_size_applied != applied_before) {
        return error;                        // the nested FT_Request_Size did it
    }

    RecordRequestedEmSize(face, PixelSize26_6(char_height != 0 ? char_height : char_width,
                                              vert_resolution != 0 ? vert_resolution
                                                                   : horz_resolution));
#if CLEARTYPE_FIREFOX_PARITY
    // Square by construction is what gfxFT2FontBase::LockFTFace asks for; a
    // scaler that split the two axes is not naming a font's own size.
    if (char_width == char_height) {
        TouchClaimedSize(face, PixelSize26_6(char_height, vert_resolution != 0 ? vert_resolution
                                                                               : horz_resolution));
    }
#endif
    ApplyWindowsMetrics(face, "FT_Set_Char_Size");
    return error;
}

// Hands Firefox a per-size OS/2 table.
//
// InitMetrics derives the external leading from the OS/2 typo metrics:
//
//     typoHeight = sTypoAscender - sTypoDescender + sTypoLineGap;
//     lineHeight = typoHeight * yScale;                    // yScale = ppem/upem
//     lineHeight = floor(max(lineHeight, maxHeight) + 0.5);
//     externalLeading = lineHeight - internalLeading - emHeight;
//
// The line gap that produces Windows' answer is therefore a function of the
// pixel size, while sTypoLineGap is a property of the face, so writing one
// into the other would make every size of a family fight over a single field.
//
// The whole struct is substituted per call instead. The size is current at
// this point, and Firefox re-fetches the table instead of caching it, so each
// metrics computation gets the copy meant for it.
//
// The font itself is untouched, which also means the raw-table path that
// shaping uses, FT_Load_Sfnt_Table, still sees the real OS/2.

// What Windows would have put in gfxFont::Metrics for that face, for the two
// underline fields and the three that identify the struct.
//
// gfxDWriteFont::ComputeMetrics writes underlinePosition and underlineThickness
// times mFUnitsConvFactor; gfxFT2FontBase::InitMetrics derives both from
// FreeType's own scaled post table, and calls SanitizeMetrics with a literal
// false, so the branch that lowers the underline for a family on Firefox's
// bad-underline list is not even compiled into a Linux build. Both differences
// are answered here, for every face. See cleartype/src/shim_exports.h for what
// the out-parameters mean.
extern "C" void CleartypeEndInitMetrics(void)
{
#if CLEARTYPE_FIREFOX_PARITY
    g_last_sfnt_face = nullptr;
    g_claimed_em_26_6 = 0;
    g_claimed_em_px = 0.0;
#endif
}

extern "C" int CleartypeWindowsUnderline(double* underline_offset, double* underline_size,
                                         double* em_height, double* max_ascent,
                                         double* max_descent, double* descent_fold)
{
#if CLEARTYPE_FIREFOX_PARITY
    // g_last_sfnt_face is a bare FT_Face that FT_Get_Sfnt_Table remembers and
    // nothing clears when the face dies, so nothing may dereference it until
    // the face table has confirmed it is still live. That table is the
    // authority, and it answers only under the lock.
    FT_Face face = g_last_sfnt_face;
    if (face == nullptr || !WindowsMetrics()) {
        return 0;
    }
    // Resolve the system font collection before locking. Building it under
    // g_faces_mutex deadlocks; see WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;                            // the face is gone
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (face->size == nullptr ||
        !GetEmSizeClaimed(face, entry->requested_em_26_6, &em_size, &x_over_y) ||
        !(em_size > 0.0)) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    const WinInstance* inst = GetWinInstanceLocked(entry, em_size);
    int answered = 0;
    if (inst != nullptr && inst->valid) {
        const WinMetrics& m = inst->metrics;
        const double fold = inst->descent_fold ? 0.5 : 0.0;
        *underline_offset = m.underlineOffset;
        *underline_size = m.underlineSize;
        *em_height = m.emHeight;
        *max_ascent = m.maxAscent - fold;
        *max_descent = m.maxDescent + fold;
        *descent_fold = fold;
        answered = 1;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return answered;
#else
    (void)underline_offset; (void)underline_size; (void)em_height;
    (void)max_ascent; (void)max_descent; (void)descent_fold;
    return 0;
#endif
}

// What Windows would have put in gfxFont::Metrics for the two strikeout
// fields, with the three that identify the struct.
//
// gfxFT2FontBase::InitMetrics finishes its strikeout block with
// SnapLineToPixels, which takes the thickness to a whole number of pixels and
// moves the offset by half the change to keep the line centered, then rounds
// that too. gfxDWriteFont::ComputeMetrics writes
// strikethroughPosition * mFUnitsConvFactor and
// strikethroughThickness * mFUnitsConvFactor and leaves them fractional, so a
// line-through at a size whose thickness lands between one and two pixels is a
// pixel thicker here, and one whose snapped offset crosses a pixel boundary
// sits a row away.
//
// Answers for every face, since nothing about the snap is per-family.
extern "C" int CleartypeWindowsStrikeout(double* strikeout_offset, double* strikeout_size,
                                         double* em_height, double* max_ascent,
                                         double* max_descent)
{
#if CLEARTYPE_FIREFOX_PARITY
    FT_Face face = g_last_sfnt_face;
    if (face == nullptr || !WindowsMetrics()) {
        return 0;
    }
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (face->size == nullptr ||
        !GetEmSizeClaimed(face, entry->requested_em_26_6, &em_size, &x_over_y) ||
        !(em_size > 0.0)) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    const WinInstance* inst = GetWinInstanceLocked(entry, em_size);
    int answered = 0;
    if (inst != nullptr && inst->valid) {
        const WinMetrics& m = inst->metrics;
        const double fold = inst->descent_fold ? 0.5 : 0.0;
        *strikeout_offset = m.strikeoutOffset;
        *strikeout_size = m.strikeoutSize;
        *em_height = m.emHeight;
        *max_ascent = m.maxAscent - fold;
        *max_descent = m.maxDescent + fold;
        answered = 1;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return answered;
#else
    (void)strikeout_offset; (void)strikeout_size; (void)em_height;
    (void)max_ascent; (void)max_descent;
    return 0;
#endif
}

// Windows' x-height and cap-height for the face last measured, or 0 when there
// is none.
//
// gfxDWriteFont::ComputeMetrics writes xHeight and capHeight as the OS/2
// sxHeight and sCapHeight times mFUnitsConvFactor, both left fractional.
// gfxFT2FontBase::InitMetrics takes them from the FreeType face at the size
// FreeType was given, which is a whole number of 1/64 px, so for any size that
// is not one of those the two disagree by a fraction of a pixel.
//
// Small as that is, vertical-align: middle places an inline box against half
// the parent's x-height, so a line carrying one differs by half the error, and
// a page of such lines accumulates it until a row crosses a device pixel and
// the text under it is drawn a pixel out. The other seventeen fields of the
// metrics struct are already answered; these two are the pair that was left.
//
// Answers for every face, since nothing about the conversion is per-family.
extern "C" int CleartypeWindowsXCapHeight(double* x_height, double* cap_height,
                                          double* em_height, double* max_ascent,
                                          double* max_descent)
{
#if CLEARTYPE_FIREFOX_PARITY
    FT_Face face = g_last_sfnt_face;
    if (face == nullptr || !WindowsMetrics()) {
        return 0;
    }
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (face->size == nullptr ||
        !GetEmSizeClaimed(face, entry->requested_em_26_6, &em_size, &x_over_y) ||
        !(em_size > 0.0)) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    const WinInstance* inst = GetWinInstanceLocked(entry, em_size);
    int answered = 0;
    if (inst != nullptr && inst->valid) {
        const WinMetrics& m = inst->metrics;
        const double fold = inst->descent_fold ? 0.5 : 0.0;
        *x_height = m.xHeight;
        *cap_height = m.capHeight;
        *em_height = m.emHeight;
        *max_ascent = m.maxAscent - fold;
        *max_descent = m.maxDescent + fold;
        answered = 1;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return answered;
#else
    (void)x_height; (void)cap_height; (void)em_height;
    (void)max_ascent; (void)max_descent;
    return 0;
#endif
}

// The font-units-to-pixels factor, as each platform sets it.
//
// gfxFT2FontBase::InitMetrics writes
// FLOAT_FROM_26_6(FLOAT_FROM_16_16(ftMetrics.x_scale)), and FreeType built
// that scale from the size rounded to 1/64 px, so it carries the rounding.
// gfxDWriteFont::ComputeMetrics writes mAdjustedSize / designUnitsPerEm and
// carries none. The field is a COLR glyph's font-unit scale, so the difference
// puts a gradient stop a fraction of a pixel away and moves the levels either
// side of it.
//
// The FreeType value is returned as well, since the caller finds the field by
// matching it. It comes from the instance, since the face carries whatever
// size was set on it last.
// The Windows maxAscent and maxDescent for the face InitMetrics is measuring,
// at a size other than the one FreeType was set to.
//
// gfxFT2FontBase::FindClosestSize clamps mFTSize to a whole pixel, FreeType
// having no size below one, so a font smaller than that is measured against a
// whole pixel's FreeType metrics while gfxDWriteFont::ComputeMetrics rounds
// the size it was given and reaches zero. mAdjustedSize is the only place the
// size survives, and libxul_patch.cpp reads it out of the font to ask here.
//
// Called while the face is still claimed, so before CleartypeEndInitMetrics.
extern "C" int CleartypeWindowsMetricsAtSize(const double size, double* asc, double* desc)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (!(size > 0.0) || asc == nullptr || desc == nullptr || !WindowsMetrics()) {
        return 0;
    }
    FT_Face face = g_last_sfnt_face;
    if (face == nullptr) {
        return 0;
    }
    // Resolving the collection under g_faces_mutex deadlocks; see
    // WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    WinInstance* inst = entry != nullptr ? GetWinInstanceLocked(entry, size) : nullptr;
    const bool ok = inst != nullptr && inst->valid;
    if (ok) {
        *asc = inst->metrics.maxAscent;
        *desc = inst->metrics.maxDescent;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return ok ? 1 : 0;
#else
    (void)size;
    (void)asc;
    (void)desc;
    return 0;
#endif
}

extern "C" int CleartypeWindowsUnitsPerPixel(double* linux_factor, double* windows_factor,
                                             double* em_height, double* max_ascent,
                                             double* max_descent)
{
#if CLEARTYPE_FIREFOX_PARITY
    FT_Face face = g_last_sfnt_face;
    if (face == nullptr || !WindowsMetrics()) {
        return 0;
    }
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (face->size == nullptr ||
        !GetEmSizeClaimed(face, entry->requested_em_26_6, &em_size, &x_over_y) ||
        !(em_size > 0.0)) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    const WinInstance* inst = GetWinInstanceLocked(entry, em_size);
    // ApplyWindowsMetrics records the scale when it runs at this size, and it
    // does not run for every instance. The face still carries a scale, but only
    // for whatever size was set on it last, so it is used only when that size
    // is this instance's.
    FT_Fixed x_scale_26_6 = inst != nullptr ? inst->ft_x_scale : 0;
    if (x_scale_26_6 == 0 && inst != nullptr) {
        double live_size = 0.0;
        float live_ratio = 1.0f;
        if (GetEmSize(face, &live_size, &live_ratio) && live_size > 0.0 &&
            static_cast<FT_Fixed>(std::floor(live_size * 64.0 + 0.5)) == inst->size_26_6) {
            x_scale_26_6 = face->size->metrics.x_scale;
        }
    }
    int answered = 0;
    if (inst != nullptr && inst->valid && x_scale_26_6 != 0) {
        const WinMetrics& m = inst->metrics;
        const double fold = inst->descent_fold ? 0.5 : 0.0;
        const double x_scale = static_cast<double>(x_scale_26_6);
        *linux_factor = static_cast<double>(static_cast<float>(x_scale / 65536.0 / 64.0));
        *windows_factor = static_cast<double>(inst->funits_conv);
        *em_height = m.emHeight;
        *max_ascent = m.maxAscent - fold;
        *max_descent = m.maxDescent + fold;
        answered = 1;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return answered;
#else
    (void)linux_factor; (void)windows_factor; (void)em_height;
    (void)max_ascent; (void)max_descent;
    return 0;
#endif
}

// What Windows would have put in gfxFont::Metrics for the two leadings, with
// the three fields that identify the struct.
//
// gfxDWriteFont::ComputeMetrics clamps:
//
//     internalLeading = max(maxHeight - emHeight, 0)
//     externalLeading = ceil(lineGap * mFUnitsConvFactor)
//
// gfxFT2FontBase::InitMetrics derives instead, and does not clamp:
//
//     internalLeading = floor(maxHeight - emHeight + 0.5)
//     externalLeading = lineHeight - internalLeading - emHeight
//
// The two agree for a face whose ascent and descent overflow the em, which is
// most text faces. They disagree for one that does not fill it, and Firefox's
// own bullet font is such a face. Windows clamps its internal leading to zero
// and the external leading is zero with it, so GetNormalLineHeight in
// ReflowInput.cpp takes its `if (!internalLeading && !externalLeading)` branch
// and returns emHeight * 1.2. Linux carries a negative internal leading into
// the same test and takes the sum, which comes out the shorter of the two.
//
// Nothing fed through the size metrics or the OS/2 copy can produce the clamp,
// because both leadings are derived from values that also place the baseline.
// So this is corrected after the fact, the same way the underline is.
extern "C" int CleartypeWindowsLeading(double* internal_leading, double* external_leading,
                                       double* em_height, double* max_ascent,
                                       double* max_descent, double* linux_external)
{
#if CLEARTYPE_FIREFOX_PARITY
    // g_last_sfnt_face is a bare FT_Face that FT_Get_Sfnt_Table remembers and
    // nothing clears when the face dies, so nothing may dereference it until
    // the face table has confirmed it is still live. That table is the
    // authority, and it answers only under the lock.
    FT_Face face = g_last_sfnt_face;
    if (face == nullptr || !WindowsMetrics()) {
        return 0;
    }
    // Resolve the system font collection before locking. Building it under
    // g_faces_mutex deadlocks; see WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;                            // the face is gone
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (face->size == nullptr ||
        !GetEmSizeClaimed(face, entry->requested_em_26_6, &em_size, &x_over_y) ||
        !(em_size > 0.0)) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    const WinInstance* inst = GetWinInstanceLocked(entry, em_size);
    int answered = 0;
    if (inst != nullptr && inst->valid) {
        const WinMetrics& m = inst->metrics;
        const double fold = inst->descent_fold ? 0.5 : 0.0;
        *internal_leading = m.internalLeading;
        *external_leading = m.externalLeading;
        *em_height = m.emHeight;
        *max_ascent = m.maxAscent - fold;
        *max_descent = m.maxDescent + fold;
        *linux_external = LinuxExternalLeadingLocked(face, *inst);
        answered = 1;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return answered;
#else
    (void)internal_leading; (void)external_leading; (void)em_height;
    (void)max_ascent; (void)max_descent; (void)linux_external;
    return 0;
#endif
}

// Windows' average character width and maximum advance, which size a text
// control and place nothing.
//
// gfxDWriteFont::ComputeMetrics scales OS/2 xAvgCharWidth and keeps the
// fraction. gfxFT2FontBase::InitMetrics scales the same field through
// ScaleRoundDesignUnits, which rounds it to whole pixels "as this is compared
// with maxAdvance to guess whether this is a fixed width font". Both then
// raise it to the advance of '0' where that is wider, so the two part only for
// a face whose average beats its zero glyph. Consolas is one, 1126/2048 of an
// em being 8.797 pixels at 16 where Linux reads 9, and nsTextControlFrame
// multiplies the difference by the control's size attribute.
//
// The rounding is applied after the table is read, so the OS/2 copy cannot
// reach it.
extern "C" int CleartypeWindowsCharWidth(double* ave_char_width, double* max_advance,
                                         double* em_height, double* max_ascent,
                                         double* max_descent)
{
#if CLEARTYPE_FIREFOX_PARITY
    // g_last_sfnt_face is a bare FT_Face that FT_Get_Sfnt_Table remembers and
    // nothing clears when the face dies, so nothing may dereference it until
    // the face table has confirmed it is still live. That table is the
    // authority, and it answers only under the lock.
    FT_Face face = g_last_sfnt_face;
    if (face == nullptr || !WindowsMetrics()) {
        return 0;
    }
    // Resolve the system font collection before locking. Building it under
    // g_faces_mutex deadlocks; see WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;                            // the face is gone
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (face->size == nullptr ||
        !GetEmSizeClaimed(face, entry->requested_em_26_6, &em_size, &x_over_y) ||
        !(em_size > 0.0)) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    const WinInstance* inst = GetWinInstanceLocked(entry, em_size);
    int answered = 0;
    if (inst != nullptr && inst->valid && inst->metrics.aveCharWidth > 0.0) {
        const WinMetrics& m = inst->metrics;
        const double fold = inst->descent_fold ? 0.5 : 0.0;
        *ave_char_width = m.aveCharWidth;
        *max_advance = m.maxAdvance;
        *em_height = m.emHeight;
        *max_ascent = m.maxAscent - fold;
        *max_descent = m.maxDescent + fold;
        answered = 1;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return answered;
#else
    (void)ave_char_width; (void)max_advance; (void)em_height;
    (void)max_ascent; (void)max_descent;
    return 0;
#endif
}

// The whole-pixel size gfxDWriteFont::ComputeMetrics rounds mAdjustedSize to
// for the face last measured, or 0 when that face keeps the size it was asked
// for. The three identifying fields come back the way the accessors above
// return them, so the caller can confirm both answers are about one instance.
//
// space_width, zero_width and ideographic_width come with it.
// gfxFT2FontBase::InitMetrics takes those three from advances of its own,
// measured before the caller has rounded anything. Windows measures all three
// through the rounded instance, so its own values go in instead.
extern "C" int CleartypeWindowsStrikeSize(double* rounded, double* unrounded,
                                         double* space_width, double* zero_width,
                                         double* ideographic_width, double* em_height,
                                         double* max_ascent, double* max_descent)
{
#if CLEARTYPE_FIREFOX_PARITY
    // g_last_sfnt_face is a bare FT_Face that FT_Get_Sfnt_Table remembers and
    // nothing clears when the face dies, so nothing may dereference it until
    // the face table has confirmed it is still live. That table is the
    // authority, and it answers only under the lock.
    FT_Face face = g_last_sfnt_face;
    if (face == nullptr || !WindowsMetrics()) {
        return 0;
    }
    // Resolve the system font collection before locking. Building it under
    // g_faces_mutex deadlocks; see WarmSystemCollection.
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;                            // the face is gone
    }
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (face->size == nullptr ||
        !GetEmSizeClaimed(face, entry->requested_em_26_6, &em_size, &x_over_y) ||
        !(em_size > 0.0)) {
        pthread_mutex_unlock(&g_faces_mutex);
        return 0;
    }
    const WinInstance* inst = GetWinInstanceLocked(entry, em_size);
    int answered = 0;
    if (inst != nullptr && inst->valid && inst->bitmap_font &&
        !SizesMatch(inst->adjusted_size, em_size)) {
        const WinMetrics& m = inst->metrics;
        const double fold = inst->descent_fold ? 0.5 : 0.0;
        *rounded = inst->adjusted_size;
        *unrounded = em_size;
        *space_width = m.spaceWidth;
        *zero_width = m.zeroWidth;
        *ideographic_width = m.ideographicWidth;
        *em_height = m.emHeight;
        *max_ascent = m.maxAscent - fold;
        *max_descent = m.maxDescent + fold;
        answered = 1;
    }
    pthread_mutex_unlock(&g_faces_mutex);
    return answered;
#else
    (void)rounded; (void)unrounded; (void)space_width; (void)zero_width;
    (void)ideographic_width; (void)em_height; (void)max_ascent; (void)max_descent;
    return 0;
#endif
}

// Names the face the running InitMetrics is about, read out of the font itself.
//
// The accessors below answer for g_last_sfnt_face, the face whose sfnt tables
// were handed out most recently, and a font whose tables Gecko already held
// reads none, so they answer about whichever face came before it. Refused
// unless the face table already knows the pointer, so a wrong guess at the
// object's layout claims nothing.
extern "C" int CleartypeClaimFace(void* candidate, double ft_size)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (candidate == nullptr || !WindowsMetrics()) {
        return 0;
    }
    auto* face = static_cast<FT_Face>(candidate);
    pthread_mutex_lock(&g_faces_mutex);
    const bool known = FindFaceLocked(face) != nullptr;
    pthread_mutex_unlock(&g_faces_mutex);
    if (!known) {
        return 0;
    }
    g_last_sfnt_face = face;
    // gfxFT2FontBase::LockFTFace converts mFTSize this way, so the instance
    // named here is the one the face was set to for this font.
    g_claimed_em_26_6 =
        ft_size > 0.0 && ft_size < 65536.0
            // NOLINTNEXTLINE(bugprone-incorrect-roundings)  -- LockFTFace truncates
            ? static_cast<FT_Fixed>(ft_size * 64.0 + 0.5)
            : 0;
    // mFTSize is FindClosestSize(face, GetAdjustedSize()), which is the
    // adjusted size itself only for a scalable face at a whole pixel or more.
    // For a bitmap face it is whichever strike was chosen, and below one pixel
    // FindClosestSize clamps to 1.0 for every face, since FreeType would clamp
    // the ppem there anyway; gfxFT2FontBase scales glyph extents back from it
    // afterwards, but InitMetrics takes the ascent and descent from the
    // clamped size without that correction. So a size under a pixel arrives
    // here as 1.0 and the true one is only in mAdjustedSize.
    g_claimed_em_px = g_claimed_em_26_6 > 0 && FT_IS_SCALABLE(face) ? ft_size : 0.0;
    if (g_claimed_em_px > 0.0) {
        RecordClaimedSize(face, g_claimed_em_px, ft_size, /*from_adjusted=*/false);
    }
    return 1;
#else
    (void)candidate;
    (void)ft_size;
    return 0;
#endif
}

extern "C" int CleartypeClaimSize(void* candidate, double px)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (candidate == nullptr || !WindowsMetrics()) {
        return 0;
    }
    auto* face = static_cast<FT_Face>(candidate);
    pthread_mutex_lock(&g_faces_mutex);
    const bool known = FindFaceLocked(face) != nullptr;
    pthread_mutex_unlock(&g_faces_mutex);
    if (!known || !FT_IS_SCALABLE(face)) {
        return 0;
    }
    // What gfxFT2FontBase::FindClosestSize answers for a scalable face, which
    // is the size itself and a pixel where the size is under one. The caller
    // runs before InitMetrics, so mFTSize does not hold it yet.
    RecordClaimedSize(face, px, px < 1.0 ? 1.0 : px, /*from_adjusted=*/true);
    return 1;
#else
    (void)candidate;
    (void)px;
    return 0;
#endif
}

// The ink box for one glyph, in pixels and without the 1/64 step the slot's
// metrics store it in. Four edges: left, top, right, bottom. `embolden` is the
// font's mEmbolden, which decides whether the top and the right come off the
// face DirectWrite simulates bold on, as they do in the slot.
//
// gfxFT2FontBase::GetFTGlyphExtents rebuilds its box out of those metrics, so
// the edges SetGlyphInkBox wrote arrive rounded. gfxFont::Measure then extends
// a synthetically obliqued box by ceil(skew * edge) in app units, where half a
// 1/64 px moves the ceiling a whole app unit and 1/60 px of ink with it. The
// caller puts these edges back; see SubstituteInkBox in libxul_patch.cpp.
//
// `ft_size` is the font's mFTSize, and everything read here is either that or
// per-face state, never the size or the variation the face carries right now.
// This runs after gfxFT2FontBase::GetFTGlyphExtents has let the face lock go,
// so another font sharing the face may already have re-set both, and a lookup
// that read them would answer differently on two threads.
//
// A variable face is refused for the same reason. Its instance is keyed on the
// axis position, which is read off the face and is that same live state.
//
// The instance is the one the glyph load that came just before built. Nothing
// is built here, because building reads the face's FreeType state and evicts
// the oldest instance, and neither belongs to a caller that is only asking.
// gfxHarfBuzzShaper::GetGlyphVOrigin's horizontal origin for one glyph, in
// 16.16, as Windows computes it.
//
// GetGlyphVOrigin sets it to half the advance GetGlyphHAdvance answers with,
// and the two platforms answer that from different places.
// gfxDWriteFont::ProvidesGlyphWidths is false while mUseSubpixelPositions
// holds and the face carries no bold simulation, so the shaper reads the hmtx
// table and the advance carries no synthetic bold; gfxFont::Draw adds that as
// cluster tracking afterwards, which widens the cluster and moves no origin.
// gfxFT2FontBase::ProvidesGlyphWidths is true for every font, and
// gfxFT2FontBase::GetFTGlyphExtents adds the embolden strength to the advance
// it reads, so half of it lands on the origin of every upright glyph in
// vertical text.
//
// `embolden` is the font's mEmbolden, which decides whether Windows would be
// simulating the bold on the face the shaper measures through.
extern "C" int CleartypeWindowsVOriginX(void* candidate, double ft_size, int embolden,
                                        unsigned glyph, int32_t* x)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (candidate == nullptr || x == nullptr || !WindowsMetrics()) {
        return 0;
    }
    auto* face = static_cast<FT_Face>(candidate);
    if (!(ft_size > 0.0) || !(ft_size < 65536.0)) {
        return 0;
    }
    double advance = -1.0;
    pthread_mutex_lock(&g_faces_mutex);
    if (FaceEntry* entry = FindFaceLocked(face); entry != nullptr && FT_IS_SCALABLE(face)) {
        // The same size resolution CleartypeGlyphInkBox does, for the same
        // reason: the face carries whichever size touched it last.
        // NOLINTNEXTLINE(bugprone-incorrect-roundings)  -- LockFTFace truncates
        const auto size_26_6 = static_cast<FT_Fixed>(ft_size * 64.0 + 0.5);
        double em_size = ft_size;
        double exact = 0.0;
        if (ClaimedSizeFor(face, size_26_6, &exact) || ExactEmSize(size_26_6, &exact) ||
            ReducedEmSize(size_26_6, &exact)) {
            em_size = exact;
        }
        const WinInstance* instance = FindWinInstanceLocked(entry, em_size);
        if (instance != nullptr && instance->valid && instance->dwrite_face != nullptr) {
            const bool has_variations = FT_HAS_MULTIPLE_MASTERS(face) != 0;
            // Where Windows fattens the outline the shaper measures through
            // the simulated face, which is the face carrying the BOLD
            // simulation ProvidesGlyphWidths tests for; the same two
            // exclusions as CleartypeGlyphInkBox decide it.
            advance = embolden != 0 && entry->memory == nullptr && !FaceHasCOLRLocked(entry)
                          ? WinBoldGlyphAdvanceLocked(entry, *instance,
                                                      static_cast<UINT16>(glyph), has_variations)
                          : WinGlyphAdvance(*instance, static_cast<UINT16>(glyph), has_variations);
        }
    }
    pthread_mutex_unlock(&g_faces_mutex);
    if (!(advance >= 0.0)) {
        return 0;
    }
    // hb_position_t is an int32 and the multiply by a half is a double, so the
    // store truncates toward zero.
    const auto fixed = static_cast<int32_t>(llround(advance * 65536.0));
    *x = static_cast<int32_t>(0.5 * static_cast<double>(fixed));
    return 1;
#else
    (void)candidate;
    (void)ft_size;
    (void)embolden;
    (void)glyph;
    (void)x;
    return 0;
#endif
}

extern "C" int CleartypeGlyphInkBox(void* candidate, double ft_size, int embolden,
                                    unsigned glyph, double* out)
{
#if CLEARTYPE_FIREFOX_PARITY
    if (candidate == nullptr || out == nullptr || !WindowsMetrics()) {
        return 0;
    }
    auto* face = static_cast<FT_Face>(candidate);
    if (!(ft_size > 0.0) || !(ft_size < 65536.0)) {
        return 0;
    }
    // The candidate is offered a word at a time and is not a face until the
    // table says so, so nothing reads through it before that.
    pthread_mutex_lock(&g_faces_mutex);
    WinInkBox box{};
    bool have = false;
    double bold_top = 0.0, bold_right = 0.0;
    bool have_bold = false;
    if (FaceEntry* entry = FindFaceLocked(face);
        entry != nullptr && FT_IS_SCALABLE(face) && !FT_HAS_MULTIPLE_MASTERS(face)) {
        // The size resolves out of the caller's own number and the table Gecko
        // filled when it named this font's face, so it does not depend on
        // which size the face happens to carry now.
        // NOLINTNEXTLINE(bugprone-incorrect-roundings)  -- LockFTFace truncates
        const auto size_26_6 = static_cast<FT_Fixed>(ft_size * 64.0 + 0.5);
        double em_size = ft_size;
        double exact = 0.0;
        if (ClaimedSizeFor(face, size_26_6, &exact) || ExactEmSize(size_26_6, &exact) ||
            ReducedEmSize(size_26_6, &exact)) {
            em_size = exact;
        }
        // Nothing else is tried when this misses. A face whose size Gecko
        // snapped to a bitmap strike carries a font size its Windows instance
        // was not built at, and the only thing linking the two is the request
        // recorded on the face, which by now is whichever font touched it
        // last. Reading it answers differently on two threads.
        const WinInstance* instance = FindWinInstanceLocked(entry, em_size);
        have = instance != nullptr && instance->valid &&
               WinGlyphInkBox(instance->dwrite_face, instance->funits_conv,
                              static_cast<UINT16>(glyph), &box);
        // Where Windows fattens the outline the top and the right come off the
        // simulated face, which is what ApplyWindowsBoldAdvance put in the
        // slot; the same two exclusions apply.
        if (have && embolden != 0 && entry->memory == nullptr && !FaceHasCOLRLocked(entry)) {
            have_bold = WinBoldGlyphInkLocked(entry, *instance, static_cast<UINT16>(glyph),
                                              &bold_top, &bold_right);
        }
    }
    pthread_mutex_unlock(&g_faces_mutex);
    FT_Pos x = 0, x2 = 0, y = 0, y2 = 0;
    if (!have || !InkBox26Dot6(box, &x, &x2, &y, &y2)) {
        return 0;                            // no box, or one the slot refused
    }
    out[0] = box.left;
    out[1] = have_bold ? bold_top : box.top;
    out[2] = have_bold ? bold_right : box.right;
    out[3] = box.bottom;
    return 1;
#else
    (void)candidate;
    (void)ft_size;
    (void)embolden;
    (void)glyph;
    (void)out;
    return 0;
#endif
}

void* FT_Get_Sfnt_Table(FT_Face face, const FT_Sfnt_Tag tag)
{
    ft_get_sfnt_table_fn real = real_FT_Get_Sfnt_Table();
    if (real == nullptr) {
        return nullptr;
    }
    void* table = real(face, tag);
#if CLEARTYPE_FIREFOX_PARITY
    // Nothing in gfx/wr reads an sfnt table, so a library reached here is not
    // WebRender's. Skia's cairo port reads head, post, PCLT and OS/2, and
    // gfxFT2FontList and gfxFT2FontBase read them from cairo's faces as well.
    //
    // FT_Outline_Get_Bitmap cannot claim a library whose first glyph comes
    // from an embedded strike, since such a glyph has no outline to hand it.
    // A CJK face carrying a strike is exactly the case where being taken for
    // WebRender draws the strike where SkScalerContext_win_dw draws the
    // outline, so the run comes out bilevel; this claims the library before
    // the glyph rather than from it. The shim's own table reads go straight to
    // the real entry point and do not arrive here.
    if (face != nullptr && face->glyph != nullptr) {
        RecordSkiaLibrary(face->glyph->library);
    }
#endif
    if (face == nullptr || !WindowsMetrics()) {
        return table;
    }
    // gfxFT2FontBase::InitMetrics reads head, OS/2 and post from the face it
    // is measuring, so this is the face it is about to write metrics for.
    // libxul_patch.cpp asks for it by the time InitMetrics returns.
    g_last_sfnt_face = face;
    if (tag == FT_SFNT_OS2) {
        return SubstituteOS2(face, table);
    }
    if (tag == FT_SFNT_POST) {
        return SubstitutePost(face, table);
    }
    return table;
}

// FORCED INTERCEPTION POINT. Not a translation of anything Windows does, but
// a hook on a FreeType call that Skia's cairo-FT port makes, placed so that
// the Windows behavior becomes reproducible through it.
//
// SkScalerContext_CairoFT::generateMetrics sizes a glyph's mask from
// FT_Outline_Get_CBox of the loaded outline, plus the subpixel offset,
// rounded out to pixels and outset by one column for LCD.
// SkScalerContextFTUtils::generateGlyphImage then clips the rasterizer's
// output to that mask. On Windows there is no such clip, because
// SkScalerContext_DW::generateDWMetrics sizes the mask from the DirectWrite
// texture bounds, so with DirectWrite rasterizing here any texture reaching
// past the control box loses its edge. A bitmap strike in GDI_CLASSIC mode
// does that, and so does a grid-fitted row above the cbox.
//
// The Skia caller therefore gets DirectWrite's texture bounds at phase zero,
// padded a pixel on each side for the quarter-pixel phases generateMetrics
// adds afterwards.
//
// WebRender keeps FreeType's answer. platform/unix/font.rs reads the cbox
// twice, in get_bounding_box and in rasterize_glyph_outline. Both reads
// describe the outline about to be rendered, not the bitmap that comes back,
// so DirectWrite's bounds would be wrong for either.
void FT_Outline_Get_CBox(const FT_Outline* outline, FT_BBox* acbox)
{
    ft_outline_get_cbox_fn real = real_FT_Outline_Get_CBox();
    if (real == nullptr) {
        if (acbox != nullptr) {
            std::memset(acbox, 0, sizeof(*acbox));
        }
        return;
    }
    real(outline, acbox);
#if CLEARTYPE_FIREFOX_PARITY
    // FreeType asks for this from inside FT_Load_Glyph and FT_Render_Glyph to
    // build slot->metrics and size its own bitmap, and its own answer is the
    // right one there. See the re-entry guard above.
    if (g_in_real_freetype != 0) {
        return;
    }
    if (!InterposerWanted() || outline == nullptr || acbox == nullptr || outline->n_contours == 0 ||
        std::getenv("CLEARTYPE_FORCE_FALLBACK") != nullptr) {
        return;
    }
    FT_Face face = nullptr;
    FT_UInt glyph_index = 0;
    if (!FindOutlineOwner(outline, &face, &glyph_index)) {
        return;
    }
    if (CallerForFace(face) != RasterCaller::Skia) {
        return;
    }
    // Measured for both targets and unioned, since which one the caller will
    // render is not known here. Skia clips the rasterizer to this box in
    // generateGlyphImage, so too large costs a little empty mask and too small
    // loses ink, and the union is the safe direction.
    //
    // `grayscale` reaches SkiaDWParamsLocked as its a8 flag, where it decides
    // the texture type, the antialias mode and which CreateGlyphRunAnalysis
    // overload runs, so the two answers bound different analyses. A gray
    // target is drawn by FreeType now and not by DirectWrite, and the gray
    // measurement stays because it bounds the same ink: on Arial 'H' at 48px
    // the two agree on 28x35 at (3,-35) to the pixel.
    DWriteGlyphImage image;
    bool measured = RasterizeThroughDWrite(face, glyph_index, outline, false, &image,
                                           /*measure_only=*/true);
    DWriteGlyphImage gray;
    if (RasterizeThroughDWrite(face, glyph_index, outline, true, &gray, /*measure_only=*/true)) {
        if (!measured) {
            image = std::move(gray);
            measured = true;
        } else {
            const int right = image.left + image.width;
            const int gray_right = gray.left + gray.width;
            const int bottom = image.top - image.height;
            const int gray_bottom = gray.top - gray.height;
            image.left = image.left < gray.left ? image.left : gray.left;
            image.top = image.top > gray.top ? image.top : gray.top;
            image.width = (right > gray_right ? right : gray_right) - image.left;
            image.height = image.top - (bottom < gray_bottom ? bottom : gray_bottom);
        }
    }
    if (!measured) {
        return;
    }
    acbox->xMin = (static_cast<FT_Pos>(image.left) << 6) - 64;
    acbox->xMax = (static_cast<FT_Pos>(image.left + image.width) << 6) + 64;
    acbox->yMax = (static_cast<FT_Pos>(image.top) << 6) + 64;
    acbox->yMin = (static_cast<FT_Pos>(image.top - image.height) << 6) - 64;
#endif
}

#if CLEARTYPE_FIREFOX_PARITY
// True where Skia fills the glyph with blitFatAntiRect, which is the one case
// its scan converter does not snap.
//
// SkScan::AAAFillPath takes the mask blitter when MaskAdditiveBlitter can hold
// the glyph - at most 32 pixels wide and SkAlign4(width) * height at most 1024
// bytes - and tries try_blit_fat_anti_rect first. That wants a path
// SkPathRaw::isRect accepts and a rounded-out width of at least 3, and it
// blits the rectangle from its own edges without ever building an
// SkAnalyticEdge, so no SnapY runs. Arial's 'l' takes this path and its
// neighbor 'i', two contours, does not.
static bool SkiaBlitsAsFatRect(const FT_Outline* outline, const FT_Bitmap* abitmap)
{
    constexpr unsigned kMaxWidth = 32;
    constexpr unsigned kMaxStorage = 1024;
    if (abitmap->width > kMaxWidth ||
        ((abitmap->width + 3u) & ~3u) * abitmap->rows > kMaxStorage) {
        return false;
    }
    if (outline->n_contours != 1 || outline->n_points < 4 || outline->n_points > 5) {
        return false;
    }
    // A closing point repeating the first is the five-point form.
    int corners = outline->n_points;
    if (corners == 5) {
        if (outline->points[4].x != outline->points[0].x ||
            outline->points[4].y != outline->points[0].y) {
            return false;
        }
        corners = 4;
    }
    for (int i = 0; i < outline->n_points; ++i) {
        if ((outline->tags[i] & FT_CURVE_TAG_ON) == 0) {
            return false;
        }
    }
    // Four axis-aligned edges that alternate, which is what a rectangle is.
    bool previous_horizontal = false;
    for (int i = 0; i < corners; ++i) {
        const FT_Vector& a = outline->points[i];
        const FT_Vector& b = outline->points[(i + 1) % corners];
        const bool horizontal = a.y == b.y && a.x != b.x;
        const bool vertical = a.x == b.x && a.y != b.y;
        if (!horizontal && !vertical) {
            return false;
        }
        if (i > 0 && horizontal == previous_horizontal) {
            return false;
        }
        previous_horizontal = horizontal;
    }
    FT_Pos left = outline->points[0].x;
    FT_Pos right = left;
    for (int i = 1; i < corners; ++i) {
        left = outline->points[i].x < left ? outline->points[i].x : left;
        right = outline->points[i].x > right ? outline->points[i].x : right;
    }
    // roundOut, in whole pixels, must leave a rectangle at least 3 wide.
    const long spans = (right + 63) / 64 - (left >= 0 ? left / 64 : (left - 63) / 64);
    return spans >= 3;
}

// Rounds an A8 glyph's outline to where Skia's own scan converter puts an
// edge, then rasterizes it through real FreeType.
//
// On Windows these masks come from the outline and not from DirectWrite:
// SkScalerContext::GenerateImageFromPath fills the path through
// SkScan::AntiFillPath, which supersamples four times vertically and computes
// exact horizontal coverage per subscanline, with SkEdge::setLine rounding
// each endpoint to the nearest subscanline. A horizontal edge therefore lands
// on a quarter pixel and the row it fringes carries a whole number of
// quarters, where FreeType would answer the true area. Rounding the y
// coordinates first makes the two agree.
//
// Measured on Arial 'H' at 48px in an <svg><text>: 175 differing pixels when
// the glyph came from DirectWrite's grayscale analysis, 29 from plain
// FreeType, 0 with this. Rendering four times as tall and averaging the bands
// was tried as well and is worse, since each band is quantized to a byte
// before the average and FreeType already integrates the whole pixel exactly.
//
// The caller's outline is restored before returning, since Skia reads the same
// slot again for the path.
static FT_Error SkiaScanlineBitmap(ft_outline_get_bitmap_fn real, FT_Library library,
                                   FT_Outline* outline, const FT_Bitmap* abitmap)
{
    if (outline->n_points <= 0 || outline->points == nullptr) {
        return real(library, outline, abitmap);
    }
    if (SkiaBlitsAsFatRect(outline, abitmap)) {
        return real(library, outline, abitmap);
    }
    std::vector<FT_Pos> saved(static_cast<size_t>(outline->n_points));
    for (int i = 0; i < outline->n_points; ++i) {
        saved[static_cast<size_t>(i)] = outline->points[i].y;
        // 26.6 units, so a quarter of a pixel is 16 of them. Rounded half away
        // from zero, which is what SkFDot6Round does to the supersampled
        // coordinate.
        const FT_Pos y = outline->points[i].y;
        outline->points[i].y = y >= 0 ? ((y + 8) & ~15) : -((-y + 8) & ~15);
    }
    const FT_Error err = real(library, outline, abitmap);
    for (int i = 0; i < outline->n_points; ++i) {
        outline->points[i].y = saved[static_cast<size_t>(i)];
    }
    return err;
}
#endif

#if CLEARTYPE_FIREFOX_PARITY

// ---------------------------------------------------------------------------
// The glyph outline Skia builds a path from.
//
// gfx/skia/skia/src/ports/SkFontHost_FreeType.cpp
// SkScalerContext_FreeType::generatePath walks the loaded outline with
// FT_Outline_Decompose and SkFTGeometrySink's callbacks. On Windows the same
// path comes from SkScalerContext_win_dw walking DirectWrite's outline. A
// stroked glyph, a COLR layer under a gradient and text past the size Skia
// keeps in an atlas are all drawn from that path, so wherever the two walks
// disagree those pixels do too.
//
// The curves themselves agree. GetGlyphRunOutline elevates the font's
// quadratics to cubics and
// SkDWriteGeometrySink::AddBeziers puts them back with check_quadratic, so
// Windows builds the path from the same quadratics FreeType holds. What is
// left is the coordinates. FT_Outline_Funcs carries FT_Vector, so a callback
// receives 26.6 whatever the outline holds, and SkFTGeometrySink divides by
// 64. That caps this route at 1/64 px, which is why the exact coordinate is
// recorded here and written into the finished path afterwards; see the
// generatePath thunk in libxul_patch.cpp.
// ---------------------------------------------------------------------------

// ID2D1SimplifiedGeometrySink, reconstructed. dwrite.h forward-declares it and
// defines IDWriteGeometrySink as an alias, and no header here completes it.
typedef enum D2D1_FILL_MODE
{
    D2D1_FILL_MODE_ALTERNATE = 0,
    D2D1_FILL_MODE_WINDING = 1,
} D2D1_FILL_MODE;

typedef enum D2D1_PATH_SEGMENT
{
    D2D1_PATH_SEGMENT_NONE = 0,
} D2D1_PATH_SEGMENT;

typedef enum D2D1_FIGURE_BEGIN
{
    D2D1_FIGURE_BEGIN_FILLED = 0,
    D2D1_FIGURE_BEGIN_HOLLOW = 1,
} D2D1_FIGURE_BEGIN;

typedef enum D2D1_FIGURE_END
{
    D2D1_FIGURE_END_OPEN = 0,
    D2D1_FIGURE_END_CLOSED = 1,
} D2D1_FIGURE_END;

typedef struct D2D1_BEZIER_SEGMENT
{
    D2D1_POINT_2F point1;
    D2D1_POINT_2F point2;
    D2D1_POINT_2F point3;
} D2D1_BEZIER_SEGMENT;

interface ID2D1SimplifiedGeometrySink : IUnknown
{
    STDMETHOD_(void, SetFillMode)(D2D1_FILL_MODE fillMode) PURE;
    STDMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT vertexFlags) PURE;
    STDMETHOD_(void, BeginFigure)(D2D1_POINT_2F startPoint, D2D1_FIGURE_BEGIN figureBegin) PURE;
    STDMETHOD_(void, AddLines)(const D2D1_POINT_2F* points, UINT32 pointsCount) PURE;
    STDMETHOD_(void, AddBeziers)(const D2D1_BEZIER_SEGMENT* beziers, UINT32 beziersCount) PURE;
    STDMETHOD_(void, EndFigure)(D2D1_FIGURE_END figureEnd) PURE;
    STDMETHOD(Close)() PURE;
};

// Armed for the length of one generatePath, by the thunk over it.
thread_local bool g_in_glyph_path = false;
thread_local std::vector<CleartypeGlyphPathPoint> g_glyph_path;
// Cleared as soon as anything about the walk stops being reproducible, so the
// finished path is left alone instead of half corrected.
thread_local bool g_glyph_path_usable = false;

// fSkXform, the matrix SkScalerContext_DW::generatePath applies to the
// finished path. computeMatrices splits the device matrix into a scale and a
// remainder, kVertical on Windows and kFull on Linux, so the two remainders
// differ by the pixel aspect alone: sA_vertical == sA_full * diag(x/y, 1).
// sA_full is fMatrix22Scalar, which reaches this library as the FT_Matrix
// SkScalerContext_FreeType::setupSize hands FT_Set_Transform.
struct PathTransform
{
    float scale_x = 1.0f;
    float skew_x = 0.0f;
    float skew_y = 0.0f;
    float scale_y = 1.0f;
};

static PathTransform SkXformFromFTMatrix(const FT_Matrix& m, const float x_over_y)
{
    PathTransform out;
    out.scale_x = static_cast<float>(static_cast<double>(m.xx) / 65536.0) * x_over_y;
    out.skew_x = static_cast<float>(static_cast<double>(-m.xy) / 65536.0);
    out.skew_y = static_cast<float>(static_cast<double>(-m.yx) / 65536.0) * x_over_y;
    out.scale_y = static_cast<float>(static_cast<double>(m.yy) / 65536.0);
    return out;
}

// Replays a glyph outline from GetGlyphRunOutline into an FT_Outline_Funcs
// caller, and records the points the caller's own sink will keep.
//
// SkFTGeometrySink drops a segment whose every point is where the pen already
// is, and emits the moveTo of a contour lazily, at the first segment that
// survives that test. Both decisions are made on the values this passes, so
// replaying them here gives the sequence the finished path holds, which is
// what lets the repair find it.
class DecomposeSink final : public IDWriteGeometrySink
{
public:
    DecomposeSink(const FT_Outline_Funcs* funcs, void* user, const PathTransform& xform)
        : funcs_(funcs), user_(user), xform_(xform) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        *out = this;
        AddRef();
        return S_OK;
    }
    // Stack-allocated for the length of one call, so the count is a formality.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override {}
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}

    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) override
    {
        if (error_ != 0 || funcs_->move_to == nullptr) {
            return;
        }
        started_ = false;
        cur_ = Point(start);
        cur_exact_ = start;
        error_ = funcs_->move_to(&cur_, user_);
    }

    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, const UINT32 count) override
    {
        if (funcs_->line_to == nullptr) {
            error_ = FT_Err_Invalid_Argument;
        }
        for (UINT32 i = 0; error_ == 0 && i < count; ++i) {
            if (!Differs(points[i])) {
                continue;                    // SkDWriteGeometrySink::AddLines drops it
            }
            FT_Vector to = Point(points[i]);
            Separate(&to);
            OpenContour();
            Record(to, points[i]);
            cur_ = to;
            cur_exact_ = points[i];
            error_ = funcs_->line_to(&to, user_);
        }
    }

    // SkDWriteGeometrySink::AddBeziers, which is the walk Windows builds this
    // path with. A cubic that is an elevated quadratic goes to the builder as
    // the quadratic it came from, so the verbs are the font's own either way.
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* segments, const UINT32 count) override
    {
        if (funcs_->cubic_to == nullptr || funcs_->conic_to == nullptr) {
            error_ = FT_Err_Invalid_Argument;
        }
        for (UINT32 i = 0; error_ == 0 && i < count; ++i) {
            const D2D1_BEZIER_SEGMENT& seg = segments[i];
            if (!Differs(seg.point1) && !Differs(seg.point2) && !Differs(seg.point3)) {
                continue;
            }
            D2D1_POINT_2F control{};
            const bool quadratic = CheckQuadratic(seg, &control);
            OpenContour();
            FT_Vector to = Point(seg.point3);
            if (quadratic) {
                FT_Vector c = Point(control);
                Separate(&c, &to);
                    Record(c, control);
                Record(to, seg.point3);
                error_ = funcs_->conic_to(&c, &to, user_);
            } else {
                FT_Vector c1 = Point(seg.point1);
                FT_Vector c2 = Point(seg.point2);
                Separate(&c1, &c2, &to);
                    Record(c1, seg.point1);
                Record(c2, seg.point2);
                Record(to, seg.point3);
                error_ = funcs_->cubic_to(&c1, &c2, &to, user_);
            }
            cur_ = to;
            cur_exact_ = seg.point3;
        }
    }

    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) override {}
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

    int error() const { return error_; }

private:
    // ftoutln.c walks the outline as SCALED(x) = (x << shift) - delta. Only
    // the zero case is answered for, which is what SkFTGeometrySink asks for,
    // so the recorded values are the caller's own.
    FT_Vector Point(const D2D1_POINT_2F p) const
    {
        const double px = static_cast<double>(p.x), py = static_cast<double>(p.y);
        const double x = px * static_cast<double>(xform_.scale_x) +
                         py * static_cast<double>(xform_.skew_x);
        const double y = py * static_cast<double>(xform_.scale_y) +
                         px * static_cast<double>(xform_.skew_y);
        return FT_Vector{ .x = static_cast<FT_Pos>(llround(x * 64.0)),
                          .y = static_cast<FT_Pos>(llround(-y * 64.0)) };
    }

    static bool Same(const FT_Vector& a, const FT_Vector& b) { return a.x == b.x && a.y == b.y; }

    // SkDWriteGeometrySink::currentIsNot, which compares DirectWrite's own
    // coordinates, unrounded.
    bool Differs(const D2D1_POINT_2F& p) const
    {
        return p.x != cur_exact_.x || p.y != cur_exact_.y;
    }

    // src/utils/SkFloatUtils.h SkFloatingPoint<float, 10>::AlmostEquals, the
    // comparison check_quadratic reduces with.
    static bool ApproximatelyEqual(const float a, const float b)
    {
        if (std::isnan(a) || std::isnan(b)) {
            return false;
        }
        uint32_t left = 0, right = 0;
        std::memcpy(&left, &a, sizeof(left));
        std::memcpy(&right, &b, sizeof(right));
        auto biased = [](const uint32_t sam) {
            constexpr uint32_t sign_bit = 1u << 31;
            return (sam & sign_bit) != 0 ? ~sam + 1 : sign_bit | sam;
        };
        const uint32_t x = biased(left), y = biased(right);
        return (x >= y ? x - y : y - x) <= 10;
    }

    // SkDWriteGeometrySink.cpp check_quadratic. A cubic whose control points
    // sit two thirds of the way to one point is that quadratic elevated, and
    // the point is where both thirds meet.
    bool CheckQuadratic(const D2D1_BEZIER_SEGMENT& seg, D2D1_POINT_2F* control) const
    {
        const float dx10 = seg.point1.x - cur_exact_.x;
        const float dx23 = seg.point2.x - seg.point3.x;
        const float mid_x = cur_exact_.x + dx10 * 3 / 2;
        if (!ApproximatelyEqual(mid_x, dx23 * 3 / 2 + seg.point3.x)) {
            return false;
        }
        const float dy10 = seg.point1.y - cur_exact_.y;
        const float dy23 = seg.point2.y - seg.point3.y;
        const float mid_y = cur_exact_.y + dy10 * 3 / 2;
        if (!ApproximatelyEqual(mid_y, dy23 * 3 / 2 + seg.point3.y)) {
            return false;
        }
        control->x = mid_x;
        control->y = mid_y;
        return true;
    }

    // Windows decides what to keep from the exact coordinates and this walk
    // from the rounded ones, so a segment Windows keeps can collapse onto the
    // pen here and be dropped, leaving the two platforms different verbs. A
    // segment in that position is moved off the pen by one 64th, which the
    // exact coordinate written into the finished path takes back out.
    void Separate(FT_Vector* a, FT_Vector* b = nullptr, FT_Vector* c = nullptr) const
    {
        if (!Same(*a, cur_) || (b != nullptr && !Same(*b, cur_)) ||
            (c != nullptr && !Same(*c, cur_))) {
            return;
        }
        FT_Vector* last = c != nullptr ? c : (b != nullptr ? b : a);
        last->x += 1;
    }

    // The moveTo SkFTGeometrySink::goingTo emits at the first surviving
    // segment, carrying the point the pen was already at.
    void OpenContour()
    {
        if (started_) {
            return;
        }
        started_ = true;
        Record(cur_, cur_exact_);
    }

    // 1/64 is a power of two, so float(x) / 64 is exact and the recorded
    // match is bit for bit what SkFDot6ToScalar produces. The sink negates y
    // on its way in, which undoes the negation Point applied.
    //
    // xform_ is fSkXform, applied to the exact coordinate alone.
    // SkScalerContext_DW::generatePath builds the path from the untransformed
    // outline and calls builder.transform(fSkXform) on the finished path, so
    // the transform reaches the points and nothing else, and every decision
    // above it was already made on the same floats Windows decided on. The
    // multiply order is SkMatrix::Affine_vpts.
    void Record(const FT_Vector& quantized, const D2D1_POINT_2F& exact) const
    {
        if (!g_glyph_path_usable) {
            return;
        }
        if (g_glyph_path.size() >= kMaxGlyphPathPoints) {
            g_glyph_path_usable = false;
            return;
        }
        g_glyph_path.push_back(CleartypeGlyphPathPoint{
            .match_x = static_cast<float>(quantized.x) * 0.015625f,
            .match_y = -(static_cast<float>(quantized.y) * 0.015625f),
            .exact_x = exact.x * xform_.scale_x + exact.y * xform_.skew_x,
            .exact_y = exact.y * xform_.scale_y + exact.x * xform_.skew_y });
    }

    static constexpr size_t kMaxGlyphPathPoints = 1u << 16;

    const FT_Outline_Funcs* funcs_;
    void* user_;
    PathTransform xform_;
    int error_ = 0;
    bool started_ = false;
    FT_Vector cur_ = { .x = 0, .y = 0 };
    D2D1_POINT_2F cur_exact_ = { .x = 0.0f, .y = 0.0f };
};

// Emits `glyph_index`'s outline through `funcs` from DirectWrite instead of
// from the FreeType outline. False when this face cannot answer, in which case
// the caller falls through to the real walk; true with *cb_error carrying
// whatever a callback returned, which is what FT_Outline_Decompose returns.
static bool DecomposeThroughDWrite(FT_Face face, const FT_UInt glyph_index,
                                   const PendingOutline& pending, const FT_Outline_Funcs* funcs,
                                   void* user, int* cb_error)
{
    // A pixel aspect other than 1 is a horizontal squeeze and is kept. Skia's
    // FreeType port splits the device matrix with PreMatrixScale::kFull, so
    // the squeeze arrives as an anisotropic char size, while
    // SkScalerContext_DW splits it with kVertical and hands the ratio to
    // builder.transform. SkScalerContext_DW asks for the outline at the
    // vertical em and transforms the finished path, which is what the sink
    // reproduces.
    // A rotation reaches the sink the same way, as the rest of fSkXform.
    double em_size = 0.0;
    float x_over_y = 1.0f;
    if (!GetEmSize(face, &em_size, &x_over_y) || !(em_size > 0.0) || !(x_over_y > 0.0f)) {
        return false;
    }
    WarmSystemCollection();
    pthread_mutex_lock(&g_faces_mutex);
    FaceEntry* entry = FindFaceLocked(face);
    if (entry == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return false;
    }
    constexpr FT_Matrix identity = { .xx = 0x10000, .xy = 0, .yx = 0, .yy = 0x10000 };
    const bool face_transformed = !entry->transform_identity;
    const FT_Matrix face_matrix = face_transformed ? entry->transform_matrix : identity;
    // FT_Set_Transform's delta moves the outline and is not part of fSkXform.
    // Skia passes none, so a face carrying one is some other caller's.
    const bool has_delta = entry->transform_delta.x != 0 || entry->transform_delta.y != 0;
    if ((face_transformed && MatrixIsSingular(face_matrix)) || has_delta) {
        pthread_mutex_unlock(&g_faces_mutex);
        return false;
    }
    // FreeType applies the face matrix inside FT_Load_Glyph by calling
    // FT_Outline_Transform, so a transformed face leaves that same matrix on
    // the outline. Any other pending matrix is a reshape this walk has no
    // Windows counterpart for.
    if (pending.has_matrix &&
        (!face_transformed || pending.matrix.xx != face_matrix.xx ||
         pending.matrix.xy != face_matrix.xy || pending.matrix.yx != face_matrix.yx ||
         pending.matrix.yy != face_matrix.yy)) {
        pthread_mutex_unlock(&g_faces_mutex);
        return false;
    }
    const WinInstance* inst = GetWinInstanceLocked(entry, em_size);
    if (inst == nullptr || !inst->valid || inst->dwrite_face == nullptr) {
        pthread_mutex_unlock(&g_faces_mutex);
        return false;
    }
    IDWriteFontFace* dwrite_face = inst->dwrite_face;
    // The analysis is kept alive by this, not read through it.
    // ReSharper disable once CppLocalVariableWithNonTrivialDtorIsNeverUsed
    const HoldFace hold(dwrite_face);
    pthread_mutex_unlock(&g_faces_mutex);

    const auto gid = static_cast<UINT16>(glyph_index);
    const float aspect = SnappedPixelAspect(face);
    if (LogEnabled() && LogFaceWanted(face)) {
        // Names the glyphs drawn from the outline, as the mask route names
        // the ones it draws.
        LogLine("path glyph %u face %p em %.4f aspect %.8f (the size alone says %.8f)"
                " xform %.8f,%.8f,%.8f,%.8f",
                static_cast<unsigned>(glyph_index), reinterpret_cast<void*>(face), em_size,
                static_cast<double>(aspect), static_cast<double>(x_over_y),
                static_cast<double>(face_matrix.xx) / 65536.0,
                static_cast<double>(face_matrix.xy) / 65536.0,
                static_cast<double>(face_matrix.yx) / 65536.0,
                static_cast<double>(face_matrix.yy) / 65536.0);
    }
    DecomposeSink sink(funcs, user, SkXformFromFTMatrix(face_matrix, aspect));
    const HRESULT hr = dwrite_face->GetGlyphRunOutline(
        static_cast<FLOAT>(em_size), &gid, nullptr, nullptr, 1, FALSE, FALSE, &sink);
    if (FAILED(hr)) {
        return false;
    }
    *cb_error = sink.error();
    return true;
}

#endif  // CLEARTYPE_FIREFOX_PARITY

// The outline-to-path walk, answered from DirectWrite while the thunk over
// SkScalerContext_FreeType::generatePath has this thread armed. An outline
// carrying a pending shift, matrix or simulation is left to the real walk, as
// is any outline this library cannot name a glyph for.
extern "C" __attribute__((visibility("default")))
FT_Error FT_Outline_Decompose(FT_Outline* outline, const FT_Outline_Funcs* func_interface, void* user)
{
    ft_outline_decompose_fn real = real_FT_Outline_Decompose();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }
#if CLEARTYPE_FIREFOX_PARITY
    if (g_in_glyph_path && dwcft::ParityActive() && InterposerWanted() && outline != nullptr &&
        func_interface != nullptr && func_interface->shift == 0 && func_interface->delta == 0) {
        FT_Face face = nullptr;
        FT_UInt glyph_index = 0;
        if (FindOutlineOwner(outline, &face, &glyph_index) && face != nullptr &&
            face->glyph != nullptr && &face->glyph->outline == outline &&
            face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            const PendingOutline pending = PeekOutlineState(outline);
            if (pending.dx == 0 && pending.dy == 0 &&
                pending.simulations == DWRITE_FONT_SIMULATIONS_NONE && !pending.unrepresentable) {
                int cb_error = 0;
                if (DecomposeThroughDWrite(face, glyph_index, pending, func_interface, user,
                                           &cb_error)) {
                    return cb_error;
                }
            }
        }
        // Only one walk per armed generatePath can be answered for; a second
        // one, or one this declined, means the path is not the one recorded.
        g_glyph_path_usable = false;
    }
#endif
    return real(outline, func_interface, user);
}

// True when the glyph this thread is about to draw belongs to the one shape
// that must come from DirectWrite's mask rather than from its outline. Read by
// the generateMetrics thunk, which holds a Skia scaler and no FreeType face,
// so the face is the one this thread last loaded a glyph for.
extern "C" int CleartypeBlobPrefersMask(void)
{
#if CLEARTYPE_FIREFOX_PARITY
    return dwcft::ParityActive() && BlobPrefersDWriteMask(g_last_loaded_face) ? 1 : 0;
#else
    return 0;
#endif
}

extern "C" void CleartypeSkiaScaler(const double text_size, const double pre_scale_x,
                                    const double pre_skew_x, const double* post)
{
#if CLEARTYPE_FIREFOX_PARITY
    g_skia_text_size = text_size > 0.0 && text_size < 65536.0 ? text_size : 0.0;
    g_skia_rec = SkiaScalerRec{};
    if (post == nullptr || !(g_skia_text_size > 0.0)) {
        return;
    }
    g_skia_rec.text_size = text_size;
    g_skia_rec.pre_scale_x = pre_scale_x;
    g_skia_rec.pre_skew_x = pre_skew_x;
    for (size_t i = 0; i < 4; ++i) {
        g_skia_rec.post[i] = post[i];
    }
    g_skia_rec.valid = true;
#else
    (void)text_size;
    (void)pre_scale_x;
    (void)pre_skew_x;
    (void)post;
#endif
}

extern "C" int CleartypeOnBlobRaster(void)
{
#if CLEARTYPE_FIREFOX_PARITY
    return OnBlobRasterThread() ? 1 : 0;
#else
    return 0;
#endif
}

extern "C" void CleartypeBeginGlyphPath(void)
{
#if CLEARTYPE_FIREFOX_PARITY
    g_glyph_path.clear();
    g_glyph_path_usable = true;
    g_in_glyph_path = true;
#endif
}

extern "C" unsigned CleartypeEndGlyphPath(const CleartypeGlyphPathPoint** points)
{
#if CLEARTYPE_FIREFOX_PARITY
    g_in_glyph_path = false;
    if (!g_glyph_path_usable || g_glyph_path.empty()) {
        return 0;
    }
    *points = g_glyph_path.data();
    return static_cast<unsigned>(g_glyph_path.size());
#else
    (void)points;
    return 0;
#endif
}

FT_Error FT_Outline_Get_Bitmap(FT_Library library, FT_Outline* outline, const FT_Bitmap* abitmap)
{
    ft_outline_get_bitmap_fn real = real_FT_Outline_Get_Bitmap();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;
    }

    // ReSharper disable once CppLocalVariableWithNonTrivialDtorIsNeverUsed
    InRealFreeType inside;

#if CLEARTYPE_FIREFOX_PARITY
    // Only Skia calls this, so the library it names is Skia's. Recorded before
    // anything can decline the glyph, because the point of it is the next
    // glyph's control box, not this one.
    RecordSkiaLibrary(library);
#endif

    // An LCD target is the subpixel case for any caller, and DirectWrite
    // answers it. A gray target is how Skia's cairo-FT port
    // (SkScalerContextFTUtils::generateGlyphImage) fills an A8 glyph, and its
    // Windows counterpart is not DirectWrite at all but Skia's own scan
    // converter over the outline; SkiaScanlineBitmap says how that was
    // measured. Only the parity build takes the gray one.
    const bool lcd_target = abitmap != nullptr && abitmap->pixel_mode == FT_PIXEL_MODE_LCD;
#if CLEARTYPE_FIREFOX_PARITY
    const bool gray_target = dwcft::ParityActive() && abitmap != nullptr &&
                             abitmap->pixel_mode == FT_PIXEL_MODE_GRAY &&
                             abitmap->num_grays == 256;
#else
    const bool gray_target = false;
#endif
    if (!InterposerWanted() || outline == nullptr || abitmap == nullptr ||
        abitmap->buffer == nullptr || abitmap->rows == 0 || abitmap->width == 0 ||
        std::getenv("CLEARTYPE_FORCE_FALLBACK") != nullptr) {
        return real(library, outline, abitmap);
    }
#if CLEARTYPE_FIREFOX_PARITY
    if (gray_target && OnBlobRasterThread()) {
        FT_Face owner = nullptr;
        FT_UInt owned_index = 0;
        bool wants_mask = false;
        if (FindOutlineOwner(outline, &owner, &owned_index)) {
            wants_mask = BlobPrefersDWriteMask(owner);
        }
        if (!wants_mask) {
            return SkiaScanlineBitmap(real, library, outline, abitmap);
        }
    }
#endif
    if (!lcd_target && !gray_target) {
        return real(library, outline, abitmap);
    }

    FT_Face face = nullptr;
    FT_UInt glyph_index = 0;
    if (!FindOutlineOwner(outline, &face, &glyph_index)) {
        return real(library, outline, abitmap);
    }

    DWriteGlyphImage image;
    if (!RasterizeThroughDWrite(face, glyph_index, outline, gray_target, &image)) {
        return real(library, outline, abitmap);
    }

    // The target bitmap's bottom-left corner sits at outline coordinate (0,0),
    // so the pen origin is at column 0, row `rows`, and a glyph whose box
    // starts `left` pixels right of the pen and `top` pixels above the
    // baseline goes at (left, rows - top). Anything outside is clipped, as
    // real FreeType clips an outline the caller did not translate into range.
    const int target_channels = lcd_target ? 3 : 1;
    const int target_width = static_cast<int>(abitmap->width) / target_channels;
    const int target_rows = static_cast<int>(abitmap->rows);
    const int origin_col = image.left;
    const int origin_row = target_rows - image.top;

    {
        const int vis_w = std::min(origin_col + image.width, target_width) -
                          std::max(origin_col, 0);
        const int vis_h = std::min(origin_row + image.height, target_rows) -
                          std::max(origin_row, 0);
        if (LogEnabled() &&
            (vis_w <= 0 || vis_h <= 0 || vis_w != image.width || vis_h != image.height)) {
            // The thread is named because who is calling is the first thing
            // a clip raises, and the answer is not always the obvious one.
            LogLine("clipped glyph %u face %p: dwrite %dx%d at (%d,%d) into %dx%d "
                    "bitmap - %ld%% of it lands%s, on %s",
                    static_cast<unsigned>(glyph_index), static_cast<void*>(face), image.width, image.height,
                    origin_col, origin_row, target_width, target_rows,
                    vis_w <= 0 || vis_h <= 0
                        ? 0L
                        : 100L * vis_w * vis_h / (static_cast<long>(image.width) * image.height),
                    vis_w <= 0 || vis_h <= 0 ? " - GLYPH LOST" : "",
                    ThisThreadName() != nullptr ? ThisThreadName() : "an unnamed thread");
        }
    }

    for (int v = 0; v < image.height; ++v) {
        const int row = origin_row + v;
        if (row < 0 || row >= target_rows) {
            continue;
        }
        unsigned char* dst_row = abitmap->buffer + static_cast<ptrdiff_t>(row) * abitmap->pitch;
        const BYTE* src_row =
            image.texture.data() + static_cast<size_t>(v) * static_cast<size_t>(image.width) * static_cast<size_t>(image.channels);
        for (int u = 0; u < image.width; ++u) {
            const int col = origin_col + u;
            if (col < 0 || col >= target_width) {
                continue;
            }
            for (int c = 0; c < target_channels; ++c) {
                // Saturating add, not assignment: FreeType's own rasterizer
                // accumulates into this buffer and does not clear it, so a
                // caller is entitled to composite more than one outline into
                // the same bitmap. A coverage texture fills all three of an
                // LCD target; a gray target takes the G channel of a
                // ClearType texture (SkScalerContext_DW::RGBToA8).
                BYTE value;
                if (image.channels == 3) {
                    value = target_channels == 3 ? src_row[u * 3 + c] : src_row[u * 3 + 1];
                } else {
                    value = src_row[u];
                }
                const int sum = dst_row[col * target_channels + c] + value;
                dst_row[col * target_channels + c] = static_cast<unsigned char>(sum > 255 ? 255 : sum);
            }
        }
    }
    return FT_Err_Ok;
}

FT_Error FT_Render_Glyph(FT_GlyphSlot slot, const FT_Render_Mode render_mode)
{
    ft_render_glyph_fn real = real_FT_Render_Glyph();
    if (real == nullptr) {
        return FT_Err_Invalid_Library_Handle;  // nothing sensible left to do
    }

    // ReSharper disable once CppLocalVariableWithNonTrivialDtorIsNeverUsed
    InRealFreeType inside(slot != nullptr ? &slot->outline : nullptr);

    MaybeLogTableCensus();
#if CLEARTYPE_FIREFOX_PARITY
    WarnOnMissingParityFonts();
#endif

    // platform/unix/font.rs rasterize_glyph_outline maps FontRenderMode::Alpha
    // to FT_RENDER_MODE_NORMAL and FontRenderMode::Subpixel to
    // FT_RENDER_MODE_LCD; platform/windows/font.rs answers both from one
    // DWRITE_TEXTURE_CLEARTYPE_3x1 texture and takes the G channel for Alpha
    // (convert_to_bgra). LCD_V has no ClearType texture and is left to FreeType.
    const bool grayscale = render_mode == FT_RENDER_MODE_NORMAL;
#if CLEARTYPE_FIREFOX_PARITY
    const bool handled =
        render_mode == FT_RENDER_MODE_LCD || (grayscale && dwcft::ParityActive());
#else
    const bool handled = (render_mode == FT_RENDER_MODE_LCD);
#endif
    if (!InterposerWanted() || !handled || slot == nullptr ||
        slot->face == nullptr || slot->format != FT_GLYPH_FORMAT_OUTLINE) {
        const FT_Error passed = real(slot, render_mode);
        // FreeType translates the outline to the grid on its way to a bitmap,
        // and that translate comes back through this shim as a new pending
        // entry - created after the load that would have said whose slot it
        // is. Say so here, so the entry goes when the face does.
        if (slot != nullptr && slot->face != nullptr) {
            SetPendingOutlineFace(&slot->outline, slot->face);
        }
        return passed;
    }

    // Test hatch: forces the fallback path on valid input, so the "any
    // failure still renders correctly" contract can be exercised. Read per
    // call so a test harness can flip it.
    if (std::getenv("CLEARTYPE_FORCE_FALLBACK") != nullptr) {
        return real(slot, render_mode);
    }

    if (RenderThroughDWrite(slot, grayscale)) {
        return FT_Err_Ok;
    }
    const FT_Error passed = real(slot, render_mode);
    // Same reason as the branch above: FreeType's own translate on the way to
    // a bitmap arrives here as a fresh pending entry with no face on it.
    if (slot != nullptr && slot->face != nullptr) {
        SetPendingOutlineFace(&slot->outline, slot->face);
    }
    return passed;
}

}  // extern "C"
