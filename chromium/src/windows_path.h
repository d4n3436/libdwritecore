//+--------------------------------------------------------------------------
//
//  windows_path.h - what Chromium's Windows text path decides, computed on
//  Linux from the same inputs.
//
//  Above the typeface the pipeline is identical on both platforms. It forks in
//  skia/ext/font_utils.cc's fontmgr_factory(), which builds SkFontMgr_New_FCI
//  with a Fontations scanner on Linux and SkFontMgr_New_DirectWrite on
//  Windows. Every glyph then goes to SkFontationsScalerContext, where skrifa
//  outlines are filled by Skia's scan converter, or to SkScalerContext_DW,
//  which hands the glyph to DirectWrite.
//
//  The two rasterize differently, and the choice of how is made entirely
//  inside the Windows scaler context's constructor. DirectWrite is told what
//  to do, it does not decide.
//
//  This is that decision, translated from:
//
//    src/ports/SkScalerContext_win_dw.cpp   SkScalerContext_DW::SkScalerContext_DW
//                                           SkScalerContext_DW::getDWMaskBits
//                                           SkScalerContext_DW::generateDWImage
//    src/core/SkScalerContext.h             SkScalerContextRec, the flags and
//                                           the hinting encoding
//
//  Comments quoting Skia are Skia's own, kept where they explain a branch that
//  would otherwise look arbitrary.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_WINDOWS_PATH_H_INCLUDED
#define CHROMIUM_WINDOWS_PATH_H_INCLUDED

#include "skia_abi.h"

namespace windows_path {

// DWRITE_RENDERING_MODE
enum RenderingMode
{
    kRenderDefault = 0,
    kRenderAliased = 1,
    kRenderGdiClassic = 2,
    kRenderGdiNatural = 3,
    kRenderNatural = 4,
    kRenderNaturalSymmetric = 5,
    kRenderOutline = 6,
};

// DWRITE_TEXTURE_TYPE
enum TextureType
{
    kTextureAliased1x1 = 0,
    kTextureClearType3x1 = 1,
};

// DWRITE_MEASURING_MODE
enum MeasuringMode
{
    kMeasureNatural = 0,
    kMeasureGdiClassic = 1,
    kMeasureGdiNatural = 2,
};

// DWRITE_GRID_FIT_MODE
enum GridFitMode
{
    kGridFitDefault = 0,
    kGridFitDisabled = 1,
    kGridFitEnabled = 2,
};

// DWRITE_TEXT_ANTIALIAS_MODE
enum AntiAliasMode
{
    kAntiAliasClearType = 0,
    kAntiAliasGrayscale = 1,
};

// Facts about the font that only the font tables can answer. On Windows
// SkScalerContext_DW reads them off the DWrite font face; nothing in the
// Fontations scaler context exposes them, so they are supplied separately and
// each one says whether it is actually known. Left unknown, the tree takes
// the same branch it would for a font that lacks the table, which is what
// Skia does for a font with no gasp and no bitmap strike.
struct FontFacts
{
    bool gasp_known = false;
    bool gasp_version_1_or_later = false;
    bool gasp_symmetric_smoothing = false;
    bool gasp_gridfit_only = false;

    bool has_bitmap_strike = false;   // at this ppem, for the gasp range
    bool is_hinted = false;           // a non-empty fpgm, prep or cvt
    bool has_cbdt = false;

    bool factory2 = true;             // DWriteCore always has IDWriteFactory2
    bool fontface2 = true;
};

struct Decision
{
    float real_text_size = 0;
    float gdi_text_size = 0;
    float text_size_render = 0;
    float text_size_measure = 0;
    RenderingMode rendering_mode = kRenderNaturalSymmetric;
    TextureType texture_type = kTextureClearType3x1;
    MeasuringMode measuring_mode = kMeasureNatural;
    GridFitMode grid_fit_mode = kGridFitEnabled;
    AntiAliasMode anti_alias_mode = kAntiAliasClearType;

    // Which of the constructor's branches was taken, for reporting.
    const char* branch = "";
};

const char* RenderingModeName(RenderingMode m);
const char* TextureTypeName(TextureType t);
const char* MeasuringModeName(MeasuringMode m);

// SkScalerContext_DW's constructor, on the values Skia would have given it.
// `scale_y` is what computeMatrices(kVertical) puts in scale.fY, the device
// y-scale, which is the size DirectWrite is actually asked for.
Decision Decide(const skia_abi::Rec& rec, float scale_y, const FontFacts& facts);

// The device y-scale, from the same rec fields computeMatrices reads. Only
// the axis-aligned case is exact here; a rotated or skewed matrix needs the
// full decomposition, and Decide is told when the answer is approximate.
float DeviceScaleY(const skia_abi::Rec& rec);

// src/ports/SkScalerContext_win_dw.cpp, is_axis_aligned.
bool IsAxisAligned(const skia_abi::Rec& rec);

}  // namespace windows_path

#endif  // CHROMIUM_WINDOWS_PATH_H_INCLUDED
