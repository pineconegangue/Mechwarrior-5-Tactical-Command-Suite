// EnhancedImaging.dll - the Enhanced Imaging vision mode, rendered INSIDE the game (2026-10-08, VHUD v23).
//
// HUDDisplay's Enhanced Imaging page, hosted in the game process so the vision mode needs no app running:
// the same 3D page (Topo3DView), the same scene (LidarView), the same outline shader, crosshair, lock
// bracket, plate and missile lock - drawn on this plugin's OWN Direct3D 11 device on its OWN thread (the
// game's device and its state are never touched), at the game's back-buffer size, into a shared image.
// CameraCapture draws that image over the game (with the depth wave), exactly as it does HUDDisplay's.
//
//   Switch:  Block->EiVisionOn - its hotkey here (GlobalSettings [hud] ei_vision_key), HUDDisplay's E-VIS
//            bezel action, or VoiceCommand ("activate / deactivate enhanced imaging").
//   Renders: while Block->EiVisWant (CameraCapture: on, or the closing wave still running).
//   Heartbeat: Block->EiVisPluginSec every loop - while it is fresh HUDDisplay leaves the vision mode to us.
//   Art:     paperdolls\ and assets\ from the apps folder GlobalSettingsPath.txt points at (AssetDirOverride).
//
// The host part below mirrors HUDDisplay Main.cpp (PrepareTargeting, DrawTargetingOverlays, DrawSrv, the
// crosshair, the missile lock). Keep the two in step - in particular kDepthEdgePs is a copy of HUDDisplay's
// psDepthEdgeCode.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <Plugin.hpp>
#include <VanillaHudShared.h>
#include <GsPointer.h>

#include "../hud_display/LidarView.h"
#include "../hud_display/Topo3DView.h"
#include "../hud_display/AssetRoot.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace uevr;

static const char kBuild[] =
    "BUILD=enhancedimaging-11 (EXIT CRASH 2026-10-18: the plugin object is never destroyed - its destructor released Direct2D / D3D objects at process exit, after NVIDIA's driver layers had gone) (enhancedimaging-10 BUG SWEEP 2026-10-17: the depth image only taken with its keyed mutex really held - a timeout counted as success; the heartbeat only while the plugin can render) (enhancedimaging-9 CENTRE CROSSHAIR TOGGLE: [hud] ei_crosshair, GlobalSettings > HUD > VISION, default on, 2026-10-17) (enhancedimaging-8 BATTLE ARMOUR: Topo3DView draws a suit as a diamond with an ELEMENTAL label + HP from its record, 2026-10-17) (enhancedimaging-7 ART BUDGET: paperdoll PNGs load 2 per 16 ms, not a whole chassis in one frame; slow-frame lines say how much was paperdoll loading) (enhancedimaging-6 with the .pak: the lock bracket on the live bounds and the beams, published by CameraCapture with the camera - were the LIDAR scan position / none; with the Enhanced Imaging .pak running (VHUD EiPakOn) only the overlay is drawn - transparent, no game depth; the last game-depth image held up to 300 ms while CameraCapture follows a remade scene depth; the renderer waits for CameraCapture to take each image again and only takes an untaken one back after 100 ms (pause / menus); vision frames over 50 ms logged; pause fix: an image CameraCapture has not taken is taken back and overwritten, so the view survives Escape / menus; pilot view shows every round (PPC, ballistic, missile) whatever the scope layers; the Enhanced Imaging vision mode rendered inside the game: HUDDisplay's 3D page, scene, outline "
    "shader, crosshair, lock and plates on the plugin's own D3D11 device and thread, at the game's size, into the shared "
    "vision image CameraCapture draws; hotkey [hud] ei_vision_key; VHUD v23)";

