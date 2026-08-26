/* glyph_digest.c - one line of text, every subpixel phase, digested.
 *
 *   glyph_digest <font.ttf> <size> [size...] [--text STR] [--mode lcd|gray]
 *
 * Loads each glyph of a string at each of the four quarter-pixel phases a
 * subpixel-positioning caller uses, renders it, and prints one line per
 * bitmap: size, character, phase, dimensions, bitmap_left/top, and an FNV-1a
 * hash of the pixels.
 *
 * Run it twice - once with the shim in LD_PRELOAD and once without, or across
 * two builds of the shim - and diff the output. Every line that changed names
 * the glyph, the size and the phase that changed, with no browser, window
 * system or second machine involved.
 *
 * The positioning mirrors what a glyph-atlas rasterizer does: the outline is
 * translated so its bounding box lands on the pixel grid at the requested
 * phase, rather than being rendered wherever the font happened to put it.
 *
 * Sizes are given in pixels and may be fractional - a page at 90% zoom asks
 * for sizes like 13.33 all day, and a shim that rounds them is worth catching.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

static const char* g_text = "Hamburgefonstiv 0123 .,;iIlLSW";

int main(int argc, char** argv)
{
    FT_Render_Mode mode = FT_RENDER_MODE_LCD;
    const char* sizes[64];
    int size_count = 0;
    const char* font = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            g_text = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            ++i;
            if (strcmp(argv[i], "gray") == 0) mode = FT_RENDER_MODE_NORMAL;
            else if (strcmp(argv[i], "lcd") == 0) mode = FT_RENDER_MODE_LCD;
            else { fprintf(stderr, "--mode takes lcd or gray\n"); return 2; }
        } else if (font == NULL) {
            font = argv[i];
        } else if (size_count < (int)(sizeof(sizes) / sizeof(sizes[0]))) {
            sizes[size_count++] = argv[i];
        }
    }
    if (font == NULL || size_count == 0) {
        fprintf(stderr, "usage: glyph_digest <font.ttf> <size> [size...] "
                        "[--text STR] [--mode lcd|gray]\n");
        return 2;
    }

    FT_Library library;
    FT_Face face;
    if (FT_Init_FreeType(&library)) return 1;
    if (FT_New_Face(library, font, 0, &face)) {
        fprintf(stderr, "cannot open %s\n", font);
        return 1;
    }

    for (int s = 0; s < size_count; ++s) {
        /* 26.6 fixed point, which is the finest a caller can ask for. */
        FT_F26Dot6 size = (FT_F26Dot6)(atof(sizes[s]) * 64.0 + 0.5);
        if (FT_Set_Char_Size(face, size, size, 0, 0)) continue;

        for (const char* p = g_text; *p; ++p) {
            FT_UInt index = FT_Get_Char_Index(face, (FT_ULong)(unsigned char)*p);
            for (int phase = 0; phase < 4; ++phase) {
                FT_BBox cbox;
                FT_Pos dx;
                FT_Bitmap* bitmap;
                unsigned long hash = 1469598103934665603UL;

                if (FT_Load_Glyph(face, index, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING)) continue;
                if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;

                FT_Outline_Get_CBox(&face->glyph->outline, &cbox);
                dx = phase * 16;          /* quarter pixels, in 26.6 */
                FT_Outline_Translate(&face->glyph->outline,
                                     dx - ((cbox.xMin + dx) & ~63),
                                     -(cbox.yMin & ~63));

                if (FT_Render_Glyph(face->glyph, mode)) continue;
                bitmap = &face->glyph->bitmap;
                for (unsigned r = 0; r < bitmap->rows; ++r) {
                    for (unsigned c = 0; c < bitmap->width; ++c) {
                        hash ^= bitmap->buffer[r * bitmap->pitch + c];
                        hash *= 1099511628211UL;
                    }
                }
                printf("%s %c %d %ux%u %d,%d %016lx\n", sizes[s], *p, phase,
                       bitmap->width, bitmap->rows,
                       face->glyph->bitmap_left, face->glyph->bitmap_top, hash);
            }
        }
    }

    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return 0;
}
