// TerrainUnderlay.h
//
// The seam between the terrain renderer and the LIDAR scope.
//
// WHY AN INTERFACE RATHER THAN A DIRECT CALL. LidarView and TopoView are both pimpl'd, both own
// their own shared-memory connection, and neither has any business knowing the other's internals.
// The scope needs exactly one thing from the map - "draw your wireframe into my disc, at my
// range, at my heading" - and this is that one thing, written down. LidarView.cpp never includes
// TerrainShared.h, never learns what a tile is, and would keep compiling if the terrain producer
// were replaced wholesale.
//
// WHY NOT COPY THE RENDERER INTO LidarView. Because then there would be two of it, and the two
// would drift - which is precisely the failure this project already paid for with
// ExportControlBlock being duplicated across two files and held together by static_asserts. One
// renderer, two callers, one place to fix anything.
//
// THE CONTRACT, and every part of it matters:
//
//   * The render target is ALREADY OPEN. The caller has done BeginDraw and will do EndDraw. The
//     implementation must not call either, must not Clear, and must leave the transform as it
//     found it (identity).
//   * The caller has already established any clip it wants - the scope pushes a circular layer
//     so terrain cannot spill outside the disc.
//   * rangeCm is the world distance from centre to the edge of `rad`, matching the scope's own
//     RangeCm, so terrain and contacts land at the same place for the same world position.
//   * yawDeg rotates the map heading-up, in the same convention LidarShared publishes and
//     LidarView::DrawContact consumes:
//         fwd   =  dx*cos(yaw) + dy*sin(yaw)
//         right = -dx*sin(yaw) + dy*cos(yaw)
//     Pass the same value the scope passes its contacts, and the two layers agree by
//     construction rather than by coincidence.
//
// Returns the number of segments drawn, purely so the caller can report it.

#pragma once

#include <cstdint>

struct ID2D1RenderTarget;

class ITerrainUnderlay
{
  public:
    virtual ~ITerrainUnderlay() = default;

    virtual uint32_t DrawTerrainUnderlay(ID2D1RenderTarget *rt, float cx, float cy, float rad, float rangeCm,
                                         float yawDeg) = 0;
};
