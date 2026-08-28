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
// The rec is marked by leaving fFrameWidth at a value of its own. Skia only
// tests that field's sign, so any negative reads as no stroke, and the later
// hooks have no other way to tell this run from a plain one.
//
// A run the page already gave a stroke is left alone, since the mark would
// overwrite the width. Blink draws such text in two passes, so the fill still
// gets the real face and only its outline stays synthetic.
bool ClearSyntheticBold(void* rec, const std::vector<uint8_t>& font);

// Whether ClearSyntheticBold marked this rec.
bool WasMarked(float frame_width);

// A face inside a mapped file. `bytes` is the whole file, which for a
// collection holds several faces and needs `face_index` to say which.
struct Face
{
    const std::vector<uint8_t>* bytes = nullptr;
    uint32_t face_index = 0;
};

// The face DirectWrite would have answered with for the family `font` belongs
// to. Empty when the family has nothing heavier than the face already in
// hand, which is the case Windows also renders with synthetic bold.
Face RealBoldFor(const std::vector<uint8_t>& font);

}  // namespace bold_fallback

#endif  // CHROMIUM_BOLD_FALLBACK_H_INCLUDED
