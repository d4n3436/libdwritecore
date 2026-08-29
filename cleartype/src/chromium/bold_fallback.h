//+--------------------------------------------------------------------------
//
//  bold_fallback.h - draw the real bold face where Linux settles for a
//  synthetic one.
//
//  Per-character fallback carries no weight on Linux. Blink asks the browser
//  for a font covering the character (ui/gfx/font_fallback_linux.cc, which
//  sorts fontconfig on FC_LANG and FC_SCALABLE alone and caches the set per
//  locale), gets a regular face back, and settles for synthetic bold in
//  FontCache::PlatformFallbackFontForCharacter. Windows hands the weight to
//  matchFamilyStyleCharacter and gets the real Bold face, whose advances come
//  from its own hmtx, so the two sides lay the run out differently.
//
//  The weight is gone before fontconfig is called, so the interposer cannot
//  answer the query differently. What does survive is the request: Skia turns
//  the synthetic bold into a stroke in SkScalerContextRec::useStrokeForFakeBold,
//  and that stroke reaches every scaler context. So the face is swapped here
//  instead, at the point the glyph is measured and drawn.
//
//  The font files are mapped at library load, which in a renderer means in
//  the zygote it was forked from. A sandboxed renderer cannot open a file,
//  and holding the descriptors open across the fork trips the zygote's own
//  checks, so each file is mapped and its descriptor closed immediately. The
//  mapping is inherited and its pages are read on first touch.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_BOLD_FALLBACK_H_INCLUDED
#define CHROMIUM_BOLD_FALLBACK_H_INCLUDED

#include <cstdint>
#include <vector>

namespace bold_fallback {

// Maps the bold faces. Called from the library constructor, which is early
// enough to still have file access, and cheap enough to run there because
// mapping reads nothing.
void MapAtLoad();

// Takes the synthetic bold off a rec whose family has a real bold face,
// leaving it the shape Windows would have carried. Called from filterRec,
// which runs before the typeface's own turns the flag into a stroke.
//
// The rec is marked in fReservedAlign, a padding byte Skia never reads, since
// the later hooks have no other way to tell this run from a plain one. A run
// the page already gave a stroke keeps its width, which is why the mark does
// not live in fFrameWidth.
bool ClearSyntheticBold(void* rec, const std::vector<uint8_t>& font);

// Whether ClearSyntheticBold marked this rec.
bool WasMarked(const void* rec);

// Whether the rec carries synthetic italic. Blink writes it as the SkFont's
// skew and SkScalerContext copies it into fPreSkewX.
bool IsOblique(float pre_skew_x);

// A face inside a mapped file. `bytes` is the whole file, which for a
// collection holds several faces and needs `face_index` to say which.
struct Face
{
    const std::vector<uint8_t>* bytes = nullptr;
    uint32_t face_index = 0;

    // Draw the font already in hand, with DirectWrite's bold simulation, which
    // is what Windows keeps for a face carrying bitmap strikes. The simulation
    // widens advances, so it is not the same as Skia's synthetic bold.
    bool simulate = false;
};

// The face DirectWrite would have answered with for the family `font` belongs
// to. `oblique` is whether the run carries synthetic italic. Empty when the
// family has nothing heavier than the face already in hand, which is the case
// Windows also renders with synthetic bold, and empty for an oblique run whose
// family has an italic face, which Windows answers with that face instead.
Face RealBoldFor(const std::vector<uint8_t>& font, bool oblique);

}  // namespace bold_fallback

#endif  // CHROMIUM_BOLD_FALLBACK_H_INCLUDED
