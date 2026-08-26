/*
 * probe.c - logging-only FreeType interposer.
 *
 * Renders nothing differently: every call is forwarded to the real
 * FreeType. Its whole job is to answer the questions the real shim's
 * design depends on, per process, on the machine you actually care about:
 *
 *   1. Does this app reach FT_Render_Glyph with FT_RENDER_MODE_LCD at all?
 *      If it does its own oversample-and-filter internally, or if
 *      fontconfig has subpixel off, there is nothing to intercept.
 *   2. Which face constructor does it use? libcleartype needs a
 *      font *file* to hand DWrite. FT_New_Face gives it a path;
 *      FT_New_Memory_Face gives it only a blob, which needs a different
 *      (in-memory font file loader) path in the shim.
 *   3. Is a non-identity FT_Set_Transform in play? FreeType exposes no
 *      getter for it, so the shim must record it here or fall through
 *      blindly - and a matrix it ignored would render slanted text upright.
 *
 * Logs to CLEARTYPE_LOG (a path) when set, stderr otherwise. Use the
 * file: GUI apps routinely reopen their inherited stdio on /dev/null when
 * they detach from the launching terminal (confirmed for KDE's Kate), so
 * a shell redirection on the launch command captures nothing even though
 * the app is running and rendering.
 */

#define _GNU_SOURCE

/* dlsym() returns void*, and every symbol resolved here is a function. ISO C
   has no conversion between an object pointer and a function pointer, which is
   what -Wpedantic reports at each of these sites; POSIX requires the
   conversion to work and this whole file exists to perform it. gcc has no
   narrower switch for that one diagnostic, so -Wpedantic is off for the file. */
#pragma GCC diagnostic ignored "-Wpedantic"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

/* FreeType's handles and the ft_*_fn function pointers are typedefs for
   pointers, so const on one of these means the handle and not the face; see
   the note in cleartype/src/freetype.cpp. */
// ReSharper disable CppParameterMayBeConst
// ReSharper disable CppLocalVariableMayBeConst

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef FT_Error (*ft_render_glyph_fn)(FT_GlyphSlot, FT_Render_Mode);
typedef FT_Error (*ft_new_face_fn)(FT_Library, const char*, FT_Long, FT_Face*);
typedef FT_Error (*ft_new_memory_face_fn)(FT_Library, const FT_Byte*, FT_Long, FT_Long, FT_Face*);
typedef FT_Error (*ft_open_face_fn)(FT_Library, const FT_Open_Args*, FT_Long, FT_Face*);
typedef void (*ft_set_transform_fn)(FT_Face, FT_Matrix*, FT_Vector*);
typedef FT_Error (*ft_load_glyph_fn)(FT_Face, FT_UInt, FT_Int32);
typedef FT_Error (*ft_outline_get_bitmap_fn)(FT_Library, FT_Outline*, const FT_Bitmap*);
typedef FT_Error (*ft_outline_render_fn)(FT_Library, FT_Outline*, FT_Raster_Params*);
typedef void (*ft_outline_translate_fn)(const FT_Outline*, FT_Pos, FT_Pos);

