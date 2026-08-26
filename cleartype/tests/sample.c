/*
 * sample.c - render a line of text straight through FreeType to a PPM.
 *
 * The controlled way to see what the shim does, without depending on which
 * GUI applications happen to be installed. Run it twice, once plainly and
 * once under LD_PRELOAD, and diff the images:
 *
 *   ./cleartype_sample <font> out-freetype.ppm
 *   LD_PRELOAD=.../libcleartype.so ./cleartype_sample <font> out-dwrite.ppm
 *
 * It is a plain FreeType program: FT_Load_Char, FT_Render_Glyph with
 * FT_RENDER_MODE_LCD, composite, advance. Nothing in it knows the shim
 * exists, so where the two images differ, a real application's rendering path
 * was redirected.
 *
 * Black text on white, composited per subpixel the way any LCD-aware
 * toolkit does it: dst = 255 - coverage, one channel at a time.
 *
 * With --subpixel it also reproduces Cairo's quarter-pixel positioning
 * convention (quantize the fractional pen position to 1/4 px, bake it into
 * the outline with FT_Outline_Translate before rendering), which is what
 * exercises the shim's own subpixel-phase path.
 *
 * With --outline it rasterizes through FT_Outline_Get_Bitmap instead of
 * FT_Render_Glyph, the way a rasterizer that manages its own glyph atlas
 * does it (WebRender, and so Firefox): load the glyph, grid-fit its control
 * box, allocate a bitmap, translate the outline into it, and fill it. That
 * is a separate interception point in the shim with entirely separate
 * placement arithmetic, so it needs its own before/after comparison.
 */

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

/* No constexpr in this file: it is C, and nothing here pins a C standard,
   so the compiler's default decides whether the keyword exists. */
// ReSharper disable CppVariableCanBeMadeConstexpr

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARGIN 12

static const char *kDefaultText = "Hamburgefonstiv 0123456789 ClearType on Linux";
static const int kDefaultSizes[] = {9, 11, 13, 16, 20, 28};

struct Image
{
    int width, height;
    unsigned char *rgb;  /* row-major, 3 bytes per pixel */
};

static void blend_lcd(const struct Image *img, const FT_Bitmap *bitmap, const int pen_x, const int pen_y)
{
    /* An ALIASED_1x1 retry comes back as one gray byte per pixel, so the
     * subpixel triple is only there for FT_PIXEL_MODE_LCD. */
    const int lcd = bitmap->pixel_mode == FT_PIXEL_MODE_LCD;
    const int width = lcd ? (int)bitmap->width / 3 : (int)bitmap->width;
    for (int y = 0; y < (int)bitmap->rows; y++) {
        const int dst_y = pen_y + y;
        if (dst_y < 0 || dst_y >= img->height) {
            continue;
        }
        const unsigned char *row = bitmap->buffer + (size_t)y * (size_t)bitmap->pitch;
        for (int x = 0; x < width; x++) {
            const int dst_x = pen_x + x;
            if (dst_x < 0 || dst_x >= img->width) {
                continue;
            }
            unsigned char *dst = img->rgb + ((size_t)dst_y * (size_t)img->width + (size_t)dst_x) * 3;
            for (int c = 0; c < 3; c++) {
                /* Black text on white: dst = bg*(1-a) + fg*a, with fg = 0.
                 * Darken only, so overlapping filter tails from adjacent
                 * glyphs cannot lighten ink that is already there. */
                const int value = 255 - row[lcd ? x * 3 + c : x];
                if (value < dst[c]) {
                    dst[c] = (unsigned char)value;
                }
            }
        }
    }
}

