#ifndef CHROMIUM_FONT_FACTS_H_INCLUDED
#define CHROMIUM_FONT_FACTS_H_INCLUDED

#include "windows_path.h"

#include <cstdint>
#include <vector>

namespace font_facts {

struct Span
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct GaspRange
{
    int min_ppem = 0;
    int max_ppem = 0;
    int version = 0;
    uint16_t flags = 0;
};

// `face_index` says which face of a collection to read. The bytes of a
// substituted bold face are the whole file it came out of, so for Nirmala.ttc
// and its like they are the same bytes whichever face is wanted, and reading
// face zero would describe the wrong one.
Span FindTable(const std::vector<uint8_t>& font, uint32_t tag, uint32_t face_index = 0);
bool IsHinted(const std::vector<uint8_t>& font, uint32_t face_index = 0);
bool GaspRangeFor(const std::vector<uint8_t>& font, int ppem, GaspRange* out,
                  uint32_t face_index = 0);
bool IsGridfitOnly(uint16_t flags);
bool SymmetricSmoothing(uint16_t flags);
bool HasBitmapStrike(const std::vector<uint8_t>& font, const GaspRange& range,
                     uint32_t face_index = 0);
bool HasCbdt(const std::vector<uint8_t>& font, uint32_t face_index = 0);

// Whether the font carries monochrome bitmap strikes. Skia's DirectWrite font
// manager tests exactly this table and no other, and it is what decides
// whether a simulated face survives; see SimulatesOblique.
bool HasEbdt(const std::vector<uint8_t>& font, uint32_t face_index = 0);

// Whether Windows would render this font through Fontations rather than
// DirectWrite. WebFontTypefaceFactory routes avar version 2, CFF2, CBDT and
// COLRv1 web fonts to MakeFontationsFallbackPreferred, so for those the
// parity substitutions have to stand aside and let plain Fontations answer,
// which is what both platforms then run. Segoe UI Emoji is the one COLRv1
// face the Windows install itself carries, and it stays on DirectWrite.
//
// The avar2 rule is the one that depends on which Chromium is running, so
// the answer is not a function of the bytes alone; see ChromiumMilestone.
bool FontationsPreferred(const std::vector<uint8_t>& font, uint32_t face_index = 0);

// Everything the Windows decision tree wants to know about this font, at the
// size it is about to be rendered. SkScalerContext_DW rounds gdiTextSize for
// the gasp lookup and truncates it for the bitmap strike, so both arrive.
windows_path::FontFacts Describe(const std::vector<uint8_t>& font, int gasp_ppem,
                                 int bitmap_ppem, uint32_t face_index = 0);

}  // namespace font_facts

#endif  // CHROMIUM_FONT_FACTS_H_INCLUDED
