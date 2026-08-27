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

Span FindTable(const std::vector<uint8_t>& font, uint32_t tag);
bool IsHinted(const std::vector<uint8_t>& font);
bool GaspRangeFor(const std::vector<uint8_t>& font, int ppem, GaspRange* out);
bool IsGridfitOnly(uint16_t flags);
bool SymmetricSmoothing(uint16_t flags);
bool HasBitmapStrike(const std::vector<uint8_t>& font, const GaspRange& range);
bool HasCbdt(const std::vector<uint8_t>& font);

// Everything the Windows decision tree wants to know about this font, at the
// size it is about to be rendered.
windows_path::FontFacts Describe(const std::vector<uint8_t>& font, int gdi_ppem);

}  // namespace font_facts

#endif  // CHROMIUM_FONT_FACTS_H_INCLUDED
