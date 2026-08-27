#pragma once

namespace fallback_order {

// The set FcFontSort returned, so the families and charsets in it can be
// learned. cleartype/src/fontconfig.cpp owns the interposer and calls this.
void NoteFontSet(const void* pattern, void* sorted);

}  // namespace fallback_order
