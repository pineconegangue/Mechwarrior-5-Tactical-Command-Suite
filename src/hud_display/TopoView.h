// TopoView.h
//
// Host-facing surface of the topographic map page inside HUDDisplay.
//
// This is the CONSUMER half of the terrain pipeline. LidarExport.dll traces the ground with
// upward rays and publishes absolute world heights into the MW5_TerrainExport block (see
// include/TerrainShared.h); everything here only reads that block and draws it.
//
// PHASE B - deliberately the simplest thing that can be wrong. A flat, top-down, north-up
// wireframe with no camera, no depth buffer and no projection. The point is that if the world
// mapping, the tile origins, the sample spacing or the hole handling are off, you SEE it
// immediately as a smear, a seam or a spike, instead of chasing it through a 3D transform that
// could just as easily be the thing at fault. The 3D camera comes next, on top of a primitive
// that has already been shown to be correct.
//
// Pimpl, matching LidarView: Main.cpp needs neither the Direct2D details nor TerrainShared.h.

#pragma once

#include <cstdint>

#include "TerrainUnderlay.h"

struct ID2D1Factory;
struct ID2D1RenderTarget;
struct IDWriteFactory;

class TopoViewImpl;

// Inherits ITerrainUnderlay so the LIDAR scope can draw this same renderer inside its disc
// without either class knowing anything about the other - see TerrainUnderlay.h.
class TopoView : public ITerrainUnderlay
{
  public:
    TopoView();
    ~TopoView();

    TopoView(const TopoView &) = delete;
    TopoView &operator=(const TopoView &) = delete;

    // Factories are the host's, so there is one D2D/DWrite stack in the process.
    bool InitResources(ID2D1Factory *factory, IDWriteFactory *dw);

    // Pass nullptr before ResizeBuffers and the fresh target afterwards, exactly as with
    // LidarView. The device-bound brush is released and recreated to match; the cached
    // terrain geometry is NOT, because a path geometry belongs to the factory, not the
    // target, and survives a resize untouched.
    void SetRenderTarget(ID2D1RenderTarget *rt);

    // Fraction of the window the host reserves around the edge for bezel legends.
    void SetContentInset(float inset);

    // Cheap: opens MW5_TerrainExport if it is not open yet, at most once a second. Called
    // every frame regardless of the active view so switching to this page is instant, and
    // so the page is already connected when a mission starts while another view is up.
    void Poll();

    // Draws the map into the current render target. Does its own BeginDraw/EndDraw, which
    // on a DXGI surface target does not present - same contract as LidarView::Render.
    void Render();

    void Shutdown();
    bool IsConnected() const;

    // ---- zoom -------------------------------------------------------------------------
    // Span across the SHORTER screen axis, in metres, so the readout means the same thing
    // whatever shape the MFD window is. Stepped a whole tile at a time so the 512 m tile grid
    // never drifts out of phase with the zoom and stays usable as a ruler.
    //
    // The host drives this from the SAME bezel buttons that zoom the LIDAR scope and the screen
    // capture region: one zoom control that acts on whichever page is up, rather than a
    // different pair of buttons per view.
    static constexpr float kMinSpanM = 1024.0f;
    // Raised from 4096: a field map came back 12 x 13 tiles, which is 6.1 x 6.7 km, so the one
    // view capable of framing a whole map could not frame one.
    static constexpr float kMaxSpanM = 16384.0f;
    static constexpr float kSpanStepM = 512.0f;

    float GetSpanM() const;
    void SetSpanM(float metres); // clamped; used to restore the value from the config
    void AdjustSpan(bool in);

    // ---- FIT: frame the whole discovered map ------------------------------------------
    //
    // One step beyond the widest manual span, reached by zooming out past it, so it needs no
    // extra button and is found by doing the obvious thing. Zooming back in leaves it.
    //
    // It frames the extent the producer actually FOUND - MinTile/MaxTile in the header, which
    // is the bounding box of tiles that came back with ground - not a fixed maximum. A small
    // map fits small. While the producer is still exploring (the first few seconds of a
    // mission) that box is still growing, so the view widens as ground is discovered and then
    // settles; the readout says EXPLORING while that is happening.
    //
    // FIT also centres on the MAP rather than the mech, which is the only way "frame the whole
    // map" can mean anything - a player-centred fit would need twice the span to reach the far
    // corner and would waste most of the screen on empty ground.
    //
    // Persisted as a span of 0 in the config rather than a second key.
    static constexpr float kFitSpanSentinel = 0.0f;
    void SetFitMap(bool on);
    bool GetFitMap() const;

    // ---- orientation ------------------------------------------------------------------
    // HEADING-UP by default, because LidarView projects contacts heading-up and this page is
    // meant to end up underneath it. North-up is available for anyone who would rather have a
    // conventional map; it is the same projection with yaw pinned to zero, not a second path.
    void SetNorthUp(bool on);
    bool GetNorthUp() const;

    // ---- mesh resolution --------------------------------------------------------------
    // A FIXED world cell size, not one fitted to the screen. A grid that re-tessellates as you
    // zoom is visibly unstable and dirties every baked tile on the way; a constant one is a
    // graticule that means the same thing at every zoom, and zooming then costs nothing at all.
    // Snapped to a whole multiple of the 16 m data spacing.
    static constexpr int kDefaultCellM = 64; // 8 x 8 cells per 512 m tile, 128 segments
    void SetCellM(int metres);
    int GetCellM() const;

    // ITerrainUnderlay. Draws into an already-open target with no chrome of its own; the
    // contract is documented in TerrainUnderlay.h.
    uint32_t DrawTerrainUnderlay(ID2D1RenderTarget *rt, float cx, float cy, float rad, float rangeCm,
                                 float yawDeg) override;

    // Snapshot for the host's log.
    //
    // The v1 version of this existed to answer one question (could the tile cache grow?) and
    // was logged only when the page was selected or zoomed - which is to say never at the
    // moment the number mattered. The answer came from the host's own frame timer instead.
    // This one is sampled periodically while the page is up, and reports what the page is
    // actually doing rather than only what it was asked to do.
    struct Diag
    {
        bool Connected;
        uint32_t Generation;

        // Producer side: the whole-map height cache.
        uint32_t TilesComplete; // tiles traced and holding ground
        uint32_t TilesClaimed;  // slots in use, including ones still filling
        uint32_t TilesOffMap;   // candidates rejected by the five-point ground probe
        bool Exploring;         // still looking for new ground
        int ExtentTilesX, ExtentTilesY;

        // Consumer side: the geometry cache, which follows the view rather than the map.
        int Baked;              // tiles with usable geometry right now
        int DirtyQueued;        // tiles waiting on the per-frame bake budget
        int DrawnTiles;         // tiles that survived view culling last frame
        uint32_t DrawnSegments; // segments actually submitted last frame
        float LastBakeMs;
        float SpanM;
        int CellM; // 16, 32 or 64 - the LOD stride in metres

        float MinElevM, MaxElevM;
        bool FitMap; // FIT is not just requested but actually framing an extent this frame
        bool NorthUp;
        float HeadingDeg;
    };
    Diag GetDiag() const;

  private:
    TopoViewImpl *P = nullptr;
};
