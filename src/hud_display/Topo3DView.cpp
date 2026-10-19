// Topo3DView.cpp - see Topo3DView.h for why this page is D3D11 and the others are not.

#include "Topo3DView.h"
#include "LidarView.h" // LidarScene and friends
#include "DropshipOutline.h" // DROPSHIPS (2026-10-08): hull outlines from assets/dropships
#include "AssetRoot.h"       // the art folder for a host not beside it (EnhancedImaging.dll)
#include "../common/PaperdollCache.h" // 2026-10-17: the masks extracted from the player's own game files

#include <windows.h>

#include <d2d1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <wincodec.h> // per-chassis paperdoll masks, loaded from PNG

#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility> // std::swap (bug sweep 21)

#include "TerrainShared.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace
{

// <windows.h> arrives without NOMINMAX in this project, so min/max are live macros and the
// <algorithm> helpers do not compile. Hand-rolled, as everywhere else in this tree.
inline float FMin(float a, float b) { return a < b ? a : b; }
inline float FMax(float a, float b) { return a > b ? a : b; }
inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

template <typename T> void SafeRelease(T *&p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

inline bool IsHole(float z) { return !(z > -1.0e8f); }
inline int32_t FloorDiv(int32_t a, int32_t b) { return (a >= 0) ? (a / b) : -(((-a) + b - 1) / b); }

// Vertical exaggeration. 1:1 read flatter than the terrain actually is, because a perspective
// view compresses height far more than a top-down one does. 1.2 is a nudge, not a lie: slopes
// stay readable as slopes and a 30 degree hill does not become a cliff.
constexpr float kVertExChase = 1.2f;
// Switchable, not constant: the pilot-camera view (the vanilla Targeting screen) must be TRUE
// scale, or the world would not line up with the game's own crosshair and mesh outlines. The
// terrain mesh bakes this in, so a change drops the tiles and they rebuild.
static float kVertEx = kVertExChase;

// How far the ground is drawn, independent of zoom.
//
// Range used to drive the camera distance, the terrain draw distance AND the distance fade all
// at once, so zooming in pulled the horizon in with the camera and the landscape dissolved into
// a dark ring a few dozen metres out - fog of war produced by the zoom control rather than by
// anything in the world.
//
// The fade is still worth having: a 3D view has no rim to hide the data edge behind, so terrain
// stopping dead would read as a cliff at exactly the draw distance. But it belongs to the
// HORIZON, which is a property of how much ground we are willing to draw, not of how close the
// camera happens to be. 1000 m matches the plugin's scan range, so the picture ends where the
// sensor does.
constexpr float kHorizonM = 1000.0f;
constexpr float kFadeStartFrac = 0.80f; // fade only over the last fifth, so it reads as haze

// World metres -> render metres. ONE function, used by the terrain mesh, the camera, the
// contacts, the stalks and the sweep alike - if any of them skipped it, that layer would float
// above or sink into the ground by 20% of its altitude, which looks like a data fault.
inline float Zr(float zM) { return zM * kVertEx; }

double NowMs()
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

// ---- 4x4, row-major, row-vector (v' = v * M) --------------------------------------------
// Hand-rolled rather than DirectXMath so the arithmetic is visible and testable. The HLSL below
// declares the matrix row_major to match, so no transpose happens anywhere.
struct Mat4
{
    float m[4][4];
};

Mat4 MatMul(const Mat4 &a, const Mat4 &b)
{
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

struct V3
{
    float x, y, z;
};
inline V3 Sub(const V3 &a, const V3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3 &a, const V3 &b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline V3 Norm(const V3 &a)
{
    const float l = std::sqrt(Dot(a, a));
    return (l > 1.0e-6f) ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1};
}

// Left-handed look-at, world up +Z. UE is Z-up, so the world axes go straight in with no
// handedness gymnastics - X north, Y east, Z up.
Mat4 LookAtLH(const V3 &eye, const V3 &focus, const V3 &up)
{
    const V3 f = Norm(Sub(focus, eye));
    const V3 r = Norm(Cross(up, f));
    const V3 u = Cross(f, r);
    Mat4 v{};
    v.m[0][0] = r.x; v.m[0][1] = u.x; v.m[0][2] = f.x; v.m[0][3] = 0.0f;
    v.m[1][0] = r.y; v.m[1][1] = u.y; v.m[1][2] = f.y; v.m[1][3] = 0.0f;
    v.m[2][0] = r.z; v.m[2][1] = u.z; v.m[2][2] = f.z; v.m[2][3] = 0.0f;
    v.m[3][0] = -Dot(r, eye); v.m[3][1] = -Dot(u, eye); v.m[3][2] = -Dot(f, eye); v.m[3][3] = 1.0f;
    return v;
}

Mat4 PerspectiveLH(float fovY, float aspect, float zn, float zf)
{
    const float ys = 1.0f / std::tan(fovY * 0.5f);
    const float xs = ys / aspect;
    Mat4 p{};
    p.m[0][0] = xs;
    p.m[1][1] = ys;
    p.m[2][2] = zf / (zf - zn);
    p.m[2][3] = 1.0f;
    p.m[3][2] = -zn * zf / (zf - zn);
    return p;
}

// Colours, matching LidarView's palette so the two pages are visibly the same instrument.
inline D2D1_COLOR_F RGB24(uint32_t c, float a = 1.0f)
{
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a);
}
constexpr uint32_t kColBackground = 0x070D0B;
constexpr uint32_t kColSweep = 0x3FE6CD;
constexpr uint32_t kColPlayer = 0x7DFF6A;
constexpr uint32_t kColFriendly = 0x5DFFA0;
constexpr uint32_t kColHostile = 0xFF5A4D;
constexpr uint32_t kColNeutral = 0xF0A93C;
constexpr uint32_t kColUnknown = 0x35E0C8;
constexpr uint32_t kColSelected = 0xFFFFFF;
// Contact labels. 10.5 px Consolas advances ~5.8 px a character; the plate and the clash test
// are sized from that rather than by measuring every string every frame.
constexpr float kContactFontPx = 10.5f;
constexpr float kContactCharW  = 5.8f;
constexpr uint32_t kColText = 0x3FE6CD;
constexpr uint32_t kColProjBallistic = 0x9A6A33;
constexpr uint32_t kColProjPPC = 0x4D8CFF;
constexpr uint32_t kColProjMissile = 0xF0503C;
constexpr uint32_t kColSmoke = 0x9AA0A0;
constexpr uint32_t kColProjBound = 0xFFD11A;


// Matching the scope's sizes exactly, because "almost identical" means these numbers.
constexpr float kSalvoFlashScale = 4.0f;

// Mirrors of LidarShared's enums. Deliberately local: this file takes its data through
// LidarScene, which is POD by design, and pulling in LidarShared.h just for three enum values
// would undo that.
// Mirrors LidarShared.h's LidarKind. Spelled out here rather than carrying one value,
// because the renderer now varies the marker by kind and a lone KIND_MECH was what led to a
// turret being drawn identically to an Atlas.
enum { KIND_MECH = 0, KIND_TANK = 1, KIND_TURRET = 2, KIND_VTOL = 3, KIND_BA = 4 }; // KIND_BA: a battle armour suit (2026-10-17)
enum { CT_FRIENDLY = 0, CT_HOSTILE = 1, CT_NEUTRAL = 2 };
// Projectile colour no longer reads Type here at all - LidarView resolves it into ColorIdx
// against the real constants, so there is nothing left to mirror and get wrong.

const char *kShaderSrc = R"HLSL(
cbuffer CB : register(b0)
{
    row_major float4x4 ViewProj;
    float4 Elev;      // x = min Z (m), y = 1/range, z = fade start (m), w = fade end (m)
    float4 FocusXY;   // xy = camera focus in world metres
    float4 ColLow;
    float4 ColHigh;
};
struct VSIn  { float3 P : POSITION; };
struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };

VSOut main_vs(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.P, 1.0), ViewProj);
    float t = saturate((i.P.z - Elev.x) * Elev.y);
    float d = distance(i.P.xy, FocusXY.xy);
    // Fade out toward the horizon instead of stopping at a hard circle: a 3D view has no rim
    // to hide the edge behind, so a hard cut would read as a cliff at exactly the range limit.
    float fade = saturate(1.0 - (d - Elev.z) / max(1e-3, Elev.w - Elev.z));
    o.col = float4(lerp(ColLow.rgb, ColHigh.rgb, t), lerp(ColLow.a, ColHigh.a, t) * fade);
    return o;
}

float4 main_ps(VSOut i) : SV_TARGET { return i.col; }
)HLSL";

struct CB
{
    Mat4 ViewProj;
    float Elev[4];
    float FocusXY[4];
    float ColLow[4];
    float ColHigh[4];
};

} // namespace

// ===========================================================================================

class Topo3DViewImpl
{
  public:
    // Terrain geometry cache, same shape as the 2D page's: bounded, LRU by what is on screen,
    // budgeted per frame. Heights are the producer's whole-map cache; this is only what can
    // currently be seen.
    // Raised from 64 when the horizon stopped following the zoom. Drawing out to 1000 m means
    // tiles are gathered to ~1760 m, which is an 8 x 8 block - exactly 64, with no headroom, and
    // a cache that is exactly full refuses new tiles rather than evicting on-screen ones. At
    // 16 m cells this is about 3 MB of vertex buffers, which is not a number worth economising.
    // bug sweep 14 (2026-10-18): 96 was sized for the fixed 1000 m horizon; the 5 km mode's horizon follows the zoom and
    // gathers up to ~430 tiles at 5 km - the cache refused the rest and filled from the south-west, so the ground round the
    // mech went missing. Now as large as the producer's whole map (TERR_MAX_TILES), so nothing it has can be refused.
    static constexpr int kMaxTiles = TERR_MAX_TILES;
    static constexpr int kBuildsPerFrame = 2;
    static constexpr int kMaxCells = 32; // 16 m cells; 64 m gives 8

    bool InitResources(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID2D1Factory *f, IDWriteFactory *dw)
    {
        Dev = dev;
        Ctx = ctx;
        Factory = f;
        DW = dw;
        if (!Dev || !Ctx)
            return false;

        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(D3DCompile(kShaderSrc, strlen(kShaderSrc), nullptr, nullptr, nullptr, "main_vs", "vs_4_0", 0, 0,
                              &vsb, &err)))
        {
            SafeRelease(err);
            return false;
        }
        SafeRelease(err);
        if (FAILED(D3DCompile(kShaderSrc, strlen(kShaderSrc), nullptr, nullptr, nullptr, "main_ps", "ps_4_0", 0, 0,
                              &psb, &err)))
        {
            SafeRelease(err);
            SafeRelease(vsb);
            return false;
        }
        SafeRelease(err);

