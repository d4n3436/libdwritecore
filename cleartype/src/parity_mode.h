//+--------------------------------------------------------------------------
//
//  parity_mode.h - whether the Firefox parity behavior applies to this
//  process.
//
//  The parity work reproduces what Firefox on Windows would have produced.
//  That is correct when Gecko is the caller and a guess otherwise.
//
//  Build with -DDWRITECORE_FIREFOX_PARITY=0 and none of it is compiled in at
//  all; ParityActive() is then a constant false and every call site folds
//  away.
//
//----------------------------------------------------------------------------

#ifndef CLEARTYPE_PARITY_MODE_H_INCLUDED
#define CLEARTYPE_PARITY_MODE_H_INCLUDED

#ifndef DWRITECORE_FIREFOX_PARITY
#  define DWRITECORE_FIREFOX_PARITY 1
#endif

namespace dwcft
{

// The master switch, honored by every part of this library: CLEARTYPE=0 (or
// off/no/false) and nothing here does anything at all - no rasterization, no
// fontconfig answers, no patching of libxul, and no side table written, so a
// process that sets it pays a forwarded call and nothing else.
bool Enabled();

#if DWRITECORE_FIREFOX_PARITY

// True when this process is a Gecko application and the library is enabled.
// Cheap enough for a per-glyph call: one relaxed load past the first call.
//
// CLEARTYPE_FORCE_PARITY=1 turns it on where it would not be, and =0 off
// where it would be; both are read from the environment at run time, so a
// single command can answer either way without a rebuild.
bool ParityActive();

// Told by libxul_patch.cpp when libxul appears, which is the authoritative
// answer and the one that catches a layout this cannot recognize.
void NoteGeckoLoaded();

// The Gecko process that owns the window, and not one of the twenty-odd
// content, GPU and utility children it starts. Anything that reaches into
// process-wide state the parent alone owns has to ask this first; a child
// has no such state, and looking for it there finds other things.
bool GeckoParentProcess();

#else

inline bool ParityActive() { return false; }
inline void NoteGeckoLoaded() {}
inline bool GeckoParentProcess() { return false; }

#endif

}  // namespace dwcft

#endif  // CLEARTYPE_PARITY_MODE_H_INCLUDED
