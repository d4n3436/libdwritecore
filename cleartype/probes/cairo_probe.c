/*
 * cairo_probe.c - which cairo text entry point does this application use?
 *
 * Logging only. Renders nothing differently; it answers the question a
 * run-level interposer's design depends on, the same way probe.c answered it
 * for FreeType.
 *
 * The question matters because cairo has five public ways to put text on a
 * surface and they are not equivalent targets:
 *
 *   cairo_show_glyphs        positioned glyphs, no text - the natural target
 *   cairo_show_text_glyphs   the same plus the source text, for PDF tagging
 *   cairo_glyph_path         glyphs as a path, rasterized by the path code
 *   cairo_show_text          a whole string, shaped by cairo's toy API
 *   cairo_scaled_font_text_to_glyphs   shaping only, no drawing
 *
 * Interposing the wrong one is silent: the application keeps working and
 * nothing is intercepted.
 *
 * It also reports what a run-level interposer would have to cope with at each
 * call - the scaled font's backend (only CAIRO_FONT_TYPE_FT can be mapped to
 * an FT_Face and from there to an IDWriteFontFace), the target surface type
 * it would have to composite into, and the antialias mode cairo was asked
 * for, since a caller wanting grayscale must not be handed subpixel coverage.
 *
 *   cc -shared -fPIC cairo_probe.c -o libcleartype-cairo-probe.so \
 *      $(pkg-config --cflags --libs cairo) -ldl
 *   DWRITECORE_CAIRO_LOG=/tmp/cairo.log LD_PRELOAD=./libcleartype-cairo-probe.so app
 */

#define _GNU_SOURCE

/* dlsym() returns void*, and every symbol resolved here is a function. ISO C
   has no conversion between an object pointer and a function pointer, which is
   what -Wpedantic reports at each of these sites; POSIX requires the
   conversion to work and this whole file exists to perform it. gcc has no
   narrower switch for that one diagnostic, so -Wpedantic is off for the file. */
#pragma GCC diagnostic ignored "-Wpedantic"
#include <cairo.h>
#include <cairo-ft.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

/* A va_list is initialized by va_start and by nothing else, which the
   analysis does not model, so every varargs forwarder here reads as
   uninitialized. */
// ReSharper disable CppLocalVariableMightNotBeInitialized
static FILE *g_log;
/* Captured when the log is opened, not read back with fileno() inside the
 * handler: the destructor may be closing g_log at the moment the signal
 * lands, and this fd stays valid for the one write the handler performs. */
static int g_log_fd = -1;
static void probe_signal(int sig);
static unsigned long g_calls[5];
static unsigned long g_glyphs[5];
static const char *const kNames[5] = {
    "cairo_show_glyphs", "cairo_show_text_glyphs", "cairo_glyph_path",
    "cairo_show_text", "cairo_scaled_font_text_to_glyphs"
};

/* Opened in a constructor, not lazily: a sandboxed child (Firefox content,
 * Flatpak) may lose the ability to create files after startup, and a log that
 * fails to open then reads as "nothing was intercepted". */
__attribute__((constructor)) static void probe_open(void)
{
    const char *path = getenv("DWRITECORE_CAIRO_LOG");
    char named[4096];
    if (path == NULL) return;
    (void)snprintf(named, sizeof named, "%s.%d", path, (int)getpid());
    g_log = fopen(named, "we");
    if (g_log == NULL) return;
    (void)setvbuf(g_log, NULL, _IOLBF, 0);
    g_log_fd = fileno(g_log);
    (void)signal(SIGTERM, probe_signal);
    (void)signal(SIGINT, probe_signal);
}

__attribute__((format(printf, 1, 2)))
static void logline(const char *fmt, ...)
{
    va_list ap;
    if (g_log == NULL) return;
    va_start(ap, fmt);
    (void)vfprintf(g_log, fmt, ap);
    va_end(ap);
    (void)fputc('\n', g_log);
}

/* Reached from the destructor below, which the loader calls and analysis
   does not see. */
// ReSharper disable once CppDFAUnreachableFunctionCall
static void probe_summary(void)
{
    if (g_log == NULL) return;
    logline("--- summary (pid %d) ---", (int)getpid());
    for (int i = 0; i < 5; ++i)
        logline("  %-32s %8lu calls  %10lu glyphs", kNames[i], g_calls[i], g_glyphs[i]);
}

__attribute__((destructor)) static void probe_close(void)
{
    if (g_log == NULL) return;
    probe_summary();
    (void)fclose(g_log);
    g_log = NULL;
}

/* A GUI application under test is almost always stopped with a signal, and
 * the default action for SIGTERM terminates without running destructors - so
 * the summary never got written and every run read as "nothing was
 * intercepted", which is exactly the wrong answer to be handed by a probe.
 * Write it here, then let the default action proceed. */
