/* Leak stress for libcleartype's FreeType interposer.
 *
 * Every mode drives one shape of side-table churn to a point where whatever
 * the shim keeps has to be released again, then closes everything it opened.
 * With CLEARTYPE_LOG set the shim writes a census of its side tables when the
 * last FT_Library goes away; anything non-zero there is state that outlived
 * the objects it belonged to.
 *
 *   stress_ft <mode> <scale> [font...]
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H

static const char* g_fonts[8];
static int g_font_count;
static int g_scale = 1;

static const char* pick(int i) { return g_fonts[i % g_font_count]; }

static void render_all_modes(FT_Face f, unsigned long ch)
{
    static const FT_Render_Mode modes[] = {
        FT_RENDER_MODE_NORMAL, FT_RENDER_MODE_LIGHT, FT_RENDER_MODE_MONO,
        FT_RENDER_MODE_LCD, FT_RENDER_MODE_LCD_V,
    };
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        if (FT_Load_Char(f, ch, FT_LOAD_TARGET_LCD) == 0) {
            (void)FT_Render_Glyph(f->glyph, modes[m]);
        }
    }
}

/* 1. Many faces alive at once, each with a rasterized slot. This is the shape
 *    that exposes anything keyed on a slot address: one at a time, the
 *    allocator hands the next face the address the last one had. */
static int mode_live_faces(FT_Library lib)
{
    int const n = 80 * g_scale;
    FT_Face* live = calloc((size_t)n, sizeof *live);
    if (live == NULL) return 1;
    for (int i = 0; i < n; i++) {
        if (FT_New_Face(lib, pick(i), 0, &live[i]) != 0) { live[i] = NULL; continue; }
        FT_Set_Pixel_Sizes(live[i], 0, (FT_UInt)(12 + i % 40));
        if (FT_Load_Char(live[i], (FT_ULong)('a' + i % 26), FT_LOAD_TARGET_LCD) == 0) {
            FT_Render_Glyph(live[i]->glyph, FT_RENDER_MODE_LCD);
        }
    }
    for (int i = 0; i < n; i++) if (live[i]) FT_Done_Face(live[i]);
    free(live);
    return 0;
}

/* 2. The same, from memory rather than from a path: a different FaceEntry
 *    shape (entry->memory) and a different DWrite loader. */
static int mode_memory_faces(FT_Library lib)
{
    int const n = 20 * g_scale;
    FT_Face* live = calloc((size_t)n, sizeof *live);
    unsigned char** blobs = calloc((size_t)n, sizeof *blobs);
    if (live == NULL || blobs == NULL) return 1;
    for (int i = 0; i < n; i++) {
        FILE* fh = fopen(pick(i), "rb");
        if (fh == NULL) continue;
        fseek(fh, 0, SEEK_END);
        long const size = ftell(fh);
        fseek(fh, 0, SEEK_SET);
        if (size <= 0) { fclose(fh); continue; }
        blobs[i] = malloc((size_t)size);
        if (blobs[i] == NULL || fread(blobs[i], 1, (size_t)size, fh) != (size_t)size) {
            free(blobs[i]); blobs[i] = NULL; fclose(fh); continue;
        }
        fclose(fh);
        if (FT_New_Memory_Face(lib, blobs[i], size, 0, &live[i]) != 0) { live[i] = NULL; continue; }
        FT_Set_Pixel_Sizes(live[i], 0, (FT_UInt)(14 + i % 20));
        render_all_modes(live[i], (FT_ULong)('A' + i % 26));
    }
    for (int i = 0; i < n; i++) { if (live[i]) FT_Done_Face(live[i]); free(blobs[i]); }
    free(live); free(blobs);
    return 0;
}