// A copy of HUDDisplay's psDepthEdgeCode (Main.cpp, InitShaders) - keep in step.
static const char kDepthEdgePs[] = R"(
    Texture2D<float> tex : register(t0);
    cbuffer XF : register(b0) { float4 uvXform; float4 params; };
    float S(int2 p, int2 wh) { p = clamp(p, int2(0, 0), wh - 1); return tex.Load(int3(p, 0)); }
    float U(int2 p, int2 wh) { p = clamp(p, int2(0, 0), wh - 1); return tex.Load(int3(p.x + wh.x, p.y, 0)); }
    bool Vis(int2 p, int2 wh) {
        float u = U(p, wh);
        if (u <= 1.0 || u > 5000000.0) return false;
        return u <= S(p, wh) * 1.01 + 30.0;
    }
    float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
        uint fw, fh;
        tex.GetDimensions(fw, fh);
        const int2 wh = int2(fw / 2, fh);
        float2 t = uv * uvXform.xy + uvXform.zw;
        int2 p = int2(t * float2(wh));

        float c = S(p, wh);
        float l = S(p - int2(1, 0), wh), r = S(p + int2(1, 0), wh);
        float u = S(p - int2(0, 1), wh), b = S(p + int2(0, 1), wh);
        float nearD = max(min(min(c, l), min(min(r, u), b)), 50.0);
        float jump = max(max(abs(c - l), abs(c - r)), max(abs(c - u), abs(c - b))) / nearD;
        float crease = (abs(l + r - 2 * c) + abs(u + b - 2 * c)) / max(c, 50.0);
        float fold = smoothstep(0.015, 0.045, crease);

        if (params.w > 0.5) {
            bool v  = Vis(p, wh);
            bool vl = Vis(p - int2(1, 0), wh), vr = Vis(p + int2(1, 0), wh);
            bool vu = Vis(p - int2(0, 1), wh), vb = Vis(p + int2(0, 1), wh);
            bool edgeIn  = v && !(vl && vr && vu && vb);
            bool edgeOut = !v && (vl || vr || vu || vb);
            float a = (edgeIn || edgeOut) ? 1.0 : (v ? smoothstep(0.02, 0.07, jump) * 0.85 : 0.0);
            if (a > 0.01) return float4(0.62, 1.0, 0.72, a);
        }
        float sil = smoothstep(0.04, 0.12, jump);
        float fade = lerp(1.0, 0.35, saturate(nearD / 150000.0));
        float nearDim = lerp(0.22, 0.6, saturate((nearD - 3000.0) / 37000.0));
        return float4(0.33, 1.0, 0.54, saturate(sil + fold * 0.33) * fade * nearDim);
    }
)";

static const char kFullscreenVs[] = R"(
    struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
    VSOut main(uint id : SV_VertexID) {
        VSOut o;
        o.uv = float2((id << 1) & 2, id & 2);
        o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
        return o;
    }
)";

class EnhancedImaging final : public Plugin
{
public:
    void on_initialize() override
    {
        API::get()->log_info("[EnhancedImaging] %s", kBuild);
        std::thread([this] { Run(); }).detach();
    }

private:
    template <typename... A> void Log(const char *f, A... a)
    {
        const std::string s = std::string("[EnhancedImaging] ") + f;
        API::get()->log_info(s.c_str(), a...);
    }
    template <typename... A> void LogErr(const char *f, A... a)
    {
        const std::string s = std::string("[EnhancedImaging] ") + f;
        API::get()->log_error(s.c_str(), a...);
    }

    // ---- the shared block ----
    HANDLE              Mmf = nullptr;
    VanillaExportBlock *Block = nullptr;
    ULONGLONG           BlockTryMs = 0;

