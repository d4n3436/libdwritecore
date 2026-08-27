#pragma once

#include <cstdint>

#include <link.h>

namespace render_params_patch {

// Replace the loaded image's GetFontRenderParamsFromFcPattern with one that
// states Windows' answers. See render_params_patch.cpp.
void Apply(uintptr_t base, const ElfW(Phdr)* phdr, ElfW(Half) phnum);

}  // namespace render_params_patch
