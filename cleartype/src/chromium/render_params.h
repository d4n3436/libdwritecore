//+--------------------------------------------------------------------------
//
//  render_params.h - the font settings Chromium takes from the host, answered
//  the way Windows answers them.
//
//  How a glyph is rasterized is settled before DWriteCore is ever asked for
//  it. Chromium reads the host's settings once in the browser process and
//  pushes them to every renderer, where WebFontRenderStyle turns them into an
//  SkFont; subpixel rendering becomes SkFont::Edging::kSubpixelAntiAlias,
//  which is what makes the mask LCD16 rather than A8.
//
//  ui/gfx/font_render_params_win.cc reads the ClearType registry and states
//  the rest, while ui/gfx/font_render_params_linux.cc asks fontconfig and the
//  desktop, so a Linux Chromium rasterizes with whatever the desktop says. On
//  a session set to grayscale antialiasing that is A8, whatever DWriteCore is
//  asked for afterwards.
//
//  Two entry points, at the two places those decisions are still visible: the
//  SkScalerContextRec each scaler context is built from, and the function the
//  browser process reads the settings with. Font smoothing on with ClearType
//  is what Windows ships, so that branch is taken as read.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_RENDER_PARAMS_H_INCLUDED
#define CHROMIUM_RENDER_PARAMS_H_INCLUDED

#include <cstdint>

#include <link.h>

namespace render_params {

// Adjust a live SkScalerContextRec in place, as SkTypeface::onFilterRec is
// allowed to. `flags_before_filter` is the rec's fFlags as it arrived, sampled
// before the typeface's own onFilterRec runs. Fontations clears
// kGenA8FromLCD_Flag there, and that flag is the only evidence that the
// surface could not display subpixel text. `plain_fontations` says Windows
// renders this typeface through plain Fontations, whose rec keeps SkFont's
// default hinting and never regains kGenA8FromLCD_Flag.
void ApplyWindowsParams(void* rec, uint16_t flags_before_filter, bool plain_fontations);

// Replace the loaded image's GetFontRenderParamsFromFcPattern with one that
// states those same answers, for the browser process, which is where Chromium
// reads them once and sends them to every renderer over Mojo. Needed because
// Chrome, Electron and CEF link fontconfig statically, so interposing it
// reaches nothing.
void ApplyToImage(uintptr_t base, const ElfW(Phdr)* phdr, ElfW(Half) phnum);

}  // namespace render_params

#endif  // CHROMIUM_RENDER_PARAMS_H_INCLUDED
