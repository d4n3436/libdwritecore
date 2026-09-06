//+--------------------------------------------------------------------------
//
//  path_abi.h - the SkPath types this patch reads out of a live process.
//
//  Same problem as skia_abi.h. Skia is statically linked and stripped, so the
//  layout cannot come from a header at build time. These are the release
//  offsets:
//
//    sizeof(SkPathData)                   128
//    sizeof(SkPath)                        16
//    sizeof(std::optional<GeneratedPath>)  32
//
//  A debug build differs by enough to matter. SkMutex carries a
//  SkDEBUGCODE-only fOwner, which makes SkIDChangeListener::List 48 bytes
//  there against 40 here and shifts every field after it.
//
//  SkPathData is immutable and owns its point, conic and verb arrays as
//  trailing storage, so as long as the count does not change the points can
//  be rewritten in place with no allocation. Nothing else in the object may
//  be touched except fBounds, which caches the point bounds, and fConvexity,
//  which caches a value derived from them.
//
//  Where the count does change, the object is replaced instead. SkPathData
//  and its arrays come out of one ::operator new, so a replacement is that
//  allocation made again at the new size with everything but the geometry
//  copied over. The offsets below cover what that has to fix up.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_PATH_ABI_H_INCLUDED
#define CHROMIUM_PATH_ABI_H_INCLUDED

#include <cstddef>
#include <cstdint>

namespace path_abi {

// SkScalerContext::GeneratedPath, wrapped in std::optional. generatePath
// returns it through a hidden pointer.
constexpr size_t kGeneratedPath = 0;      // SkPath
constexpr size_t kGeneratedModified = 16; // bool
constexpr size_t kGeneratedEngaged = 24;  // bool
constexpr size_t kGeneratedSize = 32;

// include/core/SkPath.h
constexpr size_t kPathData = 0;      // sk_sp<SkPathData>
constexpr size_t kPathFillType = 8;  // SkPathFillType

// src/core/SkPathData.h. The reference count is SkNVRefCnt's sole member and
// so sits first; fGenIDChangeListeners fills 8 through 47.
constexpr size_t kDataRefCnt = 0;      // std::atomic<int32_t>
constexpr size_t kDataPoints = 48;     // SkSpan<SkPoint>, {SkPoint*, size_t}
constexpr size_t kDataConics = 64;     // SkSpan<float>
constexpr size_t kDataVerbs = 80;      // SkSpan<SkPathVerb>
constexpr size_t kDataBounds = 96;     // SkRect
constexpr size_t kDataUniqueID = 112;  // uint32_t
constexpr size_t kDataConvexity = 116; // std::atomic<uint8_t>
constexpr size_t kDataSegmentMask = 117;
constexpr size_t kDataType = 118;      // SkPathIsAType
constexpr size_t kDataSize = 128;

// A span is a pointer and a count, in that order.
constexpr size_t kSpanCount = 8;

// fGenIDChangeListeners, an SkIDChangeListener::List, fills 8 through 47. It
// is an SkMutex holding an SkSemaphore, then an STArray<1, sk_sp<...>> whose
// TArray points at the inline storage the STArray brought with it. That
// pointer is the one field of the object that refers to the object, so a copy
// has to be told about its own storage.
constexpr size_t kListInline = 24;   // the STArray's inline element
constexpr size_t kListData = 32;     // TArray::fData, which points at it
constexpr size_t kListSize = 40;     // TArray::fSize
constexpr size_t kListCapacity = 44; // fOwnMemory in bit 0, fCapacity above it

// SkPathVerb
enum Verb : uint8_t
{
    kMove = 0,
    kLine = 1,
    kQuad = 2,
    kConic = 3,
    kCubic = 4,
    kClose = 5,
};

// SkPathConvexity, the value that means it has not been computed.
constexpr uint8_t kConvexityUnknown = 4;

// SkPathSegmentMask
enum SegmentMask : uint8_t
{
    kLineMask = 1,
    kQuadMask = 2,
    kConicMask = 4,
    kCubicMask = 8,
};

// SkPathIsAType, the value that means the path is not a rect, oval or rrect.
// Only a general path may have its points rewritten; the shape ones cache
// geometry the points would no longer agree with.
constexpr uint8_t kIsAGeneral = 0;

struct Point
{
    float x = 0;
    float y = 0;
};

// How many points each verb consumes, from the table at the top of
// SkPathData.h. -1 is not a verb.
inline int PointsForVerb(const uint8_t verb)
{
    switch (verb) {
        case kMove:
        case kLine: return 1;
        case kQuad:
        case kConic: return 2;
        case kCubic: return 3;
        case kClose: return 0;
        default: return -1;
    }
}

// next_pathdata_unique_id, src/core/SkPathData.cpp: a counter with its top two
// bits cleared to leave room for the fill type, retried until it is non-zero.
// The header's comment says the low two bits, which the code does not do.
inline bool PlausibleUniqueID(const uint32_t id)
{
    return id != 0 && id >> 30 == 0;
}

}  // namespace path_abi

#endif  // CHROMIUM_PATH_ABI_H_INCLUDED
