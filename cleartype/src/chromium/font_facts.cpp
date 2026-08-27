//+--------------------------------------------------------------------------
//
//  font_facts.cpp - the questions SkScalerContext_DW asks the font tables.
//
//  On Windows these come off the DWrite font face. Here they come out of the
//  SFNT that typeface_bridge.cpp rebuilds, so the answers are the same ones
//  Windows would get from the same font.
//
//  Translated from src/ports/SkScalerContext_win_dw.cpp: is_hinted,
//  get_gasp_range, is_gridfit_only and has_bitmap_strike.
//
//----------------------------------------------------------------------------

// Style inspections left as they are: the shapes they suggest either read
// worse against the sources being mirrored, or would change which overload
// is chosen if one were ever added.
// ReSharper disable CppUseDesignatedInitializers
// ReSharper disable CppUseStructuredBinding

#include "font_facts.h"

#include <cstring>

namespace font_facts {
namespace {

uint16_t Be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

constexpr uint32_t Tag(const char a, const char b, const char c, const char d)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<unsigned char>(d);
}

// gasp range behavior bits, from src/sfnt/SkOTTable_gasp.h.
constexpr uint16_t kGaspGridfit = 1u << 0;
constexpr uint16_t kGaspSymmetricSmoothing = 1u << 3;

}  // namespace

Span FindTable(const std::vector<uint8_t>& font, const uint32_t tag)
{
    if (font.size() < 12) {
        return {};
    }
    const uint16_t num_tables = Be16(font.data() + 4);
    if (font.size() < 12 + static_cast<size_t>(num_tables) * 16) {
        return {};
    }
    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t* entry = font.data() + 12 + static_cast<size_t>(i) * 16;
        if (Be32(entry) != tag) {
            continue;
        }
        const uint32_t offset = Be32(entry + 8);
        const uint32_t length = Be32(entry + 12);
        if (static_cast<uint64_t>(offset) + length > font.size()) {
            return {};
        }
        return {font.data() + offset, length};
    }
    return {};
}

// is_hinted: a version 1.0 maxp whose maxSizeOfInstructions is not zero.
bool IsHinted(const std::vector<uint8_t>& font)
{
    const Span maxp = FindTable(font, Tag('m', 'a', 'x', 'p'));
    // version(4) then twelve uint16 before maxSizeOfInstructions at 26.
    if (maxp.data == nullptr || maxp.size < 28) {
        return false;
    }
    if (Be32(maxp.data) != 0x00010000) {
        return false;
    }
    return Be16(maxp.data + 26) != 0;
}

// get_gasp_range: the first range whose maxPPEM is at or above this size.
bool GaspRangeFor(const std::vector<uint8_t>& font, const int ppem, GaspRange* out)
{
    const Span gasp = FindTable(font, Tag('g', 'a', 's', 'p'));
    if (gasp.data == nullptr || gasp.size < 4) {
        return false;
    }
    const uint16_t version = Be16(gasp.data);
    if (version != 0 && version != 1) {
        return false;
    }
    const uint16_t num_ranges = Be16(gasp.data + 2);
    if (num_ranges > 1024 || gasp.size < 4 + static_cast<size_t>(num_ranges) * 4) {
        return false;
    }
    int min_ppem = -1;
    for (uint16_t i = 0; i < num_ranges; ++i) {
        const uint8_t* r = gasp.data + 4 + static_cast<size_t>(i) * 4;
        const int max_ppem = Be16(r);
        if (ppem <= max_ppem) {
            out->min_ppem = min_ppem + 1;
            out->max_ppem = max_ppem;
            out->version = version;
            out->flags = Be16(r + 2);
            return true;
        }
        min_ppem = max_ppem;
    }
    return false;
}

bool IsGridfitOnly(const uint16_t flags)
{
    return flags == kGaspGridfit;
}

bool SymmetricSmoothing(const uint16_t flags)
{
    return (flags & kGaspSymmetricSmoothing) != 0;
}

// has_bitmap_strike: an EBLC with a square strike inside the gasp range that
// covers at least three glyphs.
bool HasBitmapStrike(const std::vector<uint8_t>& font, const GaspRange& range)
{
    const Span eblc = FindTable(font, Tag('E', 'B', 'L', 'C'));
    if (eblc.data == nullptr || eblc.size < 8) {
        return false;
    }
    if (Be32(eblc.data) != 0x00020000) {   // version_initial, 2.0
        return false;
    }
    const uint32_t num_sizes = Be32(eblc.data + 4);
    // A BitmapSizeTable is 48 bytes.
    constexpr size_t kSizeTable = 48;
    if (num_sizes > 1024 || eblc.size < 8 + static_cast<size_t>(num_sizes) * kSizeTable) {
        return false;
    }
    for (uint32_t i = 0; i < num_sizes; ++i) {
        const uint8_t* s = eblc.data + 8 + static_cast<size_t>(i) * kSizeTable;
        // startGlyphIndex and endGlyphIndex sit at 40 and 42; ppemX and ppemY
        // at 44 and 45.
        const uint16_t start_glyph = Be16(s + 40);
        const uint16_t end_glyph = Be16(s + 42);
        const uint8_t ppem_x = s[44];
        if (const uint8_t ppem_y = s[45];
            ppem_x == ppem_y && range.min_ppem <= ppem_x && ppem_x <= range.max_ppem &&
            end_glyph >= start_glyph + 3) {
            return true;
        }
    }
    return false;
}

bool HasCbdt(const std::vector<uint8_t>& font)
{
    return FindTable(font, Tag('C', 'B', 'D', 'T')).data != nullptr;
}

windows_path::FontFacts Describe(const std::vector<uint8_t>& font, const int gdi_ppem)
{
    windows_path::FontFacts f;
    if (font.empty()) {
        return f;
    }
    f.is_hinted = IsHinted(font);
    f.has_cbdt = HasCbdt(font);

    GaspRange range;
    if (GaspRangeFor(font, gdi_ppem, &range)) {
        f.gasp_known = true;
        f.gasp_version_1_or_later = range.version >= 1;
        f.gasp_symmetric_smoothing = SymmetricSmoothing(range.flags);
        f.gasp_gridfit_only = IsGridfitOnly(range.flags);
    }

    // has_bitmap_strike is asked with the gasp range for this ppem, and with
    // a bare range when that range is not gridfit-only, the same narrowing
    // SkScalerContext_DW's constructor does before calling it.
    GaspRange bitmap_range{gdi_ppem, gdi_ppem, 0, 0};
    if (f.gasp_known && f.gasp_gridfit_only) {
        bitmap_range = range;
    }
    f.has_bitmap_strike = HasBitmapStrike(font, bitmap_range);
    return f;
}

}  // namespace font_facts