int main(const int argc, char **argv)
{
    const char *font_path = NULL;
    const char *out_path = NULL;
    const char *text = kDefaultText;
    int subpixel = 0;
    int use_outline = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--subpixel") == 0) {
            subpixel = 1;
        } else if (strcmp(argv[i], "--outline") == 0) {
            use_outline = 1;
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            text = argv[++i];
        } else if (font_path == NULL) {
            font_path = argv[i];
        } else if (out_path == NULL) {
            out_path = argv[i];
        }
    }
    if (font_path == NULL || out_path == NULL) {
        (void)fprintf(stderr, "usage: %s <font> <out.ppm> [--text STR] [--subpixel] [--outline]\n",
                argv[0]);
        return 2;
    }

    FT_Library library;
    if (FT_Init_FreeType(&library) != 0) {
        (void)fprintf(stderr, "FT_Init_FreeType failed\n");
        return 1;
    }
    FT_Face face;
    if (FT_New_Face(library, font_path, 0, &face) != 0) {
        (void)fprintf(stderr, "cannot open %s\n", font_path);
        return 1;
    }

    /* The cast is the narrowing, spelled out. */
    // ReSharper disable once CppRedundantCastExpression
    const int size_count = (int)(sizeof kDefaultSizes / sizeof kDefaultSizes[0]);
    int line_height = 0;
    for (int i = 0; i < size_count; i++) {
        line_height += kDefaultSizes[i] * 2;
    }

    struct Image img;
    img.width = 1000;
    img.height = line_height + 2 * MARGIN;
    img.rgb = (unsigned char *)malloc((size_t)img.width * (size_t)img.height * 3);
    if (img.rgb == NULL) {
        (void)fprintf(stderr, "out of memory\n");
        return 1;
    }
    memset(img.rgb, 0xFF, (size_t)img.width * (size_t)img.height * 3);

    int baseline = MARGIN;
    for (int s = 0; s < size_count; s++) {
        const int px = kDefaultSizes[s];
        baseline += (int)(px * 1.4);
        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0) {
            continue;
        }

        /* 26.6 fixed point, so the fractional part of the pen survives
         * from one glyph to the next - the whole point of subpixel
         * positioning is that it accumulates. */
        FT_Pos pen_26_6 = MARGIN << 6;
        for (const char *p = text; *p; p++) {
            if (FT_Load_Char(face, (unsigned char)*p, FT_LOAD_TARGET_LCD) != 0) {
                continue;
            }

            if (subpixel) {
                /* Cairo's PHASE(): quantize the fractional pen position to
                 * quarter-pixel steps and translate the outline by it. */
                const FT_Pos shift = (pen_26_6 & 63) / 16 * 16;
                FT_Outline_Translate(&face->glyph->outline, shift, 0);
            }

            if (use_outline) {
                /* Grid-fit the control box the way FreeType does, pad by one
                 * pixel each side for the LCD filter's spread, allocate, move
                 * the outline into the box, and fill it. */
                FT_BBox cbox;
                FT_Outline_Get_CBox(&face->glyph->outline, &cbox);
                const int xmin = (int)(cbox.xMin >> 6) - 1;
                const int ymin = (int)(cbox.yMin >> 6);
                const int xmax = (int)((cbox.xMax + 63) >> 6) + 1;
                const int ymax = (int)((cbox.yMax + 63) >> 6);
                const int w = xmax - xmin, h = ymax - ymin;
                if (w > 0 && h > 0) {
                    FT_Bitmap bm = {0};
                    bm.rows = (unsigned)h;
                    bm.width = (unsigned)w * 3;
                    bm.pitch = w * 3;
                    bm.pixel_mode = FT_PIXEL_MODE_LCD;
                    bm.num_grays = 256;
                    bm.buffer = (unsigned char *)calloc((size_t)bm.pitch * (size_t)h, 1);
                    if (bm.buffer) {
                        FT_Outline_Translate(&face->glyph->outline,
                                             -((FT_Pos)xmin << 6), -((FT_Pos)ymin << 6));
                        if (FT_Outline_Get_Bitmap(library, &face->glyph->outline, &bm) == 0) {
                            blend_lcd(&img, &bm, (int)(pen_26_6 >> 6) + xmin, baseline - ymax);
                        }
                        free(bm.buffer);
                    }
                }
            } else if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD) == 0) {
                blend_lcd(&img, &face->glyph->bitmap, (int)(pen_26_6 >> 6) + face->glyph->bitmap_left,
                          baseline - face->glyph->bitmap_top);
            }

            /* Unhinted advance under --subpixel (fractional pen positions
             * are pointless if the advance is snapped anyway), hinted
             * otherwise. */
            pen_26_6 += subpixel ? face->glyph->linearHoriAdvance >> 10
                                 : face->glyph->advance.x & ~63;
        }
        baseline += (int)(px * 0.6);
    }

    FILE *out = fopen(out_path, "wb");
    if (out == NULL) {
        (void)fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    /* This file is the program's output, so a short write or a failed flush
     * is a real failure rather than something to shrug at: the image would be
     * silently truncated and the caller would compare it against a reference. */
    const size_t pixels = (size_t)img.width * (size_t)img.height * 3;
    int wrote_ok = fprintf(out, "P6\n%d %d\n255\n", img.width, img.height) > 0 &&
                   fwrite(img.rgb, 1, pixels, out) == pixels;
    if (fclose(out) != 0)
        wrote_ok = 0;
    if (!wrote_ok) {
        (void)fprintf(stderr, "failed to write %s\n", out_path);
        free(img.rgb);
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return 1;
    }
    (void)printf("wrote %s (%dx%d)\n", out_path, img.width, img.height);

    free(img.rgb);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return 0;
}
