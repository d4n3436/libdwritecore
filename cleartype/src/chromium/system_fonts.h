//+--------------------------------------------------------------------------
//
//  system_fonts.h - the CSS system font keywords, answered as Windows does.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_SYSTEM_FONTS_H_INCLUDED
#define CHROMIUM_SYSTEM_FONTS_H_INCLUDED

#include <cstdint>

#include <link.h>

namespace system_fonts {

// Make `font: menu`, `font: small-caption` and `font: status-bar` resolve to
// the family and size Windows gives them. Called once with the executable's
// own segments. Every patch is declined unless the code it is about to
// overwrite matches byte for byte, so a build this does not recognize keeps
// the behavior it had.
void ApplyToImage(uintptr_t base, const ElfW(Phdr)* phdr, ElfW(Half) phnum);

}  // namespace system_fonts

#endif  // CHROMIUM_SYSTEM_FONTS_H_INCLUDED