/* Everything below runs in signal context, so it may call only
 * async-signal-safe functions: no stdio, which is what vfprintf/fputc/fflush
 * would have been. Formatting is done by hand into a stack buffer and handed
 * to a single write(2), which is both the output and the flush. */
static char *sig_append(char *at, const char *end, const char *text)
{
    while (*text != '\0' && at < end) *at++ = *text++;
    return at;
}

static char *sig_append_ulong(char *at, const char *end, unsigned long v)
{
    char digits[20];
    int n = 0;
    do { digits[n++] = (char)('0' + v % 10U); v /= 10U; } while (v != 0U && n < (int)sizeof digits);
    while (n > 0 && at < end) *at++ = digits[--n];
    return at;
}

static void probe_signal(const int sig)
{
    char buf[1024];
    char *at = buf;
    const char *const end = buf + sizeof buf;

    if (g_log_fd >= 0) {
        at = sig_append(at, end, "--- summary on signal ---\n");
        for (int i = 0; i < 5; ++i) {
            at = sig_append(at, end, "  ");
            at = sig_append(at, end, kNames[i]);
            at = sig_append(at, end, " ");
            at = sig_append_ulong(at, end, g_calls[i]);
            at = sig_append(at, end, " calls  ");
            at = sig_append_ulong(at, end, g_glyphs[i]);
            at = sig_append(at, end, " glyphs\n");
        }
        /* Short writes are not retried: the process is about to die from the
         * default action, and a partial summary beats a handler that loops. */
        if (write(g_log_fd, buf, (size_t)(at - buf)) < 0) { /* nothing to do */ }
    }
    (void)signal(sig, SIG_DFL);
    (void)raise(sig);
}

static const char *font_type(cairo_scaled_font_t *sf)
{
    if (sf == NULL) return "none";
    switch (cairo_scaled_font_get_type(sf)) {
    case CAIRO_FONT_TYPE_TOY:  return "toy";
    case CAIRO_FONT_TYPE_FT:   return "ft";
    case CAIRO_FONT_TYPE_WIN32:return "win32";
    case CAIRO_FONT_TYPE_USER: return "user";
    default:                   return "other";
    }
}

static const char *surface_type(cairo_t *cr)
{
    cairo_surface_t *s = cr != NULL ? cairo_get_target(cr) : NULL;
    if (s == NULL) return "none";
    switch (cairo_surface_get_type(s)) {
    case CAIRO_SURFACE_TYPE_IMAGE:    return "image";
    case CAIRO_SURFACE_TYPE_XLIB:     return "xlib";
    case CAIRO_SURFACE_TYPE_XCB:      return "xcb";
    case CAIRO_SURFACE_TYPE_RECORDING:return "recording";
    case CAIRO_SURFACE_TYPE_SUBSURFACE: return "subsurface";
    default:                          return "other";
    }
}

static const char *antialias(cairo_t *cr)
{
    const char *name = "?";
    if (cr == NULL) return name;
    cairo_font_options_t *o = cairo_font_options_create();
    cairo_get_font_options(cr, o);
    switch (cairo_font_options_get_antialias(o)) {
    case CAIRO_ANTIALIAS_DEFAULT:  name = "default";  break;
    case CAIRO_ANTIALIAS_NONE:     name = "none";     break;
    case CAIRO_ANTIALIAS_GRAY:     name = "gray";     break;
    case CAIRO_ANTIALIAS_SUBPIXEL: name = "subpixel"; break;
    case CAIRO_ANTIALIAS_FAST:     name = "fast";     break;
    case CAIRO_ANTIALIAS_GOOD:     name = "good";     break;
    case CAIRO_ANTIALIAS_BEST:     name = "best";     break;
    }
    cairo_font_options_destroy(o);
    return name;
}

/* The blend DirectWrite performs needs the text color, which FT_Render_Glyph
 * never sees. It is available here - but only when the source is a solid
 * color. A gradient or surface source has no single text color, and the
 * gamma-correct blend would have to be evaluated per pixel against it. */
static const char *source_desc(cairo_t *cr)
{
    /* Per thread: the caller logs the returned pointer, and cairo text entry
     * points are reached from several threads at once. */
    static _Thread_local char buf[128];
    cairo_pattern_t *p = cr != NULL ? cairo_get_source(cr) : NULL;
    double r, g, b, a;
    if (p == NULL) return "none";
    switch (cairo_pattern_get_type(p)) {
    case CAIRO_PATTERN_TYPE_SOLID:
        if (cairo_pattern_get_rgba(p, &r, &g, &b, &a) == CAIRO_STATUS_SUCCESS) {
            (void)snprintf(buf, sizeof buf, "solid rgba(%.3f %.3f %.3f %.3f)", r, g, b, a);
            return buf;
        }
        return "solid (unreadable)";
    case CAIRO_PATTERN_TYPE_SURFACE:        return "surface";
    case CAIRO_PATTERN_TYPE_LINEAR:         return "linear gradient";
    case CAIRO_PATTERN_TYPE_RADIAL:         return "radial gradient";
    default:                                return "other";
    }
}

