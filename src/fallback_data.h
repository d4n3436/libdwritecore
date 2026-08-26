//+--------------------------------------------------------------------------
//
//  fallback_data.h - the per-script fallback table's shape.
//
//  The table itself is generated; see tools/gen_fallback_data.py for where the
//  ranges and the family preferences each come from, and src/system_fallback.cpp
//  for what is built out of them.
//
//----------------------------------------------------------------------------

#ifndef DWRITECORE_FALLBACK_DATA_H_INCLUDED
#define DWRITECORE_FALLBACK_DATA_H_INCLUDED

#include "dwrite_core.h"
#include "DWriteExperimental.h"

namespace dwc
{

struct FallbackRange
{
    UINT32 first;
    UINT32 last;
};

// Pointers first and the count last: interleaved, the 4-byte count sat in its
// own 8-byte slot and the entry carried padding.
//
// No font is named here. What covers a script is whatever fontconfig resolves
// for fc_lang on this machine; preferring another platform's families is a
// parity question and lives in libcleartype.
struct FallbackEntry
{
    const char* script;              // label, for diagnostics only
    const char* fc_lang;             // the language whose answer covers it
    const FallbackRange* ranges;
    unsigned range_count;
};

extern const FallbackEntry kFallbackEntries[];
extern const unsigned kFallbackEntryCount;

}  // namespace dwc

#endif  // DWRITECORE_FALLBACK_DATA_H_INCLUDED
