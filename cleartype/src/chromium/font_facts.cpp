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
#include <string>

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

// No face by that index, or nothing that parses as a font.
constexpr size_t kNoDirectory = static_cast<size_t>(-1);

// gasp range behavior bits, from src/sfnt/SkOTTable_gasp.h.
constexpr uint16_t kGaspGridfit = 1u << 0;
constexpr uint16_t kGaspSymmetricSmoothing = 1u << 3;

// Where one face's table directory starts. A single face is its own directory
// at zero; a collection puts a 'ttcf' header there instead, holding an offset
// per face. Reading a collection as if it were a single face finds no table at
// all, since what sits where the table count belongs is half of a version.
size_t DirectoryAt(const std::vector<uint8_t>& font, const uint32_t face_index)
{
    if (font.size() < 12) {
        return kNoDirectory;
    }
    if (Be32(font.data()) != Tag('t', 't', 'c', 'f')) {
        return face_index == 0 ? 0 : kNoDirectory;
    }
    const uint32_t faces = Be32(font.data() + 8);
    if (face_index >= faces ||
        font.size() < 12 + static_cast<size_t>(faces) * 4) {
        return kNoDirectory;
    }
    const uint32_t at = Be32(font.data() + 12 + static_cast<size_t>(face_index) * 4);
    return static_cast<uint64_t>(at) + 12 <= font.size() ? at : kNoDirectory;
}

}  // namespace

Span FindTable(const std::vector<uint8_t>& font, const uint32_t tag,
               const uint32_t face_index)
{
    const size_t base = DirectoryAt(font, face_index);
    if (base == kNoDirectory) {
        return {};
    }
    const uint16_t num_tables = Be16(font.data() + base + 4);
    if (font.size() < base + 12 + static_cast<size_t>(num_tables) * 16) {
        return {};
    }
    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t* entry = font.data() + base + 12 + static_cast<size_t>(i) * 16;
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
bool IsHinted(const std::vector<uint8_t>& font, const uint32_t face_index)
{
    const Span maxp = FindTable(font, Tag('m', 'a', 'x', 'p'), face_index);
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
bool GaspRangeFor(const std::vector<uint8_t>& font, const int ppem, GaspRange* out,
                  const uint32_t face_index)
{
    const Span gasp = FindTable(font, Tag('g', 'a', 's', 'p'), face_index);
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
bool HasBitmapStrike(const std::vector<uint8_t>& font, const GaspRange& range,
                     const uint32_t face_index)
{
    const Span eblc = FindTable(font, Tag('E', 'B', 'L', 'C'), face_index);
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

// The typeface family name, which is name ID 1. Unicode and Windows records
// are UTF-16BE and Macintosh ones are single byte, and all are read since a
// font need not carry any one of them.
std::string FamilyName(const std::vector<uint8_t>& font, const uint32_t face_index)
{
    const Span name = FindTable(font, Tag('n', 'a', 'm', 'e'), face_index);
    if (name.data == nullptr || name.size < 6) {
        return {};
    }
    const uint16_t count = Be16(name.data + 2);
    const uint16_t storage = Be16(name.data + 4);
    if (name.size < 6 + static_cast<size_t>(count) * 12) {
        return {};
    }
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* r = name.data + 6 + static_cast<size_t>(i) * 12;
        if (Be16(r + 6) != 1) {       // name ID 1, the family
            continue;
        }
        const uint16_t platform = Be16(r);
        const uint16_t length = Be16(r + 8);
        const size_t at = storage + Be16(r + 10);
        if (at + length > name.size) {
            continue;
        }
        std::string out;
        if (platform == 0 || platform == 3) {   // UTF-16BE, ASCII is enough here
            for (uint16_t k = 1; k < length; k += 2) {
                out.push_back(static_cast<char>(name.data[at + k]));
            }
        } else {
            out.assign(reinterpret_cast<const char*>(name.data + at), length);
        }
        if (!out.empty()) {
            return out;
        }
    }
    return {};
}

// The families blink refuses embedded bitmaps for, from
// bitmap_glyphs_block_list.cc. Calibri is the one font with both bitmaps and
// outlines for Latin, and its bitmaps space unevenly under subpixel
// positioning.
bool BlocksEmbeddedBitmaps(const std::vector<uint8_t>& font, const uint32_t face_index)
{
    const std::string family = FamilyName(font, face_index);
    return family == "Calibri" || family == "Courier New";
}

bool HasCbdt(const std::vector<uint8_t>& font, const uint32_t face_index)
{
    return FindTable(font, Tag('C', 'B', 'D', 'T'), face_index).data != nullptr;
}

bool HasEbdt(const std::vector<uint8_t>& font, const uint32_t face_index)
{
    return FindTable(font, Tag('E', 'B', 'D', 'T'), face_index).data != nullptr;
}

windows_path::FontFacts Describe(const std::vector<uint8_t>& font, const int gasp_ppem,
                                 const int bitmap_ppem, const uint32_t face_index)
{
    windows_path::FontFacts f;
    if (font.empty()) {
        return f;
    }
    f.is_hinted = IsHinted(font, face_index);
    f.has_cbdt = HasCbdt(font, face_index);
    f.blocks_embedded_bitmaps = BlocksEmbeddedBitmaps(font, face_index);

    GaspRange range;
    if (GaspRangeFor(font, gasp_ppem, &range, face_index)) {
        f.gasp_known = true;
        f.gasp_version_1_or_later = range.version >= 1;
        f.gasp_symmetric_smoothing = SymmetricSmoothing(range.flags);
        f.gasp_gridfit_only = IsGridfitOnly(range.flags);
    }

    // has_bitmap_strike is asked with the gasp range at the truncated ppem,
    // and with a bare range when that range is not gridfit-only, the same
    // narrowing SkScalerContext_DW's constructor does before calling it. Its
    // lookup is its own, at a ppem that need not equal the gasp one.
    GaspRange bitmap_range{bitmap_ppem, bitmap_ppem, 0, 0};
    if (GaspRange at_bitmap; GaspRangeFor(font, bitmap_ppem, &at_bitmap, face_index) &&
                             IsGridfitOnly(at_bitmap.flags)) {
        bitmap_range = at_bitmap;
    }
    f.has_bitmap_strike = HasBitmapStrike(font, bitmap_range, face_index);
    return f;
}

}  // namespace font_facts
