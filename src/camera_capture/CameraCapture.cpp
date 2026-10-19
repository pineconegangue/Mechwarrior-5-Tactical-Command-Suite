// CameraCapture.cpp - HUDDisplay's cameras and the TacOps overlay.
//
//   - the Targeting page's depth camera (a ring of scene captures following the game camera,
//     with a units-only pass and the lasers of the frame);
//   - the CAP page's zoom camera;
//   - the TacOps overlay: HUDDisplay's window image drawn over the flat game view, game input
//     kept (clicks / wheel / mouse look over it swallowed) while it is up.
//
// Split out of VanillaHud (vanillahud-102) unchanged, apart from what drives it: the player
// cockpit's own ReceiveTick (as LidarExport), not the vanilla HUD's - so it runs under MWVR too.
// It shares the MW5_VanillaHUDExport block with VanillaHud and the other split plugin, writing only
// its own part of it; HUDDisplay and VoiceCommand read it as before.
//
// Reflected calls, threads and the leak rules are as described at the top of VanillaHud.cpp.

#include "SlowTimer.h"
#include <PluginExtension.h>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <MinHook.h>
#include <GsPointer.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <share.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <mw5/UTextureRenderTarget2D.h>
#include <mw5/FTextureResource.h>
#include <mw5/FRHITexture2D.h>

#include <VanillaHudShared.h>
#include <LanceStatusShared.h>

using Microsoft::WRL::ComPtr;

class CameraCapture final : public PluginExtension
{
    static constexpr ULONGLONG kGameTickMs   = 100;  // decisions at 10 Hz

    static constexpr ULONGLONG kPresentStaleMs = 500; // HUD not ticking this long: publish nothing

    // ---- shared block --------------------------------------------------------------------
    HANDLE              MmfHandle   = nullptr;

    HANDLE              WriterMutex = nullptr;

    VanillaExportBlock *Block       = nullptr;

    ULONGLONG           LastSetupMs = 0;

    // ---- game-thread state ---------------------------------------------------------------
    API::UObject *Hud        = nullptr; // this plugin's world key: the player controller its captures belong to
    API::UObject *HudLevel   = nullptr; // with the controller's level (cameracapture-2)
    API::UObject *HudPawn    = nullptr; // and the player's mech
    // cameracapture-3: and the WORLD CLOCK - addresses can be reused by the next level, so pointers
    // alone are not proof. GetTimeSeconds restarts at 0 in every world and never runs backwards in
    // one (a pause stops it): seen going backwards, it is a new mission whatever the pointers say.
    float         HudWorldT  = -1.0f;

    std::atomic<ULONGLONG> HudSeenMs{0}; // read by the render thread: no HUD tick, no publishing
    // cameracapture-22: "the game has stopped" by FRAMES, not only by time. After a game hitch longer
    // than 500 ms (artillery, -21 log 01:47:54) the first frame shown came before the next tick and
    // read as a pause - one raw game frame. A pause shows frame after frame with no tick; a hitch shows
    // one or two. So: live while ticked within 500 ms OR at most 3 frames shown since the last tick.
    std::atomic<uint32_t> HudTicks{0};
    uint32_t              LiveTickSeen = 0, PresentsSinceTick = 0;
    bool GameLive(ULONGLONG now) const
    {
        const ULONGLONG seen = HudSeenMs.load(std::memory_order_relaxed);
        return seen && (Recent(seen, now, kPresentStaleMs) || PresentsSinceTick <= 3);
    }

    ULONGLONG     LastGameMs = 0;

    API::UObject       *LayoutLib         = nullptr; // WidgetLayoutLibrary CDO

    // Textures handed to the render thread. The GAME thread - which knows the render targets are
    // alive, because it just checked their components - resolves each to its D3D texture and holds
    // its own reference to it. The render thread never touches a UObject, so a level unload freeing
    // a render target cannot pull a texture out from under a copy (the vanillahud-5 load-screen crash).
    std::mutex              SrcLock;

    ComPtr<ID3D11Texture2D> Src[VS_COUNT];

    void                   *SrcRaw[VS_COUNT] = {};

    // ---- render-thread state -------------------------------------------------------------
    ComPtr<ID3D11Texture2D> Shared[VS_COUNT];

    HANDLE                  SharedHandle[VS_COUNT] = {};

    uint32_t                SharedW[VS_COUNT] = {}, SharedH[VS_COUNT] = {};

    DXGI_FORMAT             SrcFmt[VS_COUNT] = {};

    bool                    FormatWarned[VS_COUNT] = {};

    bool                    RendererWarned = false;

    API::UObject                         *Capture       = nullptr; // Caps[0]; non-null while the ring exists

    // A RING of captures, one rendered per frame in turn (manual CaptureScene, not every-frame), so
    // the image handed on is always one the render thread has certainly finished - two frames old -
    // and the camera pose it was rendered from is known exactly and travels with it (DepthCam).
    // Lasers of one frame (see ReadBeams).
    struct BeamSet
    {
        uint32_t                  Count = 0;
        VanillaExportBlock::Beam  B[VanillaExportBlock::kMaxBeams];
    };

    static constexpr int         kCaps = 4;

    API::UObject                *Caps[kCaps]    = {};

    API::UObject                *CapRt[kCaps]   = {};

    VanillaExportBlock::Camera   CapPose[kCaps] = {};

    uint64_t                     CapCount       = 0;

    VanillaExportBlock::Camera   SrcDepthPose   = {}; // guarded by SrcLock, goes with Src[VS_Depth]

    VanillaExportBlock::Lock     CapLock[kCaps] = {};

    // The UNITS ring: the same captures again, but showing only units (mechs, tanks, turrets,
    // VTOLs), rendered from the same pose in the same frame. Where its depth matches the scene's,
    // a unit is visible - which is what HUDDisplay highlights. Optional: without it the outlines
    // are drawn as before, unhighlighted.
    API::UObject                *UCaps[kCaps]   = {};

    API::UObject                *URt[kCaps]     = {};

    ComPtr<ID3D11Texture2D>      SrcUnits;           // guarded by SrcLock, goes with Src[VS_Depth]

    void                        *SrcUnitsRaw    = nullptr;

    ULONGLONG                    NextUnitsMs    = 0;

    size_t                       LastUnitCount  = (size_t)-1;

    API::UClass                 *PawnClass      = nullptr;

    API::UObject                *GameplayStatics = nullptr;

    API::UFunction              *GaacFn         = nullptr;

    std::vector<uint8_t>         GaacBuf;            // its out-array is never zeroed (leak rule)

    int32_t                      GaacWorld = -1, GaacClass = -1, GaacOut = -1;

    int32_t                      SoChildOff = -2;    // ShowOnlyActorComponents' bIncludeFromChildActors

    VanillaExportBlock::Lock     SrcDepthLock   = {}; // guarded by SrcLock, goes with Src[VS_Depth]

    API::UObject                         *CaptureActor  = nullptr;

    API::UObject                         *CaptureHud    = nullptr; // the HUD instance it was made under

    API::UObject                         *DepthRtObj    = nullptr; // the capture's render target

    ULONGLONG                             CaptureFailMs = 0;

    API::UClass                          *CaptureClass  = nullptr;

    API::UObject                         *RenderLib     = nullptr; // KismetRenderingLibrary CDO

    // ---- reflected-call plumbing ---------------------------------------------------------
    struct FnCache
    {
        const void     *Cls;
        std::wstring    Name;
        API::UFunction *Fn;
    };

    std::vector<FnCache>      Fns;

    std::vector<std::wstring> MissingLogged;

    alignas(16) uint8_t CallBuf[2048];

    std::vector<uint8_t> CbcBuf;

    API::UFunction      *CbcFn = nullptr;

    int32_t              CbcClassOff = -1, CbcRetOff = -1;

    static constexpr const wchar_t *kWriterMutexName = L"Local\\MW5_CameraCapture_Writer";

  public:
    inline static CameraCapture *Instance;

    CameraCapture()
    {
        Instance                  = this;
        PluginExtension::Instance = this;
        Name       = "CameraCapture";
        Version    = "0.1.0";
        VersionInt = 1;
    }

    ~CameraCapture() override
    {
        // Nothing of the game is restored here: this runs at process exit (see VanillaHud).
        ReleaseTextures();
        CloseBlock(false);
    }

    // cameracapture-19: a stamp written by another thread or process a moment AFTER this thread read
    // the clock is "in the future" - that is fresh, not stale. `now >= stamp` failed on it and the
    // vision mode let a raw game frame through every 10-20 s ("game shown ... (game not ticking)").
    static bool Recent(ULONGLONG stamp, ULONGLONG now, ULONGLONG ms) { return stamp && (stamp > now || now - stamp < ms); }

    // cameracapture-19: the units capture aimed at DRAW time. The cockpit tick runs before the game's
    // camera update for the frame, so the pose it read was the previous frame's: the units outline
    // trailed the world by a frame while turning (the lancemate "jitter"). The viewport client draw
    // comes after the camera update and before the frame's views are worked out, so the pose read
    // here is the one the frame renders with, and CaptureScene still lands before the frame's scene.
    std::atomic<ULONGLONG> EiLateMs{0};
    std::atomic<ULONGLONG> EiFollowMs{0}; // cameracapture-27: the units capture aimed (cockpit tick or draw time)
    bool                   EiLateLogged = false;
    // cameracapture-22: the vision mode belongs to the mech it was switched on in. When the local pawn
    // changes (death / eject, the mission ending, the hangar) it is switched off; with [hud]
    // ei_vision_resume it comes back on once you are piloting again. A pause keeps the same pawn.
    API::UObject *EiOnPawn = nullptr;
    bool          EiResume = false;
    ULONGLONG     EiPawnMs = 0;
    void EiPawnWatch(ULONGLONG now)
    {
        if (now - EiPawnMs < 250)
            return;
        EiPawnMs = now;
        const bool inMech = Recent(DrvSeenMs.load(), now, 300); // the cockpit is ticking us
        // bug sweep 15 (2026-10-18): while the cockpit ticks, the pawn the game thread saw (no game object read here, on
        // the render thread); only when it has stopped (a pause, leaving the mech) is the game asked directly.
        auto *pawn = inMech ? DrvPawn.load() : API::get()->get_local_pawn(0);
        if (Block->EiVisionOn)
        {
            EiResume = false;
            if (!EiOnPawn)
            {
                if (inMech && pawn)
                    EiOnPawn = pawn;
            }
            else if (pawn != EiOnPawn)
            {
                Block->EiVisionOn = 0;
                EiOnPawn = nullptr;
                EiResume = true;
                LogInfo("enhanced imaging vision mode switched off - you left the mech it was on in");
            }
        }
        else
        {
            EiOnPawn = nullptr;
            if (EiResume && inMech && pawn)
            {
                EiResume = false;
                if (gsp::Int(L"hud", L"ei_vision_resume", 0) != 0)
                {
                    Block->EiVisionOn = 1;
                    LogInfo("enhanced imaging vision mode back on - piloting again ([hud] ei_vision_resume)");
                }
            }
        }
    }

    // =====================================================================================
    // ENHANCED IMAGING .PAK (cameracapture-23) - the vision mode as an in-engine post-process
    // =====================================================================================
    // The mod's materials (built by pak/build_ei_materials.py, cooked into the EnhancedImaging .pak) read
    // the scene depth the engine hands every post-process: no depth buffer hunted, no viewport guessed, no
    // swap followed, per eye in VR. Two materials - M_EI_Vision_BT (Before Translucency: explosions and
    // MWVR's in-world HUD stay on top) and M_EI_Vision_AT (After Tonemapping: everything under the vision) -
    // each made a dynamic instance, both added to an unbound PostProcessComponent on the controller; the
    // chosen one gets weight 1 while the vision is up. Every frame (game thread, viewport draw) the wave
    // and the units capture of THIS frame go in as parameters. Not loadable / wrong version: the current
    // method runs as before. Tried once per world (a failed blocking load is not repeated every frame).
    static constexpr int kEiPakVersion = 1;
    static constexpr const wchar_t *kPakMat[2] = {L"/EnhancedImaging/EI/M_EI_Vision_BT.M_EI_Vision_BT",
                                                  L"/EnhancedImaging/EI/M_EI_Vision_AT.M_EI_Vision_AT"};
    enum
    {
        PN_Opacity, PN_WaveOn, PN_Front, PN_Band, PN_Fade, PN_Ripple, PN_UnitsOn, PN_EdgeGain, PN_UnitsTex, PN_Version, PN_COUNT
    };
    int                     PakState = 0; // 0 not tried in this world, 1 running, -1 not available here
    int                     PakTest = 0, PakTestSet = -1; // cameracapture-29: [hud] ei_pak_test 0 normal, 1 opacity 0, 2 red
    ULONGLONG               PakCfgMs = 0;
    API::UObject           *PakActor = nullptr, *PakHud = nullptr, *PakComp = nullptr, *PakMid[2] = {};
    float                   PakWeight[2] = {-1.0f, -1.0f};
    bool                    PakUse = true, PakBT = false, PakMissLogged = false;
    // cameracapture-25: WHERE the effect goes. A world-wide (unbound) PostProcessComponent also reaches every
    // scene capture with post-processing - our CAP zoom camera and MechWarriorVR's HUD / thermal camera views
    // would have shown the vision. So the effect is added to the player's ACTIVE CAMERA (view target, else the
    // pawn): that view only. The PostProcessComponent stays, at weight 0, only to hold the instances (a camera
    // can be replaced; the instances must not be garbage-collected meanwhile). [hud] ei_pak_attach = world uses
    // it for real (if the game's camera turns out not to apply camera post-process). No camera: current method.
    bool                    PakAttachCam = true, PakNoCamLogged = false;
    API::UObject           *PakTarget = nullptr; // what the blendables are added to (a camera, or PakComp)
    ULONGLONG               PakTargetMs = 0;
    float                   PakRipple = 0.006f, PakEdgeGain = 1.0f, PakUnitsScale = 0.6f;
    std::vector<API::FName> PakN;
    std::atomic<float>      EvFrontCm{0.0f};
    API::UObject           *PakUnitsRt = nullptr; // this frame's units capture (FollowEi)

    API::UObject *PakLoad(const wchar_t *path)
    {
        if (auto *o = API::get()->find_uobject<API::UObject>(std::wstring(L"Material ") + path))
        {
            LogInfo("enhanced imaging .pak: %ls already loaded", path); // cameracapture-26: every step logged
            return o;
        }
        static API::UObject *ksl = nullptr;
        if (!ksl)
            ksl = API::get()->find_uobject<API::UObject>(L"KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        if (!ksl)
        {
            LogError("enhanced imaging .pak: KismetSystemLibrary default object not found - cannot load %ls", path);
            return nullptr;
        }
        if (!FindFn(ksl, L"LoadAsset_Blocking"))
        {
            LogError("enhanced imaging .pak: function LoadAsset_Blocking not found - cannot load %ls", path);
            return nullptr;
        }
        // TSoftObjectPtr<UObject> (4.26): weak ptr 8, tag 4 (+4), FSoftObjectPath { FName 8, FString 16 } = 40
        struct
        {
            uint8_t    Weak[8];
            int32_t    Tag, Pad;
            API::FName Path;
            void      *SubData;
            int32_t    SubNum, SubMax;
        } soft{};
        soft.Path = API::FName(path);
        API::UObject *o = nullptr;
        if (!CallRet(ksl, L"LoadAsset_Blocking", {{L"Asset", &soft, sizeof(soft)}}, &o, sizeof(o)))
        {
            LogError("enhanced imaging .pak: LoadAsset_Blocking call failed for %ls", path);
            return nullptr;
        }
        if (o)
            LogInfo("enhanced imaging .pak: loaded %ls (%ls)", path, ClassName(o).c_str());
        else
            LogWarn("enhanced imaging .pak: LoadAsset_Blocking returned nothing for %ls (mod not mounted, or another path)", path);
        return o;
    }

    int PakScalarFails = 0; // cameracapture-28
    void PakSetScalar(API::UObject *mid, int n, float v)
    {
        if (!Call(mid, L"SetScalarParameterValue", {{L"ParameterName", &PakN[n], sizeof(API::FName)}, {L"Value", &v, 4}}) &&
            PakScalarFails++ < 10)
            LogWarn("enhanced imaging .pak: SetScalarParameterValue failed (parameter %d)", n);
    }

    void PakSetWeight(int k, float w)
    {
        if (!PakTarget || !PakMid[k] || PakWeight[k] == w)
            return;
        struct
        {
            API::UObject *Obj;
            void         *Iface;
        } bl{PakMid[k], PakMid[k]}; // TScriptInterface<IBlendableInterface>: AddBlendable uses GetObject()
        if (Call(PakTarget, L"AddOrUpdateBlendable", {{L"InBlendableObject", &bl, sizeof(bl)}, {L"InWeight", &w, 4}}))
        {
            PakWeight[k] = w;
            LogInfo("enhanced imaging .pak: %s weight %.0f", k == 0 ? "BT (over)" : "AT (under)", w); // cameracapture-28
        }
        else
            LogWarn("enhanced imaging .pak: AddOrUpdateBlendable failed (%s, weight %.0f)", k == 0 ? "BT" : "AT", w);
    }

    // The holder: both instances on the controller's PostProcessComponent at weight 0 (no effect anywhere).
    void PakHold()
    {
        for (int k = 0; k < 2; ++k)
        {
            float z = 0.0f;
            struct
            {
                API::UObject *Obj;
                void         *Iface;
            } bl{PakMid[k], PakMid[k]};
            Call(PakComp, L"AddOrUpdateBlendable", {{L"InBlendableObject", &bl, sizeof(bl)}, {L"InWeight", &z, 4}});
        }
    }

    // The player's active camera: the camera manager's view target's camera, else the pawn's.
    API::UObject *PakCamera(API::UObject *pc)
    {
        static API::UClass *camCls = nullptr;
        if (!camCls)
            camCls = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.CameraComponent");
        if (!camCls || PcmOff < 0)
            return nullptr;
        API::UObject *found = nullptr;
        auto *pcm = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(pc) + PcmOff);
        API::UObject *vt = nullptr;
        if (pcm)
            CallRet(pcm, L"GetViewTarget", {}, &vt, sizeof(vt));
        API::UObject *cands[2] = {vt, API::get()->get_local_pawn(0)};
        for (auto *a : cands)
            if (a && !found)
                CallRet(a, L"GetComponentByClass", {{L"ComponentClass", &camCls, sizeof(void *)}}, &found, sizeof(found));
        return found;
    }

    bool PakStart(API::UObject *pc)
    {
        if (PakN.empty())
        {
            static const wchar_t *n[PN_COUNT] = {L"Opacity", L"WaveOn",  L"Front",    L"Band",     L"Fade",
                                                 L"Ripple",  L"UnitsOn", L"EdgeGain", L"UnitsTex", L"EI_PakVersion"};
            for (int i = 0; i < PN_COUNT; ++i)
                PakN.emplace_back(n[i]);
        }
        LogInfo("enhanced imaging .pak: starting (in the mech, [hud] ei_pak on)");
        API::UObject *mat[2] = {PakLoad(kPakMat[0]), PakLoad(kPakMat[1])};
        if (!mat[0] || !mat[1])
        {
            if (!PakMissLogged)
            {
                PakMissLogged = true;
                LogInfo("enhanced imaging .pak: materials not found (%s) - the vision mode uses the current method",
                        mat[0] ? "AT missing" : mat[1] ? "BT missing" : "is the EnhancedImaging mod installed and enabled?");
            }
            return false;
        }
        auto *kml = API::get()->find_uobject<API::UObject>(L"KismetMaterialLibrary /Script/Engine.Default__KismetMaterialLibrary");
        auto *ppc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.PostProcessComponent");
        if (!kml || !ppc)
        {
            LogError("enhanced imaging .pak: %s not found - the current method is used",
                     !kml ? "KismetMaterialLibrary default object" : "class PostProcessComponent");
            return false;
        }
        for (int k = 0; k < 2; ++k)
        {
            PakMid[k] = nullptr;
            if (!CallRet(kml, L"CreateDynamicMaterialInstance", {{L"WorldContextObject", &pc, sizeof(void *)}, {L"Parent", &mat[k], sizeof(void *)}},
                         &PakMid[k], sizeof(void *)) ||
                !PakMid[k])
            {
                LogError("enhanced imaging .pak: CreateDynamicMaterialInstance failed");
                return false;
            }
        }
        float ver = 0.0f;
        CallRet(PakMid[0], L"K2_GetScalarParameterValue", {{L"ParameterName", &PakN[PN_Version], sizeof(API::FName)}}, &ver, 4);
        if ((int)(ver + 0.5f) != kEiPakVersion)
        {
            LogError("enhanced imaging .pak: version %d, this CameraCapture wants %d - rebuild the .pak (current method used)",
                     (int)(ver + 0.5f), kEiPakVersion);
            return false;
        }
        alignas(16) float xf[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0};
        bool no = false;
        PakComp = nullptr;
        if (!CallRet(pc, L"AddComponentByClass",
                     {{L"Class", &ppc, sizeof(void *)}, {L"bManualAttachment", &no, 1}, {L"RelativeTransform", xf, sizeof(xf)}, {L"bDeferredFinish", &no, 1}},
                     &PakComp, sizeof(void *)) ||
            !PakComp)
        {
            LogError("enhanced imaging .pak: could not add a PostProcessComponent");
            return false;
        }
        PakHold();
        PakWeight[0] = PakWeight[1] = -1.0f;
        PakTarget = nullptr; // found by PakTick
        PakTargetMs = 0;
        PakActor = pc;
        PakHud = Hud;
        LogInfo("enhanced imaging .pak: running (version %d) - scene edges, units and the wave drawn by the engine", kEiPakVersion);
        return true;
    }

    void PakTick(ULONGLONG now)
    {
        if (now - PakCfgMs >= 1000)
        {
            PakCfgMs = now;
            PakTest = gsp::Int(L"hud", L"ei_pak_test", 0);
            PakUse = gsp::Int(L"hud", L"ei_pak", 1) != 0;
            PakBT = gsp::Str(L"hud", L"ei_pak_layer", L"under") == L"over"; // default: everything under the vision
            PakAttachCam = gsp::Str(L"hud", L"ei_pak_attach", L"camera") != L"world";
            PakRipple = (float)gsp::Num(L"hud", L"ei_wave_ripple", 0.006);
            PakEdgeGain = (float)gsp::Num(L"hud", L"ei_edge_gain", 1.0);
            const double us = gsp::Num(L"hud", L"ei_units_scale", 0.6);
            PakUnitsScale = (float)((us >= 0.25 && us <= 2.0) ? us : 0.6); // cameracapture-31: up to 2x (supersampled outlines)
        }
        auto *pc = API::get()->get_player_controller(0);
        if (PakState != 0 && (!pc || pc != PakActor || Hud != PakHud) && Recent(DrvSeenMs.load(), now, 300))
        {
            // a new world: the old component and instances went with it
            PakState = 0;
            PakComp = nullptr;
            PakTarget = nullptr;
            PakMid[0] = PakMid[1] = nullptr;
            PakActor = PakHud = nullptr;
            PakUnitsRt = nullptr;
        }
        static bool tickLogged = false;
        if (!tickLogged)
        {
            tickLogged = true;
            LogInfo("enhanced imaging .pak: manager ticking (cockpit tick)");
        }
        // cameracapture-26: why it has not started, once per world after 5 s of waiting
        static ULONGLONG waitMs = 0;
        static bool waitLogged = false;
        if (PakState == 0 && Recent(DrvSeenMs.load(), now, 300)) // waiting counts only while piloting
        {
            if (!waitMs)
                waitMs = now;
            if (!waitLogged && now - waitMs > 5000)
            {
                waitLogged = true;
                LogWarn("enhanced imaging .pak: not started after 5 s in the mech - ei_pak %d, controller %s, world key %s, cockpit ticking %s",
                        PakUse ? 1 : 0, pc ? "yes" : "NO", pc && pc == Hud ? "matches" : "DIFFERS", Recent(DrvSeenMs.load(), now, 300) ? "yes" : "NO");
            }
        }
        else if (PakState != 0)
        {
            waitMs = 0;
            waitLogged = false;
        }
        if (PakState == 0 && PakUse && pc && pc == Hud && Recent(DrvSeenMs.load(), now, 300))
        {
            // cameracapture-35 (bug sweep): the world is remembered whether the start works or not - a failed start
            // (the mod not installed / not enabled / another version) left PakActor null, so the "new world" test
            // above fired on the next tick and it was tried again EVERY FRAME (object-table walks, asset loads, logs).
            PakActor = pc;
            PakHud = Hud;
            PakState = PakStart(pc) ? 1 : -1;
        }
        if (PakState == 1 && !PakUse) // switched off in GlobalSettings: back to the current method
        {
            PakSetWeight(0, 0.0f);
            PakSetWeight(1, 0.0f);
            PakState = -1;
            PakCfgMs = 0;
        }
        if (PakState != 1)
        {
            Block->EiPakOn = 0;
            if (PakState == -1 && PakUse && PakComp) // switched back on
                PakState = 1;
            return;
        }
        // the target: re-found every second (a new mech, a changed view target) - weights set afresh on it
        if (now - PakTargetMs >= 1000 || !PakTarget)
        {
            PakTargetMs = now;
            API::UObject *t = PakAttachCam ? PakCamera(pc) : PakComp;
            if (!t)
            {
                if (!PakNoCamLogged)
                {
                    PakNoCamLogged = true;
                    LogWarn("enhanced imaging .pak: no player camera found - the current method is used ([hud] ei_pak_attach = world forces the world-wide effect)");
                }
                Block->EiPakOn = 0;
                PakTarget = nullptr;
                return;
            }
            if (t != PakTarget)
            {
                // leaving the world-wide holder, or the camera on a settings switch: weight 0 there first
                // (a camera replaced by a new mech is gone - it is not touched)
                if (PakTarget == PakComp)
                    PakHold();
                else if (PakTarget && t == PakComp)
                {
                    PakSetWeight(0, 0.0f);
                    PakSetWeight(1, 0.0f);
                }
                PakTarget = t;
                PakWeight[0] = PakWeight[1] = -1.0f;
                LogInfo("enhanced imaging .pak: effect on %ls (%ls)", PakAttachCam ? L"the player camera" : L"the world",
                        ClassName(t).c_str());
            }
        }
        if (!PakTarget)
        {
            Block->EiPakOn = 0;
            return;
        }
        Block->EiPakOn = 1;
        const int phase = EvPhase;
        const int sel = PakBT ? 0 : 1;
        const bool up = phase >= 2;
        PakSetWeight(sel, up ? 1.0f : 0.0f);
        PakSetWeight(1 - sel, 0.0f);
        if (!up)
            return;
        API::UObject *mid = PakMid[sel];
        const float front = EvFrontCm.load();
        if (PakTest != PakTestSet) // cameracapture-29: the diagnostic switch, both instances
        {
            PakTestSet = PakTest;
            static const API::FName bgN(L"BgCol"), edN(L"EdgeCol");
            const bool red = PakTest == 2;
            const float bg[4] = {red ? 1.0f : 0.03f, red ? 0.0f : 0.055f, red ? 0.0f : 0.055f, 1.0f};
            const float ed[4] = {red ? 1.0f : 0.33f, red ? 0.0f : 1.0f, red ? 0.0f : 0.54f, 1.0f};
            for (int i = 0; i < 2; ++i)
            {
                const bool a = Call(PakMid[i], L"SetVectorParameterValue", {{L"ParameterName", &bgN, sizeof(API::FName)}, {L"Value", bg, sizeof(bg)}});
                const bool b = Call(PakMid[i], L"SetVectorParameterValue", {{L"ParameterName", &edN, sizeof(API::FName)}, {L"Value", ed, sizeof(ed)}});
                if (!a || !b)
                    LogWarn("enhanced imaging .pak: SetVectorParameterValue failed");
            }
            LogInfo("enhanced imaging .pak: test %d (%s)", PakTest,
                    PakTest == 1 ? "opacity 0 - the game passed through" : red ? "background and edges red" : "normal");
        }
        PakSetScalar(mid, PN_Opacity, PakTest == 1 ? 0.0f : 1.0f);
        PakSetScalar(mid, PN_WaveOn, (phase == 2 || phase == 4) ? 1.0f : 0.0f);
        PakSetScalar(mid, PN_Front, front);
        PakSetScalar(mid, PN_Band, (std::max)(front * 0.08f, 400.0f));
        PakSetScalar(mid, PN_Fade, EvFadeCm);
        PakSetScalar(mid, PN_Ripple, PakRipple);
        PakSetScalar(mid, PN_EdgeGain, PakEdgeGain);
        const bool units = PakUnitsRt && EiUCaps[0] && Recent(EiFollowMs.load(), now, 200);
        PakSetScalar(mid, PN_UnitsOn, units ? 1.0f : 0.0f);
        bool texOk = false;
        if (units)
            texOk = Call(mid, L"SetTextureParameterValue", {{L"ParameterName", &PakN[PN_UnitsTex], sizeof(API::FName)}, {L"Value", &PakUnitsRt, sizeof(void *)}});
        // cameracapture-28: once a second while up, what the material holds (read back from the instance)
        static ULONGLONG dumpMs = 0;
        if (now - dumpMs >= 1000)
        {
            dumpMs = now;
            float rb[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
            const int ids[4] = {PN_Opacity, PN_WaveOn, PN_Front, PN_UnitsOn};
            for (int i = 0; i < 4; ++i)
                CallRet(mid, L"K2_GetScalarParameterValue", {{L"ParameterName", &PakN[ids[i]], sizeof(API::FName)}}, &rb[i], 4);
            LogInfo("enhanced imaging .pak: %s phase %d weights BT %.0f AT %.0f | read back Opacity %.2f WaveOn %.0f Front %.0f cm UnitsOn %.0f"
                    " | units rt %s ring %s aimed %s tex %s | target %ls",
                    sel == 0 ? "BT" : "AT", phase, PakWeight[0], PakWeight[1], rb[0], rb[1], rb[2], rb[3], PakUnitsRt ? "yes" : "NO",
                    EiUCaps[0] ? "yes" : "NO", Recent(EiFollowMs.load(), now, 200) ? "yes" : "NO", units ? (texOk ? "set" : "FAILED") : "-",
                    ClassName(PakTarget).c_str());
        }
    }

    void on_pre_viewport_client_draw(UEVR_UGameViewportClientHandle, UEVR_FViewportHandle, UEVR_FCanvasHandle) override
    {
        if (!Block)
            return;
        const ULONGLONG now = GetTickCount64();
        EiLateAim(now); // cameracapture-27: never called in MW5 (kept in case UEVR starts calling it);
                        // the .pak manager moved to GameTick, the pawn watch to on_present
    }

    void EiLateAim(ULONGLONG now)
    {
        if (!EiUCaps[0] || PcmOff < 0)
            return;
        if (!Recent(DrvSeenMs.load(), now, 200) || !EiUnitsWanted(now)) // the game is ticking us, and it is wanted
            return;
        auto *pc = API::get()->get_player_controller(0);
        if (!pc || pc != EiActor || EiHud != Hud)
            return;
        auto *pcm = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(pc) + PcmOff);
        if (!pcm)
            return;
        float loc[3] = {}, rot[3] = {}, fov = 90.0f;
        if (!CallRet(pcm, L"GetCameraLocation", {}, loc, sizeof(loc)) || !CallRet(pcm, L"GetCameraRotation", {}, rot, sizeof(rot)))
            return;
        CallRet(pcm, L"GetFOVAngle", {}, &fov, sizeof(fov));
        FollowEi(pc, loc, rot, fov);
        EiLateMs.store(now);
        EiFollowMs.store(now);
        if (PakState == 1) // the overlay is drawn from the pose this frame renders with, like the post-process
        {
            auto &c = Block->Cam;
            c.Seq = c.Seq | 1u;
            std::atomic_thread_fence(std::memory_order_release);
            std::memcpy(c.Loc, loc, sizeof(loc));
            std::memcpy(c.Rot, rot, sizeof(rot));
            c.FovDeg = fov;
            c.Valid  = 1;
            std::atomic_thread_fence(std::memory_order_release);
            c.Seq = c.Seq + 1;
        }
        if (!EiLateLogged)
        {
            EiLateLogged = true;
            LogInfo("enhanced imaging: units capture aimed at draw time (viewport draw) - same pose as the frame");
        }
    }

