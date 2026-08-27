//+--------------------------------------------------------------------------
//
//  generic_families.h - the per-script generic font families Windows uses.
//
//  Mirrors chrome/app/resources/locale_settings_win.grd, the values
//  chrome/browser/ui/prefs/prefs_tab_helper.cc and Electron's copy of it in
//  shell/browser/font_defaults.cc read into WebPreferences' script font
//  family maps.
//
//----------------------------------------------------------------------------

#ifndef DWC_CHROMIUM_GENERIC_FAMILIES_H
#define DWC_CHROMIUM_GENERIC_FAMILIES_H

#include <cstddef>

namespace generic_families {

// Rewrites the generic font family values inside one mapped resource bundle,
// replacing what this platform ships with what Windows ships. Returns how many
// were replaced.
unsigned PatchBundle(void* base, size_t length);

}  // namespace generic_families

#endif  // DWC_CHROMIUM_GENERIC_FAMILIES_H
