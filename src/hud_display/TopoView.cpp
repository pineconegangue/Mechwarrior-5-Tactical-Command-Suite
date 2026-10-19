// TopoView.cpp
//
// The topographic map page: reads MW5_TerrainExport and draws it as a flat, north-up, green
// wireframe centred on the mech.
//
// =============================================================================================
// THE TWO CACHES, AND WHY THEY HAVE DIFFERENT HORIZONS
//
// Heights are expensive to acquire and cheap to keep: every sample is a physics trace against
// the game world, but a whole mission map is about a megabyte. Drawing geometry is the exact
// opposite - it rebuilds from heights in a couple of milliseconds per tile, but holding it for
// a whole map would be tens of megabytes and drawing it would be hopeless.
//
// So the producer caches HEIGHTS for the entire map and never re-traces, and this file caches
// GEOMETRY only for the tiles currently on screen, rebuilding freely as you fly. That division
// is what makes a whole-map cache affordable; without it, "cache the whole map" and "draw the
// whole map" collapse into the same thing and neither is possible.
//
// =============================================================================================
// CELL SIZE - fixed in the world, not fitted to the screen
//
// The first version baked every tile at the full 16 m data resolution and drew all of it, every
// frame: about 50,000 line segments. In the field that page ran at 36-44 fps while a second
// window on another page reported 51-57 at the same moments. At the default 2560 m span a 16 m
// cell is 3.75 PIXELS, so those fifty thousand segments were drawing something that cannot
// resolve as a wireframe at all - it reads as a solid haze.
//
// The first fix chose the stride from pixels-per-cell. That fixed the cost but got the idea
// wrong: a grid whose spacing changes when you zoom is not a grid, it is a rendering artefact
// the pilot can see. The mesh visibly re-tessellates mid-zoom, and every change dirtied every
// baked tile, so zooming cost a full rebake on top of looking unstable.
//
// So the cell is a FIXED WORLD SIZE - 64 m by default. It is a graticule, like the 512 m tile
// lines, and it means the same thing at every zoom: eight cells to a tile, always. Zooming now
// changes nothing about the geometry and triggers no rebake at all, which is both cheaper and
// steadier than the adaptive scheme it replaces.
//
//     64 m cell (default)    128 segments/tile   8 x 8 cells per tile
//     32 m cell              512 segments/tile  16 x 16
//     16 m cell (raw data)  2048 segments/tile  32 x 32
//
// =============================================================================================
// INCREMENTAL, BUDGETED BAKING
//
// Tiles are baked one at a time into their own geometries, and marked dirty when anything they
// depend on changes: the tile arriving, a neighbour arriving (seams - below), the elevation
// range widening, or the LOD stride changing. The dirty queue is drained at a fixed few tiles
// per frame, so no single frame can carry an unbounded rebuild. v1 rebuilt EVERYTHING whenever
// anything changed, which is exactly why the field log showed 33-47 ms spikes clustered on tile
// completions.
//
// SEAMS. A tile owns the segments joining its own samples PLUS the ones reaching east and north
// into its neighbours, so the lattice is continuous across tile boundaries. The consequence is
// that when a tile arrives, its WEST and SOUTH neighbours must be rebaked too, because they own
// the seams pointing into it. Two extra bakes per tile, and far simpler than owning seams
// jointly.
//
// HOLES ARE NOT ZERO. A sample whose trace found nothing is TERR_NO_HEIGHT (-1.0e9). Any segment
// touching one is skipped, never clamped: boundary tiles are legitimately half empty, and one
// unguarded segment would stretch ten thousand kilometres below the map and drag the elevation
// range with it, flattening every real band.
//
// =============================================================================================
// ORIENTATION - heading-up, not north-up
//
// The first version of this page was north-up, and that was fine for a standalone map. It is
// wrong for the thing this page is actually for. LidarView projects every contact HEADING-UP:
//
//     fwd   =  dx*cs + dy*sn        // LidarView.cpp, DrawContact
//     right = -dx*sn + dy*cs
//     sx = cx + (right/Range)*radX
//     sy = cy - (fwd  /Range)*radY
//
// Terrain drawn north-up under a scope drawn heading-up is off by the mech's entire heading, and
// it would still look like a perfectly good map while being wrong - the failure mode with no
// symptom. So the projection below is that formula, character for character, with the span
// standing in for the scope range. The two layers now agree by construction rather than by
// coincidence, which is the only reason the eventual merge is a small change.
//
// Rotating costs nothing, and that is not luck: geometry is baked in WORLD space and placed by a
// matrix, so heading is one more term in the matrix and never touches a baked tile.
// SetNorthUp(true) is kept for anyone who wants a conventional map instead.
//
// =============================================================================================
// READING SHARED MEMORY
//
// The header and slot table are copied under a seqlock every frame - about 8 KB. Heights are
// NOT copied; they are read in place, only at bake time, and the slot is re-checked afterwards.
// If it moved (eviction is the only thing that can do this) the bake is discarded and retried.
// Copying two megabytes a frame to read a handful of tiles would be absurd.

#include "TopoView.h"

#include "TerrainUnderlay.h"

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <cmath>
#include <cstdio>

#include "TerrainShared.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace
{

// <windows.h> is included without NOMINMAX in this project, so min/max are live macros and
// std::min / std::max / std::clamp do not compile here. Hand-rolled, as everywhere else.
inline float FMin(float a, float b) { return a < b ? a : b; }
inline float FMax(float a, float b) { return a > b ? a : b; }

template <typename T> void SafeRelease(T *&p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

// TERR_NO_HEIGHT is -1.0e9; anything below -1.0e8 is a hole by any reading, and no real world Z
// comes close. A threshold rather than an equality, so a float that went through a copy cannot
// defeat it. The negation makes NaN count as a hole too.
inline bool IsHole(float z) { return !(z > -1.0e8f); }

// Integer floor-division. C++ truncates toward zero, so -1 / 32 is 0 and every tile west or
// south of the origin would alias onto tile 0.
inline int32_t FloorDiv(int32_t a, int32_t b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); }

// Tron palette. One colour at six intensities, not a heat map.
const D2D1_COLOR_F kBackground = {0.008f, 0.020f, 0.012f, 1.0f};
const D2D1_COLOR_F kBandLow = {0.04f, 0.26f, 0.11f, 1.0f};
const D2D1_COLOR_F kBandHigh = {0.42f, 1.00f, 0.52f, 1.0f};
const D2D1_COLOR_F kTileLine = {0.10f, 0.45f, 0.60f, 1.0f}; // cyan, so tile seams read as chrome
const D2D1_COLOR_F kPlayer = {1.00f, 0.72f, 0.15f, 1.0f};
const D2D1_COLOR_F kText = {0.35f, 0.95f, 0.48f, 1.0f};
const D2D1_COLOR_F kWarn = {1.00f, 0.45f, 0.30f, 1.0f};

double NowMs()
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

} // namespace

