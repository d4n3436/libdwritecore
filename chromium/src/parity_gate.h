//+--------------------------------------------------------------------------
//
//  parity_gate.h - whether this process wants the Windows behavior.
//
//  Off unless asked for. With it off nothing here touches a typeface,
//  answers a fontconfig query or reorders fallback.
//
//  Separate from dwcft::Enabled() in cleartype/src/parity_mode.h, which gates
//  the Firefox interposer and has to detect Gecko. This library is preloaded
//  deliberately, so the only question is whether the user asked.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_PARITY_GATE_H_INCLUDED
#define CHROMIUM_PARITY_GATE_H_INCLUDED

namespace chromium_patch {

// CHROMIUM_PATCH_DWRITE=1 or =on.
bool ParityWanted();

}  // namespace chromium_patch

#endif  // CHROMIUM_PARITY_GATE_H_INCLUDED