/* 3. FT_Open_Face, both the pathname and the memory argument shapes. */
static int mode_open_face(FT_Library lib)
{
    int const n = 80 * g_scale;
    for (int i = 0; i < n; i++) {
        FT_Open_Args args;
        memset(&args, 0, sizeof args);
        args.flags = FT_OPEN_PATHNAME;
        args.pathname = (char*)pick(i);
        FT_Face f = NULL;
        if (FT_Open_Face(lib, &args, i % 3, &f) != 0) continue;
        FT_Set_Char_Size(f, 0, (FT_F26Dot6)(10 * 64 + i % 512), 96, 96);
        render_all_modes(f, 'M');
        FT_Done_Face(f);
    }
    return 0;
}

/* 4. One face, many distinct em sizes: the per-face instance cache evicts at
 *    32, and an eviction that dropped something without releasing it would
 *    only show once past that. */
static int mode_sizes(FT_Library lib)
{
    FT_Face f = NULL;
    if (FT_New_Face(lib, pick(0), 0, &f) != 0) return 1;
    int const n = 150 * g_scale;
    for (int i = 0; i < n; i++) {
        /* Distinct sizes, not large ones: what the cache is keyed on is the
           size, so 512 of them is already sixteen times its capacity, and a
           request that climbed with i instead would spend the whole run
           rasterizing glyphs that grow with the square of it. The cycle is
           longer than the run at scale 1, which therefore asks for exactly
           what it always did. */
        FT_Set_Char_Size(f, 0, (FT_F26Dot6)(6 * 64 + (i % 512) * 7), 96, 96);
        if (FT_Load_Char(f, 'g', FT_LOAD_TARGET_LCD) == 0) {
            FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
        }
        FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(8 + i % 90));
        if (FT_Load_Char(f, 'W', FT_LOAD_TARGET_LCD) == 0) {
            FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
        }
        FT_Size_RequestRec req;
        memset(&req, 0, sizeof req);
        req.type = FT_SIZE_REQUEST_TYPE_NOMINAL;
        req.width = 0;
        req.height = (FT_Long)((10 + i % 60) * 64);
        req.horiResolution = 96;
        req.vertResolution = 96;
        FT_Request_Size(f, &req);
        if (FT_Load_Char(f, 'x', FT_LOAD_TARGET_LCD) == 0) {
            FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
        }
    }
    FT_Done_Face(f);
    return 0;
}

/* 5. Transforms and outline edits: the pending-outline table is keyed on an
 *    outline address and evicts at 64. */
static int mode_transforms(FT_Library lib)
{
    int const n = 60 * g_scale;
    FT_Face* live = calloc((size_t)n, sizeof *live);
    if (live == NULL) return 1;
    for (int i = 0; i < n; i++) {
        if (FT_New_Face(lib, pick(i), 0, &live[i]) != 0) { live[i] = NULL; continue; }
        FT_Face f = live[i];
        FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(16 + i % 24));
        FT_Matrix m;
        m.xx = (FT_Fixed)(0x10000 + (i % 7) * 0x1000);
        m.xy = (FT_Fixed)((i % 5) * 0x800);
        m.yx = 0;
        m.yy = (FT_Fixed)(0x10000 - (i % 3) * 0x800);
        FT_Vector d;
        d.x = (FT_Pos)((i % 4) * 16);
        d.y = (FT_Pos)((i % 6) * 16);
        FT_Set_Transform(f, &m, &d);
        if (FT_Load_Char(f, (FT_ULong)('a' + i % 26), FT_LOAD_TARGET_LCD) == 0) {
            FT_Outline_Translate(&f->glyph->outline, (FT_Pos)(i % 64), 0);
            FT_Outline_Transform(&f->glyph->outline, &m);
            FT_Outline_Embolden(&f->glyph->outline, 32);
            FT_Outline_EmboldenXY(&f->glyph->outline, 16, 8);
            FT_GlyphSlot_Embolden(f->glyph);
            FT_GlyphSlot_Oblique(f->glyph);
            FT_BBox box;
            FT_Outline_Get_CBox(&f->glyph->outline, &box);
            FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
        }
        FT_Set_Transform(f, NULL, NULL);
    }
    for (int i = 0; i < n; i++) if (live[i]) FT_Done_Face(live[i]);
    free(live);
    return 0;
}

