//+--------------------------------------------------------------------------
//
//  colr_outline.h - the outlines a color glyph's layers are drawn from.
//
//  Skia rasterizes a COLRv1 glyph itself on both platforms, so the layers,
//  colors and gradients already agree. The layer outlines do not:
//  SkScalerContext_DW takes them from DWriteCore at the render size and
//  divides that back out, SkTypeface_fontations takes them from skrifa at
//  units per em and scales down, and for some glyphs the two differ by a
//  fraction of a pixel on the outermost fringe. This supplies DWriteCore's.
//
//  A color glyph sets neverRequestPath and its layers are read through
//  generatePathForGlyphId, which is not virtual, so generatePath is not a
//  usable seam. Both routes end at the same cxx bridge entry point, which is
//  exported even in a stripped static build, and that is the seam used here.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_COLR_OUTLINE_H_INCLUDED
#define CHROMIUM_COLR_OUTLINE_H_INCLUDED

#include <cstdint>
#include <vector>

namespace colr_outline {

// The face the scaler is about to draw a color glyph from, and the size to ask
// DWriteCore for its outlines at. Set by the raster hook, the only place that
// knows them, and read back when the layer outlines are asked for. Stays set
// after the hook returns, because the bridge call happens once Skia has taken
// over; only a color glyph writes it and only a color glyph reads it.
void SetSource(const void* face_key, const std::vector<uint8_t>* font, uint32_t face_index,
               float render_size);

// The glyph's subpixel position: phase_x and phase_y in em units for the space
// the paint tree is walked in, sub_x and sub_y in device pixels for the clip
// box. SkScalerContext_DW puts it in the matrix in both generateColorV1Metrics
// and drawColorV1Image; the Fontations scaler puts it in neither, so without
// this a color glyph is rasterized at phase zero whatever position it was
// asked for. Set by the metrics and raster hooks, which both run before the
// walk they precede.
void SetPhase(float phase_x, float phase_y, float sub_x, float sub_y);

// Find the bridge entry point and redirect its call sites. Called from the
// library's constructor, since the symbol scan reads images off disk.
void InstallAtLoad();

}  // namespace colr_outline

#endif  // CHROMIUM_COLR_OUTLINE_H_INCLUDED
