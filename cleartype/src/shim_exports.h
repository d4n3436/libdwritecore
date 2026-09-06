//+--------------------------------------------------------------------------
//
//  shim_exports.h - the symbols freetype.cpp exports to libxul_patch.cpp.
//
//  They cross a translation unit boundary as C symbols, where a mismatched
//  signature is a silent ABI error, so both sides take the spelling from here.
//
//----------------------------------------------------------------------------

#ifndef CLEARTYPE_SHIM_EXPORTS_H_INCLUDED
#define CLEARTYPE_SHIM_EXPORTS_H_INCLUDED

#include <cstdint>

extern "C" {

void CleartypeLogLine(const char* message);

// Windows' underline geometry for the face last measured, or 0 when there is
// none. Answers for every face. gfxDWriteFont::ComputeMetrics scales the post
// table by mFUnitsConvFactor where gfxFT2FontBase::InitMetrics takes
// FreeType's own scaled values, and only Windows lowers the underline of a
// family on the bad-underline list. em, ascent and descent come back as the
// values Firefox itself computed, so the caller can locate the metrics struct
// by matching them.
//
// descent_fold is the half pixel ApplyWindowsMetrics moved from the ascent to
// the descent, or zero. Adding it back to max_ascent and taking it off
// max_descent gives the two values Windows holds. A caller that writes the
// underline here makes the fold unnecessary, since nsFontMetrics then folds
// the Windows underline and reaches the Windows MaxAscent and MaxDescent from
// the unfolded pair.
int CleartypeWindowsUnderline(double* underline_offset, double* underline_size,
                              double* em_height, double* max_ascent,
                              double* max_descent, double* descent_fold);

// Windows' strikeout geometry for the face last measured, or 0 when there is
// none. Answers for every face. gfxFT2FontBase::InitMetrics ends its strikeout
// block with SnapLineToPixels, which rounds the thickness to whole pixels and
// moves the offset to keep the line centered, and gfxDWriteFont::ComputeMetrics
// has no such step. em, ascent and descent come back the same way they do for
// the underline, so the caller can locate the metrics struct by matching them.
int CleartypeWindowsStrikeout(double* strikeout_offset, double* strikeout_size,
                              double* em_height, double* max_ascent,
                              double* max_descent);

// How many app units this presentation puts in a device pixel, read out of a
// gfxShapedText by the libxul patch. 60 while a device pixel is a CSS pixel,
// and 48, 40 or 30 at a device pixel ratio of 1.25, 1.5 or 2. The synthetic
// bold tracking is a whole number of these, so it lands where Gecko's does
// only when this is the presentation's own value.
void CleartypeSetAppUnitsPerDevPixel(int units);

// Windows' x-height and cap-height for the face last measured, or 0 when there
// is none. Answers for every face. gfxDWriteFont::ComputeMetrics scales the
// OS/2 sxHeight and sCapHeight by mFUnitsConvFactor and leaves them
// fractional, and gfxFT2FontBase::InitMetrics reads them off a FreeType face
// sized in whole 1/64 px, so the two part company at any other size. em,
// ascent and descent come back as they do for the strikeout, so the caller can
// locate the metrics struct by matching them.
int CleartypeWindowsXCapHeight(double* x_height, double* cap_height,
                               double* em_height, double* max_ascent,
                               double* max_descent);

// The font-units-to-pixels factor for the face last measured, or 0 when there
// is none. linux_factor is what gfxFT2FontBase::InitMetrics writes into
// gfxFont::mFUnitsConvFactor, FreeType's 16.16 x_scale taken to a float, and
// windows_factor is the mAdjustedSize / designUnitsPerEm that
// gfxDWriteFont::ComputeMetrics writes there. FreeType's scale comes from a
// size rounded to 1/64 px, so the two differ for any size that is not a
// multiple of it. The first is exact enough to name the field.
int CleartypeWindowsUnitsPerPixel(double* linux_factor, double* windows_factor,
                                  double* em_height, double* max_ascent,
                                  double* max_descent);

// The Windows maxAscent and maxDescent for the face being measured, at a size
// other than the one FreeType holds. See the definition for why a font under
// one pixel needs it.
int CleartypeWindowsMetricsAtSize(double size, double* asc, double* desc);

// Windows' two leadings for the face last measured, or 0 when there is none.
// Answers for every face, not only the bad-underline ones: InitMetrics derives
// both from the size metrics, and disagrees with Windows for any face whose
// ascent and descent do not fill the em.
// linux_external is the external leading gfxFT2FontBase::InitMetrics derives
// for itself. gfxFont::SanitizeMetrics overwrites that field, and nothing
// else, when the @font-face rule carries line-gap-override, so a struct whose
// external leading is not this value is one the author has overridden and the
// caller leaves alone.
int CleartypeWindowsLeading(double* internal_leading, double* external_leading,
                            double* em_height, double* max_ascent,
                            double* max_descent, double* linux_external);

// Ends the claim the three accessors above answer from. They name the face
// whose sfnt tables were last handed out on this thread, which is the face
// gfxFT2FontBase::InitMetrics is measuring only while that call is still in
// flight. InitMetrics returns before reading OS/2 for a face it cannot size,
// so without this the next call inherits the previous face's answer, and two
// faces of one family at one size agree on everything the metrics struct is
// identified by.
void CleartypeEndInitMetrics(void);

// Name the face and the size the running gfxFT2FontBase::InitMetrics is
// measuring, both read out of the font object. Answers zero when the pointer
// is not a face this library knows, so a wrong guess at the object's layout
// claims nothing.
int CleartypeClaimFace(void* candidate, double ft_size);

// Name the unquantized size a face is about to be measured at, before the
// measuring starts. Kept so that the em can be recovered from the 26.6 char
// size FreeType is given, which is all a size that is not a whole app unit
// leaves behind, and so that a size FindClosestSize moved can still be found
// from the one the face ends up carrying. Answers zero for a pointer this
// library does not know as a face, or one that is not scalable.
int CleartypeClaimSize(void* candidate, double px);

// The whole-pixel size Windows rounds gfxFont::mAdjustedSize to for the face
// last measured, or 0 when that face keeps the size it was asked for.
// gfxDWriteFont::ComputeMetrics rounds it for a CJK face that carries a strike
// at the rounded size, after mFUnitsConvFactor has been taken from the
// unrounded one, so the two sizes come back separately. GetScaledFont then
// hands WebRender the rounded one and the strike is drawn at its own ppem.
//
// space_width, zero_width and ideographic_width come with it. InitMetrics
// takes those three from advances of its own, measured before the caller has
// rounded anything, so Windows' own values replace them.
int CleartypeWindowsStrikeSize(double* rounded, double* unrounded,
                               double* space_width, double* zero_width,
                               double* ideographic_width, double* em_height,
                               double* max_ascent, double* max_descent);

// Windows' average character width and maximum advance for the face last
// measured, or 0 when there is none. nsTextControlFrame sizes a text control
// as the width of its `size` in average characters plus the difference between
// the two.
int CleartypeWindowsCharWidth(double* ave_char_width, double* max_advance,
                              double* em_height, double* max_ascent,
                              double* max_descent);

// One point of a glyph outline as it reaches Skia's path builder. `match` is
// what the builder receives, a 26.6 coordinate divided by 64, and `exact` is
// the same coordinate before it was quantized. The pair lets a caller find the
// finished point array by value and replace it with the unquantized one.
struct CleartypeGlyphPathPoint
{
    float match_x, match_y;
    float exact_x, exact_y;
};

// Brackets one SkScalerContext_FreeType::generatePath. Between them
// FT_Outline_Decompose answers from the outline DirectWrite returns instead
// of FreeType's, so the path is built out of the same curves
// SkScalerContext_DW builds it from, and every point handed to the builder is
// recorded.
void CleartypeBeginGlyphPath(void);

// Ends the bracket and hands back what was recorded, in the order the builder
// received it. Zero when nothing was recorded, which is the ordinary case for
// a glyph this library cannot answer for. The array belongs to the shim and
// is valid until the next CleartypeBeginGlyphPath on this thread.
unsigned CleartypeEndGlyphPath(const CleartypeGlyphPathPoint** points);

// Bracket a Skia scaler call, so a FreeType render reached from inside one is
// attributed to Skia rather than to the thread it runs on.
void CleartypeEnterSkiaScaler(void);
void CleartypeLeaveSkiaScaler(void);

// What the Skia scaler about to draw on this thread was built with: the five
// SkScalerContextRec fields SkScalerContextRec::getSingleMatrix makes the
// total matrix out of. `post` is fPost2x2 in row order, or null to forget the
// whole thing, which is what a call this library cannot read a scaler out of
// passes. Recovering these from the char size FreeType was given is a search
// that cannot always succeed; they are the values themselves.
void CleartypeSkiaScaler(double text_size, double pre_scale_x, double pre_skew_x,
                         const double* post);

// Whether this thread is one of WebRender's blob rasterizers. Gecko replays a
// blob image through DrawTargetSkia on those, which is the one place Skia
// asks its own scan converter for a glyph on both platforms.
int CleartypeOnBlobRaster(void);

// True when the glyph about to be drawn on this blob thread has to come from
// DirectWrite's mask rather than from its outline, which is an unhinted face
// under a non-uniform scale. Everything else keeps the outline route.
int CleartypeBlobPrefersMask(void);

// The ink box of one glyph in pixels, as four edges: left, top, right, bottom.
// `candidate` is offered a word at a time the way CleartypeClaimFace is, and
// anything but a face this library knows is refused. `ft_size` is the font's
// mFTSize, which names the instance, and `embolden` its mEmbolden. Answers only
// for a box the slot's 26.6 metrics were written from, so a caller can undo
// that rounding.
int CleartypeGlyphInkBox(void* candidate, double ft_size, int embolden, unsigned glyph,
                         double* out);

// The horizontal origin gfxHarfBuzzShaper::GetGlyphVOrigin sets for one glyph
// on Windows, in 16.16. Half the advance the shaper answers with there, which
// carries no synthetic bold unless DirectWrite is simulating it on the face.
// `candidate`, `ft_size` and `embolden` are read the way CleartypeGlyphInkBox
// reads them.
int CleartypeWindowsVOriginX(void* candidate, double ft_size, int embolden, unsigned glyph,
                             int32_t* x);

// Names the WebRender font instance the next glyph is loaded from, so the size
// it holds answers for that glyph instead of an inversion of FreeType's 26.6.
void CleartypeNoteWebRenderInstance(const void* instance);

}  // extern "C"

#endif  // CLEARTYPE_SHIM_EXPORTS_H_INCLUDED