#define REAL(type, name)                                     \
    static type real_##name(void)                            \
    {                                                        \
        static type fn;                                      \
        static int resolved;                                 \
        if (!resolved) {                                     \
            fn = (type)dlsym(RTLD_NEXT, #name);              \
            resolved = 1;                                    \
        }                                                    \
        return fn;                                           \
    }

REAL(ft_render_glyph_fn, FT_Render_Glyph)
REAL(ft_new_face_fn, FT_New_Face)
REAL(ft_new_memory_face_fn, FT_New_Memory_Face)
REAL(ft_open_face_fn, FT_Open_Face)
REAL(ft_set_transform_fn, FT_Set_Transform)
REAL(ft_load_glyph_fn, FT_Load_Glyph)
REAL(ft_outline_get_bitmap_fn, FT_Outline_Get_Bitmap)
REAL(ft_outline_render_fn, FT_Outline_Render)
REAL(ft_outline_translate_fn, FT_Outline_Translate)

/* Opened in a constructor, not lazily on first use.
 *
 * A sandboxed process may not be able to open it later. Firefox engages its
 * content-process seccomp filter and file broker after dynamic linking but
 * long before the first glyph is drawn, so an fopen() at first-log time
 * fails and the probe reports nothing at all - which looks exactly like
 * "this process never rendered any text", and is the wrong conclusion. A
 * descriptor opened during library initialization survives the transition.
 *
 * The pid is appended to the path because the processes that matter here are
 * several and they write concurrently. */
static FILE* g_log;

__attribute__((constructor)) static void open_log(void)
{
    const char* path = getenv("CLEARTYPE_LOG");
    if (path == NULL) {
        return;
    }
    char named[4096];
    (void)snprintf(named, sizeof named, "%s.%d", path, (int)getpid());
    g_log = fopen(named, "a");
}

static FILE* log_target(void)
{
    return g_log ? g_log : stderr;
}

/* Every write is cast to void: this is a diagnostic log, and there is nowhere
 * useful to report a failed write to. */
#define LOG(...)                                                         \
    do {                                                                 \
        FILE* out = log_target();                                        \
        (void)fprintf(out, "[ft-probe %d] ", (int)getpid());             \
        (void)fprintf(out, __VA_ARGS__);                                 \
        (void)fputc('\n', out);                                          \
        (void)fflush(out); /* the app may be killed abruptly mid-test */ \
    } while (0)

/* Counters instead of a line per call: one page of text is tens of
 * thousands of calls, and the question is which entry points an application
 * uses at all, not how often. Reported once at exit. */
static unsigned long g_count_render, g_count_get_bitmap, g_count_outline_render;
static unsigned long g_count_load_glyph, g_count_translate;
static unsigned long g_pixel_modes[8];

__attribute__((destructor)) static void report_counts(void)
{
    if (g_count_render == 0 && g_count_get_bitmap == 0 && g_count_outline_render == 0 &&
        g_count_load_glyph == 0) {
        return;
    }
    FILE* out = log_target();
    (void)fprintf(out, "[ft-probe %d] TOTALS FT_Load_Glyph=%lu FT_Render_Glyph=%lu "
                 "FT_Outline_Get_Bitmap=%lu FT_Outline_Render=%lu FT_Outline_Translate=%lu\n",
            (int)getpid(), g_count_load_glyph, g_count_render, g_count_get_bitmap,
            g_count_outline_render, g_count_translate);
    (void)fprintf(out, "[ft-probe %d] target pixel modes:", (int)getpid());
    for (int i = 0; i < 8; i++) {
        if (g_pixel_modes[i]) {
            (void)fprintf(out, " mode%d=%lu", i, g_pixel_modes[i]);
        }
    }
    (void)fputc('\n', out);
    (void)fflush(out);
}

static const char* mode_name(const FT_Render_Mode m)
{
    switch (m) {
    case FT_RENDER_MODE_NORMAL: return "NORMAL";
    case FT_RENDER_MODE_LIGHT:  return "LIGHT";
    case FT_RENDER_MODE_MONO:   return "MONO";
    case FT_RENDER_MODE_LCD:    return "LCD";
    case FT_RENDER_MODE_LCD_V:  return "LCD_V";
    default:                    return "?";
    }
}

FT_Error FT_New_Face(FT_Library library, const char* path, const FT_Long index, FT_Face* aface)
{
    ft_new_face_fn fn = real_FT_New_Face();
    if (!fn) {
        return FT_Err_Invalid_Library_Handle;
    }
    const FT_Error err = fn(library, path, index, aface);
    LOG("FT_New_Face(\"%s\", index=%ld) -> err=%d face=%p",
        path ? path : "(null)", index, (int)err,
        (void*)(aface ? *aface : NULL));
    return err;
}

FT_Error FT_New_Memory_Face(FT_Library library, const FT_Byte* base, const FT_Long size,
                            const FT_Long index, FT_Face* aface)
{
    ft_new_memory_face_fn fn = real_FT_New_Memory_Face();
    if (!fn) {
        return FT_Err_Invalid_Library_Handle;
    }
    const FT_Error err = fn(library, base, size, index, aface);
    LOG("FT_New_Memory_Face(%ld bytes, index=%ld) -> err=%d face=%p",
        size, index, (int)err, (void*)(aface ? *aface : NULL));
    return err;
}

FT_Error FT_Open_Face(FT_Library library, const FT_Open_Args* args, const FT_Long index, FT_Face* aface)
{
    ft_open_face_fn fn = real_FT_Open_Face();
    if (!fn) {
        return FT_Err_Invalid_Library_Handle;
    }
    const FT_Error err = fn(library, args, index, aface);
    if (args) {
        LOG("FT_Open_Face(flags=0x%x%s%s%s, path=\"%s\", size=%ld, index=%ld) -> err=%d face=%p",
            (unsigned)args->flags,
            (args->flags & FT_OPEN_MEMORY) ? " MEMORY" : "",
            (args->flags & FT_OPEN_PATHNAME) ? " PATHNAME" : "",
            (args->flags & FT_OPEN_STREAM) ? " STREAM" : "",
            args->pathname ? args->pathname : "(null)",
            args->memory_size, index, (int)err,
            (void*)(aface ? *aface : NULL));
    } else {
        LOG("FT_Open_Face(args=NULL) -> err=%d", (int)err);
    }
    return err;
}

void FT_Set_Transform(FT_Face face, FT_Matrix* matrix, FT_Vector* delta)
{
    ft_set_transform_fn fn = real_FT_Set_Transform();
    if (!fn) {
        return;
    }
    fn(face, matrix, delta);

    /* Only non-identity transforms are worth a line - Cairo and Qt both
     * call this on every size change, overwhelmingly with identity, and
     * logging those would bury everything else. */
    const int identity = !matrix || (matrix->xx == 0x10000 && matrix->xy == 0 &&
                               matrix->yx == 0 && matrix->yy == 0x10000);
    const int no_delta = !delta || (delta->x == 0 && delta->y == 0);
    if (!identity || !no_delta) {
        LOG("FT_Set_Transform(face=%p, [%ld %ld; %ld %ld], delta=[%ld %ld])",
            (void*)face,
            matrix ? matrix->xx : 0x10000L, matrix ? matrix->xy : 0L,
            matrix ? matrix->yx : 0L, matrix ? matrix->yy : 0x10000L,
            delta ? delta->x : 0L, delta ? delta->y : 0L);
    }
}

FT_Error FT_Load_Glyph(FT_Face face, const FT_UInt glyph_index, const FT_Int32 load_flags)
{
    ft_load_glyph_fn fn = real_FT_Load_Glyph();
    if (!fn) {
        return FT_Err_Invalid_Library_Handle;
    }
    g_count_load_glyph++;
    return fn(face, glyph_index, load_flags);
}

/* The interesting one for WebRender-based applications: it renders an
 * outline into a bitmap the *caller* allocated and positioned, so it never
 * goes near FT_Render_Glyph. The caller's pixel_mode is what says whether
 * subpixel output was wanted. */
FT_Error FT_Outline_Get_Bitmap(FT_Library library, FT_Outline* outline,
                               const FT_Bitmap* abitmap)
{
    ft_outline_get_bitmap_fn fn = real_FT_Outline_Get_Bitmap();
    if (!fn) {
        return FT_Err_Invalid_Library_Handle;
    }
    g_count_get_bitmap++;
    /* Reported the first time each distinct pixel_mode appears instead of
     * at exit: a browser's content processes are often killed outright, and
     * a destructor that never runs reports nothing. */
    if (abitmap && abitmap->pixel_mode < 8) {
        if (g_pixel_modes[abitmap->pixel_mode]++ == 0) {
            LOG("FT_Outline_Get_Bitmap pixel_mode=%u (%s) first seen: %ux%u pitch=%d points=%d",
                (unsigned)abitmap->pixel_mode,
                abitmap->pixel_mode == FT_PIXEL_MODE_LCD ? "LCD"
                    : abitmap->pixel_mode == FT_PIXEL_MODE_GRAY ? "GRAY"
                    : abitmap->pixel_mode == FT_PIXEL_MODE_MONO ? "MONO"
                    : abitmap->pixel_mode == FT_PIXEL_MODE_LCD_V ? "LCD_V" : "?",
                abitmap->width, abitmap->rows, abitmap->pitch,
                outline ? outline->n_points : -1);
        }
    }
    return fn(library, outline, abitmap);
}

FT_Error FT_Outline_Render(FT_Library library, FT_Outline* outline, FT_Raster_Params* params)
{
    ft_outline_render_fn fn = real_FT_Outline_Render();
    if (!fn) {
        return FT_Err_Invalid_Library_Handle;
    }
    g_count_outline_render++;
    return fn(library, outline, params);
}

void FT_Outline_Translate(const FT_Outline* outline, const FT_Pos x, const FT_Pos y)
{
    ft_outline_translate_fn fn = real_FT_Outline_Translate();
    if (!fn) {
        return;
    }
    g_count_translate++;
    /* The values matter, not just the count: a caller that translates only
     * horizontally is applying a subpixel phase, but one that also translates
     * vertically is repositioning the glyph into a box of its own choosing,
     * which a shim that rasterizes from the font file has to reproduce. */
    if (g_count_translate <= 12) {
        /* Signed: C division truncates toward zero and % keeps the dividend's
         * sign, so the whole and fractional parts are printed from the
         * magnitude with one sign in front. */
        LOG("FT_Outline_Translate(dx=%ld, dy=%ld)  [%c%ld.%02ld px, %c%ld.%02ld px]",
            x, y,
            x < 0 ? '-' : '+', labs(x) / 64, labs(x) % 64 * 100 / 64,
            y < 0 ? '-' : '+', labs(y) / 64, labs(y) % 64 * 100 / 64);
    }
    fn(outline, x, y);
}

FT_Error FT_Render_Glyph(FT_GlyphSlot slot, const FT_Render_Mode render_mode)
{
    ft_render_glyph_fn fn = real_FT_Render_Glyph();
    if (!fn) {
        return FT_Err_Invalid_Library_Handle;
    }
    g_count_render++;

    /* One line per (face, mode, ppem) combination instead of per glyph:
     * a single Kate window redraw is thousands of calls, and the question
     * this probe answers is "which combinations occur", not "how often". */
    static struct {
        FT_Face face;
        FT_Render_Mode mode;
        FT_UShort ppem;
    } seen[64];
    static int seen_count;

    if (slot && slot->face) {
        const FT_UShort ppem = slot->face->size ? slot->face->size->metrics.x_ppem : 0;
        int known = 0;
        for (int i = 0; i < seen_count; i++) {
            if (seen[i].face == slot->face && seen[i].mode == render_mode && seen[i].ppem == ppem) {
                known = 1;
                break;
            }
        }
        if (!known) {
            if (seen_count < (int)(sizeof seen / sizeof seen[0])) {
                seen[seen_count].face = slot->face;
                seen[seen_count].mode = render_mode;
                seen[seen_count].ppem = ppem;
                seen_count++;
            }
            LOG("FT_Render_Glyph(mode=%s) face=%p \"%s\" ppem=%u format=%c%c%c%c",
                mode_name(render_mode), (void*)slot->face,
                slot->face->family_name ? slot->face->family_name : "?",
                (unsigned)ppem,
                (char)(slot->format >> 24), (char)(slot->format >> 16),
                (char)(slot->format >> 8), (char)slot->format);
        }
    }

    return fn(slot, render_mode);
}