        bool ok = SUCCEEDED(Dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &VS)) &&
                  SUCCEEDED(Dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &PS));

        const D3D11_INPUT_ELEMENT_DESC el[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        ok = ok && SUCCEEDED(Dev->CreateInputLayout(el, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &Layout));
        SafeRelease(vsb);
        SafeRelease(psb);
        if (!ok)
            return false;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CB);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(Dev->CreateBuffer(&bd, nullptr, &CBuf)))
            return false;

        // The fill pass is pushed AWAY from the eye so the wireframe drawn on top of it wins the
        // depth test. Without the bias the lines lie exactly on the triangles they came from and
        // z-fight into a stipple that looks like a data problem.
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE; // terrain is viewed from both sides of a ridge
        rd.DepthBias = 2000;
        rd.SlopeScaledDepthBias = 1.5f;
        rd.DepthClipEnable = TRUE;
        if (FAILED(Dev->CreateRasterizerState(&rd, &RsFill)))
            return false;
        rd.DepthBias = 0;
        rd.SlopeScaledDepthBias = 0.0f;
        if (FAILED(Dev->CreateRasterizerState(&rd, &RsLine)))
            return false;

        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(Dev->CreateDepthStencilState(&dsd, &DsOn)))
            return false;

        D3D11_BLEND_DESC bl{};
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(Dev->CreateBlendState(&bl, &BsAlpha)))
            return false;
        bl.RenderTarget[0].BlendEnable = FALSE;
        if (FAILED(Dev->CreateBlendState(&bl, &BsOpaque)))
            return false;
        bl.RenderTarget[0].RenderTargetWriteMask = 0; // depth only: the transparent page's occluders
        Dev->CreateBlendState(&bl, &BsNoColor);

        if (DW)
            DW->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &TextFmt);
        // Contact labels only. The header and the cue panel keep 13 px; the names standing over
        // the battlefield are the text that collides, so they are the text that shrinks.
        if (DW)
            DW->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, kContactFontPx, L"en-us", &TextFmtSm);
        Ready = true;
        return true;
    }

    void SetTargets(ID3D11RenderTargetView *rtv, ID2D1RenderTarget *d2d, unsigned w, unsigned h)
    {
        RTV = rtv;
        if (RT != d2d)
        {
            SafeRelease(Brush);
            RT = d2d;
            if (RT)
                RT->CreateSolidColorBrush(RGB24(kColText), &Brush);
        }
        if (w != Width || h != Height)
        {
            // bug sweep 21 (2026-10-18): TWO depth buffers - the window's and the Enhanced Imaging fallback's (the game's
            // size) - kept side by side, instead of one rebuilt twice every frame while the fallback runs.
            if (DepthTexAlt && w == AltW && h == AltH)
            {
                std::swap(DepthTex, DepthTexAlt);
                std::swap(DSV, DSVAlt);
                std::swap(Width, AltW);
                std::swap(Height, AltH);
            }
            else
            {
                SafeRelease(DSVAlt);
                SafeRelease(DepthTexAlt);
                DepthTexAlt = DepthTex;
                DSVAlt = DSV;
                AltW = Width;
                AltH = Height;
                DepthTex = nullptr;
                DSV = nullptr;
                Width = w;
                Height = h;
                RebuildDepth();
            }
        }
        if (!rtv)
        {
            SafeRelease(DSV);
            SafeRelease(DepthTex);
            SafeRelease(DSVAlt);
            SafeRelease(DepthTexAlt);
            AltW = AltH = 0;
        }
    }

    void SetContentInset(float i) { Inset = (i >= 0.0f && i < 0.45f) ? i : 0.10f; }
    void SetRangeM(unsigned m) { RangeM = (m < 50) ? 50 : (m > 5000 ? 5000 : m); } // 5000: the 5 km radar mode (2026-10-15)

    void PilotLock(const LidarScene &scene) { RenderPilotLock(scene); }

    // Lasers in the frame the outlines come from: pairs of world points (cm), muzzle to hit.
    // Drawn as bright green lines with a soft wider glow under them, over the outlines.
    static constexpr int kMaxPilotBeams = 128;
    float PilotBeams[kMaxPilotBeams][6] = {};
    int   PilotBeamCount = 0;
    void SetPilotBeams(int n, const float *fromTo)
    {
        PilotBeamCount = (n < 0 || !fromTo) ? 0 : (n > kMaxPilotBeams ? kMaxPilotBeams : n);
        for (int i = 0; i < PilotBeamCount; ++i)
            for (int j = 0; j < 6; ++j)
                PilotBeams[i][j] = fromTo[i * 6 + j];
    }
    void RenderPilotBeams()
    {
        if (!PilotOn || !PilotVpValid || !RT || !Brush || PilotBeamCount <= 0)
            return;
        RT->BeginDraw();
        RT->PushAxisAlignedClip(D2D1::RectF(Width * Inset, Height * Inset, Width * (1.0f - Inset), Height * (1.0f - Inset)),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        Mono = false;
        for (int i = 0; i < PilotBeamCount; ++i)
        {
            const float *b = PilotBeams[i];
            float ax, ay, ad, bx, by, bd;
            if (!ClipProject(b, ax, ay, bx, by))
                continue;
            Col(0x55FF8A, 0.28f);
            RT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), Brush, 5.0f);
            Col(0xC8FFD8, 0.95f);
            RT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), Brush, 1.6f);
            (void)ad; (void)bd;
        }
        RT->PopAxisAlignedClip();
        const HRESULT hr = RT->EndDraw();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET)
        {
            SafeRelease(Brush);
            RT = nullptr;
        }
    }

    // A world segment (cm) to screen, clipped against the near side of the eye so a beam from
    // our own muzzle - which starts beside the camera - still draws.
    bool ClipProject(const float *seg, float &ax, float &ay, float &bx, float &by) const
    {
        float a[3] = {seg[0] * 0.01f, seg[1] * 0.01f, seg[2] * 0.01f};
        float c[3] = {seg[3] * 0.01f, seg[4] * 0.01f, seg[5] * 0.01f};
        auto w = [&](const float *p) {
            const float z = Zr(p[2]);
            return p[0] * PilotVp.m[0][3] + p[1] * PilotVp.m[1][3] + z * PilotVp.m[2][3] + PilotVp.m[3][3];
        };
        const float wa = w(a), wc = w(c), eps = 0.1f;
        if (wa < eps && wc < eps)
            return false;
        if (wa < eps || wc < eps)
        {
            const float t = (eps - wa) / (wc - wa);
            float *m = (wa < eps) ? a : c;
            float p[3] = {a[0] + (c[0] - a[0]) * t, a[1] + (c[1] - a[1]) * t, a[2] + (c[2] - a[2]) * t};
            m[0] = p[0]; m[1] = p[1]; m[2] = p[2];
        }
        float d;
        return Project(PilotVp, a[0], a[1], a[2], ax, ay, d) && Project(PilotVp, c[0], c[1], c[2], bx, by, d);
    }
    void SetPilotBare(bool bare) { PilotBare = bare; }
    void SetTransparent(bool on) { Transparent = on; }
    void SetPilotMissileLock(bool acquiring, float progress, bool locked)
    {
        MlAcquiring = acquiring;
        MlProgress  = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
        MlLocked    = locked;
    }

    void SetPilotLiveLock(bool valid, const float *originCm, const float *extentCm)
    {
        PilotLiveValid = valid && originCm && extentCm;
        if (PilotLiveValid)
            for (int i = 0; i < 3; ++i)
            {
                PilotLiveOrigin[i] = originCm[i];
                PilotLiveExtent[i] = extentCm[i];
            }
    }
    int  PilotUnitBoxes(float (*out)[4], int max) const
    {
        const int n = PilotBoxCount < max ? PilotBoxCount : max;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < 4; ++j)
                out[i][j] = PilotBoxes[i][j];
        return n;
    }

    void SetPilotCamera(bool on, const float *locCm, const float *rotDeg, float hfovDeg)
    {
        PilotOn = on;
        if (on && locCm && rotDeg)
        {
            for (int i = 0; i < 3; ++i)
            {
                PilotLoc[i] = locCm[i];
                PilotRot[i] = rotDeg[i];
            }
            PilotHFovDeg = hfovDeg;
        }
    }

    void SetCellM(int metres)
    {
        const int raw = (int)(TERR_SPACING_CM * 0.01f);
        int stride = metres / raw;
        if (stride < 1) stride = 1;
        if (stride > 8) stride = 8;
        if (stride != Stride)
        {
            Stride = stride;
            DropAllTiles(); // every buffer was built at the old cell size
        }
    }
    int GetCellM() const { return (int)(TERR_SPACING_CM * 0.01f) * Stride; }

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

    // ---- camera ---------------------------------------------------------------------------

    void Orbit(float dYaw, float dPitch)
    {
        YawOff += dYaw;
        PitchDeg = Clampf(PitchDeg + dPitch, Topo3DView::kMinPitchDeg, Topo3DView::kMaxPitchDeg);
        Free = true;
    }

    // Pan is in the CAMERA's frame, not the world's, so "left" means left on screen whichever
    // way the pilot has spun the view. A world-frame pan would be unusable the moment you orbit.
    void Pan(float fwdM, float rightM)
    {
        // LastCamYawDeg, not the body heading. The camera follows the TORSO now, so a pan frame
        // built on the body would push the view sideways by the twist angle - and the error
        // would only show up while torso-twisted, which is the worst kind of bug to own.
        const float a = LastCamYawDeg * 0.01745329252f;
        const float cs = std::cos(a), sn = std::sin(a);
        PanX += fwdM * cs - rightM * sn;
        PanY += fwdM * sn + rightM * cs;
        Free = true;
    }

    void Recentre()
    {
        YawOff = 0.0f;
        PitchDeg = Topo3DView::kDefaultPitchDeg;
        PanX = PanY = 0.0f;
        Free = false;
    }

    bool IsFree() const { return Free; }

    void Render(const LidarScene &scene)
    {
        LastContacts = scene.ContactCount;
        LastProjectiles = scene.ProjectileCount;
        LastTrails = 0;
        LastFlashing = 0;
        for (int i = 0; i < scene.ProjectileCount; ++i)
        {
            if (scene.Projectiles[i].TrailCount >= 2) ++LastTrails;
            if (scene.Projectiles[i].Flashing) ++LastFlashing;
        }
        TilesDrawn = 0;
        LinesDrawn = 0;

        // bug sweep 21 (2026-10-18): one tile cache per vertical scale (the mesh has it baked in) - the pilot view (V-POD,
        // the Enhanced Imaging fallback) at 1:1, the chase view (3D / TacOps) at 1.2. With one cache, the fallback and the
        // window's 3D page dropped every tile twice a frame and showed at most the two rebuilt each frame.
        kVertEx = PilotOn ? 1.0f : kVertExChase;
        Tiles = TilesSet[PilotOn ? 1 : 0];

        const bool haveTerrain = ReadHeader();
        if (haveTerrain && Head.Generation != BuiltGeneration)
        {
            DropAllTiles();
            BuiltGeneration = Head.Generation;
        }

        // The mech's own position comes from whichever source is live. The terrain block
        // publishes it too, so the camera still follows the mech when the LIDAR publisher is
        // down - which is exactly when you would otherwise be staring at a frozen view and
        // wondering which half had failed.
        float px = Head.PlayerX * 0.01f, py = Head.PlayerY * 0.01f, pz = Head.PlayerZ * 0.01f;
        float yaw = Head.PlayerYaw;
        float torsoRel = Head.PlayerTorsoYaw;
        if (scene.Valid)
        {
            px = scene.PlayerX * 0.01f;
            py = scene.PlayerY * 0.01f;
            pz = scene.PlayerZ * 0.01f;
            yaw = scene.PlayerYaw;
            torsoRel = scene.PlayerTorsoYaw;
        }

        // ---- where the camera points ----------------------------------------------------
        //
        // COCKPIT, not legs. A BattleMech has two headings and this page has to choose one to
        // put "up". The legs were the obvious first choice - they match the scope and the flat
        // map - but they are the wrong answer for a chase view: you aim with the torso, so a
        // camera locked to the legs looks somewhere other than where your guns are pointing,
        // and every target you are actually engaging sits off to one side of the frame.
        //
        // PlayerTorsoYaw is a TWIST RELATIVE to the body, so the absolute cockpit heading is
        // the sum. This is the single source of truth for camera yaw: the eye position, Pan's
        // frame and the contact facing markers all read it, because three separate copies of
        // "which way is the camera looking" is three chances to drift apart.
        // TacOps uses this same chase camera, torso slew and all - the page is the 3D map with
        // orders on it, not a different map.
        CockpitYawDeg = yaw + torsoRel;
        LastPlayerM[0] = px; LastPlayerM[1] = py; LastPlayerM[2] = pz; // 2026-10-12: the compass's centre
        LastPlayerValid = true;
        const float camYawDeg = yaw + torsoRel + YawOff;
        LastCamYawDeg = camYawDeg;

        const float rangeM = (float)RangeM;       // zoom: how close the camera sits
        const float horizonM = FMax(rangeM, kHorizonM); // draw distance: how much ground exists
        const float focusX = px + PanX, focusY = py + PanY;
        const float camYaw = camYawDeg * 0.01745329252f;
        const float pitch = PitchDeg * 0.01745329252f;

        // The eye sits back along the heading and above it. Distance scales with range so the
        // horizon lands near the edge of the view at every zoom.
        const float dist = rangeM * 0.95f;

        // ---- the look-at point, and why its height is a FRACTION OF RANGE ----------------
        //
        // v1 aimed at Zr(pz) + 15 m, and both halves of that were wrong in a way that only
        // showed up at high zoom.
        //
        // `pz` is the pawn's actor ORIGIN, which floats 6-12 m above the mech's feet (the
        // producer measures exactly this as OriginAboveGroundCm), and the 15 m lift sat on top
        // of it - so the camera aimed roughly 24 render-metres above the thing it was meant to
        // be following. A FIXED world offset against a camera distance that SCALES with zoom
        // means the offset's share of the screen grows as you zoom in:
        //
        //     offset_screen = 24.6 * cos(pitch) / (dist * tan(vfov/2))
        //
        //     1000 m ->  2.4% of screen height     200 m -> 12%
        //      400 m ->  6%                        100 m -> 24%
        //
        // At the default range it is invisible, which is why it shipped; at 100 m the mech is
        // a quarter of the screen below centre. Making the lift proportional to range holds
        // the framing constant at every zoom, and 0.015 * 1000 = 15 m means the default range
        // looks exactly as it did before.
        //
        // Aiming at the GROUND under the focus rather than at the mech's origin fixes a second
        // drift in the same line: with the old code, panning onto a hillside left the look-at
        // point at the mech's altitude, so the terrain slid out of frame as you panned.
        // SMOOTHED, because GroundAtM is a nearest-sample lookup on a 16 m lattice: walking
        // across a slope steps the raw value by up to a couple of metres at a time, and at 100 m
        // range that reads as the camera jolting once per stride. A first-order filter at about
        // 0.12 s removes it without any perceptible lag - the camera is following a mech, not
        // tracking a projectile. A large jump (respawn, new mission, jump jets) snaps instead of
        // easing, so the view never spends a second sliding up out of the ground.
        float focusZ = pz;
        if (haveTerrain)
            GroundAtM(focusX, focusY, focusZ);
        if (!FocusZValid || std::fabs(focusZ - FocusZSmooth) > 50.0f)
        {
            FocusZSmooth = focusZ;
            FocusZValid = true;
        }
        else
        {
            FocusZSmooth += (focusZ - FocusZSmooth) * 0.15f;
        }
        const V3 focus{focusX, focusY, Zr(FocusZSmooth) + rangeM * 0.015f};
        const V3 eye{focus.x - std::cos(camYaw) * dist * std::cos(pitch),
                     focus.y - std::sin(camYaw) * dist * std::cos(pitch), focus.z + dist * std::sin(pitch)};

        const float aspect = (Height > 0) ? ((float)Width / (float)Height) : 1.0f;
        Mat4 view = LookAtLH(eye, focus, V3{0, 0, 1});
        // Far plane follows the HORIZON, not the zoom - otherwise zooming in clips away the
        // very terrain the horizon is there to keep.
        Mat4 proj = PerspectiveLH(50.0f * 0.01745329252f, aspect, 5.0f, horizonM * 4.0f + 500.0f);

        // PILOT CAMERA: the game's own camera, published by VanillaHud every frame - position,
        // rotation and horizontal field of view - so this page is the pilot's view of the world,
        // at true scale, and the game's crosshair and mesh outlines can be laid straight over it.
        if (PilotOn)
        {
            const float yawR = PilotRot[1] * 0.01745329252f, pitR = PilotRot[0] * 0.01745329252f;
            const V3 pe{PilotLoc[0] * 0.01f, PilotLoc[1] * 0.01f, Zr(PilotLoc[2] * 0.01f)};
            const V3 fwd{std::cos(pitR) * std::cos(yawR), std::cos(pitR) * std::sin(yawR), std::sin(pitR)};
            view = LookAtLH(pe, V3{pe.x + fwd.x, pe.y + fwd.y, pe.z + fwd.z}, V3{0, 0, 1});
            const float hf = Clampf(PilotHFovDeg, 5.0f, 170.0f) * 0.01745329252f;
            const float vf = 2.0f * std::atan(std::tan(hf * 0.5f) / aspect);
            proj = PerspectiveLH(vf, aspect, 0.5f, horizonM * 4.0f + 500.0f);
            LastCamYawDeg = PilotRot[1];
        }
        const Mat4 vp = MatMul(view, proj);
        LastVp = vp;
        LastVpValid = true;
        // The eye and its basis, for turning a click back into a ray (TacPickGround).
        TacEye = eye;
        TacF = Norm(Sub(focus, eye));
        TacR = Norm(Cross(V3{0, 0, 1}, TacF));
        TacU = Cross(TacF, TacR);
        TacTanY = std::tan(50.0f * 0.01745329252f * 0.5f);
        TacAspect = aspect;
        TacGroundZ = Zr(FocusZSmooth);
        TacHorizonM = horizonM;
        TacCamValid = !PilotOn;
        PilotVp      = vp;
        PilotVpValid = PilotOn;
        PilotBoxCount = 0;
        if (PilotOn)
            BuildPilotBoxes(scene, vp);
        if (PilotOn && PilotBare)
            return; // CAP: the view is all that was wanted - the zoom camera image is the page

        if (haveTerrain)
        {
            ServiceTiles(focusX, focusY, horizonM);
            DrawTerrain(vp, focusX, focusY, horizonM);
        }
        else
        {
            // No heightfield yet, but the buffer still has to be cleared or the page shows the
            // last thing another view drew.
            if (RTV)
                Ctx->OMSetRenderTargets(1, &RTV, nullptr);
            ClearBackBuffer();
        }

        DrawOverlay(vp, scene, px, py, pz, yaw, camYawDeg, horizonM, haveTerrain);
    }

    void GetDiag(Topo3DView::Diag &d) const
    {
        d.Connected = (Shared != nullptr);
        d.Free = Free;
        d.TilesDrawn = TilesDrawn;
        d.Lines = LinesDrawn;
        d.Contacts = LastContacts;
        d.Projectiles = LastProjectiles;
        d.Silhouettes = SilhouettesDrawn;
        d.Labels = LabelsDrawn;
        d.DollArt = DollsFromArt;
        d.CulledFar = CulledFar;
        d.CulledOffScreen = CulledOffScreen;
        d.NoRecord = NoRecord;
        d.MechContacts = MechContacts;
        d.VehicleContacts = VehicleContacts;
        d.ArtMisses = ArtMisses;
        d.FocusWeapons = FocusWeapons;
        strcpy_s(d.FocusName, FocusName);
        strcpy_s(d.MissingArt, MissingArt);
        d.InArc = InArcDrawn;
        d.MechInfo = (int)Head.MechInfoCount;
        d.LockedId = LockedIdNow;
        d.Trails = LastTrails;
        d.Flashing = LastFlashing;
        d.BuildMs = LastBuildMs;
        d.YawDeg = YawOff;
        d.PitchDeg = PitchDeg;
        d.RangeM = RangeM;
        d.CellM = GetCellM();
    }

    // ==== TacOps ============================================================================
    void SetTacOps(const Topo3DView::TacOpsView *v)
    {
        Tac = v;
        if (!v)
            TacPanelCount = 0;
    }

    void TacRecentre() { Recentre(); }

    void SetNavPoints(const Topo3DView::NavMark *pts, int n)
    {
        NavN = (pts && n > 0) ? (n < 16 ? n : 16) : 0;
        for (int i = 0; i < NavN; ++i)
            NavPts[i] = pts[i];
    }

    int GetDropships(Topo3DView::DropshipMark *out, int max) const
    {
        if (!HeadValid || Head.Version != TERRAIN_VERSION || !out)
            return 0;
        int n = 0;
        for (uint32_t i = 0; i < Head.DropshipCount && i < (uint32_t)TERR_MAX_DROPSHIPS && n < max; ++i)
        {
            const TerrainDropship &d = Head.Dropships[i];
            out[n++] = Topo3DView::DropshipMark{d.X, d.Y, d.Z, d.Yaw, d.EX, d.EY, d.EZ, d.Kind, d.Side};
        }
        return n;
    }

    // DROPSHIPS (2026-10-08): the hull - three height outlines and the centreline profile from the game's mesh
    // (DropshipOutline.h), turned and placed like the actor; an unknown kind is a box of its bounds. Friendly
    // blue, enemy red, unknown grey. The turrets the maps already show then sit on it.
    void DrawDropships(const Mat4 &vp)
    {
        Topo3DView::DropshipMark ds[TERR_MAX_DROPSHIPS];
        const int n = GetDropships(ds, TERR_MAX_DROPSHIPS);
        for (int i = 0; i < n; ++i)
        {
            const Topo3DView::DropshipMark &d = ds[i];
            const uint32_t col = d.Side == 1 ? 0x4DA6FF : (d.Side == 2 ? 0xFF4D4D : 0x9AA3A8);
            float c = std::cos(d.Yaw * 0.0174533f), s = std::sin(d.Yaw * 0.0174533f);
            auto proj = [&](float x, float y, float z, float &sx, float &sy) {
                float dep;
                const float wx = d.X + x * c - y * s, wy = d.Y + x * s + y * c, wz = d.Z + z;
                return Project(vp, wx * 0.01f, wy * 0.01f, wz * 0.01f, sx, sy, dep);
            };
            auto poly = [&](const float *xyz, int count, float a) { // count points, closed
                Col(col, a);
                for (int k = 0; k < count; ++k)
                {
                    const float *p = xyz + 3 * k, *q = xyz + 3 * ((k + 1) % count);
                    float ax, ay, bx, by;
                    if (proj(p[0], p[1], p[2], ax, ay) && proj(q[0], q[1], q[2], bx, by))
                        RT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), Brush, 1.2f);
                }
            };
            float topZ = 0.0f;
            if (const DropshipOutline *o = DropshipOutlineFor(d.Kind))
            {
                std::vector<float> buf;
                for (auto &sl : o->Slices)
                {
                    buf.clear();
                    for (size_t k = 0; k + 1 < sl.Pts.size(); k += 2)
                        buf.insert(buf.end(), {sl.Pts[k], sl.Pts[k + 1], sl.Z});
                    poly(buf.data(), (int)buf.size() / 3, 0.85f);
                    topZ = (std::max)(topZ, sl.Z);
                }
                for (auto &sd : o->Side)
                {
                    buf.clear();
                    for (size_t k = 0; k + 1 < sd.size(); k += 2)
                    {
                        buf.insert(buf.end(), {sd[k], 0.0f, sd[k + 1]});
                        topZ = (std::max)(topZ, sd[k + 1]);
                    }
                    poly(buf.data(), (int)buf.size() / 3, 0.6f);
                }
            }
            else
            {
                // a box around the bounds (X/Y/Z is the bounds' centre here, yaw 0 - the extents are world-aligned)
                const float ex = d.EX, ey = d.EY, ez = d.EZ;
                c = 1.0f;
                s = 0.0f;
                const float bot[12] = {-ex, -ey, -ez, ex, -ey, -ez, ex, ey, -ez, -ex, ey, -ez};
                const float top[12] = {-ex, -ey, ez, ex, -ey, ez, ex, ey, ez, -ex, ey, ez};
                poly(bot, 4, 0.8f);
                poly(top, 4, 0.8f);
                for (int k = 0; k < 4; ++k)
                {
                    const float v[6] = {bot[3 * k], bot[3 * k + 1], bot[3 * k + 2], top[3 * k], top[3 * k + 1], top[3 * k + 2]};
                    poly(v, 2, 0.8f);
                }
                topZ = ez;
            }
            float lx, ly;
            if (proj(0.0f, 0.0f, topZ, lx, ly))
                Label(d.Side == 2 ? L"ENEMY DROPSHIP" : (d.Side == 1 ? L"DROPSHIP" : L"DROPSHIP ?"), lx - 50.0f, ly - 26.0f, 140.0f, col, 0.95f);
        }
    }

    // NAV POINTS (2026-10-07k): a flag on the ground - a stem, a diamond at its foot, the name at its top.
    // NAV POINTS. On screen: a diamond on a stalk with its name. OFF screen - outside the view, behind
    // the camera, or simply far away (2026-10-08: a nav point 2.3 km out is past anything the 3D page
    // shows) - pinned to the edge of the page in its direction, hollow, with its name and distance, so it
    // is never lost. The nav points come from LanceControl, not the 1000 m LIDAR scan: any distance.
    void DrawNav(const Mat4 &vp, bool haveTerrain, float pxM, float pyM)
    {
        const float cl = Width * Inset, ct = Height * Inset, cr = Width * (1.0f - Inset), cb = Height * (1.0f - Inset);
        const float ccx = (cl + cr) * 0.5f, ccy = (ct + cb) * 0.5f, edge = 26.0f;
        for (int i = 0; i < NavN; ++i)
        {
            const Topo3DView::NavMark &n = NavPts[i];
            float wz = n.Loc[2] * 0.01f, g, sx, sy, d;
            if (haveTerrain && GroundAtM(n.Loc[0] * 0.01f, n.Loc[1] * 0.01f, g))
                wz = g;
            const uint32_t col = n.Mine ? 0xE07BFF : 0xFFD54D;
            const bool proj = Project(vp, n.Loc[0] * 0.01f, n.Loc[1] * 0.01f, wz + 1.0f, sx, sy, d);
            const bool inside = proj && sx >= cl + edge && sx <= cr - edge && sy >= ct + edge + 40.0f && sy <= cb - edge;
            if (inside)
            {
                Col(col, 0.95f);
                RT->DrawLine(D2D1::Point2F(sx, sy), D2D1::Point2F(sx, sy - 34.0f), Brush, 1.6f);
                D2D1_POINT_2F dm[4] = {{sx, sy - 7.0f}, {sx + 7.0f, sy}, {sx, sy + 7.0f}, {sx - 7.0f, sy}};
                for (int e = 0; e < 4; ++e)
                    RT->DrawLine(dm[e], dm[(e + 1) & 3], Brush, 1.8f);
                Col(col, 0.25f);
                RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 4.0f, 4.0f), Brush);
                Label(n.Name, sx + 4.0f, sy - 50.0f, 120.0f, col, 1.0f);
                continue;
            }
            // Its direction on the page: from the projection when there is one in front of the camera,
            // else from the camera's ground-plane basis (behind the camera, the projection flips).
            float dx, dy;
            if (proj)
            {
                dx = sx - ccx;
                dy = sy - ccy;
            }
            else
            {
                const float vx = n.Loc[0] * 0.01f - TacEye.x, vy = n.Loc[1] * 0.01f - TacEye.y;
                float fx = TacF.x, fy = TacF.y;
                const float fl = std::sqrt(fx * fx + fy * fy);
                if (fl > 1e-4f) { fx /= fl; fy /= fl; }
                dx = vx * TacR.x + vy * TacR.y;
                dy = -(vx * fx + vy * fy);
            }
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-3f)
                continue;
            dx /= len;
            dy /= len;
            const float hw = (cr - cl) * 0.5f - edge, hh = (cb - ct) * 0.5f - edge;
            const float t = FMin(std::fabs(dx) > 1e-4f ? hw / std::fabs(dx) : 1e9f, std::fabs(dy) > 1e-4f ? hh / std::fabs(dy) : 1e9f);
            const float ex = ccx + dx * t, ey = ccy + dy * t;
            Col(col, 0.95f);
            D2D1_POINT_2F dm[4] = {{ex, ey - 8.0f}, {ex + 8.0f, ey}, {ex, ey + 8.0f}, {ex - 8.0f, ey}};
            for (int e = 0; e < 4; ++e)
                RT->DrawLine(dm[e], dm[(e + 1) & 3], Brush, 1.8f); // hollow: it is off the page
            // a short arrow outward
            RT->DrawLine(D2D1::Point2F(ex + dx * 10.0f, ey + dy * 10.0f), D2D1::Point2F(ex + dx * 18.0f, ey + dy * 18.0f), Brush, 1.8f);
            const float mx = n.Loc[0] * 0.01f - pxM, my = n.Loc[1] * 0.01f - pyM;
            wchar_t txt[48];
            swprintf_s(txt, L"%ls %.1f km", n.Name, std::sqrt(mx * mx + my * my) * 0.001f);
            const float lx = Clampf(ex - 60.0f, cl + 2.0f, cr - 122.0f);
            const float ly = (dy > 0.5f) ? ey - 30.0f : ey + 12.0f;
            Label(txt, lx, ly, 120.0f, col, 1.0f);
        }
    }

    bool TacGroundAt(float xCm, float yCm, float &zCm) const
    {
        float g;
        if (!GroundAtM(xCm * 0.01f, yCm * 0.01f, g))
            return false;
        zCm = g * 100.0f;
        return true;
    }

    // Where a contact is now (its last sighting), by id - for orders that outlive the click.
    bool TacContactLoc(int id, float out[3]) const
    {
        for (int i = 0; i < GhostCount; ++i)
            if (Ghosts[i].Id == id)
            {
                out[0] = Ghosts[i].X; out[1] = Ghosts[i].Y; out[2] = Ghosts[i].Z;
                return !(MechInfoFor(Ghosts[i].Id) ? MechInfoFor(Ghosts[i].Id)->Dead : IsDeadId(Ghosts[i].Id));
            }
        return false;
    }
    bool TacContactActor(int id, uint32_t &lo, uint32_t &hi) const
    {
        const TerrainMechInfo *r = MechInfoFor((uint16_t)id);
        if (!r || r->Dead || (!r->ActorLo && !r->ActorHi))
            return false;
        lo = r->ActorLo;
        hi = r->ActorHi;
        return true;
    }

    void TacPanWorld(float dxM, float dyM)
    {
        PanX += dxM;
        PanY += dyM;
    }

    // Onto the GROUND under the point, exactly where DrawTac puts the lancemate rings, so what
    // the mouse picks and what is drawn agree.
    bool TacProject(const float cm[3], float &sx, float &sy) const
    {
        float d, wz = cm[2] * 0.01f, g;
        if (GroundAtM(cm[0] * 0.01f, cm[1] * 0.01f, g))
            wz = g;
        return LastVpValid && Project(LastVp, cm[0] * 0.01f, cm[1] * 0.01f, wz + 1.0f, sx, sy, d);
    }

    // March the click's ray over the heightfield. 2 m steps near the eye, growing with distance -
    // the lattice is 16 m, so nothing between steps is missed - then bisect to the surface.
    bool TacPickGround(float sx, float sy, float out[3]) const
    {
        if (!TacCamValid || !Width || !Height)
            return false;
        const float vx = Width * Inset, vy = Height * Inset;
        const float vw = Width * (1.0f - 2.0f * Inset), vh = Height * (1.0f - 2.0f * Inset);
        const float nx = ((sx - vx) / vw) * 2.0f - 1.0f, ny = 1.0f - ((sy - vy) / vh) * 2.0f;
        const V3 d = Norm(V3{TacF.x + TacR.x * nx * TacTanY * TacAspect + TacU.x * ny * TacTanY,
                             TacF.y + TacR.y * nx * TacTanY * TacAspect + TacU.y * ny * TacTanY,
                             TacF.z + TacR.z * nx * TacTanY * TacAspect + TacU.z * ny * TacTanY});
        auto at = [&](float t) { return V3{TacEye.x + d.x * t, TacEye.y + d.y * t, TacEye.z + d.z * t}; };
        const float maxT = TacHorizonM * 4.0f + 500.0f;
        float prevT = 0.0f;
        for (float t = 1.0f; t < maxT;)
        {
            const V3 p = at(t);
            float g;
            if (GroundAtM(p.x, p.y, g) && p.z <= Zr(g))
            {
                float lo = prevT, hi = t;
                for (int i = 0; i < 12; ++i)
                {
                    const float mid = 0.5f * (lo + hi);
                    const V3 q = at(mid);
                    float gq;
                    if (GroundAtM(q.x, q.y, gq) && q.z <= Zr(gq))
                        hi = mid;
                    else
                        lo = mid;
                }
                const V3 q = at(hi);
                float gq = g;
                GroundAtM(q.x, q.y, gq);
                out[0] = q.x * 100.0f; out[1] = q.y * 100.0f; out[2] = gq * 100.0f;
                return true;
            }
            prevT = t;
            t += FMax(2.0f, t * 0.004f);
        }
        // No heightfield under the ray: the plane of the ground at the camera's focus.
        if (d.z < -1.0e-4f)
        {
            const float t = (TacGroundZ - TacEye.z) / d.z;
            const V3 q = at(t);
            out[0] = q.x * 100.0f; out[1] = q.y * 100.0f; out[2] = (TacGroundZ / kVertEx) * 100.0f;
            return true;
        }
        return false;
    }

    // 2026-10-12 (COMPASS): the live hostiles the map shows, world cm - for the heading strip's dots.
    int HostilesCm(float (*out)[3], int max) const
    {
        int n = 0;
        for (int i = 0; i < GhostCount && n < max; ++i)
            if (GhostLiveHostile(i) && (!HostileKeep || Ghosts[i].Live)) // Vanilla: no dots for last-known marks
            {
                out[n][0] = Ghosts[i].X; out[n][1] = Ghosts[i].Y; out[n][2] = Ghosts[i].Z;
                ++n;
            }
        return n;
    }
    float CompassHeading() const { return CockpitYawDeg; }
    bool PlayerCm(float out[3]) const
    {
        if (!LastPlayerValid)
            return false;
        out[0] = LastPlayerM[0] * 100.0f; out[1] = LastPlayerM[1] * 100.0f; out[2] = LastPlayerM[2] * 100.0f;
        return true;
    }

    bool GhostLiveHostile(int i) const
    {
        const auto &g = Ghosts[i];
        if (g.Col != kColHostile)
            return false;
        const TerrainMechInfo *rec = MechInfoFor(g.Id);
        return !(rec ? rec->Dead : IsDeadId(g.Id));
    }

    int TacHostileAt(float sx, float sy, float radiusPx, float out[3]) const
    {
        if (!LastVpValid)
            return 0;
        int best = 0;
        float bestD = radiusPx * radiusPx;
        for (int i = 0; i < GhostCount; ++i)
        {
            const Ghost &g = Ghosts[i];
            if (!GhostLiveHostile(i))
                continue;
            float gx, gy, dd;
            if (!Project(LastVp, g.X * 0.01f, g.Y * 0.01f, g.Z * 0.01f, gx, gy, dd))
                continue;
            const float d2 = (gx - sx) * (gx - sx) + (gy - sy) * (gy - sy);
            if (d2 < bestD)
            {
                bestD = d2;
                best = g.Id;
                out[0] = g.X; out[1] = g.Y; out[2] = g.Z;
            }
        }
        return best;
    }

    int TacNearestHostile(const float from[3], float maxM, float out[3]) const
    {
        int best = 0;
        float bestD = maxM * 100.0f * maxM * 100.0f;
        for (int i = 0; i < GhostCount; ++i)
        {
            const Ghost &g = Ghosts[i];
            if (!GhostLiveHostile(i))
                continue;
            const float dx = g.X - from[0], dy = g.Y - from[1];
            const float d2 = dx * dx + dy * dy;
            if (d2 < bestD)
            {
                bestD = d2;
                best = g.Id;
                out[0] = g.X; out[1] = g.Y; out[2] = g.Z;
            }
        }
        return best;
    }

    int TacPanelAt(float sx, float sy) const
    {
        for (int i = 0; i < TacPanelCount; ++i)
        {
            const D2D1_RECT_F &r = TacPanels[i].R;
            if (sx >= r.left && sx <= r.right && sy >= r.top && sy <= r.bottom)
                return TacPanels[i].Slot;
        }
        return 0;
    }

    float TacPanelLeft() const { return Width * (1.0f - Inset) - 8.0f - Topo3DView::kTacPanelW; }

    // A lancemate's identity record: the one LidarExport published for the SAME ACTOR. Both
    // plugins write the unit's address (TacMember / TerrainMechInfo ActorLo/Hi), so the join is
    // exact. It used to be "the friendly contact nearest the lancemate", which handed the player's
    // plate whichever lancemate stood within 30 m and let two close lancemates swap records -
    // a Dragonfly pilot shown as a Catapult. No match is NO DATA, never a guess. The player's own
    // plate never takes a record: the game's own-mech paperdoll is drawn into it instead.
    const TerrainMechInfo *TacInfoFor(const Topo3DView::TacMate &m) const
    {
        if (m.IsPlayer || (!m.ActorLo && !m.ActorHi) || !Head.MechInfoScanned)
            return nullptr;
        for (uint32_t i = 0; i < Head.MechInfoCount && i < (uint32_t)TERR_MAX_MECHS; ++i)
        {
            const TerrainMechInfo &r = Head.Mechs[i];
            if (r.Valid && r.ActorLo == m.ActorLo && r.ActorHi == m.ActorHi)
                return &r;
        }
        return nullptr;
    }

    // The torso arrow as last drawn (window pixels): its origin and tip. The caller makes the tip
    // a grab handle that drags the torso.
    bool  TorsoArrowOk = false;
    float TorsoArrow[4] = {};
    bool TacTorsoArrow(float &ox, float &oy, float &tx, float &ty) const
    {
        if (!Tac || !TorsoArrowOk)
            return false;
        ox = TorsoArrow[0]; oy = TorsoArrow[1]; tx = TorsoArrow[2]; ty = TorsoArrow[3];
        return true;
    }

    bool TacOwnPlate(float &l, float &t, float &r, float &b) const
    {
        if (!Tac || !OwnPlateValid)
            return false;
        l = OwnPlate.left; t = OwnPlate.top; r = OwnPlate.right; b = OwnPlate.bottom;
        return true;
    }

    static constexpr uint32_t kColTacRoute = 0x3FB8FF;

    void DrawTac(const Mat4 &vp, bool haveTerrain)
    {
        const Topo3DView::TacOpsView &v = *Tac;
        auto proj = [&](const float *cm, float &sx, float &sy) -> bool {
            float wz = cm[2] * 0.01f, g;
            if (haveTerrain && GroundAtM(cm[0] * 0.01f, cm[1] * 0.01f, g))
                wz = g;
            float d;
            return Project(vp, cm[0] * 0.01f, cm[1] * 0.01f, wz + 1.0f, sx, sy, d);
        };
        EnsureLeaderStroke();

        // Routes: from the lancemate through every queued move.
        for (int i = 0; i < v.MateCount && i < 8; ++i)
        {
            const Topo3DView::TacMate &m = v.Mates[i];
            if (m.Dead || m.WpCount <= 0)
                continue;
            float ax, ay;
            bool haveA = proj(m.Loc, ax, ay);
            for (int k = 0; k < m.WpCount && k < 8; ++k)
            {
                float bx, by;
                if (!proj(m.Wp[k], bx, by))
                {
                    haveA = false;
                    continue;
                }
                Col(kColTacRoute, m.Selected ? 0.95f : 0.55f);
                if (haveA)
                    RT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), Brush, 1.4f, LeaderStroke);
                D2D1_POINT_2F dm[4] = {{bx, by - 5.0f}, {bx + 5.0f, by}, {bx, by + 5.0f}, {bx - 5.0f, by}};
                for (int e = 0; e < 4; ++e)
                    RT->DrawLine(dm[e], dm[(e + 1) & 3], Brush, 1.4f);
                ax = bx; ay = by; haveA = true;
            }
        }

        // The lancemates: a ring and their order slot.
        for (int i = 0; i < v.MateCount && i < 8; ++i)
        {
            const Topo3DView::TacMate &m = v.Mates[i];
            float sx, sy;
            if (!proj(m.Loc, sx, sy))
                continue;
            const uint32_t col = m.Dead ? 0x5E666B : (m.Selected ? kColSelected : (m.IsPlayer ? kColPlayer : kColFriendly));
            if (m.Selected && !m.Dead)
            {
                Col(kColFriendly, 0.22f);
                RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 13.0f, 13.0f), Brush);
            }
            Col(col, 0.95f);
            RT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 13.0f, 13.0f), Brush, m.Selected ? 2.2f : 1.3f);
            wchar_t n[8];
            if (m.IsPlayer)
                swprintf_s(n, L"YOU");
            else
                swprintf_s(n, L"%u", (unsigned)m.Slot);
            Label(n, sx + 12.0f, sy - 24.0f, 40.0f, col, 1.0f);
        }

        DrawTacPanels();
    }

    // The lance down the right-hand side: one short plate per lancemate - doll on the left, name
    // and weapons (two columns) beside it. Fixed height, so the formation panel fits under them.
    static uint32_t TacWeaponColour(const TerrainMechInfo &m, const TerrainWeapon &wp)
    {
        if (wp.Flags & TWF_Destroyed)
            return 0xFF5A4D;
        if (wp.Part < (uint8_t)TMP_COUNT && m.HaveCondition)
        {
            const float str = m.Structure[wp.Part], arm = m.Armour[wp.Part];
            if (str >= 0.0f && str <= 0.02f) return 0xFF5A4D;
            if (arm >= 0.0f && arm <= 0.02f) return 0xFFD11A;
        }
        return 0xD8E2E8;
    }

    // 2026-10-19 GROUPED WEAPON LINES (TERRAIN v15): identical weapons on one line while they read the same - same
    // name, same colour (TacWeaponColour). LidarExport already folds same name + part + flags into one entry with a
    // Count; here entries from different parts merge while their state matches and split again when one part's
    // state changes (armour stripped -> yellow, part gone -> red). Order = first appearance.
    struct WpnLine
    {
        const char *Name; // the record's char[20] (not always terminated - printed with %.20hs)
        uint32_t    Col;
        int         Count;
    };
    static int WeaponLines(const TerrainMechInfo &m, WpnLine *out, int cap)
    {
        int n = 0;
        for (uint8_t w = 0; w < m.WeaponCount && w < (uint8_t)TERR_MAX_WEAPONS; ++w)
        {
            const TerrainWeapon &wp = m.Weapons[w];
            if (!wp.Name[0])
                continue;
            const uint32_t col = TacWeaponColour(m, wp);
            const int cnt = wp.Count ? wp.Count : 1;
            int j = 0;
            for (; j < n; ++j)
                if (out[j].Col == col && std::strncmp(out[j].Name, wp.Name, sizeof(wp.Name)) == 0)
                    break;
            if (j < n)
                out[j].Count += cnt;
            else if (n < cap)
                out[n++] = WpnLine{wp.Name, col, cnt};
        }
        return n;
    }
    static void WeaponLineText(const WpnLine &l, wchar_t *buf, size_t cap)
    {
        if (l.Count > 1)
            _snwprintf_s(buf, cap, _TRUNCATE, L"%.20hs x%d", l.Name, l.Count);
        else
            _snwprintf_s(buf, cap, _TRUNCATE, L"%.20hs", l.Name);
    }

    // "RETURN · PPC+LAS · COOL" - what differs from the defaults; "PLUGIN" alone when only the
    // plugin firing for it (insane allies) is worth saying.
    static std::wstring TacFireTag(const Topo3DView::TacMate &m)
    {
        static const wchar_t *kCat[8] = {L"LAS", L"PPC", L"BAL", L"LOCK", L"DUMB", L"MG", L"TAG", L"NARC"};
        std::wstring t;
        auto add = [&](const std::wstring &x) { t += (t.empty() ? L"" : L" \x00B7 ") + x; };
        if (m.FsDisc == 1)
            add(L"RETURN");
        else if (m.FsDisc == 2)
            add(L"HOLD");
        if ((m.FsPlugin & 2) && m.FsDisc != 2)
            add(L"HOLD (GAME)"); // the game's own Hold Fire, obeyed by the plugin
        if (m.FsMask != 0xFF)
        {
            int on = 0;
            for (int b = 0; b < 8; ++b)
                on += (m.FsMask >> b) & 1;
            std::wstring c;
            if (on == 0)
                c = L"NO WEAPONS";
            else if (on <= 4)
            {
                for (int b = 0; b < 8; ++b)
                    if ((m.FsMask >> b) & 1)
                        c += (c.empty() ? L"" : L"+") + std::wstring(kCat[b]);
            }
            else
            {
                c = L"NO ";
                bool first = true;
                for (int b = 0; b < 8; ++b)
                    if (!((m.FsMask >> b) & 1))
                    {
                        c += (first ? L"" : L"+") + std::wstring(kCat[b]);
                        first = false;
                    }
            }
            add(c);
        }
        static const wchar_t *kHeat[4] = {L"", L"COOL", L"HOT", L"ALPHA"};
        if (m.FsHeat >= 1 && m.FsHeat <= 3)
            add(kHeat[m.FsHeat]);
        if (t.empty() && (m.FsPlugin & 1))
            add(L"PLUGIN FIRE");
        return t;
    }

    void DrawTacPanels()
    {
        TacPanelCount = 0;
        OwnPlateValid = false;
        const Topo3DView::TacOpsView &v = *Tac;
        const int n = v.MateCount < 8 ? v.MateCount : 8;
        const float x0 = TacPanelLeft(), x1 = x0 + Topo3DView::kTacPanelW;
        const float top0 = Height * Inset + Topo3DView::kTacPanelTop;
        static const wchar_t *kOrder[5] = {L"", L"MOVING", L"ATTACKING", L"FOLLOWING", L"HOLDING"};
        for (int i = 0; i < n; ++i)
        {
            const Topo3DView::TacMate &m = v.Mates[i];
            const float top = top0 + (Topo3DView::kTacPanelH + Topo3DView::kTacPanelGap) * (float)i;
            const D2D1_RECT_F r = D2D1::RectF(x0, top, x1, top + Topo3DView::kTacPanelH);
            TacPanels[TacPanelCount++] = {r, m.Slot};
            const TerrainMechInfo *info = TacInfoFor(m);
            const bool dead = m.Dead || (info && info->Dead);
            const uint32_t col = dead ? 0x5E666B : (m.Selected ? kColSelected : (m.IsPlayer ? kColPlayer : kColFriendly));
            const float a = dead ? 0.45f : 1.0f;

            DrawPlate(r, 3.0f, 0.85f);
            Col(col, m.Selected ? 0.95f : 0.5f);
            RT->DrawRectangle(D2D1::RectF(r.left - 3.0f, r.top - 3.0f, r.right + 3.0f, r.bottom + 3.0f), Brush,
                              m.Selected ? 2.0f : 1.0f);

            wchar_t line[64];
            // 2026-10-19: ONE name line - "2  Callsign  VARIANT" (or "2  VARIANT" for an unnamed pilot); the white
            // variant + tonnage line under it is gone (it doubled the variant). The voice trace sits at its right end,
            // and the order word moved down to the right end of the second line.
            const char *variant = info ? (info->Variant[0] ? info->Variant : info->Chassis) : "";
            const char *callsign = info ? info->Callsign : "";
            const char *lead = m.IsPlayer ? "YOU" : nullptr;
            if (callsign[0] && variant[0])
                lead ? swprintf_s(line, L"%hs  %hs  %hs", lead, callsign, variant)
                     : swprintf_s(line, L"%u  %hs  %hs", (unsigned)m.Slot, callsign, variant);
            else
                lead ? swprintf_s(line, L"%hs  %hs", lead, callsign[0] ? callsign : variant)
                     : swprintf_s(line, L"%u  %hs", (unsigned)m.Slot, callsign[0] ? callsign : variant);
            const bool wave = m.HaveVoice && !m.IsPlayer;
            const float waveW = 80.0f;
            Label(line, r.left + 2.0f, r.top - 1.0f, Topo3DView::kTacPanelW - (wave ? waveW + 12.0f : 4.0f), col, 1.0f);
            if (wave)
                DrawVoiceWave(m.Slot, m.Speaking != 0, m.WaveLo, m.WaveHi, r.right - waveW - 4.0f, r.top + 1.0f, waveW, 13.0f,
                              3.0f);
            const int ord = (m.Order >= 0 && m.Order < 5) ? m.Order : 0;
            const wchar_t *state = dead ? L"DESTROYED"
                                        : (m.WpCount > 0 ? (m.IsPlayer ? L"AUTOPILOT" : L"ROUTE") : kOrder[ord]);
            // 2026-10-19: the order word starts the second line, left-aligned with the weapons under it (x = the
            // weapons' tx: doll 10 * 1.9 + 12 px from the plate's left).
            const float ox = r.left + 10.0f * 1.9f + 12.0f;
            if (!dead && !m.IsPlayer && m.OrderText[0])
            {
                // 2026-10-19: the live order (feed), "{T}" = its target by name (inside 1000 m), else "TARGET".
                std::wstring t = m.OrderText;
                if (t.find(L"{T}") != std::wstring::npos)
                {
                    std::wstring tname = L"TARGET";
                    if (const TerrainMechInfo *ti = InfoForActor(m.TargetLo, m.TargetHi))
                    {
                        const char *tn = ti->Variant[0] ? ti->Variant : ti->Chassis;
                        if (tn[0])
                        {
                            wchar_t tb[48];
                            swprintf_s(tb, L"%hs", tn);
                            tname = tb;
                        }
                    }
                    for (size_t p = t.find(L"{T}"); p != std::wstring::npos; p = t.find(L"{T}", p + tname.size()))
                        t.replace(p, 3, tname);
                }
                LabelSm(t.c_str(), ox, r.top + 16.0f, r.right - ox - 4.0f, m.OrderCol, 0.95f);
            }
            else if (state[0])
                LabelSm(state, ox, r.top + 16.0f, 90.0f, dead ? 0xFF5A4D : kColTacRoute, 0.95f);

            RT->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            if (info)
            {
                const float scale = 1.9f;
                const float dollW = 10.0f * scale;
                const float dtop = r.top + 17.0f;
                DrawDoll(r.left + 4.0f + dollW * 0.5f, dtop, scale, *info, a);
                DrawRearStrip(r.left + 4.0f + dollW * 0.5f, dtop + 16.0f * scale + 2.0f * scale, dollW, scale, *info, a);

                const float tx = r.left + dollW + 12.0f;
                float y = r.top + 16.0f;
                y += 12.0f; // the second line holds the order word (right end); the weapons start under it
                const float colW = (r.right - tx) * 0.5f;
                int k = 0;
                // Fire settings: one amber line at the foot of the plate, only when they are not the
                // defaults (or the plugin fires for it anyway: insane allies / the indirect hold).
                std::wstring ftag;
                if (!m.IsPlayer)
                    ftag = TacFireTag(m);
                const float listBottom = ftag.empty() ? r.bottom : r.bottom - 11.0f;
                if (!ftag.empty())
                    LabelSm(ftag.c_str(), tx, r.bottom - 12.0f, r.right - tx, 0xFFC04D, a);
                const int rows = (int)((listBottom - y) / 11.0f);
                WpnLine wl[TERR_MAX_WEAPONS];
                const int nl = WeaponLines(*info, wl, TERR_MAX_WEAPONS);
                for (int w = 0; w < nl; ++w)
                {
                    if (rows <= 0 || k >= rows * 2)
                        break;
                    WeaponLineText(wl[w], line, _countof(line));
                    LabelSm(line, tx + colW * (float)(k / rows), y + 11.0f * (float)(k % rows), colW - 2.0f,
                            wl[w].Col, a * 0.95f);
                    ++k;
                }
            }
            else if (m.IsPlayer && v.OwnImage)
            {
                OwnPlate = D2D1::RectF(r.left + 2.0f, r.top + 16.0f, r.right - 2.0f, r.bottom - 2.0f);
                OwnPlateValid = true;
            }
            else
                Label(L"NO DATA", r.left + 2.0f, r.top + 18.0f, 120.0f, 0x8FB8C9, 0.8f);
            RT->PopAxisAlignedClip();
        }
    }

    // =====================================================================================
    // V-LNC - THE LANCE PAGE (2026-10-19, user): the TacOps lancemate plates at page size, one row
    // per lancemate, plus ammo and what each one is doing now. Drawn on its own - no terrain, no
    // camera - straight into the window's back buffer, like the 3D page's 2D pass.
    // =====================================================================================
    IDWriteTextFormat *FmtLnc[3] = {}; // 30 px bold (names, orders), 21 px (detail), 17 px (lists)
    void LanceFonts()
    {
        static const float kPx[3] = {30.0f, 21.0f, 17.0f};
        for (int i = 0; i < 3; ++i)
            if (!FmtLnc[i] && DW)
                DW->CreateTextFormat(L"Consolas", nullptr, i == 0 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, kPx[i], L"en-us", &FmtLnc[i]);
    }

    // The LidarExport record of one actor (the same exact join TacInfoFor makes).
    const TerrainMechInfo *InfoForActor(uint32_t lo, uint32_t hi) const
    {
        if ((!lo && !hi) || !Head.MechInfoScanned)
            return nullptr;
        for (uint32_t i = 0; i < Head.MechInfoCount && i < (uint32_t)TERR_MAX_MECHS; ++i)
        {
            const TerrainMechInfo &r = Head.Mechs[i];
            if (r.Valid && r.ActorLo == lo && r.ActorHi == hi)
                return &r;
        }
        return nullptr;
    }

    // The radio trace (V-LNC): a PEAK WAVEFORM, as an audio editor's overview - one thin vertical line per 11 ms
    // column from its lowest to its highest sample, 1.6 s scrolling right to left. While the pilot talks it is
    // bright; when it stops, the last trace fades out over half a second; at rest only the centre line shows.
    // The voice trace (V-LNC row, TacOps plate). 2026-10-19 PLAYS OUT FLAT: when a lancemate stops talking the
    // ring stops advancing in VoiceCommand, so the trace is scrolled on here at the playback rate (one column per
    // 11 ms) with silence coming in from the right until the speech has left the box. The highlight goes at once.
    ULONGLONG VoiceQuietMs[9] = {}; // when each slot was last seen stop talking (0 = long ago)
    uint8_t   VoiceWasOn[9] = {};
    void DrawVoiceWave(int slot, bool speaking, const int8_t *wlo, const int8_t *whi, float wx, float wy, float ww,
                       float wh, float rad)
    {
        slot = (slot >= 0 && slot < 9) ? slot : 0;
        const ULONGLONG now = GetTickCount64();
        if (VoiceWasOn[slot] && !speaking)
            VoiceQuietMs[slot] = now;
        VoiceWasOn[slot] = speaking ? 1 : 0;
        const int n = 144;
        const float shift = speaking ? 0.0f
                          : (VoiceQuietMs[slot] ? (float)(now - VoiceQuietMs[slot]) / 11.0f : (float)n); // columns
        const float in = rad > 3.5f ? 3.0f : 2.0f;
        const float mid = wy + wh * 0.5f, amp = wh * 0.5f - in;
        // 2026-10-19: no box (frame or fill) - just the trace on the plate. While talking the centre line lights
        // up instead of the old green frame.
        (void)rad;
        Col(speaking ? 0x55FF8A : 0x2E5A40, speaking ? 0.7f : 0.8f); // the centre line (silence)
        RT->DrawLine(D2D1::Point2F(wx + in, mid), D2D1::Point2F(wx + ww - in, mid), Brush, 1.0f);
        if (shift >= (float)n)
            return; // played out: flat
        // A narrow box gets fewer, wider columns (each the min / max of the ones it covers).
        const float span = ww - 2.0f * in;
        const int m = span >= 216.0f ? n : (span / 1.5f < 8.0f ? 8 : (int)(span / 1.5f));
        const float per = (float)n / (float)m;    // source columns per drawn column
        const float dx = span / (float)m;
        const float lw = dx > 2.0f ? dx - 0.6f : (dx > 1.0f ? dx : 1.0f);
        const float x0 = wx + in - (shift / per) * dx; // scrolled left by the silence since
        Col(0x5DF0A8, 0.95f); // the editor's sea-green
        for (int j = 0; j < m; ++j)
        {
            const float x = x0 + dx * ((float)j + 0.5f);
            if (x < wx + in)
                continue;
            int k0 = (int)((float)j * per), k1 = (int)((float)(j + 1) * per);
            if (k1 <= k0)
                k1 = k0 + 1;
            int lo = 127, hi = -128;
            for (int k = k0; k < k1 && k < n; ++k)
            {
                lo = wlo[k] < lo ? wlo[k] : lo;
                hi = whi[k] > hi ? whi[k] : hi;
            }
            const float flo = (float)lo / 127.0f, fhi = (float)hi / 127.0f;
            if (fhi - flo < 0.004f)
                continue;
            RT->DrawLine(D2D1::Point2F(x, mid - fhi * amp), D2D1::Point2F(x, mid - flo * amp + 0.5f), Brush, lw);
        }
    }
    void DrawVoiceTrace(const Topo3DView::LanceCard &c, float wx, float wy, float ww, float wh)
    {
        DrawVoiceWave(c.Slot, c.Speaking != 0, c.WaveLo, c.WaveHi, wx, wy, ww, wh, 4.0f);
    }

    void RenderLancePage(const Topo3DView::LancePageView &v)
    {
        if (!RT || !Brush)
            return;
        ReadHeader();
        LanceFonts();
        IDWriteTextFormat *fB = FmtLnc[0] ? FmtLnc[0] : TextFmt;
        IDWriteTextFormat *fM = FmtLnc[1] ? FmtLnc[1] : TextFmt;
        IDWriteTextFormat *fS = FmtLnc[2] ? FmtLnc[2] : TextFmt;

        RT->BeginDraw();
        RT->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        Mono = false;
        const float L = Width * Inset, T = Height * Inset, R = Width * (1.0f - Inset), B = Height * (1.0f - Inset);
        const float gap = 16.0f;
        const float rowH = (B - T - gap * 2.0f) / 3.0f;
        if (v.Count <= 0)
            LabelFmt(fM, L"NO LANCEMATES - waiting for the game", L + 10.0f, T + 10.0f, R - L - 20.0f, kColText, 0.8f);

        for (int i = 0; i < v.Count && i < 3 && rowH > 60.0f; ++i)
        {
            const Topo3DView::LanceCard &c = v.Cards[i];
            const float top = T + (rowH + gap) * (float)i;
            const D2D1_RECT_F r = D2D1::RectF(L, top, R, top + rowH);
            const TerrainMechInfo *info = InfoForActor(c.ActorLo, c.ActorHi);
            const bool dead = c.Dead || (info && info->Dead);
            const uint32_t col = dead ? 0x5E666B : kColFriendly;
            const float a = dead ? 0.45f : 1.0f;

            DrawPlate(r, 4.0f, 0.85f);
            Col(col, 0.55f);
            RT->DrawRectangle(D2D1::RectF(r.left - 4.0f, r.top - 4.0f, r.right + 4.0f, r.bottom + 4.0f), Brush, 1.5f);
            RT->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);

            // Header: number and name; the mech on the right.
            wchar_t line[128];
            const char *rec = info ? (info->Callsign[0] ? info->Callsign : (info->Variant[0] ? info->Variant : info->Chassis)) : "";
            if (c.Name[0])
                swprintf_s(line, L"%u  %ls", (unsigned)c.Slot, c.Name);
            else
                swprintf_s(line, L"%u  %hs", (unsigned)c.Slot, rec);
            LabelFmt(fB, line, r.left + 10.0f, r.top + 6.0f, (r.right - r.left) * 0.55f, col, 1.0f);
            if (info && (info->Variant[0] || info->Chassis[0]))
            {
                if (info->Tons > 0.5f)
                    swprintf_s(line, L"%hs  %.0f t", info->Variant[0] ? info->Variant : info->Chassis, info->Tons);
                else
                    swprintf_s(line, L"%hs", info->Variant[0] ? info->Variant : info->Chassis);
                LabelFmt(fM, line, r.left + (r.right - r.left) * 0.55f, r.top + 12.0f, (r.right - r.left) * 0.45f - 10.0f,
                         0xB9C4CC, a);
            }

            // The radio: a waveform beside the name, lit while it talks; "WAITING TO SPEAK" when it has lines queued.
            if (c.HaveVoice && !dead)
            {
                DrawVoiceTrace(c, r.left + (r.right - r.left) * 0.36f, r.top + 6.0f, (r.right - r.left) * 0.18f, 34.0f);
                const float wx = r.left + (r.right - r.left) * 0.36f, wy = r.top + 6.0f, ww = (r.right - r.left) * 0.18f;
                if (!c.Speaking && c.Queued)
                    LabelFmt(fS, L"WAITING TO SPEAK", wx + 6.0f, wy + 6.0f, ww - 8.0f, 0xFFC04D, 0.95f);
            }

            // The doll and its rear strip, as big as the row allows.
            const float bodyTop = r.top + 50.0f;
            float scale = (r.bottom - bodyTop - 12.0f) / 20.5f;
            scale = scale < 2.0f ? 2.0f : (scale > 14.0f ? 14.0f : scale);
            const float dollW = 10.0f * scale;
            const float dcx = r.left + 16.0f + dollW * 0.5f;
            if (info)
            {
                DrawDoll(dcx, bodyTop, scale, *info, a);
                DrawRearStrip(dcx, bodyTop + 16.0f * scale + 2.0f * scale, dollW, scale, *info, a);
            }
            else
                LabelFmt(fM, L"NO DATA", r.left + 16.0f, bodyTop + 10.0f, dollW + 40.0f, 0x8FB8C9, 0.8f);

            // What it is doing, the detail, fire settings and heat.
            const float x0 = r.left + 40.0f + dollW;
            const float tw = r.right - x0 - 12.0f;
            float y = bodyTop;
            // "{T}" = the target's name, from its LidarExport record (inside 1000 m), else "TARGET".
            std::wstring tname = L"TARGET";
            if (const TerrainMechInfo *ti = InfoForActor(c.TargetLo, c.TargetHi))
            {
                const char *tn = ti->Variant[0] ? ti->Variant : ti->Chassis;
                if (tn[0])
                {
                    wchar_t tb[48];
                    swprintf_s(tb, L"%hs", tn);
                    tname = tb;
                }
            }
            auto sub = [&](const wchar_t *src) {
                std::wstring t = src;
                for (size_t p = t.find(L"{T}"); p != std::wstring::npos; p = t.find(L"{T}", p + tname.size()))
                    t.replace(p, 3, tname);
                return t;
            };
            LabelFmt(fB, dead ? L"DESTROYED" : (c.Order[0] ? sub(c.Order).c_str() : L"-"), x0, y, tw,
                     dead ? 0xFF5A4D : c.OrderCol, a);
            y += 38.0f;
            if (!dead && c.Order2[0])
                LabelFmt(fM, sub(c.Order2).c_str(), x0, y, tw, 0xB9C4CC, a);
            y += 28.0f;
            if (!dead && c.Pending[0])
            {
                // A question waiting on your answer: an amber bar - it holds this lancemate up.
                const D2D1_RECT_F pb = D2D1::RectF(x0 - 4.0f, y - 2.0f, x0 + tw, y + 26.0f);
                Col(0x3A2A05, 0.95f);
                RT->FillRectangle(pb, Brush);
                Col(0xFFC04D, 0.95f);
                RT->DrawRectangle(pb, Brush, 1.5f);
                std::wstring q = L"AWAITING YOUR ANSWER - ";
                q += c.Pending;
                LabelFmt(fM, q.c_str(), x0 + 2.0f, y, tw - 6.0f, 0xFFC04D, 1.0f);
                y += 32.0f;
            }
            if (!dead)
            {
                float hx = x0;
                if (c.Heat >= 0.0f)
                {
                    const float h = c.Heat > 1.5f ? 1.5f : c.Heat;
                    swprintf_s(line, L"HEAT %3.0f %%", h * 100.0f);
                    LabelFmt(fM, line, hx, y, 170.0f, h >= 0.9f ? 0xFF5A4D : (h >= 0.7f ? 0xFFC04D : 0xD8E2E8), a);
                    hx += 180.0f;
                }
                Topo3DView::TacMate fm{};
                fm.FsDisc = c.FsDisc;
                fm.FsMask = c.FsMask;
                fm.FsHeat = c.FsHeat;
                fm.FsPlugin = c.FsPlugin;
                const std::wstring ftag = TacFireTag(fm);
                LabelFmt(fM, ftag.empty() ? L"WEAPONS FREE" : ftag.c_str(), hx, y, x0 + tw - hx,
                         ftag.empty() ? 0x8FB8C9 : 0xFFC04D, a);
            }
            y += 34.0f;

            // Weapons (two columns) and the ammo by type beside them.
            const float lineH = 20.0f;
            const float ammoX = x0 + tw * 0.64f;
            const int rows = (int)((r.bottom - 8.0f - y) / lineH);
            if (info && rows > 0)
            {
                const float colW = (ammoX - x0 - 12.0f) * 0.5f;
                int k = 0;
                WpnLine wl[TERR_MAX_WEAPONS];
                const int nl = WeaponLines(*info, wl, TERR_MAX_WEAPONS);
                for (int w = 0; w < nl; ++w)
                {
                    if (k >= rows * 2)
                        break;
                    WeaponLineText(wl[w], line, _countof(line));
                    LabelFmt(fS, line, x0 + colW * (float)(k / rows), y + lineH * (float)(k % rows), colW - 4.0f,
                             wl[w].Col, a * 0.95f);
                    ++k;
                }
            }
            if (rows > 0 && c.HaveFeed)
            {
                LabelFmt(fS, c.AmmoCount > 0 ? L"AMMO" : L"AMMO  none (energy)", ammoX, y, tw * 0.36f, 0x8FB8C9, a);
                for (int k = 0; k < c.AmmoCount && k + 1 < rows; ++k)
                {
                    const Topo3DView::LanceAmmo &am = c.Ammo[k];
                    const float f = am.Max > 0 ? (float)am.Count / (float)am.Max : 1.0f;
                    swprintf_s(line, L"%-10.10ls %4d / %d", am.Name, am.Count, am.Max);
                    LabelFmt(fS, am.Count <= 0 ? L"" : line, ammoX, y + lineH * (float)(k + 1), tw * 0.36f,
                             f <= 0.25f ? 0xFFC04D : 0xD8E2E8, a);
                    if (am.Count <= 0)
                    {
                        swprintf_s(line, L"%-10.10ls EMPTY", am.Name);
                        LabelFmt(fS, line, ammoX, y + lineH * (float)(k + 1), tw * 0.36f, 0xFF5A4D, a);
                    }
                }
            }
            RT->PopAxisAlignedClip();
        }

        const HRESULT hr = RT->EndDraw();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET)
        {
            SafeRelease(Brush);
            RT = nullptr;
        }
    }

    void Shutdown()
    {
        DropAllTiles();
        ReleaseArt();
        SafeRelease(ChevronStroke);
        SafeRelease(LeaderStroke);
        SafeRelease(Brush);
        SafeRelease(TextFmt);
        SafeRelease(TextFmtSm);
        for (auto *&f : FmtLnc) SafeRelease(f); // V-LNC
        SafeRelease(BsOpaque);
        SafeRelease(BsNoColor);
        SafeRelease(BsAlpha);
        SafeRelease(DsOn);
        SafeRelease(RsLine);
        SafeRelease(RsFill);
        SafeRelease(CBuf);
        SafeRelease(Layout);
        SafeRelease(PS);
        SafeRelease(VS);
        SafeRelease(DSV);
        SafeRelease(DepthTex);
        SafeRelease(DSVAlt);
        SafeRelease(DepthTexAlt);
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
    struct Tile
    {
        bool Used = false;
        bool Dirty = false;
        int32_t TileX = 0, TileY = 0;
        int Stride = 0;
        ID3D11Buffer *VB = nullptr;
        ID3D11Buffer *IBFill = nullptr;
        ID3D11Buffer *IBLine = nullptr;
        UINT FillIdx = 0, LineIdx = 0;
        double LastSeenMs = 0.0;
    };
    Tile TilesSet[2][kMaxTiles]; // bug sweep 21: [0] chase (1.2), [1] pilot (1:1)
    Tile *Tiles = TilesSet[0];

    void DropTileGpu(Tile &t)
    {
        SafeRelease(t.VB);
        SafeRelease(t.IBFill);
        SafeRelease(t.IBLine);
        t.FillIdx = t.LineIdx = 0;
    }
    void DropAllTiles()
    {
        for (auto &set : TilesSet) // bug sweep 21: both caches
            for (int i = 0; i < kMaxTiles; ++i)
            {
                DropTileGpu(set[i]);
                set[i] = Tile();
            }
    }
    int FindTile(int32_t tx, int32_t ty) const
    {
        for (int i = 0; i < kMaxTiles; ++i)
            if (Tiles[i].Used && Tiles[i].TileX == tx && Tiles[i].TileY == ty)
                return i;
        return -1;
    }
    int AcquireTile(double now)
    {
        int lru = -1;
        double t = 0.0;
        for (int i = 0; i < kMaxTiles; ++i)
        {
            if (!Tiles[i].Used)
                return i;
            if (lru < 0 || Tiles[i].LastSeenMs < t)
            {
                lru = i;
                t = Tiles[i].LastSeenMs;
            }
        }
        if (lru >= 0 && now - Tiles[lru].LastSeenMs > 250.0)
        {
            DropTileGpu(Tiles[lru]);
            Tiles[lru] = Tile();
            return lru;
        }
        return -1;
    }

    // ---- shared memory ---------------------------------------------------------------------

    bool ReadHeader()
    {
        if (!Shared)
            return false;
        for (int a = 0; a < 8; ++a)
        {
            const uint32_t before = *(volatile const uint32_t *)&Shared->H.FrameCounterEnd;
            memcpy(&Scratch, &Shared->H, sizeof(TerrainHeader));
            MemoryBarrier();
            const uint32_t after = *(volatile const uint32_t *)&Shared->H.FrameCounter;
            if (before == after)
            {
                memcpy(&Head, &Scratch, sizeof(TerrainHeader));
                HeadValid = true;
                MemoClear(); // the memo holds slot indices; a fresh table invalidates them
                break;
            }
        }
        return HeadValid && Head.Version == TERRAIN_VERSION && Head.SamplesPerSide == TERR_SAMPLES;
    }

    // Four-entry memo in front of a linear scan of 512 slots.
    //
    // This is called once per VERTEX while building a tile, and at 16 m cells that is 1,089
    // vertices against 512 slots - half a million comparisons per tile, twice a frame. A tile
    // build only ever touches four distinct tiles (itself plus the east, north and north-east
    // neighbours it reaches into for the seam), so a tiny memo removes essentially all of it.
    mutable int32_t MemoX[4] = {0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};
    mutable int32_t MemoY[4] = {0, 0, 0, 0};
    mutable int MemoSlot[4] = {-1, -1, -1, -1};
    mutable int MemoNext = 0;

    void MemoClear() const
    {
        for (int i = 0; i < 4; ++i) { MemoX[i] = 0x7FFFFFFF; MemoSlot[i] = -1; }
        MemoNext = 0;
    }

    // A tile the producer is CURRENTLY re-tracing. It exists and its old geometry was fine -
    // it is simply being resampled because something that was standing on it (a dropship, a
    // building) has gone. Distinguishing this from "no such tile" is the whole fix below.
    bool TileRefilling(int32_t tx, int32_t ty) const
    {
        const uint32_t n = (Head.TileCount < (uint32_t)TERR_MAX_TILES) ? Head.TileCount
                                                                      : (uint32_t)TERR_MAX_TILES;
        for (uint32_t i = 0; i < n; ++i)
        {
            const TerrainSlotInfo &s2 = Head.Slots[i];
            if (s2.TileX == tx && s2.TileY == ty && s2.State == TTS_Filling)
                return true;
        }
        return false;
    }

    int SlotForTile(int32_t tx, int32_t ty) const
    {
        for (int i = 0; i < 4; ++i)
            if (MemoX[i] == tx && MemoY[i] == ty)
                return MemoSlot[i];

        const uint32_t n = (Head.TileCount < (uint32_t)TERR_MAX_TILES) ? Head.TileCount : (uint32_t)TERR_MAX_TILES;
        int found = -1;
        for (uint32_t i = 0; i < n; ++i)
        {
            const TerrainSlotInfo &s = Head.Slots[i];
            if (s.State == TTS_Complete && s.Hits > 0 && s.TileX == tx && s.TileY == ty) { found = (int)i; break; }
        }
        MemoX[MemoNext] = tx;
        MemoY[MemoNext] = ty;
        MemoSlot[MemoNext] = found;
        MemoNext = (MemoNext + 1) & 3;
        return found;
    }

    bool SampleAt(int32_t gx, int32_t gy, float &zCm) const
    {
        const int32_t tx = FloorDiv(gx, TERR_SAMPLES), ty = FloorDiv(gy, TERR_SAMPLES);
        const int slot = SlotForTile(tx, ty);
        if (slot < 0)
            return false;
        const float z = Shared->Tiles[slot].Height[(gy - ty * TERR_SAMPLES) * TERR_SAMPLES + (gx - tx * TERR_SAMPLES)];
        if (IsHole(z))
            return false;
        zCm = z;
        return true;
    }

    // Terrain height under an arbitrary world point, for contact stalks. Nearest sample rather
    // than bilinear: a stalk is a visual cue, not a measurement, and 64 m of interpolation error
    // in the foot of a marker is invisible next to the marker itself.
    bool GroundAtM(float wxM, float wyM, float &zM) const
    {
        if (!HeadValid)
            return false;
        const float sp = TERR_SPACING_CM * 0.01f;
        const int32_t gx = (int32_t)std::floor(wxM / sp);
        const int32_t gy = (int32_t)std::floor(wyM / sp);
        float zcm;
        if (!SampleAt(gx, gy, zcm))
            return false;
        zM = zcm * 0.01f;
        return true;
    }

    // ---- tile building ----------------------------------------------------------------------

    void ServiceTiles(float focusX, float focusY, float horizonM)
    {
        const double now = NowMs();
        const float tileM = TERR_TILE_CM * 0.01f;
        const float reach = horizonM * 1.25f + tileM;
        const int32_t x0 = (int32_t)std::floor((focusX - reach) / tileM), x1 = (int32_t)std::floor((focusX + reach) / tileM);
        const int32_t y0 = (int32_t)std::floor((focusY - reach) / tileM), y1 = (int32_t)std::floor((focusY + reach) / tileM);

        for (int32_t ty = y0; ty <= y1; ++ty)
            for (int32_t tx = x0; tx <= x1; ++tx)
            {
                if (SlotForTile(tx, ty) < 0 || FindTile(tx, ty) >= 0)
                    continue;
                const int b = AcquireTile(now);
                if (b < 0)
                    continue;
                Tiles[b].Used = true;
                Tiles[b].Dirty = true;
                Tiles[b].TileX = tx;
                Tiles[b].TileY = ty;
                Tiles[b].LastSeenMs = now;
                // Seams: this tile's mesh reaches one step east and north, so the neighbours that
                // own the seams INTO it have to be rebuilt as well. Same rule as the 2D page.
                const int w = FindTile(tx - 1, ty);
                if (w >= 0) Tiles[w].Dirty = true;
                const int sy = FindTile(tx, ty - 1);
                if (sy >= 0) Tiles[sy].Dirty = true;
            }

        // Elevation range, widened only, quantised so the shader's colour ramp settles instead of
        // breathing. Sampled sparsely - a 50 m band does not care about the exact extremes.
        const uint32_t n = (Head.TileCount < (uint32_t)TERR_MAX_TILES) ? Head.TileCount : (uint32_t)TERR_MAX_TILES;
        float lo = 1.0e30f, hi = -1.0e30f;
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
        if (lo <= hi)
        {
            const float loM = std::floor(lo * 0.01f / 50.0f) * 50.0f;
            const float hiM = std::ceil(hi * 0.01f / 50.0f) * 50.0f;
            if (!HaveRange) { MinZm = loM; MaxZm = hiM; HaveRange = true; }
            else { MinZm = FMin(MinZm, loM); MaxZm = FMax(MaxZm, hiM); }
        }

        int done = 0;
        while (done < kBuildsPerFrame)
        {
            int pick = -1;
            double seen = 0.0;
            for (int i = 0; i < kMaxTiles; ++i)
            {
                if (!Tiles[i].Used || !Tiles[i].Dirty)
                    continue;
                if (pick < 0 || Tiles[i].LastSeenMs > seen) { pick = i; seen = Tiles[i].LastSeenMs; }
            }
            if (pick < 0)
                break;
            const double t0 = NowMs();
            BuildTile(Tiles[pick]);
            LastBuildMs = (float)(NowMs() - t0);
            ++done;
        }
    }

    void BuildTile(Tile &t)
    {
        // A REBUILD DURING A RE-TRACE IS WHAT PUNCHED THE HOLES.
        //
        // SlotForTile only accepts TTS_Complete, so while the producer re-traces a tile every
        // sample inside it fails and reads as a hole. Rebuilding then bakes that absence into
        // the mesh: one 512 m square vanishes, and because a tile's last row and column sample
        // ACROSS into its neighbours, the tiles beside it lose their seam edge too. The result
        // is a straight-sided, see-through trench on ground that is perfectly flat - which is
        // exactly what a re-traced dropship pad looks like from the air.
        //
        // The old geometry is not stale in any meaningful sense: terrain does not move, and the
        // only reason for the re-trace is that something standing ON it left. So keep drawing
        // what we have and rebuild when the tile comes back Complete.
        if (TileRefilling(t.TileX, t.TileY) ||
            TileRefilling(t.TileX + 1, t.TileY) ||
            TileRefilling(t.TileX, t.TileY + 1) ||
            TileRefilling(t.TileX + 1, t.TileY + 1))
        {
            t.Dirty = true; // try again once the producer is done
            return;
        }

        DropTileGpu(t);
        t.Dirty = false;
        t.Stride = Stride;

        const int st = Stride;
        const int cells = TERR_SAMPLES / st;       // 8 at 64 m
        const int side = cells + 1;                // 9 verts a side
        const int32_t gx0 = t.TileX * TERR_SAMPLES, gy0 = t.TileY * TERR_SAMPLES;
        const float sp = TERR_SPACING_CM * 0.01f;

        static float verts[(kMaxCells + 1) * (kMaxCells + 1) * 3];
        static uint16_t idxFill[kMaxCells * kMaxCells * 6];
        static uint16_t idxLine[(kMaxCells + 1) * kMaxCells * 4];
        static bool ok[(kMaxCells + 1) * (kMaxCells + 1)];

        for (int r = 0; r < side; ++r)
            for (int c = 0; c < side; ++c)
            {
                const int32_t gx = gx0 + c * st, gy = gy0 + r * st;
                float zcm;
                const bool got = SampleAt(gx, gy, zcm);
                const int vi = r * side + c;
                ok[vi] = got;
                // + half a spacing: the producer traces SAMPLE CENTRES, and dropping the
                // half-cell slides the whole mesh 8 m off the tile grid and off everything else.
                verts[vi * 3 + 0] = ((float)gx + 0.5f) * sp;
                verts[vi * 3 + 1] = ((float)gy + 0.5f) * sp;
                verts[vi * 3 + 2] = got ? Zr(zcm * 0.01f) : 0.0f;
            }

        UINT nf = 0, nl = 0;
        for (int r = 0; r < cells; ++r)
            for (int c = 0; c < cells; ++c)
            {
                const int a = r * side + c, b = a + 1, d = a + side, e = d + 1;
                // Holes are dropped, never clamped: a boundary tile is legitimately half empty,
                // and one unguarded quad would tent down to ten thousand kilometres below the map.
                if (!ok[a] || !ok[b] || !ok[d] || !ok[e])
                    continue;
                idxFill[nf++] = (uint16_t)a; idxFill[nf++] = (uint16_t)d; idxFill[nf++] = (uint16_t)b;
                idxFill[nf++] = (uint16_t)b; idxFill[nf++] = (uint16_t)d; idxFill[nf++] = (uint16_t)e;
            }
        // A vertex owns the segments reaching east and north from it - exactly the ownership rule
        // the 2D page uses, so no seam is ever drawn twice.
        for (int r = 0; r < side; ++r)
            for (int c = 0; c < side; ++c)
            {
                const int a = r * side + c;
                if (!ok[a])
                    continue;
                if (c + 1 < side && ok[a + 1])    { idxLine[nl++] = (uint16_t)a; idxLine[nl++] = (uint16_t)(a + 1); }
                if (r + 1 < side && ok[a + side]) { idxLine[nl++] = (uint16_t)a; idxLine[nl++] = (uint16_t)(a + side); }
            }
        if (nf == 0 && nl == 0)
            return;

        D3D11_BUFFER_DESC bd{};
        D3D11_SUBRESOURCE_DATA sd{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;

        bd.ByteWidth = (UINT)(side * side * 3 * sizeof(float));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sd.pSysMem = verts;
        if (FAILED(Dev->CreateBuffer(&bd, &sd, &t.VB))) { t.Dirty = true; return; }

        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        if (nf) { bd.ByteWidth = nf * sizeof(uint16_t); sd.pSysMem = idxFill;
                  if (SUCCEEDED(Dev->CreateBuffer(&bd, &sd, &t.IBFill))) t.FillIdx = nf; }
        if (nl) { bd.ByteWidth = nl * sizeof(uint16_t); sd.pSysMem = idxLine;
                  if (SUCCEEDED(Dev->CreateBuffer(&bd, &sd, &t.IBLine))) t.LineIdx = nl; }

        // The slot can be recycled under us by eviction while we read heights, so re-check.
        //
        // The previous version indexed Slots[0] whenever the lookup failed - a DIFFERENT tile,
        // whose state says nothing about this one - and then threw away buffers on the strength
        // of it. Now a failed lookup is treated as what it is (the tile moved or went back to
        // Filling) and the geometry is KEPT rather than dropped: a mesh built from heights that
        // were correct a moment ago beats a hole in the ground.
        const int nowSlot = SlotForTile(t.TileX, t.TileY);
        if (nowSlot < 0 || Shared->H.Slots[nowSlot].State != TTS_Complete)
            t.Dirty = true;
    }

    // The colour buffer has to be cleared, and this page was the only one not doing it.
    //
    // Every other view clears implicitly - the scope and the 2D map through D2D's Clear, the
    // widget pages by drawing a full-screen textured quad. This one draws terrain into a
    // viewport and left whatever was already in the back buffer showing through wherever the
    // terrain did not reach. Two separate-looking bugs came out of that single omission:
    //
    //   * leftovers from whatever page was up before survived until terrain happened to sweep
    //     over them, which orbiting does and sitting still does not; and
    //   * panning forward past the terrain's leading edge exposed bare buffer, so the previous
    //     frame's terrain stayed on screen underneath the new one - the map "doubling up on
    //     itself", which is really one frame of ghosting per frame of travel.
    //
    // Both stop here. Worth noting the second symptom only appeared when panning FORWARD,
    // because that is the only direction that puts the camera beyond the drawn ground.
    void ClearBackBuffer()
    {
        if (!RTV)
            return;
        const D2D1_COLOR_F bg = RGB24(kColBackground);
        const float c[4] = {bg.r, bg.g, bg.b, 1.0f};
        const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Ctx->ClearRenderTargetView(RTV, Transparent ? none : c);
    }

    void RebuildDepth()
    {
        SafeRelease(DSV);
        SafeRelease(DepthTex);
        if (!Dev || Width == 0 || Height == 0)
            return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = Width;
        td.Height = Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (SUCCEEDED(Dev->CreateTexture2D(&td, nullptr, &DepthTex)))
            Dev->CreateDepthStencilView(DepthTex, nullptr, &DSV);
    }

    void SetCB(const Mat4 &vp, float focusX, float focusY, float horizonM, bool fillPass)
    {
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(Ctx->Map(CBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
            return;
        CB cb{};
        cb.ViewProj = vp;
        const float span = FMax(1.0f, MaxZm - MinZm);
        cb.Elev[0] = Zr(MinZm);
        cb.Elev[1] = 1.0f / (span * kVertEx);
        cb.Elev[2] = horizonM * kFadeStartFrac;
        cb.Elev[3] = horizonM;
        cb.FocusXY[0] = focusX;
        cb.FocusXY[1] = focusY;
        if (fillPass)
        {
            // Background colour, fully opaque, no ramp: these triangles exist only to occlude.
            const D2D1_COLOR_F bg = RGB24(kColBackground);
            for (int i = 0; i < 2; ++i)
            {
                float *c = i ? cb.ColHigh : cb.ColLow;
                c[0] = bg.r; c[1] = bg.g; c[2] = bg.b; c[3] = 1.0f;
            }
            cb.Elev[2] = 1.0e9f; // never fade the occluders, or hills go see-through at range
            cb.Elev[3] = 1.0e9f + 1.0f;
        }
        else
        {
            cb.ColLow[0] = 0.04f; cb.ColLow[1] = 0.26f; cb.ColLow[2] = 0.11f; cb.ColLow[3] = 0.55f;
            cb.ColHigh[0] = 0.42f; cb.ColHigh[1] = 1.00f; cb.ColHigh[2] = 0.52f; cb.ColHigh[3] = 1.0f;
        }
        memcpy(ms.pData, &cb, sizeof(cb));
        Ctx->Unmap(CBuf, 0);
    }

    void DrawTerrain(const Mat4 &vp, float focusX, float focusY, float horizonM)
    {
        if (!Ready || !RTV || !DSV)
            return;

        Ctx->OMSetRenderTargets(1, &RTV, DSV);
        ClearBackBuffer();
        Ctx->ClearDepthStencilView(DSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
        Ctx->OMSetDepthStencilState(DsOn, 0);

        D3D11_VIEWPORT vpd{};
        vpd.TopLeftX = Width * Inset;
        vpd.TopLeftY = Height * Inset;
        vpd.Width = Width * (1.0f - 2.0f * Inset);
        vpd.Height = Height * (1.0f - 2.0f * Inset);
        vpd.MaxDepth = 1.0f;
        Ctx->RSSetViewports(1, &vpd);

        Ctx->IASetInputLayout(Layout);
        Ctx->VSSetShader(VS, nullptr, 0);
        Ctx->PSSetShader(PS, nullptr, 0);
        Ctx->VSSetConstantBuffers(0, 1, &CBuf);

        const UINT stride = 3 * sizeof(float), off = 0;
        const double now = NowMs();
        const float tileM = TERR_TILE_CM * 0.01f;
        const float reach = horizonM + tileM;

        // Pass 1: the occluders.
        SetCB(vp, focusX, focusY, horizonM, true);
        Ctx->OMSetBlendState(Transparent && BsNoColor ? BsNoColor : BsOpaque, nullptr, 0xFFFFFFFF);
        Ctx->RSSetState(RsFill);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (int i = 0; i < kMaxTiles; ++i)
        {
            Tile &t = Tiles[i];
            if (!t.Used || !t.VB || !t.IBFill || !t.FillIdx)
                continue;
            const float cxm = ((float)t.TileX + 0.5f) * tileM - focusX;
            const float cym = ((float)t.TileY + 0.5f) * tileM - focusY;
            if (std::sqrt(cxm * cxm + cym * cym) > reach + tileM)
                continue;
            t.LastSeenMs = now;
            Ctx->IASetVertexBuffers(0, 1, &t.VB, &stride, &off);
            Ctx->IASetIndexBuffer(t.IBFill, DXGI_FORMAT_R16_UINT, 0);
            Ctx->DrawIndexed(t.FillIdx, 0, 0);
        }

        // Pass 2: the wireframe the pilot actually sees.
        SetCB(vp, focusX, focusY, horizonM, false);
        Ctx->OMSetBlendState(BsAlpha, nullptr, 0xFFFFFFFF);
        Ctx->RSSetState(RsLine);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        for (int i = 0; i < kMaxTiles; ++i)
        {
            Tile &t = Tiles[i];
            if (!t.Used || !t.VB || !t.IBLine || !t.LineIdx)
                continue;
            const float cxm = ((float)t.TileX + 0.5f) * tileM - focusX;
            const float cym = ((float)t.TileY + 0.5f) * tileM - focusY;
            if (std::sqrt(cxm * cxm + cym * cym) > reach + tileM)
                continue;
            ++TilesDrawn;
            LinesDrawn += t.LineIdx / 2;
            Ctx->IASetVertexBuffers(0, 1, &t.VB, &stride, &off);
            Ctx->IASetIndexBuffer(t.IBLine, DXGI_FORMAT_R16_UINT, 0);
            Ctx->DrawIndexed(t.LineIdx, 0, 0);
        }

        // Hand the pipeline back the way the host expects to find it: no depth buffer bound, a
        // full-window viewport, default raster state. The widget pages and the bezel both assume
        // that, and leaving a DSV bound would silently depth-test someone else's quad.
        Ctx->OMSetRenderTargets(1, &RTV, nullptr);
        Ctx->OMSetDepthStencilState(nullptr, 0);
        Ctx->RSSetState(nullptr);
        Ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        D3D11_VIEWPORT full{};
        full.Width = (float)Width;
        full.Height = (float)Height;
        full.MaxDepth = 1.0f;
        Ctx->RSSetViewports(1, &full);
    }

    // ---- D2D overlay -------------------------------------------------------------------------

    // Takes WORLD metres and applies the vertical exaggeration itself. Deliberately: with six
    // call sites - sweep, aim ray, rounds, contacts, stalks, player - leaving it to each of them
    // means one will eventually be missed, and that layer would then float above or sink into
    // the ground by a fifth of its altitude while looking like a data fault.
    bool Project(const Mat4 &vp, float wx, float wy, float wzWorld, float &sx, float &sy, float &depth) const
    {
        const float wz = Zr(wzWorld);
        const float cw = wx * vp.m[0][3] + wy * vp.m[1][3] + wz * vp.m[2][3] + vp.m[3][3];
        if (cw <= 0.05f)
            return false; // behind the eye
        const float cx = wx * vp.m[0][0] + wy * vp.m[1][0] + wz * vp.m[2][0] + vp.m[3][0];
        const float cy = wx * vp.m[0][1] + wy * vp.m[1][1] + wz * vp.m[2][1] + vp.m[3][1];
        const float vx = Width * Inset, vy = Height * Inset;
        const float vw = Width * (1.0f - 2.0f * Inset), vh = Height * (1.0f - 2.0f * Inset);
        sx = vx + vw * 0.5f * (1.0f + cx / cw);
        sy = vy + vh * 0.5f * (1.0f - cy / cw);
        depth = cw;
        return true;
    }

    void Col(uint32_t c, float a)
    {
        if (Mono)
            c = 0x55FF8A; // the pilot view is black and green, nothing else
        Brush->SetColor(RGB24(c));
        Brush->SetOpacity(a);
    }

    void Label(const wchar_t *s, float x, float y, float w, uint32_t c, float a)
    {
        LabelFmt(TextFmt, s, x, y, w, c, a);
    }

    // Contact-label size; falls back to the page font if the small format failed to create.
    void LabelSm(const wchar_t *s, float x, float y, float w, uint32_t c, float a)
    {
        LabelFmt(TextFmtSm ? TextFmtSm : TextFmt, s, x, y, w, c, a);
    }

    void LabelFmt(IDWriteTextFormat *fmt, const wchar_t *s, float x, float y, float w, uint32_t c, float a)
    {
        if (!fmt || !DW || !s || !*s)
            return;
        IDWriteTextLayout *l = nullptr;
        if (FAILED(DW->CreateTextLayout(s, (UINT32)wcslen(s), fmt, w, 22.0f, &l)) || !l)
            return;
        Col(c, a);
        RT->DrawTextLayout(D2D1::Point2F(x, y), l, Brush);
        l->Release();
    }

    // ---- mech silhouettes ------------------------------------------------------------------
    //
    // Joints arrive in world centimetres from the producer, already posed, so this only projects
    // points and joins them. The bone list is a contract enum, so the segment table below is the
    // only place that knows what a mech looks like.
    //
    // Drawn in the contact's own colour so a silhouette reads as the same object the triangle
    // was, not as a new kind of thing. Thicker on the legs than the arms, because at range the
    // stance is what carries recognisable information - a Locust's long stride against an
    // Atlas's planted one - while the arms are mostly noise.
    void DrawSilhouettes(const Mat4 &vp, float px, float py, float horizonM)
    {
        if (!Head.SilhouettesOk || Head.SilhouetteCount == 0)
            return;

        static const int kBones[][2] = {
            {TJ_Pelvis, TJ_ThighL}, {TJ_ThighL, TJ_KneeL}, {TJ_KneeL, TJ_CalfL}, {TJ_CalfL, TJ_FootL},
            {TJ_Pelvis, TJ_ThighR}, {TJ_ThighR, TJ_KneeR}, {TJ_KneeR, TJ_CalfR}, {TJ_CalfR, TJ_FootR},
            {TJ_Pelvis, TJ_TorsoCentre}, {TJ_TorsoCentre, TJ_TorsoHead},
            {TJ_TorsoCentre, TJ_TorsoL}, {TJ_TorsoCentre, TJ_TorsoR},
            {TJ_TorsoL, TJ_UpperarmL}, {TJ_TorsoR, TJ_UpperarmR},
        };
        const int kLegBones = 8; // the first eight; drawn heavier

        for (uint32_t i = 0; i < Head.SilhouetteCount && i < (uint32_t)TERR_MAX_SILHOUETTES; ++i)
        {
            const TerrainSilhouette &s = Head.Silhouettes[i];
            if (!s.Valid)
                continue;
            // RADAR MODE (2026-10-12): Vanilla hides the skeleton of a hostile the game does not detect.
            if (s.Hostile && HostileKeep && !HostileKeep(HostileCtx, s.X[TJ_Pelvis], s.Y[TJ_Pelvis], s.Z[TJ_Pelvis]))
                continue;

            const float dx = s.X[TJ_Pelvis] * 0.01f - px, dy = s.Y[TJ_Pelvis] * 0.01f - py;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > horizonM)
                continue;

            float jx[TJ_COUNT], jy[TJ_COUNT];
            bool  ok[TJ_COUNT];
            for (int j = 0; j < TJ_COUNT; ++j)
            {
                float d;
                ok[j] = Project(vp, s.X[j] * 0.01f, s.Y[j] * 0.01f, s.Z[j] * 0.01f, jx[j], jy[j], d);
            }

            const uint32_t col = s.Hostile ? kColHostile : kColFriendly;
            const int n = (int)(sizeof(kBones) / sizeof(kBones[0]));
            for (int b = 0; b < n; ++b)
            {
                const int a0 = kBones[b][0], a1 = kBones[b][1];
                if (!ok[a0] || !ok[a1])
                    continue;
                Col(col, (b < kLegBones) ? 0.95f : 0.70f);
                RT->DrawLine(D2D1::Point2F(jx[a0], jy[a0]), D2D1::Point2F(jx[a1], jy[a1]), Brush,
                             (b < kLegBones) ? 2.0f : 1.4f);
            }
        }
        SilhouettesDrawn = (int)Head.SilhouetteCount;
    }

    // `yaw` is the BODY heading and `camYawDeg` is where the camera looks (body + torso twist +
    // the pilot's orbit). Both are needed and they are not interchangeable: the legs marker is
    // drawn along the body, while anything that has to be rotated into the camera's frame -
    // ====================================================================================
    // MECH LABELS - chevron, damage doll, and the text beside it
    //
    // A contact used to be a small closed triangle. Three edges of similar length give the eye
    // no axis, so at the size these draw at, facing became guesswork - which is the one thing a
    // contact marker has to get across. So the marker is an open V: one nose, two swept arms,
    // elongated along the heading. It still reads as a direction when it is eight pixels tall,
    // and deliberately does NOT close, because the closing edge is what made it a blob.
    //
    // Above it sits a borderless eight-part doll, coloured per part, and beside that the text.
    // No paperdoll border: a rectangle around every contact turns a battlefield into a grid of
    // boxes, and the parts are recognisable as a mech on their own.
    //
    // The chevron is drawn at the contact's TRUE WORLD Z, which the existing loop already
    // projects, so a jump-jetting mech visibly leaves the ground and its stalk stretches.
    // ====================================================================================

    // Joined by id, never by position - see the note on LidarSceneContact::Id.
    const TerrainMechInfo *MechInfoFor(uint16_t id) const
    {
        if (!Head.MechInfoScanned || id == 0)
            return nullptr;
        for (uint32_t i = 0; i < Head.MechInfoCount && i < (uint32_t)TERR_MAX_MECHS; ++i)
            if (Head.Mechs[i].Valid && Head.Mechs[i].Id == id)
                return &Head.Mechs[i];
        return nullptr;
    }

    // Inside the fire arc? Bearing from the mech against the TORSO heading (not the camera,
    // which the pilot can orbit off it). Positive off-arc angles are to the right: UE is X
    // forward, Y right, so yaw grows clockwise seen from above.
    float ArcOffDeg(float wx, float wy) const
    {
        float off = std::atan2(wy - ArcPY, wx - ArcPX) * 57.29577951f - ArcTorsoDeg;
        while (off > 180.0f) off -= 360.0f;
        while (off < -180.0f) off += 360.0f;
        return off;
    }
    void RefreshDeadIds()
    {
        const double now = NowMs();
        int keep = 0;
        for (int i = 0; i < DeadCount; ++i)
        {
            const TerrainMechInfo *r = MechInfoFor(DeadIds[i].Id);
            if ((r && !r->Dead) || now - DeadIds[i].SeenMs > 30000.0)
                continue;
            DeadIds[keep++] = DeadIds[i];
        }
        DeadCount = keep;
        if (!Head.MechInfoScanned)
            return;
        for (uint32_t k = 0; k < Head.MechInfoCount && k < (uint32_t)TERR_MAX_MECHS; ++k)
        {
            const TerrainMechInfo &r = Head.Mechs[k];
            if (!r.Valid || !r.Dead || r.Id == 0)
                continue;
            bool have = false;
            for (int i = 0; i < DeadCount && !have; ++i)
                have = (DeadIds[i].Id == r.Id);
            if (!have && DeadCount < TERR_MAX_MECHS)
                DeadIds[DeadCount++] = DeadMark{r.Id, now};
        }
    }
    bool IsDeadId(uint16_t id) const
    {
        for (int i = 0; i < DeadCount; ++i)
            if (DeadIds[i].Id == id) return true;
        return false;
    }

  public:
    // THE CLICKED CONTACT. Only it gets a paperdoll (and its full name); a click picks it from
    // the labels drawn in the last frame.
    uint16_t ShownId = 0;
    double TakeArtMs() // ART BUDGET (2026-10-13): paperdoll PNG decode ms since the last call
    {
        const double ms = ArtLoadMs;
        ArtLoadMs = 0.0;
        return ms;
    }
    int PickContact(float sx, float sy, float radiusPx) const
    {
        for (int k = PlateCount - 1; k >= 0; --k) // the plates, topmost first
            if (sx >= PlateRect[k].left && sx <= PlateRect[k].right && sy >= PlateRect[k].top && sy <= PlateRect[k].bottom)
                return PlateId[k];
        int best = 0;
        float bd = radiusPx * radiusPx;
        for (int i = 0; i < CandCount; ++i)
        {
            const LabelCand &q = Cands[i];
            if (!q.Info)
                continue;
            const float d2 = (q.X - sx) * (q.X - sx) + (q.Y - sy) * (q.Y - sy);
            if (d2 <= bd)
            {
                bd = d2;
                best = q.Info->Id;
            }
        }
        return best;
    }

  private:

    // An open V. Two strokes from the nose, round caps so the nose reads as a point rather than
    // a notch - a single closed path would need a geometry sink for what is two lines.
    void Chevron(float sx, float sy, float aRad, float r, uint32_t col, float alpha, float thick)
    {
        const float fx = std::sin(aRad), fy = -std::cos(aRad);
        const float rx = std::cos(aRad), ry = std::sin(aRad);
        const float kNose = 2.10f, kTail = 0.95f, kSpan = 0.80f;

        if (!ChevronStroke && Factory)
        {
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
            sp.startCap = D2D1_CAP_STYLE_ROUND;
            sp.endCap   = D2D1_CAP_STYLE_ROUND;
            sp.lineJoin = D2D1_LINE_JOIN_MITER;
            Factory->CreateStrokeStyle(sp, nullptr, 0, &ChevronStroke);
        }

        const D2D1_POINT_2F nose = D2D1::Point2F(sx + fx * r * kNose, sy + fy * r * kNose);
        const D2D1_POINT_2F left = D2D1::Point2F(sx - fx * r * kTail + rx * r * kSpan,
                                                 sy - fy * r * kTail + ry * r * kSpan);
        const D2D1_POINT_2F right = D2D1::Point2F(sx - fx * r * kTail - rx * r * kSpan,
                                                  sy - fy * r * kTail - ry * r * kSpan);
        Col(col, alpha);
        RT->DrawLine(left, nose, Brush, thick, ChevronStroke);
        RT->DrawLine(right, nose, Brush, thick, ChevronStroke);
    }

    // A battle armour suit (2026-10-17, user): a small filled diamond, as on the LIDAR scope - facing means nothing at
    // this size. r is the chevron size; the diamond's half-width is 1.6 r.
    void Diamond(float sx, float sy, float r, uint32_t col, float alpha)
    {
        const float h = 1.6f * r;
        Col(col, alpha);
        for (float k = 0.0f; k <= h; k += 0.75f) // filled with horizontal strokes - no geometry sink needed
        {
            const float w = h - k;
            RT->DrawLine(D2D1::Point2F(sx - w, sy - k), D2D1::Point2F(sx + w, sy - k), Brush, 1.0f);
            RT->DrawLine(D2D1::Point2F(sx - w, sy + k), D2D1::Point2F(sx + w, sy + k), Brush, 1.0f);
        }
    }

    // ---- the doll -----------------------------------------------------------------------
    // Eight parts in a 10 x 16 local box. Laid out as the game's own paperdoll does it, so a
    // pilot reading this has nothing new to learn.
    struct DollPart
    {
        int   Part;                 // TerrainMechPart
        float X, Y, W, H;           // in the 10 x 16 box, origin top-left
    };

    static const DollPart *DollLayout(int &n)
    {
        static const DollPart parts[] = {
            { TMP_Head,        4.0f, 0.0f,  2.0f, 3.0f },
            { TMP_LeftArm,     0.0f, 3.5f,  1.9f, 6.5f },
            { TMP_LeftTorso,   2.3f, 3.5f,  1.7f, 5.5f },
            { TMP_CentreTorso, 4.0f, 3.5f,  2.0f, 5.5f },
            { TMP_RightTorso,  6.0f, 3.5f,  1.7f, 5.5f },
            { TMP_RightArm,    8.1f, 3.5f,  1.9f, 6.5f },
            { TMP_LeftLeg,     3.2f, 9.5f,  1.6f, 6.5f },
            { TMP_RightLeg,    5.2f, 9.5f,  1.6f, 6.5f },
        };
        n = (int)(sizeof(parts) / sizeof(parts[0]));
        return parts;
    }

    // Armour drives the colour; structure decides whether the part is there at all. A part whose
    // structure has gone is drawn HOLLOW rather than red, because red still reads as "damaged but
    // present" and a missing side torso is a different tactical fact entirely.
    // A CONTINUOUS RAMP, NOT THREE BUCKETS.
    //
    // Three bands throw away most of what the number says: a part at 67% and one at 100% drew
    // identically, and a part losing its last third changed colour once. MWVR lerps the colour on
    // each component from the percentage (LerpColorOnComponent), so a part visibly darkens as it
    // is worn down and you can see which of two damaged parts is worse without reading a figure.
    //
    // The three anchors are kept - healthy teal, worn amber, stripped red - so the display still
    // reads the same at a glance; only the steps between them are filled in.
    static uint32_t Lerp24(uint32_t a, uint32_t b, float t)
    {
        t = Clampf(t, 0.0f, 1.0f);
        const float ar = (float)((a >> 16) & 0xFF), ag = (float)((a >> 8) & 0xFF), ab = (float)(a & 0xFF);
        const float br = (float)((b >> 16) & 0xFF), bg = (float)((b >> 8) & 0xFF), bb = (float)(b & 0xFF);
        const uint32_t r = (uint32_t)(ar + (br - ar) * t + 0.5f);
        const uint32_t g = (uint32_t)(ag + (bg - ag) * t + 0.5f);
        const uint32_t bl = (uint32_t)(ab + (bb - ab) * t + 0.5f);
        return (r << 16) | (g << 8) | bl;
    }

    static uint32_t PartColour(float armour)
    {
        const uint32_t kStripped = 0xFF5A4D, kWorn = 0xFFD11A, kHealthy = 0x2C8D7E;
        const float f = Clampf(armour, 0.0f, 1.0f);
        return (f >= 0.5f) ? Lerp24(kWorn, kHealthy, (f - 0.5f) * 2.0f)
                           : Lerp24(kStripped, kWorn, f * 2.0f);
    }

    // ====================================================================================
    // PER-CHASSIS PAPERDOLL ART
    //
    // The eight coloured rectangles say which part is hurt. They do not say what is standing
    // there - a Locust and an Atlas get the same doll, which throws away the one piece of
    // information a pilot most wants at a glance.
    //
    // The game already owns the answer. Every chassis ships its own paperdoll art in the paks:
    // 128x128 DXT5 masks, one per part, in two layers (armour over internal), authored as line
    // work on black so the game can TINT them by damage state. tools_paperdoll_extract.py pulls
    // those out to greyscale PNGs - 144 chassis including mod mechs - and this loads them.
    //
    // WHY THE PARTS COMPOSITE WITHOUT LAYOUT DATA. Each part's texture is a full-size canvas
    // with that part drawn in its true position. So every part draws into the SAME rectangle
    // and the mech assembles itself. There is no per-chassis offset table to get wrong, which
    // is the main reason this is cheap rather than fiddly.
    //
    // WHY A8 AND FillOpacityMask. The masks carry intensity, not colour - the colour is ours to
    // choose from armour and structure. Loading them as an alpha-only bitmap and filling through
    // a solid brush is exactly that operation, and it reuses the colour rules the rectangles
    // already use rather than inventing a second set.
    //
    // It degrades: a chassis with no extracted art falls back to the rectangles, so an unknown
    // mod mech still draws something honest.
    // ====================================================================================

    // ONE SLOT PER CONTACT THE CONTRACT CAN CARRY, NOT AN ARBITRARY GUESS.
    //
    // This was 32, on the reasoning that "a mission fields far fewer". One mission fielded 24
    // distinct chassis, and misses are cached too - an unresolved key burns a slot exactly like a
    // real one - so the margin was far thinner than the number suggests. The producer's own cap
    // on live components was 96 and a mech turned out to have 273; I do not get to guess this.
    // TERR_MAX_MECHS is what the contract can publish, so it is the only defensible ceiling.
    static constexpr int kArtSlots  = TERR_MAX_MECHS;
    static constexpr int kArtLayers = 2;   // 0 = internal, 1 = armour

    struct DollArt
    {
        char          Chassis[32];
        ID2D1Bitmap  *Layer[TMP_COUNT][kArtLayers];
        float         W, H;      // pixel size of the art, for aspect
        bool          Tried;     // a miss is cached too - one failed folder probe is enough
        bool          Any;       // at least one part loaded
        // ART BUDGET (2026-10-13): a mech's 16 PNGs load a couple per frame, not all in one frame (one new chassis
        // was a 90-150 ms Enhanced Imaging frame); ArtFor returns it once Done.
        bool          Done;
        int           Next;        // next part * kArtLayers + layer to load
        char          Folder[64];
    };

    DollArt  Art[kArtSlots] = {};
    int      ArtCount   = 0;
    // ART BUDGET: at most kArtLoadsPerWindow PNG decodes per 16 ms window, shared by every chassis / vehicle
    static constexpr int kArtLoadsPerWindow = 2;
    ULONGLONG ArtWinMs    = 0;
    int       ArtWinLoads = 0;
    double    ArtLoadMs   = 0.0;
    bool ArtBudget()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - ArtWinMs >= 16)
        {
            ArtWinMs = now;
            ArtWinLoads = 0;
        }
        return ArtWinLoads < kArtLoadsPerWindow;
    }
    ID2D1Bitmap *TimedLoad(const wchar_t *path)
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        ID2D1Bitmap *bmp = LoadMask(path);
        QueryPerformanceCounter(&b);
        ArtLoadMs += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
        ++ArtWinLoads;
        return bmp;
    }
    void ContinueArt(DollArt &a)
    {
        static const char *layerName[kArtLayers] = { "internal", "armor" };
        const wchar_t *root = ResolveArtRoot();
        while (!a.Done && root && ArtBudget())
        {
            const int p = a.Next / kArtLayers, L = a.Next % kArtLayers;
            wchar_t path[MAX_PATH];
            _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\%hs\\%hs_%hs.png", root, a.Folder, ArtPartName(p), layerName[L]);
            a.Layer[p][L] = TimedLoad(path);
            if (a.Layer[p][L])
            {
                a.Any = true;
                const D2D1_SIZE_F s = a.Layer[p][L]->GetSize();
                a.W = s.width;
                a.H = s.height;
            }
            if (++a.Next >= TMP_COUNT * kArtLayers)
                a.Done = true;
        }
    }
    bool     ArtRootTried = false;
    wchar_t  ArtRoot[MAX_PATH] = L"";
    IWICImagingFactory *WIC = nullptr;

    // An art folder name is an asset-path token: lowercase letters, digits and underscore, never a
    // space. So the lookup key is reduced to exactly that set.
    //
    // This only matters on the FALLBACK path - when the producer could not supply ArtKey and the
    // display name is used instead. That fallback is why the failures looked so selective: a
    // one-word chassis like "Catapult" still matched its folder by coincidence, while "Black
    // Knight" and "Shadow Hawk" became "black knight" and "shadow hawk" and matched nothing, so
    // they drew the default doll. The producer-side cause is logged separately; this makes the
    // fallback correct rather than lucky. Underscores survive, so srm_carrier and turret_generic
    // are untouched.
    static void ArtKeyNorm(const char *in, char *out, size_t cap)
    {
        size_t k = 0;
        for (size_t i = 0; in && in[i] && k + 1 < cap; ++i)
        {
            char c = in[i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
                out[k++] = c;
        }
        out[k] = '\0';
    }

    // The exe runs out of the build tree, so the art folder is not beside it. Rather than bake a
    // path or add a config key nobody would remember to set, walk up from the exe looking for it.
    // PAPERDOLL CACHE (2026-10-17): the masks now come from the player's own game files, extracted by GlobalSettings >
    // PAPERDOLLS into %LOCALAPPDATA%\MW5 Tactical Command Suite\paperdolls - that folder is used first. A new
    // extraction (source.txt rewritten) drops every loaded mask, so the next frames load the new set; checked every 5 s.
    // The old search (a paperdolls\ folder beside / above the exe) stays as the developer fallback only - the release
    // ships no art. No art at all: the dolls and vehicles are drawn as plain shapes (DrawDoll).
    ULONGLONG ArtStamp = 0, ArtStampCheckMs = 0;
    void ArtWatch()
    {
        const ULONGLONG now = GetTickCount64();
        if (ArtStampCheckMs && now - ArtStampCheckMs < 5000)
            return;
        ArtStampCheckMs = now;
        const ULONGLONG st = PaperdollCacheStamp(PaperdollCacheDir());
        if (st != ArtStamp)
        {
            const bool first = ArtStamp == 0 && !ArtRootTried;
            ArtStamp = st;
            if (!first)
                ReleaseArt(); // a new set (or the first one, extracted while we run): load afresh
        }
    }

    const wchar_t *ResolveArtRoot()
    {
        if (ArtRootTried)
            return ArtRoot[0] ? ArtRoot : nullptr;
        ArtRootTried = true;
        {
            const std::wstring cache = PaperdollCacheDir();
            if (PaperdollCacheStamp(cache))
            {
                wcscpy_s(ArtRoot, cache.c_str());
                return ArtRoot;
            }
        }
        // A host that does not live beside the art says where it is (EnhancedImaging.dll, 2026-10-08).
        if (!AssetDirOverride().empty())
        {
            wchar_t probe[MAX_PATH];
            swprintf_s(probe, L"%ls\\paperdolls", AssetDirOverride().c_str());
            const DWORD a = GetFileAttributesW(probe);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            {
                wcscpy_s(ArtRoot, probe);
                return ArtRoot;
            }
        }

        wchar_t exe[MAX_PATH] = L"";
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
            return nullptr;
        if (wchar_t *slash = wcsrchr(exe, L'\\'))
            *slash = L'\0';

        for (int up = 0; up < 6; ++up)
        {
            wchar_t probe[MAX_PATH];
            swprintf_s(probe, L"%s\\paperdolls", exe);
            const DWORD a = GetFileAttributesW(probe);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            {
                wcscpy_s(ArtRoot, probe);
                return ArtRoot;
            }
            wchar_t *slash = wcsrchr(exe, L'\\');
            if (!slash)
                break;
            *slash = L'\0';
        }
        return nullptr;
    }

    // One greyscale PNG -> one alpha-only D2D bitmap. Returns null on any failure, which is a
    // normal outcome: most chassis have no rear or vulnerability art and nobody should care.
    ID2D1Bitmap *LoadMask(const wchar_t *path)
    {
        if (!RT || !WIC)
            return nullptr;

        IWICBitmapDecoder     *dec   = nullptr;
        IWICBitmapFrameDecode *frame = nullptr;
        IWICFormatConverter   *conv  = nullptr;
        ID2D1Bitmap           *bmp   = nullptr;

        if (SUCCEEDED(WIC->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) &&
            SUCCEEDED(WIC->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat8bppGray,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom)))
        {
            UINT w = 0, h = 0;
            if (SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 512 && h <= 512)
            {
                static BYTE pix[512 * 512];
                if (SUCCEEDED(conv->CopyPixels(nullptr, w, w * h, pix)))
                {
                    // The mask's INTENSITY becomes the bitmap's ALPHA - that is the whole trick.
                    // Bright line work becomes opaque, the black field becomes transparent, and
                    // FillOpacityMask then paints our colour through it.
                    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_STRAIGHT));
                    if (FAILED(RT->CreateBitmap(D2D1::SizeU(w, h), pix, w, bp, &bmp)))
                        bmp = nullptr;
                }
            }
        }
        SafeRelease(conv);
        SafeRelease(frame);
        SafeRelease(dec);
        return bmp;
    }

    static const char *ArtPartName(int part)
    {
        switch (part)
        {
        case TMP_Head:        return "head";
        case TMP_CentreTorso: return "ct";
        case TMP_LeftTorso:   return "lt";
        case TMP_RightTorso:  return "rt";
        case TMP_LeftArm:     return "la";
        case TMP_RightArm:    return "ra";
        case TMP_LeftLeg:     return "ll";
        default:              return "rl";
        }
    }

    const DollArt *ArtFor(const char *chassisRaw)
    {
        ArtWatch();
        if (!chassisRaw || !chassisRaw[0])
            return nullptr;
        char key[32];
        ArtKeyNorm(chassisRaw, key, sizeof(key));

        for (int i = 0; i < ArtCount; ++i)
            if (std::strcmp(Art[i].Chassis, key) == 0)
            {
                ContinueArt(Art[i]); // ART BUDGET: a few more layers this frame
                return Art[i].Done && Art[i].Any ? &Art[i] : nullptr;
            }
        if (ArtCount >= kArtSlots)
            return nullptr;

        const wchar_t *root = ResolveArtRoot();
        if (!root)
            return nullptr;          // no slot burned on a miss we cannot even attempt

        DollArt &a = Art[ArtCount++];
        std::memset(&a, 0, sizeof(a));
        strcpy_s(a.Chassis, key);
        a.Tried = true;

        if (!WIC)
        {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // harmless if already done
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&WIC))))
                WIC = nullptr;
        }

        // The folder is named after the chassis, but mods do not agree on what that is: the
        // target component says "alfar" while the pak ships "alfar_battlemech". Rather than
        // enumerate the directory on every miss, try the handful of suffixes that actually
        // occur - including the misspelt one, which is in the shipped data and not a typo here.
        static const char *suffix[] = { "", "_battlemech", "_omnimech", "_batlemech", "_mech" };
        char folder[64];
        bool found = false;
        for (size_t sfx = 0; sfx < sizeof(suffix) / sizeof(suffix[0]) && !found; ++sfx)
        {
            sprintf_s(folder, "%s%s", key, suffix[sfx]);
            wchar_t probe[MAX_PATH];
            swprintf_s(probe, L"%s\\%hs", root, folder);
            const DWORD at = GetFileAttributesW(probe);
            found = (at != INVALID_FILE_ATTRIBUTES) && (at & FILE_ATTRIBUTE_DIRECTORY);
        }
        if (!found)
        {
            a.Done = true;
            return nullptr;
        }
        strcpy_s(a.Folder, folder);
        ContinueArt(a); // ART BUDGET: the first layers now, the rest over the next frames
        return a.Done && a.Any ? &a : nullptr;
    }

    void ReleaseArt()
    {
        for (int i = 0; i < ArtCount; ++i)
            for (int p = 0; p < TMP_COUNT; ++p)
                for (int L = 0; L < kArtLayers; ++L)
                    SafeRelease(Art[i].Layer[p][L]);
        ArtCount = 0;
        SafeRelease(WIC);
        ArtRootTried = false;
        ArtRoot[0] = L'\0';
    }

    // A vehicle has one body and one health number, so it gets one mask. Loaded through the
    // same slot machinery by borrowing the head slot, since a vehicle never uses it.
    //
    // ONE PATH, NOT SIX SPELLINGS. The art is named inconsistently in the paks - body_side,
    // side_uix, front_uix, plain side - and the MissileCarrier's file is called lrm_carrier.
    // The extractor resolves all of that and drops a canonical <chassis>\body.png, so nothing
    // here has to know the spellings. That matters because the alternative is an alias table
    // in C++ that goes stale the first time a mod invents a seventh name and fails silently.
    //
    // FALLING BACK BY KIND. An unrecognised tank is still a tank, and a generic tank
    // silhouette carries the two facts that matter at a glance - what it is and how hurt it
    // is - which the coloured box it would otherwise get does not. The three generics are the
    // game's own art: its generic turret paperdoll, the Demolisher, and the Warrior.
    static const char *GenericForKind(uint8_t kind)
    {
        switch (kind)
        {
        case TUK_Turret:         return "turret";
        case TUK_Vtol:           return "vtol";
        case TUK_VtolDualEngine: return "vtoldual";
        case TUK_Artillery:      return "artillery";
        case TUK_Generator:      return "generator";
        case TUK_RadarTower:     return "radartower";
        case TUK_SatelliteDish:  return "satellite";
        case TUK_BattleArmor:    return "clanelemental"; // the game's own Elemental doll (2026-10-17)
        default:                 return "tank";
        }
    }

    ID2D1Bitmap *VehicleMask(const char *nameRaw, uint8_t kind)
    {
        ArtWatch();
        char key[32];
        ArtKeyNorm(nameRaw ? nameRaw : "", key, sizeof(key));
        // A vehicle whose name did not resolve still gets a cache slot, keyed by kind, so the
        // generic below is loaded once rather than once per contact per frame.
        if (!key[0])
            sprintf_s(key, "_k%u", (unsigned)kind);

        for (int i = 0; i < ArtCount; ++i)
            if (std::strcmp(Art[i].Chassis, key) == 0)
                return Art[i].Layer[TMP_Head][1];
        if (ArtCount >= kArtSlots)
            return nullptr;
        if (!ArtBudget())
            return nullptr; // ART BUDGET: loaded on a later frame

        const wchar_t *root = ResolveArtRoot();
        DollArt &a = Art[ArtCount++];
        std::memset(&a, 0, sizeof(a));
        strcpy_s(a.Chassis, key);
        a.Tried = true;
        if (!root)
            return nullptr;
        if (!WIC)
        {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&WIC))))
                WIC = nullptr;
        }

        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%hs\\body.png", root, key);
        a.Layer[TMP_Head][1] = TimedLoad(path);
        a.Done = true;

        if (!a.Layer[TMP_Head][1])
        {
            swprintf_s(path, L"%s\\_generic\\%hs.png", root, GenericForKind(kind));
            a.Layer[TMP_Head][1] = TimedLoad(path);
        }

        if (a.Layer[TMP_Head][1])
        {
            a.Any = true;
            const D2D1_SIZE_F sz = a.Layer[TMP_Head][1]->GetSize();
            a.W = sz.width;
            a.H = sz.height;
        }
        return a.Layer[TMP_Head][1];
    }

    // Returns false when this chassis has no art, so the caller falls back to rectangles.
    bool DrawDollArt(float cx, float topY, float scale, const TerrainMechInfo &m, float alpha)
    {
        if (m.Kind != TUK_Mech)
        {
            // ArtKey comes from the unit's own paperdoll texture reference and is exact;
            // Chassis is a display name and only coincidentally matches a folder.
            ID2D1Bitmap *body = VehicleMask(m.ArtKey[0] ? m.ArtKey : m.Chassis, m.Kind);
            if (!body)
                return false;
            const D2D1_SIZE_F sz = body->GetSize();
            const float h = 11.0f * scale;              // shorter than a mech, as it should read
            const float w = h * (sz.width / FMax(sz.height, 1.0f));
            const D2D1_RECT_F dst = D2D1::RectF(cx - w * 0.5f, topY + 5.0f * scale,
                                                cx + w * 0.5f, topY + 5.0f * scale + h);
            const float hp = m.Structure[TMP_CentreTorso];
            DrawPlate(dst, 1.5f, 0.62f * alpha);
            const D2D1_ANTIALIAS_MODE pv = RT->GetAntialiasMode();
            RT->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            if (scale >= 2.4f)
            {
                Col(0x000000, 0.85f * alpha);
                for (int o = 0; o < 4; ++o)
                {
                    const float ox = (o == 0) ? -1.0f : (o == 1) ? 1.0f : 0.0f;
                    const float oy = (o == 2) ? -1.0f : (o == 3) ? 1.0f : 0.0f;
                    const D2D1_RECT_F hd = D2D1::RectF(dst.left + ox, dst.top + oy,
                                                       dst.right + ox, dst.bottom + oy);
                    RT->FillOpacityMask(body, Brush, D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
                                        &hd, nullptr);
                }
            }
            Col((hp < 0.0f) ? 0x8A8A8A : PartColour(hp), 1.0f * alpha);
            RT->FillOpacityMask(body, Brush, D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dst, nullptr);
            RT->SetAntialiasMode(pv);
            return true;
        }
        const DollArt *a = ArtFor(m.ArtKey[0] ? m.ArtKey : m.Chassis);
        if (!a || a->H <= 0.0f)
            return false;

        const float h = 16.0f * scale;              // same footprint the rectangles occupy
        const float w = h * (a->W / a->H);
        const D2D1_RECT_F dst = D2D1::RectF(cx - w * 0.5f, topY, cx + w * 0.5f, topY + h);

        // FillOpacityMask is only valid with aliased antialiasing on a D2D1.0 render target.
        DrawPlate(dst, 1.5f, 0.62f * alpha);

        const D2D1_ANTIALIAS_MODE prev = RT->GetAntialiasMode();
        RT->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

        // A dark halo under the armour line-work, drawn as the same mask nudged one pixel in
        // four directions. That is what makes thin line art read as a solid object rather than
        // a scribble. It costs four extra fills per part, so it is spent only on dolls large
        // enough to benefit - a distant doll is a few pixels and gains nothing from it.
        const bool halo = (scale >= 2.4f);
        if (halo)
            for (int p = 0; p < TMP_COUNT; ++p)
            {
                if (!a->Layer[p][1] || (m.Structure[p] >= 0.0f && m.Structure[p] <= 0.02f))
                    continue;
                Col(0x000000, 0.85f * alpha);
                for (int o = 0; o < 4; ++o)
                {
                    const float ox = (o == 0) ? -1.0f : (o == 1) ? 1.0f : 0.0f;
                    const float oy = (o == 2) ? -1.0f : (o == 3) ? 1.0f : 0.0f;
                    const D2D1_RECT_F hd = D2D1::RectF(dst.left + ox, dst.top + oy,
                                                       dst.right + ox, dst.bottom + oy);
                    RT->FillOpacityMask(a->Layer[p][1], Brush,
                                        D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &hd, nullptr);
                }
            }

        for (int p = 0; p < TMP_COUNT; ++p)
        {
            const float str = m.Structure[p];
            const float arm = m.Armour[p];

            // Structure gone means the part is gone. Drawing nothing is the honest answer and
            // reads instantly: the silhouette has a bite taken out of it.
            if (str >= 0.0f && str <= 0.02f)
                continue;

            // Internals first, dim. When the armour above is stripped this is what remains
            // visible, so "armour gone, structure holding" needs no special case - it simply
            // stops being covered up.
            if (a->Layer[p][0])
            {
                // Internals coloured by the part's STRUCTURE on the same scale as armour (grey if
                // unread). A fixed salmon here showed wherever the armour art does not cover the
                // internals art, so every healthy mech looked structure-damaged.
                Col((str < 0.0f) ? 0x8A8A8A : PartColour(str), 0.80f * alpha);
                RT->FillOpacityMask(a->Layer[p][0], Brush,
                                    D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dst, nullptr);
            }
            if (a->Layer[p][1] && !(arm >= 0.0f && arm <= 0.02f))
            {
                // Unread armour draws neutral grey rather than healthy green: an unresolved
                // read must not look like good news.
                Col((arm < 0.0f) ? 0x8A8A8A : PartColour(arm), 1.0f * alpha);
                RT->FillOpacityMask(a->Layer[p][1], Brush,
                                    D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dst, nullptr);
            }
        }

        RT->SetAntialiasMode(prev);
        return true;
    }

    void DrawDoll(float cx, float topY, float scale, const TerrainMechInfo &m, float alpha)
    {
        // Real chassis art when we have it; the eight rectangles are the fallback, not the plan.
        if (DrawDollArt(cx, topY, scale, m, alpha))
        {
            ++DollsFromArt;
            return;
        }
        // Falling back is legitimate - a stock turret has no per-unit art because the game
        // itself uses one generic turret doll - but a MECH with no art is a name mismatch worth
        // chasing, so the name is carried out to the log rather than silently absorbed.
        ++ArtMisses;
        if (m.Chassis[0])
            strcpy_s(MissingArt, m.Chassis);

        // Fallback for a vehicle with no art: ONE box. Drawing the eight-part mech doll here
        // would assert seven parts the game does not model.
        // 2026-10-17: a plain shape per KIND, drawn in code (no art is shipped any more): tank = hull + turret + gun,
        // turret / artillery = base + head + gun, VTOL = body + rotor + tail, everything else (buildings, suits) a box.
        if (m.Kind != TUK_Mech)
        {
            const float hp = m.Structure[TMP_CentreTorso];
            Col((hp < 0.0f) ? 0x6A6A6A : PartColour(hp), 0.92f * alpha);
            const float s = scale, by = topY + 9.0f * s;
            auto box = [&](float x0, float y0, float x1, float y1) { RT->FillRectangle(D2D1::RectF(cx + x0 * s, by + y0 * s, cx + x1 * s, by + y1 * s), Brush); };
            auto line = [&](float x0, float y0, float x1, float y1, float w) {
                RT->DrawLine(D2D1::Point2F(cx + x0 * s, by + y0 * s), D2D1::Point2F(cx + x1 * s, by + y1 * s), Brush, w * s);
            };
            switch (m.Kind)
            {
            case TUK_Tank:
                box(-4.5f, 1.5f, 4.5f, 4.5f);  // hull
                box(-2.0f, 0.0f, 2.0f, 1.5f);  // turret
                line(2.0f, 0.7f, 6.0f, 0.7f, 0.7f); // gun
                break;
            case TUK_Turret:
            case TUK_Artillery:
                box(-3.0f, 2.5f, 3.0f, 4.5f);  // base
                box(-1.8f, 0.3f, 1.8f, 2.5f);  // head
                line(1.8f, 1.2f, 5.5f, 1.2f, 0.7f);
                break;
            case TUK_Vtol:
            case TUK_VtolDualEngine:
                RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - 0.5f * s, by + 2.6f * s), 3.0f * s, 1.4f * s), Brush); // body
                line(-5.0f, 0.4f, 4.0f, 0.4f, 0.5f);  // rotor
                line(2.0f, 2.6f, 6.0f, 2.0f, 0.6f);   // tail
                break;
            default:
            {
                const float bw = 7.0f, bh = 4.5f;
                box(-bw * 0.5f, 0.0f, bw * 0.5f, bh);
                break;
            }
            }
            return;
        }

        int n = 0;
        const DollPart *parts = DollLayout(n);
        const float w = 10.0f * scale;

        for (int i = 0; i < n; ++i)
        {
            const DollPart &p = parts[i];
            const float x0 = cx - w * 0.5f + p.X * scale;
            const float y0 = topY + p.Y * scale;
            const D2D1_RECT_F r = D2D1::RectF(x0, y0, x0 + p.W * scale, y0 + p.H * scale);

            const float arm = m.Armour[p.Part];
            const float str = m.Structure[p.Part];

            if (str >= 0.0f && str <= 0.02f)
            {
                // Gone. A faint outline keeps the silhouette readable - without it the doll
                // loses its shape and you cannot tell which part is missing.
                Col(0x6A6A6A, 0.45f * alpha);
                RT->DrawRectangle(r, Brush, 0.7f);
                continue;
            }
            if (arm < 0.0f)
            {
                // Never read. Neutral grey, so an unresolved condition looks unknown rather
                // than healthy - presenting a guess as data is the failure mode to avoid.
                Col(0x6A6A6A, 0.55f * alpha);
                RT->FillRectangle(r, Brush);
                continue;
            }
            Col(PartColour(arm), 0.92f * alpha);
            RT->FillRectangle(r, Brush);

            // Structure showing through: a thin dark inset once the armour is off, so "armour
            // stripped" and "structure damaged" are two readable states rather than one.
            if (arm <= 0.02f && str > 0.02f)
            {
                const float inset = 0.55f * scale;
                const D2D1_RECT_F ir = D2D1::RectF(r.left + inset, r.top + inset,
                                                   r.right - inset, r.bottom - inset);
                if (ir.right > ir.left && ir.bottom > ir.top)
                {
                    Col(0x3A0E0A, 0.75f * alpha);
                    RT->FillRectangle(ir, Brush);
                }
            }
        }
    }

    // The back of a mech, as a strip of three chips under the doll - left, centre, right torso
    // in the order they appear on the doll above.
    //
    // WHY NOT A SECOND SILHOUETTE. The rear masks were extracted and are sitting on disk, so a
    // full back view was available. But the doll is 17-30 px wide: a second one doubles the
    // footprint of every contact on a page that may show a dozen, to convey three numbers. The
    // strip says the same thing in a quarter of the space, and its position under the front doll
    // is what makes it read as "the back" without a caption.
    //
    // This matters more than its size suggests. Rear armour is where a mech is actually killed,
    // and a front-only doll draws an intact-looking target whose back is wide open exactly the
    // same as one that is genuinely healthy.
    // A dark plate behind a readout.
    //
    // The complaint was that labels and dolls get hard to read as a contact fades, and the fix
    // is NOT opacity - the label layer already draws everything at 1.0 and has since the fade
    // was removed. The real problem is CONTRAST: hostile red line-art sits on a wireframe
    // terrain that is itself a mesh of bright lines, and on a canyon map with 500 m of relief
    // there are far more of them. Nothing is fading; the background is winning.
    //
    // So both the doll and the text get a plate to sit on. One mechanism, both symptoms, and it
    // costs a single FillRectangle per readout.
    void DrawPlate(const D2D1_RECT_F &r, float pad, float a)
    {
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF(r.left - pad, r.top - pad, r.right + pad, r.bottom + pad), 2.5f, 2.5f);
        Col(0x05080A, a);
        RT->FillRoundedRectangle(rr, Brush);
    }

    void DrawRearStrip(float cx, float topY, float width, float scale,
                       const TerrainMechInfo &m, float alpha)
    {
        if (!m.HaveRear)
            return;

        const int   order[3] = { TRP_LeftTorso, TRP_CentreTorso, TRP_RightTorso };
        const float gap = FMax(1.0f, 0.35f * scale);
        const float bw  = (width - gap * 2.0f) / 3.0f;
        const float bh  = FMax(2.0f, 1.8f * scale);
        if (bw <= 0.5f)
            return;

        for (int i = 0; i < 3; ++i)
        {
            const int   k   = order[i];
            const float arm = m.ArmourRear[k];
            const float str = m.StructureRear[k];
            const float x0  = cx - width * 0.5f + i * (bw + gap);
            const D2D1_RECT_F r = D2D1::RectF(x0, topY, x0 + bw, topY + bh);

            if (str >= 0.0f && str <= 0.02f)
            {
                Col(0x6A6A6A, 0.45f * alpha);
                RT->DrawRectangle(r, Brush, 0.7f);
                continue;
            }
            Col((arm < 0.0f) ? 0x6A6A6A : PartColour(arm), 0.9f * alpha);
            RT->FillRectangle(r, Brush);
        }
    }


    // ====================================================================================
    // THE LABEL LAYER - two tiers, drawn nearest first
    //
    // Everything used to get the full treatment: doll, rear strip, name, tonnage, heat and a
    // line per weapon. At four contacts that reads beautifully. At forty it is roughly three
    // hundred lines of text on one page, overlapping in scene order, and the information became
    // unreadable precisely when there was most of it - which is the wrong way round.
    //
    // So detail follows attention, not availability:
    //
    //   FOCUSED  - the game's locked target, or the contact cycled to with the bezel. Gets
    //              everything, because this is the unit the pilot is actually thinking about.
    //   EVERYONE - variant designation and the paperdoll. That is enough to answer "what is it
    //              and how hurt is it" at a glance, which is all a background contact owes you.
    //
    // Dolls are never suppressed, only text. A doll is positional - it sits over its chevron and
    // says what stands there - so hiding it would lose information that has nowhere else to go.
    // Text is what collides, so text is what gets declutered.
    // ====================================================================================
    struct LabelCand
    {
        float X, Y, D, S;
        const TerrainMechInfo *Info;
        uint32_t Col;
        bool Shown; // this is the contact the pilot clicked: doll and full name
    };
    // ====================================================================================
    // GHOSTS - contacts the sweep has stopped refreshing
    //
    // The label was never fading. The CONTACT was leaving the scene: the scope reveals a contact
    // on the ping and fades it out over a few seconds, and once its alpha reaches zero it is
    // dropped from the list entirely. No contact, no label - which is why the text vanished in
    // lockstep with the chevron.
    //
    // That fade is right for the scope, where it says "this position is getting old", and the
    // scope's own design should not be disturbed to serve this page. So the persistence lives
    // here: the 3D page remembers where each unit last was and keeps drawing it.
    //
    // WHAT IS STALE AND WHAT IS NOT. The POSITION is frozen at last contact - that is the honest
    // part, and the dimmed grey chevron is what says so. The doll and label are NOT stale: the
    // identity records come from the producer's own 250 ms scan, which never stopped, so armour
    // and heat continue to update on a unit standing at its last known spot.
    //
    // A ghost dies when its record does. The record only exists while the unit is alive and in
    // scan range, so a kill removes it on the next scan - which is exactly the behaviour asked
    // for. The timeout is a backstop for anything that slips past that.
    struct Ghost
    {
        uint16_t Id;
        float    X, Y, Z, Yaw;
        uint8_t  Kind;
        uint32_t Col;
        double   LastSeenMs;
        bool     Live;      // refreshed by the scene this frame
        bool     Selected;  // the game's lock, remembered across the fade
    };
    Ghost  Ghosts[TERR_MAX_MECHS];
    int    GhostCount = 0;
    static constexpr double kGhostMs = 45000.0; // backstop only; the record normally decides

    void NoteContact(const LidarSceneContact &c, uint32_t col)
    {
        if (c.Id == 0)
            return;
        for (int i = 0; i < GhostCount; ++i)
            if (Ghosts[i].Id == c.Id)
            {
                Ghost &g = Ghosts[i];
                g.X = c.X; g.Y = c.Y; g.Z = c.Z; g.Yaw = c.Yaw;
                g.Kind = c.Kind; g.Col = col; g.Selected = (c.Selected != 0);
                g.LastSeenMs = NowMs(); g.Live = true;
                return;
            }
        if (GhostCount >= TERR_MAX_MECHS)
            return;
        Ghost &g = Ghosts[GhostCount++];
        g.Id = c.Id; g.X = c.X; g.Y = c.Y; g.Z = c.Z; g.Yaw = c.Yaw;
        g.Kind = c.Kind; g.Col = col; g.Selected = (c.Selected != 0);
        g.LastSeenMs = NowMs(); g.Live = true;
    }

    // Draws whatever the sweep is no longer refreshing, and reaps what has died.
    void DrawGhosts(const Mat4 &vp, float px, float py, float horizonM, float camYawDeg)
    {
        const double now = NowMs();
        int keep = 0;
        for (int i = 0; i < GhostCount; ++i)
        {
            Ghost &g = Ghosts[i];
            const TerrainMechInfo *info = MechInfoFor(g.Id);

            // No record means the unit is gone - killed, or out of the producer's scan range.
            // Either way there is nothing true left to draw, so the ghost goes with it.
            if (!info || (now - g.LastSeenMs) > kGhostMs)
                continue;
            Ghosts[keep++] = g;

            if (g.Live)
                continue; // the live pass already drew it

            const float wx = g.X * 0.01f, wy = g.Y * 0.01f, wz = g.Z * 0.01f;
            const float dx = wx - px, dy = wy - py;
            if (dx * dx + dy * dy > horizonM * horizonM)
                continue;
            float sx, sy, d;
            if (!Project(vp, wx, wy, wz, sx, sy, d))
                continue;

            // Dim grey, not team colour: the mark is still there but its POSITION is old, and
            // colour is what the eye reads first. Losing the red is the signal.
            const float sz = (g.Kind == KIND_MECH)   ? 2.25f
                           : (g.Kind == KIND_TURRET) ? 1.50f
                           : (g.Kind == KIND_BA)     ? 1.00f // a suit: the smallest mark
                                                     : 1.80f;
            if (g.Kind == KIND_BA)
                Diamond(sx, sy, sz, 0x5E666B, 0.75f);
            else
                Chevron(sx, sy, (g.Yaw - camYawDeg) * 0.01745329252f, sz, 0x5E666B, 0.75f,
                        (g.Kind == KIND_MECH) ? 1.25f : 0.9f);

            if (info->Dead)
                continue; // a wreck gets its grey mark and nothing else
            if (CandCount < (int)(sizeof(Cands) / sizeof(Cands[0])))
            {
                LabelCand &q = Cands[CandCount++];
                q.X = sx; q.Y = sy; q.D = d; q.S = sz;
                q.Info = info; q.Col = g.Col;
                q.Shown = (ShownId && info->Id == ShownId) || info->Locked; // it has a plate
            }
        }
        GhostCount = keep;
    }

    LabelCand Cands[TERR_MAX_MECHS];
    int       CandCount = 0;

    // Text rectangles already placed this frame, so a later one can stand off.
    D2D1_RECT_F Placed[TERR_MAX_MECHS];
    int         PlacedCount = 0;

    static bool Overlaps(const D2D1_RECT_F &a, const D2D1_RECT_F &b)
    {
        return !(a.right < b.left || b.right < a.left || a.bottom < b.top || b.bottom < a.top);
    }

    void DrawLabelLayer()
    {
        // Nearest first. Insertion sort - <algorithm> does not compile in this translation unit
        // (no NOMINMAX, see the note at the top) and at this size it is faster anyway.
        for (int i = 1; i < CandCount; ++i)
        {
            LabelCand v = Cands[i];
            int j = i - 1;
            for (; j >= 0 && Cands[j].D > v.D; --j)
                Cands[j + 1] = Cands[j];
            Cands[j + 1] = v;
        }

        PlacedCount = 0;
        for (int i = 0; i < CandCount; ++i)
        {
            const LabelCand &q = Cands[i];
            const float scale   = Clampf(180.0f / ((q.D > 1.0f) ? q.D : 1.0f), 1.7f, 4.4f);
            const float dollH   = 16.0f * scale;
            const float dollTop = q.Y - q.S * 2.6f - dollH - 6.0f;

            // Variant, chassis as the fallback - "CPLT-C2" tells a pilot more than "Catapult".
            // Cut to kFarNameChars unless its doll is shown: it only has to say WHAT stands there,
            // and a long vehicle name is the widest thing on the page.
            wchar_t name[40] = L"";
            if (q.Info->Variant[0])      swprintf_s(name, L"%hs", q.Info->Variant);
            else if (q.Info->Chassis[0]) swprintf_s(name, L"%hs", q.Info->Chassis);
            if (!q.Shown && wcslen(name) > (size_t)Topo3DView::kFarNameChars)
                name[Topo3DView::kFarNameChars] = L'\0';

            // The clicked contact and the lock get a full plate (DrawCuePanel), which carries the
            // doll and the name: no tag of their own here. Everything else is a name.
            float anchorY = q.Y - 8.0f;
            (void)dollTop;
            if (q.Shown)
            {
                ++InArcDrawn;
                ++LabelsDrawn;
                continue;
            }
            if (!name[0])
            {
                ++LabelsDrawn;
                continue;
            }

            // A CLASH MOVES THE LABEL, IT NEVER DELETES IT - other side first, then up and down
            // in rungs, and wherever it lands a dotted leader ties it back to its chevron.
            const float flipX = (q.X > (float)Width * 0.66f) ? 1.0f : 0.0f;
            const float nw = kContactCharW * (float)wcslen(name) + 4.0f;
            const float xPref = flipX ? (q.X - nw - 12.0f) : (q.X + 12.0f);
            const float xAlt  = flipX ? (q.X + 12.0f)      : (q.X - nw - 12.0f);
            const float rowH  = kContactFontPx + 3.0f;

            float px = xPref, py = anchorY;
            bool  free = false, moved = false;
            for (int rung = 0; rung <= 6 && !free; ++rung)
                for (int sd = 0; sd < 2 && !free; ++sd)
                    for (int sg = 0; sg < 2 && !free; ++sg)
                    {
                        if (rung == 0 && sg == 1)
                            continue; // dy of zero exists once, not twice
                        const float cx2 = (sd == 0) ? xPref : xAlt;
                        const float cy2 = anchorY + (sg ? -1.0f : 1.0f) * rowH * (float)rung;
                        const D2D1_RECT_F t = D2D1::RectF(cx2, cy2 - 2.0f, cx2 + nw, cy2 + rowH);
                        bool clash = false;
                        for (int k = 0; k < PlacedCount && !clash; ++k)
                            clash = Overlaps(t, Placed[k]);
                        if (clash)
                            continue;
                        px = cx2; py = cy2; free = true;
                        moved = (sd != 0) || (rung != 0);
                    }
            if (!free)
                moved = true; // nowhere clear; it still gets drawn, and still gets its tether

            if (moved)
            {
                EnsureLeaderStroke();
                // Anchor on the label's inner edge, so the line leaves the text pointing at the
                // contact rather than cutting across its own words.
                const float ax = (px < q.X) ? (px + nw) : px;
                const float ay = py + rowH * 0.5f;
                Col(q.Col, 0.45f);
                RT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(q.X, q.Y), Brush,
                             0.9f, LeaderStroke);
            }

            DrawPlate(D2D1::RectF(px, py, px + nw - 2.0f, py + rowH - 2.0f), 2.0f, 0.60f);
            LabelSm(name, px, py, nw + 8.0f, q.Col, q.Shown ? 1.0f : 0.75f);
            if (PlacedCount < TERR_MAX_MECHS)
                Placed[PlacedCount++] = D2D1::RectF(px, py - 2.0f, px + nw, py + rowH);
            ++LabelsDrawn;
        }
    }

    void EnsureLeaderStroke()
    {
        if (LeaderStroke || !Factory)
            return;
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
        sp.dashStyle = D2D1_DASH_STYLE_DOT;
        sp.startCap  = D2D1_CAP_STYLE_ROUND;
        sp.endCap    = D2D1_CAP_STYLE_ROUND;
        Factory->CreateStrokeStyle(sp, nullptr, 0, &LeaderStroke);
    }

    // ====================================================================================
    // THE CUE PANEL - the game's lock, and nothing else
    //
    // Fixed bottom-left, always present. It shows the unit the GAME has locked - the record the
    // producer flags as TargetTracking.CurrentTarget - so a lock that has faded off the ping
    // keeps its panel. It does not follow the arc - slewing away from the lock is
    // exactly when you still want its readout - and it clears the moment the game drops the
    // lock. Position comes from the ghost table, which holds the last-known spot of every
    // contact seen, live or faded.
    // ====================================================================================
    // THE TARGET PLATES (was the fixed bottom-left cue panel). The game's lock, and the contact
    // the pilot clicked, each get the full readout - doll, rear strip, name, weapons - on a plate
    // standing above the unit on a stalk (below it when there is no room above), as V-POD does.
    // A target that is not on the page waits bottom-left (the lock) or bottom-right (the clicked
    // one) and says which way it went. The plates' rects are kept for PickContact.
    D2D1_RECT_F PlateRect[2] = {};
    uint16_t    PlateId[2] = {};
    int         PlateCount = 0;

    const TerrainMechInfo *InfoById(uint16_t id) const
    {
        if (!id || !Head.MechInfoScanned)
            return nullptr;
        for (uint32_t k = 0; k < Head.MechInfoCount && k < (uint32_t)TERR_MAX_MECHS; ++k)
            if (Head.Mechs[k].Valid && Head.Mechs[k].Id == id && !Head.Mechs[k].Dead)
                return &Head.Mechs[k];
        return nullptr;
    }

    void DrawCuePanel(const LidarScene &scene, const Mat4 &vp)
    {
        (void)scene;
        (void)vp;
        LockedIdNow = 0;
        PlateCount = 0;
        const TerrainMechInfo *lock = nullptr;
        if (Head.MechInfoScanned)
            for (uint32_t k = 0; k < Head.MechInfoCount && k < (uint32_t)TERR_MAX_MECHS && !lock; ++k)
                if (Head.Mechs[k].Valid && Head.Mechs[k].Locked && !Head.Mechs[k].Dead)
                    lock = &Head.Mechs[k];
        if (lock)
            LockedIdNow = lock->Id;
        const TerrainMechInfo *shown = InfoById(ShownId);
        if (shown == lock)
            shown = nullptr;
        // The clicked one first, the lock last: the lock's plate is drawn on top.
        if (shown)
            DrawTargetPlate(*shown, false);
        if (lock)
            DrawTargetPlate(*lock, true);
    }

    void DrawTargetPlate(const TerrainMechInfo &info, bool isLock)
    {
        const float cl = Width * Inset, ct = Height * Inset, cr = Width * (1.0f - Inset), cb = Height * (1.0f - Inset);
        const float edge = 8.0f, stalk = 34.0f;

        const Ghost *g = nullptr;
        for (int i = 0; i < GhostCount; ++i)
            if (Ghosts[i].Id == info.Id) { g = &Ghosts[i]; break; }
        // A FRIEND is cyan here exactly as on V-POD; an enemy keeps its contact colour.
        const uint32_t col = !info.Hostile ? kColAllyLock : (g ? g->Col : kColHostile);

        wchar_t hdr[64];
        if (g)
        {
            const float wx = g->X * 0.01f, wy = g->Y * 0.01f;
            const int rng = (int)(std::sqrt((wx - ArcPX) * (wx - ArcPX) + (wy - ArcPY) * (wy - ArcPY)) + 0.5f);
            if (isLock)
                swprintf_s(hdr, L"LOCK  %d m", rng);
            else
                swprintf_s(hdr, L"%d m", rng);
        }
        else
            swprintf_s(hdr, isLock ? L"LOCK" : L"");

        // The plate's size, worked out as V-POD works it out.
        const float scale = 2.8f;
        const float dollW = 10.0f * scale, dollH = 16.0f * scale;
        const int   lines = FocusedLineCount(info);
        const float textH = 13.0f + 12.0f * (float)(lines - 1) + 4.0f;
        const float bodyH = FMax(dollH + (info.HaveRear ? 3.0f * scale : 0.0f), textH);
        const float plateW = dollW + 10.0f + 150.0f, plateH = bodyH + 17.0f;

        // Where the unit is on the page this frame: its label candidate (the chevron).
        const LabelCand *q = nullptr;
        for (int i = 0; i < CandCount; ++i)
            if (Cands[i].Info && Cands[i].Info->Id == info.Id) { q = &Cands[i]; break; }
        const bool on = q && q->X >= cl && q->X <= cr && q->Y >= ct && q->Y <= cb;

        float left, top;
        if (on)
        {
            const float unitTop = q->Y - q->S * 2.6f, unitBot = q->Y + q->S * 2.0f;
            left = Clampf(q->X - plateW * 0.5f, cl + edge, cr - edge - plateW);
            top = unitTop - stalk - plateH;
            float stalkFrom = unitTop, stalkTo = unitTop - stalk;
            bool below = top < ct + edge + 17.0f;
            // Two plates over each other: this one goes below its unit instead.
            const D2D1_RECT_F mine = D2D1::RectF(left - 4.0f, top - 4.0f, left + plateW + 4.0f, top + plateH + 4.0f);
            for (int k = 0; k < PlateCount && !below; ++k)
                below = !(mine.right < PlateRect[k].left || PlateRect[k].right < mine.left ||
                          mine.bottom < PlateRect[k].top || PlateRect[k].bottom < mine.top);
            if (below)
            {
                top = FMin(unitBot + stalk, cb - edge - plateH);
                stalkFrom = unitBot;
                stalkTo = top;
            }
            Col(col, 0.8f);
            RT->DrawLine(D2D1::Point2F(q->X, stalkFrom), D2D1::Point2F(q->X, stalkTo), Brush, 1.2f);
            DrawLockPlate(left, top, plateW, plateH, dollW, dollH, scale, info, col, hdr);
        }
        else
        {
            // Not on the page: the lock waits bottom-left, the clicked one bottom-right.
            left = isLock ? cl + edge + 4.0f : cr - edge - 4.0f - plateW;
            top = cb - edge - plateH;
            DrawLockPlate(left, top, plateW, plateH, dollW, dollH, scale, info, col, hdr);
            const wchar_t *where = L"OFF SCREEN";
            if (g)
                where = (ArcOffDeg(g->X * 0.01f, g->Y * 0.01f) > 0.0f) ? L"OFF SCREEN >" : L"< OFF SCREEN";
            Label(where, isLock ? left + plateW + 10.0f : left - 110.0f, top - 2.0f, 140.0f, col, 0.9f);
        }
        if (PlateCount < 2)
        {
            PlateRect[PlateCount] = D2D1::RectF(left - 4.0f, top - 4.0f, left + plateW + 4.0f, top + plateH + 4.0f);
            PlateId[PlateCount] = info.Id;
            ++PlateCount;
        }
    }

    // ---- the text ------------------------------------------------------------------------
    // Weapon lines carry the colour rule that makes this worth drawing: a weapon whose mount has
    // lost its armour goes amber, and one whose mount's structure is gone goes red, because that
    // weapon is gone with it. That turns the label from an inventory into a threat assessment.
    // Mirrors what DrawMechLabel will actually emit, so the plate is neither short nor a
    // great black slab over empty ground.
    static int FocusedLineCount(const TerrainMechInfo &m)
    {
        int n = 0;
        if (m.Variant[0] || m.Chassis[0]) ++n;
        if (m.Callsign[0]) ++n; // 2026-10-09: the pilot line under the name - uncounted, the last weapon hung below the plate
        if (m.Kind != TUK_Mech)
        {
            ++n;                                            // TANK / TURRET / VTOL
            if (m.Structure[TMP_CentreTorso] >= 0.0f) ++n;  // HP
        }
        else
        {
            if (m.Tons > 0.5f || m.WeightClass <= 3) ++n;
            if (m.HaveHeat && m.HeatPct >= 0.0f) ++n;
        }
        WpnLine wl[TERR_MAX_WEAPONS];
        n += WeaponLines(m, wl, TERR_MAX_WEAPONS); // grouped, as drawn (v15)
        return (n < 1) ? 1 : n;
    }

    void DrawMechLabel(float sx, float sy, const TerrainMechInfo &m, uint32_t col, float alpha,
                       bool flipLeft)
    {
        wchar_t line[96];
        const float lw = 150.0f;
        const float x = flipLeft ? (sx - lw - 12.0f) : (sx + 12.0f);
        float y = sy;

        // Name: variant if we have it, chassis otherwise - the variant is the more useful of the
        // two to a pilot (it implies the loadout) and is the one the game itself leads with.
        // THE VARIANT LEADS, AND THE CALLSIGN IS EXTRA.
        //
        // The callsign used to replace the variant, which made a bad read maximally destructive:
        // a misread persona string put gibberish where "CPLT-C2" belonged, and the one piece of
        // information that was definitely correct was the piece it hid. The identification the
        // display exists to give comes first; the pilot is a bonus line under it.
        wchar_t name[64] = L"";
        if (m.Variant[0])
            swprintf_s(name, L"%hs", m.Variant);
        else if (m.Chassis[0])
            swprintf_s(name, L"%hs", m.Chassis);
        if (name[0])
        {
            Label(name, x, y, lw, col, alpha);
            y += 13.0f;
        }
        if (m.Callsign[0])
        {
            wchar_t cs[40];
            swprintf_s(cs, L"%hs", m.Callsign);
            Label(cs, x, y, lw, 0x8FB8C9, alpha * 0.9f);
            y += 12.0f;
        }
        if (m.Kind != TUK_Mech)
        {
            // 2026-10-17: every kind by name (was [Kind & 3]: a twin-engine VTOL read blank, artillery TANK,
            // a generator TURRET) - and ELEMENTAL for a battle armour suit
            static const wchar_t *kindName[TUK_COUNT] = { L"", L"TANK", L"TURRET", L"VTOL", L"VTOL", L"ARTILLERY",
                                                          L"GENERATOR", L"RADAR TOWER", L"SATELLITE", L"ELEMENTAL" };
            Label(m.Kind < TUK_COUNT ? kindName[m.Kind] : L"", x, y, lw, 0xB9C4CC, alpha * 0.9f);
            y += 12.0f;
            const float hp = m.Structure[TMP_CentreTorso];
            if (hp >= 0.0f)
            {
                swprintf_s(line, L"HP %d%%", (int)(hp * 100.0f + 0.5f));
                Label(line, x, y, lw, PartColour(hp), alpha * 0.9f);
                y += 12.0f;
            }
            WpnLine wl[TERR_MAX_WEAPONS];
            const int nl = WeaponLines(m, wl, TERR_MAX_WEAPONS);
            for (int w2 = 0; w2 < nl; ++w2)
            {
                WeaponLineText(wl[w2], line, _countof(line));
                Label(line, x, y, lw, 0xD8E2E8, alpha * 0.95f);
                y += 12.0f;
            }
            return;
        }

        // Weight: the tonnage when it resolved, the class when it did not. Never both, because
        // the class is implied by the tonnage and the label has no room for redundancy.
        if (m.Tons > 0.5f)
        {
            swprintf_s(line, L"%.0ft", m.Tons);
            Label(line, x, y, lw, 0xB9C4CC, alpha * 0.9f);
            y += 12.0f;
        }
        else if (m.WeightClass <= 3)
        {
            static const wchar_t *cls[4] = { L"LIGHT", L"MEDIUM", L"HEAVY", L"ASSAULT" };
            Label(cls[m.WeightClass], x, y, lw, 0xB9C4CC, alpha * 0.9f);
            y += 12.0f;
        }

        if (m.HaveHeat && m.HeatPct >= 0.0f)
        {
            const uint32_t hc = (m.HeatPct > 0.8f) ? 0xFF5A4D
                              : (m.HeatPct > 0.5f) ? 0xFFD11A : 0x6FB3C9;
            swprintf_s(line, L"HEAT %d%%", (int)(m.HeatPct * 100.0f + 0.5f));
            Label(line, x, y, lw, hc, alpha * 0.9f);
            y += 12.0f;
        }

        // Grouped (v15): identical weapons in the same state on one line - the colours are TacWeaponColour's
        // (red = destroyed or its mount gone, yellow = its mount's armour stripped, white = live).
        WpnLine wl[TERR_MAX_WEAPONS];
        const int nl = WeaponLines(m, wl, TERR_MAX_WEAPONS);
        for (int w = 0; w < nl; ++w)
        {
            WeaponLineText(wl[w], line, _countof(line));
            Label(line, x, y, lw, wl[w].Col, alpha * 0.95f);
            y += 12.0f;
        }
    }

    // ====================================================================================
    // PILOT VIEW: THE LOCK, MARKED WHERE IT STANDS
    //
    // The vanilla Targeting screen's own lock marker, drawn AFTER the mesh outlines and the HUD
    // overlay so nothing paints over it: a red acquisition bracket around the game's locked unit,
    // and the cue panel's doll-and-weapons plate hung above it on a stalk. Position is the
    // contact's own (last known if the ping has faded); the bracket's height is the unit's
    // height - ground under it to the top of the hull - so it frames the mesh outline.
    // ====================================================================================
    // Screen boxes of the contacts in the pilot view, for the outline pass: edges inside a box are
    // a UNIT's and are drawn bright at any range; everything else is the world, faded when near.
    static constexpr int kMaxPilotBoxes = 32;
    float PilotBoxes[kMaxPilotBoxes][4] = {};
    int   PilotBoxCount = 0;

    // Ground under the unit to the top of its hull, metres. `kind` uses the shared numbering
    // (0 mech, 1 tank, 2 turret, 3 VTOL, 4 twin-engine VTOL).
    void UnitExtent(float wx, float wy, float wz, uint8_t kind, uint8_t weight, float &zBot, float &zTop) const
    {
        float gz = 0.0f;
        const bool haveG = GroundAtM(wx, wy, gz) && gz < wz + 2.0f;
        if (kind == 0)
        {
            static const float kH[4] = {9.0f, 11.0f, 13.0f, 15.0f};
            const float h = (weight <= 3) ? kH[weight] : 12.0f;
            zBot = haveG ? gz : wz - h * 0.5f;
            zTop = zBot + FMax(h, (wz - zBot) * 1.8f);
        }
        else if (kind == 3 || kind == 4)
        {
            zBot = wz - 3.0f;
            zTop = wz + 3.0f;
        }
        else if (kind == TUK_BattleArmor) // 2026-10-17: a suit stands on the ground, ~3 m tall
        {
            zBot = haveG ? gz : wz - 1.5f;
            zTop = zBot + 3.0f;
            return;
        }
        else
        {
            zBot = haveG ? gz : wz - 2.0f;
            zTop = FMax(wz, zBot) + 4.0f;
        }
    }

    void BuildPilotBoxes(const LidarScene &scene, const Mat4 &vp)
    {
        PilotBoxCount = 0;
        if (PilotLiveValid)
        {
            float l, t, r, b;
            if (ScreenBox(vp, PilotLiveOrigin, PilotLiveExtent, l, t, r, b))
            {
                float *bx = PilotBoxes[PilotBoxCount++];
                bx[0] = l - 6.0f; bx[1] = t - 6.0f; bx[2] = r + 6.0f; bx[3] = b + 6.0f;
            }
        }
        for (int i = 0; i < scene.ContactCount && scene.Contacts && PilotBoxCount < kMaxPilotBoxes; ++i)
        {
            const LidarSceneContact &c = scene.Contacts[i];
            const float wx = c.X * 0.01f, wy = c.Y * 0.01f, wz = c.Z * 0.01f;
            float zBot, zTop, bx, by, bd, tx, ty, td;
            // contacts carry LidarKind, where 4 is a battle armour suit (not a twin-engine VTOL as in the records)
            UnitExtent(wx, wy, wz, c.Kind == KIND_BA ? (uint8_t)TUK_BattleArmor : c.Kind, c.Weight, zBot, zTop);
            if (!Project(vp, wx, wy, zBot, bx, by, bd) || !Project(vp, wx, wy, zTop, tx, ty, td))
                continue;
            // Generous: a walking mech's arms and legs swing wider than it is tall.
            const float half = std::fabs(by - ty) * 0.65f + 6.0f;
            const float cx = (bx + tx) * 0.5f, cy = (by + ty) * 0.5f;
            float *b = PilotBoxes[PilotBoxCount++];
            b[0] = cx - half;
            b[1] = cy - half;
            b[2] = cx + half;
            b[3] = cy + half;
        }
    }

    Mat4     PilotVp{};
    bool     PilotVpValid     = false;
    uint16_t PilotLockId      = 0;
    float    PilotLockPos[3]  = {};
    bool     PilotLockHavePos = false;
    // The lock as the game had it in the frame the outlines come from (VanillaHud, every frame):
    // world bounds centre and half-size, cm. Preferred over the 3 s contact scan when present.
    bool     PilotLiveValid = false;
    bool     PilotBare = false;                       // CAP: view only, nothing drawn by Render
    bool     Transparent = false;                     // the .pak overlay: no background, occluders depth-only
    bool     MlAcquiring = false, MlLocked = false;   // the player's missile lock (SetPilotMissileLock)
    float    MlProgress = 0.0f;
    float    PilotLiveOrigin[3] = {}, PilotLiveExtent[3] = {};
    static constexpr uint32_t kColAllyLock = 0x33E6FF; // cyan: the acquired target is a friend

    // Screen rectangle of a world box (cm), from its 8 corners. False if any corner is behind
    // the eye.
    bool ScreenBox(const Mat4 &vp, const float *o, const float *e, float &l, float &t, float &r, float &b) const
    {
        l = t = 1e9f;
        r = b = -1e9f;
        for (int i = 0; i < 8; ++i)
        {
            const float x = (o[0] + ((i & 1) ? e[0] : -e[0])) * 0.01f;
            const float y = (o[1] + ((i & 2) ? e[1] : -e[1])) * 0.01f;
            const float z = (o[2] + ((i & 4) ? e[2] : -e[2])) * 0.01f;
            float sx, sy, d;
            if (!Project(vp, x, y, z, sx, sy, d))
                return false;
            l = sx < l ? sx : l;
            r = sx > r ? sx : r;
            t = sy < t ? sy : t;
            b = sy > b ? sy : b;
        }
        return true;
    }

    static constexpr uint32_t kColAcquire = 0xFF3B30;

    void RenderPilotLock(const LidarScene &scene)
    {
        if (!PilotOn || !PilotVpValid || !RT || !Brush)
            return;
        const TerrainMechInfo *info = nullptr;
        if (Head.MechInfoScanned)
            for (uint32_t k = 0; k < Head.MechInfoCount && k < (uint32_t)TERR_MAX_MECHS && !info; ++k)
                if (Head.Mechs[k].Valid && Head.Mechs[k].Locked && !Head.Mechs[k].Dead)
                    info = &Head.Mechs[k];
        if (!info)
        {
            PilotLockId = 0;
            PilotLockHavePos = false;
            LockedIdNow = 0;
            return;
        }
        LockedIdNow = info->Id;
        if (info->Id != PilotLockId)
        {
            PilotLockId = info->Id;
            PilotLockHavePos = false;
        }
        for (int i = 0; i < scene.ContactCount && scene.Contacts; ++i)
            if (scene.Contacts[i].Id == info->Id)
            {
                PilotLockPos[0] = scene.Contacts[i].X;
                PilotLockPos[1] = scene.Contacts[i].Y;
                PilotLockPos[2] = scene.Contacts[i].Z;
                PilotLockHavePos = true;
                break;
            }
        if (PilotLiveValid)
        {
            PilotLockPos[0] = PilotLiveOrigin[0];
            PilotLockPos[1] = PilotLiveOrigin[1];
            PilotLockPos[2] = PilotLiveOrigin[2];
            PilotLockHavePos = true;
        }
        if (!PilotLockHavePos)
            return;

        RT->BeginDraw();
        const float cl = Width * Inset, ct = Height * Inset, cr = Width * (1.0f - Inset), cb = Height * (1.0f - Inset);
        RT->PushAxisAlignedClip(D2D1::RectF(cl, ct, cr, cb), D2D1_ANTIALIAS_MODE_ALIASED);
        Mono = false;
        const uint32_t col = info->Hostile ? kColHostile : kColAllyLock;
        const uint32_t bracketCol = info->Hostile ? kColAcquire : kColAllyLock;

        // The unit's vertical extent: ground under it to the top of the hull.
        const float wx = PilotLockPos[0] * 0.01f, wy = PilotLockPos[1] * 0.01f, wz = PilotLockPos[2] * 0.01f;
        float zBot, zTop;
        UnitExtent(wx, wy, wz, info->Kind, info->WeightClass, zBot, zTop);

        // The bracket's box: the live world bounds when VanillaHud sent them (exact, this frame),
        // else a square on the unit's estimated height at its last scanned spot.
        float bl = 0, bt = 0, br = 0, bb = 0;
        bool on;
        if (PilotLiveValid && ScreenBox(PilotVp, PilotLiveOrigin, PilotLiveExtent, bl, bt, br, bb))
        {
            const float pad = 4.0f;
            bl -= pad; bt -= pad; br += pad; bb += pad;
            const float minHalf = 11.0f, cxm = (bl + br) * 0.5f, cym = (bt + bb) * 0.5f;
            if (br - bl < 2 * minHalf) { bl = cxm - minHalf; br = cxm + minHalf; }
            if (bb - bt < 2 * minHalf) { bt = cym - minHalf; bb = cym + minHalf; }
            on = br >= cl && bl <= cr && bb >= ct && bt <= cb;
        }
        else
        {
            float bx = 0, by = 0, bd = 0, tx = 0, ty = 0, td = 0;
            on = Project(PilotVp, wx, wy, zBot, bx, by, bd) && Project(PilotVp, wx, wy, zTop, tx, ty, td) &&
                 tx >= cl && tx <= cr && by >= ct && ty <= cb;
            const float cx0 = (bx + tx) * 0.5f, cy0 = (by + ty) * 0.5f;
            const float half = FMax(std::fabs(by - ty) * 0.575f, 11.0f);
            bl = cx0 - half; br = cx0 + half; bt = cy0 - half; bb = cy0 + half;
        }

        const float rng = std::sqrt((wx - PilotLoc[0] * 0.01f) * (wx - PilotLoc[0] * 0.01f) +
                                    (wy - PilotLoc[1] * 0.01f) * (wy - PilotLoc[1] * 0.01f) +
                                    (wz - PilotLoc[2] * 0.01f) * (wz - PilotLoc[2] * 0.01f));
        wchar_t hdr[64];
        swprintf_s(hdr, L"LOCK  %d m", (int)(rng + 0.5f));

        // The plate's size, worked out as the cue panel works it out.
        const float scale = 2.8f; // 3.5 was 20% too big
        const float dollW = 10.0f * scale, dollH = 16.0f * scale;
        const int   lines = FocusedLineCount(*info);
        const float textH = 13.0f + 12.0f * (float)(lines - 1) + 4.0f;
        const float bodyH = FMax(dollH + (info->HaveRear ? 3.0f * scale : 0.0f), textH);
        const float plateW = dollW + 10.0f + 150.0f, plateH = bodyH + 17.0f;
        const float stalk = 34.0f, edge = 8.0f;

        if (on)
        {
            // Acquisition bracket: four corners around the unit - red on an enemy, cyan on a friend.
            const float l = bl, r = br, t = bt, b = bb;
            const float cx = (l + r) * 0.5f;
            const float arm = FMin(r - l, b - t) * 0.225f + 2.0f;
            Col(bracketCol, 0.95f);
            if (MlLocked)
            {
                // MISSILE LOCK: the four corners close into one solid square, same line weight.
                RT->DrawRectangle(D2D1::RectF(l, t, r, b), Brush, 2.0f);
            }
            else
            {
                const D2D1_POINT_2F corners[4][3] = {
                    {{l, t + arm}, {l, t}, {l + arm, t}}, {{r - arm, t}, {r, t}, {r, t + arm}},
                    {{r, b - arm}, {r, b}, {r - arm, b}}, {{l + arm, b}, {l, b}, {l, b - arm}}};
                for (auto &c : corners)
                {
                    RT->DrawLine(c[0], c[1], Brush, 2.0f);
                    RT->DrawLine(c[1], c[2], Brush, 2.0f);
                }
                if (MlAcquiring)
                {
                    // Acquiring: a thin square closing in on the bracket as the lock builds -
                    // two and a half times its size at the start, on it at 100 %.
                    const float k = 1.0f + 1.5f * (1.0f - MlProgress);
                    const float cxm = (l + r) * 0.5f, cym = (t + b) * 0.5f;
                    Col(bracketCol, 0.85f);
                    RT->DrawRectangle(D2D1::RectF(cxm - (cxm - l) * k, cym - (cym - t) * k,
                                                  cxm + (r - cxm) * k, cym + (b - cym) * k), Brush, 1.0f);
                    Col(bracketCol, 0.95f);
                }
            }

            // The plate above on a stalk - below instead when there is no room above.
            float left = Clampf(cx - plateW * 0.5f, cl + edge, cr - edge - plateW);
            float top = t - stalk - plateH;
            float stalkFrom = t, stalkTo = t - stalk;
            if (top < ct + edge + 17.0f)
            {
                top = b + stalk;
                stalkFrom = b;
                stalkTo = b + stalk;
            }
            Col(col, 0.8f);
            RT->DrawLine(D2D1::Point2F(cx, stalkFrom), D2D1::Point2F(cx, stalkTo), Brush, 1.2f);
            DrawLockPlate(left, top, plateW, plateH, dollW, dollH, scale, *info, col, hdr);
        }
        else
        {
            // Off screen: the plate waits bottom-left and says which way to turn.
            const float left = cl + edge + 4.0f, top = cb - edge - plateH;
            DrawLockPlate(left, top, plateW, plateH, dollW, dollH, scale, *info, col, hdr);
            const float yawTo = std::atan2(wy - PilotLoc[1] * 0.01f, wx - PilotLoc[0] * 0.01f) * 57.29577951f;
            float off = yawTo - PilotRot[1];
            while (off > 180.0f) off -= 360.0f;
            while (off < -180.0f) off += 360.0f;
            Label((off > 0.0f) ? L"OFF SCREEN >" : L"< OFF SCREEN", left + plateW + 10.0f, top - 2.0f, 140.0f, col, 0.9f);
        }

        RT->PopAxisAlignedClip();
        const HRESULT hr = RT->EndDraw();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET)
        {
            SafeRelease(Brush);
            RT = nullptr;
        }
    }

    void DrawLockPlate(float x0, float top0, float plateW, float plateH, float dollW, float dollH, float scale,
                       const TerrainMechInfo &info, uint32_t col, const wchar_t *hdr)
    {
        const float hdrY = top0 + 2.0f;
        const float top = hdrY + 17.0f;
        const float bottom = top0 + plateH;
        const float textX = x0 + dollW + 10.0f;
        const float right = x0 + plateW;
        DrawPlate(D2D1::RectF(x0, hdrY, right, bottom), 4.0f, 0.82f);
        Col(col, 0.9f);
        RT->DrawRectangle(D2D1::RectF(x0 - 4.0f, hdrY - 4.0f, right + 4.0f, bottom + 4.0f), Brush, 1.2f);
        Label(hdr, x0, hdrY - 2.0f, right - x0, kColText, 0.95f);
        DrawDoll(x0 + dollW * 0.5f, top, scale, info, 1.0f);
        DrawRearStrip(x0 + dollW * 0.5f, top + dollH + 2.0f * scale, dollW, scale, info, 1.0f);
        DrawMechLabel(textX - 12.0f, top, info, col, 1.0f, false);
        FocusWeapons = info.WeaponCount;
        strcpy_s(FocusName, info.Variant[0] ? info.Variant : info.Chassis);
    }

    void DrawRounds(const Mat4 &vp, const LidarScene &scene)
    {
        // Rounds, styled exactly as the scope styles them - same colours, same radii, same
        // 0.45-to-0 smoke fade, same 4x amber swell on the salvo guiding at us. The decisions
        // (extrapolated position, streak origin, flash phase, dead rounds dropped) were all made
        // in LidarView::BuildScene, so this only draws them. The one thing that is genuinely
        // different is that every point carries its real Z, so an LRM arc looks like an arc.
        for (int i = 0; i < scene.ProjectileCount; ++i)
        {
            const LidarSceneProjectile &p = scene.Projectiles[i];
            float sx, sy, d;
            if (!Project(vp, p.X * 0.01f, p.Y * 0.01f, p.Z * 0.01f, sx, sy, d))
                continue;

            uint32_t col = (p.ColorIdx == 2) ? kColProjMissile
                         : (p.ColorIdx == 1) ? kColProjPPC
                                             : kColProjBallistic;
            if (p.Flashing)
                col = kColProjBound;

            if (p.IsMissile)
            {
                // Fading grey smoke: oldest segment faintest, newest brightest, 0.45 at the head.
                if (p.TrailCount >= 2 && scene.Trail)
                {
                    for (int k = 1; k < p.TrailCount; ++k)
                    {
                        const LidarSceneTrailPt &t0 = scene.Trail[p.TrailStart + k - 1];
                        const LidarSceneTrailPt &t1 = scene.Trail[p.TrailStart + k];
                        if (t1.Break)
                            continue; // gap the scope would not have bridged either
                        float ax, ay, ad, bx, by, bd;
                        if (!Project(vp, t0.X * 0.01f, t0.Y * 0.01f, t0.Z * 0.01f, ax, ay, ad))
                            continue;
                        if (!Project(vp, t1.X * 0.01f, t1.Y * 0.01f, t1.Z * 0.01f, bx, by, bd))
                            continue;
                        Col(kColSmoke, 0.45f * (float)k / (float)(p.TrailCount - 1));
                        RT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), Brush, 1.4f);
                    }
                }
                // Arrow IV reads 3x. A round guiding on us swells 4x as well as going amber,
                // because size is what carries peripherally - amber against red at 1.1 px is a
                // contrast nobody catches on a busy scope.
                float r = p.IsArrowIV ? 3.3f : 1.1f;
                if (PilotOn)
                    r *= 2.0f; // V-POD / CAP: read at a glance over the camera image (2026-10-08)
                if (p.Flashing)
                    r *= kSalvoFlashScale;
                Col(col, 0.95f);
                RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), r, r), Brush);
            }
            else
            {
                // Ballistic / PPC: short coloured streak plus a dot. In the pilot view (V-POD, CAP)
                // they are drawn much heavier - a PPC bolt as a 3 px streak with a 3.5 px head -
                // because 1 px over a camera image could not be seen (2026-10-08).
                const bool ppc = p.ColorIdx == 1;
                const float lw = PilotOn ? (ppc ? 3.0f : 2.0f) : 0.9f;
                const float dr = PilotOn ? (ppc ? 3.5f : 2.2f) : 1.1f;
                Col(col, 0.95f);
                if (p.HasStreak)
                {
                    float fx2, fy2, fd;
                    if (Project(vp, p.FromX * 0.01f, p.FromY * 0.01f, p.FromZ * 0.01f, fx2, fy2, fd))
                        RT->DrawLine(D2D1::Point2F(fx2, fy2), D2D1::Point2F(sx, sy), Brush, lw);
                }
                RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), dr, dr), Brush);
            }
        }

    }

    // contact facing triangles - is drawn against the camera.
    void DrawOverlay(const Mat4 &vp, const LidarScene &scene, float px, float py, float pz, float yaw,
                     float camYawDeg, float horizonM, bool haveTerrain)
    {
        if (!RT || !Brush)
            return;
        RT->BeginDraw();

        const D2D1_RECT_F content = D2D1::RectF(Width * Inset, Height * Inset, Width * (1.0f - Inset),
                                                Height * (1.0f - Inset));
        RT->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_ALIASED);

        // The pilot view is the ground and what moves over it: rounds and smoke, in green. The
        // units themselves come from the game's own meshes (the depth-outline capture laid over
        // this page), so no chevrons, labels, dolls, arcs or cue panel are drawn here.
        if (PilotOn)
        {
            Mono = true;
            DrawRounds(vp, scene);
            Mono = false;
            RT->PopAxisAlignedClip();
            const HRESULT phr = RT->EndDraw();
            if (phr == (HRESULT)D2DERR_RECREATE_TARGET)
            {
                SafeRelease(Brush);
                RT = nullptr;
            }
            return;
        }

        // The arc hangs off the TORSO, not the camera: camYawDeg includes the pilot's orbit.
        ArcPX = px; ArcPY = py;
        ArcTorsoDeg = CockpitYawDeg;

        // Silhouettes first: they are the same objects the contact markers describe, so the
        // markers - selection rings, range labels, the aim ray - must paint over them.
        if (haveTerrain)
            DrawSilhouettes(vp, px, py, horizonM);

        // Ping sweep: a ring on the GROUND, sampled around the circle and lifted onto the
        // terrain under each point. On the flat scope this was an ellipse; here it drapes over
        // the hills, which is the whole reason it is worth having in 3D.
        if (scene.Valid && (scene.Layers & (1u << 1)) && scene.PulseCm > 0.0f && scene.PulseCm <= scene.RangeCm)
        {
            const float r = scene.PulseCm * 0.01f;
            const float fade = 1.0f - (scene.PulseCm / FMax(1.0f, scene.RangeCm));
            Col(kColSweep, 0.12f + 0.60f * fade);
            bool started = false;
            float lx = 0, ly = 0;
            for (int i = 0; i <= 72; ++i)
            {
                const float a = (float)i * (6.2831853f / 72.0f);
                const float wx = px + std::cos(a) * r, wy = py + std::sin(a) * r;
                float gz = pz;
                if (haveTerrain)
                    GroundAtM(wx, wy, gz);
                float sx, sy, d;
                if (!Project(vp, wx, wy, gz + 2.0f, sx, sy, d)) { started = false; continue; }
                if (started)
                    RT->DrawLine(D2D1::Point2F(lx, ly), D2D1::Point2F(sx, sy), Brush, 2.0f);
                lx = sx; ly = sy; started = true;
            }
        }

        // Torso aim ray, out along the twist, lying on the ground.
        if (scene.Valid && (scene.Layers & (1u << 2)))
        {
            const float a = (scene.PlayerYaw + scene.PlayerTorsoYaw) * 0.01745329252f;
            Col(kColSweep, 0.85f);
            bool started = false;
            float lx = 0, ly = 0;
            for (int i = 0; i <= 24; ++i)
            {
                const float d0 = horizonM * (float)i / 24.0f;
                const float wx = px + std::cos(a) * d0, wy = py + std::sin(a) * d0;
                float gz = pz;
                if (haveTerrain)
                    GroundAtM(wx, wy, gz);
                float sx, sy, dd;
                if (!Project(vp, wx, wy, gz + 1.0f, sx, sy, dd)) { started = false; continue; }
                if (started)
                    RT->DrawLine(D2D1::Point2F(lx, ly), D2D1::Point2F(sx, sy), Brush, 1.4f);
                lx = sx; ly = sy; started = true;
            }
        }

        DrawRounds(vp, scene);

        // Contacts, far to near. Each gets a stalk to the ground beneath it, which is the only
        // thing that makes altitude readable in perspective - a VTOL at 200 m and a mech on a
        // ridge project to almost the same pixel, and the stalk is what tells them apart.
        LabelsDrawn = 0;
        InArcDrawn = 0;
        DollsFromArt = 0;
        CandCount = 0;
        RefreshDeadIds();
        for (int i = 0; i < GhostCount; ++i) Ghosts[i].Live = false;
        CulledFar = CulledOffScreen = NoRecord = MechContacts = VehicleContacts = 0;
        ArtMisses = 0;
        FocusWeapons = -1;
        FocusName[0] = '\0';

        for (int pass = 0; pass < 3; ++pass)
            for (int i = 0; i < scene.ContactCount; ++i)
            {
                const LidarSceneContact &c = scene.Contacts[i];
                const float wx = c.X * 0.01f, wy = c.Y * 0.01f, wz = c.Z * 0.01f;
                const float dx = wx - px, dy = wy - py;
                const uint32_t col = (c.Type == CT_HOSTILE)  ? kColHostile
                                   : (c.Type == CT_FRIENDLY) ? kColFriendly
                                   : (c.Type == CT_NEUTRAL)  ? kColNeutral
                                                             : kColUnknown;

                // Kills. A dead record draws grey with no stalk, doll or name for its 5 s hold;
                // once the record is gone, the scope's leftover track is not drawn at all.
                const TerrainMechInfo *rec = MechInfoFor(c.Id);
                const bool deadRec = rec && rec->Dead;
                if (!rec && IsDeadId(c.Id))
                    continue;

                // Remembered BEFORE the horizon and projection culls. A contact that is off
                // screen is still a contact, and freezing its position the moment it leaves the
                // view would strand its ghost somewhere it has long since walked away from.
                if (pass == 0)
                    NoteContact(c, col);
                // Clipped at the HORIZON, not at the zoom. Zooming the camera in is a request to
                // look closer, not a request to stop being told about a mech 800 m away.
                if (pass == 0)
                {
                    if (c.Kind == KIND_MECH) ++MechContacts; else ++VehicleContacts;
                }
                if (dx * dx + dy * dy > horizonM * horizonM)
                {
                    if (pass == 0) ++CulledFar;
                    continue;
                }
                float sx, sy, d;
                if (!Project(vp, wx, wy, wz, sx, sy, d))
                {
                    if (pass == 0) ++CulledOffScreen;
                    continue;
                }

                float gz = wz;
                const bool haveGround = haveTerrain && GroundAtM(wx, wy, gz);
                const float agl = haveGround ? (wz - gz) : 0.0f;

                const float alpha = Clampf(c.Alpha, 0.0f, 1.0f);

                if (pass == 0)
                {
                    if (!haveGround || deadRec)
                        continue;
                    float gx2, gy2, gd;
                    if (!Project(vp, wx, wy, gz, gx2, gy2, gd))
                        continue;
                    Col(col, 0.40f * alpha);
                    RT->DrawLine(D2D1::Point2F(gx2, gy2), D2D1::Point2F(sx, sy), Brush, 1.0f);
                    Col(col, 0.65f * alpha);
                    RT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(gx2, gy2), 6.0f, 2.6f), Brush, 1.0f);
                    continue;
                }

                // Mechs read big, everything else reads small. 8 against 6 was not a
                // difference you could see at a glance - a tank and an Atlas looked like the
                // same contact - and size is the cheapest channel available here, since colour
                // is already carrying team and the doll is already carrying condition. Turrets
                // are smallest of all: they cannot move, so they are the least urgent mark on
                // the page and should recede.
                // A QUARTER of the previous size. The chevron's job changed once the doll
                // arrived: it is no longer the thing you read, it is the ground anchor that
                // says where the doll's mech actually stands. The doll carries identity and
                // condition, so the marker only has to mark - and a small one sits under a
                // large doll without fighting it for attention.
                // The 25% pass was sized against the mech chevron and took the others with it:
                // a turret ended up under three pixels long, which is not "small", it is absent.
                // Mechs stay where they were; the rest are pulled back up to legible and keep a
                // clear size order - mech, vehicle, turret.
                const float s = (c.Kind == KIND_MECH)   ? 2.25f
                              : (c.Kind == KIND_TURRET) ? 1.50f
                              : (c.Kind == KIND_BA)     ? 1.00f // a suit: the smallest mark
                                                        : 1.80f;
                const float a = (c.Yaw - camYawDeg) * 0.01745329252f;
                const TerrainMechInfo *info = MechInfoFor(c.Id);

                if (pass == 1)
                {
                    // Thinner stroke too: at this size a 2.2 px line is wider than the arms
                    // are long, and the V closes up into a dot.
                    if (deadRec)
                    {
                        if (c.Kind == KIND_BA) Diamond(sx, sy, s, 0x5E666B, 0.75f);
                        else Chevron(sx, sy, a, s, 0x5E666B, 0.75f, (c.Kind == KIND_MECH) ? 1.25f : 0.9f);
                        continue;
                    }
                    if (c.Kind == KIND_BA) Diamond(sx, sy, s, col, alpha); // a suit: a diamond, as on the LIDAR scope
                    else Chevron(sx, sy, a, s, col, alpha, (c.Kind == KIND_MECH) ? 1.25f : 0.9f);

                    // The lock ring follows the same CurrentTarget flag as the cue panel, so
                    // the two can never disagree about what is locked.
                    if (info && info->Locked)
                    {
                        Col(kColSelected, alpha);
                        RT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), s + 5.0f, s + 5.0f), Brush, 1.4f);
                    }
                    if (haveGround && agl > 40.0f && !info)
                    {
                        wchar_t b[32];
                        swprintf_s(b, L"+%dm", (int)agl);
                        Label(b, sx + 11.0f, sy - 8.0f, 70.0f, col, alpha);
                    }
                    continue;
                }

                // pass 2: the doll and the label, drawn over every chevron so a near contact's
                // text is never buried under a far one's marker.
                // Counted so a missing doll can be told apart from a missing RECORD. Without
                // this the log says "9 known, 4 labelled" and cannot say whether the other five
                // were off screen, out of range, or simply never published.
                if (!info)
                    ++NoRecord;
                if (!info || deadRec)
                    continue;

                // pass 2 COLLECTS rather than draws. In scene order a contact 900 m away
                // could lay its label across one at 80 m, and with forty units on the page the
                // text became a single unreadable mat. Collecting first lets the draw run
                // nearest-first, which is the order that should win the space.
                if (CandCount < (int)(sizeof(Cands) / sizeof(Cands[0])))
                {
                    LabelCand &q = Cands[CandCount++];
                    q.X = sx; q.Y = sy; q.D = d; q.S = s;
                    q.Info = info; q.Col = col;
                    q.Shown = (ShownId && info->Id == ShownId) || info->Locked; // it has a plate
                }
            }

        DrawGhosts(vp, px, py, horizonM, camYawDeg);
        DrawLabelLayer();

        // ---- the mech: legs and torso, which point in different directions -----------------
        //
        // A single symmetric marker cannot say which way the mech is FACING, and in a BattleMech
        // that is two answers, not one: the legs carry you and the torso carries the guns. On the
        // flat scope the body is always screen-up so only the twist needed drawing; here the
        // camera can be anywhere, so both are drawn explicitly.
        //
        //   legs  - a triangle flat on the ground, pointing along the body heading
        //   torso - an arrow from the same origin along body + twist
        //
        // Both are built in WORLD space on the ground plane so they foreshorten with the camera
        // and read as planted rather than pasted on. Their world size scales with range, which
        // keeps them a roughly constant number of pixels at every zoom - a fixed world size would
        // be a speck at 1000 m and swallow the screen at 100 m.
        {
            float gz = pz;
            if (haveTerrain)
                GroundAtM(px, py, gz);
            gz += 1.0f; // just clear of the mesh, so the marker is never z-fought by a cell edge

            // Scaled by the ZOOM, not the horizon: these mark where the mech is, so they should
            // stay a constant size on screen as the camera moves in and out.
            const float legLen = (float)RangeM * 0.016f;
            const float torLen = (float)RangeM * 0.055f;
            const float bodyA = yaw * 0.01745329252f;
            // camYawDeg already IS body + twist; backing out the pilot's orbit offset leaves the
            // absolute cockpit heading. Deriving it rather than re-adding scene.PlayerTorsoYaw
            // keeps the marker and the camera provably in agreement - and keeps this working
            // when the LIDAR publisher is down and the torso angle came from the terrain block.
            const float torsoA = CockpitYawDeg * 0.01745329252f;

            auto P2 = [&](float a, float fwd, float side, float &ox, float &oy) -> bool {
                const float fx2 = std::cos(a), fy2 = std::sin(a);
                const float rx = -std::sin(a), ry = std::cos(a);
                float dd;
                return Project(vp, px + fx2 * fwd + rx * side, py + fy2 * fwd + ry * side, gz, ox, oy, dd);
            };

            // Legs.
            float ax, ay, bx, by, cx2, cy2;
            const bool haveTri = P2(bodyA, legLen, 0.0f, ax, ay) &&
                                 P2(bodyA, -legLen * 0.55f, legLen * 0.62f, bx, by) &&
                                 P2(bodyA, -legLen * 0.55f, -legLen * 0.62f, cx2, cy2);
            if (haveTri)
            {
                Col(kColPlayer, 1.0f);
                RT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), Brush, 2.0f);
                RT->DrawLine(D2D1::Point2F(bx, by), D2D1::Point2F(cx2, cy2), Brush, 2.0f);
                RT->DrawLine(D2D1::Point2F(cx2, cy2), D2D1::Point2F(ax, ay), Brush, 2.0f);
            }

            // Torso, in the scope's aim-line cyan so the two directions never read as one thing.
            TorsoArrowOk = false;
            float ox, oy, tx2, ty2, h1x, h1y, h2x, h2y;
            if (P2(torsoA, 0.0f, 0.0f, ox, oy) && P2(torsoA, torLen, 0.0f, tx2, ty2) &&
                P2(torsoA, torLen * 0.72f, torLen * 0.20f, h1x, h1y) &&
                P2(torsoA, torLen * 0.72f, -torLen * 0.20f, h2x, h2y))
            {
                Col(kColSweep, 0.95f);
                RT->DrawLine(D2D1::Point2F(ox, oy), D2D1::Point2F(tx2, ty2), Brush, 2.0f);
                RT->DrawLine(D2D1::Point2F(h1x, h1y), D2D1::Point2F(tx2, ty2), Brush, 2.0f);
                RT->DrawLine(D2D1::Point2F(h2x, h2y), D2D1::Point2F(tx2, ty2), Brush, 2.0f);
                TorsoArrowOk = true;
                TorsoArrow[0] = ox; TorsoArrow[1] = oy; TorsoArrow[2] = tx2; TorsoArrow[3] = ty2;
                if (Tac)
                {
                    // TacOps: the tip is a handle - drag it to turn the torso.
                    Col(kColSweep, 0.95f);
                    RT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(tx2, ty2), 6.0f, 6.0f), Brush, 1.5f);
                    Col(kColSweep, 0.35f);
                    RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx2, ty2), 5.0f, 5.0f), Brush);
                }
            }
        }

        DrawDropships(vp);        // DROPSHIPS (2026-10-08): the 3D page and TacOps alike
        DrawNav(vp, haveTerrain, px, py); // NAV POINTS (2026-10-07k): the 3D page and TacOps alike; off-page pinned (2026-10-08)
        if (Tac)
            DrawTac(vp, haveTerrain);
        DrawCuePanel(scene, vp);

        RT->PopAxisAlignedClip();
        Col(kColText, 0.55f);
        RT->DrawRectangle(content, Brush, 1.0f);

        wchar_t b[224];
        const float tx = Width * Inset + 4.0f;
        float ty = Height * Inset + 2.0f;
        if (Tac)
            swprintf_s(b, L"TACOPS   zoom %u m   %s", RangeM, Free ? L"FREE" : L"CHASE");
        else
            swprintf_s(b, L"3D   zoom %u m   horizon %.0f m   cell %d m   %s", RangeM, horizonM, GetCellM(),
                       Free ? L"FREE" : L"CHASE");
        Label(b, tx, ty, Width * 0.6f, kColText, 0.95f);
        ty += 17.0f;
        swprintf_s(b, L"     %d tiles / %u lines   %d contacts   %d rounds", TilesDrawn, LinesDrawn,
                   scene.ContactCount, scene.ProjectileCount);
        Label(b, tx, ty, Width * 0.6f, kColText, 0.8f);
        if (!scene.Valid)
        {
            ty += 17.0f;
            Label(L"     LIDAR publisher not advancing - terrain only", tx, ty, Width * 0.7f, kColNeutral, 0.9f);
        }

        const HRESULT hr = RT->EndDraw();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET)
        {
            SafeRelease(Brush);
            RT = nullptr;
        }
    }

    // ---- state --------------------------------------------------------------------------------

    ID3D11Device *Dev = nullptr;
    ID3D11DeviceContext *Ctx = nullptr;
    ID3D11RenderTargetView *RTV = nullptr;
    ID3D11Texture2D *DepthTex = nullptr;
    ID3D11Texture2D *DepthTexAlt = nullptr; // bug sweep 21: the other size's depth buffer
    ID3D11DepthStencilView *DSVAlt = nullptr;
    unsigned AltW = 0, AltH = 0;
    ID3D11DepthStencilView *DSV = nullptr;
    ID3D11VertexShader *VS = nullptr;
    ID3D11PixelShader *PS = nullptr;
    ID3D11InputLayout *Layout = nullptr;
    ID3D11Buffer *CBuf = nullptr;
    ID3D11RasterizerState *RsFill = nullptr, *RsLine = nullptr;
    ID3D11DepthStencilState *DsOn = nullptr;
    ID3D11BlendState *BsAlpha = nullptr, *BsOpaque = nullptr, *BsNoColor = nullptr;
    bool Ready = false;

    ID2D1Factory *Factory = nullptr;
    IDWriteFactory *DW = nullptr;
    ID2D1RenderTarget *RT = nullptr;
    ID2D1SolidColorBrush *Brush = nullptr;
    IDWriteTextFormat *TextFmt = nullptr;
    IDWriteTextFormat *TextFmtSm = nullptr; // contact labels

    HANDLE Map = nullptr;
    const TerrainBlock *Shared = nullptr;
    double LastAttemptMs = 0.0;
    TerrainHeader Head{};
    TerrainHeader Scratch{};
    bool HeadValid = false;
    uint32_t BuiltGeneration = 0xFFFFFFFFu;

    unsigned Width = 0, Height = 0;
    float Inset = 0.10f;
    unsigned RangeM = 1000;
    int Stride = 2; // 32 m cells - see Topo3DView.h on why 3D wants finer than the flat page

    float YawOff = 0.0f, PitchDeg = Topo3DView::kDefaultPitchDeg;
    // Absolute camera heading as of the last Render: body + torso twist + YawOff. Pan reads it
    // between frames, which is why it is a member and not a local.
    float LastCamYawDeg = 0.0f;
    int   SilhouettesDrawn = 0;
   public:
    // RADAR MODE (2026-10-12): the host's hostile filter (Vanilla); null = Full LIDAR.
    Topo3DView::HostileKeepFn HostileKeep = nullptr;
    void *HostileCtx = nullptr;

   private:

    // ---- label state ---------------------------------------------------------------------
    // Ids whose record said Dead. Once the producer stops publishing the wreck, the scope may
    // still hold its track for a few seconds; this keeps that track from coming back as a live
    // red chevron. A live (Dead == 0) record for the id clears it - ids are reused after the
    // actor is gone - and a 30 s backstop clears the rest.
    struct DeadMark { uint16_t Id; double SeenMs; };
    DeadMark DeadIds[TERR_MAX_MECHS] = {};
    int      DeadCount = 0;

    // Fire-arc origin and heading for this frame, set at the top of DrawOverlay.
    float    ArcPX = 0.0f, ArcPY = 0.0f, ArcTorsoDeg = 0.0f;
    int      InArcDrawn   = 0;
    // Pilot camera (see Render).
    bool     PilotOn = false, Mono = false;
    float    PilotLoc[3] = {}, PilotRot[3] = {}, PilotHFovDeg = 90.0f;
    uint16_t LockedIdNow  = 0;
    int      LabelsDrawn  = 0;
    int      DollsFromArt = 0; // dolls drawn as real chassis art rather than boxes
    int      CulledFar = 0, CulledOffScreen = 0, NoRecord = 0;
    int      MechContacts = 0, VehicleContacts = 0;
    int      ArtMisses = 0;
    char     MissingArt[32] = {};
    int      FocusWeapons = -1;   // weapon count on whatever is focused, -1 when nothing is
    char     FocusName[32] = {};
    ID2D1StrokeStyle *ChevronStroke = nullptr;
    ID2D1StrokeStyle *LeaderStroke  = nullptr; // dotted, for a displaced label's tether

    float FocusZSmooth = 0.0f;
    bool FocusZValid = false;
    float PanX = 0.0f, PanY = 0.0f;
    bool Free = false;

    // ---- TacOps --------------------------------------------------------------------------
    const Topo3DView::TacOpsView *Tac = nullptr;
    Topo3DView::NavMark NavPts[16] = {};
    int                 NavN = 0;
    float CockpitYawDeg = 0.0f;
    float LastPlayerM[3] = {};          // 2026-10-12: where the mech was in the last frame (m)
    bool  LastPlayerValid = false;         // body + torso twist, whatever the camera is doing
    Mat4  LastVp{};
    bool  LastVpValid = false;
    V3    TacEye{}, TacF{}, TacR{}, TacU{};
    float TacTanY = 0.4663f, TacAspect = 1.0f, TacGroundZ = 0.0f, TacHorizonM = 1000.0f;
    bool  TacCamValid = false;
    struct TacPanel { D2D1_RECT_F R; int Slot; };

  public:
    // bug sweep 21 (2026-10-18): the Enhanced Imaging fallback renders the pilot view through this same view between two
    // of the window's frames. SaveView / RestoreView put the window's camera back afterwards, so TacOps clicks
    // (TacPickGround / TacProject / TacHostileAt), the compass and Pan use the window's camera, not the pilot's.
    struct ViewSnap
    {
        Mat4  LastVp{}, PilotVp{};
        bool  LastVpValid = false, PilotVpValid = false, TacCamValid = false, LastPlayerValid = false, FocusZValid = false;
        V3    TacEye{}, TacF{}, TacR{}, TacU{};
        float TacTanY = 0, TacAspect = 0, TacGroundZ = 0, TacHorizonM = 0, LastCamYawDeg = 0, CockpitYawDeg = 0;
        float FocusZSmooth = 0, VertEx = kVertExChase;
        float LastPlayerM[3] = {};
        float PilotBoxes[kMaxPilotBoxes][4] = {};
        int   PilotBoxCount = 0;
        Tile *Tiles = nullptr;
        const Topo3DView::TacOpsView *Tac = nullptr;
        TacPanel Panels[8] = {};
        int   PanelCount = 0;
        D2D1_RECT_F OwnPlate = {};
        bool  OwnPlateValid = false;
    } Snap;
    void SaveView()
    {
        Snap.LastVp = LastVp; Snap.PilotVp = PilotVp;
        Snap.LastVpValid = LastVpValid; Snap.PilotVpValid = PilotVpValid; Snap.TacCamValid = TacCamValid;
        Snap.LastPlayerValid = LastPlayerValid; Snap.FocusZValid = FocusZValid;
        Snap.TacEye = TacEye; Snap.TacF = TacF; Snap.TacR = TacR; Snap.TacU = TacU;
        Snap.TacTanY = TacTanY; Snap.TacAspect = TacAspect; Snap.TacGroundZ = TacGroundZ; Snap.TacHorizonM = TacHorizonM;
        Snap.LastCamYawDeg = LastCamYawDeg; Snap.CockpitYawDeg = CockpitYawDeg;
        Snap.FocusZSmooth = FocusZSmooth; Snap.VertEx = kVertEx;
        std::memcpy(Snap.LastPlayerM, LastPlayerM, sizeof(LastPlayerM));
        std::memcpy(Snap.PilotBoxes, PilotBoxes, sizeof(PilotBoxes));
        Snap.PilotBoxCount = PilotBoxCount;
        Snap.Tiles = Tiles;
        Snap.Tac = Tac;
        std::memcpy(Snap.Panels, TacPanels, sizeof(TacPanels));
        Snap.PanelCount = TacPanelCount;
        Snap.OwnPlate = OwnPlate;
        Snap.OwnPlateValid = OwnPlateValid;
    }
    void RestoreView()
    {
        if (!Snap.Tiles)
            return;
        LastVp = Snap.LastVp; PilotVp = Snap.PilotVp;
        LastVpValid = Snap.LastVpValid; PilotVpValid = Snap.PilotVpValid; TacCamValid = Snap.TacCamValid;
        LastPlayerValid = Snap.LastPlayerValid; FocusZValid = Snap.FocusZValid;
        TacEye = Snap.TacEye; TacF = Snap.TacF; TacR = Snap.TacR; TacU = Snap.TacU;
        TacTanY = Snap.TacTanY; TacAspect = Snap.TacAspect; TacGroundZ = Snap.TacGroundZ; TacHorizonM = Snap.TacHorizonM;
        LastCamYawDeg = Snap.LastCamYawDeg; CockpitYawDeg = Snap.CockpitYawDeg;
        FocusZSmooth = Snap.FocusZSmooth; kVertEx = Snap.VertEx;
        std::memcpy(LastPlayerM, Snap.LastPlayerM, sizeof(LastPlayerM));
        std::memcpy(PilotBoxes, Snap.PilotBoxes, sizeof(PilotBoxes));
        PilotBoxCount = Snap.PilotBoxCount;
        Tiles = Snap.Tiles;
        Tac = Snap.Tac;
        std::memcpy(TacPanels, Snap.Panels, sizeof(TacPanels));
        TacPanelCount = Snap.PanelCount;
        OwnPlate = Snap.OwnPlate;
        OwnPlateValid = Snap.OwnPlateValid;
        Snap.Tiles = nullptr;
    }

  private:
    TacPanel TacPanels[8] = {};
    int   TacPanelCount = 0;
    D2D1_RECT_F OwnPlate = {};
    bool  OwnPlateValid = false;

    bool HaveRange = false;
    float MinZm = 0.0f, MaxZm = 0.0f;

    int TilesDrawn = 0;
    uint32_t LinesDrawn = 0;
    int LastContacts = 0, LastProjectiles = 0;
    int LastTrails = 0, LastFlashing = 0;
    float LastBuildMs = 0.0f;
};

