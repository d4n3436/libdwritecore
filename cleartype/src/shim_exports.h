//+--------------------------------------------------------------------------
//
//  shim_exports.h - the symbols freetype.cpp exports to libxul_patch.cpp.
//
//  They cross a translation unit boundary as C symbols, where a mismatched
//  signature is a silent ABI error, so both sides take the spelling from here.
//
//----------------------------------------------------------------------------

#ifndef CLEARTYPE_SHIM_EXPORTS_H_INCLUDED
#define CLEARTYPE_SHIM_EXPORTS_H_INCLUDED

extern "C" {

void CleartypeLogLine(const char* message);

// Windows' underline geometry for the face last measured, or 0 when there is
// none. em, ascent and descent come back as the values Firefox itself
// computed, so the caller can locate the metrics struct by matching them.
//
// descent_fold is the half pixel ApplyWindowsMetrics moved from the ascent to
// the descent, or zero. Adding it back to max_ascent and taking it off
// max_descent gives the two values Windows holds. A caller that writes the
// underline here makes the fold unnecessary, since nsFontMetrics then folds
// the Windows underline and reaches the Windows MaxAscent and MaxDescent from
// the unfolded pair.
int CleartypeWindowsUnderline(double* underline_offset, double* underline_size,
                              double* em_height, double* max_ascent,
                              double* max_descent, double* descent_fold);

// Windows' two leadings for the face last measured, or 0 when there is none.
// Answers for every face, not only the bad-underline ones: InitMetrics derives
// both from the size metrics, and disagrees with Windows for any face whose
// ascent and descent do not fill the em.
int CleartypeWindowsLeading(double* internal_leading, double* external_leading,
                            double* em_height, double* max_ascent,
                            double* max_descent);

// Ends the claim the three accessors above answer from. They name the face
// whose sfnt tables were last handed out on this thread, which is the face
// gfxFT2FontBase::InitMetrics is measuring only while that call is still in
// flight. InitMetrics returns before reading OS/2 for a face it cannot size,
// so without this the next call inherits the previous face's answer, and two
// faces of one family at one size agree on everything the metrics struct is
// identified by.
void CleartypeEndInitMetrics(void);

// Windows' average character width and maximum advance for the face last
// measured, or 0 when there is none. nsTextControlFrame sizes a text control
// as the width of its `size` in average characters plus the difference between
// the two.
int CleartypeWindowsCharWidth(double* ave_char_width, double* max_advance,
                              double* em_height, double* max_ascent,
                              double* max_descent);

}  // extern "C"

#endif  // CLEARTYPE_SHIM_EXPORTS_H_INCLUDED