/* 6. Heap outlines that outlive their face, rasterized bare: the owner table
 *    is keyed on an outline address and evicts at 128. */
static int mode_heap_outlines(FT_Library lib)
{
    /* One face per iteration, which is what this costs; the outline handling
       it is here to exercise is the same at any count. */
    int const n = 120 * g_scale;
    for (int i = 0; i < n; i++) {
        FT_Face f = NULL;
        if (FT_New_Face(lib, pick(i), 0, &f) != 0) continue;
        FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(24 + i % 48));
        FT_Glyph g = NULL;
        if (FT_Load_Char(f, (FT_ULong)('a' + i % 26), FT_LOAD_NO_BITMAP) == 0 &&
            FT_Get_Glyph(f->glyph, &g) == 0) {
            FT_Done_Face(f);
            if (g->format == FT_GLYPH_FORMAT_OUTLINE) {
                FT_Outline* o = &((FT_OutlineGlyph)g)->outline;
                FT_BBox box;
                FT_Outline_Get_CBox(o, &box);
                long const w = ((box.xMax - box.xMin) >> 6) + 2;
                long const h = ((box.yMax - box.yMin) >> 6) + 2;
                if (w > 0 && h > 0 && w < 4096 && h < 4096) {
                    FT_Bitmap bm;
                    memset(&bm, 0, sizeof bm);
                    bm.rows = (unsigned)h;
                    bm.width = (unsigned)(w * 3);
                    bm.pitch = (int)(w * 3);
                    bm.pixel_mode = FT_PIXEL_MODE_LCD;
                    bm.num_grays = 256;
                    bm.buffer = calloc((size_t)(w * 3) * (size_t)h, 1);
                    if (bm.buffer) {
                        FT_Outline_Translate(o, -box.xMin, -box.yMin);
                        FT_Outline_Get_Bitmap(lib, o, &bm);
                        free(bm.buffer);
                    }
                }
            }
            FT_Done_Glyph(g);
        } else {
            FT_Done_Face(f);
        }
    }
    return 0;
}

/* 7. SFNT tables: the shim substitutes OS/2 and post per face and keeps the
 *    copies in a map keyed by face. */
static int mode_sfnt(FT_Library lib)
{
    int const n = 120 * g_scale;
    FT_Face* live = calloc((size_t)n, sizeof *live);
    if (live == NULL) return 1;
    for (int i = 0; i < n; i++) {
        if (FT_New_Face(lib, pick(i), 0, &live[i]) != 0) { live[i] = NULL; continue; }
        FT_Face f = live[i];
        FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(11 + i % 30));
        if (FT_Load_Char(f, 'H', FT_LOAD_TARGET_LCD) == 0) {
            FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
        }
        (void)FT_Get_Sfnt_Table(f, FT_SFNT_OS2);
        (void)FT_Get_Sfnt_Table(f, FT_SFNT_POST);
        (void)FT_Get_Sfnt_Table(f, FT_SFNT_HEAD);
        (void)FT_Get_Sfnt_Table(f, FT_SFNT_HHEA);
        (void)FT_Get_Sfnt_Table(f, FT_SFNT_MAXP);
        FT_ULong len = 0;
        (void)FT_Load_Sfnt_Table(f, FT_MAKE_TAG('n','a','m','e'), 0, NULL, &len);
    }
    for (int i = 0; i < n; i++) if (live[i]) FT_Done_Face(live[i]);
    free(live);
    return 0;
}

/* 8. Variable fonts: axis coordinates change the DWrite face the shim caches,
 *    and that cache evicts at 24 per face. */
