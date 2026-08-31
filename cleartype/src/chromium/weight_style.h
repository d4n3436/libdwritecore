//+--------------------------------------------------------------------------
//
//  weight_style.h - the weight a face reports, so synthetic bold starts where
//  Windows starts it.
//
//  Blink decides synthetic bold differently on the two platforms, and both
//  forms are in the tree this build was made from:
//
//    win    font_cache_skia_win.cc:503
//           Weight() >= kBoldThreshold && !typeface->isBold()
//
//    linux  font_cache_skia.cc:330
//           Weight() > FontSelectionValue(200)
//                      + FontSelectionValue(typeface->fontStyle().weight())
//
//  Against a face of weight 400 the Linux form is `> 600`, so weight 600
//  misses it and 700 hits, while Windows takes 600. Every family with no bold
//  face draws regular text at 600 on Linux and emboldened text on Windows.
//
//  The Linux form reads the face's own weight, so reporting 399 for a face
//  that reports 400 makes the comparison `> 599`, which is the Windows
//  threshold exactly. It changes nothing where a bolder face exists, because
//  then that face is the one selected and its own weight is what gets read.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_WEIGHT_STYLE_H_INCLUDED
#define CHROMIUM_WEIGHT_STYLE_H_INCLUDED

namespace weight_style {

// Find the bridge entry point and redirect its call sites. Called from the
// library's constructor, since the symbol scan reads images off disk.
void InstallAtLoad();

// The same, for a build whose fontconfig is compiled in and where Skia builds
// the style from an FcPattern rather than from the Fontations bridge.
void InstallFontconfig();

}  // namespace weight_style

#endif  // CHROMIUM_WEIGHT_STYLE_H_INCLUDED
