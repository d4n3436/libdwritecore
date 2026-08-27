//+--------------------------------------------------------------------------
//
//  render_params.h - the font settings Chromium takes from the host, mirrored
//  from what Windows would have answered.
//
//  How a glyph is rasterized is settled before DirectWrite is ever asked for
//  it. Electron reads the host's settings once per WebContents in
//  shell/browser/api/electron_api_web_contents.cc and pushes them to every
//  renderer, where web_view_impl.cc hands them to WebFontRenderStyle and
//  web_font_render_style.cc turns them into an SkFont. Subpixel rendering
//  becomes SkFont::Edging::kSubpixelAntiAlias, which is what makes the mask
//  LCD16 rather than A8.
//
//  The same Electron line runs on both platforms, but GetFontRenderParams
//  does not. ui/gfx/font_render_params_win.cc reads the ClearType registry
//  and states the rest, while ui/gfx/font_render_params_linux.cc asks
//  fontconfig and the desktop, so a Linux Electron renders with whatever the
//  desktop says. On a session set to grayscale antialiasing that is A8, and
//  no amount of getting DirectWrite right afterwards produces ClearType.
//
//  This applies font_render_params_win.cc's answer to the SkScalerContextRec,
//  the last place those decisions are still visible. Font smoothing on with
//  ClearType is what Windows ships, so that branch is taken as read.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_RENDER_PARAMS_H_INCLUDED
#define CHROMIUM_RENDER_PARAMS_H_INCLUDED

#include <cstdint>

namespace render_params {

// Adjust a live SkScalerContextRec in place, as SkTypeface::onFilterRec is
// allowed to. `flags_before_filter` is the rec's fFlags as it arrived, sampled
// before the typeface's own onFilterRec runs. Fontations clears
// kGenA8FromLCD_Flag there, and that flag is the only evidence that the
// surface could not display subpixel text.
void ApplyWindowsParams(void* rec, uint16_t flags_before_filter);

}  // namespace render_params

#endif  // CHROMIUM_RENDER_PARAMS_H_INCLUDED
