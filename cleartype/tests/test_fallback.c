/*
 * test_fallback.c - contract test for libcleartype.so.
 *
 * A plain FreeType program with no dlopen/dlsym trickery at all, run under
 * real LD_PRELOAD exactly like any real application. Reaching the shim's
 * FT_Render_Glyph through dlopen()+dlsym() instead crashes on the harness's
 * own account: dlsym(RTLD_NEXT) resolves by load order, and a library brought
 * in by a late dlopen() sits *after* the executable's own dependencies, the
 * opposite of where LD_PRELOAD puts it.
 *
 * Run it twice, and both runs must exit 0:
 *
 *   LD_PRELOAD=.../libcleartype.so ./cleartype_test_fallback <font>
 *   CLEARTYPE_FORCE_FALLBACK=1 LD_PRELOAD=... ./cleartype_test_fallback <font>
 *
 * The forced run is the one that matters most. It exercises the path every
 * failure inside the shim takes - unknown face, unsupported font, a failed
 * HRESULT - on input that is otherwise perfectly valid, which is the only
 * way to test it without corrupting FreeType-facing data structures to
 * manufacture a failure.
 *
 * Also measures RSS across a long render loop. The shim installs a heap
 * buffer into the glyph slot on every glyph it handles, and FreeType's own
 * bitmap-ownership API does not free those; see the side-table comment in
 * freetype.cpp.
 */

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include FT_MULTIPLE_MASTERS_H

/* FreeType's handles are pointer typedefs, so const on one of these
   parameters means the handle and not the face; see the note in
   cleartype/src/freetype.cpp. */
// ReSharper disable CppParameterMayBeConst

/* No constexpr in this file: it is C, and nothing here pins a C standard,
   so the compiler's default decides whether the keyword exists. */
// ReSharper disable CppVariableCanBeMadeConstexpr

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                     \
    do {                                     \
        if (!(cond)) {                       \
            printf("FAIL  ");                \
            printf(__VA_ARGS__);             \
            printf("\n");                    \
            failures++;                      \
        }                                    \
    } while (0)

/* Resident set size in kB, or 0 if it cannot be read. */
static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    /* strtol, not fscanf("%ld"): a value too large for long is undefined
     * behavior for the scanf conversion and merely a range error for strtol. */
    char line[128];
    long resident = 0;
    if (fgets(line, sizeof line, f) != NULL) {
        char *at = line;
        (void)strtol(at, &at, 10);          /* total pages, not used here */
        char *after = at;
        const long value = strtol(at, &after, 10);
        if (after != at)
            resident = value;
    }
    (void)fclose(f);
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

/* Total coverage in a bitmap, as a stand-in for "how much ink is there" -
 * enough to tell a bold rendering from a regular one without caring which
 * engine produced either. */
static long bitmap_ink(const FT_Bitmap *bitmap)
{
    long total = 0;
    if (bitmap->buffer == NULL) {
        return 0;
    }
    for (unsigned y = 0; y < bitmap->rows; y++) {
        const unsigned char *row = bitmap->buffer + (size_t)y * (size_t)bitmap->pitch;
        for (unsigned x = 0; x < bitmap->width; x++) {
            total += row[x];
        }
    }
    return total;
}

/* Everything a real FT_Render_Glyph(FT_RENDER_MODE_LCD) call guarantees to
 * its caller. The shim has to be indistinguishable from it here, whichever
 * path it took, or it is not safe to preload into an arbitrary app. */
static void check_lcd_slot(FT_GlyphSlot slot, const unsigned long ch)
{
    CHECK(slot->format == FT_GLYPH_FORMAT_BITMAP, "U+%04lX: format is not a bitmap", ch);
    /* Gray is a legitimate answer to an LCD request. A bitmap strike and an
     * ALIASED_1x1 retry both produce one coverage byte per pixel, and
     * platform/unix/font.rs reads the slot's pixel_mode rather than assuming. */
    CHECK(slot->bitmap.pixel_mode == FT_PIXEL_MODE_LCD ||
              slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY,
          "U+%04lX: pixel_mode is %d, neither LCD nor GRAY", ch,
          (int)slot->bitmap.pixel_mode);
    if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_LCD) {
        CHECK(slot->bitmap.width % 3 == 0, "U+%04lX: LCD width %u is not a multiple of 3", ch,
              slot->bitmap.width);
    }
    if (slot->bitmap.rows > 0 && slot->bitmap.width > 0) {
        CHECK(slot->bitmap.buffer != NULL, "U+%04lX: %ux%u bitmap has no buffer", ch,
              slot->bitmap.width, slot->bitmap.rows);
        CHECK((unsigned)abs(slot->bitmap.pitch) >= slot->bitmap.width,
              "U+%04lX: pitch %d is narrower than width %u", ch, slot->bitmap.pitch,
              slot->bitmap.width);
    }
}

