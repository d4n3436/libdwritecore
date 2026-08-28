//+--------------------------------------------------------------------------
//
//  family_match.h - pick the face within a family that Windows would pick.
//
//  Asking for a named family at a weight no face carries exactly is answered
//  differently on the two platforms. The answer is a different file, so the
//  glyphs themselves differ.
//
//  Windows goes through SkFontStyleSet_DirectWrite::matchStyle, which hands
//  the weight to IDWriteFontFamily::GetFirstMatchingFont unchanged. Linux
//  goes through SkFontConfigInterfaceDirect::matchFamilyName, which converts
//  the weight to fontconfig's scale, sorts with FcFontSort and takes the
//  first entry the set offers. The two scales are not proportional, so the
//  faces swap over at different weights: Segoe UI at 500 is Semibold on
//  Windows and regular on Linux.
//
//  fontconfig still decides what is installed, what a family name means and
//  which faces answer to it. Only the choice among the faces already in the
//  set is replaced.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_FAMILY_MATCH_H_INCLUDED
#define CHROMIUM_FAMILY_MATCH_H_INCLUDED

namespace family_match {

// The set FcFontSort returned, reordered so the face DirectWrite would have
// answered with sits first. cleartype/src/fontconfig.cpp owns the interposer
// and calls this. Does nothing to a set that was not sorted for a named
// family, which is how the per-character fallback sort is left alone.
void ReorderForWindows(const void* pattern, void* sorted);

// map_ranges on the fontconfig weight column, so a candidate's fontconfig
// weight reads as the weight its OS/2 table holds.
float OpenTypeWeight(int fc_weight);

// GetFirstMatchingFont's pick between two OpenType weights, for a request of
// `wanted`. True when `candidate` is the one DirectWrite would answer with.
// cleartype/src/chromium/bold_fallback.cpp asks the same question of a family
// whose faces it is choosing among.
bool BeatsForWindows(float candidate, float best, float wanted);

}  // namespace family_match

#endif  // CHROMIUM_FAMILY_MATCH_H_INCLUDED
