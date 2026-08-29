//+--------------------------------------------------------------------------
//
//  geometry_sink.h - ID2D1SimplifiedGeometrySink, reconstructed.
//
//  IDWriteFontFace::GetGlyphRunOutline takes one of these, but include/dwrite.h
//  only forward declares it (the SDK puts it in d2d1.h, which is not part of
//  the DirectWrite header set this repository mirrors). The declaration below
//  is the documented interface, in its documented method order, and is only
//  ever implemented here, never consumed from DirectWrite.
//
//----------------------------------------------------------------------------

#ifndef CHROMIUM_GEOMETRY_SINK_H_INCLUDED
#define CHROMIUM_GEOMETRY_SINK_H_INCLUDED

#include "compat.h"
#include "dwrite.h"

typedef enum D2D1_FILL_MODE
{
    D2D1_FILL_MODE_ALTERNATE = 0,
    D2D1_FILL_MODE_WINDING = 1,
} D2D1_FILL_MODE;

typedef enum D2D1_PATH_SEGMENT
{
    D2D1_PATH_SEGMENT_NONE = 0,
    D2D1_PATH_SEGMENT_FORCE_UNSTROKED = 1,
    D2D1_PATH_SEGMENT_FORCE_ROUND_LINE_JOIN = 2,
} D2D1_PATH_SEGMENT;

typedef enum D2D1_FIGURE_BEGIN
{
    D2D1_FIGURE_BEGIN_FILLED = 0,
    D2D1_FIGURE_BEGIN_HOLLOW = 1,
} D2D1_FIGURE_BEGIN;

typedef enum D2D1_FIGURE_END
{
    D2D1_FIGURE_END_OPEN = 0,
    D2D1_FIGURE_END_CLOSED = 1,
} D2D1_FIGURE_END;

typedef struct D2D1_BEZIER_SEGMENT
{
    D2D1_POINT_2F point1;
    D2D1_POINT_2F point2;
    D2D1_POINT_2F point3;
} D2D1_BEZIER_SEGMENT;

interface ID2D1SimplifiedGeometrySink : IUnknown
{
    STDMETHOD_(void, SetFillMode)(D2D1_FILL_MODE fillMode) PURE;
    STDMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT vertexFlags) PURE;
    STDMETHOD_(void, BeginFigure)(D2D1_POINT_2F startPoint, D2D1_FIGURE_BEGIN figureBegin) PURE;
    STDMETHOD_(void, AddLines)(_In_reads_(pointsCount) const D2D1_POINT_2F* points,
                               UINT32 pointsCount) PURE;
    STDMETHOD_(void, AddBeziers)(_In_reads_(beziersCount) const D2D1_BEZIER_SEGMENT* beziers,
                                 UINT32 beziersCount) PURE;
    STDMETHOD_(void, EndFigure)(D2D1_FIGURE_END figureEnd) PURE;
    STDMETHOD(Close)() PURE;
};

#endif  // CHROMIUM_GEOMETRY_SINK_H_INCLUDED
