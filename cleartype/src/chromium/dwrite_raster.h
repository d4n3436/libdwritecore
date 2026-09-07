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

#include "path_abi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dwrite_raster {

// One design-space coordinate of a variable font, as
// SkFontArguments::VariationPosition::Coordinate lays it out.
struct VariationCoord
{
    uint32_t axis = 0;
    float value = 0;
};


// Map libdwritecore.so. Must be called before the sandbox closes, which
// means from the library's constructor, in the zygote.
bool Preload();

// Whether the system font collection has this family, and which of the code
// points it covers. `points` and `covers` are parallel arrays of `count`
// entries. False when the collection has no such family, leaving `covers`
// untouched. The answer comes from the same collection the raster path draws
// through, so a family the host has and this library cannot see counts as
// absent, which is what the raster path would do with it anyway.
bool FamilyCoverage(const char* family, const unsigned* points, unsigned count,
                    bool* covers);

// The directory holding the file this family's regular face comes from.
// Written to `out` with a terminator, no trailing separator. False when the
// collection has no such family or the face is not backed by a local file.
bool FamilyDirectory(const char* family, char* out, size_t size);

// Every family the system collection holds, ASCII-named ones only.
bool FamilyNames(std::vector<std::string>* out);

// The face GetFirstMatchingFont answers with, weight and slant both. `style`
// is the DWRITE_FONT_STYLE value the request carries, since an italic and an
// oblique request are answered differently. False when the family is not in
// the collection.
bool FamilyMatchFace(const char* family, int weight, int style,
                     int* out_weight, bool* out_italic);

// The weight DirectWrite answers a request for `weight` with, among this
// family's faces, which is what decides the face on Windows. Zero when the
// family is not installed.
int FamilyMatchWeight(const char* family, int weight);

// Whether a factory could be built. Called lazily, after the fork, in the
// process that will actually rasterize.
bool Available();

// `simulate_bold` and `simulate_oblique` ask DirectWrite for the face carrying
// its own bold or oblique simulation, which is what Skia's DirectWrite font
// manager leaves in place for a font with bitmap strikes. With
// `simulate_oblique` the caller must pass a rec with no pre-skew, since the
// slant belongs to the face and no longer to the matrix.

// The advance Windows would measure for this glyph, in pixels. False when it
// cannot be had, and the caller should keep whatever Skia computed.
bool GlyphAdvance(const void* typeface, const std::vector<uint8_t>& font_bytes,
                  uint16_t glyph_id, const skia_abi::Rec& rec,
                  const windows_path::Decision& decision, float* advance_x, float* advance_y,
                  uint32_t face_index = 0, bool simulate_bold = false,
                  bool simulate_oblique = false);

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
                 int* left, int* top, int* right, int* bottom, uint32_t face_index = 0,
                 bool simulate_bold = false, bool simulate_oblique = false);

// The font-wide metrics Windows would report, written into an SkFontMetrics.
// False when they cannot be had, and the caller keeps Skia's own.
bool FontMetrics(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const windows_path::Decision& decision, void* sk_font_metrics,
                 uint32_t face_index = 0, bool simulate_bold = false,
                 bool simulate_oblique = false);

// SkScalerContext_DW::generatePath, the outline DirectWrite hands Skia on
// Windows. `size` is fTextSizeRender, and the verbs and points come back in
// Skia's own order and convention, since SkDWriteGeometrySink is what
// translates them there and is mirrored here.
//
// False when the outline cannot be had, and the caller keeps skrifa's.
bool GlyphOutline(const void* typeface, const std::vector<uint8_t>& font_bytes,
                  uint16_t glyph_id, float size, std::vector<uint8_t>* verbs,
                  std::vector<path_abi::Point>* points, uint32_t face_index = 0,
                  bool simulate_bold = false, bool simulate_oblique = false);

// Fill `image_buffer` with this glyph's mask, laid out the way Skia expects
// for the glyph's own mask format and row bytes. False means the glyph was
// declined and Skia's own generateImage should run instead, which is the
// answer for a color glyph, an unsupported mask format, or any DirectWrite
// call that fails.
bool RenderGlyph(const void* typeface, const std::vector<uint8_t>& font_bytes,
                 const skia_abi::Rec& rec, const skia_abi::Glyph& glyph,
                 const skia_abi::PreBlend& preblend, const windows_path::Decision& decision,
                 void* image_buffer, uint32_t face_index = 0, bool simulate_bold = false,
                 bool simulate_oblique = false);

// Live entries in the face slot table, for the census.
size_t CensusFaces();

}  // namespace dwrite_raster

#endif  // CHROMIUM_DWRITE_RASTER_H_INCLUDED
