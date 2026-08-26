//+--------------------------------------------------------------------------
//
//  abi_check.cpp - pins the mirrored headers to the Windows x64 ABI.
//
//  libdwritecore.so was compiled against the Windows App SDK headers on the
//  x86-64 System V ABI. Any layout drift between those headers and this mirror
//  would corrupt every call without producing a compile error, so the layouts
//  that matter are asserted here rather than assumed.
//
//----------------------------------------------------------------------------

#include "dwrite_core.h"

#include <cstddef>
#include <cstdio>

// ---------------------------------------------------------------------------
// Scalar types
// ---------------------------------------------------------------------------

// The one that silently breaks everything: Windows WCHAR is 2 bytes, but
// Linux wchar_t is 4. Every string parameter depends on this.
static_assert(sizeof(WCHAR) == 2, "WCHAR must be 2 bytes (char16_t), not wchar_t");
static_assert(sizeof(HRESULT) == 4, "HRESULT must be 4 bytes");
static_assert(sizeof(BOOL) == 4, "BOOL must be 4 bytes");
static_assert(sizeof(FLOAT) == 4, "FLOAT must be 4 bytes");
static_assert(sizeof(GUID) == 16, "GUID must be 16 bytes");
static_assert(sizeof(UINT32) == 4 && sizeof(UINT16) == 2 && sizeof(UINT64) == 8,
              "fixed-width integer types");

// ---------------------------------------------------------------------------
// The ClearType pipeline structures
// ---------------------------------------------------------------------------

static_assert(sizeof(DWRITE_GLYPH_OFFSET) == 8, "DWRITE_GLYPH_OFFSET");
static_assert(sizeof(DWRITE_MATRIX) == 24, "DWRITE_MATRIX");

static_assert(sizeof(DWRITE_GLYPH_RUN) == 48, "DWRITE_GLYPH_RUN size");
static_assert(offsetof(DWRITE_GLYPH_RUN, fontFace) == 0, "GLYPH_RUN.fontFace");
static_assert(offsetof(DWRITE_GLYPH_RUN, fontEmSize) == 8, "GLYPH_RUN.fontEmSize");
static_assert(offsetof(DWRITE_GLYPH_RUN, glyphCount) == 12, "GLYPH_RUN.glyphCount");
static_assert(offsetof(DWRITE_GLYPH_RUN, glyphIndices) == 16, "GLYPH_RUN.glyphIndices");
static_assert(offsetof(DWRITE_GLYPH_RUN, glyphAdvances) == 24, "GLYPH_RUN.glyphAdvances");
static_assert(offsetof(DWRITE_GLYPH_RUN, glyphOffsets) == 32, "GLYPH_RUN.glyphOffsets");
static_assert(offsetof(DWRITE_GLYPH_RUN, isSideways) == 40, "GLYPH_RUN.isSideways");
static_assert(offsetof(DWRITE_GLYPH_RUN, bidiLevel) == 44, "GLYPH_RUN.bidiLevel");

static_assert(sizeof(DWRITE_GLYPH_RUN_DESCRIPTION) == 40, "DWRITE_GLYPH_RUN_DESCRIPTION");
static_assert(sizeof(DWRITE_FONT_METRICS) == 20, "DWRITE_FONT_METRICS");
static_assert(sizeof(DWRITE_GLYPH_METRICS) == 28, "DWRITE_GLYPH_METRICS");

// GDI interop surface.
//
// Caveat: unlike the DWRITE_* structures above, which come from the mirrored
// SDK headers, these types are *defined* in compat.h from the documented Win32
// ABI. Asserting their size here therefore only pins compat.h against
// accidental edits - it cannot independently confirm the Windows layout, since
// both sides of the comparison originate here. Replacing compat.h with the real
// windef.h/wingdi.h would turn these into genuine checks.
static_assert(sizeof(RECT) == 16, "RECT");
static_assert(sizeof(SIZE) == 8, "SIZE");
static_assert(sizeof(LOGFONTW) == 92, "LOGFONTW");
static_assert(offsetof(LOGFONTW, lfFaceName) == 28, "LOGFONTW.lfFaceName");
static_assert(sizeof(FONTSIGNATURE) == 24, "FONTSIGNATURE");

// Enumerations are INT32-based throughout DirectWrite.
static_assert(sizeof(DWRITE_RENDERING_MODE) == 4, "DWRITE_RENDERING_MODE");
static_assert(sizeof(DWRITE_TEXTURE_TYPE) == 4, "DWRITE_TEXTURE_TYPE");
static_assert(sizeof(DWRITE_PIXEL_GEOMETRY) == 4, "DWRITE_PIXEL_GEOMETRY");
static_assert(sizeof(DWRITE_MEASURING_MODE) == 4, "DWRITE_MEASURING_MODE");
static_assert(sizeof(DWRITE_FACTORY_TYPE) == 4, "DWRITE_FACTORY_TYPE");

// ---------------------------------------------------------------------------
// Text layout structures
// ---------------------------------------------------------------------------

static_assert(sizeof(DWRITE_TEXT_RANGE) == 8, "DWRITE_TEXT_RANGE");
static_assert(sizeof(DWRITE_FONT_FEATURE) == 8, "DWRITE_FONT_FEATURE");
static_assert(sizeof(DWRITE_TEXT_METRICS) == 36, "DWRITE_TEXT_METRICS");
static_assert(sizeof(DWRITE_LINE_METRICS) == 24, "DWRITE_LINE_METRICS");
static_assert(sizeof(DWRITE_HIT_TEST_METRICS) == 36, "DWRITE_HIT_TEST_METRICS");
static_assert(sizeof(DWRITE_CLUSTER_METRICS) == 8, "DWRITE_CLUSTER_METRICS");
static_assert(sizeof(DWRITE_TRIMMING) == 12, "DWRITE_TRIMMING");

// ---------------------------------------------------------------------------
// COM vtable shape
//
// A DirectWrite interface must be a pure abstract class with a single vtable
// pointer and no data members, so an interface pointer is exactly a vptr.
// ---------------------------------------------------------------------------

static_assert(sizeof(IDWriteFactory*) == 8, "interface pointer size");
static_assert(sizeof(IUnknown) == 8, "IUnknown is vptr-only");
static_assert(sizeof(IDWriteGlyphRunAnalysis) == 8, "IDWriteGlyphRunAnalysis is vptr-only");

int main()
{
    std::printf("ABI conformance checks passed.\n");
    std::printf("  WCHAR              %2zu bytes\n", sizeof(WCHAR));
    std::printf("  GUID               %2zu bytes\n", sizeof(GUID));
    std::printf("  DWRITE_GLYPH_RUN   %2zu bytes\n", sizeof(DWRITE_GLYPH_RUN));
    std::printf("  DWRITE_MATRIX      %2zu bytes\n", sizeof(DWRITE_MATRIX));
    std::printf("  LOGFONTW           %2zu bytes\n", sizeof(LOGFONTW));

    // The IID table must agree with the strings DWriteCore's QueryInterface
    // parses at runtime; spot-check the factory, whose IID was read out of the
    // binary at 0x251a00.
    const GUID& factory_iid = DWRITE_UUIDOF(IDWriteFactory);
    if (factory_iid.Data1 != 0xB859EE5A)
    {
        std::printf("FAIL: IDWriteFactory IID mismatch\n");
        return 1;
    }
    std::printf("  IID_IDWriteFactory %08X-%04X-%04X ok\n",
                factory_iid.Data1, factory_iid.Data2, factory_iid.Data3);
    return 0;
}
