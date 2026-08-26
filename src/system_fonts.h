//+--------------------------------------------------------------------------
//
//  system_fonts.h - the font list behind the proxy's system-font getters.
//
//  See src/system_fonts.cpp for why this exists and why fontconfig is
//  dlopen'd instead of linked. Every function here returns a borrowed
//  pointer owned by the SystemFonts instance, or nullptr when no font list
//  could be produced - in which case the caller forwards to the real method.
//
//----------------------------------------------------------------------------

#ifndef DWRITECORE_SYSTEM_FONTS_H_INCLUDED
#define DWRITECORE_SYSTEM_FONTS_H_INCLUDED

#include "dwrite_core.h"
#include "DWriteExperimental.h"

#include <new>
#include <string>

namespace dwc
{

struct SystemFonts;

SystemFonts* SystemFontsCreate();
void SystemFontsDestroy(SystemFonts* fonts);

// All three return a *borrowed* pointer, owned by the SystemFonts instance and
// valid for as long as it is. The distinction matters at the two kinds of call
// site in factory_proxy_overrides.cpp:
//
//   * Returned through an out-parameter, COM says the caller owns a reference,
//     so the getter must AddRef before handing it out.
//   * Passed as an input parameter - CreateTextFormat's fontCollection - the
//     callee AddRefs only if it retains it, so the borrow is passed as is.
//
// Getting that backwards leaks on one path and over-releases on the other.
IDWriteFontSet* SystemFontSet(SystemFonts* fonts, IDWriteFactory9* real);
IDWriteFontCollection1* SystemCollection1(SystemFonts* fonts, IDWriteFactory9* real);
IDWriteFontCollection* SystemCollectionLegacy(SystemFonts* fonts, IDWriteFactory9* real);
IDWriteFontCollection2* SystemCollection2(SystemFonts* fonts, IDWriteFactory9* real,
                                          DWRITE_FONT_FAMILY_MODEL model);
IDWriteFontFallback* SystemFallback(SystemFonts* fonts, IDWriteFactory9* real);

// The family fontconfig resolves for a language, used as the last entry of each
// fallback mapping. See src/system_fallback.cpp.
bool FontconfigFamilyForLang(const char* lang, std::string* out);

// Implemented in src/system_fallback.cpp.
IDWriteFontFallback* BuildSystemFallback(IDWriteFactory9* real,
                                         IDWriteFontCollection* collection);

}  // namespace dwc

#endif  // DWRITECORE_SYSTEM_FONTS_H_INCLUDED
