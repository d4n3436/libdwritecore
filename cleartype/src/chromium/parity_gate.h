//+--------------------------------------------------------------------------
//
//  parity_gate.h - whether this process wants the Windows behavior, and
//  which Chromium it is.
//
//  Off unless asked for. With it off nothing here touches a typeface,
//  answers a fontconfig query or reorders fallback.
//
//  CLEARTYPE=0 turns this off with everything else in the library. Past that
//  it is its own switch and not dwcft::ParityActive(), which asks whether the
//  process is Gecko. The answer here is only whether the user asked.
//
//----------------------------------------------------------------------------

#ifndef CLEARTYPE_CHROMIUM_PARITY
#  define CLEARTYPE_CHROMIUM_PARITY 1
#endif

#ifndef CHROMIUM_PARITY_GATE_H_INCLUDED
#define CHROMIUM_PARITY_GATE_H_INCLUDED

namespace chromium_patch {
#if CLEARTYPE_CHROMIUM_PARITY
// On unless CLEARTYPE_CHROMIUM says otherwise, and off whenever CLEARTYPE is
// off.
bool ParityWanted();

// The Chromium milestone the host was built from, or 0 when no loaded image
// names one. Blink's web font routing differs between the milestones in use,
// so a rule that mirrors it has to know which one is running. A release
// build is stripped, so the answer comes from the image's own strings.
int ChromiumMilestone();

#else

// Built without the Chromium patch, so nothing of it applies.
inline bool ParityWanted() { return false; }
inline int ChromiumMilestone() { return 0; }

#endif

}  // namespace chromium_patch

#endif  // CHROMIUM_PARITY_GATE_H_INCLUDED