// ===========================================================================================

Topo3DView::Topo3DView() : P(new Topo3DViewImpl()) {}
Topo3DView::~Topo3DView()
{
    if (P)
    {
        P->Shutdown();
        delete P;
        P = nullptr;
    }
}

bool Topo3DView::InitResources(ID3D11Device *d, ID3D11DeviceContext *c, ID2D1Factory *f, IDWriteFactory *dw)
{
    return P->InitResources(d, c, f, dw);
}
void Topo3DView::SetTargets(ID3D11RenderTargetView *rtv, ID2D1RenderTarget *d2d, unsigned w, unsigned h)
{
    P->SetTargets(rtv, d2d, w, h);
}
void Topo3DView::SetContentInset(float i) { P->SetContentInset(i); }
void Topo3DView::SetRangeM(unsigned m) { P->SetRangeM(m); }
void Topo3DView::SetCellM(int m) { P->SetCellM(m); }
void Topo3DView::SetPilotCamera(bool on, const float *locCm, const float *rotDeg, float hfovDeg)
{
    P->SetPilotCamera(on, locCm, rotDeg, hfovDeg);
}
void Topo3DView::Poll() { P->Poll(); }
void Topo3DView::Render(const LidarScene &s) { P->Render(s); }
void Topo3DView::RenderPilotLock(const LidarScene &s) { P->PilotLock(s); }
void Topo3DView::SetPilotBeams(int n, const float *fromTo) { P->SetPilotBeams(n, fromTo); }
void Topo3DView::RenderPilotBeams() { P->RenderPilotBeams(); }
int Topo3DView::PilotUnitBoxes(float (*out)[4], int max) const { return P->PilotUnitBoxes(out, max); }
void Topo3DView::SetPilotBare(bool bare) { P->SetPilotBare(bare); }
void Topo3DView::SetTransparent(bool on) { P->SetTransparent(on); }
void Topo3DView::SetPilotMissileLock(bool acquiring, float progress, bool locked)
{
    P->SetPilotMissileLock(acquiring, progress, locked);
}
void Topo3DView::SetPilotLiveLock(bool valid, const float *originCm, const float *extentCm)
{
    P->SetPilotLiveLock(valid, originCm, extentCm);
}
void Topo3DView::SetTacOps(const TacOpsView *v) { P->SetTacOps(v); }
void Topo3DView::RenderLancePage(const LancePageView &v) { P->RenderLancePage(v); } // V-LNC
void Topo3DView::SaveView() { P->SaveView(); }       // bug sweep 21
void Topo3DView::RestoreView() { P->RestoreView(); } // bug sweep 21
void Topo3DView::SetNavPoints(const NavMark *pts, int n) { P->SetNavPoints(pts, n); }
int Topo3DView::GetDropships(DropshipMark *out, int max) const { return P->GetDropships(out, max); }
void Topo3DView::TacRecentre() { P->TacRecentre(); }
bool Topo3DView::TacPickGround(float sx, float sy, float outCm[3]) const { return P->TacPickGround(sx, sy, outCm); }
bool Topo3DView::TacProject(const float cm[3], float &sx, float &sy) const { return P->TacProject(cm, sx, sy); }
int Topo3DView::TacHostileAt(float sx, float sy, float r, float outCm[3]) const { return P->TacHostileAt(sx, sy, r, outCm); }
float Topo3DView::CompassHeadingDeg() const { return P->CompassHeading(); }
bool Topo3DView::CompassPlayerCm(float out[3]) const { return P->PlayerCm(out); }
int Topo3DView::CompassHostilesCm(float (*out)[3], int max) const { return P->HostilesCm(out, max); }
void Topo3DView::SetHostileFilter(HostileKeepFn fn, void *ctx)
{
    P->HostileKeep = fn;
    P->HostileCtx = ctx;
}
int Topo3DView::TacNearestHostile(const float f[3], float maxM, float outCm[3]) const
{
    return P->TacNearestHostile(f, maxM, outCm);
}
int Topo3DView::TacPanelAt(float sx, float sy) const { return P->TacPanelAt(sx, sy); }
float Topo3DView::TacPanelLeft() const { return P->TacPanelLeft(); }
bool Topo3DView::TacOwnPlate(float &l, float &t, float &r, float &b) const { return P->TacOwnPlate(l, t, r, b); }
bool Topo3DView::TacTorsoArrow(float &ox, float &oy, float &tx, float &ty) const { return P->TacTorsoArrow(ox, oy, tx, ty); }
void Topo3DView::TacPanWorld(float dx, float dy) { P->TacPanWorld(dx, dy); }
bool Topo3DView::TacContactLoc(int id, float outCm[3]) const { return P->TacContactLoc(id, outCm); }
bool Topo3DView::TacContactActor(int id, uint32_t &lo, uint32_t &hi) const { return P->TacContactActor(id, lo, hi); }
bool Topo3DView::TacGroundAt(float x, float y, float &z) const { return P->TacGroundAt(x, y, z); }
void Topo3DView::Shutdown() { P->Shutdown(); }
bool Topo3DView::IsConnected() const { return P->IsConnected(); }
void Topo3DView::Orbit(float y, float p) { P->Orbit(y, p); }
void Topo3DView::Pan(float f, float r) { P->Pan(f, r); }
void Topo3DView::Recentre() { P->Recentre(); }
bool Topo3DView::IsFree() const { return P->IsFree(); }
int Topo3DView::PickContact(float sx, float sy, float radiusPx) const { return P->PickContact(sx, sy, radiusPx); }
void Topo3DView::SetShownContact(int id) { P->ShownId = (uint16_t)id; }
double Topo3DView::TakeArtLoadMs() { return P->TakeArtMs(); }
int Topo3DView::ShownContact() const { return P->ShownId; }

Topo3DView::Diag Topo3DView::GetDiag() const
{
    Diag d = {};
    P->GetDiag(d);
    return d;
}
