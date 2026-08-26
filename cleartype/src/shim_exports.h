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
int CleartypeWindowsUnderline(double* underline_offset, double* underline_size,
                              double* em_height, double* max_ascent,
                              double* max_descent);

// Windows' two leadings for the face last measured, or 0 when there is none.
// Answers for every face, not only the bad-underline ones: InitMetrics derives
// both from the size metrics, and disagrees with Windows for any face whose
// ascent and descent do not fill the em.
int CleartypeWindowsLeading(double* internal_leading, double* external_leading,
                            double* em_height, double* max_ascent,
                            double* max_descent);

// Windows' average character width and maximum advance for the face last
// measured, or 0 when there is none. nsTextControlFrame sizes a text control
// as the width of its `size` in average characters plus the difference between
// the two.
int CleartypeWindowsCharWidth(double* ave_char_width, double* max_advance,
                              double* em_height, double* max_ascent,
                              double* max_descent);

}  // extern "C"

#endif  // CLEARTYPE_SHIM_EXPORTS_H_INCLUDED