// ===========================================================================================

class TopoViewImpl
{
  public:
    static constexpr int kBands = 6;

    // Geometry cache size. Comfortably more than any span can show (4096 m covers about 64
    // tiles), with room to keep tiles just off screen so turning around costs nothing.
    // Raised from 96 when FIT arrived: a 12 x 13 tile map is 156 tiles, and a cache that cannot
    // hold what is on screen evicts tiles it is about to need again. At the 64 m cells the flat
    // page uses these are ~128 segments each, so this is small.
    // bug sweep 14 (2026-10-18): FIT / a 16 km span can ask for every tile the producer has (TERR_MAX_TILES, 512); at 256
    // the northern part of a big explored map was never baked. Now the producer's whole map fits.
    static constexpr int kMaxBaked = TERR_MAX_TILES;

    // Per-frame bake budget. Two tiles is about 4 ms worst case at stride 1 - fits inside a
    // frame without being felt, and a full screen refills in well under a second.
    static constexpr int kBakesPerFrame = 2;

    // Elevation banding is quantised to this, in metres, so the range settles after the first
    // few tiles instead of nudging - and every nudge dirties every baked tile.
    static constexpr float kRangeQuantM = 50.0f;

    bool InitResources(ID2D1Factory *factory, IDWriteFactory *dw)
    {
        Factory = factory;
        DW = dw;
        if (!Factory || !DW)
            return false;
        DW->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &TextFmt);
        return true;
    }

    void SetRenderTarget(ID2D1RenderTarget *rt)
    {
        if (RT == rt)
            return;
        SafeRelease(Brush);
        RT = rt;
        if (RT)
            RT->CreateSolidColorBrush(kBackground, &Brush);
        // Geometry is factory-owned and survives the target swap, so it is NOT dropped here.
    }

    void SetContentInset(float inset) { Inset = (inset >= 0.0f && inset < 0.45f) ? inset : 0.10f; }

    void SetNorthUp(bool on) { NorthUp = on; }
    bool GetNorthUp() const { return NorthUp; }

    float GetSpanM() const { return SpanM; }
    void SetSpanM(float m) { SpanM = (m < kMinSpanM) ? kMinSpanM : (m > kMaxSpanM ? kMaxSpanM : m); }

    // One tile per step, so the span always lands on a whole number of 512 m tiles and the cyan
    // tile grid stays a usable ruler rather than drifting out of phase with the zoom.
    // Zooming OUT past the widest manual span lands on FIT; zooming IN from FIT leaves it at
    // that widest span. One extra detent on a control that already exists.
    void AdjustSpan(bool in)
    {
        if (FitMap)
        {
            if (in) { FitMap = false; SpanM = kMaxSpanM; }
            return;
        }
        if (!in && SpanM >= kMaxSpanM - 0.5f) { FitMap = true; return; }
        SetSpanM(SpanM + (in ? -kSpanStepM : kSpanStepM));
    }

    void SetFitMap(bool on) { FitMap = on; }
    bool GetFitMap() const { return FitMap; }

    // The frame FIT should show: the centre and span of the ground the producer has found.
    // Returns false when there is no usable extent yet, in which case the caller stays on the
    // manual span rather than guessing.
    bool FitFrame(float &cxM, float &cyM, float &spanM) const
    {
        if (Head.Complete == 0)
            return false;
        const float tileM = TERR_TILE_CM * 0.01f;
        const float x0 = (float)Head.MinTileX * tileM, x1 = (float)(Head.MaxTileX + 1) * tileM;
        const float y0 = (float)Head.MinTileY * tileM, y1 = (float)(Head.MaxTileY + 1) * tileM;
        const float w = x1 - x0, h = y1 - y0;
        if (!(w > 1.0f) || !(h > 1.0f))
            return false;
        cxM = (x0 + x1) * 0.5f;
        cyM = (y0 + y1) * 0.5f;
        // The extent is a BOUNDING BOX, so an L-shaped map frames its empty corner too. Live
        // with it: the alternative is a per-tile hull, and 8% of margin matters more.
        spanM = FMax(w, h) * 1.08f;
        if (spanM < kMinSpanM) spanM = kMinSpanM;
        if (spanM > kMaxSpanM) spanM = kMaxSpanM;
        return true;
    }

    bool IsConnected() const { return Shared != nullptr; }

    void Poll()
    {
        if (Shared)
            return;
        const double now = NowMs();
        if (now - LastAttemptMs < 1000.0)
            return;
        LastAttemptMs = now;

        Map = OpenFileMappingW(FILE_MAP_READ, FALSE, TERRAIN_MMF_NAME);
        if (!Map)
            return;
        Shared = static_cast<const TerrainBlock *>(MapViewOfFile(Map, FILE_MAP_READ, 0, 0, sizeof(TerrainBlock)));
        if (!Shared)
        {
            CloseHandle(Map);
            Map = nullptr;
        }
    }

    void Render()
    {
        if (!RT || !Brush)
            return; // host tore the target down for a resize

        RT->BeginDraw();
        RT->Clear(kBackground);

        const D2D1_SIZE_F sz = RT->GetSize();

        if (!Shared)
        {
            DrawBanner(sz, L"TOPO  -  waiting for MW5_TerrainExport",
                       L"LidarExport.dll is not running, or the game is not up.", kWarn);
            EndDraw();
            return;
        }

        if (!ReadHeader() && !HeadValid)
        {
            DrawBanner(sz, L"TOPO  -  reading...", L"", kText);
            EndDraw();
            return;
        }

        if (Head.Version != TERRAIN_VERSION || Head.SamplesPerSide != TERR_SAMPLES)
        {
            wchar_t d[224];
            swprintf_s(d,
                       L"block reports version %u / %d samples; this build expects %u / %d. "
                       L"Rebuild LidarExport.dll and HUDDisplay.exe together.",
                       Head.Version, (int)Head.SamplesPerSide, (unsigned)TERRAIN_VERSION, (int)TERR_SAMPLES);
            DrawBanner(sz, L"TOPO  -  contract mismatch", d, kWarn);
            EndDraw();
            return;
        }

        // The banners above are the page's own reporting; Prepare re-runs the same checks so
        // the scope gets the generation handling too, and is the single place either caller
        // decides whether there is anything to draw.
        if (!Prepare())
        {
            DrawBanner(sz, L"TOPO  -  reading...", L"", kText);
            EndDraw();
            return;
        }

        // Resolve the frame ONCE, here, so the tiles that get baked and the tiles that get drawn
        // can never be computed from different numbers.
        float focusX = Head.PlayerX * 0.01f, focusY = Head.PlayerY * 0.01f, span = SpanM;
        FitActive = false;
        if (FitMap)
            FitActive = FitFrame(focusX, focusY, span);

        // Half-diagonal: at a 45 degree heading the corners of a rectangular view reach sqrt(2)
        // further than its edges, and a half-span radius would leave them empty.
        ServiceBakes(span * 0.7072f, focusX, focusY);

        if (Head.Complete == 0)
        {
            DrawBanner(sz, L"TOPO  -  no terrain",
                       L"Not in a mission, or the producer has not completed a tile yet.", kText);
            EndDraw();
            return;
        }

        DrawMap(sz, focusX, focusY, span);
        EndDraw();
    }

    // ---- ITerrainUnderlay ------------------------------------------------------------------
    //
    // The scope's version of a frame. Same preparation, same renderer, no chrome: no BeginDraw,
    // no Clear, no tile grid, no player marker and no readout, because the scope draws its own
    // centre mark and rings and owns the background. See TerrainUnderlay.h for the contract.
    uint32_t DrawTerrainUnderlay(ID2D1RenderTarget *rt, float cx, float cy, float rad, float rangeCm, float yawDeg)
    {
        if (!rt || !Shared || !Factory)
            return 0;
        // The host hands both views the same ID2D1DeviceContext, so this is the same target the
        // page draws into and Brush is valid for it. Adopt it if the page has not been shown yet
        // and we have no target of our own, and refuse anything else rather than draw with a
        // brush built on a different device.
        if (!RT)
            SetRenderTarget(rt);
        if (rt != RT || !Brush)
            return 0;

        if (!Prepare())
            return 0;

        const float rangeM = rangeCm * 0.01f;
        if (!(rangeM > 1.0f) || !(rad > 1.0f))
            return 0;

        const float s = rad / rangeM;
        // Radius, not half-diagonal: the scope's visible region is a disc. Always centred on the
        // mech - FIT is a property of the full-page map, not of the scope's underlay.
        ServiceBakes(rangeM, Head.PlayerX * 0.01f, Head.PlayerY * 0.01f);

        if (Head.Complete == 0)
            return 0;

        // Clip to the disc. The scope is round, so an axis-aligned clip would let the wireframe
        // square off the corners of a circular instrument - the one place this would look
        // obviously wrong rather than subtly so.
        //
        // Cached on its geometry: this is called every frame, and a COM allocation per frame for
        // a circle that only changes when the window is resized is the sort of small waste that
        // is invisible until it is not.
        if (!Disc || Disc_cx != cx || Disc_cy != cy || Disc_r != rad)
        {
            SafeRelease(Disc);
            if (FAILED(Factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), &Disc)) || !Disc)
                return 0;
            Disc_cx = cx;
            Disc_cy = cy;
            Disc_r = rad;
        }

        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
        lp.geometricMask = Disc;
        lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        rt->PushLayer(lp, nullptr);

        const float yawRad = yawDeg * 0.01745329252f;
        const D2D1_RECT_F cull = D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad);
        const uint32_t segs = DrawTerrainCore(cx, cy, s, yawRad, cull, false, false, Head.PlayerX * 0.01f,
                                              Head.PlayerY * 0.01f);

        rt->PopLayer();
        return segs;
    }

  private:
    // Everything a frame needs before anything can be drawn, shared by both callers. Returns
    // false when there is nothing worth drawing; the page turns that into a banner, the scope
    // just draws no terrain.
    bool Prepare()
    {
        if (!Shared)
            return false;
        if (!ReadHeader() && !HeadValid)
            return false;
        if (Head.Version != TERRAIN_VERSION || Head.SamplesPerSide != TERR_SAMPLES)
            return false;
        if (Head.Generation != BakedGeneration)
        {
            DropAllBaked();
            BakedGeneration = Head.Generation;
            HaveRange = false;
            RangeMinM = RangeMaxM = 0.0f;
        }
        return true;
    }

  public:

    void FillDiag(TopoView::Diag &d) const
    {
        d.Connected = (Shared != nullptr);
        d.Generation = Head.Generation;
        d.TilesComplete = Head.Complete;
        d.TilesClaimed = Head.Claimed;
        d.TilesOffMap = Head.OffMap;
        d.Exploring = (Head.Exploring != 0);
        d.Baked = BakedCount();
        d.DirtyQueued = DirtyCount();
        d.DrawnTiles = LastDrawnTiles;
        d.DrawnSegments = LastDrawnSegments;
        d.LastBakeMs = LastBakeMs;
        d.SpanM = SpanM;
        d.CellM = (int)(TERR_SPACING_CM * 0.01f) * Stride;
        d.MinElevM = RangeMinM;
        d.MaxElevM = RangeMaxM;
        d.NorthUp = NorthUp;
        d.FitMap = FitActive;
        d.HeadingDeg = Head.PlayerYaw;
        d.ExtentTilesX = Head.MaxTileX - Head.MinTileX + 1;
        d.ExtentTilesY = Head.MaxTileY - Head.MinTileY + 1;
    }

    void Shutdown()
    {
        DropAllBaked();
        SafeRelease(Disc);
        SafeRelease(Brush);
        SafeRelease(TextFmt);
        if (Shared)
        {
            UnmapViewOfFile(Shared);
            Shared = nullptr;
        }
        if (Map)
        {
            CloseHandle(Map);
            Map = nullptr;
        }
    }

  private:
    // ---- baked tiles ---------------------------------------------------------------------

    struct Baked
    {
        bool Used = false;
        bool Dirty = false;
        int32_t TileX = 0, TileY = 0;
        int Stride = 0;                        // the LOD this was baked at
        ID2D1PathGeometry *Bands[kBands] = {}; // null where a band got no segments
        uint32_t Segments = 0;
        double LastSeenMs = 0.0; // drives geometry eviction, and bake priority
    };

    Baked Cache[kMaxBaked];

    int BakedCount() const
    {
        int n = 0;
        for (int i = 0; i < kMaxBaked; ++i)
            if (Cache[i].Used && !Cache[i].Dirty)
                ++n;
        return n;
    }

    int DirtyCount() const
    {
        int n = 0;
        for (int i = 0; i < kMaxBaked; ++i)
            if (Cache[i].Used && Cache[i].Dirty)
                ++n;
        return n;
    }

    void DropBakedGeometry(Baked &b)
    {
        for (int i = 0; i < kBands; ++i)
            SafeRelease(b.Bands[i]);
        b.Segments = 0;
    }

    void DropAllBaked()
    {
        for (int i = 0; i < kMaxBaked; ++i)
        {
            DropBakedGeometry(Cache[i]);
            Cache[i] = Baked();
        }
    }

    int FindBaked(int32_t tx, int32_t ty) const
    {
        for (int i = 0; i < kMaxBaked; ++i)
            if (Cache[i].Used && Cache[i].TileX == tx && Cache[i].TileY == ty)
                return i;
        return -1;
    }

    void MarkDirty(int32_t tx, int32_t ty)
    {
        const int i = FindBaked(tx, ty);
        if (i >= 0)
            Cache[i].Dirty = true;
    }

    void MarkAllDirty()
    {
        for (int i = 0; i < kMaxBaked; ++i)
            if (Cache[i].Used)
                Cache[i].Dirty = true;
    }

    // A free entry, or the least-recently-drawn one. Evicting GEOMETRY is cheap and safe: the
    // heights are still in the producer's cache, so coming back costs a rebake and no traces at
    // all. That asymmetry is the whole point of keeping the two caches on different horizons.
    int AcquireSlot(double now)
    {
        int lru = -1;
        double lruT = 0.0;
        for (int i = 0; i < kMaxBaked; ++i)
        {
            if (!Cache[i].Used)
                return i;
            if (lru < 0 || Cache[i].LastSeenMs < lruT)
            {
                lru = i;
                lruT = Cache[i].LastSeenMs;
            }
        }
        if (lru >= 0 && now - Cache[lru].LastSeenMs > 250.0) // never evict something on screen
        {
            DropBakedGeometry(Cache[lru]);
            Cache[lru] = Baked();
            return lru;
        }
        return -1;
    }

    // ---- shared-memory reads ---------------------------------------------------------------

    // Header seqlock. The producer bumps FrameCounter BEFORE touching the header or slot table
    // and FrameCounterEnd after, so sampling End first and Counter last gives equal values only
    // if no write straddled the copy. Reads through Scratch and commits on success, so a torn
    // read cannot destroy the last good header.
    bool ReadHeader()
    {
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const uint32_t before = *(volatile const uint32_t *)&Shared->H.FrameCounterEnd;
            memcpy(&Scratch, &Shared->H, sizeof(TerrainHeader));
            MemoryBarrier();
            const uint32_t after = *(volatile const uint32_t *)&Shared->H.FrameCounter;
            if (before == after)
            {
                memcpy(&Head, &Scratch, sizeof(TerrainHeader));
                HeadValid = true;
                return true;
            }
        }
        return false;
    }

    uint32_t SlotCount() const
    {
        return (Head.TileCount < (uint32_t)TERR_MAX_TILES) ? Head.TileCount : (uint32_t)TERR_MAX_TILES;
    }

    int SlotForTile(int32_t tx, int32_t ty) const
    {
        const uint32_t n = SlotCount();
        for (uint32_t i = 0; i < n; ++i)
        {
            const TerrainSlotInfo &s = Head.Slots[i];
            if (s.State == TTS_Complete && s.Hits > 0 && s.TileX == tx && s.TileY == ty)
                return (int)i;
        }
        return -1;
    }

    // ---- LOD --------------------------------------------------------------------------------

  public:
    // Cell size in metres, snapped to a whole multiple of the 16 m data spacing. Constant across
    // zoom levels and shared by both callers, so the page and the scope draw the same mesh and
    // switching between them costs nothing.
    void SetCellM(int metres)
    {
        const int raw = (int)(TERR_SPACING_CM * 0.01f); // 16
        int stride = metres / raw;
        if (stride < 1) stride = 1;
        if (stride > 8) stride = 8; // 128 m; beyond that a 512 m tile is only 4 cells across
        if (stride != Stride)
        {
            Stride = stride;
            MarkAllDirty(); // every baked tile was built at the old cell size
        }
    }

    int GetCellM() const { return (int)(TERR_SPACING_CM * 0.01f) * Stride; }

  private:

    // ---- baking -----------------------------------------------------------------------------

    void ServiceBakes(float radiusM, float centreXm, float centreYm)
    {
        const double now = NowMs();
        SyncCacheWithProducer(now, radiusM, centreXm, centreYm);

        for (int done = 0; done < kBakesPerFrame; ++done)
        {
            int pick = -1;
            double pickSeen = 0.0;
            for (int i = 0; i < kMaxBaked; ++i)
            {
                if (!Cache[i].Used || !Cache[i].Dirty)
                    continue;
                // LastSeenMs is refreshed for visible tiles every frame, so "most recently seen"
                // is a good proxy for "most wanted right now".
                if (pick < 0 || Cache[i].LastSeenMs > pickSeen)
                {
                    pick = i;
                    pickSeen = Cache[i].LastSeenMs;
                }
            }
            if (pick < 0)
                break;

            const double t0 = NowMs();
            BakeTile(Cache[pick]);
            LastBakeMs = (float)(NowMs() - t0);
        }
    }

    // Adopt every complete tile a generous view rectangle touches, and keep the elevation range
    // current. The margin means tiles are already baked before they scroll on screen.
    void SyncCacheWithProducer(double now, float radiusM, float centreXm, float centreYm)
    {
        // radiusM is the world distance the caller can see from the centre, already including
        // whatever diagonal allowance it needs. One tile of prefetch beyond it so tiles are
        // baked before they scroll into view.
        const float tileM = TERR_TILE_CM * 0.01f;
        const float halfM = radiusM + tileM;
        const int32_t tx0 = (int32_t)std::floor((centreXm - halfM) / tileM);
        const int32_t tx1 = (int32_t)std::floor((centreXm + halfM) / tileM);
        const int32_t ty0 = (int32_t)std::floor((centreYm - halfM) / tileM);
        const int32_t ty1 = (int32_t)std::floor((centreYm + halfM) / tileM);

        for (int32_t ty = ty0; ty <= ty1; ++ty)
            for (int32_t tx = tx0; tx <= tx1; ++tx)
            {
                if (SlotForTile(tx, ty) < 0)
                    continue;
                if (FindBaked(tx, ty) >= 0)
                    continue;

                const int b = AcquireSlot(now);
                if (b < 0)
                    continue; // cache full of on-screen tiles; it will come next frame

                Cache[b].Used = true;
                Cache[b].Dirty = true;
                Cache[b].TileX = tx;
                Cache[b].TileY = ty;
                Cache[b].Stride = 0;
                Cache[b].LastSeenMs = now;

                // Seams reach east and north, so the tiles owning the seams INTO this one are
                // its west and south neighbours; they have to be rebuilt as well.
                MarkDirty(tx - 1, ty);
                MarkDirty(tx, ty - 1);
            }

        // Widen-only elevation range over everything the producer has finished - not just what
        // is baked - so banding does not shift as you fly into new ground. Sampled sparsely
        // (every 37th height, a stride coprime with the row length so it walks the whole tile)
        // because this runs every frame and the exact extremes do not matter to a 50 m band.
        float lo = 1.0e30f, hi = -1.0e30f;
        const uint32_t n = SlotCount();
        for (uint32_t i = 0; i < n; ++i)
        {
            const TerrainSlotInfo &s = Head.Slots[i];
            if (s.State != TTS_Complete || s.Hits == 0)
                continue;
            for (int k = 0; k < TERR_SAMPLES * TERR_SAMPLES; k += 37)
            {
                const float z = Shared->Tiles[i].Height[k];
                if (IsHole(z))
                    continue;
                lo = FMin(lo, z);
                hi = FMax(hi, z);
            }
        }
        if (lo > hi)
            return;

        const float loM = std::floor(lo * 0.01f / kRangeQuantM) * kRangeQuantM;
        const float hiM = std::ceil(hi * 0.01f / kRangeQuantM) * kRangeQuantM;
        bool moved = false;
        if (!HaveRange)
        {
            RangeMinM = loM;
            RangeMaxM = hiM;
            HaveRange = true;
            moved = true;
        }
        else
        {
            if (loM < RangeMinM)
            {
                RangeMinM = loM;
                moved = true;
            }
            if (hiM > RangeMaxM)
            {
                RangeMaxM = hiM;
                moved = true;
            }
        }
        if (moved)
            MarkAllDirty();
    }

    // Read a height from the shared block by global lattice index, across tile boundaries.
    // gx = TileX * TERR_SAMPLES + col.
    bool SampleAt(int32_t gx, int32_t gy, float &outZcm) const
    {
        const int32_t tx = FloorDiv(gx, TERR_SAMPLES);
        const int32_t ty = FloorDiv(gy, TERR_SAMPLES);
        const int slot = SlotForTile(tx, ty);
        if (slot < 0)
            return false;
        const int32_t col = gx - tx * TERR_SAMPLES;
        const int32_t row = gy - ty * TERR_SAMPLES;
        const float z = Shared->Tiles[slot].Height[row * TERR_SAMPLES + col];
        if (IsHole(z))
            return false;
        outZcm = z;
        return true;
    }

    void BakeTile(Baked &b)
    {
        DropBakedGeometry(b);
        b.Stride = Stride;
        b.Dirty = false;

        const int slot = SlotForTile(b.TileX, b.TileY);
        if (slot < 0)
        {
            // The tile went away (eviction, or a generation change we have not processed).
            // Retire the entry rather than leaving an empty one loitering in the cache.
            b = Baked();
            return;
        }

        // Snapshot the slot identity, for the post-read check at the bottom.
        const int32_t wasX = Head.Slots[slot].TileX, wasY = Head.Slots[slot].TileY;

        const float spanZ = (RangeMaxM - RangeMinM) > 1.0f ? (RangeMaxM - RangeMinM) : 1.0f;
        const float spacingM = TERR_SPACING_CM * 0.01f;
        const int st = b.Stride;
        const int32_t gx0 = b.TileX * TERR_SAMPLES, gy0 = b.TileY * TERR_SAMPLES;

        ID2D1GeometrySink *sink[kBands] = {};
        for (int i = 0; i < kBands; ++i)
        {
            if (FAILED(Factory->CreatePathGeometry(&b.Bands[i])) || !b.Bands[i] ||
                FAILED(b.Bands[i]->Open(&sink[i])))
            {
                for (int k = 0; k < i; ++k)
                {
                    sink[k]->Close();
                    sink[k]->Release();
                }
                DropBakedGeometry(b);
                b.Dirty = true; // retry next frame rather than leave a permanent hole
                return;
            }
        }

        uint32_t segs = 0;
        auto emit = [&](int32_t ax, int32_t ay, float azCm, int32_t bx, int32_t by, float bzCm) {
            const float midM = (azCm + bzCm) * 0.5f * 0.01f;
            int band = (int)(((midM - RangeMinM) / spanZ) * (float)kBands);
            if (band < 0)
                band = 0;
            if (band >= kBands)
                band = kBands - 1;

            // + half a spacing: the producer traces SAMPLE CENTRES
            //     x = TileX*TILE + col*SPACING + SPACING*0.5
            // so the lattice sits half a cell inside each tile. Dropping the half-cell slides
            // the whole wireframe 8 m off the tile grid and off the player marker.
            const D2D1_POINT_2F p0 = D2D1::Point2F(((float)ax + 0.5f) * spacingM, ((float)ay + 0.5f) * spacingM);
            const D2D1_POINT_2F p1 = D2D1::Point2F(((float)bx + 0.5f) * spacingM, ((float)by + 0.5f) * spacingM);
            sink[band]->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
            sink[band]->AddLine(p1);
            sink[band]->EndFigure(D2D1_FIGURE_END_OPEN);
            ++segs;
        };

        // OWNERSHIP RULE: a tile owns exactly the segments whose SOUTH-WEST endpoint lies inside
        // it. Origins therefore stop one step short of the tile edge, while the east and north
        // targets are free to land in the neighbour - SampleAt resolves across tiles, so the
        // seam closes with no duplicate.
        //
        // The first cut of this iterated origins to <= TERR_SAMPLES to "reach" the neighbour,
        // which quietly drew every seam segment TWICE: once from this tile's column 32 and once
        // from the east tile's column 0. Two overlapping strokes at different band alphas, which
        // would have read as a brighter line down every tile boundary and looked like a data
        // artefact rather than a bookkeeping one.
        for (int32_t row = 0; row < TERR_SAMPLES; row += st)
            for (int32_t col = 0; col < TERR_SAMPLES; col += st)
            {
                const int32_t gx = gx0 + col, gy = gy0 + row;
                float z;
                if (!SampleAt(gx, gy, z))
                    continue;
                float zr;
                if (SampleAt(gx + st, gy, zr))
                    emit(gx, gy, z, gx + st, gy, zr);
                float zu;
                if (SampleAt(gx, gy + st, zu))
                    emit(gx, gy, z, gx, gy + st, zu);
            }

        for (int i = 0; i < kBands; ++i)
        {
            sink[i]->Close();
            sink[i]->Release();
        }
        b.Segments = segs;

        // POST-READ CHECK. Heights are read in place, outside any lock. The one thing that can
        // pull the rug out is eviction flipping this slot to a different tile mid-bake, so if
        // the slot no longer says what it said, throw the work away and retry. Cheap insurance
        // against a garbage frame that would be near impossible to reproduce.
        const TerrainSlotInfo &nowInfo = Shared->H.Slots[slot];
        if (nowInfo.State != TTS_Complete || nowInfo.TileX != wasX || nowInfo.TileY != wasY)
        {
            DropBakedGeometry(b);
            b.Dirty = true;
        }
    }

    // ---- drawing ----------------------------------------------------------------------------

    void EndDraw()
    {
        const HRESULT hr = RT->EndDraw();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET)
        {
            // The host owns recovery: it rebuilds the bitmap and hands the new target back
            // through SetRenderTarget. Drop the stale pointer so nothing draws into it first.
            SafeRelease(Brush);
            RT = nullptr;
        }
    }

    void SetColor(const D2D1_COLOR_F &c, float alpha = 1.0f)
    {
        Brush->SetColor(c);
        Brush->SetOpacity(alpha);
    }

    // <windows.h> #defines DrawText -> DrawTextW before d2d1.h is reached, so
    // ID2D1RenderTarget::DrawText is not callable by that name. The rest of this project
    // sidesteps it with CreateTextLayout + DrawTextLayout rather than #undef-ing a macro other
    // headers may rely on, and so does this - one convention, not two.
    void Label(const wchar_t *s, float x, float y, float w, const D2D1_COLOR_F &c, float alpha = 1.0f)
    {
        if (!TextFmt || !DW || !s || !*s)
            return;
        IDWriteTextLayout *layout = nullptr;
        if (FAILED(DW->CreateTextLayout(s, (UINT32)wcslen(s), TextFmt, w, 24.0f, &layout)) || !layout)
            return;
        SetColor(c, alpha);
        RT->DrawTextLayout(D2D1::Point2F(x, y), layout, Brush);
        layout->Release();
    }

    void DrawBanner(const D2D1_SIZE_F &sz, const wchar_t *title, const wchar_t *detail, const D2D1_COLOR_F &c)
    {
        const float x = sz.width * Inset;
        const float w = sz.width * (1.0f - 2.0f * Inset);
        const float y = sz.height * 0.5f - 20.0f;
        Label(title, x, y, w, c);
        Label(detail, x, y + 20.0f, w, c, 0.65f);
    }

    // Conservative visibility: the tile is an axis-aligned square in WORLD space, but an
    // arbitrarily rotated one on screen, so its four transformed corners give the screen-space
    // bounding box to test. Four points per cached tile per frame - nothing.
    static bool TileOnScreen(const D2D1_MATRIX_3X2_F &m, float x0, float y0, float side,
                             const D2D1_RECT_F &r)
    {
        const float xs[2] = {x0, x0 + side}, ys[2] = {y0, y0 + side};
        float sx0 = 1.0e30f, sy0 = 1.0e30f, sx1 = -1.0e30f, sy1 = -1.0e30f;
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
            {
                const float X = xs[i] * m.m11 + ys[j] * m.m21 + m.dx;
                const float Y = xs[i] * m.m12 + ys[j] * m.m22 + m.dy;
                sx0 = FMin(sx0, X);
                sx1 = FMax(sx1, X);
                sy0 = FMin(sy0, Y);
                sy1 = FMax(sy1, Y);
            }
        return !(sx1 < r.left || sx0 > r.right || sy1 < r.top || sy0 > r.bottom);
    }

    void DrawMap(const D2D1_SIZE_F &sz, float focusX, float focusY, float span)
    {
        const float viewW = sz.width * (1.0f - 2.0f * Inset);
        const float viewH = sz.height * (1.0f - 2.0f * Inset);
        const float cx = sz.width * 0.5f, cy = sz.height * 0.5f;

        // The span is measured across the shorter screen axis, so the readout means the same
        // thing on a square MFD window as on a wide one and the scale bar agrees with it.
        const float shorter = FMin(viewW, viewH);
        const float s = (span > 1.0f) ? (shorter / span) : 1.0f;
        const float yawRad = NorthUp ? 0.0f : (Head.PlayerYaw * 0.01745329252f);
        const float cs = std::cos(yawRad), sn = std::sin(yawRad);

        // The content rectangle - the same 80% box the widget screens are drawn into - is both
        // the clip and the cull test, so what is culled and what is visible cannot disagree.
        const D2D1_RECT_F content = D2D1::RectF(sz.width * Inset, sz.height * Inset,
                                                sz.width * (1.0f - Inset), sz.height * (1.0f - Inset));

        // Everything the map is made of stays inside the display area, exactly like the screen
        // pages and the scope. Without this the wireframe runs under the bezel legends and off
        // the edge of the window, which is why this page did not read as one of the displays.
        RT->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_ALIASED);
        DrawTerrainCore(cx, cy, s, yawRad, content, true, true, focusX, focusY);
        RT->PopAxisAlignedClip();

        // The frame, so the page reads as a display of the same size as the others rather than
        // as terrain that happens to stop.
        SetColor(kTileLine, 0.75f);
        RT->DrawRectangle(content, Brush, 1.0f);

        DrawReadout(sz, s, cs, sn, span);
    }

    // -----------------------------------------------------------------------------------------
    // THE RENDERER, shared by the full-page map and the LIDAR underlay.
    //
    // Takes everything it needs as arguments - centre, scale, heading, cull rectangle - so that
    // neither caller has any private knowledge of how terrain is drawn and there is exactly one
    // implementation to be right. The caller owns the clip (a rectangle for the page, a disc for
    // the scope) and owns whether the tile grid and player marker appear at all, because the
    // scope already draws its own rings and centre mark and a second set would just be clutter.
    //
    // Leaves the transform as it found it.
    // -----------------------------------------------------------------------------------------
    uint32_t DrawTerrainCore(float cx, float cy, float s, float yawRad, const D2D1_RECT_F &cull, bool tileGrid,
                             bool playerMarker, float focusXm, float focusYm)
    {
        // The point the view is centred on. Normally the mech; in FIT, the centre of the map,
        // because framing a map around the player would need twice the span to reach its far
        // corner and spend most of the screen on nothing.
        const float fx = focusXm;
        const float fy = focusYm;
        const float cs = std::cos(yawRad), sn = std::sin(yawRad);

        // Exactly LidarView::DrawContact's projection, so the two layers composite without
        // either being re-derived:
        //     fwd   =  dx*cs + dy*sn      sy = cy - fwd  * s
        //     right = -dx*sn + dy*cs      sx = cx + right* s
        // expanded into a matrix over absolute world coordinates:
        //     sx = wx*(-s*sn) + wy*( s*cs) + [cx + s*( fx*sn - fy*cs)]
        //     sy = wx*(-s*cs) + wy*(-s*sn) + [cy + s*( fx*cs + fy*sn)]
        // North-up is this same formula with yaw pinned to zero - not a second code path.
        const D2D1_MATRIX_3X2_F m =
            D2D1::Matrix3x2F(-s * sn, -s * cs, s * cs, -s * sn, cx + s * (fx * sn - fy * cs),
                             cy + s * (fx * cs + fy * sn));
        RT->SetTransform(m);

        // Stroke widths are in the geometry's own units and scale with the transform, so
        // dividing by s pins them to a constant pixel width.
        const float px = (s > 1.0e-6f) ? (1.0f / s) : 1.0f;

        // World-space extent to cover, from the cull rectangle's half-diagonal, so it still
        // covers the view when the map is rotated to the mech's heading.
        const float tileM = TERR_TILE_CM * 0.01f;
        const float cw = cull.right - cull.left, ch = cull.bottom - cull.top;
        const float half = 0.5f * std::sqrt(cw * cw + ch * ch) / s + tileM;
        const float minX = fx - half, maxX = fx + half;
        const float minY = fy - half, maxY = fy + half;

        if (tileGrid)
        {
        // Tile boundaries, underneath, so the lattice reads on top of them.
        SetColor(kTileLine, 0.5f);
        for (int32_t i = (int32_t)std::floor(minX / tileM); i <= (int32_t)std::floor(maxX / tileM) + 1; ++i)
        {
            const float wx = (float)i * tileM;
            RT->DrawLine(D2D1::Point2F(wx, minY), D2D1::Point2F(wx, maxY), Brush, px);
        }
        for (int32_t j = (int32_t)std::floor(minY / tileM); j <= (int32_t)std::floor(maxY / tileM) + 1; ++j)
        {
            const float wy = (float)j * tileM;
            RT->DrawLine(D2D1::Point2F(minX, wy), D2D1::Point2F(maxX, wy), Brush, px);
        }
        }

        // The wireframe, culled to the visible rectangle - which is what lets the producer cache
        // an entire map without the drawing cost following it, and what makes zooming in
        // genuinely cheaper rather than merely different.
        const double now = NowMs();
        uint32_t drawnSegs = 0;
        int drawnTiles = 0;

        for (int i = 0; i < kMaxBaked; ++i)
        {
            Baked &b = Cache[i];
            if (!b.Used)
                continue;
            // Culled in SCREEN space, against the same rectangle that clips the drawing. A
            // world-space box test cannot be used once the map rotates - and the previous
            // version also added a tile of slack on top of an exact overlap test that already
            // handled partial tiles, which is why the field log read "drawing 56 tiles /
            // 25216 segs" where 36 and 18k were expected.
            if (!TileOnScreen(m, (float)b.TileX * tileM, (float)b.TileY * tileM, tileM, cull))
                continue;

            // Visible: protected from geometry eviction, and wanted first by the bake queue.
            b.LastSeenMs = now;

            // Deliberately NOT skipped while dirty. A dirty tile still holds its previous
            // geometry, and drawing that is strictly better than drawing nothing: the map
            // refines in place instead of blanking and refilling. Without this, anything that
            // dirties every tile at once - the elevation range widening, or switching between
            // the page and the scope at different LOD strides - would empty the display for the
            // second or so the bake queue takes to drain.
            if (b.Segments == 0)
                continue;

            ++drawnTiles;
            drawnSegs += b.Segments;
            for (int k = 0; k < kBands; ++k)
            {
                if (!b.Bands[k])
                    continue;
                const float t = (kBands > 1) ? ((float)k / (float)(kBands - 1)) : 1.0f;
                const D2D1_COLOR_F c = {kBandLow.r + (kBandHigh.r - kBandLow.r) * t,
                                        kBandLow.g + (kBandHigh.g - kBandLow.g) * t,
                                        kBandLow.b + (kBandHigh.b - kBandLow.b) * t, 1.0f};
                SetColor(c, 0.55f + 0.45f * t);
                RT->DrawGeometry(b.Bands[k], Brush, px * (0.9f + 0.5f * t));
            }
        }
        LastDrawnTiles = drawnTiles;
        LastDrawnSegments = drawnSegs;

        if (playerMarker)
        {
            // The MECH's position, not the focus point. Those are the same thing in the normal
            // chase view and different in FIT, where drawing the marker at fx/fy would plant the
            // mech in the middle of the map - plausible-looking and completely wrong.
            const float mx = Head.PlayerX * 0.01f;
            const float my = Head.PlayerY * 0.01f;
            SetColor(kPlayer, 1.0f);
            const float r = 10.0f * px;
            RT->DrawLine(D2D1::Point2F(mx - r, my), D2D1::Point2F(mx + r, my), Brush, px * 1.6f);
            RT->DrawLine(D2D1::Point2F(mx, my - r), D2D1::Point2F(mx, my + r), Brush, px * 1.6f);
            RT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), r * 0.55f, r * 0.55f), Brush, px * 1.6f);
        }

        RT->SetTransform(D2D1::Matrix3x2F::Identity());
        return drawnSegs;
    }

    void DrawReadout(const D2D1_SIZE_F &sz, float pixelsPerMetre, float cs, float sn, float span)
    {
        const float x = sz.width * Inset + 4.0f;
        float y = sz.height * Inset + 2.0f;
        const float w = sz.width * (1.0f - 2.0f * Inset);
        wchar_t b[256];

        swprintf_s(b, L"TOPO   gen %u   map %u tiles%s   %d x %d", Head.Generation, Head.Complete,
                   Head.Exploring ? L" exploring" : L"", Head.MaxTileX - Head.MinTileX + 1,
                   Head.MaxTileY - Head.MinTileY + 1);
        Label(b, x, y, w, kText);
        y += 18.0f;

        swprintf_s(b, L"elev   %.0f - %.0f m   relief %.0f m", RangeMinM, RangeMaxM, RangeMaxM - RangeMinM);
        Label(b, x, y, w, kText, 0.85f);
        y += 18.0f;

        // The numbers that matter for cost: what is actually being drawn, at what resolution,
        // and what the last rebuild cost. v1 shipped a diag that only fired on page select,
        // which is to say never at the moment the number was interesting.
        swprintf_s(b, L"%-6s %.0f m   cell %d m   draw %d tiles / %u segs   bake %.1f ms   %s",
                   FitActive ? (Head.Exploring ? L"FIT*" : L"FIT") : L"span", span,
                   (int)(TERR_SPACING_CM * 0.01f) * Stride, LastDrawnTiles, LastDrawnSegments, LastBakeMs,
                   NorthUp ? L"NORTH-UP" : L"HDG-UP");
        Label(b, x, y, w, kText, 0.85f);
        y += 18.0f;

        if (Head.OriginAboveGroundCm >= 0.0f)
            swprintf_s(b, L"plyr   X %.0f  Y %.0f  Z %.0f m   AGL %.1f m", Head.PlayerX * 0.01f,
                       Head.PlayerY * 0.01f, Head.PlayerZ * 0.01f, Head.OriginAboveGroundCm * 0.01f);
        else
            swprintf_s(b, L"plyr   X %.0f  Y %.0f  Z %.0f m   AGL --", Head.PlayerX * 0.01f,
                       Head.PlayerY * 0.01f, Head.PlayerZ * 0.01f);
        Label(b, x, y, w, kText, 0.85f);

        // The compass now has to TURN, because the map does. World north is +X, and putting
        // (1,0) through the same projection gives its screen direction: fwd = cs, right = -sn,
        // so the arrow points along (-sn, -cs). At yaw 0 that is straight up, which is the
        // north-up case falling out of the general one rather than being special-cased.
        //
        // A fixed arrow over a rotating map would be the same class of error as a north-up map
        // under a heading-up scope: confidently wrong, and nothing on screen to say so.
        const float ndx = -sn, ndy = -cs;
        const float px2 = -ndy, py2 = ndx; // perpendicular, for the arrow head
        const float ncx = sz.width * (1.0f - Inset) - 22.0f;
        const float ncy = sz.height * Inset + 22.0f;
        const float nlen = 14.0f;
        const float tipX = ncx + ndx * nlen, tipY = ncy + ndy * nlen;
        SetColor(kText, 0.85f);
        RT->DrawLine(D2D1::Point2F(ncx - ndx * nlen, ncy - ndy * nlen), D2D1::Point2F(tipX, tipY), Brush, 1.5f);
        RT->DrawLine(D2D1::Point2F(tipX - ndx * 6.0f + px2 * 4.0f, tipY - ndy * 6.0f + py2 * 4.0f),
                     D2D1::Point2F(tipX, tipY), Brush, 1.5f);
        RT->DrawLine(D2D1::Point2F(tipX - ndx * 6.0f - px2 * 4.0f, tipY - ndy * 6.0f - py2 * 4.0f),
                     D2D1::Point2F(tipX, tipY), Brush, 1.5f);
        Label(L"N", tipX - 4.0f, tipY - 9.0f, 20.0f, kText, 0.85f);

        // Heading readout, since "up" no longer tells you which way you are pointing.
        if (!NorthUp)
        {
            swprintf_s(b, L"HDG %03d", ((int)Head.PlayerYaw % 360 + 360) % 360);
            Label(b, sz.width * 0.5f - 28.0f, sz.height * Inset + 2.0f, 80.0f, kPlayer, 0.9f);
        }

        if (pixelsPerMetre > 1.0e-6f)
        {
            float barM = 500.0f;
            const float maxPx = sz.width * 0.25f;
            while (barM * pixelsPerMetre > maxPx && barM > 50.0f)
                barM *= 0.5f;
            const float barPx = barM * pixelsPerMetre;
            const float bx = sz.width * Inset + 4.0f;
            const float by = sz.height * (1.0f - Inset) - 10.0f;
            SetColor(kText, 0.8f);
            RT->DrawLine(D2D1::Point2F(bx, by), D2D1::Point2F(bx + barPx, by), Brush, 1.5f);
            RT->DrawLine(D2D1::Point2F(bx, by - 4.0f), D2D1::Point2F(bx, by + 4.0f), Brush, 1.5f);
            RT->DrawLine(D2D1::Point2F(bx + barPx, by - 4.0f), D2D1::Point2F(bx + barPx, by + 4.0f), Brush, 1.5f);
            swprintf_s(b, L"%.0f m", barM);
            Label(b, bx + barPx + 6.0f, by - 10.0f, 80.0f, kText, 0.8f);
        }
    }

    // ---- state ------------------------------------------------------------------------------

    // Aliased, not redefined - two copies of a clamp range is how a public API and its
    // implementation drift apart without anyone noticing.
    static constexpr float kMinSpanM = TopoView::kMinSpanM;
    static constexpr float kMaxSpanM = TopoView::kMaxSpanM;
    static constexpr float kSpanStepM = TopoView::kSpanStepM;

    ID2D1Factory *Factory = nullptr;
    IDWriteFactory *DW = nullptr;
    ID2D1RenderTarget *RT = nullptr;
    ID2D1SolidColorBrush *Brush = nullptr;
    IDWriteTextFormat *TextFmt = nullptr;

    HANDLE Map = nullptr;
    const TerrainBlock *Shared = nullptr;
    double LastAttemptMs = 0.0;

    TerrainHeader Head = {};    // last CONSISTENT header read
    TerrainHeader Scratch = {}; // landing pad for a read that may turn out to be torn
    bool HeadValid = false;

    float Inset = 0.10f;
    float SpanM = 2560.0f;
    int Stride = 4; // 64 m cells; see CELL SIZE at the top of this file
    bool NorthUp = false; // heading-up by default, matching the scope this will underlay
    bool FitMap = false;   // requested
    bool FitActive = false; // and actually achievable this frame - there may be no extent yet

    uint32_t BakedGeneration = 0xFFFFFFFFu;
    bool HaveRange = false;
    float RangeMinM = 0.0f, RangeMaxM = 0.0f;

    // Scope-disc clip geometry, cached across frames (see DrawTerrainUnderlay).
    ID2D1EllipseGeometry *Disc = nullptr;
    float Disc_cx = 0.0f, Disc_cy = 0.0f, Disc_r = 0.0f;

    float LastBakeMs = 0.0f;
    int LastDrawnTiles = 0;
    uint32_t LastDrawnSegments = 0;
};

