//+--------------------------------------------------------------------------
//
//  fallback_hook.h - per-character fallback where fontconfig cannot be
//  interposed. See fallback_hook.cpp.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_FALLBACK_HOOK_H_INCLUDED
#define CHROMIUM_FALLBACK_HOOK_H_INCLUDED

namespace fallback_hook {

// Replaces gfx::GetFallbackFontForChar where it can be found. Does nothing
// twice, and nothing at all when the function cannot be recognized.
void Install();

}  // namespace fallback_hook

#endif  // CHROMIUM_FALLBACK_HOOK_H_INCLUDED