int main(int argc, char **argv)
{
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s <font-path>\n", argv[0]);
        return 2;
    }
    const int forced = getenv("CLEARTYPE_FORCE_FALLBACK") != NULL;
    printf("libcleartype contract test\n  font: %s\n  mode: %s\n\n", argv[1],
           forced ? "forced fallback (real FreeType)" : "normal (DWriteCore where possible)");

    FT_Library library;
    if (FT_Init_FreeType(&library) != 0) {
        (void)fprintf(stderr, "FT_Init_FreeType failed\n");
        return 1;
    }
    FT_Face face;
    if (FT_New_Face(library, argv[1], 0, &face) != 0) {
        (void)fprintf(stderr, "FT_New_Face(\"%s\") failed\n", argv[1]);
        return 1;
    }

    /* A spread of sizes, including one that is not an integer number of
     * pixels: point sizes converted at 96dpi almost never are, and a size
     * the shim mishandles by rounding would show up as wrong-looking text
     * and never as an error. */
    const int sizes[] = {8, 11, 13, 16, 24, 48};
    int rendered = 0, inked = 0;
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        const char *text = "Hamburgefonstiv 0123456789 .,;:!? @#$%&";
        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)sizes[s]) != 0) {
            printf("FAIL  FT_Set_Pixel_Sizes(%d) failed\n", sizes[s]);
            failures++;
            continue;
        }
        for (const char *p = text; *p; p++) {
            unsigned long ch = (unsigned char)*p;
            if (FT_Load_Char(face, ch, FT_LOAD_TARGET_LCD) != 0) {
                continue;
            }
            FT_Error err = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
            CHECK(err == 0, "U+%04lX at %dpx: FT_Render_Glyph returned %d", ch, sizes[s], (int)err);
            if (err != 0) {
                continue;
            }
            check_lcd_slot(face->glyph, ch);
            rendered++;

            if (face->glyph->bitmap.rows > 0 && face->glyph->bitmap.buffer != NULL) {
                for (unsigned y = 0; y < face->glyph->bitmap.rows && ch != ' '; y++) {
                    const unsigned char *row =
                        face->glyph->bitmap.buffer + (size_t)y * (size_t)face->glyph->bitmap.pitch;
                    for (unsigned x = 0; x < face->glyph->bitmap.width; x++) {
                        if (row[x] != 0) {
                            inked++;
                            y = face->glyph->bitmap.rows;
                            break;
                        }
                    }
                }
            }
        }
    }
    printf("ok    %d glyphs rendered, %d had ink\n", rendered, inked);
    CHECK(rendered > 100, "only %d glyphs rendered; the test text did not map", rendered);
    CHECK(inked > 100, "only %d glyphs had any ink; rasterization produced blanks", inked);

    /* Leak check. 50,000 renders on one slot: a per-glyph leak of a typical
     * 13px LCD bitmap would be tens of megabytes, far outside any plausible
     * allocator noise. */
    FT_Set_Pixel_Sizes(face, 0, 13);
    FT_Load_Char(face, 'g', FT_LOAD_TARGET_LCD);
    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
    const long rss_before = rss_kb();
    for (int i = 0; i < 50000; i++) {
        if (FT_Load_Char(face, (FT_ULong)'a' + (FT_ULong)(i % 26), FT_LOAD_TARGET_LCD) == 0) {
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
        }
    }
    const long rss_after = rss_kb();
    printf("ok    50000 renders, RSS %ld -> %ld kB (%+ld)\n", rss_before, rss_after,
           rss_after - rss_before);
    CHECK(rss_before == 0 || rss_after - rss_before < 8192,
          "RSS grew by %ld kB over 50000 renders - the installed bitmaps are leaking",
          rss_after - rss_before);

    /* Every other render mode must be untouched by the shim. */
    const FT_Render_Mode others[] = {FT_RENDER_MODE_NORMAL, FT_RENDER_MODE_MONO,
                                     FT_RENDER_MODE_LIGHT, FT_RENDER_MODE_LCD_V};
    const FT_Pixel_Mode expected[] = {FT_PIXEL_MODE_GRAY, FT_PIXEL_MODE_MONO, FT_PIXEL_MODE_GRAY,
                                      FT_PIXEL_MODE_LCD_V};
    for (size_t m = 0; m < sizeof others / sizeof others[0]; m++) {
        if (FT_Load_Char(face, 'M', FT_LOAD_DEFAULT) != 0) {
            continue;
        }
        FT_Error err = FT_Render_Glyph(face->glyph, others[m]);
        CHECK(err == 0, "render mode %d returned %d", (int)others[m], (int)err);
        if (err == 0) {
            CHECK(face->glyph->bitmap.pixel_mode == expected[m],
                  "render mode %d produced pixel_mode %d, expected %d", (int)others[m],
                  (int)face->glyph->bitmap.pixel_mode, (int)expected[m]);
        }
    }
    printf("ok    non-LCD render modes pass through unchanged\n");

    /* Every check below asserts something that holds whichever engine
     * rendered, so they are meaningful in both modes. Each covers a way the
     * shim can silently produce a *plausible but wrong* glyph instead of
     * failing, which is the failure class that does not announce itself. */

    /* A referenced face outlives its first FT_Done_Face. FreeType
     * reference-counts faces and Qt6 relies on it; a shim that forgets the
     * face on the first Done stops recognizing it and quietly reverts to
     * FreeType for everything drawn afterwards. Same glyph, same size, before
     * and after a reference/release pair - the output must not move. */
    FT_Set_Pixel_Sizes(face, 0, 13);
    unsigned before_w = 0, before_rows = 0;
    int before_left = 0, before_top = 0;
    if (FT_Load_Char(face, 'H', FT_LOAD_TARGET_LCD) == 0 &&
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD) == 0) {
        before_w = face->glyph->bitmap.width;
        before_rows = face->glyph->bitmap.rows;
        before_left = face->glyph->bitmap_left;
        before_top = face->glyph->bitmap_top;
    }
    CHECK(FT_Reference_Face(face) == 0, "FT_Reference_Face failed");
    CHECK(FT_Done_Face(face) == 0, "FT_Done_Face on a referenced face failed");
    if (FT_Load_Char(face, 'H', FT_LOAD_TARGET_LCD) == 0 &&
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD) == 0) {
        CHECK(face->glyph->bitmap.width == before_w &&
                  face->glyph->bitmap.rows == before_rows &&
                  face->glyph->bitmap_left == before_left &&
                  face->glyph->bitmap_top == before_top,
              "a referenced face renders differently after one FT_Done_Face: "
              "%ux%u@%d,%d became %ux%u@%d,%d",
              before_w, before_rows, before_left, before_top,
              face->glyph->bitmap.width, face->glyph->bitmap.rows,
              face->glyph->bitmap_left, face->glyph->bitmap_top);
    }
    printf("ok    face survives a reference/release pair\n");

    /* Synthetic bold and italic reshape the outline in place. A shim that
     * rasterizes from the font file cannot see that, so left unhandled it
     * returns byte-identical output for emboldened and regular text - bold
     * silently rendered at the regular weight. Ink must increase; a slant
     * must widen. */
    long plain_ink = 0, bold_ink = 0;
    unsigned plain_width = 0, oblique_width = 0;
    for (const char *p = "Hamburgefonstiv"; *p; p++) {
        if (FT_Load_Char(face, (unsigned char)*p, FT_LOAD_TARGET_LCD) != 0) continue;
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD) != 0) continue;
        plain_ink += bitmap_ink(&face->glyph->bitmap);
        plain_width += face->glyph->bitmap.width;
    }
    for (const char *p = "Hamburgefonstiv"; *p; p++) {
        if (FT_Load_Char(face, (unsigned char)*p, FT_LOAD_TARGET_LCD) != 0) continue;
        FT_GlyphSlot_Embolden(face->glyph);
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD) != 0) continue;
        bold_ink += bitmap_ink(&face->glyph->bitmap);
    }
    for (const char *p = "Hamburgefonstiv"; *p; p++) {
        if (FT_Load_Char(face, (unsigned char)*p, FT_LOAD_TARGET_LCD) != 0) continue;
        FT_GlyphSlot_Oblique(face->glyph);
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD) != 0) continue;
        oblique_width += face->glyph->bitmap.width;
    }
    CHECK(bold_ink > plain_ink, "emboldened text has no more ink than regular (%ld vs %ld)",
          bold_ink, plain_ink);
    CHECK(oblique_width > plain_width, "obliqued text is no wider than upright (%u vs %u)",
          oblique_width, plain_width);
    printf("ok    synthetic bold adds ink (%ld -> %ld), oblique adds width (%u -> %u)\n",
           plain_ink, bold_ink, plain_width, oblique_width);

    /* FT_Set_Transform carries a matrix and a translation, and only the
     * matrix is a shape change. Qt6 puts its subpixel glyph offset in the
     * translation, so a shim that treats the whole call as unsupported gives
     * up on most glyphs the moment fractional positioning is switched on.
     * A whole-pixel delta must move the glyph by exactly that much. */
    {
        FT_Vector delta = {.x = 3L * 64, .y = 2L * 64};
        FT_Load_Char(face, 'H', FT_LOAD_TARGET_LCD);
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
        int base_left = face->glyph->bitmap_left, base_top = face->glyph->bitmap_top;
        FT_Set_Transform(face, NULL, &delta);
        FT_Load_Char(face, 'H', FT_LOAD_TARGET_LCD);
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
        CHECK(face->glyph->bitmap_left == base_left + 3 &&
                  face->glyph->bitmap_top == base_top + 2,
              "a 3,2 px FT_Set_Transform delta moved the glyph to %+d,%+d instead",
              face->glyph->bitmap_left - base_left, face->glyph->bitmap_top - base_top);
        FT_Set_Transform(face, NULL, NULL);
        printf("ok    FT_Set_Transform delta moves the glyph by exactly that much\n");
    }

    /* Above roughly 96px DirectWrite stops rasterizing and returns geometry
     * instead. Asking DWriteCore for a ClearType texture in that mode does
     * not fail cleanly - it panics inside its own Rust code - so the shim
     * has to recognize the mode and fall through. These sizes must still
     * render, and still have ink. */
    for (int px = 90; px <= 130; px += 20) {
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
        if (FT_Load_Char(face, 'H', FT_LOAD_TARGET_LCD) != 0) continue;
        FT_Error err = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD);
        CHECK(err == 0, "%dpx: FT_Render_Glyph returned %d", px, (int)err);
        CHECK(bitmap_ink(&face->glyph->bitmap) > 0, "%dpx: glyph has no ink", px);
    }
    printf("ok    sizes past the ClearType/outline threshold still render\n");

    /* A variable font must render at the axis position the caller set, not
     * at the file's default. The shim rasterizes from the font file, so it
     * has to read the position back out of FreeType and hand it to
     * DirectWrite; getting that wrong produces text at the wrong weight,
     * which reads as a font substitution and not as a bug. Ink must rise
     * monotonically with weight. Skipped for a font without a wght axis. */
    {
        FT_MM_Var *mm = NULL;
        if (face->face_flags & FT_FACE_FLAG_MULTIPLE_MASTERS && FT_Get_MM_Var(face, &mm) == 0 &&
            mm != NULL) {
            int wght = -1;
            for (FT_UInt i = 0; i < mm->num_axis; i++) {
                if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
                    wght = (int)i;
                }
            }
            if (wght >= 0) {
                FT_Set_Pixel_Sizes(face, 0, 32);
                long previous = -1;
                int steps = 0, rising = 0;
                /* Stepped in 16.16, not in a double that accumulates +100
                 * per turn: the axis bounds and the coordinate handed to
                 * FreeType are both 16.16 already, so this walks exactly the
                 * same positions with no rounding to reason about. */
                const FT_Fixed w_step = 100L * 65536;
                const FT_Fixed w_end = mm->axis[wght].maximum + 65536;
                for (FT_Fixed w = mm->axis[wght].minimum; w <= w_end; w += w_step) {
                    FT_Fixed coords[32];
                    /* FreeType reads as many entries as the count says, so the
                       count and the buffer have to agree. fvar allows far more
                       axes than this holds, and the weight axis can sit past
                       the end of it. */
                    const FT_UInt n_axis = mm->num_axis < 32 ? mm->num_axis : 32;
                    for (FT_UInt i = 0; i < n_axis; i++) {
                        coords[i] = mm->axis[i].def;
                    }
                    if ((FT_UInt)wght < n_axis) {
                        coords[wght] = w;
                    }
                    FT_Set_Var_Design_Coordinates(face, n_axis, coords);
                    long total = 0;
                    for (const char *p = "Hamburgefonstiv"; *p; p++) {
                        if (FT_Load_Char(face, (unsigned char)*p, FT_LOAD_TARGET_LCD) != 0) continue;
                        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD) != 0) continue;
                        check_lcd_slot(face->glyph, (unsigned char)*p);
                        total += bitmap_ink(&face->glyph->bitmap);
                    }
                    if (previous >= 0) {
                        steps++;
                        rising += total > previous;
                    }
                    previous = total;
                }
                CHECK(steps > 0 && rising == steps,
                      "variable weight axis: ink rose on only %d of %d steps - the face is "
                      "probably rendering at its default position",
                      rising, steps);
                printf("ok    variable font tracks its weight axis (%d steps)\n", steps);
                /* Back to the default, or later checks inherit the weight. */
                FT_Fixed coords[32];
                const FT_UInt n_axis = mm->num_axis < 32 ? mm->num_axis : 32;
                for (FT_UInt i = 0; i < n_axis; i++) coords[i] = mm->axis[i].def;
                FT_Set_Var_Design_Coordinates(face, n_axis, coords);
            }
            FT_Done_MM_Var(library, mm);
        }
    }

    /* Memory faces take a different route inside the shim: there is no path
     * to hand DWrite, so the font has to go through an in-memory font file
     * loader instead. Nothing else here exercises that, and an untested
     * branch in a library that gets preloaded into arbitrary applications
     * is exactly the kind that is discovered by a crash. */
    {
        FILE *f = fopen(argv[1], "rb");
        CHECK(f != NULL, "cannot reopen %s for the memory-face case", argv[1]);
        if (f != NULL) {
            /* Checked: an unseekable stream would leave `size` wrong and
             * the read below would quietly build a truncated face. */
            long size = -1;
            if (fseek(f, 0, SEEK_END) == 0) {
                size = ftell(f);
                if (fseek(f, 0, SEEK_SET) != 0)
                    size = -1;
            }
            CHECK(size >= 0, "cannot size %s", argv[1]);
            unsigned char *blob = size >= 0 ? (unsigned char *)malloc((size_t)size) : NULL;
            if (blob != NULL && fread(blob, 1, (size_t)size, f) == (size_t)size) {
                FT_Face mem_face;
                FT_Error err = FT_New_Memory_Face(library, blob, size, 0, &mem_face);
                CHECK(err == 0, "FT_New_Memory_Face returned %d", (int)err);
                if (err == 0) {
                    FT_Set_Pixel_Sizes(mem_face, 0, 16);
                    int mem_rendered = 0;
                    for (const char *p = "Hamburgefonstiv"; *p; p++) {
                        if (FT_Load_Char(mem_face, (unsigned char)*p, FT_LOAD_TARGET_LCD) != 0) {
                            continue;
                        }
                        FT_Error rerr = FT_Render_Glyph(mem_face->glyph, FT_RENDER_MODE_LCD);
                        CHECK(rerr == 0, "memory face: FT_Render_Glyph returned %d", (int)rerr);
                        if (rerr == 0) {
                            check_lcd_slot(mem_face->glyph, (unsigned char)*p);
                            mem_rendered++;
                        }
                    }
                    printf("ok    memory face: %d glyphs rendered\n", mem_rendered);
                    CHECK(mem_rendered > 10, "memory face rendered only %d glyphs", mem_rendered);
                    FT_Done_Face(mem_face);
                }
            }
            free(blob); /* after FT_Done_Face: FreeType reads the blob for the face's lifetime */
            (void)fclose(f);
        }
    }

    FT_Done_Face(face);
    FT_Done_FreeType(library);

    printf("\n%s\n", failures == 0 ? "contract holds." : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