// ===========================================================================================
// Thin forwarding shell, matching LidarView.

TopoView::TopoView() : P(new TopoViewImpl()) {}
TopoView::~TopoView()
{
    if (P)
    {
        P->Shutdown();
        delete P;
        P = nullptr;
    }
}

bool TopoView::InitResources(ID2D1Factory *f, IDWriteFactory *dw) { return P->InitResources(f, dw); }
void TopoView::SetRenderTarget(ID2D1RenderTarget *rt) { P->SetRenderTarget(rt); }
void TopoView::SetContentInset(float inset) { P->SetContentInset(inset); }
void TopoView::Poll() { P->Poll(); }
void TopoView::Render() { P->Render(); }
void TopoView::Shutdown() { P->Shutdown(); }
bool TopoView::IsConnected() const { return P->IsConnected(); }
float TopoView::GetSpanM() const { return P->GetSpanM(); }
void TopoView::SetSpanM(float metres) { P->SetSpanM(metres); }
void TopoView::AdjustSpan(bool in) { P->AdjustSpan(in); }
void TopoView::SetFitMap(bool on) { P->SetFitMap(on); }
bool TopoView::GetFitMap() const { return P->GetFitMap(); }
void TopoView::SetNorthUp(bool on) { P->SetNorthUp(on); }
void TopoView::SetCellM(int metres) { P->SetCellM(metres); }
int TopoView::GetCellM() const { return P->GetCellM(); }
uint32_t TopoView::DrawTerrainUnderlay(ID2D1RenderTarget *rt, float cx, float cy, float rad, float rangeCm,
                                       float yawDeg)
{
    return P->DrawTerrainUnderlay(rt, cx, cy, rad, rangeCm, yawDeg);
}
bool TopoView::GetNorthUp() const { return P->GetNorthUp(); }

TopoView::Diag TopoView::GetDiag() const
{
    Diag d = {};
    P->FillDiag(d);
    return d;
}