static int mode_variable(FT_Library lib)
{
    /* Each axis position makes DirectWrite build a fresh instance, so this is
       the most expensive mode per iteration. Twelve positions still span the
       axis and the whole suite stays inside a few seconds under memcheck. */
    int const n = 10 * g_scale;
    for (int i = 0; i < n; i++) {
        FT_Face f = NULL;
        if (FT_New_Face(lib, pick(i), 0, &f) != 0) continue;
        FT_MM_Var* mm = NULL;
        if ((f->face_flags & FT_FACE_FLAG_MULTIPLE_MASTERS) != 0 &&
            FT_Get_MM_Var(f, &mm) == 0 && mm != NULL) {
            FT_Fixed coords[16];
            for (unsigned step = 0; step < 12; step++) {
                for (FT_UInt a = 0; a < mm->num_axis && a < 16; a++) {
                    FT_Fixed const lo = mm->axis[a].minimum;
                    FT_Fixed const hi = mm->axis[a].maximum;
                    coords[a] = lo + (FT_Fixed)(((hi - lo) / 41) * (step + 1));
                }
                FT_Set_Var_Design_Coordinates(f, mm->num_axis < 16 ? mm->num_axis : 16, coords);
                FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(16 + step % 8));
                if (FT_Load_Char(f, 'a' + (FT_ULong)(step % 26), FT_LOAD_TARGET_LCD) == 0) {
                    FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
                }
            }
            FT_Done_MM_Var(lib, mm);
        }
        FT_Done_Face(f);
    }
    return 0;
}

/* 9. Reference counting: the shim keeps its own count beside FreeType's. */
static int mode_refcount(FT_Library lib)
{
    int const n = 100 * g_scale;
    for (int i = 0; i < n; i++) {
        FT_Face f = NULL;
        if (FT_New_Face(lib, pick(i), 0, &f) != 0) continue;
        FT_Set_Pixel_Sizes(f, 0, 18);
        int const refs = 1 + i % 5;
        for (int r = 0; r < refs; r++) FT_Reference_Face(f);
        if (FT_Load_Char(f, 'R', FT_LOAD_TARGET_LCD) == 0) {
            FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
        }
        for (int r = 0; r < refs; r++) FT_Done_Face(f);
        FT_Done_Face(f);
    }
    return 0;
}

/* 10. Failure paths: nothing the shim allocates on the way to declining may
 *     survive the decline. */
static int mode_failure(FT_Library lib)
{
    int const n = 40 * g_scale;
    unsigned char junk[4096];
    memset(junk, 0xA5, sizeof junk);
    for (int i = 0; i < n; i++) {
        FT_Face bad = NULL;
        (void)FT_New_Memory_Face(lib, junk, (FT_Long)sizeof junk, 0, &bad);
        if (bad) FT_Done_Face(bad);
        bad = NULL;
        (void)FT_New_Face(lib, "/nonexistent/font.ttf", 0, &bad);
        if (bad) FT_Done_Face(bad);

        FT_Face f = NULL;
        if (FT_New_Face(lib, pick(i), 0, &f) != 0) continue;
        FT_Set_Pixel_Sizes(f, 0, 0);                 /* rejected */
        FT_Set_Char_Size(f, 0, 0, 0, 0);             /* rejected */
        FT_Set_Pixel_Sizes(f, 0, 12);
        (void)FT_Load_Char(f, 0x10FFFD, FT_LOAD_TARGET_LCD);   /* no such glyph */
        if (FT_Load_Char(f, ' ', FT_LOAD_TARGET_LCD) == 0) {   /* no ink */
            FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
        }
        /* Past every threshold the shim has, whose only size branch is at
           20px and whose bitmap strikes sit far below this. Not larger: the
           cost of this line is FreeType's own rasterization, which grows with
           the square of the size and otherwise dominates the whole run. */
        FT_Set_Pixel_Sizes(f, 0, 256);
        if (FT_Load_Char(f, 'O', FT_LOAD_TARGET_LCD) == 0) {
            FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LCD);
        }
        FT_Done_Face(f);
    }
    return 0;
}

