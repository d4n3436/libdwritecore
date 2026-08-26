//+--------------------------------------------------------------------------
//
//  compat.h - what the Windows SDK cannot supply.
//
//  Every Windows type, macro and decoration the DirectWrite headers use is
//  real SDK text, extracted by tools/mirror_sdk_headers.py into the headers
//  included at the bottom of this file. What remains here is only:
//
//    1. MSVC *compiler* keywords. These are built into the compiler, not
//       declared by any header, so off-Windows they must be neutralized.
//
//    2. The deliberate ABI deviations - WCHAR and the LP64 group. These are
//       the places where mirroring the SDK verbatim would be actively wrong;
//       each is documented at its definition and asserted in
//       tests/abi_check.cpp.
//
//    3. Two project decisions: how the entry points are exported, and the
//       replacement for __uuidof.
//
//----------------------------------------------------------------------------

#ifndef DWRITE_COMPAT_H_INCLUDED
#define DWRITE_COMPAT_H_INCLUDED
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>   // guiddef.h's IsEqualGUID is written in terms of memcmp

// ---------------------------------------------------------------------------
// 1. MSVC compiler keywords
//
// libdwritecore.so targets the x86-64 System V ABI, which has one calling
// convention, so __stdcall expands to nothing. (__cdecl is not here: winnt.h
// defines it as empty in its non-MSVC branch, so it comes from the mirror.)
// __int64 is MSVC's spelling of a 64-bit integer, used by the SDK headers
// themselves.
// ---------------------------------------------------------------------------

#define __stdcall

#ifndef __int64
#define __int64 long long
#endif

// ---------------------------------------------------------------------------
// 2a. Deliberate deviation: WCHAR
//
// The SDK types WCHAR as wchar_t, which is 2 bytes under MSVC but 4 bytes on
// Linux. Mirroring that verbatim would silently misalign every string
// parameter in the API rather than failing to compile, so WCHAR is pinned to
// char16_t here.
//
// DWriteCore itself has a rust/dwrite/src/wchar.rs module for the same reason.
// ---------------------------------------------------------------------------

// typedef, not using: this header is guarded for C as well as C++, and the
// bionic-compat and probe translation units are C.
// ReSharper disable CppEnforceTypeAliasCodeStyle
#ifdef __cplusplus
typedef char16_t  WCHAR;
#else
typedef uint16_t  WCHAR;
#endif

// ---------------------------------------------------------------------------
// 2b. Deliberate deviations: the LP64 group
//
// LONG, ULONG, DWORD, HRESULT and GUID all share one cause. The Windows SDK
// spells its 32-bit types as "long", which is 32 bits under Windows' LLP64
// model but 64 bits under LP64. Mirroring that text verbatim silently doubles
// the width of everything built on it:
//
//   * minwindef.h's "typedef unsigned long DWORD" made FONTSIGNATURE 48 bytes
//     instead of 24 - caught by tests/abi_check.cpp.
//   * windef.h's RECT would have become 32 bytes instead of 16.
//   * guiddef.h declares GUID.Data1 as "unsigned long", giving a 24-byte GUID.
//   * winnt.h has two spellings of HRESULT: the __midl branch says
//     "typedef LONG HRESULT", but the branch a C++ compiler takes says raw
//     "long".
//
// These are excluded from the mirror manifests and pinned here. Everything
// downstream stays real SDK text - windef.h's RECT comes out correct once LONG
// is right.
// ---------------------------------------------------------------------------

typedef int32_t   LONG;
typedef uint32_t  ULONG;
typedef uint32_t  DWORD;
typedef int32_t   HRESULT;

typedef struct _GUID
{
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID;

// ---------------------------------------------------------------------------
// The mirrored Windows SDK headers.
//
// Included here, after the types above that they are written in terms of.
// Order matters. specstrings.h comes first because the extracted SDK text
// carries its own SAL annotations - winnt.h spells LPSTR with
// _Null_terminated_. basetsd.h and minwindef.h precede winnt.h because the
// declarations taken from winnt.h are written in terms of INT32, PVOID and
// CONST.
// ---------------------------------------------------------------------------

#include "specstrings.h"
#include "basetsd.h"
#include "minwindef.h"
#include "winnt.h"
#include "guiddef.h"
#include "rpcdce.h"
#include "rpc.h"
#include "rpcsal.h"
#include "rpcndr.h"
#include "basetyps.h"
#include "combaseapi.h"
#include "windef.h"
#include "wingdi.h"
#include "winerror.h"
#include "dxgiformat.h"
#include "d3d9types.h"
#include "d2dbasetypes.h"

// ---------------------------------------------------------------------------
// 3a. DWRITE_EXPORT
//
// The SDK marks the entry points __declspec(dllimport), which is what a
// consumer of the retail DLL needs. This project *defines* them, so they get
// default ELF visibility instead.
// ---------------------------------------------------------------------------

#ifndef DWRITE_EXPORT
#define DWRITE_EXPORT __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------------------
// 3b. Interface UUID association
//
// The SDK pairs DECLSPEC_UUID with __uuidof, which is a compiler feature clang
// offers only under -fms-extensions - no header declares it. Each mirrored
// interface instead registers its IID through a traits specialization emitted
// into dwrite_iids.h.
//
// The _WIN32 guard below is the one this header keeps. Nothing here builds for
// Windows, but __uuidof is a keyword rather than a macro, so !defined(__uuidof)
// does not see it - only the platform test stops the definition from colliding
// with the real thing if these headers ever reach an MSVC-family compiler.
// ---------------------------------------------------------------------------

#ifdef __cplusplus

template <typename T>
struct DWriteInterfaceTraits;

#define DWRITE_DEFINE_IID(iface, d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7) \
    template <>                                                             \
    struct DWriteInterfaceTraits<iface>                                     \
    {                                                                       \
        static const GUID& IID_Value()                                      \
        {                                                                   \
            static const GUID value =                                       \
                { d1, d2, d3, { b0, b1, b2, b3, b4, b5, b6, b7 } };          \
            return value;                                                   \
        }                                                                   \
    };

#define DWRITE_UUIDOF(T) (DWriteInterfaceTraits<T>::IID_Value())

#if !defined(_WIN32) && !defined(__uuidof)
#define __uuidof(T) DWRITE_UUIDOF(T)
#endif

#endif // __cplusplus

#endif // DWRITE_COMPAT_H_INCLUDED
