#pragma once

namespace fallback_order {

// The set FcFontSort returned, so the families and charsets in it can be
// learned. cleartype/src/fontconfig.cpp owns the interposer and calls this.
void NoteFontSet(const void* pattern, void* sorted);

// One row of the script table: the code point range, and the families Windows
// answers it with, in the order font_fallback_win.cc lists them.
struct ScriptRow
{
    unsigned first;
    unsigned last;
    const char* const* families;
    unsigned count;
};

// The whole table, for a build that cannot reach fontconfig to impose the
// order a call at a time and has to state it up front instead.
const ScriptRow* Scripts(unsigned* count);

}  // namespace fallback_order