/* 11. Several libraries at once, each with its own faces. */
static int mode_libraries(FT_Library unused)
{
    (void)unused;
    int const n = 12 * g_scale;
    for (int i = 0; i < n; i++) {
        FT_Library l = NULL;
        if (FT_Init_FreeType(&l) != 0) continue;
        FT_Face f[6];
        for (int j = 0; j < 6; j++) {
            if (FT_New_Face(l, pick(i + j), 0, &f[j]) != 0) { f[j] = NULL; continue; }
            FT_Set_Pixel_Sizes(f[j], 0, (FT_UInt)(13 + j * 5));
            render_all_modes(f[j], (FT_ULong)('a' + j));
        }
        /* Half the libraries are torn down without closing their faces, which
         * is what FT_Done_FreeType's sweep is for. */
        if (i % 2 == 0) {
            for (int j = 0; j < 6; j++) if (f[j]) FT_Done_Face(f[j]);
        }
        FT_Done_FreeType(l);
    }
    return 0;
}

/* 12. Concurrency: the side tables are shared and each has its own lock. */
struct worker_arg { FT_Library lib; int seed; };

static void* worker(void* p)
{
    struct worker_arg* a = p;
    int const n = 20 * g_scale;
    for (int i = 0; i < n; i++) {
        FT_Face f = NULL;
        if (FT_New_Face(a->lib, pick(a->seed + i), 0, &f) != 0) continue;
        FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(12 + (a->seed + i) % 30));
        FT_Matrix m = { 0x10000, (FT_Fixed)((i % 3) * 0x600), 0, 0x10000 };
        FT_Set_Transform(f, &m, NULL);
        render_all_modes(f, (FT_ULong)('a' + (a->seed + i) % 26));
        (void)FT_Get_Sfnt_Table(f, FT_SFNT_OS2);
        FT_Done_Face(f);
    }
    return NULL;
}

static int mode_threads(FT_Library lib)
{
    enum { kThreads = 8 };
    pthread_t t[kThreads];
    struct worker_arg a[kThreads];
    for (int i = 0; i < kThreads; i++) {
        a[i].lib = lib;
        a[i].seed = i * 7;
        if (pthread_create(&t[i], NULL, worker, &a[i]) != 0) return 1;
    }
    for (int i = 0; i < kThreads; i++) pthread_join(t[i], NULL);
    return 0;
}

struct mode { const char* name; int (*fn)(FT_Library); };
static const struct mode kModes[] = {
    {"live-faces",    mode_live_faces},
    {"memory-faces",  mode_memory_faces},
    {"open-face",     mode_open_face},
    {"sizes",         mode_sizes},
    {"transforms",    mode_transforms},
    {"heap-outlines", mode_heap_outlines},
    {"sfnt",          mode_sfnt},
    {"variable",      mode_variable},
    {"refcount",      mode_refcount},
    {"failure",       mode_failure},
    {"libraries",     mode_libraries},
    {"threads",       mode_threads},
};

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <mode|all> <scale> <font>...\n", argv[0]);
        for (size_t i = 0; i < sizeof kModes / sizeof kModes[0]; i++)
            fprintf(stderr, "  %s\n", kModes[i].name);
        return 2;
    }
    const char* want = argv[1];
    g_scale = atoi(argv[2]);
    if (g_scale < 1) g_scale = 1;
    for (int i = 3; i < argc && g_font_count < 8; i++) g_fonts[g_font_count++] = argv[i];

    FT_Library lib = NULL;
    if (FT_Init_FreeType(&lib) != 0) { fprintf(stderr, "no freetype\n"); return 2; }

    int ran = 0;
    for (size_t i = 0; i < sizeof kModes / sizeof kModes[0]; i++) {
        if (strcmp(want, "all") != 0 && strcmp(want, kModes[i].name) != 0) continue;
        (void)kModes[i].fn(lib);
        ran++;
        printf("ran %s\n", kModes[i].name);
        fflush(stdout);
    }
    FT_Done_FreeType(lib);
    if (ran == 0) { fprintf(stderr, "no such mode: %s\n", want); return 2; }
    return 0;
}