    // The TacOps overlay's log lines through the game log only (the probe file is LanceControl's).
    template <typename... Args> void TacLog(const char *format, Args... args) { LogInfo(format, args...); }

    void on_initialize() override
    {
        LogInfo("%s %s initialized", Name, Version);
        LogInfo("BUILD=cameracapture-37 (QUIET SLOW LINES 2026-10-18: one-by-one slow: lines only with [debug] verbose_log, hitches over 50 ms always; 60 s summary kept) (cameracapture-36 BUG SWEEP 2 2026-10-18: the render thread looks for the cockpit only while its tick is not reaching us, and takes the pawn from the game thread while it is; the units-capture images handed over with a reference held) (cameracapture-35 BUG SWEEP 2026-10-17: a failed .pak start is tried once per world, not every frame; mouse input never left swallowed after a world change or a closed overlay) (cameracapture-34 RAY RANGE ALWAYS CM: a laser that hits nothing is drawn to its range in cm - no under-20000-is-metres rule, 2026-10-17) (cameracapture-33 (LOOP GUARD on the cockpit tick chain, 2026-10-17) (cameracapture-32 BEAMS: a laser that hits nothing is drawn to its range along the muzzle, machine guns as moving tracers, flamers as a flickering spray) (cameracapture-31 UNITS CAPTURE UP TO 2x: [hud] ei_units_scale 0.25-2.0, was 1.0 max) (cameracapture-30 FORMAT CALLS TRUNCATE: every swprintf_s / sprintf_s into a fixed buffer is _snwprintf_s / _snprintf_s with _TRUNCATE - a long game name no longer fast-fails the game) (cameracapture-29: .pak: lock bounds and beams published every units capture (inside the camera seqlock) for EnhancedImaging.dll - the bracket used the LIDAR scan position; [hud] ei_pak_test 0 / 1 opacity 0 / 2 red background and edges; .pak manager every cockpit tick, vision on or off - it was only reached while the units were wanted, so switching off left the effect frozen on screen; weights and failed calls logged, the material's parameters read back once a second while up; the .pak manager runs from the cockpit tick and the pawn watch (vision off on leaving the mech, ei_vision_resume) from present - UEVR's viewport-draw callback never fires in MW5; .pak start-up logged step by step: manager ticking, why not started after 5 s, each asset load, each lookup; .pak: the effect goes on the player's active camera, not world-wide (it reached scene captures: our CAP camera, MWVR's HUD / thermal views), [hud] ei_pak_attach = camera | world; layering [hud] ei_pak_layer = under (default, After Tonemapping) | over (Before Translucency); first activation: the scene depth is found ahead of the vision while piloting (pre-warm, tracking only, not while the .pak runs) and the wave waits (up to 2 s) for the depth and the units capture - it used to flip on at once with the outlines 1.5 s late; ENHANCED IMAGING .PAK: when the EnhancedImaging mod's materials load (version 1) the scene edges, units and wave are an in-engine post-process (no depth hunting), [hud] ei_pak / ei_pak_bt / ei_edge_gain / ei_wave_ripple / ei_units_scale; EnhancedImaging.dll then draws only its overlay (VHUD EiPakOn, was ZoomPad); otherwise the current method; the game counts as stopped only with >3 frames since its last tick or 500 ms - a long hitch no longer shows a raw game frame; viewport: the most used screen-shaped one again (1485 is the picture); scene depth swap checked every frame (3 missed clears); vision off when you leave the mech ([hud] ei_vision_resume brings it back at the next drop); units capture skips pawn classes named in [hud] ei_units_skip and logs every pawn class; scene depth swaps (MW5 r.SceneRenderTargetResizeMethod 3 remakes the scene buffers when a larger / smaller scene is rendered) followed within 250 ms: the other reversed-Z buffers are counted all along and the busiest that can hold the picture is taken at once, picture area kept - the vision page lost its scene for 1-2.3 s; units outline paired EXACTLY with the depth frame: the ring entry the render thread drew as a render target this frame (kEiLag only as fallback) - the -19 clip had it repeating / leading a frame; stray game frames: a stamp written just after the clock was read counted as stale - fixed everywhere (Recent()); units capture aimed at draw time from the viewport draw callback (the cockpit tick read the previous frame's camera - outline jitter); Enhanced Imaging view: the largest screen-shaped viewport; Enhanced Imaging view: only viewports with the screen's shape count (alt-tab left the whole grown buffer as the busiest one), else the screen's shape fitted in; vision wave fade: a band of fixed depth ([hud] ei_wave_band_m, default 50 m) trailing the front instead of a share of its distance; vision wave: a gradient fade trailing the front ([hud] ei_wave_fade, %% of its distance); performance: the depth hooks act for the game's own context only, the viewport is sampled 0.3 s every 3 s and never waited on; freeze logger ('freeze:' when 150 ms pass between frames); Enhanced Imaging: only the part of the scene depth the game draws into is used (its viewport) - the buffer grows after a menu and the view was squashed upwards, the units capture the wrong shape; vision mode: a late or skipped renderer image redraws the last one (up to 1.5 s) instead of showing the game for a frame, and the reasons the game shows through are logged every 10 s; vision mode WAVE: Enhanced Imaging sweeps out through the world by the game's depth when switched on (cockpit first, then ground, hills, horizon) and collapses back from the horizon when off, a glowing front with a ripple; EiVisWant keeps the renderer going through the closing wave; VHUD v23; ENHANCED IMAGING VISION MODE, VHUD v22: HUDDisplay's Enhanced Imaging drawn over the whole screen, opaque, while EiVisionOn - input untouched; the game's size published for it; ENHANCED IMAGING, VHUD v21: the game's own scene depth (found by its reversed-Z clear and bind count, MinHooked while wanted) turned into cm and published with a units-only capture of the same frame as VS_EiDepth - V-POD's clone without the second scene render at 1x; the post-process and depth probes switched off; depth probe 3: the scene depth picked as a reversed-Z buffer (cleared to 0) - -10 read the 2048x2048 shadow map; depth-buffer probe 2, read-only: -9 saw no clears through the vtable, so for 2 s ClearDepthStencilView, OMSetRenderTargets and OMSetRenderTargetsAndUnorderedAccessViews are MinHooked (then removed): every depth buffer cleared or bound, described, the scene-sized one read at five points - [db] lines; missile lock progress = AcquireTimer / TotalTimeToAcquireLock (found by the -6 dump); post-process probe, read-only: the game's vision-mode and outline materials and their parameters, vision components and functions, the camera's post-process blendables (logged on every change - toggle night / thermal vision to see them), custom depth on targets - [pp] lines; V-POD depth capture at half the viewport, was a quarter; missile lock: every float on the LockOnComponent logged at each state change, to find the lock progress; missile lock: the player's LockOnComponent state / percent / target bounds and the zoom camera's pose published every frame, VHUD v20; slow-step timer: game tick / present over 2 ms logged as 'slow:'; world key: controller + level + mech + world clock; split from vanillahud-102: Targeting depth camera, CAP zoom camera, TacOps overlay)");
        EnsureBlock();
    }