    bool OpenBlock()
    {
        if (Block)
        {
            if (Block->Version == VHUD_VERSION)
                return true;
            UnmapViewOfFile(Block);
            CloseHandle(Mmf);
            Block = nullptr;
            Mmf = nullptr;
        }
        const ULONGLONG now = GetTickCount64();
        if (now - BlockTryMs < 1000)
            return false;
        BlockTryMs = now;
        Mmf = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, VHUD_MMF_NAME);
        if (!Mmf)
            return false;
        Block = static_cast<VanillaExportBlock *>(MapViewOfFile(Mmf, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VanillaExportBlock)));
        if (!Block || Block->Version != VHUD_VERSION)
        {
            if (Block)
                UnmapViewOfFile(Block);
            CloseHandle(Mmf);
            Block = nullptr;
            Mmf = nullptr;
            return false;
        }
        Log("block open (v%u)", VHUD_VERSION);
        return true;
    }

    // ---- the hotkey ----
    int       HotVk = 0;
    bool      CrossOn = true; // enhancedimaging-9: [hud] ei_crosshair (user, 2026-10-17), read with the hotkey
    bool      HotDown = false;
    ULONGLONG HotCfgMs = 0;

    void Hotkey()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - HotCfgMs >= 1000)
        {
            HotCfgMs = now;
            const int vk = gsp::Int(L"hud", L"ei_vision_key", 0);
            if (vk != HotVk)
            {
                HotVk = vk;
                Log("hotkey: %s (virtual key %d)", vk ? "set" : "none", vk);
            }
            const bool cross = gsp::Int(L"hud", L"ei_crosshair", 1) != 0;
            if (cross != CrossOn)
            {
                CrossOn = cross;
                Log("centre crosshair: %s", cross ? "on" : "off");
            }
            if (AssetDirOverride().empty())
                AssetDirOverride() = gsp::AppsDir();
        }
        if (!HotVk)
            return;
        // Only while the game has the focus - typing elsewhere must not switch it.
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(HotVk) & 0x8000) != 0;
        if (down && !HotDown)
        {
            Block->EiVisionOn = Block->EiVisionOn ? 0u : 1u;
            Log("vision mode %s (hotkey)", Block->EiVisionOn ? "ON" : "off");
        }
        HotDown = down;
    }

    // ---- our device ----
    ComPtr<ID3D11Device>             Dev;
    ComPtr<ID3D11DeviceContext>      Ctx;
    ComPtr<ID2D1Factory1>            D2dFactory;
    ComPtr<ID2D1Device>              D2dDevice;
    ComPtr<ID2D1DeviceContext>       D2d;
    ComPtr<IDWriteFactory>           Dw;
    ComPtr<ID2D1SolidColorBrush>     Brush;
    ComPtr<ID3D11VertexShader>       Vs;
    ComPtr<ID3D11PixelShader>        PsEdge;
    ComPtr<ID3D11Buffer>             Cb;
    ComPtr<ID3D11SamplerState>       Samp;
    ComPtr<ID3D11BlendState>         Blend;
    bool                             DeviceTried = false, DeviceOk = false;
    bool                             TargetFail = false;   // enhancedimaging-10: the vision target could not be made
    ULONGLONG                        TargetRetryMs = 0;    // ...tried again from then (every 2 s, not every frame)
    LidarView                        Lidar;
    Topo3DView                       Topo3d;

    bool InitDevice()
    {
        if (DeviceTried)
            return DeviceOk;
        DeviceTried = true;
        const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, lv, 2,
                                     D3D11_SDK_VERSION, Dev.GetAddressOf(), nullptr, Ctx.GetAddressOf())))
        {
            LogErr("no D3D11 device - the vision mode stays with HUDDisplay");
            return false;
        }
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                     reinterpret_cast<void **>(D2dFactory.GetAddressOf()))) ||
            FAILED(Dev.As(&dxgi)) || FAILED(D2dFactory->CreateDevice(dxgi.Get(), D2dDevice.GetAddressOf())) ||
            FAILED(D2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, D2d.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown **>(Dw.GetAddressOf()))) ||
            FAILED(D2d->CreateSolidColorBrush(D2D1::ColorF(0x55FF8A), Brush.GetAddressOf())))
        {
            LogErr("no Direct2D / DirectWrite - the vision mode stays with HUDDisplay");
            return false;
        }
        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(D3DCompile(kFullscreenVs, sizeof(kFullscreenVs) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0,
                              vsb.GetAddressOf(), err.ReleaseAndGetAddressOf())) ||
            FAILED(D3DCompile(kDepthEdgePs, sizeof(kDepthEdgePs) - 1, nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0,
                              psb.GetAddressOf(), err.ReleaseAndGetAddressOf())) ||
            FAILED(Dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, Vs.GetAddressOf())) ||
            FAILED(Dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, PsEdge.GetAddressOf())))
        {
            LogErr("shaders failed: %s", err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
            return false;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC bl{};
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(Dev->CreateBuffer(&bd, nullptr, Cb.GetAddressOf())) || FAILED(Dev->CreateSamplerState(&sd, Samp.GetAddressOf())) ||
            FAILED(Dev->CreateBlendState(&bl, Blend.GetAddressOf())))
            return false;
        if (!Lidar.InitResources(D2dFactory.Get(), Dw.Get()))
            LogErr("the scene (LidarView) could not start - no contacts or rounds");
        Lidar.SetRangeM(1000);
        if (!Topo3d.InitResources(Dev.Get(), Ctx.Get(), D2dFactory.Get(), Dw.Get()))
        {
            LogErr("the 3D page could not start");
            return false;
        }
        Topo3d.SetCellM(Topo3DView::kDefaultCellM);
        Topo3d.SetRangeM(1000);
        DeviceOk = true;
        Log("device ready - rendering the vision mode here, HUDDisplay not needed");
        return true;
    }

    // ---- the vision image ----
    ComPtr<ID3D11Texture2D>        Tex;
    ComPtr<ID3D11RenderTargetView> Rtv;
    ComPtr<ID2D1Bitmap1>           Bmp;
    HANDLE                         TexHandle = nullptr;
    UINT                           TexW = 0, TexH = 0;

    void ReleaseTarget()
    {
        if (Block && TexHandle && Block->EiVisHandle == (uint64_t)(uintptr_t)TexHandle)
            Block->EiVisHandle = 0;
        Topo3d.SetTargets(nullptr, nullptr, 0, 0);
        if (D2d)
            D2d->SetTarget(nullptr);
        Bmp.Reset();
        Rtv.Reset();
        Tex.Reset();
        TexHandle = nullptr;
        TexW = TexH = 0;
    }

    bool EnsureTarget(UINT w, UINT h)
    {
        if (Tex && TexW == w && TexH == h)
            return true;
        ReleaseTarget();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<IDXGIResource> r;
        ComPtr<IDXGISurface> surf;
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(Dev->CreateTexture2D(&d, nullptr, Tex.GetAddressOf())) ||
            FAILED(Dev->CreateRenderTargetView(Tex.Get(), nullptr, Rtv.GetAddressOf())) || FAILED(Tex.As(&r)) ||
            FAILED(r->GetSharedHandle(&TexHandle)) || !TexHandle || FAILED(Tex.As(&surf)) ||
            FAILED(D2d->CreateBitmapFromDxgiSurface(surf.Get(), &bp, Bmp.GetAddressOf())))
        {
            LogErr("could not make the shared vision image (%ux%u)", w, h);
            ReleaseTarget();
            return false;
        }
        TexW = w;
        TexH = h;
        Log("vision image %ux%u", w, h);
        return true;
    }

    // ---- the game-depth image (VS_EiDepth), as HUDDisplay's VanillaSrv does it ----
    ComPtr<ID3D11Texture2D>          DepShared, DepCached;
    ComPtr<ID3D11ShaderResourceView> DepSrv;
    uint64_t                         DepHandle = 0;
    VanillaExportBlock::Camera       DepCam{};
    VanillaExportBlock::Lock         DepLock{};
    uint32_t                         DepBeamCount = 0;
    VanillaExportBlock::Beam         DepBeams[VanillaExportBlock::kMaxBeams] = {};
    VanillaExportBlock::Lock         PakLock{};      // enhancedimaging-6: with the .pak, published with the camera
    uint32_t                         PakBeamCount = 0;
    VanillaExportBlock::Beam         PakBeams[VanillaExportBlock::kMaxBeams] = {};
    UINT                             DepW = 0, DepH = 0;
    bool                             PakLogged = false;
    ULONGLONG                        DepOkMs = 0; // enhancedimaging-4: the last image taken

    ID3D11ShaderResourceView *DepthSrv()
    {
        const auto &info = Block->Slots[VS_EiDepth];
        // enhancedimaging-4: CameraCapture publishes nothing for the few frames it takes to follow the game
        // to a remade scene depth - the last image (with its own pose) is kept for up to 300 ms rather
        // than the page dropping to the grid alone.
        if (!info.Active || !info.SharedHandle)
        {
            const ULONGLONG t = GetTickCount64();
            return (DepSrv && DepOkMs && t - DepOkMs < 300) ? DepSrv.Get() : nullptr;
        }
        if (info.SharedHandle != DepHandle || !DepShared)
        {
            DepShared.Reset();
            DepCached.Reset();
            DepSrv.Reset();
            DepHandle = 0;
            if (FAILED(Dev->OpenSharedResource(reinterpret_cast<HANDLE>(info.SharedHandle), __uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void **>(DepShared.GetAddressOf()))))
                return nullptr;
            D3D11_TEXTURE2D_DESC d{};
            DepShared->GetDesc(&d);
            d.MiscFlags = 0;
            if (FAILED(Dev->CreateTexture2D(&d, nullptr, DepCached.GetAddressOf())) ||
                FAILED(Dev->CreateShaderResourceView(DepCached.Get(), nullptr, DepSrv.GetAddressOf())))
            {
                DepShared.Reset();
                return nullptr;
            }
            DepHandle = info.SharedHandle;
            DepW = d.Width;
            DepH = d.Height;
        }
        ComPtr<IDXGIKeyedMutex> km;
        if (SUCCEEDED(DepShared.As(&km)) && km->AcquireSync(1, 8) == S_OK) // enhancedimaging-10: WAIT_TIMEOUT is a SUCCESS code
        {
            Ctx->CopyResource(DepCached.Get(), DepShared.Get());
            DepCam = Block->EiCam;
            DepLock = Block->EiLock;
            DepBeamCount = Block->EiBeamCount <= (uint32_t)VanillaExportBlock::kMaxBeams ? Block->EiBeamCount : 0;
            std::memcpy(DepBeams, (const void *)Block->EiBeams, sizeof(DepBeams[0]) * DepBeamCount);
            km->ReleaseSync(0);
            DepOkMs = GetTickCount64();
        }
        return DepSrv.Get();
    }

    // ---- one frame (HUDDisplay: PrepareTargeting + the targeting branch of Render) ----
    void DrawSrv(ID3D11ShaderResourceView *srv, float w, float h, const float xf[4], const float params[4])
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(Ctx->Map(Cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            float cb[8] = {xf[0], xf[1], xf[2], xf[3], params[0], params[1], params[2], params[3]};
            std::memcpy(m.pData, cb, sizeof(cb));
            Ctx->Unmap(Cb.Get(), 0);
        }
        ID3D11RenderTargetView *rt = Rtv.Get();
        Ctx->OMSetRenderTargets(1, &rt, nullptr);
        const float bf[4] = {0, 0, 0, 0};
        Ctx->OMSetBlendState(Blend.Get(), bf, 0xFFFFFFFF);
        D3D11_VIEWPORT vp = {0.0f, 0.0f, w, h, 0.0f, 1.0f};
        Ctx->RSSetViewports(1, &vp);
        Ctx->IASetInputLayout(nullptr);
        Ctx->VSSetShader(Vs.Get(), nullptr, 0);
        Ctx->PSSetShader(PsEdge.Get(), nullptr, 0);
        ID3D11Buffer *cbp = Cb.Get();
        Ctx->PSSetConstantBuffers(0, 1, &cbp);
        Ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState *sp = Samp.Get();
        Ctx->PSSetSamplers(0, 1, &sp);
        Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Ctx->Draw(3, 0);
        ID3D11ShaderResourceView *none = nullptr;
        Ctx->PSSetShaderResources(0, 1, &none);
        Ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
    }

    void Crosshair(float cx, float cy)
    {
        const float gap = 6.0f, arm = 18.0f;
        D2d->BeginDraw();
        for (int pass = 0; pass < 2; ++pass)
        {
            Brush->SetColor(pass ? D2D1::ColorF(0x55FF8A, 0.95f) : D2D1::ColorF(0x000000, 0.6f));
            const float wdt = pass ? 1.5f : 3.5f;
            D2d->DrawLine(D2D1::Point2F(cx - gap - arm, cy), D2D1::Point2F(cx - gap, cy), Brush.Get(), wdt);
            D2d->DrawLine(D2D1::Point2F(cx + gap, cy), D2D1::Point2F(cx + gap + arm, cy), Brush.Get(), wdt);
            D2d->DrawLine(D2D1::Point2F(cx, cy - gap - arm), D2D1::Point2F(cx, cy - gap), Brush.Get(), wdt);
            D2d->DrawLine(D2D1::Point2F(cx, cy + gap), D2D1::Point2F(cx, cy + gap + arm), Brush.Get(), wdt);
        }
        D2d->EndDraw();
    }

    void MissileLock(bool &acquiring, float &progress, bool &locked)
    {
        acquiring = locked = false;
        progress = 0.0f;
        VanillaExportBlock::MissileLockInfo m{};
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t s1 = Block->MissileLock.Seq;
            std::memcpy(&m, (const void *)&Block->MissileLock, sizeof(m));
            if (s1 == Block->MissileLock.Seq && !(s1 & 1u))
            {
                locked = m.OnTarget && m.State == kVhudLockLocked;
                acquiring = m.OnTarget && m.State != 0 && m.State != kVhudLockLocked;
                progress = m.Percent;
                return;
            }
        }
    }

    bool        FirstFrameLogged = false;
    bool        NoDepthLogged = false;

    void RenderFrame()
    {
        UINT W = Block->GameW, H = Block->GameH;
        if (W < 64 || H < 64 || W > 8192 || H > 8192)
        {
            W = 1920;
            H = 1080;
        }
        // enhancedimaging-10 (bug sweep): a target that cannot be made is retried every 2 s (it logged an error every
        // frame); while it fails no heartbeat is written, so HUDDisplay renders the vision instead.
        if (TargetFail && GetTickCount64() < TargetRetryMs)
            return;
        if (!EnsureTarget(W, H))
        {
            TargetFail = true;
            TargetRetryMs = GetTickCount64() + 2000;
            return;
        }
        TargetFail = false;
        ComPtr<IDXGIKeyedMutex> km;
        // enhancedimaging-2: if CameraCapture has not taken the last image (the game paused, a menu up -
        // it only takes them while drawing), take our own unread image back and overwrite it. Waiting
        // on key 0 there deadlocked: no new image -> never "fresh" -> never taken -> the view stayed off.
        // enhancedimaging-3: ...but only once it has gone untaken for 100 ms (a pause, a menu). In play the
        // renderer waits for CameraCapture again, as in -1, instead of redrawing frames nobody sees.
        if (FAILED(Tex.As(&km)))
            return;
        if (km->AcquireSync(0, 0) != S_OK)
        {
            const ULONGLONG handed = Block->EiVisFrameMs;
            if ((handed && GetTickCount64() - handed < 100) || km->AcquireSync(1, 0) != S_OK)
                return;
        }
        D2d->SetTarget(Bmp.Get());
        // enhancedimaging-5: with the .pak running (CameraCapture sets EiPakOn) the engine draws the scene
        // edges, units and wave; this image is only the overlay - transparent, no game depth needed.
        const bool pak = Block->EiPakOn != 0;
        const float black[4] = {0.0f, 0.0f, 0.0f, pak ? 0.0f : 1.0f};
        Ctx->ClearRenderTargetView(Rtv.Get(), black);
        Topo3d.SetTransparent(pak);
        if (pak != PakLogged)
        {
            PakLogged = pak;
            Log(pak ? "the .pak draws the scene - overlay only (transparent)" : "the .pak is not running - full vision image");
        }

        Lidar.Poll();
        Topo3d.Poll();
        Lidar.NotifyRendered();
        LidarScene scene;
        Lidar.BuildScene(scene, true); // pilot view: every round (PPCs too), whatever the scope layers
        Topo3d.SetTargets(Rtv.Get(), D2d.Get(), W, H);
        Topo3d.SetContentInset(0.0f);
        Topo3d.SetTacOps(nullptr);

        // PrepareTargeting, Enhanced Imaging at 1x: the game's own depth (not with the .pak - it has its own).
        if (!pak)
            Block->EiWantMs = GetTickCount64();
        VanillaExportBlock::Camera cam{};
        bool got = false;
        for (int i = 0; i < 4 && !got; ++i)
        {
            const uint32_t s1 = Block->Cam.Seq;
            std::memcpy(&cam, (const void *)&Block->Cam, sizeof(cam));
            if (pak) // enhancedimaging-6: the lock bounds and beams, as one set with the pose (CameraCapture's seqlock)
            {
                PakLock = Block->EiLock;
                const uint32_t n = Block->EiBeamCount;
                PakBeamCount = n <= (uint32_t)VanillaExportBlock::kMaxBeams ? n : 0;
                std::memcpy(PakBeams, (const void *)Block->EiBeams, sizeof(PakBeams[0]) * PakBeamCount);
            }
            got = s1 == Block->Cam.Seq && !(s1 & 1u) && cam.Valid;
        }
        if (got)
        {
            ID3D11ShaderResourceView *depth = pak ? nullptr : DepthSrv();
            const UINT dw = DepW / 2, dh = DepH;
            const float as = (dw && dh) ? (float)dw / (float)dh : 16.0f / 9.0f, ar = (float)W / (float)H;
            float xf[4] = {1.0f, 1.0f, 0.0f, 0.0f};
            if (ar < as)
                xf[0] = ar / as;
            else
                xf[1] = as / ar;
            xf[2] = 0.5f - xf[0] * 0.5f;
            xf[3] = 0.5f - xf[1] * 0.5f;
            if (depth && DepCam.Valid)
            {
                const float hf = DepCam.FovDeg * 0.01745329252f;
                Topo3d.SetPilotCamera(true, DepCam.Loc, DepCam.Rot, 2.0f * std::atan(std::tan(hf * 0.5f) * xf[0]) * 57.29577951f);
                Topo3d.SetPilotLiveLock(DepLock.Valid != 0, DepLock.Origin, DepLock.Extent);
                Topo3d.SetPilotBeams((int)DepBeamCount, &DepBeams[0].From[0]);
            }
            else
            {
                if (!NoDepthLogged && !pak)
                {
                    NoDepthLogged = true;
                    Log("no game depth yet - the page draws without outlines until CameraCapture publishes it");
                }
                const float hf = cam.FovDeg * 0.01745329252f;
                Topo3d.SetPilotCamera(true, cam.Loc, cam.Rot, 2.0f * std::atan(std::tan(hf * 0.5f) * xf[0]) * 57.29577951f);
                if (pak) // enhancedimaging-6: live bounds and beams (were off: the bracket used the LIDAR scan position)
                {
                    Topo3d.SetPilotLiveLock(PakLock.Valid != 0, PakLock.Origin, PakLock.Extent);
                    Topo3d.SetPilotBeams((int)PakBeamCount, &PakBeams[0].From[0]);
                }
                else
                {
                    Topo3d.SetPilotLiveLock(false, nullptr, nullptr);
                    Topo3d.SetPilotBeams(0, nullptr);
                }
            }
            Topo3d.Render(scene);
            if (depth && DepW && DepH)
            {
                const float params[4] = {0.0f, 1.0f / (float)DepW, 1.0f / (float)DepH, DepCam.UnitsValid};
                DrawSrv(depth, (float)W, (float)H, xf, params);
            }
            if (CrossOn) // enhancedimaging-9: GlobalSettings > HUD > VISION > Centre crosshair
                Crosshair((float)W * 0.5f, (float)H * 0.5f);
            Topo3d.RenderPilotBeams();
            bool acq, lk;
            float prog;
            MissileLock(acq, prog, lk);
            Topo3d.SetPilotMissileLock(acq, prog, lk);
            Topo3d.RenderPilotLock(scene);
        }
        Topo3d.SetPilotCamera(false, nullptr, nullptr, 0.0f);
        D2d->SetTarget(nullptr);
        Ctx->Flush();
        km->ReleaseSync(1);
        if (Block->EiVisHandle != (uint64_t)(uintptr_t)TexHandle)
        {
            Block->EiVisW = W;
            Block->EiVisH = H;
            MemoryBarrier();
            Block->EiVisHandle = (uint64_t)(uintptr_t)TexHandle;
        }
        Block->EiVisFrameMs = GetTickCount64();
        if (!FirstFrameLogged)
        {
            FirstFrameLogged = true;
            Log("first vision frame handed to CameraCapture (%ux%u)", W, H);
        }
    }

    // ---- the thread ----
    void Run()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // the paperdoll PNGs (WIC) are read on this thread
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        for (;;)
        {
            if (!OpenBlock())
            {
                Sleep(500);
                continue;
            }
            // enhancedimaging-10 (bug sweep): "this plugin renders the vision" only while it can - before the device is
            // tried, or with a working device and target. A failed device used to stop HUDDisplay stepping in.
            if (!DeviceTried || (DeviceOk && !TargetFail))
                Block->EiVisPluginSec = (uint32_t)(GetTickCount64() / 1000ull);
            Hotkey();
            if (!Block->EiVisWant)
            {
                if (Tex)
                    ReleaseTarget();
                Sleep(20);
                continue;
            }
            if (!InitDevice())
            {
                Sleep(1000);
                continue;
            }
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            RenderFrame();
            QueryPerformanceCounter(&t1);
            {
                const double rms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
                const double artMs = Topo3d.TakeArtLoadMs(); // ART BUDGET (enhancedimaging-7): paperdoll PNGs this frame
                if (rms >= 50.0)
                    Log("slow vision frame: %.0f ms (paperdoll loading %.0f ms)", rms, artMs);
            }
            // About 120 frames a second at most: the game rarely runs faster, and a frame the
            // compositor never shows is wasted GPU.
            const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
            if (ms < 8.0)
                Sleep((DWORD)(8.0 - ms));
        }
    }
};

// enhancedimaging-11 (EXIT CRASH, user 2026-10-17/18): the plugin object is created once and NEVER destroyed. As a
// unique_ptr it was destroyed by the CRT at DLL_PROCESS_DETACH (dllmain_crt_process_detach -> atexit table ->
// ~EnhancedImaging), which released the Direct2D / D3D11 objects after the game and NVIDIA's driver layers (NvCamera,
// nvwgf2umx) had already shut down: an access violation in d3d11.dll on every Exit Game (crash dumps 2026-10-17/18,
// identified by disassembling the frames). Process exit frees everything anyway; the render thread is ended by the
// process before any detach runs. (Plugin's constructor registers the instance with UEVR - nothing else holds it.)
// NOLINTNEXTLINE(misc-use-internal-linkage)
EnhancedImaging *const g_plugin = new EnhancedImaging();
