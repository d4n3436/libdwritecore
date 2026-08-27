//+--------------------------------------------------------------------------
//
//  parity_gate.h - whether this process wants the Windows behavior.
//
//  Off unless asked for. With it off nothing here touches a typeface,
//  answers a fontconfig query or reorders fallback.
//
//  CLEARTYPE=0 turns this off with everything else in the library. Past that
//  it is its own switch and not dwcft::ParityActive(), which asks whether the
//  process is Gecko. The answer here is only whether the user asked.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_PARITY_GATE_H_INCLUDED
#define CHROMIUM_PARITY_GATE_H_INCLUDED

namespace chromium_patch {

// CHROMIUM_PATCH_DWRITE=1 or =on.
bool ParityWanted();

}  // namespace chromium_patch

#endif  // CHROMIUM_PARITY_GATE_H_INCLUDED
