//+--------------------------------------------------------------------------
//
//  hb_abi.h - the part of HarfBuzz this library calls, declared here rather
//  than included.
//
//  Nothing here is linked against. Every entry point is resolved by name, the
//  same way fontconfig.cpp reaches fontconfig, so the build needs no HarfBuzz
//  package and a process without HarfBuzz resolves nothing.
//
//  Mirrors, from harfbuzz 14.3.1:
//
//    hb-common.h  hb_bool_t, hb_tag_t, hb_destroy_func_t
//    hb-blob.h    hb_blob_t, hb_memory_mode_t, hb_blob_create, hb_blob_destroy
//    hb-face.h    hb_face_t, hb_face_create, hb_face_get_upem,
//                 hb_face_get_glyph_count, hb_face_destroy, hb_face_get_empty
//    hb-font.h    hb_font_t, hb_font_funcs_t, hb_font_create,
//                 hb_font_create_sub_font, hb_font_destroy, hb_font_get_face,
//                 hb_font_get_scale, hb_font_set_scale, hb_font_get_ptem,
//                 hb_font_set_ptem, hb_font_set_funcs,
//                 hb_font_funcs_get_empty
//    hb-ot-font.h hb_ot_font_set_funcs
//    hb-ot-var.h  hb_ot_var_get_axis_count
//    hb-shape.h   hb_feature_t, hb_shape, hb_shape_full
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_HB_ABI_H_INCLUDED
#define CHROMIUM_HB_ABI_H_INCLUDED

#include <cstdint>

extern "C" {

using hb_bool_t = int;
using hb_tag_t = uint32_t;
using hb_destroy_func_t = void (*)(void*);

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;
struct hb_font_funcs_t;
struct hb_buffer_t;

enum hb_memory_mode_t
{
    HB_MEMORY_MODE_DUPLICATE,
    HB_MEMORY_MODE_READONLY,
    HB_MEMORY_MODE_WRITABLE,
    HB_MEMORY_MODE_READONLY_MAY_MAKE_WRITABLE
};

// hb-shape.h. Opaque here: this library forwards the array it was handed and
// never reads a feature.
struct hb_feature_t
{
    hb_tag_t tag;
    uint32_t value;
    unsigned int start;
    unsigned int end;
};

}  // extern "C"

#include <string>
#include <unordered_map>

namespace hb_abi {

using BlobCreateFn = hb_blob_t* (*)(const char*, unsigned int, hb_memory_mode_t,
                                    void*, hb_destroy_func_t);
using BlobDestroyFn = void (*)(hb_blob_t*);
using FaceCreateFn = hb_face_t* (*)(hb_blob_t*, unsigned int);
using FaceDestroyFn = void (*)(hb_face_t*);
using FaceGetUpemFn = unsigned int (*)(const hb_face_t*);
using FaceGetGlyphCountFn = unsigned int (*)(const hb_face_t*);
using FontCreateFn = hb_font_t* (*)(hb_face_t*);
using FontCreateSubFontFn = hb_font_t* (*)(hb_font_t*);
using FontDestroyFn = void (*)(hb_font_t*);
using FontGetFaceFn = hb_face_t* (*)(hb_font_t*);
using FontGetScaleFn = void (*)(hb_font_t*, int*, int*);
using FontSetScaleFn = void (*)(hb_font_t*, int, int);
using FontGetPtemFn = float (*)(hb_font_t*);
using FontSetPtemFn = void (*)(hb_font_t*, float);
using FontSetFuncsFn = void (*)(hb_font_t*, hb_font_funcs_t*, void*,
                                hb_destroy_func_t);
using OtFontSetFuncsFn = void (*)(hb_font_t*);
using OtVarGetAxisCountFn = unsigned int (*)(hb_face_t*);
using ShapeFn = void (*)(hb_font_t*, hb_buffer_t*, const hb_feature_t*,
                         unsigned int);
using ShapeFullFn = int (*)(hb_font_t*, hb_buffer_t*, const hb_feature_t*,
                            unsigned int, const char* const*);
using FontFuncsGetEmptyFn = hb_font_funcs_t* (*)();
using FaceGetEmptyFn = hb_face_t* (*)();

// How HarfBuzz got into this process, which decides whether the entry points
// this library exports are ever called.
enum class Linkage
{
    // Nothing answered to the name. Shaping is left alone.
    kAbsent,
    // A shared library, so the loader binds Blink's calls to the exports of
    // whichever object comes first, and a preloaded one comes before all.
    kInterposable,
    // Compiled into the binary. Blink's calls were bound at build time and
    // never reach the interposed entry points, so hb_shape is replaced in
    // place.
    kInImage,
};

// Work out which of those it is, and read the symbol table off disk if it
// takes that. Needs the filesystem, so it runs in the zygote.
void ResolveAtLoad();

Linkage Where();

// The real entry point behind ours, or null when HarfBuzz is not in this
// process. Resolved once per name and cached by the caller.
//
// In kInImage this hands back what the binary's own symbol table names, which
// for hb_shape is the function about to be replaced. Anything that has to
// defer to HarfBuzz's real behavior goes through hb_shape_full instead.
void* Real(const char* name);

// Every symbol in the process whose name starts with `prefix`, read from each
// loaded image's dynamic table and, where a static link kept them out of it,
// from the symbol table the file holds on disk. Needs the filesystem, so it
// runs in the zygote.
//
// This is how a hook reaches a library the build compiled in: the loader
// binds nothing to a preloaded export there, so RTLD_NEXT answers null and
// the address has to come from the image instead.
std::unordered_map<std::string, void*> SymbolsWithPrefix(const char* prefix);

}  // namespace hb_abi

#endif  // CHROMIUM_HB_ABI_H_INCLUDED