/* One line the first time each entry point is seen, then counters only: a
 * per-call log on a scrolling window buries the answer in its own noise. */
static void first_sighting(const int slot, cairo_t *cr, const int nglyphs)
{
    if (g_calls[slot] != 1) return;
    cairo_scaled_font_t *sf = cr != NULL ? cairo_get_scaled_font(cr) : NULL;
    FT_Face face = NULL;
    if (sf != NULL && cairo_scaled_font_get_type(sf) == CAIRO_FONT_TYPE_FT)
        face = cairo_ft_scaled_font_lock_face(sf);
    logline("%s: first call, %d glyphs, font=%s surface=%s antialias=%s ft_face=%p",
            kNames[slot], nglyphs, font_type(sf), surface_type(cr), antialias(cr),
            (void *)face);
    logline("    source: %s", source_desc(cr));
    if (face != NULL) cairo_ft_scaled_font_unlock_face(sf);
}

#define REAL(name, type) \
    static type real; \
    if (real == NULL) real = (type)dlsym(RTLD_NEXT, name); \
    if (real == NULL) return

typedef void (*show_glyphs_fn)(cairo_t *, const cairo_glyph_t *, int);
void cairo_show_glyphs(cairo_t *cr, const cairo_glyph_t *glyphs, const int num_glyphs)
{
    REAL("cairo_show_glyphs", show_glyphs_fn);
    g_calls[0]++; g_glyphs[0] += num_glyphs > 0 ? (unsigned long)num_glyphs : 0;
    first_sighting(0, cr, num_glyphs);
    real(cr, glyphs, num_glyphs);
}

typedef void (*show_text_glyphs_fn)(cairo_t *, const char *, int,
                                    const cairo_glyph_t *, int,
                                    const cairo_text_cluster_t *, int,
                                    cairo_text_cluster_flags_t);
void cairo_show_text_glyphs(cairo_t *cr, const char *utf8, const int utf8_len,
                            const cairo_glyph_t *glyphs, const int num_glyphs,
                            const cairo_text_cluster_t *clusters, const int num_clusters,
                            const cairo_text_cluster_flags_t flags)
{
    REAL("cairo_show_text_glyphs", show_text_glyphs_fn);
    g_calls[1]++; g_glyphs[1] += num_glyphs > 0 ? (unsigned long)num_glyphs : 0;
    first_sighting(1, cr, num_glyphs);
    real(cr, utf8, utf8_len, glyphs, num_glyphs, clusters, num_clusters, flags);
}

typedef void (*glyph_path_fn)(cairo_t *, const cairo_glyph_t *, int);
void cairo_glyph_path(cairo_t *cr, const cairo_glyph_t *glyphs, const int num_glyphs)
{
    REAL("cairo_glyph_path", glyph_path_fn);
    g_calls[2]++; g_glyphs[2] += num_glyphs > 0 ? (unsigned long)num_glyphs : 0;
    first_sighting(2, cr, num_glyphs);
    real(cr, glyphs, num_glyphs);
}

typedef void (*show_text_fn)(cairo_t *, const char *);
void cairo_show_text(cairo_t *cr, const char *utf8)
{
    REAL("cairo_show_text", show_text_fn);
    g_calls[3]++; g_glyphs[3] += utf8 != NULL ? strlen(utf8) : 0;
    first_sighting(3, cr, utf8 != NULL ? (int)strlen(utf8) : 0);
    real(cr, utf8);
}

typedef cairo_status_t (*text_to_glyphs_fn)(cairo_scaled_font_t *, double, double,
                                            const char *, int, cairo_glyph_t **, int *,
                                            cairo_text_cluster_t **, int *,
                                            cairo_text_cluster_flags_t *);
cairo_status_t cairo_scaled_font_text_to_glyphs(
    cairo_scaled_font_t *sf, const double x, const double y, const char *utf8, const int utf8_len,
    cairo_glyph_t **glyphs, int *num_glyphs,
    cairo_text_cluster_t **clusters, int *num_clusters,
    cairo_text_cluster_flags_t *cluster_flags)
{
    static text_to_glyphs_fn real;
    if (real == NULL) real = (text_to_glyphs_fn)dlsym(RTLD_NEXT, "cairo_scaled_font_text_to_glyphs");
    if (real == NULL) return CAIRO_STATUS_NULL_POINTER;
    g_calls[4]++;
    if (g_calls[4] == 1)
        logline("cairo_scaled_font_text_to_glyphs: first call, font=%s", font_type(sf));
    return real(sf, x, y, utf8, utf8_len, glyphs, num_glyphs,
                clusters, num_clusters, cluster_flags);
}
