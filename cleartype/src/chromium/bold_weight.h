//+--------------------------------------------------------------------------
//
//  bold_weight.h - whether the run asking for a fallback font is bold.
//
//  Chromium's per-character fallback drops the weight before it asks: the
//  mojo call is FallbackFontForCharacter(character, locale) and carries no
//  style, so Linux answers with the regular face and Blink applies a synthetic
//  bold, while Windows resolves the family at the real weight and gets the
//  true Bold face. The weight still exists one frame up, in
//  FontCache::PlatformFallbackFontForCharacter's FontDescription, which is
//  where this reads it.
//
//  A build with symbols is not required. See bold_weight.cpp for how the
//  function and the offset are found in a stripped one.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_BOLD_WEIGHT_H_INCLUDED
#define CHROMIUM_BOLD_WEIGHT_H_INCLUDED

namespace bold_weight {

// Find the function and redirect its one call site. Called from the library's
// constructor, before the sandbox closes, since it reads the mapped image.
void InstallAtLoad();

// Whether the fallback being answered on this thread belongs to a bold run.
// False whenever the hook is not installed, so a caller can ask unguarded.
bool RunIsBold();

// Set on the character of a fallback query made for a bold run. The weight
// does not survive the mojo call to the browser, and the character is the only
// argument that reaches gfx::GetFallbackFontForChar per query, so the request
// travels on it. Bit 30, which no code point reaches and which leaves the
// value positive. It also keys the renderer's own per-character cache
// (WebSandboxSupportLinux::unicode_font_families_), which ignores both weight
// and locale, so without a distinct character a bold answer and a regular one
// would overwrite each other.
constexpr int kBoldMark = 1 << 30;

}  // namespace bold_weight

#endif  // CHROMIUM_BOLD_WEIGHT_H_INCLUDED