    // =====================================================================================
    // RENDER THREAD
    // =====================================================================================
    // SLOW-STEP TIMER (cameracapture-4): the game-thread tick and the render-thread present, timed whole.
    SlowTimer SlowG{"CameraCapture"}, SlowR{"CameraCapture render"};
    ULONGLONG LastPresentQpc = 0, FreezeQpcFreq = 0; // cameracapture-17: the freeze logger
    void on_present() override
    {
        if (Block) // cameracapture-27: was in the viewport-draw callback, which MW5 never calls; it must also
            EiPawnWatch(GetTickCount64()); // run when the cockpit stops ticking (leaving the mech)
        {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            if (!FreezeQpcFreq)
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                FreezeQpcFreq = (ULONGLONG)f.QuadPart;
            }
            if (LastPresentQpc && Block)
            {
                const double gap = (double)((ULONGLONG)q.QuadPart - LastPresentQpc) * 1000.0 / (double)FreezeQpcFreq;
                if (gap >= 150.0)
                    LogInfo("freeze: %.0f ms between frames (vision phase %d, depth hooks %s, scene depth %s, vision renderer image %llu ms old)",
                            gap, EvPhase, EiHooksOn ? "on" : "off", EiFindPhase == 2 ? "held" : "searching",
                            (unsigned long long)(Block->EiVisFrameMs ? GetTickCount64() - Block->EiVisFrameMs : 0));
            }
            LastPresentQpc = (ULONGLONG)q.QuadPart;
        }
        SlowR.Begin();
        PresentInner();
        SlowR.Step("on_present");
        SlowR.Flush([](const char *l) { uevr::API::get()->log_info("%s", l); });
    }
    void PresentInner()
    {
        if (!Block && !EnsureBlock())
            return;
        const ULONGLONG now = GetTickCount64();
        Block->ProducerTickMs = now;

        DrvInstall();

        const auto *rd = API::get()->param()->renderer;
        if (!rd || rd->renderer_type != UEVR_RENDERER_D3D11 || !rd->device)
        {
            if (!RendererWarned)
            {
                RendererWarned = true;
                LogError("renderer is not D3D11 (type %d) - textures cannot be shared", rd ? rd->renderer_type : -1);
            }
            return;
        }
        auto *dev = static_cast<ID3D11Device *>(rd->device);
        ComPtr<ID3D11DeviceContext> ctx;
        dev->GetImmediateContext(ctx.GetAddressOf());
        if (!ctx)
            return;
        DbProbeTick(dev, ctx.Get(), now); // cameracapture-9: depth-buffer probe (read only; off since -12)
        {
            const uint32_t t = HudTicks.load(std::memory_order_relaxed);
            if (t != LiveTickSeen)
            {
                LiveTickSeen = t;
                PresentsSinceTick = 0;
            }
            else
                ++PresentsSinceTick;
        }
        EiPresent(dev, ctx.Get(), now, GameLive(now)); // cameracapture-12

        // Nothing is published while the game-thread driver is not ticking - a loading screen, a
        // level being torn down: what the textures show then is stale, and the game thread is not
        // there to vouch for them.
        const bool      live = GameLive(now); // cameracapture-22: frames since the last tick count too
        ComPtr<ID3D11Texture2D> src[VS_COUNT];
        VanillaExportBlock::Camera depthPose{};
        VanillaExportBlock::Lock   depthLock{};
        ComPtr<ID3D11Texture2D>    units;
        BeamSet                    beams;
        {
            std::lock_guard<std::mutex> g(SrcLock);
            for (int i = VS_Depth; i < VS_COUNT; ++i) // this plugin's slots
                src[i] = Src[i];
            depthPose = SrcDepthPose;
            depthLock = SrcDepthLock;
            units     = SrcUnits;
            beams     = SrcBeams;
        }
        // The depth capture, while a Targeting screen asks for it.
        PublishDepth(dev, ctx.Get(), live ? src[VS_Depth].Get() : nullptr, units.Get(), depthPose, depthLock, beams);
        // The CAP page's zoom camera, in its render targets' own formats.
        Publish(dev, ctx.Get(), VS_ZoomCam, live ? src[VS_ZoomCam].Get() : nullptr, DXGI_FORMAT_UNKNOWN);
        Publish(dev, ctx.Get(), VS_ZoomUnits, live ? src[VS_ZoomUnits].Get() : nullptr, DXGI_FORMAT_UNKNOWN);
        // Last, over everything: the TacOps overlay, while HUDDisplay has it up.
        EvDraw(dev, ctx.Get(), rd, live); // cameracapture-13: Enhanced Imaging over the view (under the TacOps overlay)
        OvDraw(dev, ctx.Get(), rd);
    }

    void on_device_reset() override
    {
        ReleaseTextures();
        OvReleaseImage();
        OvVs.Reset(); OvPs.Reset(); OvCb.Reset(); OvSamp.Reset(); OvBlend.Reset(); OvRs.Reset(); OvDss.Reset();
        OvPipeTried = OvPipeOk = false;
        if (Block)
            for (int i = VS_Depth; i < VS_COUNT; ++i) // this plugin's slots only
            {
                Block->Slots[i].Active       = 0;
                Block->Slots[i].SharedHandle = 0;
            }
    }

    // ---- the shared block (MW5_VanillaHUDExport), split three ways since the module split ----
    // VanillaHud, CameraCapture and LanceControl each map the SAME block (HUDDisplay and
    // VoiceCommand read it unchanged) and each writes only its own fields: VanillaHud the HUD
    // slots, CameraCapture the camera slots and the overlay, LanceControl the TacOps table and
    // order ring. The first of them to start in this process clears it (a section adopted from
    // an earlier game run may hold that run's handles); the others leave it be.
    bool EnsureBlock()
    {
        if (Block)
            return true;
        const ULONGLONG now = GetTickCount64();
        if (LastSetupMs && now - LastSetupMs < 2000)
            return false;
        LastSetupMs = now;

        if (!WriterMutex)
        {
            WriterMutex = CreateMutexW(nullptr, TRUE, kWriterMutexName);
            if (!WriterMutex)
                return false;
            if (GetLastError() == ERROR_ALREADY_EXISTS)
            {
                CloseHandle(WriterMutex);
                WriterMutex = nullptr;
                LogError("another %s writer is live - retrying", Name);
                return false;
            }
        }
        MmfHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(sizeof(VanillaExportBlock)), VHUD_MMF_NAME);
        if (!MmfHandle)
            return false;
        Block = static_cast<VanillaExportBlock *>(
            MapViewOfFile(MmfHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VanillaExportBlock)));
        if (!Block)
        {
            CloseHandle(MmfHandle);
            MmfHandle = nullptr;
            return false;
        }
        // First module in this process: clear it. The marker is a process-local named event.
        wchar_t initName[96];
        _snwprintf_s(initName, _countof(initName), _TRUNCATE, L"Local\\MW5_VanillaHUDExport_Init_%lu", GetCurrentProcessId());
        HANDLE init = CreateEventW(nullptr, TRUE, FALSE, initName);
        const bool first = init && GetLastError() != ERROR_ALREADY_EXISTS;
        // (kept open for the life of the process: it is the "already cleared" marker)
        if (first || Block->Version != VHUD_VERSION)
        {
            std::memset(Block, 0, sizeof(VanillaExportBlock));
            Block->Version       = VHUD_VERSION;
            Block->ExportEnabled = 1;
        }
        LogInfo("export block ready (%zu bytes, v%u)%s", sizeof(VanillaExportBlock), VHUD_VERSION,
                first ? " - cleared (first module in this process)" : " - shared");
        return true;
    }

    void CloseBlock(bool lastWriter)
    {
        if (Block)
        {
            if (lastWriter)
                Block->ExportEnabled = 0;
            UnmapViewOfFile(Block);
            Block = nullptr;
        }
        if (MmfHandle)
            CloseHandle(MmfHandle);
        MmfHandle = nullptr;
        if (WriterMutex)
        {
            ReleaseMutex(WriterMutex);
            CloseHandle(WriterMutex);
            WriterMutex = nullptr;
        }
    }

    // ---- the GAME-THREAD DRIVER (module split): the player cockpit's ReceiveTick -------------
    // What LidarExport runs on (confirmed per frame, game thread, vanilla HUD or MWVR alike): the
    // cockpit child actor's own ReceiveTick. The UFunction's native pointer is swapped for ours,
    // which runs our tick and then chains whatever was there (LidarExport's detour, another
    // module's, or the game's). Each cockpit class's function is hooked ONCE and never unhooked -
    // re-hooking one we already sit on would make us our own "original" - and the original is
    // looked up by the function actually running (FFrame::Node), so a chassis change is safe.
    struct DrvHook
    {
        API::UFunction *Fn;
        BP_FUNC         Orig;
    };
    std::vector<DrvHook> DrvHooks;
    std::mutex           DrvMx;
    const void          *DrvClass = nullptr;
    ULONGLONG            DrvNextTryMs = 0;
    std::atomic<ULONGLONG> DrvSeenMs{0};
    std::atomic<API::UObject *> DrvPawn{nullptr}; // bug sweep 15: the local pawn, as the game thread saw it at the last tick
    bool                 DrvWarned = false;

    // The local player's cockpit actor (the one whose tick drives us), or nullptr.
    static API::UObject *LocalCockpit()
    {
        auto *pawn = API::get()->get_local_pawn(0);
        if (!pawn)
            return nullptr;
        auto *meshPP = pawn->get_property_data<void *>(L"MechMeshComponent");
        if (!meshPP || !*meshPP)
            return nullptr;
        auto *cockPP = reinterpret_cast<API::UObject *>(*meshPP)->get_property_data<void *>(L"FirstPersonCockpitComponent");
        if (!cockPP || !*cockPP)
            return nullptr;
        auto *childPP = reinterpret_cast<API::UObject *>(*cockPP)->get_property_data<void *>(L"ChildActor");
        return (childPP && *childPP) ? reinterpret_cast<API::UObject *>(*childPP) : nullptr;
    }

    void DrvInstall()
    {
        const ULONGLONG now = GetTickCount64();
        if (now < DrvNextTryMs)
            return;
        DrvNextTryMs = now + 1000;
        // bug sweep 15 (2026-10-18): nothing to do while the cockpit tick is reaching us - the hook is in and working. The
        // look-up below reads game objects (pawn -> mesh -> cockpit) on the RENDER thread, which can meet a level being torn
        // down; it now runs only when the tick has stopped (menus, a load, another chassis) instead of every second always.
        if (DrvClass && now - DrvSeenMs.load() < 1000)
            return;
        API::UObject *cockpit = LocalCockpit();
        if (!cockpit)
            return; // not in a mech
        API::UClass *cls = cockpit->get_class();
        if (!cls || static_cast<const void *>(cls) == DrvClass)
        {
            // Hooked, but silent for 3 s while in a mech: say so once (the hook was knocked off).
            const ULONGLONG seen = DrvSeenMs.load();
            if (DrvClass && !DrvWarned && seen && now - seen > 3000)
            {
                DrvWarned = true;
                LogError("game-thread driver: the cockpit tick stopped reaching this plugin");
            }
            return;
        }
        API::UFunction *fn = cls->find_function(L"ReceiveTick");
        if (!fn)
            return;
        std::lock_guard<std::mutex> lk(DrvMx);
        for (auto &h : DrvHooks)
            if (h.Fn == fn)
            {
                DrvClass = cls; // hooked before (this chassis again): never twice
                return;
            }
        auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
        DrvHooks.push_back({fn, *slot});
        *slot = &OnDrvTick;
        DrvClass = cls;
        DrvWarned = false;
        LogInfo("game-thread driver: hooked %ls::ReceiveTick (%p)", cls->get_fname()->to_string().c_str(), (void *)fn);
    }

    static void *OnDrvTick(API::UObject *self, FFrame *frame, void *const result)
    {
        BP_FUNC orig = nullptr;
        if (Instance)
        {
            std::lock_guard<std::mutex> lk(Instance->DrvMx);
            for (auto &h : Instance->DrvHooks)
                if (frame && h.Fn == frame->Node)
                    orig = h.Orig;
            if (!orig && !Instance->DrvHooks.empty())
                orig = Instance->DrvHooks.back().Orig; // (never expected: Node is the running function)
        }
        // LOOP GUARD (user 2026-10-17, after the LidarExport / TestPilot stack-overflow crash): four plugins chain this
        // cockpit tick. If the chain ever leads back into us, it is cut at depth 8 - one error line - instead of recursing
        // until the stack overflows. Game thread only, so a thread-local count.
        static thread_local int loopDepth = 0;
        if (loopDepth >= 8)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                API::get()->log_error("[CameraCapture] game-thread driver: the cockpit tick chain loops back into CameraCapture - cut");
            }
            return nullptr;
        }
        ++loopDepth;
        void *r = orig ? orig(self, frame, result) : nullptr; // the cockpit (and anyone before us) first
        --loopDepth;
        // Every mech has a cockpit actor of this class; only the player's drives us.
        if (Instance && self && self == LocalCockpit())
        {
            Instance->DrvPawn = API::get()->get_local_pawn(0); // bug sweep 15 (before the stamp: read with it)
            Instance->DrvSeenMs = GetTickCount64();
            Instance->SlowG.Begin(); // SLOW-STEP TIMER (cameracapture-4)
            Instance->GameTick();
            Instance->SlowG.Step("GameTick");
            Instance->SlowG.Flush([](const char *l) { uevr::API::get()->log_info("%s", l); });
        }
        return r;
    }

    // =====================================================================================
    // GAME THREAD
    // =====================================================================================
    // Driven by the player cockpit's tick (every frame), not the HUD: works under MWVR too.
    void GameTick()
    {
        if (!Block)
            return;
        const ULONGLONG now = GetTickCount64();
        auto *pc = API::get()->get_player_controller(0);
        if (!pc)
            return;
        // Back from a stretch with no tick (death, pause, a menu, loading): the game has likely
        // changed the input mode itself meanwhile, so the overlay's is applied afresh either way.
        if (OvApplied && HudSeenMs && now - HudSeenMs > 1000)
            OvReapply = true;
        HudSeenMs = now;
        HudTicks.fetch_add(1, std::memory_order_relaxed); // cameracapture-22
        // "Hud" is this plugin's world key: the player controller its captures were made on (a
        // new level is a new controller). Everything of the old one is let go first.
        // cameracapture-2: the controller alone missed a second mission that kept the same
        // controller - its level and the player's mech are keyed too (GameTick runs off the
        // player cockpit's tick, so the local pawn here is always the mech).
        API::UObject *level = pc->get_outer();
        API::UObject *pawn  = API::get()->get_local_pawn(0);
        // The clock only every 100 ms (this runs every frame).
        static ULONGLONG clockMs = 0;
        float wt = -1.0f;
        if (now - clockMs >= 100)
        {
            clockMs = now;
            wt = WorldSeconds(pc);
        }
        const bool newPc    = pc != Hud;
        const bool newLevel = !newPc && level != HudLevel;
        const bool newPawn  = !newPc && !newLevel && pawn && HudPawn && pawn != HudPawn;
        const bool newClock = !newPc && !newLevel && !newPawn && wt >= 0.0f && HudWorldT >= 0.0f && wt + 1.0f < HudWorldT;
        if (wt >= 0.0f)
            HudWorldT = wt;
        if (newPc || newLevel || newPawn || newClock)
        {
            if (Hud)
                LogInfo("new world (%s) - camera and overlay state reset",
                        newPc ? "controller changed" : newLevel ? "level changed" : newPawn ? "player mech changed" : "world clock restarted");
            ForgetWorldCam(pc);
            Hud      = pc;
            HudLevel = level;
            HudPawn  = nullptr;
        }
        if (pawn)
            HudPawn = pawn;
        if (!CamResolve())
            return;

        // Every frame: the game camera, and the depth capture following it. Only while a
        // Targeting screen is up - it is the one consumer, and the capture is a second scene render.
        const uint64_t tw = Block->TargetingWantMs;
        const bool targeting = Recent(tw, now, kVhudTargetingStaleMs);
        if (targeting || ZoomWanted(now) || EiUnitsWanted(now))
            FollowCamera();
        PublishMissileLock(pawn, targeting || ZoomWanted(now)); // cameracapture-5, every frame
        // cameracapture-28: the .pak manager every cockpit tick, vision on or off (-27 had it inside FollowCamera,
        // which only runs while the units are wanted: switched off, the weight stayed 1, the wave frozen).
        // Runs on the game thread right after this tick's units capture. (UEVR's viewport-draw callback never fires in MW5.)
        PakTick(now);

        if (now - LastGameMs < kGameTickMs)
            return;
        LastGameMs = now;

        OvTick(pc);           // the TacOps overlay: game input off / on with it
        OvAxisCheck();
        PpProbe(pc, pawn, now); // cameracapture-7: read-only post-process probe

        // The depth capture exists only while a Targeting screen wants it.
        std::vector<API::UObject *> caps;
        const bool resChanged = Capture && ((Block->TargetingFullRes != 0) != CapFullRes); // cameracapture-14
        if (Capture && (!targeting || resChanged || pc != CaptureActor || (ComponentsOn(pc, CaptureClass, caps) && !RingOn(caps))))
            DropCapture(pc == CaptureActor);
        if (targeting && !Capture && now - CaptureFailMs > 5000)
            if (!MakeCapture(pc))
                CaptureFailMs = now;
        // The CAP page's zoom camera.
        TickZoom(pc, now);
        // Enhanced Imaging's units capture (cameracapture-12).
        TickEi(pc, now);

        if (Capture || ZCap || EiUCaps[0])
        {
            HideOwnMech(now);
            RefreshUnits(pc, now);
        }
    }

    // What the captures need (was VanillaHud's ResolveClasses, HUD parts left there).
    bool CamResolved = false;
    bool CamResolve()
    {
        if (CamResolved)
            return CaptureClass != nullptr;
        CamResolved = true;
        if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.WidgetLayoutLibrary"))
            LayoutLib = c->get_class_default_object();
        if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.KismetRenderingLibrary"))
            RenderLib = c->get_class_default_object();
        CaptureClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SceneCaptureComponent2D");
        LogInfo("capture class %s, rendering lib %s, layout lib %s", CaptureClass ? "found" : "NOT FOUND",
                RenderLib ? "found" : "NOT FOUND", LayoutLib ? "found" : "NOT FOUND");
        return CaptureClass != nullptr;
    }

    float WorldSeconds(API::UObject *pc)
    {
        static API::UObject *gs = nullptr;
        static bool off = false; // not callable here: pointers only, said once
        if (!gs)
            if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameplayStatics"))
                gs = c->get_class_default_object();
        float t = -1.0f;
        if (off || !gs)
            return -1.0f;
        if (CallRet(gs, L"GetTimeSeconds", {{L"WorldContextObject", &pc, sizeof(pc)}}, &t, sizeof(t)))
            return t;
        off = true;
        LogWarn("world clock: GameplayStatics.GetTimeSeconds could not be called - new missions are told by pointers only");
        return -1.0f;
    }

    // A new world (controller change): the cameras' and the overlay's own per-world state.
    void ForgetWorldCam(API::UObject *pc)
    {
        if (ZCap)
        {
            std::vector<API::UObject *> caps;
            const bool alive = pc && pc == ZCapActor && CaptureClass && ComponentsOn(pc, CaptureClass, caps) && ZoomOn(caps);
            DropZoomCam(alive);
        }
        if (Capture)
        {
            std::vector<API::UObject *> caps;
            const bool alive = pc && pc == CaptureActor && CaptureClass && ComponentsOn(pc, CaptureClass, caps) && RingOn(caps);
            DropCapture(alive);
        }
        if (EiUCaps[0])
        {
            std::vector<API::UObject *> caps;
            const bool alive = pc && pc == EiActor && CaptureClass && ComponentsOn(pc, CaptureClass, caps) && EiRingOn(caps);
            DropEi(alive);
        }
        DepthRtObj = nullptr;
        ClearSource(VS_Depth);
        HiddenActors.clear();
        NextHideMs  = 0;
        OvApplied   = false; // re-applied to the new world's controller if the overlay is still up
        OvAppliedPc = nullptr;
        // cameracapture-35 (bug sweep): the input swallowing goes with the old world too - with the overlay closed in
        // the new one OvTick returned early (closed == not applied) and mouse look stayed swallowed until it was
        // opened and closed again. Re-applied by OvTick if the overlay is still up.
        OvInputOn      = false;
        OvCursorInside = false;
        PwCls = nullptr;     // the pilot watch's cached handler layout: re-read from the new world
        PwFields.clear();
    }


    // The depth pair: one shared texture twice as wide - scene depth on the left, units-only
    // depth on the right - so both halves, the pose and the lock arrive under one key, together.
    void PublishDepth(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *scene, ID3D11Texture2D *units,
                      VanillaExportBlock::Camera pose, const VanillaExportBlock::Lock &lock, const BeamSet &beams)
    {
        auto &info = Block->Slots[VS_Depth];
        if (!scene)
        {
            info.Active       = 0;
            info.SharedHandle = 0;
            return;
        }
        D3D11_TEXTURE2D_DESC sd{};
        scene->GetDesc(&sd);
        const UINT w = sd.Width;
        D3D11_TEXTURE2D_DESC pair = sd;
        pair.Width = w * 2;
        if (sd.SampleDesc.Count != 1 || !EnsureShared(dev, VS_Depth, pair, DXGI_FORMAT_UNKNOWN))
        {
            info.Active       = 0;
            info.SharedHandle = 0;
            return;
        }
        bool unitsOk = false;
        if (units)
        {
            D3D11_TEXTURE2D_DESC ud{};
            units->GetDesc(&ud);
            unitsOk = ud.Width == sd.Width && ud.Height == sd.Height && ud.Format == sd.Format && ud.SampleDesc.Count == 1;
        }
        info.Width        = SharedW[VS_Depth];
        info.Height       = SharedH[VS_Depth];
        info.SharedHandle = reinterpret_cast<uint64_t>(SharedHandle[VS_Depth]);
        if (!info.Active)
        {
            info.Active = 1;
            LogInfo("depth pair published: %ux%u (scene | units%s)", SharedW[VS_Depth], SharedH[VS_Depth],
                    unitsOk ? "" : " - not yet");
        }
        ComPtr<IDXGIKeyedMutex> m;
        if (FAILED(Shared[VS_Depth].As(&m)) || m->AcquireSync(0, 0) != S_OK)
            return;
        ctx->CopySubresourceRegion(Shared[VS_Depth].Get(), 0, 0, 0, 0, scene, 0, nullptr);
        if (unitsOk)
            ctx->CopySubresourceRegion(Shared[VS_Depth].Get(), 0, w, 0, 0, units, 0, nullptr);
        pose.UnitsValid = unitsOk ? 1.0f : 0.0f;
        Block->DepthCam  = pose;
        Block->DepthLock = lock;
        Block->BeamCount = beams.Count;
        std::memcpy(Block->Beams, beams.B, sizeof(beams.B[0]) * beams.Count);
        m->ReleaseSync(1);
    }

    void Publish(ID3D11Device *dev, ID3D11DeviceContext *ctx, int i, ID3D11Texture2D *src, DXGI_FORMAT fmt,
                 const VanillaExportBlock::Camera *pose = nullptr, const VanillaExportBlock::Lock *lock = nullptr)
    {
        auto &info = Block->Slots[i];
        if (!src)
        {
            info.Active       = 0;
            info.SharedHandle = 0;
            return;
        }
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.SampleDesc.Count != 1 || !EnsureShared(dev, i, sd, fmt))
        {
            info.Active       = 0;
            info.SharedHandle = 0;
            return;
        }
        info.Width        = SharedW[i];
        info.Height       = SharedH[i];
        info.SharedHandle = reinterpret_cast<uint64_t>(SharedHandle[i]);
        if (!info.Active)
        {
            info.Active = 1;
            LogInfo("%ls published: %ux%u", VanillaSourceName(i), SharedW[i], SharedH[i]);
        }
        CopyInto(ctx, i, src, pose, lock);
    }

    // Every capture of the ring still on the controller (component list `caps`).
    bool RingOn(const std::vector<API::UObject *> &caps) const
    {
        for (auto *c : Caps)
            if (!c || std::find(caps.begin(), caps.end(), c) == caps.end())
                return false;
        for (auto *c : UCaps)
            if (c && std::find(caps.begin(), caps.end(), c) == caps.end())
                return false;
        return true;
    }

    // Widget components on an actor, from the ENGINE's own list. The out-array lives in a buffer
    // kept for this call alone and never zeroed: the engine assigns each result over the last and
    // frees it (zeroing would orphan it every call - the LidarExport leak).
    bool ComponentsOn(API::UObject *actor, API::UClass *cls, std::vector<API::UObject *> &out)
    {
        out.clear();
        if (!actor || !cls)
            return false;
        if (!CbcFn)
        {
            CbcFn = FindFn(actor, L"K2_GetComponentsByClass");
            if (!CbcFn)
                return false;
            CbcClassOff = ParamOffset(CbcFn, L"ComponentClass");
            CbcRetOff   = ParamOffset(CbcFn, L"ReturnValue");
            const int32_t frame = CbcFn->get_properties_size();
            if (CbcClassOff < 0 || CbcRetOff < 0 || frame <= 0)
            {
                CbcFn = nullptr;
                return false;
            }
            CbcBuf.assign((size_t)frame + 16, 0);
        }
        std::memcpy(CbcBuf.data() + CbcClassOff, &cls, sizeof(void *));
        actor->process_event(CbcFn, CbcBuf.data());
        struct Arr
        {
            API::UObject **Data;
            int32_t        Num, Max;
        } arr{};
        std::memcpy(&arr, CbcBuf.data() + CbcRetOff, sizeof(arr));
        for (int32_t i = 0; arr.Data && i < arr.Num; ++i)
            out.push_back(arr.Data[i]);
        return true;
    }

    // =====================================================================================
    // CAMERA AND DEPTH CAPTURE
    //
    // The Targeting screen is HUDDisplay's 3D map drawn from the pilot's own camera, with the
    // outlines of the game's real meshes laid over it. Those outlines come from a
    // SceneCaptureComponent2D on the player controller: the engine renders the live scene - every
    // mech, variant, mod and animation, exactly as they are - from our copy of the game camera,
    // but writes linear DEPTH instead of colour, so there is no lighting, weather or texture in
    // it. HUDDisplay turns depth discontinuities into green lines. It exists only while a
    // Targeting screen is open, because it is a second render of the scene.
    // =====================================================================================
    int32_t PcmOff = -1, CapFovOff = -1;

    void FollowCamera()
    {
        auto *pc = API::get()->get_player_controller(0);
        if (!pc)
            return;
        if (PcmOff < 0)
        {
            auto *p = FindProp(pc->get_class(), L"PlayerCameraManager");
            if (!p || !p->get_class() || p->get_class()->get_name() != L"ObjectProperty")
                return;
            PcmOff = p->get_offset();
        }
        auto *pcm = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(pc) + PcmOff);
        if (!pcm)
            return;
        float loc[3] = {}, rot[3] = {}, fov = 90.0f;
        if (!CallRet(pcm, L"GetCameraLocation", {}, loc, sizeof(loc)) ||
            !CallRet(pcm, L"GetCameraRotation", {}, rot, sizeof(rot)))
            return;
        CallRet(pcm, L"GetFOVAngle", {}, &fov, sizeof(fov));

        auto &c = Block->Cam;
        c.Seq = c.Seq | 1u; // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(c.Loc, loc, sizeof(loc));
        std::memcpy(c.Rot, rot, sizeof(rot));
        c.FovDeg = fov;
        c.Valid  = 1;
        std::atomic_thread_fence(std::memory_order_release);
        c.Seq = c.Seq + 1; // even: complete

        FollowZoom(pc, loc, rot, fov);
        if (!Recent(EiLateMs.load(), GetTickCount64(), 1000)) // cameracapture-19: else aimed at draw time
        {
            FollowEi(pc, loc, rot, fov); // cameracapture-12
            EiFollowMs.store(GetTickCount64());
        }

        // Only a capture made on THIS controller under THIS HUD: after a level change the old one is
        // freed memory (vanillahud-5 moved it anyway and crashed).
        if (Capture && CaptureActor == pc && CaptureHud == Hud)
        {
            // Optical zoom: the Targeting screen's magnification narrows the capture's own field of
            // view, so the engine re-renders the narrower view at full resolution.
            float zoom = Block->TargetingZoom;
            zoom = (zoom >= 1.0f && zoom <= kVhudMaxZoom) ? zoom : 1.0f;
            const float capFov = 2.0f * std::atan(std::tan(fov * 0.5f * 0.01745329252f) / zoom) * 57.29577951f;

            // This frame's capture: the next in the ring, rendered once from this exact pose.
            const int k = (int)(CapCount % kCaps);
            API::UObject *cap = Caps[k];
            bool yes = true;
            Call(cap, L"K2_SetWorldLocationAndRotation",
                 {{L"NewLocation", loc, sizeof(loc)}, {L"NewRotation", rot, sizeof(rot)}, {L"bTeleport", &yes, 1}});
            if (CapFovOff >= 0)
                *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(cap) + CapFovOff) = capFov;
            Call(cap, L"CaptureScene", {});
            if (API::UObject *ucap = UCaps[k])
            {
                Call(ucap, L"K2_SetWorldLocationAndRotation",
                     {{L"NewLocation", loc, sizeof(loc)}, {L"NewRotation", rot, sizeof(rot)}, {L"bTeleport", &yes, 1}});
                if (CapFovOff >= 0)
                    *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(ucap) + CapFovOff) = capFov;
                Call(ucap, L"CaptureScene", {});
            }
            VanillaExportBlock::Camera &pose = CapPose[k];
            std::memcpy(pose.Loc, loc, sizeof(loc));
            std::memcpy(pose.Rot, rot, sizeof(rot));
            pose.FovDeg = capFov; // the image's own horizontal field of view, zoom included
            pose.Valid  = 1;
            pose.Seq    = (uint32_t)(CapCount * 2u);
            ReadLock(CapLock[k]); // the locked target as it stands in this same frame
            ReadBeams(pc, CapBeams[k]); // and the lasers firing in it
            ++CapCount;

            // Hand on the capture from two frames ago: the engine lets the render thread trail
            // the game thread by one frame at most, so that one is finished.
            if (CapCount >= 3)
            {
                const int r = (int)((CapCount - 3) % kCaps);
                SetDepthSource(reinterpret_cast<UTextureRenderTarget2D *>(CapRt[r]),
                               reinterpret_cast<UTextureRenderTarget2D *>(URt[r]), CapPose[r], CapLock[r], CapBeams[r]);
            }
        }
    }

    // =====================================================================================
    // ZOOM CAMERA - HUDDisplay's CAP page
    //
    // A full-colour SceneCaptureComponent2D following the game camera at its own magnification:
    // the real scene re-rendered sharp at every zoom, instead of an enlarged crop of the screen.
    // Half the viewport's resolution, every frame, only while the page asks for it.
    //
    // IR mode swaps the colour capture to scene colour HDR + scene depth in alpha (RGBA32F) and
    // runs a units-only depth capture beside it; HUDDisplay turns the pair into a thermal image:
    // cold scenery dark, visible units white.
    // =====================================================================================
    // Three captures, each with ONE fixed job: colour (final colour, RGBA8), IR colour (scene
    // colour HDR + depth in alpha, RGBA32F) and units-only depth. Switching IR on or off only
    // starts and stops rendering (bCaptureEveryFrame) - nothing about a live capture is ever
    // re-pointed. vanillahud-15 changed one capture's source and target on the fly, and turning
    // IR off crashed the game inside the engine's capture render.
    API::UObject *ZCap = nullptr, *ZICap = nullptr, *ZUCap = nullptr;

    API::UObject *ZRtLdr = nullptr, *ZRtIr = nullptr, *ZURt = nullptr;

    API::UObject *ZCapActor = nullptr, *ZCapHud = nullptr;

    bool          ZIrOn = false, ZPaused = false;

    ULONGLONG     ZFailMs = 0;

    int32_t       CapSourceOff = -1, CapTargetOff = -1;

    API::FBoolProperty *CapEveryFrameProp = nullptr;

    bool ZoomWanted(ULONGLONG now) const
    {
        const uint64_t zw = Block ? Block->ZoomCamWantMs : 0;
        return Recent(zw, now, kVhudTargetingStaleMs);
    }

    API::UObject *MakeRt(API::UObject *pc, int32_t w, int32_t h, uint8_t fmt)
    {
        API::UObject *rt = nullptr;
        if (!CallRet(RenderLib, L"CreateRenderTarget2D",
                     {{L"WorldContextObject", &pc, sizeof(void *)}, {L"Width", &w, 4}, {L"Height", &h, 4}, {L"Format", &fmt, 1}},
                     &rt, sizeof(rt)))
            return nullptr;
        return rt;
    }

    // A colour capture: MakeOneCapture builds a depth one, which is re-pointed ONCE, before it
    // has ever rendered, at its own target and source, and never touched again.
    API::UObject *MakeColourCapture(API::UObject *pc, int32_t w, int32_t h, uint8_t rtFmt, uint8_t source, API::UObject *&rt)
    {
        API::UObject *comp = nullptr, *depthRt = nullptr;
        rt = nullptr;
        if (!MakeOneCapture(pc, w, h, comp, depthRt))
            return nullptr;
        rt = MakeRt(pc, w, h, rtFmt);
        if (!rt || CapSourceOff < 0 || CapTargetOff < 0 || !CapEveryFrameProp)
        {
            Call(comp, L"K2_DestroyComponent", {{L"Object", &comp, sizeof(void *)}});
            rt = nullptr;
            return nullptr;
        }
        *(reinterpret_cast<uint8_t *>(comp) + CapSourceOff) = source;
        *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(comp) + CapTargetOff) = rt;
        return comp;
    }

    bool MakeZoomCam(API::UObject *pc)
    {
        if (!CaptureClass || !RenderLib || !LayoutLib)
            return false;
        float vp[2] = {};
        if (!CallRet(LayoutLib, L"GetViewportSize", {{L"WorldContextObject", &pc, sizeof(void *)}}, vp, sizeof(vp)) ||
            vp[0] < 64.0f || vp[1] < 64.0f)
            return false;
        const int32_t w = (int32_t)(vp[0] * 0.5f), h = (int32_t)(vp[1] * 0.5f);

        ZCap = MakeColourCapture(pc, w, h, 2, 2, ZRtLdr); // RTF_RGBA8, SCS_FinalColorLDR
        if (!ZCap)
        {
            LogError("zoom camera: colour capture could not be made");
            return false;
        }
        // IR: HDR colour + depth in cm (RTF_RGBA32f - a half float stops at 655 m), and units.
        ZICap = MakeColourCapture(pc, w, h, 9, 3, ZRtIr); // SCS_SceneColorSceneDepth
        API::UObject *ucomp = nullptr, *urt = nullptr;
        if (ZICap && MakeOneCapture(pc, w, h, ucomp, urt, true))
        {
            if (auto *p = FindProp(ucomp->get_class(), L"PrimitiveRenderMode"))
            {
                *(reinterpret_cast<uint8_t *>(ucomp) + p->get_offset()) = 2; // PRM_UseShowOnlyList
                ZUCap = ucomp;
                ZURt  = urt;
            }
            else
                Call(ucomp, L"K2_DestroyComponent", {{L"Object", &ucomp, sizeof(void *)}});
        }
        if (ZICap && !ZUCap)
        {
            Call(ZICap, L"K2_DestroyComponent", {{L"Object", &ZICap, sizeof(void *)}});
            ZICap = nullptr;
            ZRtIr = nullptr;
        }
        ZCapActor   = pc;
        ZCapHud     = Hud;
        NextHideMs  = 0;
        NextUnitsMs = 0;
        HiddenActors.clear();
        ZIrOn   = false;
        ZPaused = false;
        Render(false);
        LogInfo("zoom camera running (%dx%d colour, every frame%s)", w, h, ZICap ? ", IR ready" : ", IR unavailable");
        return true;
    }

    // Which captures render: colour only, or IR colour + units. Paused: none.
    void Render(bool paused)
    {
        const bool ir = ZIrOn && ZICap && ZUCap;
        if (ZCap)
            CapEveryFrameProp->set_value_in_object(ZCap, !paused && !ir);
        if (ZICap)
            CapEveryFrameProp->set_value_in_object(ZICap, !paused && ir);
        if (ZUCap)
            CapEveryFrameProp->set_value_in_object(ZUCap, !paused && ir);
    }

    // Only on a new world / controller. Closing the page PAUSES the captures instead (see
    // TickZoom): nothing is destroyed while it might be mid-render.
    void DropZoomCam(bool alive)
    {
        ClearSource(VS_ZoomCam);
        ClearSource(VS_ZoomUnits);
        if (alive)
            for (API::UObject *c : {ZCap, ZICap, ZUCap})
                if (c)
                {
                    API::UObject *cc = c;
                    Call(cc, L"K2_DestroyComponent", {{L"Object", &cc, sizeof(void *)}});
                }
        const bool had = ZCap != nullptr;
        ZCap = ZICap = ZUCap = ZRtLdr = ZRtIr = ZURt = nullptr;
        ZCapActor = ZCapHud = nullptr;
        ZIrOn = ZPaused = false;
        if (had)
            LogInfo("zoom camera removed%s", alive ? "" : " (its world is gone - forgotten, not touched)");
    }

    bool ZoomOn(const std::vector<API::UObject *> &caps) const
    {
        for (API::UObject *c : {ZCap, ZICap, ZUCap})
            if (c && std::find(caps.begin(), caps.end(), c) == caps.end())
                return false;
        return ZCap != nullptr;
    }

    // Every frame, from FollowCamera: the zoom captures onto the game camera at its magnification.
    void FollowZoom(API::UObject *pc, const float loc[3], const float rot[3], float fov)
    {
        if (!ZCap || ZPaused || ZCapActor != pc || ZCapHud != Hud)
            return;
        float zoom = Block->ZoomCamZoom;
        zoom = (zoom >= 1.0f && zoom <= kVhudMaxZoom) ? zoom : 1.0f;
        const float capFov = 2.0f * std::atan(std::tan(fov * 0.5f * 0.01745329252f) / zoom) * 57.29577951f;
        // cameracapture-5: the pose the zoom camera is pointed with, for the CAP page's lock
        // bracket, plate and missile lock (VHUD v20). Seqlock as Cam.
        {
            auto &z = Block->ZoomCam;
            z.Seq = z.Seq | 1u;
            std::atomic_thread_fence(std::memory_order_release);
            std::memcpy(z.Loc, loc, sizeof(float) * 3);
            std::memcpy(z.Rot, rot, sizeof(float) * 3);
            z.FovDeg = capFov;
            z.Valid  = 1;
            std::atomic_thread_fence(std::memory_order_release);
            z.Seq = z.Seq + 1;
        }
        bool yes = true;
        for (API::UObject *c : {ZCap, ZICap, ZUCap})
        {
            if (!c)
                continue;
            Call(c, L"K2_SetWorldLocationAndRotation",
                 {{L"NewLocation", loc, sizeof(float) * 3}, {L"NewRotation", rot, sizeof(float) * 3}, {L"bTeleport", &yes, 1}});
            if (CapFovOff >= 0)
                *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(c) + CapFovOff) = capFov;
        }
    }

    // 10 Hz, from GameTick: create / pause / resume / mode / publish.
    void TickZoom(API::UObject *pc, ULONGLONG now)
    {
        const bool want = ZoomWanted(now);
        if (ZCap && pc != ZCapActor)
            DropZoomCam(false); // another controller: the old one's components went with it
        std::vector<API::UObject *> caps;
        if (ZCap && ComponentsOn(pc, CaptureClass, caps) && !ZoomOn(caps))
            DropZoomCam(false);
        if (want && !ZCap && now - ZFailMs > 5000)
            if (!MakeZoomCam(pc))
                ZFailMs = now;
        if (!ZCap)
            return;
        if (!want)
        {
            if (!ZPaused)
            {
                ZPaused = true;
                Render(true);
                ClearSource(VS_ZoomCam);
                ClearSource(VS_ZoomUnits);
                LogInfo("zoom camera paused");
            }
            return;
        }
        const bool ir = Block->ZoomCamIR != 0 && ZICap && ZUCap;
        if (ZPaused || ir != ZIrOn)
        {
            const bool wasPaused = ZPaused;
            ZPaused = false;
            ZIrOn   = ir;
            Render(false);
            LogInfo("zoom camera: %s%s", ir ? "IR" : "colour", wasPaused ? " (resumed)" : "");
        }
        SetSource(VS_ZoomCam, reinterpret_cast<UTextureRenderTarget2D *>(ZIrOn ? ZRtIr : ZRtLdr));
        if (ZIrOn)
            SetSource(VS_ZoomUnits, reinterpret_cast<UTextureRenderTarget2D *>(ZURt));
        else
            ClearSource(VS_ZoomUnits);
    }

    // =====================================================================================
    // POST-PROCESS PROBE (cameracapture-7) - READ ONLY, nothing is changed. Scoping work for
    // projecting V-POD / IR into the game view with the game's OWN effects instead of a second
    // scene render. Everything goes to log.txt as "[pp]" lines:
    //   1. 20 s into the first mission, ONE walk of the object array (about the cost of one failed
    //      find_uobject - a single hitch of up to ~1 s, once per session): every loaded Material /
    //      MaterialInstanceConstant named like a vision mode, an outline or a post-process, with
    //      its domain and blend settings, or its parent and scalar / vector / texture parameters;
    //      plus the names of every loaded post-process-domain material.
    //   2. Then: the player mech's components that look like vision modes, their settings, and
    //      which vision functions exist on the mech, its components and the controller.
    //   3. Every second: the player camera's post-process blendables and blend weight, logged
    //      when they change - toggle night / thermal vision in game and the log shows what the
    //      game itself adds to the camera, and how.
    //   4. The first few targets: whether their meshes render custom depth / stencil, which is
    //      what an outline post-process keys on.
    // =====================================================================================
    static constexpr bool kPpProbe = false; // cameracapture-12: done (results in HANDOFF)
    bool        PpScanned = false;
    ULONGLONG   PpFirstMs = 0, PpCamMs = 0;
    std::wstring PpCamSig;
    void       *PpTgtSeen[6] = {};
    int         PpTgtN = 0;
    API::UClass *PpActorComp = nullptr, *PpCamClass = nullptr, *PpPrimClass = nullptr;

    static bool PpNameHas(const std::wstring &n, const wchar_t *w)
    {
        std::wstring a = n, b = w;
        for (auto &c : a) c = (wchar_t)towlower(c);
        for (auto &c : b) c = (wchar_t)towlower(c);
        return a.find(b) != std::wstring::npos;
    }

    // Simple-typed properties of an object, compactly: name=value. `only` filters by name (any of).
    std::wstring PpProps(API::UObject *o, std::initializer_list<const wchar_t *> only = {}, size_t cap = 2500)
    {
        std::wstring out;
        if (!o || !o->get_class())
            return out;
        for (auto *st = static_cast<API::UStruct *>(o->get_class()); st; st = st->get_super_struct())
        {
            const std::wstring sn = st->get_fname() ? st->get_fname()->to_string() : L"";
            if (sn == L"Object" || sn == L"ActorComponent" || sn == L"Actor" || sn == L"SceneComponent")
                break;
            for (auto *f = st->get_child_properties(); f && out.size() < cap; f = f->get_next())
            {
                const std::wstring n = f->get_fname()->to_string();
                if (only.size())
                {
                    bool any = false;
                    for (auto *w : only)
                        any = any || PpNameHas(n, w);
                    if (!any)
                        continue;
                }
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring t = PropType(p);
                const uint8_t *b = reinterpret_cast<const uint8_t *>(o) + p->get_offset();
                wchar_t v[160] = L"";
                if (t == L"BoolProperty")
                    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%d", reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(o) ? 1 : 0);
                else if (t == L"ByteProperty" || t == L"EnumProperty")
                    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%u", (unsigned)b[0]);
                else if (t == L"IntProperty")
                    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%d", *reinterpret_cast<const int32_t *>(b));
                else if (t == L"FloatProperty")
                    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%.3f", *reinterpret_cast<const float *>(b));
                else if (t == L"ObjectProperty")
                {
                    auto *x = *reinterpret_cast<API::UObject *const *>(b);
                    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%ls", x ? (ClassName(x) + L":" + (x->get_fname() ? x->get_fname()->to_string() : L"?")).c_str() : L"null");
                }
                else if (t == L"NameProperty")
                    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%ls", reinterpret_cast<const API::FName *>(b)->to_string().c_str());
                else
                    _snwprintf_s(v, _countof(v), _TRUNCATE, L"<%ls>", t.c_str());
                out += L" " + n + L"=" + v;
            }
        }
        return out;
    }

    // A MaterialInstance's parameter array: ScalarParameterValues (kind 0), VectorParameterValues
    // (1) or TextureParameterValues (2), read through reflection - names from ParameterInfo.Name.
    std::wstring PpParams(API::UObject *mi, const wchar_t *arrName, int kind)
    {
        std::wstring out;
        API::FProperty *p = FindProp(mi->get_class(), arrName);
        if (!p || PropType(p) != L"ArrayProperty")
            return out;
        API::FProperty *inner = reinterpret_cast<API::FArrayProperty *>(p)->get_inner();
        if (!inner || PropType(inner) != L"StructProperty")
            return out;
        API::UScriptStruct *st = reinterpret_cast<API::FStructProperty *>(inner)->get_struct();
        if (!st)
            return out;
        const int32_t sz = st->get_struct_size();
        API::FProperty *pi = st->find_property(L"ParameterInfo");
        API::FProperty *pv = st->find_property(L"ParameterValue");
        if (!pi || !pv || sz <= 0 || PropType(pi) != L"StructProperty")
            return out;
        API::UScriptStruct *pis = reinterpret_cast<API::FStructProperty *>(pi)->get_struct();
        API::FProperty *pn = pis ? pis->find_property(L"Name") : nullptr;
        if (!pn)
            return out;
        struct Arr
        {
            uint8_t *Data;
            int32_t  Num, Max;
        } arr{};
        std::memcpy(&arr, reinterpret_cast<uint8_t *>(mi) + p->get_offset(), sizeof(arr));
        for (int32_t i = 0; arr.Data && i < arr.Num && i < 64; ++i)
        {
            const uint8_t *e = arr.Data + (size_t)i * sz;
            const std::wstring nm = reinterpret_cast<const API::FName *>(e + pi->get_offset() + pn->get_offset())->to_string();
            const uint8_t *v = e + pv->get_offset();
            wchar_t b[200];
            if (kind == 0)
                _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls=%.3f", nm.c_str(), *reinterpret_cast<const float *>(v));
            else if (kind == 1)
            {
                const float *c = reinterpret_cast<const float *>(v);
                _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls=(%.2f %.2f %.2f %.2f)", nm.c_str(), c[0], c[1], c[2], c[3]);
            }
            else
            {
                auto *t = *reinterpret_cast<API::UObject *const *>(v);
                _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls=%ls", nm.c_str(), t && t->get_fname() ? t->get_fname()->to_string().c_str() : L"null");
            }
            out += b;
        }
        return out;
    }

    void PpScanMaterials()
    {
        auto *mat = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Material");
        auto *mic = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.MaterialInstanceConstant");
        auto *arr = API::FUObjectArray::get();
        if (!mat || !arr)
        {
            LogWarn("[pp] Material class or object array not found - material scan skipped");
            return;
        }
        int32_t domOff = -1;
        if (auto *dp = FindProp(mat, L"MaterialDomain"))
            domOff = dp->get_offset();
        LARGE_INTEGER fq{}, t0{}, t1{};
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        const int32_t n = arr->get_object_count();
        int dumped = 0, mats = 0, mics = 0;
        std::wstring ppDomain;
        for (int32_t i = 0; i < n; ++i)
        {
            API::UObject *o = arr->get_object(i);
            if (!o)
                continue;
            void *c = o->get_class();
            const bool isMat = c == static_cast<void *>(mat), isMic = mic && c == static_cast<void *>(mic);
            if (!isMat && !isMic)
                continue;
            (isMat ? mats : mics)++;
            const std::wstring nm = o->get_fname() ? o->get_fname()->to_string() : L"";
            if (isMat && domOff >= 0 && reinterpret_cast<const uint8_t *>(o)[domOff] == 4 && ppDomain.size() < 3000)
                ppDomain += L" " + nm; // MD_PostProcess
            const bool want = PpNameHas(nm, L"vision") || PpNameHas(nm, L"thermal") || PpNameHas(nm, L"outline") ||
                              PpNameHas(nm, L"_pp") || PpNameHas(nm, L"pp_") || PpNameHas(nm, L"postprocess");
            if (!want || dumped >= 40)
                continue;
            ++dumped;
            std::wstring d;
            if (isMat)
                d = PpProps(o, {L"MaterialDomain", L"Blendable", L"BlendMode", L"bIsBlendable", L"bUsedWithSkeletal"});
            else
            {
                API::UObject *parent = ObjProp(o, L"Parent");
                d = L" parent=" + (parent && parent->get_fname() ? parent->get_fname()->to_string() : std::wstring(L"null")) +
                    L" | scalars:" + PpParams(o, L"ScalarParameterValues", 0) + L" | vectors:" +
                    PpParams(o, L"VectorParameterValues", 1) + L" | textures:" + PpParams(o, L"TextureParameterValues", 2);
            }
            LogInfo("[pp] %ls %ls:%ls", isMat ? L"Material" : L"MIC", o->get_full_name().c_str(), d.c_str());
        }
        QueryPerformanceCounter(&t1);
        LogInfo("[pp] scan: %d objects, %d materials, %d instances, %d dumped, %.0f ms", n, mats, mics, dumped,
                (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
        LogInfo("[pp] post-process-domain materials loaded:%ls", ppDomain.empty() ? L" (none)" : ppDomain.c_str());
    }

    void PpVision(API::UObject *pc, API::UObject *pawn)
    {
        if (!PpActorComp)
            PpActorComp = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        LogInfo("[pp] mech vision-ish properties:%ls", PpProps(pawn, {L"vision", L"thermal", L"night", L"postprocess"}).c_str());
        std::vector<API::UObject *> comps;
        if (PpActorComp && ComponentsOn(pawn, PpActorComp, comps))
        {
            int shown = 0;
            for (auto *c : comps)
            {
                bool hit = false;
                for (auto *st = static_cast<API::UStruct *>(c->get_class()); st && !hit; st = st->get_super_struct())
                {
                    const std::wstring sn = st->get_fname() ? st->get_fname()->to_string() : L"";
                    hit = PpNameHas(sn, L"vision") || PpNameHas(sn, L"thermal") || PpNameHas(sn, L"night") ||
                          PpNameHas(sn, L"postprocess") || PpNameHas(sn, L"sensor");
                }
                if (!hit || shown++ >= 8)
                    continue;
                LogInfo("[pp] component %ls:%ls", ClassName(c).c_str(), PpProps(c).c_str());
            }
            LogInfo("[pp] %zu components on the mech, %d vision-like", comps.size(), shown);
        }
        static const wchar_t *fns[] = {L"ToggleNightVision", L"ToggleThermalVision", L"ToggleVisionMode", L"SetVisionMode",
                                       L"CycleVisionMode", L"NightVisionPostProcess", L"SetNightVision", L"SetThermalVision"};
        std::wstring have;
        for (auto *f : fns)
        {
            if (FindFn(pawn, f))
                have += std::wstring(L" mech.") + f;
            if (pc && FindFn(pc, f))
                have += std::wstring(L" controller.") + f;
            for (auto *c : comps)
                if (FindFn(c, f))
                {
                    have += L" " + ClassName(c) + L"." + f;
                    break;
                }
        }
        LogInfo("[pp] vision functions:%ls", have.empty() ? L" (none of the names tried)" : have.c_str());
    }

    void PpCamera(API::UObject *pawn)
    {
        if (!PpCamClass)
            PpCamClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.CameraComponent");
        std::vector<API::UObject *> cams;
        if (!PpCamClass || !ComponentsOn(pawn, PpCamClass, cams))
            return;
        if (cams.empty() && PpCamSig != L"-")
        {
            PpCamSig = L"-";
            LogInfo("[pp] no CameraComponent on the mech - the view comes from elsewhere (camera manager / cockpit)");
        }
        if (cams.empty())
            return;
        std::wstring sig;
        for (auto *cam : cams)
        {
            API::FProperty *pps = FindProp(cam->get_class(), L"PostProcessSettings");
            API::FProperty *pbw = FindProp(cam->get_class(), L"PostProcessBlendWeight");
            wchar_t b[200];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L" [%ls weight %.2f", cam->get_fname() ? cam->get_fname()->to_string().c_str() : L"?",
                       pbw ? *reinterpret_cast<const float *>(reinterpret_cast<uint8_t *>(cam) + pbw->get_offset()) : -1.0f);
            sig += b;
            if (pps && PropType(pps) == L"StructProperty")
            {
                API::UScriptStruct *st = reinterpret_cast<API::FStructProperty *>(pps)->get_struct();
                API::FProperty *wb = st ? st->find_property(L"WeightedBlendables") : nullptr;
                API::UScriptStruct *wbs = (wb && PropType(wb) == L"StructProperty") ? reinterpret_cast<API::FStructProperty *>(wb)->get_struct() : nullptr;
                API::FProperty *ar = wbs ? wbs->find_property(L"Array") : nullptr;
                if (ar)
                {
                    struct Arr
                    {
                        uint8_t *Data;
                        int32_t  Num, Max;
                    } a{};
                    std::memcpy(&a, reinterpret_cast<uint8_t *>(cam) + pps->get_offset() + wb->get_offset() + ar->get_offset(), sizeof(a));
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L" blendables %d:", a.Num);
                    sig += b;
                    for (int32_t i = 0; a.Data && i < a.Num && i < 8; ++i)
                    {
                        const float w = *reinterpret_cast<const float *>(a.Data + i * 16);
                        auto *o = *reinterpret_cast<API::UObject *const *>(a.Data + i * 16 + 8);
                        _snwprintf_s(b, _countof(b), _TRUNCATE, L" %.2f*%ls", w, o ? (ClassName(o) + L":" + o->get_fname()->to_string()).c_str() : L"null");
                        sig += b;
                    }
                }
            }
            sig += L"]";
        }
        if (sig != PpCamSig)
        {
            PpCamSig = sig;
            LogInfo("[pp] camera post-process:%ls", sig.c_str());
        }
    }

    void PpTargetDepth(API::UObject *pawn)
    {
        if (PpTgtN >= 6)
            return;
        API::UObject *tt = ObjProp(pawn, L"TargetTracking");
        API::UObject *tgt = tt ? ObjProp(tt, L"CurrentTarget") : nullptr;
        if (!tgt)
            return;
        for (int i = 0; i < PpTgtN; ++i)
            if (PpTgtSeen[i] == tgt)
                return;
        PpTgtSeen[PpTgtN++] = tgt;
        if (!PpPrimClass)
            PpPrimClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.PrimitiveComponent");
        std::vector<API::UObject *> prims;
        if (!PpPrimClass || !ComponentsOn(tgt, PpPrimClass, prims))
            return;
        std::wstring d;
        int k = 0;
        for (auto *c : prims)
        {
            const std::wstring cn = ClassName(c);
            if (!PpNameHas(cn, L"Mesh") || k++ >= 6)
                continue;
            d += L" {" + cn + PpProps(c, {L"bRenderCustomDepth", L"CustomDepthStencilValue", L"CustomDepthStencilWriteMask"}, 400) + L"}";
        }
        LogInfo("[pp] target %ls custom depth:%ls", ClassName(tgt).c_str(), d.empty() ? L" (no mesh components)" : d.c_str());
    }

    void PpProbe(API::UObject *pc, API::UObject *pawn, ULONGLONG now)
    {
        if (!kPpProbe || !pawn)
            return;
        if (!PpFirstMs)
            PpFirstMs = now;
        if (!PpScanned && now - PpFirstMs > 20000)
        {
            PpScanned = true;
            LogInfo("[pp] probe start (read only) - one object-array walk follows");
            PpScanMaterials();
            PpVision(pc, pawn);
        }
        if (PpScanned && now - PpCamMs >= 1000)
        {
            PpCamMs = now;
            PpCamera(pawn);
            PpTargetDepth(pawn);
        }
    }

    // =====================================================================================
    // DEPTH-BUFFER PROBE (cameracapture-9) - READ ONLY. Can V-POD's outlines come from the game's
    // OWN depth buffer (no second scene render)? Steps, each logged as "[db]":
    //   1. 40 s after the first live frame, for 2 s, ID3D11DeviceContext::ClearDepthStencilView is
    //      borrowed: its vtable slot (53) on the immediate context's vtable points at DbClear, which
    //      notes the view (AddRef'd), the flags, the clear value and how often - then calls the
    //      original. The slot is put back after the 2 s (and never touched again).
    //   2. Each depth buffer seen is described: size, format, bind flags (SRV = we could sample it),
    //      sample count, clears, clear depth (0 = reversed Z), against the back buffer's size.
    //   3. The scene-sized one with the most clears is copied once to a CPU texture and five
    //      points read (centre and four around it): raw device Z, and 10 / Z (cm, if reversed
    //      infinite Z with a 10 cm near plane - look at the centre in game and compare).
    // =====================================================================================
    static constexpr bool kDbProbe = false; // cameracapture-12: done - Enhanced Imaging hooks the same functions
    // cameracapture-10: -9 borrowed the vtable slot and saw NOTHING (0 clears in 282 frames) - the
    // engine reaches the context through another interface / vtable. So the FUNCTIONS themselves are
    // hooked now (MinHook, whatever vtable points at them), and depth buffers BOUND are noted as well
    // as cleared ones: a depth buffer has to be bound to be drawn into.
    using ClearDsvFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
    using OmSetFn    = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
    using OmSetUavFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *,
                                                 UINT, UINT, ID3D11UnorderedAccessView *const *, const UINT *);
    static inline ClearDsvFn DbOrig = nullptr;
    static inline OmSetFn    DbOmOrig = nullptr;
    static inline OmSetUavFn DbOmUavOrig = nullptr;
    static inline void      *DbTargets[3] = {};
    struct DbSeen
    {
        ID3D11DepthStencilView *V;
        UINT                    Flags;
        float                   Depth;
        uint32_t                N;     // clears
        uint32_t                Binds;
    };
    static inline DbSeen           DbList[32] = {};
    static inline std::atomic<int> DbCount{0};
    static inline std::atomic<uint32_t> DbCalls{0}, DbBindCalls{0};
    int       DbPhase = 0; // 0 waiting, 1 watching, 2 done
    ULONGLONG DbStartMs = 0, DbFirstLiveMs = 0;
    uint64_t  DbPresents = 0;

    static DbSeen *DbNote(ID3D11DepthStencilView *v)
    {
        if (!v)
            return nullptr;
        const int n = DbCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (DbList[i].V == v)
                return &DbList[i];
        if (n >= 32)
            return nullptr;
        v->AddRef();
        DbList[n] = {v, 0, -1.0f, 0, 0};
        DbCount.store(n + 1, std::memory_order_release);
        return &DbList[n];
    }

    static void STDMETHODCALLTYPE DbClear(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT f, FLOAT d, UINT8 st)
    {
        DbCalls.fetch_add(1, std::memory_order_relaxed);
        if (DbSeen *e = DbNote(v))
        {
            ++e->N;
            e->Flags = f;
            e->Depth = d;
        }
        DbOrig(c, v, f, d, st);
    }
    static void STDMETHODCALLTYPE DbOm(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *v)
    {
        DbBindCalls.fetch_add(1, std::memory_order_relaxed);
        if (DbSeen *e = DbNote(v))
            ++e->Binds;
        DbOmOrig(c, n, rt, v);
    }
    static void STDMETHODCALLTYPE DbOmUav(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *v,
                                          UINT us, UINT un, ID3D11UnorderedAccessView *const *uav, const UINT *ic)
    {
        DbBindCalls.fetch_add(1, std::memory_order_relaxed);
        if (DbSeen *e = DbNote(v))
            ++e->Binds;
        DbOmUavOrig(c, n, rt, v, us, un, uav, ic);
    }

    // Which vtable each context interface uses - logged so the -9 miss is understood.
    void DbVtableDiag(ID3D11DeviceContext *ctx)
    {
        void **vt = *reinterpret_cast<void ***>(ctx);
        std::string d;
        char b[160];
        _snprintf_s(b, _countof(b), _TRUNCATE, "ctx %p vtbl %p [53]=%p [33]=%p", (void *)ctx, (void *)vt, vt[53], vt[33]);
        d += b;
        ComPtr<ID3D11DeviceContext1> c1;
        if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(c1.GetAddressOf()))))
        {
            void **v1 = *reinterpret_cast<void ***>(c1.Get());
            _snprintf_s(b, _countof(b), _TRUNCATE, " | ctx1 %p vtbl %p [53]=%p", (void *)c1.Get(), (void *)v1, v1[53]);
            d += b;
        }
        LogInfo("[db] %s", d.c_str());
    }

    static float DbRead(const uint8_t *row, UINT x, DXGI_FORMAT f, bool &ok)
    {
        ok = true;
        switch (f)
        {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        {
            uint32_t v = 0;
            std::memcpy(&v, row + x * 4, 4);
            return (float)(v & 0xFFFFFFu) / 16777215.0f;
        }
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        {
            float v = 0;
            std::memcpy(&v, row + x * 8, 4);
            return v;
        }
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        {
            float v = 0;
            std::memcpy(&v, row + x * 4, 4);
            return v;
        }
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        {
            uint16_t v = 0;
            std::memcpy(&v, row + x * 2, 2);
            return (float)v / 65535.0f;
        }
        default:
            ok = false;
            return 0.0f;
        }
    }

    void DbProbeTick(ID3D11Device *dev, ID3D11DeviceContext *ctx, ULONGLONG now)
    {
        if (!kDbProbe || DbPhase == 2)
            return;
        ++DbPresents;
        const ULONGLONG seen = HudSeenMs.load(std::memory_order_relaxed);
        const bool live = Recent(seen, now, kPresentStaleMs);
        if (DbPhase == 0)
        {
            if (!live)
                return;
            if (!DbFirstLiveMs)
                DbFirstLiveMs = now;
            if (now - DbFirstLiveMs < 40000)
                return;
            DbVtableDiag(ctx);
            void **vt = *reinterpret_cast<void ***>(ctx);
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                LogError("[db] MinHook could not start (%d) - probe abandoned", (int)init);
                DbPhase = 2;
                return;
            }
            DbTargets[0] = vt[53];
            DbTargets[1] = vt[33];
            DbTargets[2] = vt[34];
            void *det[3] = {reinterpret_cast<void *>(&DbClear), reinterpret_cast<void *>(&DbOm), reinterpret_cast<void *>(&DbOmUav)};
            void **orig[3] = {reinterpret_cast<void **>(&DbOrig), reinterpret_cast<void **>(&DbOmOrig), reinterpret_cast<void **>(&DbOmUavOrig)};
            int hooked = 0;
            for (int i = 0; i < 3; ++i)
            {
                const MH_STATUS cs = MH_CreateHook(DbTargets[i], det[i], orig[i]);
                const MH_STATUS es = (cs == MH_OK) ? MH_EnableHook(DbTargets[i]) : cs;
                if (es == MH_OK)
                    ++hooked;
                else
                {
                    LogError("[db] hook %d (%p) failed: %d", i, DbTargets[i], (int)es);
                    if (cs == MH_OK)
                        MH_RemoveHook(DbTargets[i]);
                    DbTargets[i] = nullptr;
                }
            }
            if (!hooked)
            {
                DbPhase = 2;
                return;
            }
            DbStartMs = now;
            DbPresents = 0;
            DbPhase = 1;
            LogInfo("[db] watching depth clears and binds for 2 s (%d of 3 hooks, read only)", hooked);
            return;
        }
        if (now - DbStartMs < 2000)
            return;
        // the hooks off first, whatever happens next
        // Disabled, NOT removed: a call still inside a detour on another thread may yet jump to its
        // trampoline, and removing the hook would free it. The trampolines stay; the code is restored.
        for (void *t : DbTargets)
            if (t)
                MH_DisableHook(t);
        DbPhase = 2;

        UINT bbW = 0, bbH = 0;
        if (const auto *rd = API::get()->param()->renderer)
            if (auto *sc = static_cast<IDXGISwapChain *>(rd->swapchain))
            {
                DXGI_SWAP_CHAIN_DESC sd{};
                if (SUCCEEDED(sc->GetDesc(&sd)))
                {
                    bbW = sd.BufferDesc.Width;
                    bbH = sd.BufferDesc.Height;
                }
            }
        const int n = DbCount.load();
        LogInfo("[db] %llu presents, %u clears, %u render-target binds, %d distinct depth buffers; back buffer %ux%u",
                (unsigned long long)DbPresents, DbCalls.load(), DbBindCalls.load(), n, bbW, bbH);
        ComPtr<ID3D11Texture2D> texs[32];
        int read = 0;
        for (int i = 0; i < n; ++i)
        {
            ComPtr<ID3D11Resource> r;
            DbList[i].V->GetResource(r.GetAddressOf());
            D3D11_TEXTURE2D_DESC d{};
            if (r && SUCCEEDED(r.As(&texs[i])))
                texs[i]->GetDesc(&d);
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
            DbList[i].V->GetDesc(&vd);
            LogInfo("[db]  #%d %ux%u fmt %d (view %d) bind 0x%x%s samples %u arr %u mips %u misc 0x%x | clears %u flags %u depth %.3f | bound %u", i,
                    d.Width, d.Height, (int)d.Format, (int)vd.Format, d.BindFlags,
                    (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) ? " (SRV)" : "", d.SampleDesc.Count, d.ArraySize, d.MipLevels,
                    d.MiscFlags, DbList[i].N, DbList[i].Flags, DbList[i].Depth, DbList[i].Binds);
            // cameracapture-11: the scene depth is REVERSED Z - cleared to 0. -10 picked the largest buffer, the
            // 2048x2048 shadow map (cleared to 1). Every reversed-Z single-sample buffer is read now.
            if (texs[i] && d.SampleDesc.Count == 1 && d.ArraySize == 1 && DbList[i].N > 0 && DbList[i].Depth == 0.0f && read < 4)
            {
                ++read;
                DbReadPoints(dev, ctx, texs[i].Get(), i);
            }
        }
        for (int i = 0; i < n; ++i)
            if (DbList[i].V)
                DbList[i].V->Release();
        DbCount.store(0);
        LogInfo("[db] probe done - hooks disabled");
    }

    // Five points of one depth buffer, through a CPU copy: device Z, and for reversed infinite Z the near
    // plane each reading would need for the distance the user measured (Z * distance) - and 10 / Z cm.
    void DbReadPoints(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, int idx)
    {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        ComPtr<ID3D11Texture2D> stg;
        HRESULT hr = dev->CreateTexture2D(&sd, nullptr, stg.GetAddressOf());
        if (FAILED(hr))
        {
            LogError("[db]  #%d: a CPU copy could not be made (0x%08x) - format %d", idx, (unsigned)hr, (int)d.Format);
            return;
        }
        if (d.MipLevels == 1)
            ctx->CopyResource(stg.Get(), tex);
        else
            ctx->CopySubresourceRegion(stg.Get(), 0, 0, 0, 0, tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr = ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr))
        {
            LogError("[db]  #%d could not be mapped (0x%08x)", idx, (unsigned)hr);
            return;
        }
        const UINT px[5][2] = {{d.Width / 2, d.Height / 2}, {d.Width / 4, d.Height / 2}, {d.Width * 3 / 4, d.Height / 2},
                               {d.Width / 2, d.Height / 4}, {d.Width / 2, d.Height * 3 / 4}};
        static const char *where[5] = {"centre", "left", "right", "up", "down"};
        for (int k = 0; k < 5; ++k)
        {
            bool ok = false;
            const float z = DbRead(static_cast<const uint8_t *>(m.pData) + (size_t)px[k][1] * m.RowPitch, px[k][0], d.Format, ok);
            if (!ok)
            {
                LogInfo("[db]  #%d format %d not decoded", idx, (int)d.Format);
                break;
            }
            LogInfo("[db]  #%d %s (%u,%u): device Z %.8f -> %.0f cm at a 10 cm near plane", idx, where[k], px[k][0], px[k][1], z,
                    z > 1e-9f ? 10.0f / z : -1.0f);
        }
        ctx->Unmap(stg.Get(), 0);
    }

    // =====================================================================================
    // ENHANCED IMAGING (cameracapture-12, VHUD v21). V-POD's clone, built on the GAME's own depth:
    // the depth-buffer probe (-9..-11) found the scene depth - reversed infinite Z, 32-bit float,
    // sampleable, at the game's render resolution - so at 1x there is no second scene render.
    //   GAME THREAD (FollowEi, every frame while wanted): a units-only capture ring at the scene
    //     depth's size and the game camera's own field of view - the units image V-POD's outline
    //     shader uses to tell mechs from ground - with the pose, the lock and the lasers per frame.
    //   RENDER THREAD (EiPresent): the scene depth is found by its clear (ClearDepthStencilView,
    //     MinHooked while wanted: depth cleared to 0 = reversed Z), confirmed for a second by how
    //     often it is bound (the busiest one is the scene; the custom-depth buffer is bound once a
    //     frame and is empty), then each frame turned into cm (kEiNearCm / Z) by a small pixel
    //     shader into our own R32F texture and published beside the units capture of the SAME frame
    //     (the ring entry two frames back - the render thread trails the game thread by one) as the
    //     side-by-side pair V-POD's shader already reads. V-POD itself is untouched.
    // =====================================================================================
    static constexpr float kEiNearCm = 4.53f; // from the probe: Z 0.00007093 at the measured 637-640 m
    static constexpr int   kEiLag = 2;        // ring entries back from the newest = the frame being presented

    bool EiWanted(ULONGLONG now) const
    {
        const uint64_t w = Block ? Block->EiWantMs : 0;
        return Recent(w, now, kVhudTargetingStaleMs);
    }
    // cameracapture-23: the units ring is also wanted by the .pak post-process while the vision is up (it
    // needs no game depth, so EnhancedImaging.dll no longer asks for EiWantMs then).
    bool EiUnitsWanted(ULONGLONG now) const { return EiWanted(now) || (PakState == 1 && Block && Block->EiVisWant); }

    // ---- game thread ----
    API::UObject              *EiUCaps[kCaps] = {}, *EiURt[kCaps] = {};
    API::UObject              *EiActor = nullptr, *EiHud = nullptr;
    VanillaExportBlock::Camera EiPose[kCaps] = {};
    VanillaExportBlock::Lock   EiLockR[kCaps] = {};
    BeamSet                    EiBeamsR[kCaps];
    std::atomic<uint64_t>      EiCount{0};
    std::atomic<uint32_t>      EiGameW{0}, EiGameH{0}; // the scene depth's size, seen by the render thread
    uint32_t                   EiRingW = 0, EiRingH = 0;
    ULONGLONG                  EiFailMs = 0;
    // handed to the render thread under SrcLock
    // bug sweep 16 (2026-10-18): held with a reference, as Src[] / SrcUnits are (the file's own rule) - a render target
    // collected between the game thread listing it and the render thread copying it was a freed texture.
    ComPtr<ID3D11Texture2D>    SrcEiTex[kCaps];
    VanillaExportBlock::Camera SrcEiPose[kCaps] = {};
    VanillaExportBlock::Lock   SrcEiLock[kCaps] = {};
    BeamSet                    SrcEiBeams[kCaps];

    bool EiRingOn(const std::vector<API::UObject *> &caps) const
    {
        for (auto *c : EiUCaps)
            if (!c || std::find(caps.begin(), caps.end(), c) == caps.end())
                return false;
        return true;
    }

    bool MakeEi(API::UObject *pc, uint32_t w, uint32_t h)
    {
        if (!CaptureClass || !RenderLib)
            return false;
        API::UObject *made[kCaps] = {}, *rts[kCaps] = {};
        bool ok = true;
        for (int k = 0; k < kCaps && ok; ++k)
        {
            ok = MakeOneCapture(pc, (int32_t)w, (int32_t)h, made[k], rts[k], true);
            if (ok)
            {
                auto *p = FindProp(made[k]->get_class(), L"PrimitiveRenderMode");
                if (p)
                    *(reinterpret_cast<uint8_t *>(made[k]) + p->get_offset()) = 2; // PRM_UseShowOnlyList: units only
                else
                    ok = false;
            }
        }
        if (!ok)
        {
            for (auto *&c : made)
                if (c)
                    Call(c, L"K2_DestroyComponent", {{L"Object", &c, sizeof(void *)}});
            LogError("enhanced imaging: the units capture could not be made");
            return false;
        }
        for (int k = 0; k < kCaps; ++k)
        {
            EiUCaps[k] = made[k];
            EiURt[k]   = rts[k];
            EiPose[k]  = VanillaExportBlock::Camera{};
        }
        EiActor = pc;
        EiHud   = Hud;
        EiRingW = w;
        EiRingH = h;
        EiCount.store(0);
        NextUnitsMs = 0; // the units onto its show-only lists at once
        LogInfo("enhanced imaging: units capture running (ring of %d, %ux%u - the game's render resolution)", kCaps, w, h);
        return true;
    }

    void DropEi(bool alive)
    {
        {
            std::lock_guard<std::mutex> g(SrcLock);
            for (auto &t : SrcEiTex)
                t = nullptr;
            for (auto &rr : EiRingRes)
                rr.store(nullptr);
            EiDrawnK.store(-1);
        }
        for (int k = 0; k < kCaps; ++k)
        {
            if (alive && EiUCaps[k])
                Call(EiUCaps[k], L"K2_DestroyComponent", {{L"Object", &EiUCaps[k], sizeof(void *)}});
            EiUCaps[k] = nullptr;
            EiURt[k]   = nullptr;
        }
        const bool had = EiActor != nullptr;
        EiActor = EiHud = nullptr;
        EiCount.store(0);
        if (had)
            LogInfo("enhanced imaging: units capture stopped");
    }

    // 10 Hz from GameTick: make / drop the units ring.
    void TickEi(API::UObject *pc, ULONGLONG now)
    {
        const bool want = EiUnitsWanted(now);
        uint32_t gw = EiGameW.load(), gh = EiGameH.load();
        if (PakState == 1 && !EiWanted(now) && Block->GameW >= 64 && Block->GameH >= 64)
        {
            // .pak only (no game depth hunted): the engine samples the units image by the view's own UVs,
            // so any size with the screen's shape lines up - [hud] ei_units_scale of the back buffer.
            gw = (uint32_t)((float)Block->GameW * PakUnitsScale + 0.5f);
            gh = (uint32_t)((float)Block->GameH * PakUnitsScale + 0.5f);
        }
        if (EiUCaps[0])
        {
            std::vector<API::UObject *> caps;
            const bool sameWorld = pc == EiActor && EiHud == Hud;
            const bool alive = sameWorld && ComponentsOn(pc, CaptureClass, caps) && EiRingOn(caps);
            if (!want || !alive || (gw && (gw != EiRingW || gh != EiRingH)))
                DropEi(alive);
        }
        if (want && !EiUCaps[0] && gw >= 64 && gh >= 64 && now - EiFailMs > 5000)
            if (!MakeEi(pc, gw, gh))
                EiFailMs = now;
    }

    // Every frame from FollowCamera: this frame's units capture, from the game camera's own pose
    // and field of view, with the frame's lock and lasers.
    void FollowEi(API::UObject *pc, const float loc[3], const float rot[3], float fov)
    {
        if (!EiUCaps[0] || EiActor != pc || EiHud != Hud)
            return;
        const uint64_t c = EiCount.load();
        const int k = (int)(c % kCaps);
        API::UObject *cap = EiUCaps[k];
        bool yes = true;
        Call(cap, L"K2_SetWorldLocationAndRotation",
             {{L"NewLocation", loc, sizeof(float) * 3}, {L"NewRotation", rot, sizeof(float) * 3}, {L"bTeleport", &yes, 1}});
        if (CapFovOff >= 0)
            *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(cap) + CapFovOff) = fov;
        Call(cap, L"CaptureScene", {});
        PakUnitsRt = EiURt[k]; // cameracapture-23: the .pak samples this frame's units capture
        VanillaExportBlock::Camera &pose = EiPose[k];
        std::memcpy(pose.Loc, loc, sizeof(float) * 3);
        std::memcpy(pose.Rot, rot, sizeof(float) * 3);
        pose.FovDeg = fov;
        pose.Valid  = 1;
        pose.Seq    = (uint32_t)(c * 2u);
        ReadLock(EiLockR[k]);
        ReadBeams(pc, EiBeamsR[k]);
        {
            std::lock_guard<std::mutex> g(SrcLock);
            SrcEiTex[k]   = NativeOf(reinterpret_cast<UTextureRenderTarget2D *>(EiURt[k]));
            if (k < 4)
                EiRingRes[k].store(SrcEiTex[k].Get()); // cameracapture-20: what the render thread looks for
            SrcEiPose[k]  = pose;
            SrcEiLock[k]  = EiLockR[k];
            SrcEiBeams[k] = EiBeamsR[k];
        }
        EiCount.store(c + 1);
        // cameracapture-29: with the .pak there is no depth image to carry the lock bounds and beams to
        // EnhancedImaging.dll (the bracket fell back to the LIDAR scan position). Published here, with this
        // capture, inside the camera's seqlock so the reader gets pose + lock + beams as one set.
        if (PakState == 1 && Block)
        {
            auto &cm = Block->Cam;
            cm.Seq = cm.Seq | 1u;
            std::atomic_thread_fence(std::memory_order_release);
            Block->EiLock      = EiLockR[k];
            Block->EiBeamCount = EiBeamsR[k].Count;
            std::memcpy(Block->EiBeams, EiBeamsR[k].B, sizeof(EiBeamsR[k].B[0]) * EiBeamsR[k].Count);
            std::atomic_thread_fence(std::memory_order_release);
            cm.Seq = cm.Seq + 1;
        }
    }

    // ---- render thread: finding the scene depth ----
    using EiClearFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
    using EiOmFn    = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
    static inline EiClearFn EiClearOrig = nullptr;
    static inline EiOmFn    EiOmOrig = nullptr;
    static inline void     *EiClearTgt = nullptr, *EiOmTgt = nullptr;
    // cameracapture-16: the part of the scene depth the game actually draws into. UE's scene buffers only
    // ever GROW (a menu, a bigger view once) and the view then sits in their top-left corner - after
    // Escape the depth went 1488x836 -> 1488x1024 with the picture still 1488x836, so the whole buffer
    // read as the view squashed the world upwards and the units capture got the wrong shape. The game's
    // own viewports, set while the scene depth is bound, say where the picture is.
    using EiVpFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
    static inline EiVpFn EiVpOrig = nullptr;
    static inline void  *EiVpTgt = nullptr;
    static inline std::atomic<bool> EiSceneBound{false};
    // cameracapture-17: the hooks act only for the game's own context (EnhancedImaging.dll's device
    // goes through the same d3d11 functions), the viewport is only looked at in short samples, and
    // the table is never waited on - a busy lock just skips that sample.
    static inline std::atomic<ID3D11DeviceContext *> EiGameCtx{nullptr};
    static inline std::atomic<bool>                  EiVpSampling{false};
    // cameracapture-20: EXACT units/depth pairing. The -19 clip showed the units outline sometimes
    // repeating the previous frame and sometimes a frame ahead: "the entry kEiLag back from the newest"
    // drifts with the game thread / render thread timing. The render thread itself now says which
    // ring entry it drew: when the game context binds a ring texture as a render target, that entry's
    // capture is being rendered - and it is rendered just before the frame's own scene, so at present
    // the last one bound belongs to the frame being presented.
    static inline std::atomic<ID3D11Resource *> EiRingRes[4] = {};
    static inline std::atomic<int>              EiDrawnK{-1};
    static inline std::atomic<ULONGLONG>        EiDrawnMs{0};
    struct EiVpRec
    {
        uint32_t X, Y, W, H, N;
    };
    static inline EiVpRec          EiVps[8] = {};
    static inline int              EiVpN = 0;
    static inline std::atomic_flag EiVpLock = ATOMIC_FLAG_INIT;
    static void STDMETHODCALLTYPE EiRsVp(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v)
    {
        if (n && v && EiVpSampling.load(std::memory_order_relaxed) && c == EiGameCtx.load(std::memory_order_relaxed) &&
            EiSceneBound.load(std::memory_order_relaxed) && v[0].Width >= 1.0f && v[0].Height >= 1.0f &&
            v[0].TopLeftX >= 0.0f && v[0].TopLeftY >= 0.0f && !EiVpLock.test_and_set(std::memory_order_acquire))
        {
            const uint32_t x = (uint32_t)(v[0].TopLeftX + 0.5f), y = (uint32_t)(v[0].TopLeftY + 0.5f);
            const uint32_t w = (uint32_t)(v[0].Width + 0.5f), h = (uint32_t)(v[0].Height + 0.5f);
            int i = 0;
            for (; i < EiVpN; ++i)
                if (EiVps[i].X == x && EiVps[i].Y == y && EiVps[i].W == w && EiVps[i].H == h)
                    break;
            if (i < EiVpN)
                ++EiVps[i].N;
            else if (EiVpN < 8)
                EiVps[EiVpN++] = {x, y, w, h, 1};
            EiVpLock.clear(std::memory_order_release);
        }
        EiVpOrig(c, n, v);
    }
    static inline std::atomic<bool> EiCounting{false};
    struct EiCand
    {
        ID3D11DepthStencilView *V;
        uint32_t                Binds;
        uint32_t                Clears;
    };
    static inline EiCand           EiCands[16] = {};
    static inline std::atomic<int> EiCandN{0};
    static inline std::atomic<uint32_t> EiHeldClears{0};
    static inline std::atomic<ID3D11DepthStencilView *> EiHeldView{nullptr};

    static EiCand *EiCandOf(ID3D11DepthStencilView *v, bool add)
    {
        const int n = EiCandN.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (EiCands[i].V == v)
                return &EiCands[i];
        if (!add || n >= 16)
            return nullptr;
        v->AddRef();
        EiCands[n] = {v, 0, 0};
        EiCandN.store(n + 1, std::memory_order_release);
        return &EiCands[n];
    }

    static void STDMETHODCALLTYPE EiClear(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT f, FLOAT d, UINT8 st)
    {
        if (v && (f & D3D11_CLEAR_DEPTH) && d == 0.0f && c == EiGameCtx.load(std::memory_order_relaxed)) // reversed Z: the scene depth and its kin
        {
            if (v == EiHeldView.load(std::memory_order_relaxed))
                EiHeldClears.fetch_add(1, std::memory_order_relaxed);
            if (EiCounting.load(std::memory_order_relaxed))
                if (EiCand *e = EiCandOf(v, true))
                    ++e->Clears;
        }
        EiClearOrig(c, v, f, d, st);
    }
    static void STDMETHODCALLTYPE EiOm(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rt, ID3D11DepthStencilView *v)
    {
        if (c == EiGameCtx.load(std::memory_order_relaxed))
        {
            if (n && rt && rt[0] && EiRingRes[0].load(std::memory_order_relaxed))
            {
                ID3D11Resource *res = nullptr;
                rt[0]->GetResource(&res);
                if (res)
                {
                    for (int k = 0; k < 4; ++k)
                        if (res == EiRingRes[k].load(std::memory_order_relaxed))
                        {
                            EiDrawnK.store(k, std::memory_order_relaxed);
                            EiDrawnMs.store(GetTickCount64(), std::memory_order_relaxed);
                            break;
                        }
                    res->Release();
                }
            }
            if (v && EiCounting.load(std::memory_order_relaxed))
                if (EiCand *e = EiCandOf(v, false))
                    ++e->Binds;
            if (EiVpSampling.load(std::memory_order_relaxed))
                EiSceneBound.store(v && v == EiHeldView.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        EiOmOrig(c, n, rt, v);
    }

    bool      EiHooksMade = false, EiHooksOn = false, EiHooksFailed = false;
    int       EiFindPhase = 0; // 0 none held / searching, 1 counting, 2 holding
    ULONGLONG EiFindMs = 0, EiCheckMs = 0, EiLastWantMs = 0;
    ComPtr<ID3D11DepthStencilView> EiView;
    ComPtr<ID3D11Texture2D>        EiSceneTex;

    void EiHooks(ID3D11DeviceContext *ctx, bool on)
    {
        EiGameCtx.store(ctx);
        if (on == EiHooksOn || EiHooksFailed)
            return;
        if (on && !EiHooksMade)
        {
            void **vt = *reinterpret_cast<void ***>(ctx);
            const MH_STATUS init = MH_Initialize();
            EiClearTgt = vt[53];
            EiOmTgt = vt[33];
            EiVpTgt = vt[44]; // RSSetViewports
            if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
                MH_CreateHook(EiClearTgt, reinterpret_cast<void *>(&EiClear), reinterpret_cast<void **>(&EiClearOrig)) != MH_OK ||
                MH_CreateHook(EiOmTgt, reinterpret_cast<void *>(&EiOm), reinterpret_cast<void **>(&EiOmOrig)) != MH_OK ||
                MH_CreateHook(EiVpTgt, reinterpret_cast<void *>(&EiRsVp), reinterpret_cast<void **>(&EiVpOrig)) != MH_OK)
            {
                EiHooksFailed = true;
                LogError("enhanced imaging: the depth hooks could not be made - Enhanced Imaging has no game depth");
                return;
            }
            EiHooksMade = true;
        }
        // Disabled, never removed: a call still inside a detour may jump to its trampoline.
        if (on)
        {
            MH_EnableHook(EiClearTgt);
            MH_EnableHook(EiOmTgt);
            MH_EnableHook(EiVpTgt);
        }
        else
        {
            MH_DisableHook(EiClearTgt);
            MH_DisableHook(EiOmTgt);
            MH_DisableHook(EiVpTgt);
            EiSceneBound.store(false);
        }
        EiHooksOn = on;
    }

    // The view inside the scene depth: the viewport the game set most often while it was bound, over
    // the last half second. Until one is seen (2 s at most) nothing is published; after that the
    // whole buffer is used, as before.
    uint32_t  EiVX = 0, EiVY = 0, EiVW = 0, EiVH = 0;
    ULONGLONG EiVpMs = 0, EiHeldMs = 0;
    bool EiViewOf(D3D11_TEXTURE2D_DESC &d, ULONGLONG now)
    {
        if (!EiVpSampling.load() && (!EiVW || now - EiVpMs >= 3000))
        {
            EiVpMs = now; // a sample starts: the table is empty, the hooks fill it for 0.3 s
            EiVpSampling.store(true);
        }
        if (EiVpSampling.load() && now - EiVpMs >= 300 && !EiVpLock.test_and_set(std::memory_order_acquire))
        {
            EiVpSampling.store(false);
            EiSceneBound.store(false);
            EiVpMs = now;
            // cameracapture-18: the view has the SCREEN's shape. After an alt-tab the -17 log had the busiest
            // viewport at the whole grown buffer (1488x1024 - full-buffer passes), which squashed the world
            // again; only viewports with the back buffer's aspect (within 2 %) are taken now, and failing
            // those, the screen's shape fitted into the buffer's top-left corner.
            const double bbW = Block->GameW, bbH = Block->GameH;
            const double aspect = (bbW >= 64.0 && bbH >= 64.0) ? bbW / bbH : 0.0;
            auto shaped = [&](uint32_t w, uint32_t h) {
                return aspect <= 0.0 || std::fabs((double)w / (double)h / aspect - 1.0) <= 0.02;
            };
            // cameracapture-22: the MOST USED screen-shaped viewport. -19 took the largest, on the wrong idea
            // that 1485 was a stray pass: the game draws at ~58% of 2560x1440 = 1485x836 and pads its
            // buffer to 1488 - 1485 is the picture.
            int best = -1;
            for (int i = 0; i < EiVpN; ++i)
                if (EiVps[i].X + EiVps[i].W <= d.Width && EiVps[i].Y + EiVps[i].H <= d.Height && shaped(EiVps[i].W, EiVps[i].H) &&
                    EiVps[i].W * 4 >= d.Width && EiVps[i].H * 4 >= d.Height && (best < 0 || EiVps[i].N > EiVps[best].N))
                    best = i;
            EiVpRec b = best >= 0 ? EiVps[best] : EiVpRec{};
            if (best < 0 && aspect > 0.0 && !shaped(d.Width, d.Height))
            {
                b = {0, 0, d.Width, (uint32_t)((double)d.Width / aspect + 0.5), 1};
                if (b.H > d.Height)
                    b = {0, 0, (uint32_t)((double)d.Height * aspect + 0.5), d.Height, 1};
                best = 0;
            }
            EiVpN = 0;
            EiVpLock.clear(std::memory_order_release);
            if (best >= 0 && (b.X != EiVX || b.Y != EiVY || b.W != EiVW || b.H != EiVH))
            {
                EiVX = b.X;
                EiVY = b.Y;
                EiVW = b.W;
                EiVH = b.H;
                LogInfo("enhanced imaging: the game draws %ux%u at (%u,%u) of the %ux%u scene depth", EiVW, EiVH, EiVX, EiVY,
                        d.Width, d.Height);
            }
        }
        if (!EiVW)
        {
            if (now - EiHeldMs < 2000)
                return false;
            EiVX = EiVY = 0;
            EiVW = d.Width;
            EiVH = d.Height;
            LogInfo("enhanced imaging: no viewport seen on the scene depth - using all of it (%ux%u)", d.Width, d.Height);
        }
        d.Width = EiVW;
        d.Height = EiVH;
        return true;
    }

    // keepView: the scene buffers were remade under us - the picture area (and the units ring's size)
    // stay as a hint for the search, so nothing waits on them again when it is found.
    void EiForget(bool keepView = false)
    {
        if (!keepView)
        {
            EiVX = EiVY = EiVW = EiVH = 0;
            EiGameW.store(0);
            EiGameH.store(0);
        }
        EiHeldView.store(nullptr);
        EiView.Reset();
        EiSceneTex.Reset();
        EiSrv.Reset();
        EiFindPhase = 0;
        EiNewWindow(false); // drop the candidates' references (a replaced buffer is not kept alive)
    }

    // Keeps EiView on the scene depth: a 1 s count (clears to 0 + binds), the busiest wins; then held.
    // cameracapture-21: MW5 runs r.SceneRenderTargetResizeMethod 3 - the scene buffers are remade
    // whenever anything renders a scene larger (or, later, smaller) than they are, and the held depth
    // then stops being cleared. That used to cost up to 1 s to notice + 1 s to count + 0.3 s to find
    // the picture: the vision page lost its scene for 1-2.3 s (01:28:20 in the -20 log, right after a
    // missile lock). Now, while holding, every other reversed-Z buffer keeps being counted in 250 ms
    // windows; the held one is checked every 250 ms, and when it stops being cleared (with frames
    // still coming - a hitch is not a swap) the busiest buffer of the window that can hold the
    // picture (up to 1.5x its size - not the 2048 shadow map) is taken at once, picture area kept.
    // Nothing suitable: wait out the second as before, then the full search, picture kept as a hint.
    ULONGLONG EiLastClearMs = 0;
    uint32_t  EiWinPresents = 0, EiMissed = 0;

    int EiBest(ID3D11DepthStencilView *exclude, uint32_t minW, uint32_t minH, uint32_t maxW, uint32_t maxH)
    {
        const int n = EiCandN.load();
        int best = -1;
        uint64_t bestScore = 0;
        for (int i = 0; i < n; ++i)
        {
            if (!EiCands[i].V || EiCands[i].V == exclude || !EiCands[i].Clears)
                continue;
            ComPtr<ID3D11Resource> r;
            EiCands[i].V->GetResource(r.GetAddressOf());
            ComPtr<ID3D11Texture2D> t;
            D3D11_TEXTURE2D_DESC d{};
            if (!r || FAILED(r.As(&t)))
                continue;
            t->GetDesc(&d);
            if (d.SampleDesc.Count != 1 || d.ArraySize != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE))
                continue;
            if (minW && (d.Width < minW || d.Height < minH || d.Width > maxW || d.Height > maxH))
                continue;
            const uint64_t score = (uint64_t)EiCands[i].Binds * 1000000ull + (uint64_t)d.Width * d.Height / 1000ull;
            if (score > bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }

    // The candidate list starts again (its references dropped - an old, replaced buffer is not kept alive).
    void EiNewWindow(bool counting)
    {
        EiCounting.store(false);
        const int n = EiCandN.load();
        for (int i = 0; i < n; ++i)
            if (EiCands[i].V)
                EiCands[i].V->Release();
        EiCandN.store(0);
        EiWinPresents = 0;
        EiCounting.store(counting);
    }

    void EiHold(int best, ULONGLONG now, const char *how)
    {
        ID3D11DepthStencilView *v = EiCands[best].V;
        ComPtr<ID3D11Resource> r;
        v->GetResource(r.GetAddressOf());
        ComPtr<ID3D11Texture2D> t;
        D3D11_TEXTURE2D_DESC d{};
        if (!r || FAILED(r.As(&t)))
            return;
        t->GetDesc(&d);
        EiView = v; // AddRef'd by the ComPtr
        EiSceneTex = t;
        EiSrv.Reset(); // the shader view was on the old buffer
        EiHeldView.store(v);
        EiHeldClears.store(0);
        EiCheckMs = now;
        EiHeldMs = now;
        EiLastClearMs = now;
        EiMissed = 0;
        EiFindPhase = 2;
        const bool kept = EiVW && EiVX + EiVW <= d.Width && EiVY + EiVH <= d.Height;
        if (!kept)
        {
            EiVX = EiVY = EiVW = EiVH = 0; // EiGameW / H: set by EiPresent once the view inside it is known
            EiVpSampling.store(false);
            EiVpMs = 0;
        }
        LogInfo("enhanced imaging: %s %ux%u format %d, bound %u times / cleared %u (of %d reversed-Z buffers)%s", how, d.Width,
                d.Height, (int)d.Format, EiCands[best].Binds, EiCands[best].Clears, EiCandN.load(),
                kept ? " - picture area kept" : "");
    }

    void EiTrack(ULONGLONG now)
    {
        if (EiFindPhase == 2)
        {
            // cameracapture-22: checked EVERY frame - the game clears its scene depth once a frame, so
            // 3 frames in a row without that clear is a swap (~40 ms, was up to 250). A hitch shows no
            // frames at all, so it never counts. The candidate window still restarts every 250 ms.
            ++EiWinPresents;
            if (EiHeldClears.exchange(0) != 0)
            {
                EiMissed = 0;
                EiLastClearMs = now;
                if (now - EiCheckMs >= 250)
                {
                    EiCheckMs = now;
                    EiNewWindow(true);
                }
                return;
            }
            if (++EiMissed < 3)
                return;
            EiCounting.store(false);
            const uint32_t w = EiVW, h = EiVH;
            const int best = w ? EiBest(EiView.Get(), w, h, w * 3 / 2, h * 3 / 2) : -1;
            if (best >= 0)
            {
                EiHold(best, now, "the game remade its scene depth - followed at once to");
                EiMissed = 0;
                EiCheckMs = now;
                EiNewWindow(true);
                return;
            }
            if (now - EiLastClearMs < 1000)
            {
                EiCounting.store(true); // keep counting (the window is kept: the new buffer's count builds)
                return;
            }
            LogInfo("enhanced imaging: the scene depth stopped being cleared - looking for it again");
            EiForget(true);
            return;
        }
        if (EiFindPhase == 0)
        {
            EiNewWindow(true);
            EiFindPhase = 1;
            EiFindMs = now;
            return;
        }
        if (now - EiFindMs < 1000)
            return;
        EiCounting.store(false);
        const int best = EiBest(nullptr, 0, 0, 0, 0);
        if (best < 0)
        {
            EiFindPhase = 0; // nothing yet - count again
            return;
        }
        EiHold(best, now, "scene depth found -");
        EiNewWindow(true); // and keep counting the others, for the next swap
    }

    // ---- render thread: depth -> cm, published with the units capture ----
    ComPtr<ID3D11VertexShader>       EiVs;
    ComPtr<ID3D11PixelShader>        EiPs;
    ComPtr<ID3D11Buffer>             EiCb;
    ComPtr<ID3D11ShaderResourceView> EiSrv;
    ComPtr<ID3D11Texture2D>          EiLin;
    ComPtr<ID3D11ShaderResourceView> EiLinSrv; // cameracapture-14: the vision mode's wave reads it
    ComPtr<ID3D11RenderTargetView>   EiLinRtv;
    ComPtr<ID3D11RasterizerState>    EiRs;
    ComPtr<ID3D11DepthStencilState>  EiDss;
    ComPtr<ID3D11BlendState>         EiBs;
    bool EiPipeTried = false, EiPipeOk = false, EiLogged = false, EiExactLogged = false, EiPrewarmLogged = false;

    bool EiPipeline(ID3D11Device *dev)
    {
        if (EiPipeTried)
            return EiPipeOk;
        EiPipeTried = true;
        static const char kSrc[] =
            "cbuffer C : register(b0) { float4 P; };\n" // P.x near cm, P.y far cap cm
            "struct V { float4 p : SV_Position; };\n"
            "V vs(uint id : SV_VertexID) { float2 t = float2((id << 1) & 2, id & 2); V o;\n"
            "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
            "Texture2D<float> D : register(t0);\n"
            "float ps(V i) : SV_Target { float z = D.Load(int3(i.p.xy + P.zw, 0)); return z > 1e-12 ? min(P.x / z, P.y) : P.y; }\n";
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc ? reinterpret_cast<OvCompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        ComPtr<ID3DBlob> vsb, psb, err;
        if (!compile ||
            FAILED(compile(kSrc, sizeof(kSrc) - 1, "ei_depth", nullptr, nullptr, "vs", "vs_4_0", 0, 0, vsb.GetAddressOf(),
                           err.ReleaseAndGetAddressOf())) ||
            FAILED(compile(kSrc, sizeof(kSrc) - 1, "ei_depth", nullptr, nullptr, "ps", "ps_4_0", 0, 0, psb.GetAddressOf(),
                           err.ReleaseAndGetAddressOf())))
        {
            LogError("enhanced imaging: shader compile failed: %s", err ? static_cast<const char *>(err->GetBufferPointer()) : "no compiler");
            return false;
        }
        if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, EiVs.GetAddressOf())) ||
            FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, EiPs.GetAddressOf())))
            return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = FALSE;
        ds.StencilEnable = FALSE;
        D3D11_BLEND_DESC bl{};
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, EiCb.GetAddressOf())) || FAILED(dev->CreateRasterizerState(&rs, EiRs.GetAddressOf())) ||
            FAILED(dev->CreateDepthStencilState(&ds, EiDss.GetAddressOf())) || FAILED(dev->CreateBlendState(&bl, EiBs.GetAddressOf())))
            return false;
        EiPipeOk = true;
        return true;
    }

    // Our own linear-depth texture and the SRV on the game's, (re)made when the game's changes.
    bool EiTargets(ID3D11Device *dev)
    {
        D3D11_TEXTURE2D_DESC d{};
        EiSceneTex->GetDesc(&d);
        if (!EiSrv)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            switch (d.Format)
            {
            case DXGI_FORMAT_R32G8X24_TYPELESS: sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
            case DXGI_FORMAT_R24G8_TYPELESS:    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
            case DXGI_FORMAT_R32_TYPELESS:      sv.Format = DXGI_FORMAT_R32_FLOAT; break;
            default:
                LogError("enhanced imaging: scene depth format %d is not handled", (int)d.Format);
                return false;
            }
            if (FAILED(dev->CreateShaderResourceView(EiSceneTex.Get(), &sv, EiSrv.GetAddressOf())))
            {
                LogError("enhanced imaging: no shader view on the scene depth");
                return false;
            }
        }
        d.Width = EiVW;  // the view, not the whole (grown) buffer - EiViewOf ran first
        d.Height = EiVH;
        if (EiLin)
        {
            D3D11_TEXTURE2D_DESC ld{};
            EiLin->GetDesc(&ld);
            if (ld.Width != d.Width || ld.Height != d.Height)
            {
                EiLin.Reset();
                EiLinRtv.Reset();
                EiLinSrv.Reset();
            }
        }
        if (!EiLin)
        {
            D3D11_TEXTURE2D_DESC ld{};
            ld.Width = d.Width;
            ld.Height = d.Height;
            ld.MipLevels = 1;
            ld.ArraySize = 1;
            ld.Format = DXGI_FORMAT_R32_FLOAT;
            ld.SampleDesc.Count = 1;
            ld.Usage = D3D11_USAGE_DEFAULT;
            ld.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&ld, nullptr, EiLin.GetAddressOf())) ||
                FAILED(dev->CreateRenderTargetView(EiLin.Get(), nullptr, EiLinRtv.GetAddressOf())) ||
                FAILED(dev->CreateShaderResourceView(EiLin.Get(), nullptr, EiLinSrv.GetAddressOf())))
                return false;
        }
        return true;
    }

    // One fullscreen triangle, every piece of state it touches saved first and put back after.
    void EiConvert(ID3D11DeviceContext *ctx, UINT w, UINT h)
    {
        D3D11_PRIMITIVE_TOPOLOGY topo;
        ctx->IAGetPrimitiveTopology(&topo);
        ComPtr<ID3D11InputLayout> il;
        ctx->IAGetInputLayout(il.GetAddressOf());
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11HullShader> hs;
        ComPtr<ID3D11DomainShader> dsh;
        ID3D11ClassInstance *vsI[256], *psI[256], *gsI[256];
        UINT vsN = 256, psN = 256, gsN = 256;
        ctx->VSGetShader(vs.GetAddressOf(), vsI, &vsN);
        ctx->PSGetShader(ps.GetAddressOf(), psI, &psN);
        ctx->GSGetShader(gs.GetAddressOf(), gsI, &gsN);
        ctx->HSGetShader(hs.GetAddressOf(), nullptr, nullptr);
        ctx->DSGetShader(dsh.GetAddressOf(), nullptr, nullptr);
        ComPtr<ID3D11Buffer> psCb;
        ctx->PSGetConstantBuffers(0, 1, psCb.GetAddressOf());
        ComPtr<ID3D11ShaderResourceView> psSrv;
        ctx->PSGetShaderResources(0, 1, psSrv.GetAddressOf());
        ComPtr<ID3D11RasterizerState> rs;
        ctx->RSGetState(rs.GetAddressOf());
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ctx->RSGetViewports(&nvp, vps);
        ComPtr<ID3D11BlendState> bs;
        float bf[4];
        UINT sm = 0;
        ctx->OMGetBlendState(bs.GetAddressOf(), bf, &sm);
        ComPtr<ID3D11DepthStencilState> dss;
        UINT sref = 0;
        ctx->OMGetDepthStencilState(dss.GetAddressOf(), &sref);
        ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ComPtr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv.GetAddressOf());
        ComPtr<ID3D11Buffer> vb, ib;
        UINT vbStride = 0, vbOff = 0, ibOff = 0;
        DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
        ctx->IAGetVertexBuffers(0, 1, vb.GetAddressOf(), &vbStride, &vbOff);
        ctx->IAGetIndexBuffer(ib.GetAddressOf(), &ibFmt, &ibOff);

        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(EiCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            float *c = static_cast<float *>(m.pData);
            c[0] = kEiNearCm;
            c[1] = 2.0e7f; // 200 km: the sky
            c[2] = (float)EiVX; // the view's corner inside the scene depth
            c[3] = (float)EiVY;
            ctx->Unmap(EiCb.Get(), 0);
            D3D11_VIEWPORT vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
            ID3D11RenderTargetView *target = EiLinRtv.Get();
            ctx->OMSetRenderTargets(1, &target, nullptr);
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(EiRs.Get());
            ctx->OMSetBlendState(EiBs.Get(), nullptr, 0xFFFFFFFFu);
            ctx->OMSetDepthStencilState(EiDss.Get(), 0);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer *noVb = nullptr;
            UINT zero = 0;
            ctx->IASetVertexBuffers(0, 1, &noVb, &zero, &zero);
            ctx->VSSetShader(EiVs.Get(), nullptr, 0);
            ctx->PSSetShader(EiPs.Get(), nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer *cb = EiCb.Get();
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView *srv = EiSrv.Get();
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->Draw(3, 0);
        }

        ID3D11ShaderResourceView *noSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &noSrv);
        ctx->IASetPrimitiveTopology(topo);
        ctx->IASetInputLayout(il.Get());
        ID3D11Buffer *vbp = vb.Get();
        ctx->IASetVertexBuffers(0, 1, &vbp, &vbStride, &vbOff);
        ctx->IASetIndexBuffer(ib.Get(), ibFmt, ibOff);
        ctx->VSSetShader(vs.Get(), vsI, vsN);
        ctx->PSSetShader(ps.Get(), psI, psN);
        ctx->GSSetShader(gs.Get(), gsI, gsN);
        ctx->HSSetShader(hs.Get(), nullptr, 0);
        ctx->DSSetShader(dsh.Get(), nullptr, 0);
        for (UINT i = 0; i < vsN; ++i) if (vsI[i]) vsI[i]->Release();
        for (UINT i = 0; i < psN; ++i) if (psI[i]) psI[i]->Release();
        for (UINT i = 0; i < gsN; ++i) if (gsI[i]) gsI[i]->Release();
        ID3D11Buffer *pcb = psCb.Get();
        ctx->PSSetConstantBuffers(0, 1, &pcb);
        ID3D11ShaderResourceView *ps0 = psSrv.Get();
        ctx->PSSetShaderResources(0, 1, &ps0);
        ctx->RSSetState(rs.Get());
        ctx->RSSetViewports(nvp, vps);
        ctx->OMSetBlendState(bs.Get(), bf, sm);
        ctx->OMSetDepthStencilState(dss.Get(), sref);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv.Get());
        for (auto *r : rtvs)
            if (r)
                r->Release();
    }

    void EiPresent(ID3D11Device *dev, ID3D11DeviceContext *ctx, ULONGLONG now, bool live)
    {
        auto &info = Block->Slots[VS_EiDepth];
        const bool want = EiWanted(now);
        if (want)
            EiLastWantMs = now;
        // cameracapture-24: PRE-WARM. The first activation used to start the search for the scene depth
        // (~1 s count + 0.3 s picture area) only once the vision was on, so the wave had no depth to ride:
        // the image flipped on at once and the outlines came ~1.5 s later. Now, while piloting (the cockpit
        // ticked within 30 s - a pause does not churn the hooks) and the .pak is not drawing the scene, the
        // depth is found and held (swaps followed, picture area kept) all along - tracking only: nothing is
        // converted or published until the vision (or E-IMG) wants it. Cost: the three hooks' pointer checks.
        const bool prewarm = !want && PakState != 1 && Recent(DrvSeenMs.load(), now, 30000);
        // the hooks stay on a few seconds past the last want, so flicking pages does not churn them
        EiHooks(ctx, want || prewarm || (EiLastWantMs && now - EiLastWantMs < 5000));
        if (!want || !live || EiHooksFailed)
        {
            info.Active = 0;
            info.SharedHandle = 0;
            if (prewarm && live && !EiHooksFailed)
            {
                EiTrack(now);
                D3D11_TEXTURE2D_DESC pd{};
                if (EiFindPhase == 2 && EiSceneTex)
                {
                    EiSceneTex->GetDesc(&pd);
                    if (EiViewOf(pd, now) && (EiGameW.load() != pd.Width || EiGameH.load() != pd.Height))
                    {
                        EiGameW.store(pd.Width);
                        EiGameH.store(pd.Height);
                    }
                }
                if (!EiPrewarmLogged && EiFindPhase == 2)
                {
                    EiPrewarmLogged = true;
                    LogInfo("enhanced imaging: scene depth found ahead of the vision (pre-warm) - the first activation waves at once");
                }
                return;
            }
            if (!want && !prewarm && EiFindPhase != 0 && EiLastWantMs && now - EiLastWantMs >= 5000)
                EiForget();
            return;
        }
        EiTrack(now);
        D3D11_TEXTURE2D_DESC d{};
        if (EiFindPhase == 2 && EiSceneTex)
            EiSceneTex->GetDesc(&d);
        if (EiFindPhase != 2 || !EiSceneTex || !EiViewOf(d, now) || !EiPipeline(dev) || !EiTargets(dev))
        {
            info.Active = 0;
            info.SharedHandle = 0;
            return;
        }
        if (EiGameW.load() != d.Width || EiGameH.load() != d.Height)
        {
            EiGameW.store(d.Width); // the units capture follows (TickEi remakes the ring at this size)
            EiGameH.store(d.Height);
        }
        EiConvert(ctx, d.Width, d.Height);

        // the units capture, pose, lock and lasers of the frame being presented
        ComPtr<ID3D11Texture2D>    units;
        VanillaExportBlock::Camera pose{};
        VanillaExportBlock::Lock   lock{};
        BeamSet                    beams;
        const uint64_t c = EiCount.load();
        // the entry the render thread actually drew this frame; the old guess only if that is not seen
        const int drawn = EiDrawnK.load();
        const bool exact = drawn >= 0 && drawn < kCaps && Recent(EiDrawnMs.load(), now, 500);
        if (exact != EiExactLogged)
        {
            EiExactLogged = exact;
            LogInfo(exact ? "enhanced imaging: units paired exactly (the render thread's own capture of the frame)"
                          : "enhanced imaging: units paired by frame count (kEiLag) - the capture's draw is not seen");
        }
        if (exact || c >= (uint64_t)kEiLag)
        {
            const int k = exact ? drawn : (int)((c - kEiLag) % kCaps);
            std::lock_guard<std::mutex> g(SrcLock);
            units = SrcEiTex[k];
            pose  = SrcEiPose[k];
            lock  = SrcEiLock[k];
            beams = SrcEiBeams[k];
        }
        if (!pose.Valid)
        {
            info.Active = 0;
            info.SharedHandle = 0;
            return;
        }
        D3D11_TEXTURE2D_DESC pair{};
        EiLin->GetDesc(&pair);
        pair.Width = d.Width * 2;
        if (!EnsureShared(dev, VS_EiDepth, pair, DXGI_FORMAT_UNKNOWN))
        {
            info.Active = 0;
            info.SharedHandle = 0;
            return;
        }
        bool unitsOk = false;
        if (units)
        {
            D3D11_TEXTURE2D_DESC ud{};
            units->GetDesc(&ud);
            unitsOk = ud.Width == d.Width && ud.Height == d.Height && ud.Format == DXGI_FORMAT_R32_FLOAT && ud.SampleDesc.Count == 1;
        }
        info.Width        = SharedW[VS_EiDepth];
        info.Height       = SharedH[VS_EiDepth];
        info.SharedHandle = reinterpret_cast<uint64_t>(SharedHandle[VS_EiDepth]);
        if (!info.Active)
            info.Active = 1;
        if (!EiLogged)
        {
            EiLogged = true;
            LogInfo("enhanced imaging: publishing %ux%u (game depth | units%s), near plane %.2f cm", info.Width, info.Height,
                    unitsOk ? "" : " - not yet", kEiNearCm);
        }
        ComPtr<IDXGIKeyedMutex> km;
        if (FAILED(Shared[VS_EiDepth].As(&km)) || km->AcquireSync(0, 0) != S_OK)
            return;
        ctx->CopySubresourceRegion(Shared[VS_EiDepth].Get(), 0, 0, 0, 0, EiLin.Get(), 0, nullptr);
        if (unitsOk)
            ctx->CopySubresourceRegion(Shared[VS_EiDepth].Get(), 0, d.Width, 0, 0, units.Get(), 0, nullptr);
        pose.UnitsValid = unitsOk ? 1.0f : 0.0f;
        Block->EiCam       = pose;
        Block->EiLock      = lock;
        Block->EiNearCm    = kEiNearCm;
        Block->EiBeamCount = beams.Count;
        std::memcpy(Block->EiBeams, beams.B, sizeof(beams.B[0]) * beams.Count);
        km->ReleaseSync(1);
    }

    // =====================================================================================
    // ENHANCED IMAGING VISION MODE (cameracapture-13, VHUD v22). HUDDisplay renders Enhanced Imaging at
    // the game's size into a shared image; while Block->EiVisionOn is set and the image is fresh, it is
    // drawn here over the whole back buffer, opaque - in place of the view. Unlike the TacOps overlay
    // nothing about input changes: the game keeps the mouse, keys, stick and throttle.
    // =====================================================================================
    ComPtr<ID3D11Texture2D>          EvShared, EvPriv;
    ComPtr<ID3D11ShaderResourceView> EvSrv;
    uint64_t                         EvHandleOpen = 0;
    bool                             EvLogged = false, EvWasOn = false;
    ULONGLONG                        EvDiagMs = 0; // cameracapture-15: why the game showed through
    uint32_t                         EvSkipLive = 0, EvSkipStale = 0, EvSkipOther = 0, EvLate = 0, EvDrawn = 0;
    ULONGLONG                        EvSizeMs = 0;

    void EvRelease()
    {
        EvSrv.Reset();
        EvPriv.Reset();
        EvShared.Reset();
        EvHandleOpen = 0;
    }

    // The game's back buffer size, for HUDDisplay to render at (once a second).
    void EvPublishSize(const UEVR_RendererData *rd, ULONGLONG now)
    {
        if (now - EvSizeMs < 1000 || !rd->swapchain)
            return;
        EvSizeMs = now;
        DXGI_SWAP_CHAIN_DESC sd{};
        if (SUCCEEDED(static_cast<IDXGISwapChain *>(rd->swapchain)->GetDesc(&sd)))
        {
            Block->GameW = sd.BufferDesc.Width;
            Block->GameH = sd.BufferDesc.Height;
        }
    }

    // ---- THE WAVE (cameracapture-14) --------------------------------------------------------------
    // Switched on, Enhanced Imaging sweeps OUT through the world: every pixel nearer than the wave front
    // (the game's own depth, EiLin) shows it - the cockpit first, then the ground ahead, the hills, the
    // horizon. Switched off, the world comes back from the horizon inward. The front is a glowing contour
    // that hugs whatever it crosses, with a slight ripple. The first kilometre takes 85% of the time and
    // the rest snaps in (to 250 km), so the whole thing lasts [hud] ei_wave_s (default 1 s).
    int    EvPhase = 0;      // 0 off, 1 on - waiting for the first image, 2 opening, 3 on, 4 closing
    double EvT0 = 0.0, EvU0 = 0.0, EvWaveS = 1.0, EvU = 0.0;
    float  EvFadeCm = 5000.0f; // cameracapture-18: [hud] ei_wave_band_m - a band of fixed depth trailing the front
    ULONGLONG EvCfgMs = 0;
    ComPtr<ID3D11PixelShader> EvPs;
    ComPtr<ID3D11SamplerState> EvPointSamp;
    bool EvPsTried = false;

    static double EvNowMs()
    {
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
    }

    // u 0..1 -> the front, cm: 1 km at u = 0.85 (quadratic, so the cockpit and the ground go first),
    // then exponentially to 250 km at u = 1.
    static float EvFront(double u)
    {
        if (u <= 0.0)
            return 0.0f;
        if (u <= 0.85)
            return (float)(100000.0 * (u / 0.85) * (u / 0.85));
        return (float)(100000.0 * std::pow(250.0, (u - 0.85) / 0.15));
    }

    bool EvWavePipeline(ID3D11Device *dev)
    {
        if (EvPsTried)
            return EvPs != nullptr;
        EvPsTried = true;
        static const char kSrc[] =
            "cbuffer C : register(b0) { float4 Rect; float4 P; };\n" // P.x opacity, P.y front cm, P.z band cm, P.w fade cm (0 = no wave)
            "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
            "Texture2D T : register(t0); Texture2D<float> D : register(t1);\n"
            "SamplerState S : register(s0); SamplerState SP : register(s1);\n"
            "float4 ps(V i) : SV_Target {\n"
            "  if (P.w < -0.5) { float4 o = T.Sample(S, i.uv); return float4(o.a > 0.0001 ? o.rgb / o.a : 0.0, o.a * P.x); }\n" // .pak overlay
            "  if (P.w < 0.5) return float4(T.Sample(S, i.uv).rgb, P.x);\n"
            "  float L = D.SampleLevel(SP, i.uv, 0);\n"
            "  float d = L - P.y;\n"
            "  float edge = saturate(1.0 - abs(d) / P.z);\n"
            "  float2 uv = i.uv + edge * 0.006 * float2(sin(i.uv.y * 140.0 + P.y * 0.0007), cos(i.uv.x * 140.0 + P.y * 0.0007));\n"
            "  float inside = saturate(-d / P.w);\n" // 0 at the front, full P.w behind it
            "  float3 c = T.Sample(S, uv).rgb * inside + float3(0.33, 1.0, 0.54) * edge * edge;\n"
            "  return float4(c, saturate(inside + edge * 0.9) * P.x);\n"
            "}\n";
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc ? reinterpret_cast<OvCompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        ComPtr<ID3DBlob> psb, err;
        if (!compile || FAILED(compile(kSrc, sizeof(kSrc) - 1, "ei_wave", nullptr, nullptr, "ps", "ps_4_0", 0, 0,
                                       psb.GetAddressOf(), err.GetAddressOf())) ||
            FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, EvPs.GetAddressOf())))
        {
            LogError("enhanced imaging wave: shader failed: %s", err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
            EvPs.Reset();
            return false;
        }
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; // a depth between two depths is a depth that does not exist
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, EvPointSamp.GetAddressOf());
        return true;
    }

    // The switch, the wave's phase, and whether the renderers should keep going (EiVisWant).
    bool        EvReady = false;
    const char *EvMissing = "";
    void EvStep(bool fresh)
    {
        const ULONGLONG tick = GetTickCount64();
        if (tick - EvCfgMs >= 1000)
        {
            EvCfgMs = tick;
            const double s = gsp::Num(L"hud", L"ei_wave_s", 1.0);
            EvWaveS = (s >= 0.1 && s <= 5.0) ? s : 1.0;
            const double f = gsp::Num(L"hud", L"ei_wave_band_m", 50.0); // the fade band behind the front, metres
            EvFadeCm = (float)((f >= 1.0 && f <= 1000.0) ? f : 50.0) * 100.0f;
        }
        const bool on = Block->EiVisionOn != 0;
        const double now = EvNowMs(), T = EvWaveS * 1000.0;
        if (on && (EvPhase == 0 || EvPhase == 4))
        {
            EvU0 = (EvPhase == 4) ? EvU : 0.0; // turned back on mid-collapse: carry on outward from there
            EvPhase = (EvPhase == 4) ? 2 : 1;
            EvT0 = now;
        }
        else if (!on && (EvPhase == 1 || EvPhase == 2 || EvPhase == 3))
        {
            if (EvPhase == 1)
                EvPhase = 0;
            else
            {
                EvU0 = (EvPhase == 2) ? EvU : 1.0;
                EvPhase = 4;
                EvT0 = now;
            }
        }
        // cameracapture-24: the wave starts once everything it shows is there - the overlay image (fresh),
        // and the depth + units it rides on (EvReady) - or after 2 s whatever is missing, so a failure can
        // never keep the vision from coming up.
        if (EvPhase == 1 && fresh && (EvReady || now - EvT0 >= 2000.0))
        {
            if (!EvReady)
                LogInfo("enhanced imaging vision mode: started without %s after 2 s", EvMissing);
            EvPhase = 2;
            EvT0 = now;
            EvU0 = 0.0;
        }
        if (EvPhase == 2)
        {
            EvU = EvU0 + (now - EvT0) / T;
            if (EvU >= 1.0)
            {
                EvU = 1.0;
                EvPhase = 3;
            }
        }
        else if (EvPhase == 4)
        {
            EvU = EvU0 - (now - EvT0) / T;
            if (EvU <= 0.0)
            {
                EvU = 0.0;
                EvPhase = 0;
            }
        }
        else if (EvPhase == 3)
            EvU = 1.0;
        Block->EiVisWant = EvPhase != 0 ? 1u : 0u;
    }

    void EvDraw(ID3D11Device *dev, ID3D11DeviceContext *ctx, const UEVR_RendererData *rd, bool live)
    {
        const ULONGLONG now = GetTickCount64();
        EvPublishSize(rd, now);
        const bool on = Block->EiVisionOn != 0;
        if (on != EvWasOn)
        {
            EvWasOn = on;
            LogInfo("enhanced imaging vision mode %s", on ? "ON" : "off");
        }
        const uint64_t handle = Block->EiVisHandle, stamp = Block->EiVisFrameMs;
        const bool fresh = handle && Recent(stamp, now, 500 + 1);
        // cameracapture-15: a renderer hiccup (a late image, a skipped one) no longer drops the view for
        // a frame - the last image is drawn again for up to 1.5 s. Only a renderer that has really
        // gone (or the game not ticking: a menu, a load) shows the game.
        const bool recent = handle && Recent(stamp, now, 1500 + 1);
        if (Block->EiPakOn) // the .pak: the engine has the depth; the units capture of a frame is what is waited for
        {
            EvReady = PakUnitsRt && EiUCaps[0];
            EvMissing = "the units capture";
        }
        else
        {
            const bool depth = Block->Slots[VS_EiDepth].Active && EiLinSrv;
            EvReady = depth && Block->EiCam.UnitsValid > 0.5f;
            EvMissing = depth ? "the units capture" : "the game depth";
        }
        EvStep(fresh);
        EvFrontCm.store(EvFront(EvU)); // cameracapture-23: the .pak's wave front, read by the game thread
        const bool waving = EvPhase == 2 || EvPhase == 4;
        if (EvPhase >= 2)
        {
            // why the game showed through, counted and logged every 10 s - the "single frame" hunt
            if (!live) ++EvSkipLive;
            else if (!recent) ++EvSkipStale;
            else if (!fresh) ++EvLate;
            if (now - EvDiagMs >= 10000)
            {
                if (EvSkipLive || EvSkipStale || EvLate || EvSkipOther)
                    LogInfo("vision mode: game shown %u (game not ticking) / %u (no image 1.5 s) / %u (draw failed), late image redrawn %u, drawn %u",
                            EvSkipLive, EvSkipStale, EvSkipOther, EvLate, EvDrawn);
                EvDiagMs = now;
                EvSkipLive = EvSkipStale = EvLate = EvSkipOther = EvDrawn = 0;
            }
        }
        if (EvPhase < 2 || !live || !recent || !rd->swapchain)
        {
            if (EvPhase == 0 && EvShared)
                EvRelease();
            return;
        }
        if (!OvPipeline(dev))
        {
            ++EvSkipOther;
            return;
        }
        if (handle != EvHandleOpen || !EvShared)
        {
            EvRelease();
            ComPtr<ID3D11Texture2D> t;
            if (FAILED(dev->OpenSharedResource(reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void **>(t.GetAddressOf()))))
                return;
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            D3D11_TEXTURE2D_DESC p = d;
            p.MiscFlags = 0;
            p.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            p.Usage = D3D11_USAGE_DEFAULT;
            p.CPUAccessFlags = 0;
            p.MipLevels = 1;
            if (FAILED(dev->CreateTexture2D(&p, nullptr, EvPriv.GetAddressOf())) ||
                FAILED(dev->CreateShaderResourceView(EvPriv.Get(), nullptr, EvSrv.GetAddressOf())))
            {
                EvRelease();
                return;
            }
            EvShared = t;
            EvHandleOpen = handle;
            LogInfo("enhanced imaging vision mode: image opened, %ux%u", d.Width, d.Height);
        }
        ComPtr<IDXGIKeyedMutex> km;
        if (SUCCEEDED(EvShared.As(&km)) && km->AcquireSync(1, 0) == S_OK)
        {
            ctx->CopyResource(EvPriv.Get(), EvShared.Get());
            km->ReleaseSync(0);
        }
        auto *sc = static_cast<IDXGISwapChain *>(rd->swapchain);
        ComPtr<ID3D11Texture2D> bb;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(bb.GetAddressOf()))))
        {
            ++EvSkipOther;
            return;
        }
        D3D11_TEXTURE2D_DESC bd{};
        bb->GetDesc(&bd);
        D3D11_RENDER_TARGET_VIEW_DESC rvd{};
        rvd.Format = Typed(bd.Format);
        rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(dev->CreateRenderTargetView(bb.Get(), &rvd, rtv.GetAddressOf())))
            return;

        // Save everything the draw touches.
        D3D11_PRIMITIVE_TOPOLOGY topo;
        ctx->IAGetPrimitiveTopology(&topo);
        ComPtr<ID3D11InputLayout> il;
        ctx->IAGetInputLayout(il.GetAddressOf());
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11HullShader> hs;
        ComPtr<ID3D11DomainShader> dsh;
        ID3D11ClassInstance *vsI[256], *psI[256], *gsI[256];
        UINT vsN = 256, psN = 256, gsN = 256;
        ctx->VSGetShader(vs.GetAddressOf(), vsI, &vsN);
        ctx->PSGetShader(ps.GetAddressOf(), psI, &psN);
        ctx->GSGetShader(gs.GetAddressOf(), gsI, &gsN);
        ctx->HSGetShader(hs.GetAddressOf(), nullptr, nullptr);
        ctx->DSGetShader(dsh.GetAddressOf(), nullptr, nullptr);
        ComPtr<ID3D11Buffer> vsCb, psCb;
        ctx->VSGetConstantBuffers(0, 1, vsCb.GetAddressOf());
        ctx->PSGetConstantBuffers(0, 1, psCb.GetAddressOf());
        ComPtr<ID3D11ShaderResourceView> psSrv;
        ctx->PSGetShaderResources(0, 1, psSrv.GetAddressOf());
        ComPtr<ID3D11SamplerState> psSamp, psSamp1;
        ctx->PSGetSamplers(0, 1, psSamp.GetAddressOf());
        ctx->PSGetSamplers(1, 1, psSamp1.GetAddressOf());
        ComPtr<ID3D11ShaderResourceView> psSrv1;
        ctx->PSGetShaderResources(1, 1, psSrv1.GetAddressOf());
        ComPtr<ID3D11RasterizerState> rs;
        ctx->RSGetState(rs.GetAddressOf());
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ctx->RSGetViewports(&nvp, vps);
        ComPtr<ID3D11BlendState> bs;
        float bf[4];
        UINT sm = 0;
        ctx->OMGetBlendState(bs.GetAddressOf(), bf, &sm);
        ComPtr<ID3D11DepthStencilState> dss;
        UINT sref = 0;
        ctx->OMGetDepthStencilState(dss.GetAddressOf(), &sref);
        ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ComPtr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv.GetAddressOf());
        ComPtr<ID3D11Buffer> vb, ib;
        UINT vbStride = 0, vbOff = 0, ibOff = 0;
        DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
        ctx->IAGetVertexBuffers(0, 1, vb.GetAddressOf(), &vbStride, &vbOff);
        ctx->IAGetIndexBuffer(ib.GetAddressOf(), &ibFmt, &ibOff);

        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(OvCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            float *c = static_cast<float *>(m.pData);
            c[0] = -1.0f; c[1] = 1.0f; c[2] = 1.0f; c[3] = -1.0f; // the whole back buffer
            c[4] = 1.0f;                                           // opaque
            // the wave: its front and band (cm); off when it has finished, or there is no game depth to ride
            const float front = EvFront(EvU);
            // cameracapture-23: with the .pak the engine draws the scene and the wave; this image is only
            // EnhancedImaging.dll's overlay (grid, labels...), transparent, faded in with the wave.
            const bool pak = Block->EiPakOn != 0;
            const bool wave = !pak && waving && EiLinSrv && EvWavePipeline(dev);
            const bool overlay = pak && EvWavePipeline(dev);
            if (overlay)
                c[4] = (float)EvU;
            c[5] = front;
            c[6] = (std::max)(front * 0.08f, 400.0f);
            // the gradient: the vision fades in from the front back over this distance (cm) - wave on
            c[7] = wave ? EvFadeCm : (overlay ? -1.0f : 0.0f);
            ctx->Unmap(OvCb.Get(), 0);
            ++EvDrawn;
            D3D11_VIEWPORT vp{0.0f, 0.0f, (float)bd.Width, (float)bd.Height, 0.0f, 1.0f};
            ID3D11RenderTargetView *target = rtv.Get();
            ctx->OMSetRenderTargets(1, &target, nullptr);
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(OvRs.Get());
            ctx->OMSetBlendState(OvBlend.Get(), nullptr, 0xFFFFFFFFu);
            ctx->OMSetDepthStencilState(OvDss.Get(), 0);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ID3D11Buffer *noVb = nullptr;
            UINT zero = 0;
            ctx->IASetVertexBuffers(0, 1, &noVb, &zero, &zero);
            ctx->VSSetShader(OvVs.Get(), nullptr, 0);
            ctx->PSSetShader((wave || overlay) ? EvPs.Get() : OvPs.Get(), nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer *cb = OvCb.Get();
            ctx->VSSetConstantBuffers(0, 1, &cb);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView *srvs[2] = {EvSrv.Get(), wave ? EiLinSrv.Get() : nullptr};
            ctx->PSSetShaderResources(0, 2, srvs);
            ID3D11SamplerState *smps[2] = {OvSamp.Get(), EvPointSamp.Get()};
            ctx->PSSetSamplers(0, 2, smps);
            ctx->Draw(4, 0);
            if (!EvLogged)
            {
                EvLogged = true;
                LogInfo("enhanced imaging vision mode: drawn over the %ux%u back buffer", bd.Width, bd.Height);
            }
        }

        ID3D11ShaderResourceView *noSrv[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(0, 2, noSrv);
        ID3D11ShaderResourceView *ps1 = psSrv1.Get();
        ctx->PSSetShaderResources(1, 1, &ps1);
        ID3D11SamplerState *s1 = psSamp1.Get();
        ctx->PSSetSamplers(1, 1, &s1);
        ctx->IASetPrimitiveTopology(topo);
        ctx->IASetInputLayout(il.Get());
        ID3D11Buffer *vbp = vb.Get();
        ctx->IASetVertexBuffers(0, 1, &vbp, &vbStride, &vbOff);
        ctx->IASetIndexBuffer(ib.Get(), ibFmt, ibOff);
        ctx->VSSetShader(vs.Get(), vsI, vsN);
        ctx->PSSetShader(ps.Get(), psI, psN);
        ctx->GSSetShader(gs.Get(), gsI, gsN);
        ctx->HSSetShader(hs.Get(), nullptr, 0);
        ctx->DSSetShader(dsh.Get(), nullptr, 0);
        for (UINT i = 0; i < vsN; ++i) if (vsI[i]) vsI[i]->Release();
        for (UINT i = 0; i < psN; ++i) if (psI[i]) psI[i]->Release();
        for (UINT i = 0; i < gsN; ++i) if (gsI[i]) gsI[i]->Release();
        ID3D11Buffer *vcb = vsCb.Get(), *pcb = psCb.Get();
        ctx->VSSetConstantBuffers(0, 1, &vcb);
        ctx->PSSetConstantBuffers(0, 1, &pcb);
        ID3D11ShaderResourceView *ps0 = psSrv.Get();
        ctx->PSSetShaderResources(0, 1, &ps0);
        ID3D11SamplerState *s0 = psSamp.Get();
        ctx->PSSetSamplers(0, 1, &s0);
        ctx->RSSetState(rs.Get());
        ctx->RSSetViewports(nvp, vps);
        ctx->OMSetBlendState(bs.Get(), bf, sm);
        ctx->OMSetDepthStencilState(dss.Get(), sref);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv.Get());
        for (auto *r : rtvs)
            if (r)
                r->Release();
    }

    bool    LockWarned = false;
    bool    CapFullRes = false; // cameracapture-14: the depth capture ring was made at the full viewport

    // =====================================================================================
    // THE MISSILE LOCK (cameracapture-5, VHUD v20). The player's own LockOnComponent, on the
    // mech's WeaponSystemComponent - the same object LanceControl's insane allies reads - every
    // frame: LockOnState (ELockOnState: None / Acquiring / Losing / Locked as the game stores
    // them; 3 = locked, the value insane allies already relies on), CurrentLockPercent, and
    // whether PendingLockTarget is the current target. HUDDisplay closes a square onto the target
    // bracket as the percent climbs, turns the bracket solid when locked, and plays the tone.
    // The raw values are logged the first time each state is seen, so the enum and the percent's
    // range are on record. The target's bounds only while V-POD or CAP is up (a reflected call).
    // =====================================================================================
    void   *MlCls     = nullptr;
    int32_t MlStateOff = -1, MlPctOff = -1;
    uint32_t MlSeen    = 0;  // bit per state value logged
    uint32_t MlLast    = 0;
    int32_t  MlAcqOff  = -1, MlTotOff = -1;
    int      MlDumps   = 0;
    float   MlPctMax   = 0.0f;

    void PublishMissileLock(API::UObject *pawn, bool wantBounds)
    {
        if (!Block)
            return;
        uint32_t state = 0, onTgt = 0;
        float    pct   = 0.0f;
        VanillaExportBlock::Lock bounds{};
        API::UObject *ws = pawn ? ObjProp(pawn, L"WeaponSystemComponent") : nullptr;
        API::UObject *lc = ws ? ObjProp(ws, L"LockOnComponent") : nullptr;
        if (lc && lc->get_class())
        {
            if (static_cast<void *>(lc->get_class()) != MlCls)
            {
                MlCls = lc->get_class();
                API::FProperty *sp = FindProp(lc->get_class(), L"LockOnState");
                API::FProperty *pp = FindProp(lc->get_class(), L"CurrentLockPercent");
                MlStateOff = sp ? sp->get_offset() : -1;
                MlPctOff   = (pp && pp->get_class() && pp->get_class()->get_name() == L"FloatProperty") ? pp->get_offset() : -1;
                // cameracapture-8: the component has no percent, but it has the timer and its goal.
                API::FProperty *ap = FindProp(lc->get_class(), L"AcquireTimer");
                API::FProperty *tp = FindProp(lc->get_class(), L"TotalTimeToAcquireLock");
                MlAcqOff = (ap && PropType(ap) == L"FloatProperty") ? ap->get_offset() : -1;
                MlTotOff = (tp && PropType(tp) == L"FloatProperty") ? tp->get_offset() : -1;
                LogInfo("missile lock: AcquireTimer %ls, TotalTimeToAcquireLock %ls", MlAcqOff >= 0 ? L"found" : L"MISSING",
                        MlTotOff >= 0 ? L"found" : L"MISSING");
                LogInfo("missile lock: LockOnState %ls, CurrentLockPercent %ls", sp ? L"found" : L"MISSING",
                        MlPctOff >= 0 ? L"found" : (pp ? L"not a float" : L"MISSING"));
            }
            const uint8_t *base = reinterpret_cast<const uint8_t *>(lc);
            if (MlStateOff >= 0)
                state = base[MlStateOff];
            float raw = 0.0f;
            if (MlPctOff >= 0)
                std::memcpy(&raw, base + MlPctOff, sizeof(raw));
            else if (MlAcqOff >= 0 && MlTotOff >= 0)
            {
                float acq = 0.0f, tot = 0.0f;
                std::memcpy(&acq, base + MlAcqOff, sizeof(acq));
                std::memcpy(&tot, base + MlTotOff, sizeof(tot));
                raw = (tot > 0.01f) ? acq / tot : 0.0f; // 0..1
            }
            if (!(raw == raw) || raw < 0.0f) // NaN or negative: nothing
                raw = 0.0f;
            if (raw > MlPctMax)
                MlPctMax = raw;
            pct = raw > 1.5f ? raw * 0.01f : raw; // 0..100 or 0..1, whichever the game keeps
            pct = pct > 1.0f ? 1.0f : pct;
            API::UObject *pending = ObjProp(lc, L"PendingLockTarget");
            API::UObject *tt      = ObjProp(pawn, L"TargetTracking");
            API::UObject *cur     = tt ? ObjProp(tt, L"CurrentTarget") : nullptr;
            onTgt = (cur && (!pending || pending == cur)) ? 1u : 0u;
            if (state != MlLast && MlDumps < 4)
            {
                // cameracapture-6: CurrentLockPercent is not a property of the component (it is only
                // a parameter of its LockOnProgressSig delegate), so every float it does hold is
                // logged at each state change - the lock timer is one of them.
                ++MlDumps;
                std::wstring fl;
                for (auto *st = static_cast<API::UStruct *>(lc->get_class()); st; st = st->get_super_struct())
                {
                    if (st->get_fname() && st->get_fname()->to_string() == L"ActorComponent")
                        break;
                    for (auto *fp = st->get_child_properties(); fp; fp = fp->get_next())
                    {
                        auto *p = reinterpret_cast<API::FProperty *>(fp);
                        if (!p->get_class() || p->get_class()->get_name() != L"FloatProperty")
                            continue;
                        float v = 0.0f;
                        std::memcpy(&v, base + p->get_offset(), sizeof(v));
                        wchar_t b[96];
                        _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls=%.3f", fp->get_fname()->to_string().c_str(), v);
                        fl += b;
                    }
                }
                LogInfo("missile lock: state %u -> %u, floats:%ls", MlLast, state, fl.empty() ? L" (none)" : fl.c_str());
            }
            MlLast = state;
            if (state < 32 && !(MlSeen & (1u << state)))
            {
                MlSeen |= 1u << state;
                LogInfo("missile lock: state %u first seen (percent raw %.3f, highest so far %.3f, on current target %u)",
                        state, raw, MlPctMax, onTgt);
            }
        }
        if (wantBounds)
            ReadLock(bounds);
        auto &m = Block->MissileLock;
        m.Seq = m.Seq | 1u;
        std::atomic_thread_fence(std::memory_order_release);
        m.State    = state;
        m.Percent  = pct;
        m.OnTarget = onTgt;
        m.Bounds   = bounds;
        std::atomic_thread_fence(std::memory_order_release);
        m.Seq = m.Seq + 1;
    }

    void ReadLock(VanillaExportBlock::Lock &out)
    {
        out = VanillaExportBlock::Lock{};
        auto *pawn = API::get()->get_local_pawn(0);
        if (!pawn)
            return;
        auto *tt = ObjProp(pawn, L"TargetTracking");
        if (!tt)
            return;
        auto *p = FindProp(tt->get_class(), L"CurrentTarget");
        if (!p || !p->get_class() || p->get_class()->get_name() != L"ObjectProperty")
        {
            if (!LockWarned)
            {
                LockWarned = true;
                LogWarn("TargetTracking.CurrentTarget is %ls, not an object property - no live lock",
                        (p && p->get_class()) ? p->get_class()->get_name().c_str() : L"missing");
            }
            return;
        }
        auto *tgt = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(tt) + p->get_offset());
        if (!tgt)
            return;
        // The COLLIDING components first: a mech's hit boxes follow its hull. All components
        // take in whatever else rides on the actor - weapon effects, jump jet plumes, the
        // cockpit - which is what made the bracket sometimes far bigger than the mech.
        float o[3], e[3];
        const bool hull = ActorBounds(tgt, true, o, e) && e[0] > 50.0f && e[1] > 50.0f && e[2] > 50.0f;
        if (!hull && !ActorBounds(tgt, false, o, e))
            return;
        out.Valid = 1;
        std::memcpy(out.Origin, o, sizeof(o));
        std::memcpy(out.Extent, e, sizeof(e));
    }

    // AActor::GetActorBounds - out-params are plain vectors, so the frame may be zeroed.
    bool ActorBounds(API::UObject *actor, bool colliding, float origin[3], float extent[3])
    {
        API::UFunction *fn = FindFn(actor, L"GetActorBounds");
        if (!fn)
            return false;
        const int32_t oo = ParamOffset(fn, L"Origin"), eo = ParamOffset(fn, L"BoxExtent");
        const int32_t co = ParamOffset(fn, L"bOnlyCollidingComponents");
        const int32_t frame = fn->get_properties_size();
        if (oo < 0 || eo < 0 || co < 0 || frame <= 0 || frame > (int32_t)sizeof(CallBuf) - 16)
            return false;
        std::memset(CallBuf, 0, (size_t)frame + 16);
        CallBuf[co] = colliding ? 1 : 0;
        const int32_t ch = ParamOffset(fn, L"bIncludeFromChildActors");
        if (ch >= 0)
            CallBuf[ch] = 0;
        actor->process_event(fn, CallBuf);
        std::memcpy(origin, CallBuf + oo, 3 * sizeof(float));
        std::memcpy(extent, CallBuf + eo, 3 * sizeof(float));
        return extent[0] > 1.0f || extent[1] > 1.0f || extent[2] > 1.0f;
    }

    bool MakeCapture(API::UObject *pc)
    {
        if (!CaptureClass || !RenderLib || !LayoutLib)
            return false;
        float vp[2] = {};
        if (!CallRet(LayoutLib, L"GetViewportSize", {{L"WorldContextObject", &pc, sizeof(void *)}}, vp, sizeof(vp)) ||
            vp[0] < 64.0f || vp[1] < 64.0f)
            return false;
        // cameracapture-6: HALF the resolution (was a quarter) - the user asked for twice the detail.
        // cameracapture-14: FULL when Enhanced Imaging asks (zoomed) - V-POD keeps half.
        CapFullRes = Block && Block->TargetingFullRes != 0;
        const float rs = CapFullRes ? 1.0f : 0.5f;
        int32_t w = (int32_t)(vp[0] * rs), h = (int32_t)(vp[1] * rs);
        API::UObject *made[kCaps] = {}, *rts[kCaps] = {};
        for (int k = 0; k < kCaps; ++k)
            if (!MakeOneCapture(pc, w, h, made[k], rts[k], true))
            {
                for (int j = 0; j < k; ++j)
                    Call(made[j], L"K2_DestroyComponent", {{L"Object", &made[j], sizeof(void *)}});
                return false;
            }
        for (int k = 0; k < kCaps; ++k)
        {
            Caps[k]  = made[k];
            CapRt[k] = rts[k];
            CapPose[k] = VanillaExportBlock::Camera{};
        }
        MakeUnitsRing(pc, w, h);
        CapCount     = 0;
        Capture      = Caps[0];
        CaptureActor = pc;
        CaptureHud   = Hud;
        NextHideMs   = 0;
        HiddenActors.clear();
        LogInfo("depth capture running (ring of %d, %dx%d, scene depth, one capture per frame from the game camera)",
                kCaps, w, h);
        return true;
    }

    // The units ring. PrimitiveRenderMode = PRM_UseShowOnlyList (2): only the components on its
    // show-only list are drawn, and that list is the units (RefreshUnits).
    void MakeUnitsRing(API::UObject *pc, int32_t w, int32_t h)
    {
        API::UObject *made[kCaps] = {}, *rts[kCaps] = {};
        bool ok = true;
        for (int k = 0; k < kCaps && ok; ++k)
        {
            ok = MakeOneCapture(pc, w, h, made[k], rts[k], true);
            if (ok)
            {
                auto *p = FindProp(made[k]->get_class(), L"PrimitiveRenderMode");
                if (p)
                    *(reinterpret_cast<uint8_t *>(made[k]) + p->get_offset()) = 2; // PRM_UseShowOnlyList
                else
                {
                    LogError("units capture: PrimitiveRenderMode not found - outlines stay unhighlighted");
                    ok = false;
                    Call(made[k], L"K2_DestroyComponent", {{L"Object", &made[k], sizeof(void *)}});
                    made[k] = nullptr;
                }
            }
        }
        if (!ok)
        {
            for (auto *&c : made)
                if (c)
                    Call(c, L"K2_DestroyComponent", {{L"Object", &c, sizeof(void *)}});
            return;
        }
        for (int k = 0; k < kCaps; ++k)
        {
            UCaps[k] = made[k];
            URt[k]   = rts[k];
        }
        NextUnitsMs   = 0;
        LastUnitCount = (size_t)-1;
        LogInfo("units capture running (ring of %d, show-only list of units)", kCaps);
    }

    // Once a second: every unit in the world - pawns other than our own - onto the units ring's
    // show-only lists. The engine keeps that list as weak references, so a unit that dies in
    // between simply drops off it.
    // GameplayStatics::GetAllActorsOfClass, its frame kept in one buffer whose out-array is
    // never zeroed (the engine reuses it: the leak rule).
    bool ActorsOf(API::UObject *pc, API::UClass *cls, std::vector<API::UObject *> &out)
    {
        out.clear();
        if (!pc || !cls)
            return false;
        if (!GaacFn)
        {
            if (GaacFailed)
                return false;
            if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameplayStatics"))
                GameplayStatics = c->get_class_default_object();
            GaacFn = GameplayStatics ? FindFn(GameplayStatics, L"GetAllActorsOfClass") : nullptr;
            if (GaacFn)
            {
                GaacWorld = ParamOffset(GaacFn, L"WorldContextObject");
                GaacClass = ParamOffset(GaacFn, L"ActorClass");
                GaacOut   = ParamOffset(GaacFn, L"OutActors");
            }
            const int32_t frame = GaacFn ? GaacFn->get_properties_size() : 0;
            if (!GaacFn || GaacWorld < 0 || GaacClass < 0 || GaacOut < 0 || frame <= 0)
            {
                LogError("GetAllActorsOfClass has an unexpected frame - units and beams not listed");
                GaacFn     = nullptr;
                GaacFailed = true;
                return false;
            }
            GaacBuf.assign((size_t)frame + 16, 0);
        }
        std::memcpy(GaacBuf.data() + GaacWorld, &pc, sizeof(void *));
        std::memcpy(GaacBuf.data() + GaacClass, &cls, sizeof(void *));
        GameplayStatics->process_event(GaacFn, GaacBuf.data());
        struct Arr
        {
            API::UObject **Data;
            int32_t        Num, Max;
        } arr{};
        std::memcpy(&arr, GaacBuf.data() + GaacOut, sizeof(arr));
        for (int32_t i = 0; arr.Data && i < arr.Num && i < 512; ++i)
            if (arr.Data[i])
                out.push_back(arr.Data[i]);
        return true;
    }

    bool GaacFailed = false;

    // LASERS. A shot is an MWRay actor (Ray_C79SmallLaser_C < Ray_C < MWRay): bActivated while it
    // fires, HitLocation where it lands (Ray_C), EffectsLocationComponent at the muzzle. Beams
    // are translucent and never reach a depth image, so they are read here every frame and
    // sent with the frame's depth image.
    BeamSet      CapBeams[4];

    BeamSet      SrcBeams; // guarded by SrcLock, goes with Src[VS_Depth]

    API::UClass *RayClass = nullptr;

    bool         RayTried = false;

    struct RayLayout
    {
        const void         *Cls;
        int32_t             Hit;
        API::FBoolProperty *Active;
        // cameracapture-32 (2026-10-13, user): a shot that hits nothing has HitLocation (0,0,0) and was dropped - it
        // is now drawn along the muzzle's forward to the ray's range; machine guns are drawn as moving tracers.
        int32_t             RangeOff = -1; // a float property named *Range* on the ray, if any
        bool                Mg = false;
        bool                Flamer = false; // a flickering spray, not one solid beam
        bool                MissLogged = false, DirLogged = false;
    };

    std::vector<RayLayout> RayLayouts;

    size_t                 LastBeamCount = 0;

    const RayLayout &RayLayoutOf(API::UObject *r)
    {
        const void *cls = r->get_class();
        for (auto &l : RayLayouts)
            if (l.Cls == cls)
                return l;
        RayLayout l{cls, -1, nullptr};
        {
            const std::wstring cn = ClassName(r);
            l.Mg = cn.find(L"MachineGun") != std::wstring::npos || cn.find(L"Machinegun") != std::wstring::npos ||
                   cn.find(L"_MG") != std::wstring::npos || cn.find(L"Gatling") != std::wstring::npos;
            // every float property of the ray (once per class): where its range lives, for the miss case
            std::wstring fl;
            for (auto *st = static_cast<API::UStruct *>(r->get_class()); st; st = st->get_super_struct())
            {
                if (st->get_fname() && st->get_fname()->to_string() == L"Actor")
                    break;
                for (auto *f = st->get_child_properties(); f; f = f->get_next())
                {
                    auto *fp = reinterpret_cast<API::FProperty *>(f);
                    if (!fp->get_class() || fp->get_class()->get_name() != L"FloatProperty")
                        continue;
                    const std::wstring nm = f->get_fname()->to_string();
                    const float v = *reinterpret_cast<const float *>(reinterpret_cast<const uint8_t *>(r) + fp->get_offset());
                    if (fl.size() < 600)
                        fl += L" " + nm + L"=" + std::to_wstring((int)v);
                    if (l.RangeOff < 0 && (nm.find(L"Range") != std::wstring::npos || nm.find(L"Length") != std::wstring::npos))
                        l.RangeOff = fp->get_offset();
                }
            }
            l.Flamer = cn.find(L"Flamer") != std::wstring::npos;
            LogInfo("beams: %ls%s floats:%ls", cn.c_str(), l.Mg ? " (machine gun - tracers)" : l.Flamer ? " (flamer - spray)" : "",
                    fl.c_str());
        }
        if (auto *p = FindProp(r->get_class(), L"HitLocation"))
            if (p->get_class() && p->get_class()->get_name() == L"StructProperty")
                l.Hit = p->get_offset();
        if (auto *p = FindProp(r->get_class(), L"bActivated"))
            if (p->get_class() && p->get_class()->get_name() == L"BoolProperty")
                l.Active = reinterpret_cast<API::FBoolProperty *>(p);
        LogInfo("beams: %ls - HitLocation +%d, bActivated %s", ClassName(r).c_str(), l.Hit, l.Active ? "found" : "MISSING");
        RayLayouts.push_back(l);
        return RayLayouts.back();
    }

    void ReadBeams(API::UObject *pc, BeamSet &out)
    {
        out.Count = 0;
        if (!RayClass && !RayTried)
        {
            RayTried = true;
            RayClass = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWRay");
            LogInfo("beams: MWRay class %s", RayClass ? "found" : "not found yet (learned from the first laser seen)");
        }
        if (!RayClass)
            return;
        std::vector<API::UObject *> rays;
        if (!ActorsOf(pc, RayClass, rays))
            return;
        for (auto *r : rays)
        {
            if (out.Count >= (uint32_t)VanillaExportBlock::kMaxBeams)
            {
                if (!BeamCapLogged)
                {
                    BeamCapLogged = true;
                    LogWarn("beams: more than %d lasers firing in one frame - the rest dropped for that frame",
                            VanillaExportBlock::kMaxBeams);
                }
                break;
            }
            const RayLayout &l = RayLayoutOf(r);
            if (l.Hit < 0 || !l.Active || !l.Active->get_value_from_object(r))
                continue;
            float to[3];
            std::memcpy(to, reinterpret_cast<uint8_t *>(r) + l.Hit, sizeof(to));
            float from[3] = {};
            API::UObject *fx = ObjProp(r, L"EffectsLocationComponent");
            if (!(fx && CallRet(fx, L"K2_GetComponentLocation", {}, from, sizeof(from))))
                CallRet(r, L"K2_GetActorLocation", {}, from, sizeof(from));
            float fwd[3] = {};
            const bool haveFwd = (fx && CallRet(fx, L"GetForwardVector", {}, fwd, sizeof(fwd))) ||
                                 CallRet(r, L"GetActorForwardVector", {}, fwd, sizeof(fwd));
            if (to[0] == 0.0f && to[1] == 0.0f && to[2] == 0.0f)
            {
                // cameracapture-32: it hit nothing - out to its range along the muzzle's forward
                if (!haveFwd || (fwd[0] == 0.0f && fwd[1] == 0.0f && fwd[2] == 0.0f))
                    continue;
                float range = 100000.0f; // 1000 m when the ray carries no range of its own
                if (l.RangeOff >= 0)
                {
                    const float v = *reinterpret_cast<const float *>(reinterpret_cast<const uint8_t *>(r) + l.RangeOff);
                    if (v > 1.0f)
                        range = v; // cameracapture-34: always cm (game ranges are cm - the under-20000-is-metres rule broke short weapons)
                }
                for (int k = 0; k < 3; ++k)
                    to[k] = from[k] + fwd[k] * range;
                if (!l.MissLogged)
                {
                    const_cast<RayLayout &>(l).MissLogged = true;
                    LogInfo("beams: %ls hit nothing - drawn %.0f m along the muzzle's forward", ClassName(r).c_str(), range * 0.01f);
                }
            }
            else if (haveFwd && !l.DirLogged)
            {
                // once per class: does the muzzle's forward agree with where hits land? (checks the miss direction)
                const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
                const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dl > 100.0f)
                {
                    const_cast<RayLayout &>(l).DirLogged = true;
                    const float c = (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / dl;
                    LogInfo("beams: %ls - muzzle forward vs the hit: %.1f deg apart (%.0f m shot)", ClassName(r).c_str(),
                            std::acos((std::max)(-1.0f, (std::min)(1.0f, c))) * 57.29578f, dl * 0.01f);
                }
            }
            if (l.Flamer)
            {
                // flamers: five short lines fanning out from the muzzle along the flame's path, re-drawn every frame with
                // new lengths and angles so it shimmers (each ends no further than the game's hit point)
                const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len < 1.0f)
                    continue;
                const float u[3] = {d[0] / len, d[1] / len, d[2] / len};
                float rt[3] = {u[1], -u[0], 0.0f}; // u x up
                float rl = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1]);
                if (rl < 1e-3f)
                {
                    rt[0] = 1.0f;
                    rt[1] = 0.0f;
                    rl = 1.0f;
                }
                rt[0] /= rl;
                rt[1] /= rl;
                const float up[3] = {rt[1] * u[2], -rt[0] * u[2], rt[0] * u[1] - rt[1] * u[0]}; // rt x u
                static uint32_t seed = 0x2545F491u;
                auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; }; // 0..1
                for (int i = 0; i < 5 && out.Count < (uint32_t)VanillaExportBlock::kMaxBeams; ++i)
                {
                    const float spread = 0.06f; // about 3.5 degrees each way
                    const float a = (rnd() * 2.0f - 1.0f) * spread, e = (rnd() * 2.0f - 1.0f) * spread;
                    float v[3];
                    for (int k = 0; k < 3; ++k)
                        v[k] = u[k] + rt[k] * a + up[k] * e;
                    const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    const float start = len * 0.05f * rnd(), stop = len * (0.55f + 0.45f * rnd());
                    auto &b = out.B[out.Count++];
                    for (int k = 0; k < 3; ++k)
                    {
                        b.From[k] = from[k] + v[k] / vl * start;
                        b.To[k] = from[k] + v[k] / vl * stop;
                    }
                }
                continue;
            }
            if (l.Mg)
            {
                // machine guns: short tracers moving down the line, not one solid beam
                const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len < 1.0f)
                    continue;
                const float u[3] = {d[0] / len, d[1] / len, d[2] / len};
                const float seg = 800.0f, spacing = 5000.0f, speed = 60000.0f; // 8 m tracers, 50 m apart, 600 m/s
                const float phase = std::fmod((float)(GetTickCount64() % 1000000ull) * 0.001f * speed, spacing);
                int made = 0;
                for (float a = phase; a + seg <= len && made < 16 && out.Count < (uint32_t)VanillaExportBlock::kMaxBeams;
                     a += spacing, ++made)
                {
                    auto &b = out.B[out.Count++];
                    for (int k = 0; k < 3; ++k)
                    {
                        b.From[k] = from[k] + u[k] * a;
                        b.To[k] = from[k] + u[k] * (a + seg);
                    }
                }
                continue;
            }
            auto &b = out.B[out.Count++];
            std::memcpy(b.From, from, sizeof(from));
            std::memcpy(b.To, to, sizeof(to));
        }
        if ((out.Count > 0) != (LastBeamCount > 0))
        {
            LastBeamCount = out.Count;
            if (out.Count)
                LogDistinctBeam(out.Count);
        }
    }

    bool BeamLogged = false;

    bool BeamCapLogged = false;

    void LogDistinctBeam(uint32_t n)
    {
        if (BeamLogged)
            return;
        BeamLogged = true;
        LogInfo("beams: first live beam(s) read (%u) - lasers now go to V-POD", n);
    }

    ULONGLONG                    UnitSkipMs = 0;
    std::vector<std::wstring>    UnitSkip;
    std::unordered_set<std::wstring> UnitClassesSeen;
    void RefreshUnits(API::UObject *pc, ULONGLONG now)
    {
        if ((!UCaps[0] && !ZUCap && !EiUCaps[0]) || now < NextUnitsMs)
            return;
        NextUnitsMs = now + 1000;
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> all;
        if (!PawnClass || !ActorsOf(pc, PawnClass, all))
            return;
        auto *own = API::get()->get_local_pawn(0);
        std::vector<API::UObject *> units;
        // cameracapture-22: every Pawn was a "unit" - the -21 log had 61 during an artillery barrage
        // (about 10 otherwise), each rendered again by every units capture. Pawn classes whose name holds
        // a [hud] ei_units_skip word (comma separated; default below) are left out, and each pawn class
        // is logged once, shown or skipped, so the list can be tuned from the log.
        if (now - UnitSkipMs >= 5000)
        {
            UnitSkipMs = now;
            UnitSkip.clear();
            const std::wstring list = gsp::Str(L"hud", L"ei_units_skip", L"Projectile,Artillery,Airstrike,Strike,Shell,Bomb,Debris,Marker");
            size_t a0 = 0;
            while (a0 <= list.size())
            {
                size_t a1 = list.find(L',', a0);
                if (a1 == std::wstring::npos)
                    a1 = list.size();
                std::wstring w = list.substr(a0, a1 - a0);
                while (!w.empty() && w.front() == L' ')
                    w.erase(w.begin());
                while (!w.empty() && w.back() == L' ')
                    w.pop_back();
                if (!w.empty())
                    UnitSkip.push_back(w);
                a0 = a1 + 1;
            }
        }
        size_t skipped = 0;
        for (auto *a : all)
        {
            if (a == own || !a->get_class())
                continue;
            const std::wstring cls = a->get_class()->get_fname()->to_string();
            bool skip = false;
            for (const auto &w : UnitSkip)
                if (cls.find(w) != std::wstring::npos)
                {
                    skip = true;
                    break;
                }
            if (UnitClassesSeen.insert(cls).second)
                LogInfo("units capture: pawn class %ls - %s", cls.c_str(), skip ? "SKIPPED ([hud] ei_units_skip)" : "shown");
            if (skip)
                ++skipped;
            else
                units.push_back(a);
        }

        if (SoChildOff == -2)
        {
            API::UFunction *fn = FindFn(UCaps[0] ? UCaps[0] : (ZUCap ? ZUCap : EiUCaps[0]), L"ShowOnlyActorComponents");
            SoChildOff = fn ? ParamOffset(fn, L"bIncludeFromChildActors") : -1;
        }
        const bool yes = true;
        std::vector<API::UObject *> listed(std::begin(UCaps), std::end(UCaps));
        listed.push_back(ZUCap);
        listed.insert(listed.end(), std::begin(EiUCaps), std::end(EiUCaps)); // cameracapture-12
        for (auto *cap : listed)
        {
            if (!cap)
                continue;
            Call(cap, L"ClearShowOnlyComponents", {});
            for (auto *a : units)
                if (SoChildOff >= 0)
                    Call(cap, L"ShowOnlyActorComponents", {{L"InActor", &a, sizeof(void *)}, {L"bIncludeFromChildActors", &yes, 1}});
                else
                    Call(cap, L"ShowOnlyActorComponents", {{L"InActor", &a, sizeof(void *)}});
        }
        if (units.size() != LastUnitCount)
        {
            LastUnitCount = units.size();
            LogInfo("units capture shows %zu units (%zu pawns skipped)", units.size(), skipped);
        }
    }

    // One capture of the ring: its own render target (which its TextureTarget keeps alive),
    // depth only, rendered only when asked.
    // LEAN depth captures. A scene capture renders the whole frame - shadows, lighting, fog,
    // particles, post-processing - even when only its depth is kept, and that was most of
    // V-POD's cost. Its show flags can only be set before the component registers, so the
    // component is added DEFERRED, the list is written, and then it is finished. The list is
    // a TArray<FEngineShowFlagsSetting> built with the ENGINE's allocator (UEVR's FMalloc), so the
    // engine frees it normally when the component goes.
    static constexpr const wchar_t *kLeanOff[] = {
        L"DynamicShadows", L"Lighting", L"PostProcessing", L"Fog", L"AtmosphericFog", L"Atmosphere",
        L"VolumetricFog", L"Particles", L"Translucency", L"AmbientOcclusion", L"DistanceFieldAO",
        L"ScreenSpaceReflections", L"ScreenSpaceAO", L"Bloom", L"MotionBlur", L"SkyLighting", L"Decals",
        L"LightShafts", L"ContactShadows", L"CapsuleShadows", L"GlobalIllumination", L"ReflectionEnvironment",
        L"EyeAdaptation", L"LensFlares", L"DepthOfField", L"AntiAliasing", L"TemporalAA"};

    bool LeanLogged = false;

    int ApplyLeanFlags(API::UObject *comp)
    {
        auto *prop = FindProp(comp->get_class(), L"ShowFlagSettings");
        if (!prop || !prop->get_class() || prop->get_class()->get_name() != L"ArrayProperty")
            return -1;
        auto *inner = reinterpret_cast<API::FArrayProperty *>(prop)->get_inner();
        if (!inner || !inner->get_class() || inner->get_class()->get_name() != L"StructProperty")
            return -1;
        API::UStruct *es = reinterpret_cast<API::FStructProperty *>(inner)->get_struct();
        if (!es)
            return -1;
        API::FProperty *nameP = es->find_property(L"ShowFlagName");
        API::FProperty *enP   = es->find_property(L"Enabled");
        if (!nameP || !enP || !enP->get_class() || enP->get_class()->get_name() != L"BoolProperty" ||
            !nameP->get_class() || nameP->get_class()->get_name() != L"StrProperty")
            return -1;
        const int32_t sz = es->get_properties_size(), al = es->get_min_alignment();
        const int32_t stride = (al > 0) ? ((sz + al - 1) / al) * al : sz;
        struct Arr
        {
            void   *Data;
            int32_t Num, Max;
        };
        Arr *arr = reinterpret_cast<Arr *>(reinterpret_cast<uint8_t *>(comp) + prop->get_offset());
        if (arr->Num != 0 || arr->Data)
            return -1; // never replace a list someone else owns
        API::FMalloc *fm = API::FMalloc::get();
        if (!fm || stride <= 0)
            return -1;
        const int n = (int)(sizeof(kLeanOff) / sizeof(kLeanOff[0]));
        auto *data = static_cast<uint8_t *>(fm->malloc((uint32_t)(n * stride), 8));
        if (!data)
            return -1;
        std::memset(data, 0, (size_t)n * stride);
        for (int i = 0; i < n; ++i)
        {
            uint8_t *e = data + (size_t)i * stride;
            const size_t len = wcslen(kLeanOff[i]) + 1;
            auto *chars = static_cast<wchar_t *>(fm->malloc((uint32_t)(len * sizeof(wchar_t)), 2));
            if (!chars)
                return -1;
            std::memcpy(chars, kLeanOff[i], len * sizeof(wchar_t));
            Arr str{chars, (int32_t)len, (int32_t)len};
            std::memcpy(e + nameP->get_offset(), &str, sizeof(str));
            reinterpret_cast<API::FBoolProperty *>(enP)->set_value_in_object(e, false);
        }
        arr->Data = data;
        arr->Num  = n;
        arr->Max  = n;
        return n;
    }

    bool MakeOneCapture(API::UObject *pc, int32_t w, int32_t h, API::UObject *&comp, API::UObject *&rt, bool lean = false)
    {
        comp = rt = nullptr;
        uint8_t fmt = 7; // ETextureRenderTargetFormat::RTF_R32f - depth in cm overflows a half float
        if (!CallRet(RenderLib, L"CreateRenderTarget2D",
                     {{L"WorldContextObject", &pc, sizeof(void *)}, {L"Width", &w, 4}, {L"Height", &h, 4}, {L"Format", &fmt, 1}},
                     &rt, sizeof(rt)) ||
            !rt)
        {
            LogError("depth capture: CreateRenderTarget2D failed");
            return false;
        }
        alignas(16) float xf[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0};
        bool no = false;
        const bool deferred = lean && FindFn(pc, L"FinishAddComponent") != nullptr;
        if (!CallRet(pc, L"AddComponentByClass",
                     {{L"Class", &CaptureClass, sizeof(void *)},
                      {L"bManualAttachment", &no, 1},
                      {L"RelativeTransform", xf, sizeof(xf)},
                      {L"bDeferredFinish", &deferred, 1}},
                     &comp, sizeof(comp)) ||
            !comp)
        {
            LogError("depth capture: could not add a SceneCaptureComponent2D");
            return false;
        }
        if (deferred)
        {
            const int off = ApplyLeanFlags(comp);
            Call(pc, L"FinishAddComponent",
                 {{L"Component", &comp, sizeof(void *)}, {L"bManualAttachment", &no, 1}, {L"RelativeTransform", xf, sizeof(xf)}});
            if (!LeanLogged)
            {
                LeanLogged = true;
                if (off > 0)
                    LogInfo("depth capture: lean - %d show flags off (shadows, lighting, fog, particles, translucency, post)", off);
                else
                    LogWarn("depth capture: show flags could not be set - full-cost render as before");
            }
        }
        // Properties by declared type only.
        auto *cls = comp->get_class();
        bool ok = true;
        if (auto *p = FindProp(cls, L"TextureTarget"); p && p->get_class() && p->get_class()->get_name() == L"ObjectProperty")
            *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(comp) + p->get_offset()) = rt;
        else
            ok = false;
        if (auto *p = FindProp(cls, L"CaptureSource"))
        {
            *(reinterpret_cast<uint8_t *>(comp) + p->get_offset()) = 4; // ESceneCaptureSource::SCS_SceneDepth
            CapSourceOff = p->get_offset();
        }
        else
            ok = false;
        if (auto *p = FindProp(cls, L"TextureTarget"))
            CapTargetOff = p->get_offset();
        if (auto *p = FindProp(cls, L"bCaptureEveryFrame"); p && p->get_class() && p->get_class()->get_name() == L"BoolProperty")
        {
            CapEveryFrameProp = reinterpret_cast<API::FBoolProperty *>(p);
            CapEveryFrameProp->set_value_in_object(comp, false);
        }
        else
            ok = false;
        if (auto *p = FindProp(cls, L"bCaptureOnMovement"); p && p->get_class() && p->get_class()->get_name() == L"BoolProperty")
            reinterpret_cast<API::FBoolProperty *>(p)->set_value_in_object(comp, false);
        if (auto *p = FindProp(cls, L"FOVAngle"); p && p->get_class() && p->get_class()->get_name() == L"FloatProperty")
            CapFovOff = p->get_offset();
        if (!ok || !FindFn(comp, L"CaptureScene"))
        {
            LogError("depth capture: TextureTarget / CaptureSource / bCaptureEveryFrame / CaptureScene not found - dropped");
            Call(comp, L"K2_DestroyComponent", {{L"Object", &comp, sizeof(void *)}});
            comp = nullptr;
            return false;
        }
        return true;
    }

    void DropCapture(bool componentAlive)
    {
        DepthRtObj = nullptr;
        ClearSource(VS_Depth);
        for (int k = 0; k < kCaps; ++k)
        {
            if (componentAlive && Caps[k])
                Call(Caps[k], L"K2_DestroyComponent", {{L"Object", &Caps[k], sizeof(void *)}});
            Caps[k]  = nullptr;
            CapRt[k] = nullptr;
            if (componentAlive && UCaps[k])
                Call(UCaps[k], L"K2_DestroyComponent", {{L"Object", &UCaps[k], sizeof(void *)}});
            UCaps[k] = nullptr;
            URt[k]   = nullptr;
        }
        {
            std::lock_guard<std::mutex> g(SrcLock);
            SrcUnits.Reset();
            SrcUnitsRaw = nullptr;
        }
        CapCount = 0;
        const bool had = Capture != nullptr;
        Capture      = nullptr;
        CaptureActor = nullptr;
        CaptureHud   = nullptr;
        HiddenActors.clear();
        if (had)
            LogInfo("depth capture stopped%s", componentAlive ? "" : " (its world is gone - forgotten, not touched)");
    }

    // Our own mech - body, cockpit, the pilot in it and anything else riding along - would fill
    // the capture's foreground. Everything ATTACHED to the mech is hidden, recursively (the pilot
    // is its own actor inside the cockpit; vanillahud-5 hid only the mech and the cockpit and the
    // headless pilot body showed through). Re-checked every second: the game spawns and swaps
    // attached actors mid-mission, and HideActorComponents only covers what exists when it is called.
    std::vector<API::UObject *> HiddenActors;

    ULONGLONG                   NextHideMs = 0;

    std::vector<uint8_t>        GaaBuf; // GetAttachedActors frame; its out-array is never zeroed (leak rule)

    API::UFunction             *GaaFn = nullptr;

    int32_t                     GaaOut = -1, GaaReset = -1;

    int32_t                     HacChildOff = -2; // HideActorComponents' bIncludeFromChildActors (-1: absent)

    // LASER DISCOVERY. A laser shot is its own short-lived actor attached to the firing mech
    // (Ray_C79SmallLaser_C in an earlier log). To draw beams on V-POD we need its base class and
    // where its two ends are kept, so the first time each Ray_* class is seen its class chain,
    // location and every vector / float / object property (with values) are logged.
    std::vector<const void *> BeamClassesLogged;

    void LogBeamOnce(API::UObject *a)
    {
        if (!a || !a->get_class() || BeamClassesLogged.size() >= 6)
            return;
        const std::wstring cn = ClassName(a);
        if (cn.rfind(L"Ray_", 0) != 0 && cn.find(L"Laser") == std::wstring::npos && cn.find(L"Beam") == std::wstring::npos)
            return;
        const void *cls = a->get_class();
        if (std::find(BeamClassesLogged.begin(), BeamClassesLogged.end(), cls) != BeamClassesLogged.end())
            return;
        BeamClassesLogged.push_back(cls);

        std::wstring chain;
        for (auto *st = static_cast<API::UStruct *>(a->get_class()); st; st = st->get_super_struct())
        {
            chain += (chain.empty() ? L"" : L" < ") + (st->get_fname() ? st->get_fname()->to_string() : L"?");
            if (st->get_fname() && st->get_fname()->to_string() == L"Actor")
                break;
        }
        float loc[3] = {};
        CallRet(a, L"K2_GetActorLocation", {}, loc, sizeof(loc));
        LogInfo("beam: %ls at (%.0f, %.0f, %.0f)", chain.c_str(), loc[0], loc[1], loc[2]);
        if (!RayClass)
            for (auto *st = static_cast<API::UStruct *>(a->get_class()); st; st = st->get_super_struct())
                if (st->get_fname() && st->get_fname()->to_string() == L"MWRay")
                {
                    RayClass = static_cast<API::UClass *>(st);
                    LogInfo("beams: MWRay class learned from this laser");
                    break;
                }

        for (auto *st = static_cast<API::UStruct *>(a->get_class()); st; st = st->get_super_struct())
        {
            if (st->get_fname() && st->get_fname()->to_string() == L"Actor")
                break;
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
            {
                if (!f->get_class())
                    continue;
                const std::wstring ty = f->get_class()->get_name();
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const uint8_t *at = reinterpret_cast<const uint8_t *>(a) + p->get_offset();
                const std::wstring nm = f->get_fname() ? f->get_fname()->to_string() : L"?";
                if (ty == L"StructProperty")
                {
                    auto *sp = reinterpret_cast<API::FStructProperty *>(f);
                    auto *ss = sp->get_struct();
                    const std::wstring sn = (ss && ss->get_fname()) ? ss->get_fname()->to_string() : L"?";
                    if (sn == L"Vector")
                    {
                        float v[3];
                        std::memcpy(v, at, sizeof(v));
                        LogInfo("beam:   %ls  Vector +%d = (%.0f, %.0f, %.0f)", nm.c_str(), p->get_offset(), v[0], v[1], v[2]);
                    }
                    else
                        LogInfo("beam:   %ls  %ls +%d", nm.c_str(), sn.c_str(), p->get_offset());
                }
                else if (ty == L"FloatProperty")
                {
                    float v;
                    std::memcpy(&v, at, sizeof(v));
                    LogInfo("beam:   %ls  float +%d = %.2f", nm.c_str(), p->get_offset(), v);
                }
                else if (ty == L"ObjectProperty")
                {
                    API::UObject *o = nullptr;
                    std::memcpy(&o, at, sizeof(o));
                    LogInfo("beam:   %ls  object +%d = %ls", nm.c_str(), p->get_offset(), o ? ClassName(o).c_str() : L"null");
                }
                else
                    LogInfo("beam:   %ls  %ls +%d", nm.c_str(), ty.c_str(), p->get_offset());
            }
        }
    }

    bool AttachedActors(API::UObject *actor, std::vector<API::UObject *> &out)
    {
        out.clear();
        if (!actor)
            return false;
        if (!GaaFn)
        {
            GaaFn = FindFn(actor, L"GetAttachedActors");
            if (!GaaFn)
                return false;
            GaaOut   = ParamOffset(GaaFn, L"OutActors");
            GaaReset = ParamOffset(GaaFn, L"bResetArray");
            const int32_t frame = GaaFn->get_properties_size();
            if (GaaOut < 0 || frame <= 0)
            {
                GaaFn = nullptr;
                return false;
            }
            GaaBuf.assign((size_t)frame + 16, 0);
        }
        // The array is in/out: empty it without freeing (Num = 0 keeps the allocation it owns).
        const int32_t zero = 0;
        std::memcpy(GaaBuf.data() + GaaOut + sizeof(void *), &zero, sizeof(zero));
        if (GaaReset >= 0)
            GaaBuf[GaaReset] = 1;
        actor->process_event(GaaFn, GaaBuf.data());
        struct Arr
        {
            API::UObject **Data;
            int32_t        Num, Max;
        } arr{};
        std::memcpy(&arr, GaaBuf.data() + GaaOut, sizeof(arr));
        for (int32_t i = 0; arr.Data && i < arr.Num && i < 256; ++i)
            if (arr.Data[i])
                out.push_back(arr.Data[i]);
        return true;
    }

    void HideOwnMech(ULONGLONG now)
    {
        if (now < NextHideMs)
            return;
        NextHideMs = now + 1000;
        auto *pawn = API::get()->get_local_pawn(0);
        API::UObject *anyCap = Capture ? Capture : ZCap;
        if (!pawn || !anyCap)
            return;

        std::vector<API::UObject *> found{pawn}, kids;
        if (auto *mesh = ObjProp(pawn, L"MechMeshComponent"))
            if (auto *cockpit = ObjProp(mesh, L"FirstPersonCockpitComponent"))
                if (auto *child = ObjProp(cockpit, L"ChildActor"))
                    found.push_back(child);
        for (size_t i = 0; i < found.size() && found.size() < 64; ++i)
            if (AttachedActors(found[i], kids))
                for (auto *k : kids)
                    if (std::find(found.begin(), found.end(), k) == found.end())
                        found.push_back(k);
        for (auto *a : found)
            LogBeamOnce(a);

        if (HacChildOff == -2)
        {
            API::UFunction *fn = FindFn(anyCap, L"HideActorComponents");
            HacChildOff = fn ? ParamOffset(fn, L"bIncludeFromChildActors") : -1;
        }
        const bool yes = true;
        std::vector<API::UObject *> hiders(std::begin(Caps), std::end(Caps));
        hiders.push_back(ZCap);
        hiders.push_back(ZICap);
        for (auto *cap : hiders)
            if (cap)
                for (auto *a : found)
                    if (HacChildOff >= 0)
                        Call(cap, L"HideActorComponents", {{L"InActor", &a, sizeof(void *)}, {L"bIncludeFromChildActors", &yes, 1}});
                    else
                        Call(cap, L"HideActorComponents", {{L"InActor", &a, sizeof(void *)}});

        if (found != HiddenActors)
        {
            std::wstring names;
            for (auto *a : found)
                names += (names.empty() ? L"" : L", ") + ClassName(a);
            LogInfo("depth capture hides %zu actors: %ls", found.size(), names.c_str());
            HiddenActors = found;
        }
    }

    static std::wstring PropType(API::FProperty *p)
    {
        return (p && p->get_class()) ? p->get_class()->get_name() : L"?";
    }

    // =====================================================================================
    // THE PILOT'S HANDS (vanillahud-55). An order on the player's own mech - the torso slew, the
    // autopilot - ends the moment the pilot touches the matching control, whatever it is: mouse,
    // keyboard, stick, HOTAS throttle, pedals, remapped through vJoy or not. Rather than asking
    // what the device is, the mech's own input handler (MechInputHandler) is watched: every float
    // it holds is read, grouped by name (throttle / turn / move -> LEGS; torso / twist / yaw /
    // pitch / look / aim -> TORSO), and a change in the group an order cares about is the pilot.
    // The baseline is re-taken for a second after every order starts and after every write of
    // ours (ApZeroThrottle), and followed - not judged - while the overlay has the game in
    // UI-only input. The field that ended an order is logged by name, so a field that turns out
    // to move on its own can be excluded. Inputs in neither group are logged the first time they
    // change, for discovery, and never end anything.
    // =====================================================================================
    enum PwGroupE { PWG_Other = 0, PWG_Legs, PWG_Torso };

    struct PwField
    {
        std::wstring    Name;
        API::FProperty *Prop;
        int             Group;
        float           Base;
        bool            Noted;
    };

    std::vector<PwField> PwFields;

    API::UClass *PwCls = nullptr;

    ULONGLONG    PwSettleUntil[3] = {};

    static int PwGroupOf(const std::wstring &name)
    {
        std::wstring n = name;
        for (auto &ch : n)
            ch = (wchar_t)towlower(ch);
        auto has = [&](const wchar_t *k) { return n.find(k) != std::wstring::npos; };
        if (has(L"torso") || has(L"twist"))
            return PWG_Torso;
        if (has(L"throttle") || has(L"forward") || has(L"move") || has(L"turn") || has(L"steer") || has(L"reverse"))
            return PWG_Legs;
        if (has(L"yaw") || has(L"pitch") || has(L"look") || has(L"aim"))
            return PWG_Torso;
        return PWG_Other;
    }

    API::UObject *PwHandler(API::UObject *pawn)
    {
        API::UObject *ih = pawn ? ObjProp(pawn, L"MechInputHandler") : nullptr;
        if (!ih || !ih->get_class())
            return nullptr;
        if (ih->get_class() == PwCls)
            return ih;
        PwCls = ih->get_class();
        PwFields.clear();
        std::wstring list;
        for (auto *st = static_cast<API::UStruct *>(PwCls); st; st = st->get_super_struct())
        {
            if (st->get_fname()->to_string() == L"Object")
                break;
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                if (PropType(p) != L"FloatProperty")
                    continue;
                PwField e{f->get_fname()->to_string(), p, 0, 0.0f, false};
                e.Group = PwGroupOf(e.Name);
                e.Base = *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(ih) + p->get_offset());
                static const wchar_t *kG[] = {L"-", L"LEGS", L"TORSO"};
                wchar_t b[128];
                _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls[%ls]=%.2f", e.Name.c_str(), kG[e.Group], e.Base);
                list += b;
                PwFields.push_back(e);
            }
        }
        TacLog("tacops: pilot watch - %zu input(s) on %ls:%ls", PwFields.size(), PwCls->get_fname()->to_string().c_str(),
                list.empty() ? L" (none)" : list.c_str());
        return ih;
    }

    // A second of settling for a group (-1: all): whatever the handler holds becomes the baseline.
    void PwSettle(int group)
    {
        const ULONGLONG until = GetTickCount64() + 1000;
        for (int g = 0; g < 3; ++g)
            if (group < 0 || g == group)
                PwSettleUntil[g] = until;
    }

    // THE THROTTLE, BACK TO ZERO. CancelMoveTo ends the walk-there but leaves the throttle where
    // MoveTo last set it - a MechWarrior throttle is a lever that stays put - so the mech kept
    // walking after "arrived". Zeroed in every place a throttle could be held: the movement
    // component's input (MoveInput.Throttle / .Turn) and any float named like a throttle on the
    // mech's input handler. What each held is logged, so the one that mattered can be seen.
    void ApZeroThrottle(API::UObject *pawn, bool log = true)
    {
        float before = 0.0f;
        CallRet(pawn, L"GetMechThrottle", {}, &before, sizeof(before));
        std::wstring found;
        API::UObject *mc = nullptr;
        CallRet(pawn, L"GetMechMovementComponent", {}, &mc, sizeof(mc));
        if (mc && mc->get_class())
            if (auto *mi = FindProp(mc->get_class(), L"MoveInput"))
                if (PropType(mi) == L"StructProperty")
                    if (auto *st = reinterpret_cast<API::FStructProperty *>(mi)->get_struct())
                    {
                        uint8_t *base = reinterpret_cast<uint8_t *>(mc) + mi->get_offset();
                        for (const wchar_t *n : {L"Throttle", L"Turn"})
                            if (auto *f = st->find_property(n))
                                if (PropType(f) == L"FloatProperty")
                                {
                                    float *v = reinterpret_cast<float *>(base + f->get_offset());
                                    wchar_t b[64];
                                    _snwprintf_s(b, _countof(b), _TRUNCATE, L" MoveInput.%ls=%.2f", n, *v);
                                    found += b;
                                    *v = 0.0f;
                                }
                    }
        if (API::UObject *ih = ObjProp(pawn, L"MechInputHandler"))
            for (auto *st = static_cast<API::UStruct *>(ih->get_class()); st; st = st->get_super_struct())
            {
                const std::wstring cn = st->get_fname()->to_string();
                if (cn == L"Object")
                    break;
                for (auto *f = st->get_child_properties(); f; f = f->get_next())
                {
                    const std::wstring n = f->get_fname()->to_string();
                    auto *p = reinterpret_cast<API::FProperty *>(f);
                    if (n.find(L"Throttle") == std::wstring::npos || PropType(p) != L"FloatProperty")
                        continue;
                    float *v = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(ih) + p->get_offset());
                    wchar_t b[96];
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L" InputHandler.%ls=%.2f", n.c_str(), *v);
                    found += b;
                    *v = 0.0f;
                }
            }
        PwSettle(PWG_Legs); // our own write, not the pilot's
        float after = 0.0f;
        CallRet(pawn, L"GetMechThrottle", {}, &after, sizeof(after));
        if (log)
            TacLog("tacops: autopilot throttle to zero - GetMechThrottle %.2f -> %.2f; zeroed:%ls", before, after,
                    found.empty() ? L" (nothing found)" : found.c_str());
    }

    // ---- reflection helpers --------------------------------------------------------------
    struct Arg
    {
        const wchar_t *Name;
        const void    *Data;
        size_t         Size;
    };

    static std::wstring ClassName(API::UObject *o)
    {
        return (o && o->get_class() && o->get_class()->get_fname()) ? o->get_class()->get_fname()->to_string() : L"?";
    }

    static API::FProperty *FindProp(API::UClass *cls, const wchar_t *name)
    {
        for (auto *st = static_cast<API::UStruct *>(cls); st; st = st->get_super_struct())
            if (auto *p = st->find_property(name))
                return p;
        return nullptr;
    }

    static API::UObject *ObjProp(API::UObject *obj, const wchar_t *name)
    {
        if (!obj || !obj->get_class())
            return nullptr;
        auto *p = FindProp(obj->get_class(), name);
        if (!p || !p->get_class() || p->get_class()->get_name() != L"ObjectProperty")
            return nullptr;
        return *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(obj) + p->get_offset());
    }

    API::UFunction *FindFn(API::UObject *obj, const wchar_t *name)
    {
        if (!obj || !obj->get_class())
            return nullptr;
        const void *cls = obj->get_class();
        for (auto &c : Fns)
            if (c.Cls == cls && c.Name == name)
                return c.Fn;
        API::UFunction *fn = nullptr;
        for (auto *st = static_cast<API::UStruct *>(obj->get_class()); st && !fn; st = st->get_super_struct())
            fn = st->find_function(name);
        if (Fns.size() >= 512)
            Fns.clear();
        Fns.push_back({cls, name, fn});
        if (!fn)
        {
            bool seen = false;
            for (auto &m : MissingLogged)
                seen |= (m == name);
            if (!seen)
            {
                MissingLogged.emplace_back(name);
                LogWarn("function %ls not found on %ls", name, ClassName(obj).c_str());
            }
        }
        return fn;
    }

    static int32_t ParamOffset(API::UFunction *fn, const wchar_t *name)
    {
        for (auto *f = fn->get_child_properties(); f; f = f->get_next())
            if (f->get_fname() && f->get_fname()->to_string() == name)
                return reinterpret_cast<API::FProperty *>(f)->get_offset();
        return -1;
    }

    bool Call(API::UObject *obj, const wchar_t *name, std::initializer_list<Arg> args)
    {
        return CallRet(obj, name, args, nullptr, 0);
    }

    // One reflected call. Arguments are placed by NAME at the offsets the function declares, and
    // nothing is written that the frame has no room for.
    bool CallRet(API::UObject *obj, const wchar_t *name, std::initializer_list<Arg> args, void *ret, size_t retSize)
    {
        if (!obj)
            return false;
        API::UFunction *fn = FindFn(obj, name);
        if (!fn)
            return false;
        const int32_t frame = fn->get_properties_size();
        if (frame < 0 || frame > (int32_t)sizeof(CallBuf) - 16)
            return false;
        std::memset(CallBuf, 0, (size_t)frame + 16);
        for (const auto &a : args)
        {
            const int32_t off = ParamOffset(fn, a.Name);
            if (off < 0 || (size_t)off + a.Size > (size_t)frame)
            {
                LogWarn("%ls: parameter %ls not in its frame - call skipped", name, a.Name);
                return false;
            }
            std::memcpy(CallBuf + off, a.Data, a.Size);
        }
        int32_t roff = -1;
        if (ret)
        {
            roff = ParamOffset(fn, L"ReturnValue");
            if (roff < 0 || (size_t)roff + retSize > (size_t)frame)
                return false;
        }
        obj->process_event(fn, CallBuf);
        if (ret)
            std::memcpy(ret, CallBuf + roff, retSize);
        return true;
    }

    // =====================================================================================
    // TEXTURE SHARING (render thread) - ScreenExport's proven path
    // =====================================================================================
    // GAME THREAD: hand the render thread render target `rt`'s D3D texture (or nothing), with our
    // own reference held on it.
    void SetSource(int s, UTextureRenderTarget2D *rt)
    {
        ID3D11Texture2D *tex = nullptr;
        if (rt && rt->Resource && rt->Resource->Texture2DRHI)
            tex = rt->Resource->Texture2DRHI->GetNativeResource();
        std::lock_guard<std::mutex> g(SrcLock);
        if (SrcRaw[s] == tex)
            return;
        Src[s]    = tex; // AddRef the new, Release the old
        SrcRaw[s] = tex;
    }

    void ClearSource(int s) { SetSource(s, nullptr); }

    static ID3D11Texture2D *NativeOf(UTextureRenderTarget2D *rt)
    {
        return (rt && rt->Resource && rt->Resource->Texture2DRHI) ? rt->Resource->Texture2DRHI->GetNativeResource()
                                                                  : nullptr;
    }

    void SetDepthSource(UTextureRenderTarget2D *rt, UTextureRenderTarget2D *unitsRt,
                        const VanillaExportBlock::Camera &pose, const VanillaExportBlock::Lock &lock,
                        const BeamSet &beams)
    {
        ID3D11Texture2D *tex = NativeOf(rt), *utex = NativeOf(unitsRt);
        std::lock_guard<std::mutex> g(SrcLock);
        SrcBeams = beams;
        SrcDepthPose = pose;
        SrcDepthLock = lock;
        if (SrcUnitsRaw != utex)
        {
            SrcUnits    = utex;
            SrcUnitsRaw = utex;
        }
        if (SrcRaw[VS_Depth] == tex)
            return;
        Src[VS_Depth]    = tex;
        SrcRaw[VS_Depth] = tex;
    }

    // A typeless source needs a typed twin for HUDDisplay's null-descriptor SRV.
    static DXGI_FORMAT Typed(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        default: return f;
        }
    }

    // `fmt` UNKNOWN = publish in the source's own (typed) format; otherwise that fixed format.
    bool EnsureShared(ID3D11Device *dev, int s, const D3D11_TEXTURE2D_DESC &src, DXGI_FORMAT fmt)
    {
        if (Shared[s] && SharedW[s] == src.Width && SharedH[s] == src.Height && SrcFmt[s] == src.Format)
            return true;
        Shared[s].Reset();
        SharedHandle[s] = nullptr;

        const DXGI_FORMAT out = (fmt == DXGI_FORMAT_UNKNOWN) ? Typed(src.Format) : fmt;
        if (fmt != DXGI_FORMAT_UNKNOWN && src.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS &&
            src.Format != DXGI_FORMAT_B8G8R8A8_UNORM && src.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
            !FormatWarned[s])
        {
            FormatWarned[s] = true;
            LogError("%ls render target is format %d, outside the B8G8R8A8 group", VanillaSourceName(s),
                     (int)src.Format);
        }

        D3D11_TEXTURE2D_DESC d{};
        d.Width            = src.Width;
        d.Height           = src.Height;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = out;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        d.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, tex.GetAddressOf())))
        {
            if (!FormatWarned[s])
            {
                FormatWarned[s] = true;
                LogError("%ls: cannot create a shared texture in format %d", VanillaSourceName(s), (int)out);
            }
            return false;
        }
        ComPtr<IDXGIResource> dxgi;
        HANDLE h = nullptr;
        if (FAILED(tex.As(&dxgi)) || FAILED(dxgi->GetSharedHandle(&h)) || !h)
            return false;
        Shared[s]       = tex;
        SharedHandle[s] = h;
        SharedW[s]      = src.Width;
        SharedH[s]      = src.Height;
        SrcFmt[s]       = src.Format;
        return true;
    }

    // Producer acquires 0 and releases 1; HUDDisplay acquires 1 and releases 0. Zero timeout.
    // `pose`, when given, is written into Block->DepthCam while the key is held: the consumer reads
    // it under the same key, so the image and the pose it came from always arrive together.
    void CopyInto(ID3D11DeviceContext *ctx, int s, ID3D11Texture2D *src, const VanillaExportBlock::Camera *pose = nullptr,
                  const VanillaExportBlock::Lock *lock = nullptr)
    {
        ComPtr<IDXGIKeyedMutex> m;
        if (FAILED(Shared[s].As(&m)) || m->AcquireSync(0, 0) != S_OK)
            return;
        ctx->CopyResource(Shared[s].Get(), src);
        if (pose)
            Block->DepthCam = *pose;
        if (lock)
            Block->DepthLock = *lock;
        m->ReleaseSync(1);
    }

    // =====================================================================================
    // THE TACOPS OVERLAY (vanillahud-44, VHUD v16). HUDDisplay's window image drawn over the game
    // (flat screen), worked with the game's own mouse cursor. Two halves:
    //   GAME THREAD (OvTick): while HUDDisplay keeps OvWantMs fresh - the throttle is zeroed once,
    //     the cursor shown (PlayerController.bShowMouseCursor) and the game put in UI-only input
    //     (WidgetBlueprintLibrary::SetInputMode_UIOnlyEx), which is how the game's own menus stop
    //     the mech taking mouse and keys. Put back (SetInputMode_GameOnly, cursor hidden) when
    //     it goes stale - closed, or HUDDisplay gone.
    //   RENDER THREAD (OvDraw, from on_present): the shared image copied under its keyed mutex,
    //     drawn over the back buffer at OvOpacity, and the cursor's place over it reported back.
    //     Every piece of pipeline state touched is saved first and put back after: the engine
    //     caches its D3D state and must find it exactly as it left it.
    // =====================================================================================
    // true (vanillahud-58, confirmed in game): the game keeps its input while the overlay is up
    // (game-and-UI mode), so the stick, throttle and keyboard still fly the mech; the mouse's clicks
    // and wheel over the overlay, and its raw movement (mouse look) anywhere, are swallowed in
    // on_message. The mouse never turned the torso with the cursor showing.
    // false = the older UI-only mode, where every device stops and held stick axes are released.
    static constexpr bool kOvGameInput = true;

    std::atomic<bool> OvCursorInside{false};  // render thread: the cursor is over the overlay image

    std::atomic<uint32_t> OvSwallowed[3] = {}; // clicks, wheel, raw mouse moves swallowed since opening (logged)

    // The game window's messages, before the game sees them (UEVR's window hook). false = swallow.
    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override
    {
        (void)hwnd;
        (void)wparam;
        if (!kOvGameInput || !OvInputOn.load())
            return true;
        switch (msg)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            // Over the overlay only: outside it the mouse still fires. (The overlay reads the
            // buttons itself with GetAsyncKeyState, which swallowing does not affect.)
            if (OvCursorInside.load())
            {
                OvSwallowed[0]++;
                return false;
            }
            return true;
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            if (OvCursorInside.load())
            {
                OvSwallowed[1]++;
                return false;
            }
            return true;
        case WM_INPUT:
        {
            // Raw MOUSE input only (mouse look): a stick or throttle read through raw input is a
            // different device type and goes through.
            RAWINPUTHEADER h{};
            UINT sz = sizeof(h);
            if (GetRawInputData((HRAWINPUT)lparam, RID_HEADER, &h, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                h.dwType == RIM_TYPEMOUSE)
            {
                OvSwallowed[2]++;
                return false;
            }
            return true;
        }
        default:
            return true;
        }
    }

    std::atomic<bool> OvInputOn{false};

    ULONGLONG         OvInputOffMs = 0; // when the overlay last gave the game its input back

    bool              OvApplied = false;

    bool              OvReapply = false; // the HUD stopped ticking a while: set the input mode again

    API::UObject     *OvAppliedPc = nullptr;

    void OvTick(API::UObject *pc)
    {
        const ULONGLONG now = GetTickCount64();
        const uint64_t want = Block->OvWantMs;
        const bool on = want && Block->OvHandle && (int64_t)(now - want) < 500;
        if (on == OvApplied && (!on || pc == OvAppliedPc) && !OvReapply)
            return;
        OvReapply = false;
        OvApplied = on;
        OvAppliedPc = on ? pc : nullptr;
        API::UObject *pawn = API::get()->get_local_pawn(0);
        if (!kOvGameInput)
        {
            if (on && pawn)
                ApZeroThrottle(pawn, false); // the mech stops while the pilot is in the map (quietly: every open)
            // Held stick axes, both ways: on opening, the game stops reading input and the last stick
            // value was left applied (a torso slewing at 2 deg/s kept slewing with the stick centred);
            // on closing, the stick only reports again when it moves, so the stale value came back.
            OvReleaseAxes(pc, pawn, on ? "open" : "close");
        }
        bool cursorOk = false;
        if (auto *p = FindProp(pc->get_class(), L"bShowMouseCursor"))
            if (PropType(p) == L"BoolProperty")
            {
                reinterpret_cast<API::FBoolProperty *>(p)->set_value_in_object(pc, on);
                cursorOk = true;
            }
        bool modeOk = false;
        static API::UObject *wbl = nullptr;
        if (!wbl)
            if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.WidgetBlueprintLibrary"))
                wbl = c->get_class_default_object();
        if (wbl)
        {
            // Newer engines give the UI-only switch a bFlushInput flag: asked for when it exists.
            API::UFunction *uiFn = (on && !kOvGameInput) ? FindFn(wbl, L"SetInputMode_UIOnlyEx") : nullptr;
            const bool flushArg = uiFn && ParamOffset(uiFn, L"bFlushInput") >= 0;
            const uint8_t yes = 1;
            if (on && kOvGameInput)
                // Game AND UI: the game keeps its input (stick, throttle, keyboard) with the cursor
                // showing; mouse lock off and the cursor kept during capture (both zero = defaults).
                // Clicks, the wheel and mouse look are swallowed in on_message instead.
                modeOk = Call(wbl, L"SetInputMode_GameAndUIEx", {{L"PlayerController", &pc, sizeof(pc)}});
            else if (on && flushArg)
                modeOk = Call(wbl, L"SetInputMode_UIOnlyEx", {{L"PlayerController", &pc, sizeof(pc)}, {L"bFlushInput", &yes, 1}});
            else
                modeOk = on ? Call(wbl, L"SetInputMode_UIOnlyEx", {{L"PlayerController", &pc, sizeof(pc)}})
                            : Call(wbl, L"SetInputMode_GameOnly", {{L"PlayerController", &pc, sizeof(pc)}});
            static bool flushLogged = false;
            if (on && !flushLogged && !kOvGameInput)
            {
                flushLogged = true;
                LogInfo("tacops overlay: SetInputMode_UIOnlyEx %s a bFlushInput flag", flushArg ? "has" : "has no");
            }
        }
        OvInputOn.store(on);
        if (!on)
            OvInputOffMs = GetTickCount64();
        LogInfo("tacops overlay: %s - cursor %s, input mode %s%s", on ? "OPEN" : "closed", cursorOk ? "set" : "NOT FOUND",
                on ? (kOvGameInput ? "game and UI " : "UI only ") : "game only ", modeOk ? "set" : "FAILED");
        if (!on && kOvGameInput)
            LogInfo("tacops overlay: while open, swallowed %u click(s), %u wheel notch(es), %u raw mouse move(s)",
                    OvSwallowed[0].load(), OvSwallowed[1].load(), OvSwallowed[2].load());
        OvSwallowed[0] = OvSwallowed[1] = OvSwallowed[2] = 0;
    }

    // THE STICK, LET GO. Every float on the mech's input handler that holds an axis's current
    // value (named ...Input / ...Axis / Joystick... / Gamepad..., and not a setting such as a
    // sensitivity, deadzone or scale) is set back to zero, and the controller's pressed keys and
    // axes are flushed where the engine exposes that. The first time, every float the handler
    // has is logged with its value (PwHandler), and whatever was zeroed is logged each time -
    // and again a second later, to see whether something wrote it straight back.
    ULONGLONG OvAxisCheckMs = 0;

    static bool OvIsAxisName(const std::wstring &name)
    {
        std::wstring n = name;
        for (auto &ch : n)
            ch = (wchar_t)towlower(ch);
        auto has = [&](const wchar_t *k) { return n.find(k) != std::wstring::npos; };
        if (has(L"sensitiv") || has(L"deadzone") || has(L"dead_zone") || has(L"scale") || has(L"curve") ||
            has(L"max") || has(L"min") || has(L"speed") || has(L"invert") || has(L"multiplier") || has(L"exponent") ||
            has(L"threshold") || has(L"time") || has(L"delay"))
            return false;
        return has(L"input") || has(L"axis") || has(L"joystick") || has(L"gamepad") || has(L"stick");
    }

    void OvReleaseAxes(API::UObject *pc, API::UObject *pawn, const char *when)
    {
        if (pc)
        {
            static int flushState = 0; // 0 untried, 1 found, 2 not a reflected function
            if (FindFn(pc, L"FlushPressedKeys"))
            {
                Call(pc, L"FlushPressedKeys", {});
                if (flushState != 1)
                    LogInfo("tacops overlay: FlushPressedKeys called");
                flushState = 1;
            }
            else if (flushState == 0)
            {
                flushState = 2;
                LogInfo("tacops overlay: FlushPressedKeys is not callable here");
            }
        }
        API::UObject *ih = PwHandler(pawn); // logs the handler's floats the first time
        if (!ih)
            return;
        std::wstring zeroed;
        for (const auto &f : PwFields)
        {
            if (!OvIsAxisName(f.Name))
                continue;
            float *v = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(ih) + f.Prop->get_offset());
            if (*v == 0.0f)
                continue;
            wchar_t b[96];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls=%.2f", f.Name.c_str(), *v);
            zeroed += b;
            *v = 0.0f;
        }
        PwSettle(-1); // our write, not the pilot's
        LogInfo("tacops overlay: %s - stick axes zeroed:%ls", when, zeroed.empty() ? L" (none held)" : zeroed.c_str());
        OvAxisCheckMs = GetTickCount64() + 1000;
    }

    // A second after an open or close: did anything write the zeroed axes straight back?
    void OvAxisCheck()
    {
        if (!OvAxisCheckMs || GetTickCount64() < OvAxisCheckMs)
            return;
        OvAxisCheckMs = 0;
        API::UObject *ih = PwHandler(API::get()->get_local_pawn(0));
        if (!ih)
            return;
        std::wstring held;
        for (const auto &f : PwFields)
        {
            if (!OvIsAxisName(f.Name))
                continue;
            const float v = *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(ih) + f.Prop->get_offset());
            if (v != 0.0f)
            {
                wchar_t b[96];
                _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls=%.2f", f.Name.c_str(), v);
                held += b;
            }
        }
        LogInfo("tacops overlay: 1 s later, stick axes%ls", held.empty() ? L" all still zero" : (L" non-zero again:" + held).c_str());
    }

    // Render-thread half.
    ComPtr<ID3D11Texture2D>          OvShared, OvPriv;

    ComPtr<ID3D11ShaderResourceView> OvSrv;

    uint64_t                         OvHandleOpen = 0;

    UINT                             OvTexW = 0, OvTexH = 0;

    ComPtr<ID3D11VertexShader>       OvVs;

    ComPtr<ID3D11PixelShader>        OvPs;

    ComPtr<ID3D11Buffer>             OvCb;

    ComPtr<ID3D11SamplerState>       OvSamp;

    ComPtr<ID3D11BlendState>         OvBlend;

    ComPtr<ID3D11RasterizerState>    OvRs;

    ComPtr<ID3D11DepthStencilState>  OvDss;

    bool                             OvPipeTried = false, OvPipeOk = false, OvDrawLogged = false;

    typedef HRESULT(WINAPI *OvCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                        LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

    bool OvPipeline(ID3D11Device *dev)
    {
        if (OvPipeTried)
            return OvPipeOk;
        OvPipeTried = true;
        static const char kSrc[] =
            "cbuffer C : register(b0) { float4 Rect; float4 Params; };\n"
            "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
            "V vs(uint id : SV_VertexID) { float2 t = float2(id & 1, id >> 1); V o;\n"
            "  o.p = float4(lerp(Rect.x, Rect.z, t.x), lerp(Rect.y, Rect.w, t.y), 0, 1); o.uv = t; return o; }\n"
            "Texture2D T : register(t0); SamplerState S : register(s0);\n"
            "float4 ps(V i) : SV_Target { float4 c = T.Sample(S, i.uv); return float4(c.rgb, Params.x); }\n";
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc ? reinterpret_cast<OvCompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        if (!compile)
        {
            LogError("tacops overlay: d3dcompiler_47.dll / D3DCompile not available - overlay cannot be drawn");
            return false;
        }
        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(compile(kSrc, sizeof(kSrc) - 1, "tacops_overlay", nullptr, nullptr, "vs", "vs_4_0", 0, 0,
                           vsb.GetAddressOf(), err.ReleaseAndGetAddressOf())) ||
            FAILED(compile(kSrc, sizeof(kSrc) - 1, "tacops_overlay", nullptr, nullptr, "ps", "ps_4_0", 0, 0,
                           psb.GetAddressOf(), err.ReleaseAndGetAddressOf())))
        {
            LogError("tacops overlay: shader compile failed: %s",
                     err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
            return false;
        }
        if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, OvVs.GetAddressOf())) ||
            FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, OvPs.GetAddressOf())))
            return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, OvCb.GetAddressOf())))
            return false;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sd, OvSamp.GetAddressOf())))
            return false;
        D3D11_BLEND_DESC bl{};
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (FAILED(dev->CreateBlendState(&bl, OvBlend.GetAddressOf())))
            return false;
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&rs, OvRs.GetAddressOf())))
            return false;
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = FALSE;
        ds.StencilEnable = FALSE;
        if (FAILED(dev->CreateDepthStencilState(&ds, OvDss.GetAddressOf())))
            return false;
        OvPipeOk = true;
        LogInfo("tacops overlay: drawing pipeline ready");
        return true;
    }

    void OvReleaseImage()
    {
        OvSrv.Reset();
        OvPriv.Reset();
        OvShared.Reset();
        OvHandleOpen = 0;
        OvTexW = OvTexH = 0;
    }

    void OvDraw(ID3D11Device *dev, ID3D11DeviceContext *ctx, const UEVR_RendererData *rd)
    {
        const uint64_t handle = Block->OvHandle;
        // Not over a death screen, a pause menu or a load: with the HUD not ticking the game thread
        // cannot switch the input mode back yet (it does as soon as the HUD returns), and the map
        // has nothing current to show anyway.
        const bool hudLive = GameLive(GetTickCount64()); // cameracapture-22
        // cameracapture-35 (bug sweep): HUDDisplay stopped asking (closed, or the overlay shut where the cockpit does
        // not tick - menus, hangar - so OvTick never ran): the input is given back here as well. OvTick still restores
        // the game's input mode on the next cockpit tick.
        {
            const uint64_t want = Block->OvWantMs;
            if (OvInputOn.load() && (!want || GetTickCount64() - want > 1500))
                OvInputOn.store(false);
        }
        if (!hudLive)
        {
            Block->OvShown = 0;
            OvCursorInside.store(false);
            return;
        }
        if (!OvInputOn.load() || !handle || !rd->swapchain)
        {
            OvCursorInside.store(false);
            Block->OvShown = 0;
            if (!OvInputOn.load() && OvShared)
                OvReleaseImage();
            return;
        }
        if (!OvPipeline(dev))
        {
            Block->OvShown = 0;
            return;
        }
        // The image, reopened whenever HUDDisplay made a new one (its window was resized).
        if (handle != OvHandleOpen || !OvShared)
        {
            OvReleaseImage();
            ComPtr<ID3D11Texture2D> t;
            if (FAILED(dev->OpenSharedResource(reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void **>(t.GetAddressOf()))))
            {
                Block->OvShown = 0;
                return;
            }
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            D3D11_TEXTURE2D_DESC p = d;
            p.MiscFlags = 0;
            p.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            p.Usage = D3D11_USAGE_DEFAULT;
            p.CPUAccessFlags = 0;
            p.MipLevels = 1;
            if (FAILED(dev->CreateTexture2D(&p, nullptr, OvPriv.GetAddressOf())) ||
                FAILED(dev->CreateShaderResourceView(OvPriv.Get(), nullptr, OvSrv.GetAddressOf())))
            {
                OvReleaseImage();
                Block->OvShown = 0;
                return;
            }
            OvShared = t;
            OvHandleOpen = handle;
            OvTexW = d.Width;
            OvTexH = d.Height;
            LogInfo("tacops overlay: image opened, %ux%u", OvTexW, OvTexH);
        }
        ComPtr<IDXGIKeyedMutex> km;
        if (SUCCEEDED(OvShared.As(&km)) && km->AcquireSync(1, 0) == S_OK)
        {
            ctx->CopyResource(OvPriv.Get(), OvShared.Get());
            km->ReleaseSync(0);
        }

        // The back buffer the frame is about to be presented from.
        auto *sc = static_cast<IDXGISwapChain *>(rd->swapchain);
        ComPtr<ID3D11Texture2D> bb;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(bb.GetAddressOf()))))
            return;
        D3D11_TEXTURE2D_DESC bd{};
        bb->GetDesc(&bd);
        D3D11_RENDER_TARGET_VIEW_DESC rvd{};
        rvd.Format = Typed(bd.Format);
        rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(dev->CreateRenderTargetView(bb.Get(), &rvd, rtv.GetAddressOf())))
            return;

        // Where it goes: OvScale of the height, the image's own aspect, centred.
        const float bw = (float)bd.Width, bh = (float)bd.Height;
        float oh = bh * (Block->OvScale > 0.1f && Block->OvScale <= 1.0f ? Block->OvScale : 0.9f);
        float ow = oh * (float)OvTexW / (float)(OvTexH ? OvTexH : 1);
        if (ow > bw * 0.98f)
        {
            oh *= bw * 0.98f / ow;
            ow = bw * 0.98f;
        }
        // Where the pilot put it (HUDDisplay remembers it), else centred; never more than 90% off
        // screen, so it can always be grabbed and brought back.
        float rx0 = (bw - ow) * 0.5f, ry0 = (bh - oh) * 0.5f;
        if (Block->OvPosX > -5.0f && Block->OvPosY > -5.0f && Block->OvPosX < 5.0f && Block->OvPosY < 5.0f) // -10 = centred
        {
            rx0 = (std::min)((std::max)(Block->OvPosX * bw, -ow * 0.9f), bw - ow * 0.1f);
            ry0 = (std::min)((std::max)(Block->OvPosY * bh, -oh * 0.9f), bh - oh * 0.1f);
        }
        Block->OvRectX = rx0 / bw;
        Block->OvRectY = ry0 / bh;
        Block->OvRectW = ow / bw;
        Block->OvRectH = oh / bh;
        const float opacity = (Block->OvOpacity > 0.05f && Block->OvOpacity <= 1.0f) ? Block->OvOpacity : 0.7f;

        // The cursor over it, in the same pixels.
        {
            DXGI_SWAP_CHAIN_DESC scd{};
            POINT cp{};
            RECT cr{};
            float u = -1.0f, v = -1.0f;
            if (SUCCEEDED(sc->GetDesc(&scd)) && scd.OutputWindow && GetCursorPos(&cp) &&
                ScreenToClient(scd.OutputWindow, &cp) && GetClientRect(scd.OutputWindow, &cr) && cr.right > 0 &&
                cr.bottom > 0)
            {
                const float px = (float)cp.x * bw / (float)cr.right, py = (float)cp.y * bh / (float)cr.bottom;
                u = (px - rx0) / ow;
                v = (py - ry0) / oh;
            }
            uint32_t b = 0;
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) b |= 1u;
            if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) b |= 2u;
            if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) b |= 4u;
            OvCursorInside.store(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
            Block->OvMouseU = u;
            Block->OvMouseV = v;
            Block->OvButtons = b;
            MemoryBarrier();
            Block->OvMouseSeq++;
        }

        // Save everything the draw touches.
        D3D11_PRIMITIVE_TOPOLOGY topo;
        ctx->IAGetPrimitiveTopology(&topo);
        ComPtr<ID3D11InputLayout> il;
        ctx->IAGetInputLayout(il.GetAddressOf());
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11HullShader> hs;
        ComPtr<ID3D11DomainShader> dsh;
        ID3D11ClassInstance *vsI[256], *psI[256], *gsI[256];
        UINT vsN = 256, psN = 256, gsN = 256;
        ctx->VSGetShader(vs.GetAddressOf(), vsI, &vsN);
        ctx->PSGetShader(ps.GetAddressOf(), psI, &psN);
        ctx->GSGetShader(gs.GetAddressOf(), gsI, &gsN);
        ctx->HSGetShader(hs.GetAddressOf(), nullptr, nullptr);
        ctx->DSGetShader(dsh.GetAddressOf(), nullptr, nullptr);
        ComPtr<ID3D11Buffer> vsCb, psCb;
        ctx->VSGetConstantBuffers(0, 1, vsCb.GetAddressOf());
        ctx->PSGetConstantBuffers(0, 1, psCb.GetAddressOf());
        ComPtr<ID3D11ShaderResourceView> psSrv;
        ctx->PSGetShaderResources(0, 1, psSrv.GetAddressOf());
        ComPtr<ID3D11SamplerState> psSamp;
        ctx->PSGetSamplers(0, 1, psSamp.GetAddressOf());
        ComPtr<ID3D11RasterizerState> rs;
        ctx->RSGetState(rs.GetAddressOf());
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ctx->RSGetViewports(&nvp, vps);
        UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_RECT scs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ctx->RSGetScissorRects(&nsc, scs);
        ComPtr<ID3D11BlendState> bs;
        float bf[4];
        UINT sm = 0;
        ctx->OMGetBlendState(bs.GetAddressOf(), bf, &sm);
        ComPtr<ID3D11DepthStencilState> dss;
        UINT sref = 0;
        ctx->OMGetDepthStencilState(dss.GetAddressOf(), &sref);
        ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ComPtr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv.GetAddressOf());
        ComPtr<ID3D11Buffer> vb, ib;
        UINT vbStride = 0, vbOff = 0, ibOff = 0;
        DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
        ctx->IAGetVertexBuffers(0, 1, vb.GetAddressOf(), &vbStride, &vbOff);
        ctx->IAGetIndexBuffer(ib.GetAddressOf(), &ibFmt, &ibOff);

        // Draw.
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(OvCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            float *c = static_cast<float *>(m.pData);
            c[0] = rx0 / bw * 2.0f - 1.0f;
            c[1] = 1.0f - ry0 / bh * 2.0f;
            c[2] = (rx0 + ow) / bw * 2.0f - 1.0f;
            c[3] = 1.0f - (ry0 + oh) / bh * 2.0f;
            c[4] = opacity;
            c[5] = c[6] = c[7] = 0.0f;
            ctx->Unmap(OvCb.Get(), 0);
            D3D11_VIEWPORT vp{0.0f, 0.0f, bw, bh, 0.0f, 1.0f};
            ID3D11RenderTargetView *target = rtv.Get();
            ctx->OMSetRenderTargets(1, &target, nullptr);
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(OvRs.Get());
            ctx->OMSetBlendState(OvBlend.Get(), nullptr, 0xFFFFFFFFu);
            ctx->OMSetDepthStencilState(OvDss.Get(), 0);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ID3D11Buffer *noVb = nullptr;
            UINT zero = 0;
            ctx->IASetVertexBuffers(0, 1, &noVb, &zero, &zero);
            ctx->VSSetShader(OvVs.Get(), nullptr, 0);
            ctx->PSSetShader(OvPs.Get(), nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ID3D11Buffer *cb = OvCb.Get();
            ctx->VSSetConstantBuffers(0, 1, &cb);
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView *srv = OvSrv.Get();
            ctx->PSSetShaderResources(0, 1, &srv);
            ID3D11SamplerState *smp = OvSamp.Get();
            ctx->PSSetSamplers(0, 1, &smp);
            ctx->Draw(4, 0);
            Block->OvShown = 1;
            if (!OvDrawLogged)
            {
                OvDrawLogged = true;
                LogInfo("tacops overlay: drawn at %.0fx%.0f on a %ux%u back buffer (format %d), opacity %.2f", ow, oh,
                        bd.Width, bd.Height, (int)bd.Format, opacity);
            }
        }

        // Put it all back.
        ID3D11ShaderResourceView *noSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &noSrv);
        ctx->IASetPrimitiveTopology(topo);
        ctx->IASetInputLayout(il.Get());
        ID3D11Buffer *vbp = vb.Get();
        ctx->IASetVertexBuffers(0, 1, &vbp, &vbStride, &vbOff);
        ctx->IASetIndexBuffer(ib.Get(), ibFmt, ibOff);
        ctx->VSSetShader(vs.Get(), vsI, vsN);
        ctx->PSSetShader(ps.Get(), psI, psN);
        ctx->GSSetShader(gs.Get(), gsI, gsN);
        ctx->HSSetShader(hs.Get(), nullptr, 0);
        ctx->DSSetShader(dsh.Get(), nullptr, 0);
        for (UINT i = 0; i < vsN; ++i) if (vsI[i]) vsI[i]->Release();
        for (UINT i = 0; i < psN; ++i) if (psI[i]) psI[i]->Release();
        for (UINT i = 0; i < gsN; ++i) if (gsI[i]) gsI[i]->Release();
        ID3D11Buffer *vcb = vsCb.Get(), *pcb = psCb.Get();
        ctx->VSSetConstantBuffers(0, 1, &vcb);
        ctx->PSSetConstantBuffers(0, 1, &pcb);
        ID3D11ShaderResourceView *oldSrv = psSrv.Get();
        ctx->PSSetShaderResources(0, 1, &oldSrv);
        ID3D11SamplerState *oldSmp = psSamp.Get();
        ctx->PSSetSamplers(0, 1, &oldSmp);
        ctx->RSSetState(rs.Get());
        ctx->RSSetViewports(nvp, vps);
        ctx->RSSetScissorRects(nsc, scs);
        ctx->OMSetBlendState(bs.Get(), bf, sm);
        ctx->OMSetDepthStencilState(dss.Get(), sref);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv.Get());
        for (auto *r : rtvs)
            if (r)
                r->Release();
    }

    void ReleaseTextures()
    {
        for (int s = 0; s < VS_COUNT; ++s)
        {
            Shared[s].Reset();
            SharedHandle[s] = nullptr;
            SharedW[s] = SharedH[s] = 0;
            SrcFmt[s]  = DXGI_FORMAT_UNKNOWN;
        }
    }
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::unique_ptr<CameraCapture> g_plugin{new CameraCapture()};
