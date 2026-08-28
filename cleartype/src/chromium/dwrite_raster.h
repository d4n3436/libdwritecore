//+--------------------------------------------------------------------------
//
//  dwrite_raster.h - rasterize one glyph through DWriteCore, the way
//  SkScalerContext_DW does it on Windows.
//
//  Translated from src/ports/SkScalerContext_win_dw.cpp:
//    SkScalerContext_DW::getDWMaskBits     the glyph run and the alpha texture
//    SkScalerContext_DW::generateDWImage   which conversion the mask gets
//    RGBToA8, RGBToLcd16, GrayscaleToA8, BilevelToBW
//
//  The parameters come from windows_path::Decide, so what DirectWrite is
//  asked for here is what it would be asked for on Windows for the same
//  glyph in the same font at the same size.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_DWRITE_RASTER_H_INCLUDED
#define CHROMIUM_DWRITE_RASTER_H_INCLUDED

#include "skia_abi.h"
#include "windows_path.h"

#include <cstdint>
#include <vector>

namespace dwrite_raster {

// Map libdwritecore.so. Must be called before the sandbox closes, which
// means from the library's constructor, in the zygote.
bool Preload();

// Whether a factory could be built. Called lazily, after the fork, in the
// process that will actually rasterize.
bool Available();

// Fill `image_buffer` with this glyph's mask, laid out the way Skia expects
// for the glyph's own mask format and row bytes. False means the glyph was
// declined and Skia's own generateImage should run instead, which is the
// answer for a color glyph, an unsupported mask format, or any DirectWrite
// call that fails.
// The advance Windows would measure for this glyph, in pixels. False when it
// cannot be had, and the caller should keep whatever Skia computed.
bool GlyphAdvance(const void* typeface, const std::vector<uint8_t>& font_bytes,
                  uint16_t glyph_id, const skia_abi::Rec& rec,
                  const windows_path::Decision& decision, float* advance_x, float* advance_y,
                  uint32_t face_index = 0);

// SkScalerContext_DW::generateDWMetrics, the box DirectWrite will fill.
// Fontations instead rounds out an unhinted outline, which can sit a pixel
// inside a grid-fitted glyph and clip the row DirectWrite would have drawn.
//
// `left`, `top`, `right` and `bottom` receive GetAlphaTextureBounds' RECT.
// False means it came back empty, which Skia reads as this texture type
// having nothing to draw.
bool GlyphBounds(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const skia_abi::Glyph& glyph, const skia_abi::Rec& rec,
                 const windows_path::Decision& decision,
                 windows_path::RenderingMode rendering_mode,
                 windows_path::TextureType texture_type,
                 int* left, int* top, int* right, int* bottom, uint32_t face_index = 0);

// The font-wide metrics Windows would report, written into an SkFontMetrics.
// False when they cannot be had, and the caller keeps Skia's own.
bool FontMetrics(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const windows_path::Decision& decision, void* sk_font_metrics,
                 uint32_t face_index = 0);

bool RenderGlyph(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const skia_abi::Rec& rec, const skia_abi::Glyph& glyph,
                 const skia_abi::PreBlend& preblend, const windows_path::Decision& decision,
                 void* image_buffer, uint32_t face_index = 0);

}  // namespace dwrite_raster

#endif  // CHROMIUM_DWRITE_RASTER_H_INCLUDED
