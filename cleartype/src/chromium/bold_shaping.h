//+--------------------------------------------------------------------------
//
//  bold_shaping.h - shape a substituted bold run with the bold face.
//
//  The scaler patch in bold_fallback.h swaps the face where a glyph is
//  measured and drawn. Positioning is decided a level above, by HarfBuzz,
//  which Blink points at the regular face for both weights, so the bold run
//  is laid out with the regular face's GPOS. This swaps the face for shaping
//  as well, leaving the advances alone.
//
//  The entry points below are called only where HarfBuzz is a shared library.
//  A build that compiles it in binds Blink's calls at build time, and
//  InstallAtLoad replaces hb_shape in place instead. hb_abi.h tells the two
//  apart.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_BOLD_SHAPING_H_INCLUDED
#define CHROMIUM_BOLD_SHAPING_H_INCLUDED

#include <cstdint>
#include <vector>

#include "hb_abi.h"

// The font behind a typeface, from the cache vtable_patch.cpp keeps for the
// scaler hooks. Null unless those hooks have already read this typeface, which
// makes it safe to call on a pointer that is only believed to be one. Shaping
// reads the same bytes they do, so the two cannot disagree about which runs
// carry a substituted face.
const std::vector<uint8_t>* ChromiumFontBytes(void* typeface);

// The variation coordinates the typeface was cloned with, read once beside
// the bytes, or null for a static face. The pointee lives as long as the
// typeface entry.
namespace dwrite_raster { struct VariationCoord; }
const std::vector<dwrite_raster::VariationCoord>* ChromiumVariationCoords(
    const void* typeface);

namespace bold_shaping {

// Work out how this build links HarfBuzz and install the swap the way that
// build needs. Reads a symbol table off disk, so it belongs in the zygote,
// beside the rest of the load-time work.
void InstallAtLoad();

// Live entries in the shaping side tables, for the census. Substitutes is
// thread_local, so this reports the calling thread's own.
void CensusCounts(size_t* bounds, size_t* substitutes);

}  // namespace bold_shaping

// The interposed HarfBuzz entry points. Declared so each definition is checked
// against the signature this library mirrors.
//
// hb_shape is where the face is swapped. The other two only keep track of what
// Blink bound to a font, since HarfBuzz has no way to ask for it back, and of
// when that font goes away.
extern "C" __attribute__((visibility("default")))
void hb_shape(hb_font_t* font, hb_buffer_t* buffer, const hb_feature_t* features,
              unsigned int num_features);

extern "C" __attribute__((visibility("default")))
void hb_font_set_funcs(hb_font_t* font, hb_font_funcs_t* klass, void* font_data,
                       hb_destroy_func_t destroy);

extern "C" __attribute__((visibility("default")))
void hb_font_destroy(hb_font_t* font);

#endif  // CHROMIUM_BOLD_SHAPING_H_INCLUDED
