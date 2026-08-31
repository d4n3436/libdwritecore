#pragma once

#ifndef CLEARTYPE_CHROMIUM_PARITY
#  define CLEARTYPE_CHROMIUM_PARITY 1
#endif

namespace fallback_order {

// One row of the script table: the code point range, and the families Windows
// answers it with, in the order font_fallback_win.cc lists them.
struct ScriptRow
{
    unsigned first;
    unsigned last;
    const char* const* families;
    unsigned count;
};

#if CLEARTYPE_CHROMIUM_PARITY
// The set FcFontSort returned, so the families and charsets in it can be
// learned. cleartype/src/fontconfig.cpp owns the interposer and calls this.
void NoteFontSet(const void* pattern, void* sorted);

// The whole table, for a build that cannot reach fontconfig to impose the
// order a call at a time and has to state it up front instead.
const ScriptRow* Scripts(unsigned* count);

// The unified-Han candidates for one content language, in the order
// font_fallback_win.cc lists them. Null when the language names no Han script,
// which is GetFallbackFamily leaving USCRIPT_HAN unsettled.
const char* const* HanCandidates(const char* locale, unsigned* count);

// The families Windows answers this character with, in the order
// font_fallback_win.cc lists them. Null where no row names one, which is
// GetFallbackFamily naming nothing and the pan-Unicode list answering. For a
// build that has to name the family a character at a time.
const char* const* FamiliesFor(unsigned codepoint, unsigned* count);

// Whether the sorted set's own order answers this character, which is unified
// Han and the scripts whose family is Times New Roman. Both are already in
// front of it, so nothing has to be named.
bool SetOrderAnswers(unsigned codepoint);

// GetFallbackFamilyNameFromHardcodedChoices' pan-Unicode list, walked when the
// script's own family does not cover the character. `cjk` picks the list an
// unsettled USCRIPT_HAN takes; every other script takes the common one.
const char* const* PanUnicode(bool cjk, unsigned* count);

// Which measured IDWriteFontFallback row answers this character, or -1. That
// is what Windows reaches once the script family and the pan-Unicode list have
// both missed, so the family named here is walked after both of them.
int DWriteRowFor(unsigned codepoint);

// The row's family, and how many rows there are, for a build that has to state
// every answer up front instead of asking per character.
const char* DWriteRowFamily(int row);
unsigned DWriteRowCount();

// The family the row puts in front of the pan-Unicode list, or null. Only a
// build with no sorted set to watch reads it.
const char* DWriteRowFront(int row);

// The first codepoint the row covers, which names the script families that
// come before it.
unsigned DWriteRowFirst(int row);

// The family the sorted set is relied on to put in front for this character,
// or null where the run's language decides it. Only a build with no sort reads
// it.
const char* SetOrderFront(unsigned codepoint);

#else

// Built without the Chromium patch, so no order is imposed.
inline void NoteFontSet(const void*, void*) {}
inline const ScriptRow* Scripts(unsigned* count)
{
    if (count != nullptr) {
        *count = 0;
    }
    return nullptr;
}
inline const char* const* HanCandidates(const char*, unsigned* count)
{
    if (count != nullptr) {
        *count = 0;
    }
    return nullptr;
}
inline const char* const* FamiliesFor(unsigned, unsigned* count)
{
    if (count != nullptr) {
        *count = 0;
    }
    return nullptr;
}
inline bool SetOrderAnswers(unsigned) { return true; }
inline const char* const* PanUnicode(bool, unsigned* count)
{
    if (count != nullptr) {
        *count = 0;
    }
    return nullptr;
}
inline int DWriteRowFor(unsigned) { return -1; }
inline const char* DWriteRowFamily(int) { return nullptr; }
inline unsigned DWriteRowCount() { return 0; }
inline const char* DWriteRowFront(int) { return nullptr; }
inline unsigned DWriteRowFirst(int) { return 0; }
inline const char* SetOrderFront(unsigned) { return nullptr; }

#endif

}  // namespace fallback_order
