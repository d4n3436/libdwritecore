//+--------------------------------------------------------------------------
//
//  dwritecore_shim.h - the entry points this build has and retail does not.
//
//  Not a DirectWrite header. Include <dwrite_core.h> for DirectWrite itself,
//  the way a caller on Windows does; this declares only what is particular to
//  standing the Android DWriteCore up on glibc, and only the few files that
//  care about that need it. Definitions are in src/dwritecore_shim.cpp.
//
//----------------------------------------------------------------------------

#ifndef DWRITECORE_SHIM_H_INCLUDED
#define DWRITECORE_SHIM_H_INCLUDED
#pragma once

#ifndef DWRITECORE_API
#define DWRITECORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Non-null when the implementation could not be loaded; describes why. The
// one export here that retail has no equivalent for: loading the real
// library is this build's problem and nobody else's, so failing to do it
// needs somewhere to be reported.
DWRITECORE_API const char* DWriteCoreShimGetLastLoadError(void);

// Undocumented, and in no Windows App SDK header: the signature was
// recovered from libdwriteshim.so. Declared here so the definitions in
// src/dwritecore_shim.cpp have one.
DWRITECORE_API void DWriteCoreSetFeatureStagingCallback(void* context, void* callback);
DWRITECORE_API void DWrite10Velocity_SetFeatureStagingCallback(void* context, void* callback);

#ifdef __cplusplus
}
#endif

#endif  // DWRITECORE_SHIM_H_INCLUDED
