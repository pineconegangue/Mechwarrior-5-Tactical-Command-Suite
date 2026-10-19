// VanillaHud.cpp - the game's own (vanilla) HUD, moved off the main screen into HUDDisplay.
//
// No MWVR dependency. Where ScreenExport captures MWVR's cockpit screen components, this plugin
// makes its OWN off-screen widget components and puts the game's vanilla HUD into them.
//
// =========================================================================================
// WHAT HAPPENS WHEN THE HUD APPEARS (build vanillahud-3)
//
//   1. every listed HUD element (weapons, paperdolls, heat, minimap...) is taken off the HUD and
//      given its own off-screen widget component, sized to what it occupied on screen;
//   2. the decorative frames those elements sat in are collapsed;
//   3. the HUD itself - which by now holds only the crosshair, lock ring, hit arrows, prompts
//      and D-pad panel - is taken off the viewport into one more component at full viewport
//      size, so its contents keep their screen positions and can be laid back over the game image.
// The main screen is left with no HUD at all. HUDDisplay shows all of it.
//
// Components live on the PLAYER CONTROLLER, not the mech: the controller survives a mech switch,
// and a controller is a hidden actor, so nothing attached to it is ever drawn in the world. Each
// component also sits 20 km under the map and is "owner no see", belt and braces.
//
// Element CLASS, not member name, finds each element; the HUD_C property it came through is
// re-read before every use, which is also the liveness test (the HUD's own reference keeps it
// alive, and the GC nulls that reference if it ever dies).
//
// NOTHING HERE IS PERSISTENT. Only live objects in memory change - no game file, save or setting.
// A restart gives back the stock HUD.
//
// Paperdolls are the game's own paperdoll widgets, drawn by the game with its own art.
//
// =========================================================================================
// THREADS
//
// All UMG work runs on the GAME thread inside a detour on HUD_C's own Tick (hooked by nothing else
// in this project). Once the HUD itself is hosted, that Tick is driven by its component, which
// keeps ticking while unseen (TickWhenOffscreen).
//
// SCOPE (vanillahud-103, the module split): this plugin ONLY captures the vanilla HUD, as MWVR's
// ScreenExport captures MWVR's. Everything else it once did lives in plugins of its own, driven by
// the player cockpit's tick so they work under MWVR as well:
//   CameraCapture.dll - the Targeting depth camera, the CAP zoom camera, the TacOps overlay;
//   LanceControl.dll  - TacOps orders and the lance map, autopilot and torso, insane allies, fire
//                       settings, missile arc / range checks, the lance status feed, the probes.
// All three share the MW5_VanillaHUDExport block (each writes only its own part), so HUDDisplay
// and VoiceCommand are unchanged.
//
// The render thread (on_present) only copies textures, and never touches a UObject: the game thread
// hands it each render target's D3D texture with a reference of its own held (SetSource).
//
// Reflected calls return object pointers, ints, bools, floats or plain structs - nothing that owns
// heap memory - so zeroing their frames cannot leak. The one TArray-returning call
// (K2_GetComponentsByClass) keeps its own never-zeroed buffer.
// =========================================================================================

#include "SlowTimer.h"
#include <PluginExtension.h>

#include <windows.h>
#include <GsPointer.h>
#include <d3d11.h>
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
#include <vector>
#include <unordered_set>

#include <mw5/UTextureRenderTarget2D.h>
#include <mw5/FTextureResource.h>
#include <mw5/FRHITexture2D.h>

#include <VanillaHudShared.h>
#include <LanceStatusShared.h>

using Microsoft::WRL::ComPtr;

class VanillaHud final : public PluginExtension
{
    static constexpr const wchar_t *kHudClassName = L"WidgetBlueprintGeneratedClass /Game/UI/Mech/HUD.HUD_C";

    static constexpr ULONGLONG kHookRetryMs  = 3000; // looking for HUD_C while it is not loaded

    static constexpr ULONGLONG kGameTickMs   = 100;  // decisions at 10 Hz

    static constexpr ULONGLONG kOtherHudMs   = 1000; // a second HUD_C instance may take over after this

    static constexpr ULONGLONG kPresentStaleMs = 500; // HUD not ticking this long: publish nothing

    static constexpr int       kMaxHostFails = 3;    // per source, per HUD instance

    static constexpr float     kHideDepthCm  = -2000000.0f; // 20 km under the map

    static constexpr uint8_t   kCollapsed    = 1;    // ESlateVisibility::Collapsed

    static constexpr float     kOverflowScale = 1.6f; // element canvas vs its on-screen box

    static constexpr float     kOverflowPad   = 64.0f;

    // ---- shared block --------------------------------------------------------------------
    HANDLE              MmfHandle   = nullptr;

    HANDLE              WriterMutex = nullptr;

    VanillaExportBlock *Block       = nullptr;

    ULONGLONG           LastSetupMs = 0;

    // ---- the HUD_C Tick hook -------------------------------------------------------------
    API::UFunction *HookedFn      = nullptr;

    BP_FUNC         TickOrig      = nullptr;

    bool            HookGaveUp    = false;

    ULONGLONG       LastHookTryMs = 0;

    // ---- game-thread state ---------------------------------------------------------------
    API::UObject *Hud        = nullptr; // the HUD_C instance we are working with

    std::atomic<ULONGLONG> HudSeenMs{0}; // read by the render thread: no HUD tick, no publishing

    ULONGLONG     LastGameMs = 0;

    bool          Scanned    = false;

    API::UObject *HostActor  = nullptr; // the player controller our components are on

    API::UClass        *WidgetCompClass   = nullptr;

    int32_t             RtOffset          = -1; // UWidgetComponent::RenderTarget

    API::FBoolProperty *TickOffscreenProp = nullptr;

    API::UObject       *LayoutLib         = nullptr; // WidgetLayoutLibrary CDO

    API::UObject       *SlateLib          = nullptr; // SlateBlueprintLibrary CDO

    bool                ClassesResolved   = false;

    struct SlotSnap
    {
        struct Item
        {
            std::wstring Setter;
            uint8_t      Bytes[64];
            int32_t      Size;
        };
        std::vector<Item> Items;
    };

    // One per HUD element, plus one for the HUD itself (index VS_Overlay).
    struct SlotState
    {
        API::UObject *Widget    = nullptr;
        int32_t       MemberOff = -1; // the HUD_C property it came through (-1 for the HUD itself)
        API::UObject *Parent    = nullptr;
        int32_t       Index     = -1;
        bool          WasInViewport = false; // the HUD itself: it lives in the viewport, not a panel
        SlotSnap      Snap;
        API::UObject *Comp      = nullptr;
        uint8_t       State     = VHS_Idle;
        int           Fails     = 0;
    };

    static constexpr int kHosted = VS_Depth; // the elements, the HUD overlay, the markers layer, the dialogue
    // vanillahud-105: the HUD itself and the markers layer stay in the game (V-POD has its own crosshair).
    static constexpr bool kCaptureRestOfHud = false;

    SlotState S[kHosted];
    // vanillahud-122 (user 2026-10-18): each panel's on-screen size as first measured for this HUD - a panel given back to
    // the game and taken again was measured after the game re-laid it out (torso twist 1088 -> 3136 wide, alerts
    // 0x172 -> 3136x1792). The capture keeps the first size.
    float FirstW[kHosted] = {}, FirstH[kHosted] = {};
    bool  FirstSet[kHosted] = {};

    struct Frame
    {
        API::UObject *W;
        int32_t       MemberOff;
        uint8_t       Orig = 0xFF; // its visibility before we first collapsed it (0xFF = never collapsed)
    };

    std::vector<Frame> Frames;
    API::UObject      *HookTryPawn = nullptr; // bug sweep 15: the pawn at the last hook try

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

    API::UClass                          *CaptureClass  = nullptr;

    API::UObject                         *RenderLib     = nullptr; // KismetRenderingLibrary CDO

    API::UObject                         *WidgetLib     = nullptr; // WidgetBlueprintLibrary CDO

    API::UClass                          *MarkerClass   = nullptr; // MWMarkerWidget

    API::UClass                          *TargetBoxClass = nullptr; // MWTargetBoxWidget

    // ---- world markers (nav points, objective / unit markers, target boxes) ----------------
    API::UObject *MarkerRoot     = nullptr; // their common top-level layer, when there is one

    bool          MarkersEach    = false;   // no common layer: each marker is its own top-level widget

    ULONGLONG     LastMarkerScan = 0;

    std::vector<uint8_t> GawBuf; // GetAllWidgetsOfClass frame; its out-array is never zeroed (leak rule)

    API::UFunction      *GawFn = nullptr;

    int32_t GawWorld = -1, GawOut = -1, GawClass = -1, GawTop = -1;

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

    static constexpr const wchar_t *kWriterMutexName = L"Local\\MW5_VanillaHUD_Writer";

  public:
    inline static VanillaHud *Instance;

    VanillaHud()
    {
        Instance                  = this;
        PluginExtension::Instance = this;
        Name       = "VanillaHud";
        Version    = "0.4.0";
        VersionInt = 4;
    }

    ~VanillaHud() override
    {
        // Neither the hook nor the HUD is restored here: this runs at process exit, when both may
        // already be gone, and writing into freed objects is how an exit crash happens.
        ReleaseTextures();
        CloseBlock(true);
    }

    void on_initialize() override
    {
        LogInfo("%s %s initialized (MWVR handshake deliberately skipped)", Name, Version);
        LogInfo("BUILD=vanillahud-122 (PANEL SIZE KEPT + QUIET SLOW LINES 2026-10-18: a panel taken again keeps the size it was first captured at; one-by-one slow: lines only with [debug] verbose_log, hitches over 50 ms always) (vanillahud-121 QUIET LOG 2026-10-18: periodic profiles / status lines only with GlobalSettings [debug] verbose_log = 1) (vanillahud-120 CRASH REVIEW 2026-10-18: every template object checked live and not being destroyed before it is read or written; a panel the HUD replaced is never touched; live markers only while a mech is piloted; the early detours install with a barrier) (vanillahud-119 (HIDDEN FROM BIRTH 2026-10-18: the HUD panels HUDDisplay takes, their frames, the transmission and the IN GAME pieces switched off are at opacity 0 in HUD_C's template; the lock ring's template root too; an early game-thread driver (UMG function detours) finds the HUD / marker / box classes as they load and hooks the HUD tick at once - nothing is drawn on the game's screen first) (vanillahud-118 (WORLD MARKERS / TARGET BOX HIDDEN AT THE TEMPLATE 2026-10-18: each marker / target-box class's template root collapsed, so new ones are born hidden - no flash; existing ones collapsed once; minimap icons left alone; probe / profile logging of 115-117 removed) (vanillahud-114 (BUG SWEEP 2 2026-10-18: the HUD hook is looked for only with the same mech two tries running - never mid level change; a HUD rescan keeps the frames' original visibility) (vanillahud-113 GAME TARGET BOX HIDDEN WHILE ENHANCED IMAGING IS UP - EI draws its own; back when EI is off, if [hud] game_target_box is ticked) (vanillahud-112 PRESENT COPIES AT 60 HZ + UNUSED-SLOT BACK-OFF for good, after the A/B: +5 fps, 42 %% fewer present gaps over 12 ms; present profile kept) (vanillahud-111 A/B: the copy mode alternates every 30 s - OLD every present / NEW 60 Hz + back-off - with frame pacing per window; measuring build) (vanillahud-110 PRESENT COPIES AT 60 HZ + UNUSED-SLOT BACK-OFF: copies on 60 copy frames per second, all slots together; a slot HUDDisplay has not taken for 30 copy frames is tried every 250 ms) (vanillahud-109 PRESENT PROFILE: the render-thread copy timed per slot - acquire / copy / release, copies vs busy - 'present profile' every 30 s; measuring only) (vanillahud-108 lock-sound mute removed (did not work); give-back fixes: the paperdoll frame goes with the OWN panel only; clipping / retaining switched off for the capture is put back when a panel returns to the game; ECM is an IN GAME piece (the HUD has no ECM panel widget); a returned panel's visibility logged; panel switches: GlobalSettings [hud] picks HUDDisplay or the game per panel, live; the rest of the HUD - crosshair, prompts, markers - stays in the game, V-POD draws its own; game pieces (crosshair, lock ring, hit arrows, incoming arrows, prompts, d-pad, detection, airstrike, world markers, target box) shown or hidden per GlobalSettings [hud] game_*; VHUD v20; slow-step timer: HUD tick / present over 2 ms logged as 'slow:'; vanilla HUD capture only: cameras, overlay and lance control are their own plugins now)");
        EnsureBlock();
        InstallEarly(); // vanillahud-119
    }

    // =====================================================================================
    // RENDER THREAD
    // =====================================================================================
    // SLOW-STEP TIMER (vanillahud-104): the HUD tick and the render-thread present, timed whole.
    SlowTimer SlowG{"VanillaHud"}, SlowR{"VanillaHud render"};
    void on_present() override
    {
        SlowR.Begin();
        PresentInner();
        SlowR.Step("on_present");
        SlowR.Flush([](const char *l) { uevr::API::get()->log_info("%s", l); });
    }
    // PRESENT PROFILE (vanillahud-109, user 2026-10-13 "go ahead and investigate"): measuring only. The render-thread
    // present copies every hosted HUD slot into its shared texture through a keyed mutex; over 1 ms on ~30 presents a
    // second in a fight (worst 5-9 ms). Every 30 s "present profile": presents per second, total avg / worst, the
    // prelude (hook check, renderer, context), and per slot: tries, copies (= HUDDisplay took the previous frame -
    // the mutex came back on key 0), busy (HUDDisplay had not taken it: nothing copied), and the time in AcquireSync /
    // CopyResource / ReleaseSync. Nothing decided or copied changes.
    LARGE_INTEGER PpFreq{};
    double PpNow()
    {
        if (!PpFreq.QuadPart)
            QueryPerformanceFrequency(&PpFreq);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return double(t.QuadPart) * 1000.0 / double(PpFreq.QuadPart);
    }
    struct PpSlot
    {
        unsigned Tries = 0, Copies = 0, Busy = 0, NoSrc = 0;
        double   Acq = 0, Copy = 0, Rel = 0, Max = 0;
    };
    PpSlot    Pp[VS_COUNT];
    // COPY RATE + UNUSED-SLOT BACK-OFF (vanillahud-110, user 2026-10-13 "they should run at 60hz, make this fix"). The
    // 109 profile: ~70 % of the present's cost was the FIRST keyed-mutex AcquireSync of each present (D3D11 flushes
    // there), on a slot HUDDisplay never takes; HUDDisplay took only the slots its pages show (the paperdoll, at its
    // own ~60 Hz). Now: (1) copies happen on COPY FRAMES only - 60 per second on a deadline clock (every slot on the same
    // presents, so one flush per copy frame); presents in between touch no mutex at all. (2) A slot HUDDisplay has not
    // taken for 30 copy frames in a row (~0.5 s) is tried only every 250 ms until it is taken again, so a page switched
    // to starts updating within a quarter of a second.
    static constexpr double kCopyIntervalMs = 1000.0 / 60.0;
    static constexpr int    kBusyBackoffAfter = 30;
    static constexpr double kBusyRetryMs = 250.0;
    double    CopyNextMs = 0;
    bool      CopyFrame = false;
    // A/B (vanillahud-111, user 2026-10-13 "yes try that"): the copy mode alternates every 30 s profile window -
    // OLD = every present, no back-off (vanillahud-108's behaviour) / NEW = 60 copy frames + back-off (110). Each window's
    // line says its mode, the present rate and the present-to-present intervals (frame pacing), so the two can be
    // compared across the same fight. Measuring build: the mode in use flips by itself.
    // vanillahud-112: the A/B is over (2026-10-13, 9 pairs in one fight: NEW +5 fps avg 90.6 -> 95.6 presents/s, present
    // gaps over 12 ms -42 %, our present cost 0.5-0.9 -> 0.2-0.4 ms) - NEW for good, the profile line stays.
    bool      AbNew = true;
    double    PpLastPresent = 0, PpGapMax = 0;
    unsigned  PpGap12 = 0, PpGap20 = 0, PpGap33 = 0;
    int       SlotBusyRun[VS_COUNT] = {};
    double    SlotRetryAt[VS_COUNT] = {};
    unsigned  PpCopyFrames = 0, PpBackedOff = 0;
    double    PpCopyMs = 0; // running total of the time inside CopyInto (to split it from the rest of Publish)
    unsigned  PpPresents = 0, PpLive = 0;
    double    PpTot = 0, PpTotMax = 0, PpPre = 0, PpPubMs = 0;
    ULONGLONG PpLogMs = 0;
    void PpReport(ULONGLONG now)
    {
        if (!PpLogMs)
            PpLogMs = now;
        if (now - PpLogMs < 30000)
            return;
        const double sec = (now - PpLogMs) / 1000.0, r = PpPresents ? double(PpPresents) : 1.0;
        if (gsp::VerboseLog()) LogInfo("present profile, last %.0f s: MODE %s | %u presents (%.1f/s, HUD live in %u), gaps over 12 / 20 / 33 ms: %u / %u / %u, "
                "longest %.1f ms | copy frames %u (%.0f/s), slot tries skipped by the back-off %u, total avg %.3f / worst %.2f ms, "
                "prelude avg %.3f ms, slots outside the copy avg %.3f ms",
                sec, AbNew ? "NEW (60 Hz + back-off)" : "OLD (every present)", PpPresents, PpPresents / sec, PpLive, PpGap12, PpGap20,
                PpGap33, PpGapMax, PpCopyFrames, PpCopyFrames / sec, PpBackedOff, PpTot / r, PpTotMax, PpPre / r, PpPubMs / r);
        for (int i = 0; i < VS_COUNT; ++i)
        {
            const PpSlot &e = Pp[i];
            if (!e.Tries && !e.NoSrc)
                continue;
            if (gsp::VerboseLog()) LogInfo("present profile:   %-18ls tries %6u copies %6u busy %6u no-source %6u | per present: acquire %.3f copy %.3f release %.3f ms, "
                    "worst one %.2f ms",
                    VanillaSourceName(i), e.Tries, e.Copies, e.Busy, e.NoSrc, e.Acq / r, e.Copy / r, e.Rel / r, e.Max);
        }
        for (auto &e : Pp)
            e = PpSlot{};
        PpPresents = PpLive = PpCopyFrames = PpBackedOff = 0;
        PpGap12 = PpGap20 = PpGap33 = 0;
        PpGapMax = 0;
        // (vanillahud-111 alternated the mode here; 112 keeps NEW)
        PpTot = PpTotMax = PpPre = PpPubMs = 0;
        PpLogMs = now;
    }

    void PresentInner()
    {
        if (!Block && !EnsureBlock())
            return;
        const double pp0 = PpNow(); // vanillahud-109
        if (PpLastPresent > 0) // vanillahud-111: frame pacing
        {
            const double gap = pp0 - PpLastPresent;
            PpGapMax = (std::max)(PpGapMax, gap);
            if (gap > 12.0)
                ++PpGap12;
            if (gap > 20.0)
                ++PpGap20;
            if (gap > 33.0)
                ++PpGap33;
        }
        PpLastPresent = pp0;
        const ULONGLONG now = GetTickCount64();
        Block->ProducerTickMs = now;

        TryInstallHook();

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

        // Nothing is published while the HUD is not ticking - a loading screen, a level being torn
        // down: what the textures show then is stale, and the game thread is not there to vouch for them.
        const ULONGLONG seen = HudSeenMs.load(std::memory_order_relaxed);
        const bool      live = seen && now >= seen && now - seen < kPresentStaleMs;
        ComPtr<ID3D11Texture2D> src[VS_COUNT];
        {
            std::lock_guard<std::mutex> g(SrcLock);
            for (int i = 0; i < kHosted; ++i)
                src[i] = Src[i];
        }
        const double pp1 = PpNow();
        PpPre += pp1 - pp0;
        // vanillahud-110: is this present a copy frame? (a deadline clock: 60 per second on average at any present rate)
        CopyFrame = !AbNew || pp1 >= CopyNextMs; // vanillahud-111: OLD mode copies on every present
        if (CopyFrame)
        {
            CopyNextMs += kCopyIntervalMs;
            if (CopyNextMs < pp1) // far behind (a hitch, the first present): start again from now
                CopyNextMs = pp1 + kCopyIntervalMs;
            ++PpCopyFrames;
        }
        double inCopy = 0;
        for (int i = 0; i < kHosted; ++i)
        {
            auto &info = Block->Slots[i];
            info.HostState = S[i].State; // a byte; torn reads are harmless
            if (!(live && src[i]))
                ++Pp[i].NoSrc;
            const double c0 = PpCopyMs;
            Publish(dev, ctx.Get(), i, live ? src[i].Get() : nullptr, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
            inCopy += PpCopyMs - c0;
        }
        const double pp2 = PpNow();
        PpPubMs += (pp2 - pp1) - inCopy;
        PpTot += pp2 - pp0;
        PpTotMax = (std::max)(PpTotMax, pp2 - pp0);
        ++PpPresents;
        if (live)
            ++PpLive;
        PpReport(now);
        // (The depth / zoom cameras and the TacOps overlay are CameraCapture's since the split.)
    }

    void on_device_reset() override
    {
        ReleaseTextures();
        if (Block)
            for (int i = 0; i < kHosted; ++i) // this plugin's slots only
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
        swprintf_s(initName, L"Local\\MW5_VanillaHUDExport_Init_%lu", GetCurrentProcessId());
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

    // Find HUD_C and hook its Tick. Only tried while the local pawn is a mech - that is when the
    // HUD exists - and throttled, because find_uobject walks the object table.
    void TryInstallHook()
    {
        if (HookedFn || HookGaveUp)
            return;
        const ULONGLONG now = GetTickCount64();
        if (LastHookTryMs && now - LastHookTryMs < kHookRetryMs)
            return;
        LastHookTryMs = now;

        auto *pawn = API::get()->get_local_pawn(0);
        if (!pawn || !pawn->get_property_data<void *>(L"MechMeshComponent"))
        {
            HookTryPawn = nullptr;
            return;
        }
        // bug sweep 15 (2026-10-18): this runs on the RENDER thread (until the hook is in - once per session). The object
        // table walk below waits for the same mech on two tries running (kHookRetryMs apart), so it is never done while a
        // level is being loaded or torn down.
        if (pawn != HookTryPawn)
        {
            HookTryPawn = pawn;
            return;
        }
        auto *cls = API::get()->find_uobject<API::UClass>(kHudClassName);
        if (!cls)
            return;
        auto *fn = cls->find_function(L"Tick");
        if (!fn)
        {
            LogError("HUD_C has no Tick function - cannot run on the game thread");
            HookGaveUp = true;
            return;
        }
        auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
        if (*slot == &OnHudTick)
            return;
        TickOrig = *slot;
        *slot    = &OnHudTick;
        HookedFn = fn;
        LogInfo("hooked HUD_C::Tick (%p)", (void *)fn);
    }

    static void *OnHudTick(API::UObject *self, FFrame *frame, void *const result)
    {
        void *r = nullptr;
        if (Instance && Instance->TickOrig)
            r = Instance->TickOrig(self, frame, result); // the HUD updates its elements first
        if (Instance)
            Instance->SlowG.Begin(); // SLOW-STEP TIMER (vanillahud-104)
        if (Instance)
            Instance->InGameTick = true;
        if (Instance)
            Instance->GameTick(self);
        if (Instance)
            Instance->InGameTick = false;
            Instance->SlowG.Step("GameTick");
            Instance->SlowG.Flush([](const char *l) { uevr::API::get()->log_info("%s", l); });
        return r;
    }

    // =====================================================================================
    // GAME THREAD
    // =====================================================================================
    void GameTick(API::UObject *hud)
    {
        if (!hud || !Block)
            return;
        const ULONGLONG now = GetTickCount64();

        if (hud != Hud)
        {
            if (Hud && now - HudSeenMs < kOtherHudMs)
                return; // another HUD_C ticking while ours is alive: leave it alone
            AdoptHud(hud);
        }
        HudSeenMs = now;

        if (now - LastGameMs < kGameTickMs)
            return;
        LastGameMs = now;

        if (!ResolveClasses())
            return;
        if (!Scanned)
            ScanHud();
        HudSettingsTick(); // vanillahud-105: the panel switches

        auto *pc = API::get()->get_player_controller(0);
        if (!pc)
            return;
        // A different controller: every component went with the old one. Put what we held back
        // where it came from; the next tick hosts it again on the new controller.
        if (HostActor && pc != HostActor)
        {
            LogInfo("player controller changed - returning everything to the HUD first");
            for (int s = kHosted - 1; s >= 0; --s)
                if (S[s].State == VHS_Hosted)
                {
                    ClearSource(s);
                    S[s].Comp = nullptr;
                    Restore(s);
                    S[s].State = VHS_Idle;
                }
            HostActor = nullptr;
            ForgetWorldHud(pc);
        }

        std::vector<API::UObject *> live;
        const bool haveList = ComponentsOn(pc, WidgetCompClass, live);
        auto onActor = [&](API::UObject *c) {
            if (!haveList)
                return true; // cannot tell; assume it is still there rather than thrash
            for (auto *x : live)
                if (x == c)
                    return true;
            return false;
        };

        // 1. the elements
        bool allTried = true;
        for (int s = 0; s < VS_ELEMENT_COUNT; ++s)
        {
            SlotState &st = S[s];
            if (!st.Widget)
            {
                st.State = VHS_NotFound;
                continue;
            }
            // vanillahud-105: left on the game's HUD by the player's choice.
            if (!WantHosted(s))
            {
                GiveBack(s, live, haveList);
                if (st.State != VHS_InGame)
                    Opaque(st.Widget); // vanillahud-119: the game keeps it - seen on its screen
                st.State = VHS_InGame;
                PublishRt(s);
                continue;
            }
            if (st.State == VHS_InGame)
            {
                st.State = VHS_Idle;
                st.Fails = 0;
            }
            if (st.State == VHS_Hosted && !onActor(st.Comp))
            {
                ClearSource(s);
                st.Comp = nullptr;
                Restore(s);
                st.State = VHS_Idle;
            }
            if ((st.State == VHS_Idle || st.State == VHS_Failed) && st.Fails < kMaxHostFails)
            {
                allTried = false;
                if (HostElement(s, pc))
                {
                    st.State  = VHS_Hosted;
                    HostActor = pc;
                }
                else
                {
                    ++st.Fails;
                    st.State = VHS_Failed;
                    if (st.Fails >= kMaxHostFails && StillCurrent(s)) // vanillahud-120: never a widget the HUD replaced
                        Opaque(st.Widget); // vanillahud-119: not taken - left visible on the game's HUD
                }
            }
            PublishRt(s);
        }

        // 2. the frames
        CollapseFrames();
        WatchHudMembers();
        ApplyGamePieces(pc); // vanillahud-105b: crosshair, lock ring, arrows, prompts... shown or hidden in game

        // 3./4. the rest of the HUD (crosshair, prompts, hit arrows) and the world markers: no
        //    longer captured (vanillahud-105) - they stay in the game, and V-POD draws its own
        //    crosshair and lock. The old code is kept below, switched off.
        (void)allTried;
        if (kCaptureRestOfHud)
        {
            // 3. the HUD itself - last, once every element has had its go, so the elements never ride
            //    along inside the overlay.
            SlotState &ov = S[VS_Overlay];
            if (ov.State == VHS_Hosted && !onActor(ov.Comp))
            {
                ClearSource(VS_Overlay);
                ov.Comp = nullptr;
                Restore(VS_Overlay);
                ov.State = VHS_Idle;
            }
            if (allTried && (ov.State == VHS_Idle || ov.State == VHS_Failed) && ov.Fails < kMaxHostFails)
            {
                if (HostOverlay(pc))
                {
                    ov.State  = VHS_Hosted;
                    HostActor = pc;
                }
                else
                {
                    ++ov.Fails;
                    ov.State = VHS_Failed;
                }
            }
            else if (ov.State == VHS_Hosted)
            {
                // The game may put its HUD back on the viewport (after a cutscene, a menu...). Take it
                // off again; a HUD in two places at once is the one state we must not leave it in.
                bool inViewport = false;
                if (CallRet(Hud, L"IsInViewport", {}, &inViewport, 1) && inViewport)
                {
                    LogInfo("the game re-added its HUD to the viewport - moving it back to the overlay");
                    Call(Hud, L"RemoveFromParent", {});
                    Call(ov.Comp, L"SetWidget", {{L"Widget", &Hud, sizeof(void *)}});
                }
            }
            PublishRt(VS_Overlay);

            // 4. world markers - off the main view too.
            HandleMarkers(pc, live, haveList);
            PublishRt(VS_Markers);

        }
        else
        {
            S[VS_Overlay].State = VHS_InGame;
            S[VS_Markers].State = VHS_InGame;
            PublishRt(VS_Overlay);
            PublishRt(VS_Markers);
        }

        // 5. the mission dialogue - off the main view, onto the Objectives screen.
        HandleDialogue(pc, live, haveList);
        PublishRt(VS_Dialogue);
        PublishRt(VS_Subtitles);

    }

    // A new HUD instance (new mission, or the HUD was rebuilt). Components still on the current
    // controller hold pieces of a HUD that no longer exists: empty and destroy them.
    void AdoptHud(API::UObject *hud)
    {
        auto *pc = API::get()->get_player_controller(0);
        std::vector<API::UObject *> live;
        const bool haveList = pc && pc == HostActor && ComponentsOn(pc, WidgetCompClass, live);
        for (int s = 0; s < kHosted; ++s)
        {
            ClearSource(s);
            bool present = false;
            if (haveList && S[s].Comp)
                for (auto *x : live)
                    present |= (x == S[s].Comp);
            if (S[s].State == VHS_Hosted && present)
            {
                API::UObject *none = nullptr;
                Call(S[s].Comp, L"SetWidget", {{L"Widget", &none, sizeof(none)}});
                DestroyComponent(S[s].Comp);
            }
            S[s] = SlotState{};
            FirstSet[s] = false; // vanillahud-122
        }
        ForgetWorldHud(pc);
        Frames.clear();
        LockRingClass = nullptr; // a new mission may load it at another address
        // vanillahud-119: the pieces were born from HUD_C's template (opacity 0 where switched off): one pass
        // over every piece puts the ones switched on at full opacity, whatever the template said then.
        for (int i = 0; i < kPieceCount; ++i)
            if (kPieces[i].Special < 2)
                PieceHidden[i] = PieceFirst[i] = true;
        HostActor = nullptr;
        Hud       = hud;
        Scanned   = false;
        LogInfo("HUD instance %p adopted", (void *)hud);
    }

    // A new world (HUD or controller change): the HUD capture's own per-world state.
    void ForgetWorldHud(API::UObject *pc)
    {
        (void)pc;
        MarkerRoot     = nullptr;
        MarkersEach    = false;
        LastMarkerScan = 0;
        MarkerVerdicts.clear();
        MarkerSummary.clear();
        ForgetDialogue();
        ForgetTemplates(); // vanillahud-118
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

    void PublishRt(int s)
    {
        SlotState &st = S[s];
        if (st.State == VHS_Hosted && st.Comp && RtOffset >= 0)
            SetSource(s, *reinterpret_cast<UTextureRenderTarget2D **>(reinterpret_cast<uint8_t *>(st.Comp) + RtOffset));
        else
            ClearSource(s);
    }

    bool ResolveClasses()
    {
        if (ClassesResolved)
            return WidgetCompClass != nullptr;
        ClassesResolved = true;
        WidgetCompClass = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.WidgetComponent");
        if (!WidgetCompClass)
        {
            LogError("UMG.WidgetComponent class not found - hosting disabled");
            return false;
        }
        if (auto *p = FindProp(WidgetCompClass, L"RenderTarget"))
            if (p->get_class() && p->get_class()->get_name() == L"ObjectProperty")
                RtOffset = p->get_offset();
        if (auto *p = FindProp(WidgetCompClass, L"TickWhenOffscreen"))
            if (p->get_class() && p->get_class()->get_name() == L"BoolProperty")
                TickOffscreenProp = reinterpret_cast<API::FBoolProperty *>(p);
        if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.WidgetLayoutLibrary"))
            LayoutLib = c->get_class_default_object();
        if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.SlateBlueprintLibrary"))
            SlateLib = c->get_class_default_object();
        if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.WidgetBlueprintLibrary"))
            WidgetLib = c->get_class_default_object();
        if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.KismetRenderingLibrary"))
            RenderLib = c->get_class_default_object();
        UserWidgetClass = API::get()->find_uobject<API::UClass>(L"Class /Script/UMG.UserWidget");
        CaptureClass   = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SceneCaptureComponent2D");
        MarkerClass    = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWMarkerWidget");
        TargetBoxClass = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWTargetBoxWidget");
        LogInfo("capture class %s, rendering lib %s, widget lib %s, marker class %s, target box class %s",
                CaptureClass ? "found" : "NOT FOUND", RenderLib ? "found" : "NOT FOUND", WidgetLib ? "found" : "NOT FOUND",
                MarkerClass ? "found" : "NOT FOUND", TargetBoxClass ? "found" : "NOT FOUND");
        LogInfo("WidgetComponent: RenderTarget @%d, TickWhenOffscreen %s; layout lib %s, slate lib %s", RtOffset,
                TickOffscreenProp ? "found" : "NOT FOUND", LayoutLib ? "found" : "NOT FOUND",
                SlateLib ? "found" : "NOT FOUND");
        return true;
    }

    // Elements by the CLASS of what HUD_C's object properties point at; frames by class name.
    // Only declared, type-checked ObjectProperty slots are read.
    // Every widget the HUD holds by property, for the visibility log (what appears when).
    struct HudMember
    {
        std::wstring Name, Cls;
        int32_t      Off;
        API::UObject *Last;
        int8_t       Visible; // -1 unknown
    };

    std::vector<HudMember> HudMembers;

    ULONGLONG              MemberCheckMs = 0;

    int32_t                DlgMemberOff[2] = {-1, -1}; // HUD_C members taken for the dialogue slots

    API::UObject *HudMemberOfClass(const wchar_t *cls)
    {
        for (auto *st = static_cast<API::UStruct *>(Hud->get_class()); st; st = st->get_super_struct())
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
            {
                if (!f->get_class() || f->get_class()->get_name() != L"ObjectProperty")
                    continue;
                auto *obj = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(Hud) +
                                                               reinterpret_cast<API::FProperty *>(f)->get_offset());
                if (obj && ClassName(obj) == cls)
                    return obj;
            }
        return nullptr;
    }

    // name:Class, children in [ ], down through panels and into nested user widgets (depth 6).
    void DumpWidgetTree(API::UObject *w, std::wstring &out, int depth = 0)
    {
        if (!w || depth > 6 || out.size() > 6000)
            return;
        out += (w->get_fname() ? w->get_fname()->to_string() : L"?") + L":" + ClassName(w);
        API::UObject *root = nullptr;
        if (API::UObject *wt = ObjProp(w, L"WidgetTree"))
            root = ObjProp(wt, L"RootWidget");
        if (root)
        {
            out += L" { ";
            DumpWidgetTree(root, out, depth + 1);
            out += L" }";
            return;
        }
        int32_t n = 0;
        if (!ChildCountFn(w) || !CallRet(w, L"GetChildrenCount", {}, &n, sizeof(n)) || n <= 0)
            return;
        out += L" [ ";
        for (int32_t i = 0; i < n && i < 32; ++i)
        {
            API::UObject *c = nullptr;
            int32_t idx = i;
            if (CallRet(w, L"GetChildAt", {{L"Index", &idx, sizeof(idx)}}, &c, sizeof(c)) && c)
            {
                if (i)
                    out += L", ";
                DumpWidgetTree(c, out, depth + 1);
            }
        }
        out += L" ]";
    }

    // Walks a widget tree like DumpWidgetTree, turning off each widget's own clipping and noting
    // what it was (EWidgetClipping: 0 inherit, 1 clip to bounds, 2 without intersecting,
    // 3 always, 4 on demand) and any SizeBox overrides.
    // vanillahud-106: what Unclip switched off, per slot, so Restore can put it back - the own
    // paperdoll came back to the game offset with its RetainerBox no longer retaining.
    struct UnclipUndo
    {
        API::UObject *W;
        uint8_t       Kind; // 0 retain rendering, 1 clipping
        uint8_t       Old;
    };
    std::vector<UnclipUndo> Undo[VS_COUNT];
    std::vector<UnclipUndo> *UndoNow = nullptr;

    void Unclip(API::UObject *w, std::wstring &out, int &changed, int depth = 0)
    {
        if (!w || depth > 8 || out.size() > 8000)
            return;
        out += (w->get_fname() ? w->get_fname()->to_string() : L"?") + L":" + ClassName(w);
        // A RetainerBox draws its children into an image the size of ITSELF and shows that -
        // anything outside is lost whatever canvas the panel gets. That is what cut the owner
        // panel's rear doll (OwnerPanelRetainerBox). Retaining off: the children draw directly.
        if (ClassName(w) == L"RetainerBox")
        {
            const bool no = false;
            API::UFunction *fn = FindFn(w, L"SetRetainRendering");
            const std::wstring pn = fn ? FirstParamName(fn) : L"";
            uint8_t wasRetain = 1;
            if (auto *rp = FindProp(w->get_class(), L"bRetainRender"))
                if (rp->get_class() && rp->get_class()->get_name() == L"BoolProperty")
                    wasRetain = reinterpret_cast<API::FBoolProperty *>(rp)->get_value_from_object(w) ? 1 : 0;
            if (fn && !pn.empty() && Call(w, L"SetRetainRendering", {{pn.c_str(), &no, 1}}))
            {
                if (UndoNow)
                    UndoNow->push_back({w, 0, wasRetain});
                out += L"(retain OFF)";
                ++changed;
            }
            else
                out += L"(retain: SetRetainRendering failed)";
        }
        if (auto *p = FindProp(w->get_class(), L"Clipping"))
        {
            const uint8_t clip = *(reinterpret_cast<uint8_t *>(w) + p->get_offset());
            if (clip != 0)
            {
                out += L"(clip " + std::to_wstring(clip) + L")";
                if (UndoNow)
                    UndoNow->push_back({w, 1, clip});
                const uint8_t inherit = 0;
                if (!Call(w, L"SetClipping", {{L"InClipping", &inherit, 1}}))
                    *(reinterpret_cast<uint8_t *>(w) + p->get_offset()) = 0;
                ++changed;
            }
        }
        for (const wchar_t *ov : {L"WidthOverride", L"HeightOverride", L"MinDesiredWidth", L"MaxDesiredWidth"})
            if (auto *p = FindProp(w->get_class(), ov))
                if (p->get_class() && p->get_class()->get_name() == L"FloatProperty")
                {
                    const float v = *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(w) + p->get_offset());
                    if (v > 0.0f)
                        out += std::wstring(L"(") + ov + L" " + std::to_wstring((int)v) + L")";
                }
        API::UObject *root = nullptr;
        if (API::UObject *wt = ObjProp(w, L"WidgetTree"))
            root = ObjProp(wt, L"RootWidget");
        if (root)
        {
            out += L" { ";
            Unclip(root, out, changed, depth + 1);
            out += L" }";
            return;
        }
        int32_t n = 0;
        if (!ChildCountFn(w) || !CallRet(w, L"GetChildrenCount", {}, &n, sizeof(n)) || n <= 0)
            return;
        out += L" [ ";
        for (int32_t i = 0; i < n && i < 32; ++i)
        {
            API::UObject *c = nullptr;
            int32_t idx = i;
            if (CallRet(w, L"GetChildAt", {{L"Index", &idx, sizeof(idx)}}, &c, sizeof(c)) && c)
            {
                if (i)
                    out += L", ";
                Unclip(c, out, changed, depth + 1);
            }
        }
        out += L" ]";
    }

    // Panels have GetChildrenCount; asking anything else would log a "function not found".
    bool ChildCountFn(API::UObject *w)
    {
        for (auto *st = static_cast<API::UStruct *>(w->get_class()); st; st = st->get_super_struct())
            if (st->find_function(L"GetChildrenCount"))
                return true;
        return false;
    }

    static bool LooksLikeDialogue(const std::wstring &cls)
    {
        static const wchar_t *k[] = {L"Dialog", L"Subtitle", L"Coms", L"Comms", L"Transmission", L"Portrait", L"Speaker"};
        for (auto *w : k)
            if (cls.find(w) != std::wstring::npos)
                return true;
        return false;
    }

    // Logs HUD members whose visibility changes - a transmission showing up names its widget.
    void WatchHudMembers()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - MemberCheckMs < 1000)
            return;
        MemberCheckMs = now;
        for (auto &m : HudMembers)
        {
            auto *obj = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(Hud) + m.Off);
            if (!obj)
                continue;
            if (obj != m.Last)
            {
                m.Last    = obj;
                m.Visible = -1;
                m.Cls     = ClassName(obj);
            }
            bool vis = false;
            if (!FindFn(obj, L"IsVisible") || !CallRet(obj, L"IsVisible", {}, &vis, 1))
                continue;
            if (m.Visible >= 0 && (int8_t)vis != m.Visible)
                LogInfo("HUD member %ls (%ls) %ls", m.Name.c_str(), m.Cls.c_str(), vis ? L"SHOWN" : L"hidden");
            m.Visible = (int8_t)vis;
        }
    }

    void ScanHud()
    {
        Scanned = true;
        HudMembers.clear();
        DlgMemberOff[0] = DlgMemberOff[1] = -1;
        std::wstring memberList;
        for (int s = 0; s < VS_ELEMENT_COUNT; ++s)
            if (S[s].State != VHS_Hosted)
            {
                S[s].Widget    = nullptr;
                S[s].MemberOff = -1;
                S[s].Fails     = 0;
            }
        // bug sweep 23 (2026-10-18): a rescan of the same HUD keeps each frame's visibility from before we collapsed it -
        // the rebuilt list had "never collapsed" (0xFF) for frames we had collapsed, so they were never shown again when
        // their panel went back to the game.
        const std::vector<Frame> oldFrames = Frames;
        Frames.clear();
        int found = 0;
        for (auto *st = static_cast<API::UStruct *>(Hud->get_class()); st; st = st->get_super_struct())
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
            {
                if (!f->get_class() || f->get_class()->get_name() != L"ObjectProperty")
                    continue;
                auto *p   = reinterpret_cast<API::FProperty *>(f);
                auto *obj = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(Hud) + p->get_offset());
                if (!obj || !obj->get_class() || !obj->get_class()->get_fname())
                    continue;
                const std::wstring cn = obj->get_class()->get_fname()->to_string();
                bool used = false;
                for (int s = 0; s < VS_ELEMENT_COUNT && !used; ++s)
                    if (!S[s].Widget && cn == VanillaSlotClass(s))
                    {
                        S[s].Widget    = obj;
                        S[s].MemberOff = p->get_offset();
                        ++found;
                        used = true;
                    }
                // The decorative frames the elements sat in (HUD_WeaponList_BackgroundElement_C,
                // HUD_PaperDoll_BackgroundElement_C, HUD_MiniMap_BackgroundElement_C,
                // HUD_Compass_BackgroundElement_C).
                if (!used && cn.find(L"BackgroundElement") != std::wstring::npos)
                {
                    Frame fr{obj, p->get_offset()};
                    for (const Frame &o : oldFrames) // bug sweep 23
                        if (o.W == obj)
                            fr.Orig = o.Orig;
                    Frames.push_back(fr);
                    used = true;
                }
                const std::wstring pn = f->get_fname() ? f->get_fname()->to_string() : L"?";
                memberList += (memberList.empty() ? L"" : L", ") + pn + L":" + cn;
                if (used)
                    continue;
                // The mission dialogue lives inside the HUD: take it by its class name.
                if (LooksLikeDialogue(cn))
                {
                    const int d = (cn.find(L"Subtitle") != std::wstring::npos) ? 1 : 0;
                    if (DlgMemberOff[d] < 0)
                    {
                        DlgMemberOff[d] = p->get_offset();
                        LogInfo("dialogue candidate in the HUD: %ls (%ls) -> %ls", pn.c_str(), cn.c_str(),
                                VanillaSourceName(VS_Dialogue + d));
                    }
                    else
                        LogInfo("another dialogue-like HUD member left in place: %ls (%ls)", pn.c_str(), cn.c_str());
                    continue;
                }
                HudMembers.push_back({pn, cn, p->get_offset(), obj, -1});
            }
        S[VS_Overlay].Widget = Hud;
        LogInfo("HUD scan: %d of %d elements, %zu frames", found, (int)VS_ELEMENT_COUNT, Frames.size());
        LogInfo("HUD members: %ls", memberList.c_str());
        // The crosshair's own parts - which is the torso reticle, which the arms - for our own
        // crosshairs, drawn where the game places its reticles.
        if (auto *xh = HudMemberOfClass(L"HUD_CrossHair_C"))
        {
            std::wstring tree;
            DumpWidgetTree(xh, tree);
            LogInfo("crosshair widget tree: %ls", tree.c_str());
        }
        for (int s = 0; s < VS_ELEMENT_COUNT; ++s)
            if (!S[s].Widget)
                LogInfo("  not in this HUD: %ls", VanillaSourceName(s));
    }

    // Re-read the HUD_C property the element came through: the liveness test and the "has the HUD
    // replaced it" test in one. We are inside the HUD's own Tick, so the HUD is alive.
    bool StillCurrent(int s)
    {
        const SlotState &st = S[s];
        if (s == VS_Overlay)
            return Hud && st.Widget == Hud;
        if (s == VS_Markers)
            return st.Widget && st.Widget == MarkerRoot;
        if (s == VS_Dialogue || s == VS_Subtitles)
            return st.Widget && st.Widget == DlgWidget[s - VS_Dialogue];
        if (!Hud || !st.Widget || st.MemberOff < 0)
            return false;
        return *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(Hud) + st.MemberOff) == st.Widget;
    }

    // ==========================================================================================
    // PANEL SWITCHES (vanillahud-105, VHUD v20). GlobalSettings.ini [hud] says, per panel, whether
    // HUDDisplay takes it (1, the default) or the game keeps it on its own HUD (0) - VhudHudKey names
    // the keys. Read live, the way LanceControl reads [lance]: the file's path from
    // GlobalSettingsPath.txt (every 10 s), the file itself only when its write time changes (checked
    // once a second). No file: every panel to HUDDisplay, as before.
    //
    // The rest of the HUD (crosshair, lock ring, hit arrows, prompts) and the world markers are no
    // longer captured at all: they stay in the game, and V-POD draws its own crosshair and lock.
    // ==========================================================================================
    bool         HudWant[VS_COUNT];
    bool         HudWantInit = false;
    ULONGLONG    HsMs = 0, HsPathMs = 0, HsStamp = 0;
    std::wstring HsIni, HsLogged;

    bool WantHosted(int s) const
    {
        const wchar_t *k = VhudHudKey(s);
        return !k[0] || !HudWantInit || HudWant[s];
    }

    void HudSettingsTick()
    {
        if (!HudWantInit)
        {
            for (bool &b : HudWant)
                b = true;
            HudWantInit = true;
        }
        const ULONGLONG now = GetTickCount64();
        if (HsMs && now - HsMs < 1000)
            return;
        HsMs = now;
        if (HsIni.empty() || now - HsPathMs > 10000)
        {
            HsPathMs = now;
            const std::wstring was = HsIni;
            HsIni.clear();
            wchar_t ad[MAX_PATH] = L"";
            const DWORD n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
            if (n && n < MAX_PATH)
            {
                const std::wstring f = std::wstring(ad) + L"\\UnrealVRMod\\MechWarrior-Win64-Shipping\\GlobalSettingsPath.txt";
                HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    char b[2048];
                    DWORD got = 0;
                    if (ReadFile(h, b, sizeof(b) - 1, &got, nullptr) && got)
                    {
                        const int k = MultiByteToWideChar(CP_UTF8, 0, b, (int)got, nullptr, 0);
                        if (k > 0)
                        {
                            HsIni.resize((size_t)k);
                            MultiByteToWideChar(CP_UTF8, 0, b, (int)got, &HsIni[0], k);
                            while (!HsIni.empty() && (HsIni.back() == L'\n' || HsIni.back() == L'\r' || HsIni.back() == L' '))
                                HsIni.pop_back();
                        }
                    }
                    CloseHandle(h);
                }
            }
            if (HsIni != was)
                HsStamp = 0;
            if (HsIni != HsLogged)
            {
                HsLogged = HsIni;
                LogInfo("panel switches: %ls", HsIni.empty() ? L"GlobalSettingsPath.txt not found - every panel to HUDDisplay"
                                                             : (L"from " + HsIni).c_str());
            }
        }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (HsIni.empty() || !GetFileAttributesExW(HsIni.c_str(), GetFileExInfoStandard, &fa))
        {
            for (bool &b : HudWant)
                b = true;
            HsStamp = 0;
            return;
        }
        const ULONGLONG st = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        if (st == HsStamp)
            return;
        HsStamp = st;
        std::wstring inGame;
        for (int s = 0; s < VS_COUNT; ++s)
        {
            const wchar_t *k = VhudHudKey(s);
            HudWant[s] = !k[0] || GetPrivateProfileIntW(L"hud", k, 1, HsIni.c_str()) != 0;
            if (!HudWant[s] && s != VS_Subtitles)
                inGame += (inGame.empty() ? L"" : L", ") + std::wstring(k);
        }
        LogInfo("panel switches: left in the game: %ls", inGame.empty() ? L"none" : inGame.c_str());
        std::wstring hidden;
        for (int i = 0; i < kPieceCount; ++i)
        {
            PieceShown[i] = GetPrivateProfileIntW(L"hud", kPieces[i].Key, 1, HsIni.c_str()) != 0;
            if (!PieceShown[i])
                hidden += (hidden.empty() ? L"" : L", ") + std::wstring(kPieces[i].Key);
        }
        LogInfo("game pieces hidden: %ls", hidden.empty() ? L"none" : hidden.c_str());
    }

    // ==========================================================================================
    // GAME PIECES (vanillahud-105b). The parts of the game's HUD nothing captures any more - shown
    // or hidden ON THE GAME'S SCREEN, per GlobalSettings [hud] game_* (1 shown, the default). Hidden
    // with SetRenderOpacity 0, re-asserted every HUD tick (the game re-shows things with
    // SetVisibility, which opacity survives), and put back to 1 once when switched on again.
    // Found by the HUD_C property names in the "HUD members" log line, except:
    //   lock ring  - WeaponTargetLock_C (UI/Mech/Components): created by HUD_CrossHair at run time,
    //                so found as a live widget by class name. Hiding the crosshair hides it too.
    //                NOTE: it is also what plays the game's own lock sounds (Missile_Acquiring /
    //                Missile_Locked / Missile_Nolock) - hiding it is not expected to silence them.
    //   markers    - every MWMarkerWidget (objective / nav markers in the world)
    //   target box - every MWTargetBoxWidget (the game's box on the targeted unit)
    // ==========================================================================================
    struct GamePiece
    {
        const wchar_t *Key;
        const wchar_t *Members[8];
        int            Special; // 0 HUD_C members, 1 lock ring, 2 markers, 3 target box
    };
    static constexpr int kPieceCount = 11;
    static constexpr GamePiece kPieces[kPieceCount] = {
        {L"game_crosshair", {L"HUD_CrossHair"}, 0},
        {L"game_lock_ring", {}, 1},
        {L"game_hit_arrows", {L"HUD_DamageIndicator"}, 0},
        {L"game_incoming", {L"IncomingArrow_0", L"IncomingArrow_45", L"IncomingArrow_90", L"IncomingArrow_135",
                            L"IncomingArrow_180", L"IncomingArrow_225", L"IncomingArrow_270", L"IncomingArrow_315"}, 0},
        {L"game_prompts", {L"HUD_InteractorWidget"}, 0},
        {L"game_dpad", {L"Dpad_MainPanel"}, 0},
        {L"game_detection", {L"AlertOverlay_DetectionStatus", L"MechSpottedWrapper"}, 0},
        {L"game_airstrike", {L"HUD_AirstrikeIndicator", L"HUD_AirStrikeScope", L"AIRSTRIKEALERT_1", L"AIRSTRIKEALERT_2"}, 0},
        {L"game_markers", {}, 2},
        {L"game_target_box", {}, 3},
        {L"game_ecm", {L"ECM", L"ECMName"}, 0}, // vanillahud-106
    };
    bool          PieceShown[kPieceCount] = {true, true, true, true, true, true, true, true, true, true, true};
    bool          PieceHidden[kPieceCount] = {}; // what we last did: hidden by us
    bool          PieceFirst[kPieceCount] = {};  // vanillahud-119: a new HUD's first pass (born from the template)
    API::UClass  *LockRingClass = nullptr;
    ULONGLONG     LockRingFindMs = 0;
    int           PieceLogs = 0;

    void PieceWidgets(API::UObject *pc, const GamePiece &g, std::vector<API::UObject *> &out)
    {
        out.clear();
        if (g.Special == 0)
        {
            if (!Hud || !Hud->get_class())
                return;
            for (const wchar_t *m : g.Members)
            {
                if (!m)
                    break;
                API::FProperty *p = FindProp(Hud->get_class(), m);
                if (!p || !p->get_class() || p->get_class()->get_name() != L"ObjectProperty")
                    continue;
                if (auto *w = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(Hud) + p->get_offset()))
                    out.push_back(w);
            }
        }
        else if (g.Special == 1)
        {
            if (FindLockRingClass(pc))
                FirstWidgetOf(pc, LockRingClass, &out);
        }
        else if (g.Special == 2)
            FirstWidgetOf(pc, MarkerClass, &out);
        else if (g.Special == 3)
            FirstWidgetOf(pc, TargetBoxClass, &out);
    }

    bool FindLockRingClass(API::UObject *pc)
    {
        const ULONGLONG now = GetTickCount64();
        if (!LockRingClass && UserWidgetClass && now - LockRingFindMs >= 3000)
        {
            LockRingFindMs = now;
            std::vector<API::UObject *> every;
            FirstWidgetOf(pc, UserWidgetClass, &every, false);
            for (auto *w : every)
                if (ClassName(w) == L"WeaponTargetLock_C")
                {
                    LockRingClass = static_cast<API::UClass *>(w->get_class());
                    LogInfo("game pieces: lock ring widget found (WeaponTargetLock_C)");
                    break;
                }
        }
        return LockRingClass != nullptr;
    }

    // ==========================================================================================
    // HIDDEN FROM BIRTH - AT THE TEMPLATE (vanillahud-118 markers / target box, vanillahud-119 the HUD's own
    // panels and pieces + the early driver; user 2026-10-18: "no chance of it flashing", "hidden immediately").
    // Every UMG widget is copied from a template the moment it is created. What we hide is hidden IN THE
    // TEMPLATE, so the copy is born hidden and never drawn - not even on the first frame of a mission.
    //   markers / target box - each MWMarkerWidget / MWTargetBoxWidget blueprint class owns a template tree;
    //               its root (an unnamed CanvasPanel no blueprint code refers to - PIECE PROBE 2) is
    //               COLLAPSED. Minimap icons (instances with a UMG parent, LocalMechIcon_C...) are shown again
    //               one by one; classes are "world" by their path (/Objects/Targets/) or by their first instance.
    //   HUD panels  - HUD_C's own template tree: each panel HUDDisplay takes (panel switch ticked), its frame,
    //               the transmission panels, and each IN GAME piece switched off gets RENDER OPACITY 0 (opacity,
    //               not visibility: the layout - which hosting measures, and the crosshair parts V-POD reads -
    //               stays exactly as it was; the game re-shows things with SetVisibility, which opacity survives).
    //               A hosted panel is made opaque again inside HUDDisplay's capture; a panel the game keeps, or one
    //               we fail to host, is made opaque again on the game's screen.
    //   lock ring   - WeaponTargetLock_C is created at run time: its class template root gets opacity 0.
    //   early driver- this has to run before the HUD and the markers exist, and the HUD hook only comes seconds
    //               after the mech. The SDK's engine tick is not delivered here (see LidarExport), so a few UMG
    //               functions the game's blueprints call all the time, on the game thread (SetVisibility,
    //               SetRenderOpacity, SetText...), are detoured: each call runs the original, then
    //               TemplatesTick - which costs one timer read when there is nothing new.
    //   finding     - the object table: every new slot (the high-water mark, up to 20000 objects per 5 ms) and a
    //               slow cycle through the whole table (3000 objects per 20 ms, for reused slots). A class found
    //               is checked against the switches at once. Entries are dropped when their class goes.
    //   switches    - GlobalSettings [hud]: the panel switches and game_* (read live, as before). A change puts the
    //               templates right and fixes the live copies.
    // ==========================================================================================
    struct TplClass
    {
        API::UClass  *Cls;
        int32_t       Index; // its object-table slot: the liveness check
        uint8_t       Orig;  // the template root's own visibility
        int           Piece; // index into kPieces (markers or target box)
        bool          World; // collapsed when hidden; false = minimap / panel class, never touched
    };
    struct TplOrigByName
    {
        std::wstring Name;
        uint8_t      Orig;
    };
    struct HudTpl // one widget of HUD_C's template tree, or the lock ring's template root
    {
        API::UObject *W;
        int           Kind; // 0 panel / transmission (Idx = slot), 1 frame (Idx = slot or -1), 2 piece (Idx = kPieces), 3 lock ring root
        int           Idx;
        float         Orig;
        int8_t        Applied; // -1 never, 0 shown, 1 hidden
        int           Order;   // its HUD_C property's place (ScanHud takes the first of a class), -1 none
    };
    std::vector<TplClass>      Tpl;
    std::vector<TplOrigByName> TplOrig;          // the session: survives a class being loaded again
    bool                       TplHidden[kPieceCount] = {};
    bool                       TplDirty[kPieceCount] = {}; // a class registered while hidden: fix its live copies
    std::unordered_set<API::UObject *> TplSeen[kPieceCount];
    ULONGLONG                  TplSweepMs = 0;
    int                        TplLogs = 0;
    ULONGLONG                  TplCheckMs = 0; // one "game pieces check" line 30 s after a piece is hidden
    // the object table walk
    int32_t       TplHigh = 0, TplCycle = 0;
    LARGE_INTEGER TplHighT{}, TplCycleT{}, TplQf{};
    API::UClass  *HudNativeClass = nullptr; // MWMechHudWidget
    // HUD_C's template
    API::UClass        *HudTplClass = nullptr;
    int32_t             HudTplIndex = -1;
    std::vector<HudTpl> HudTpls;
    std::vector<std::wstring> HudPropNames; // HUD_C's object properties, in ScanHud's order
    API::UClass        *LockTplClass = nullptr;
    int32_t             LockTplIndex = -1;
    bool                InTpl = false, InGameTick = false;
    ULONGLONG           TplApplyMs = 0;

    void ForgetTemplates()
    {
        // The templates and the class list stay (classes outlive a mission, and a dead one is dropped by
        // its liveness check); only the per-mission bookkeeping goes.
        for (auto &s : TplSeen)
            s.clear();
        TplSweepMs = 0;
        TplCheckMs = 0;
    }

    static int32_t ObjIndex(API::UObject *o) { return *reinterpret_cast<int32_t *>(reinterpret_cast<uint8_t *>(o) + 12); }
    static bool Alive(API::UObject *o, int32_t index)
    {
        auto *arr = API::FUObjectArray::get();
        return o && arr && index >= 0 && index < arr->get_object_count() && arr->get_object(index) == o;
    }
    // vanillahud-120 (crash review): an object we are about to read through or write into - still in its object-table
    // slot and not being destroyed. A level unload frees a class and its template tree over several frames, in no set
    // order: the class can still be listed while its tree is gone.
    static bool Valid(API::UObject *o)
    {
        if (!o || !Alive(o, ObjIndex(o)))
            return false;
        const uint32_t flags = *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(o) + 8); // UObjectBase::ObjectFlags
        return (flags & (0x00008000u | 0x00010000u)) == 0; // RF_BeginDestroyed | RF_FinishDestroyed
    }

    static API::UObject *TreeRootOf(API::UObject *owner)
    {
        if (!owner)
            return nullptr;
        auto *tp = owner->get_property_data<API::UObject *>(L"WidgetTree");
        API::UObject *tree = tp ? *tp : nullptr;
        if (!Valid(tree)) // vanillahud-120
            return nullptr;
        auto *rp = tree->get_property_data<API::UObject *>(L"RootWidget");
        API::UObject *root = rp ? *rp : nullptr;
        return Valid(root) ? root : nullptr; // vanillahud-120
    }

    int PieceOfClass(API::UStruct *c) const
    {
        for (int d = 0; c && d < 24; ++d, c = c->get_super_struct())
        {
            if (MarkerClass && c == static_cast<API::UStruct *>(MarkerClass))
                return 8; // game_markers
            if (TargetBoxClass && c == static_cast<API::UStruct *>(TargetBoxClass))
                return 9; // game_target_box
        }
        return -1;
    }
    bool IsHudClass(API::UStruct *c) const
    {
        if (!HudNativeClass)
            return false;
        for (int d = 0; c && d < 24; ++d, c = c->get_super_struct())
            if (c == static_cast<API::UStruct *>(HudNativeClass))
                return true;
        return false;
    }

    TplClass *FindTpl(API::UClass *c)
    {
        for (auto &t : Tpl)
            if (t.Cls == c)
                return &t;
        return nullptr;
    }

    bool PieceHide(int i) const
    {
        const bool eiUp = kPieces[i].Special == 3 && Block && (Block->EiVisionOn || Block->EiVisWant);
        return !PieceShown[i] || eiUp;
    }

    void SetTplRoot(const TplClass &t, bool hide)
    {
        if (!Alive(reinterpret_cast<API::UObject *>(t.Cls), t.Index) || !Valid(reinterpret_cast<API::UObject *>(t.Cls)))
            return;
        API::UObject *root = TreeRootOf(reinterpret_cast<API::UObject *>(t.Cls));
        if (!root)
            return;
        if (auto *vp = root->get_property_data<uint8_t>(L"Visibility"))
            *vp = hide ? kCollapsed : t.Orig;
    }

    // A marker / target-box class seen for the first time. world: -1 = decide by the path (or not yet, when the
    // path says nothing), 0/1 = decided by the caller (its first instance's parent).
    TplClass *RegisterTpl(API::UClass *c, int piece, int world)
    {
        if (auto *t = FindTpl(c))
            return t;
        auto *co = reinterpret_cast<API::UObject *>(c);
        const std::wstring full = co->get_full_name();
        const bool onPath = full.find(L"/Objects/Targets/") != std::wstring::npos;
        if (world < 0 && !onPath)
            return nullptr; // not a known marker folder: decided by its first instance
        TplClass t{c, ObjIndex(co), 4, piece, world >= 0 ? world != 0 : onPath};
        API::UObject *root = TreeRootOf(co);
        if (root)
        {
            auto *vp = root->get_property_data<uint8_t>(L"Visibility");
            auto it = std::find_if(TplOrig.begin(), TplOrig.end(), [&](const TplOrigByName &o) { return o.Name == full; });
            if (it != TplOrig.end())
                t.Orig = it->Orig;
            else
            {
                if (vp && *vp != kCollapsed)
                    t.Orig = *vp;
                TplOrig.push_back({full, t.Orig});
            }
        }
        Tpl.push_back(t);
        if (TplLogs++ < 80)
            LogInfo("game pieces: %ls class %ls - %ls%ls", kPieces[piece].Key, co->get_fname() ? co->get_fname()->to_string().c_str() : L"?",
                    t.World ? L"world (hidden at the template)" : L"in a panel (left alone)", root ? L"" : L", no template tree of its own");
        if (t.World && TplHidden[piece])
        {
            SetTplRoot(Tpl.back(), true);
            TplDirty[piece] = true;
        }
        return &Tpl.back();
    }

    // One live marker / box: collapsed or shown at its root, per its class and where it sits.
    void FixInstance(API::UObject *w, int piece, bool hide)
    {
        auto *c = static_cast<API::UClass *>(w->get_class());
        API::UObject *parent = nullptr;
        CallRet(w, L"GetParent", {}, &parent, sizeof(parent));
        TplClass *t = FindTpl(c);
        if (!t)
            t = RegisterTpl(c, piece, parent ? 0 : 1);
        if (!t || !t->World)
            return;
        API::UObject *root = TreeRootOf(w);
        if (!root)
            return;
        const uint8_t want = (hide && !parent) ? kCollapsed : t->Orig; // in a panel (minimap): always shown
        uint8_t vis = 0xFF;
        if (CallRet(root, L"GetVisibility", {}, &vis, 1) && vis == want)
            return;
        uint8_t v = want;
        Call(root, L"SetVisibility", {{L"InVisibility", &v, 1}});
    }

    // ---- HUD_C's template ---------------------------------------------------------------------
    static int FrameSlot(const std::wstring &cn)
    {
        if (cn.find(L"WeaponList") != std::wstring::npos) return VS_Weapons;
        if (cn.find(L"PaperDoll") != std::wstring::npos)  return VS_Owner;
        if (cn.find(L"MiniMap") != std::wstring::npos)    return VS_Map;
        if (cn.find(L"Compass") != std::wstring::npos)    return VS_Compass;
        return -1;
    }

    void HudTplVisit(API::UObject *w, int depth, std::vector<API::UObject *> &seen)
    {
        if (!w || depth > 14 || seen.size() > 3000 || std::find(seen.begin(), seen.end(), w) != seen.end())
            return;
        seen.push_back(w);
        const std::wstring name = w->get_fname() ? w->get_fname()->to_string() : L"";
        const std::wstring cn   = ClassName(w);
        int kind = -1, idx = -1;
        for (int s = 0; s < VS_ELEMENT_COUNT && kind < 0; ++s)
            if (cn == VanillaSlotClass(s))
                kind = 0, idx = s;
        if (kind < 0 && cn.find(L"BackgroundElement") != std::wstring::npos)
            kind = 1, idx = FrameSlot(cn);
        if (kind < 0 && LooksLikeDialogue(cn))
            kind = 0, idx = cn.find(L"Subtitle") != std::wstring::npos ? VS_Subtitles : VS_Dialogue;
        for (int i = 0; i < kPieceCount && kind < 0; ++i)
            if (kPieces[i].Special == 0)
                for (const wchar_t *m : kPieces[i].Members)
                {
                    if (!m)
                        break;
                    if (name == m)
                    {
                        kind = 2, idx = i;
                        break;
                    }
                }
        // Panels, frames and the transmission only as ScanHud sees them: through a HUD_C property of that name.
        int order = -1;
        for (size_t k = 0; k < HudPropNames.size(); ++k)
            if (HudPropNames[k] == name)
            {
                order = (int)k;
                break;
            }
        if ((kind == 0 || kind == 1) && order < 0)
            kind = -1;
        if (kind >= 0)
        {
            if (auto *op = w->get_property_data<float>(L"RenderOpacity"))
                HudTpls.push_back({w, kind, idx, *op > 0.001f ? *op : 1.0f, -1, order});
            if (kind != 2)
                return; // a panel / frame: what is inside goes with it
        }
        int32_t n = 0;
        if (CallRet(w, L"GetChildrenCount", {}, &n, sizeof(n)) && n > 0)
            for (int32_t i = 0; i < n && i < 256; ++i)
            {
                API::UObject *c = nullptr;
                CallRet(w, L"GetChildAt", {{L"Index", &i, sizeof(i)}}, &c, sizeof(c));
                HudTplVisit(c, depth + 1, seen);
            }
        // A user widget's named slots (the paperdoll frame holds the own paperdoll this way).
        if (auto *nb = w->get_property_data<uint8_t>(L"NamedSlotBindings"))
        {
            uint8_t *data = *reinterpret_cast<uint8_t **>(nb);
            const int32_t cnt = *reinterpret_cast<int32_t *>(nb + 8);
            for (int32_t i = 0; data && i < cnt && i < 64; ++i)
                HudTplVisit(*reinterpret_cast<API::UObject **>(data + i * 16 + 8), depth + 1, seen);
        }
    }

    void RegisterHudTpl(API::UClass *c)
    {
        auto *co = reinterpret_cast<API::UObject *>(c);
        HudTplClass = c;
        HudTplIndex = ObjIndex(co);
        // Opacity already 0 from an earlier load of the same class is not its own: keep what we noted.
        std::vector<HudTpl> old;
        old.swap(HudTpls);
        HudPropNames.clear();
        for (auto *st = static_cast<API::UStruct *>(c); st; st = st->get_super_struct())
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
                if (f->get_class() && f->get_class()->get_name() == L"ObjectProperty" && f->get_fname())
                    HudPropNames.push_back(f->get_fname()->to_string());
        std::vector<API::UObject *> seen;
        HudTplVisit(TreeRootOf(co), 0, seen);
        // One widget per panel / transmission slot - the first by property order, as ScanHud takes it.
        {
            std::vector<HudTpl> kept;
            for (const auto &e : HudTpls)
            {
                bool later = false;
                for (const auto &o : HudTpls)
                    if (e.Kind == 0 && o.Kind == 0 && o.Idx == e.Idx && o.Order < e.Order)
                        later = true;
                if (!later)
                    kept.push_back(e);
            }
            HudTpls.swap(kept);
        }
        for (const auto &o : old)
            if (o.Kind == 3)
                HudTpls.push_back(o); // the lock ring's root is not part of HUD_C's tree
        int counts[4] = {};
        for (auto &e : HudTpls)
        {
            ++counts[e.Kind];
            for (const auto &o : old)
                if (o.W == e.W)
                    e.Orig = o.Orig;
        }
        LogInfo("HUD template (%ls): %d panels, %d frames, %d pieces found in %zu widgets",
                co->get_fname() ? co->get_fname()->to_string().c_str() : L"?", counts[0], counts[1], counts[2], seen.size());
        ApplyHudTpls(true);
        // The game thread is ours here: the Tick hook goes in now, not seconds later from the render thread.
        if (!HookedFn && !HookGaveUp)
            if (auto *fn = c->find_function(L"Tick"))
            {
                auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
                if (*slot != &OnHudTick)
                {
                    TickOrig = *slot;
                    *slot    = &OnHudTick;
                    HookedFn = fn;
                    LogInfo("hooked HUD_C::Tick (%p) from the game thread, as the HUD class loaded", (void *)fn);
                }
            }
    }

    bool HudTplHide(const HudTpl &e) const
    {
        switch (e.Kind)
        {
        case 0: return WantHosted(e.Idx);
        case 1: return e.Idx < 0 || WantHosted(e.Idx); // a frame with no panel of its own is always collapsed (CollapseFrames)
        case 2: return !PieceShown[e.Idx];
        case 3: return !PieceShown[1];
        }
        return false;
    }

    void ApplyHudTpls(bool force)
    {
        if (HudTplClass && !(Alive(reinterpret_cast<API::UObject *>(HudTplClass), HudTplIndex) &&
                             Valid(reinterpret_cast<API::UObject *>(HudTplClass))))
        {
            HudTpls.erase(std::remove_if(HudTpls.begin(), HudTpls.end(), [](const HudTpl &e) { return e.Kind != 3; }), HudTpls.end());
            HudTplClass = nullptr;
        }
        for (auto &e : HudTpls)
        {
            const bool hide = HudTplHide(e);
            if (!force && e.Applied == (hide ? 1 : 0))
                continue;
            if (e.Kind == 3 && !(Alive(reinterpret_cast<API::UObject *>(LockTplClass), LockTplIndex) &&
                                 Valid(reinterpret_cast<API::UObject *>(LockTplClass))))
                continue;
            if (!Valid(e.W)) // vanillahud-120: the template widget itself
                continue;
            if (auto *op = e.W->get_property_data<float>(L"RenderOpacity"))
                *op = hide ? 0.0f : e.Orig;
            e.Applied = hide ? 1 : 0;
        }
    }

    // Instance side: a panel the game shows (kept in game, or not hosted) or one inside HUDDisplay's capture.
    void Opaque(API::UObject *w)
    {
        if (!w)
            return;
        float cur = -1.0f;
        if (CallRet(w, L"GetRenderOpacity", {}, &cur, sizeof(cur)) && cur >= 0.999f)
            return;
        const float one = 1.0f;
        Call(w, L"SetRenderOpacity", {{L"InOpacity", &one, sizeof(one)}});
    }

    // ---- the object table ---------------------------------------------------------------------
    void TplLook(API::UObject *o, API::UClass *ucls)
    {
        if (!o || !o->is_a(ucls))
            return;
        auto *c  = static_cast<API::UClass *>(reinterpret_cast<API::UStruct *>(o));
        auto *st = static_cast<API::UStruct *>(c);
        const int piece = PieceOfClass(st);
        if (piece >= 0)
        {
            if (c != MarkerClass && c != TargetBoxClass)
                RegisterTpl(c, piece, -1);
            return;
        }
        if (c != HudTplClass && IsHudClass(st) && o->get_fname() && o->get_fname()->to_string() == L"HUD_C")
        {
            RegisterHudTpl(c);
            return;
        }
        if (c != LockTplClass && o->get_fname() && o->get_fname()->to_string() == L"WeaponTargetLock_C")
            if (API::UObject *root = TreeRootOf(o))
                if (auto *op = root->get_property_data<float>(L"RenderOpacity"))
                {
                    // the old entry (a previous load) goes
                    HudTpls.erase(std::remove_if(HudTpls.begin(), HudTpls.end(), [](const HudTpl &e) { return e.Kind == 3; }), HudTpls.end());
                    LockTplClass = c;
                    LockTplIndex = ObjIndex(o);
                    HudTpls.push_back({root, 3, 1, *op > 0.001f ? *op : 1.0f, -1, -1});
                    ApplyHudTpls(false);
                    LogInfo("lock ring template found (WeaponTargetLock_C)");
                }
    }

    void TplWalk()
    {
        auto *arr = API::FUObjectArray::get();
        auto *ucls = API::UClass::static_class();
        if (!arr || !ucls)
            return;
        if (!TplQf.QuadPart)
            QueryPerformanceFrequency(&TplQf);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        const int32_t n = arr->get_object_count();
        if (TplHigh > n)
            TplHigh = n;
        if (TplHigh < n && (t.QuadPart - TplHighT.QuadPart) * 1000 >= TplQf.QuadPart * 5)
        {
            TplHighT = t;
            const int32_t end = (std::min)(n, TplHigh + 20000);
            for (int32_t i = TplHigh; i < end; ++i)
                TplLook(arr->get_object(i), ucls);
            TplHigh = end;
        }
        if ((t.QuadPart - TplCycleT.QuadPart) * 1000 >= TplQf.QuadPart * 20)
        {
            TplCycleT = t;
            if (TplCycle >= TplHigh)
            {
                TplCycle = 0;
                // a class that went (level change) is dropped once per cycle
                Tpl.erase(std::remove_if(Tpl.begin(), Tpl.end(),
                                         [](const TplClass &x) {
                                             return !Alive(reinterpret_cast<API::UObject *>(x.Cls), x.Index) ||
                                                    !Valid(reinterpret_cast<API::UObject *>(x.Cls));
                                         }),
                          Tpl.end());
            }
            const int32_t end = (std::min)(TplHigh, TplCycle + 3000);
            for (int32_t i = TplCycle; i < end; ++i)
                TplLook(arr->get_object(i), ucls);
            TplCycle = end;
        }
    }

    // ---- the driver -----------------------------------------------------------------------------
    // From the early detours (any time, before there is a HUD) and from the HUD tick.
    void TemplatesTick()
    {
        if (InTpl)
            return;
        InTpl = true;
        if (!MarkerClass && !ClassesResolved)
            ResolveClasses();
        if (!HudNativeClass)
            HudNativeClass = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWMechHudWidget");
        TplWalk();
        const ULONGLONG now = GetTickCount64();
        if (now - TplApplyMs >= 100)
        {
            TplApplyMs = now;
            HudSettingsTick(); // the switches, read live (throttled inside)
            ApplyHudTpls(false);
            // vanillahud-120: live markers are touched only while a mech is piloted - never while a level is torn
            // down or loaded (the templates, which need no world, are kept right all the time).
            auto *pc   = API::get()->get_player_controller(0);
            auto *pawn = API::get()->get_local_pawn(0);
            if (!pc || !pawn || !pawn->get_property_data<void *>(L"MechMeshComponent"))
                pc = nullptr;
            std::vector<API::UObject *> ws;
            for (int i = 8; i <= 9; ++i)
            {
                const bool hide = PieceHide(i);
                if (hide != TplHidden[i])
                {
                    TplHidden[i] = hide;
                    for (auto &t : Tpl)
                        if (t.Piece == i && t.World)
                            SetTplRoot(t, hide);
                    TplDirty[i] = true;
                    if (PieceLogs++ < 40)
                        LogInfo("game pieces: %ls %ls (templates %ls)", kPieces[i].Key, hide ? L"hidden" : L"shown again",
                                hide ? L"collapsed" : L"restored");
                }
                if (TplDirty[i] && pc)
                {
                    TplDirty[i] = false;
                    PieceWidgets(pc, kPieces[i], ws);
                    for (auto *w : ws)
                        FixInstance(w, i, hide);
                    TplSeen[i].clear();
                    TplSeen[i].insert(ws.begin(), ws.end());
                }
            }
            if (pc && (TplHidden[8] || TplHidden[9]) && now - TplSweepMs >= 500)
            {
                TplSweepMs = now;
                for (int i = 8; i <= 9; ++i)
                {
                    if (!TplHidden[i])
                        continue;
                    PieceWidgets(pc, kPieces[i], ws);
                    std::unordered_set<API::UObject *> seen(ws.begin(), ws.end());
                    for (auto *w : ws)
                        if (!TplSeen[i].count(w))
                            FixInstance(w, i, true);
                    TplSeen[i].swap(seen);
                }
                TplCheck(pc, now);
            }
        }
        InTpl = false;
    }

    // One line per mission: what the hidden markers / boxes look like (collapsed / shown / in a panel).
    void TplCheck(API::UObject *pc, ULONGLONG now)
    {
        if (!Hud || TplCheckMs == 1)
            return;
        if (!TplCheckMs)
        {
            TplCheckMs = now;
            return;
        }
        if (now - TplCheckMs < 30000)
            return;
        TplCheckMs = 1;
        std::vector<API::UObject *> ws;
        for (int i = 8; i <= 9; ++i)
        {
            if (!TplHidden[i])
                continue;
            PieceWidgets(pc, kPieces[i], ws);
            int coll = 0, shown = 0, panel = 0, other = 0;
            for (auto *w : ws)
            {
                API::UObject *parent = nullptr;
                CallRet(w, L"GetParent", {}, &parent, sizeof(parent));
                TplClass *t = FindTpl(static_cast<API::UClass *>(w->get_class()));
                API::UObject *root = TreeRootOf(w);
                uint8_t vis = 0xFF;
                if (root)
                    CallRet(root, L"GetVisibility", {}, &vis, 1);
                if (parent)
                    ++panel;
                else if (!t || !t->World || !root)
                    ++other;
                else if (vis == kCollapsed)
                    ++coll;
                else
                    ++shown;
            }
            LogInfo("game pieces check: %ls - %zu live: %d collapsed, %d NOT collapsed, %d in a panel (shown), %d not handled",
                    kPieces[i].Key, ws.size(), coll, shown, panel, other);
        }
    }

    // The early detours: UMG functions blueprints call all the time, on the game thread.
    static constexpr int kEarlyCount = 6;
    inline static BP_FUNC EarlyOrig[kEarlyCount] = {};
    bool EarlyInstalled = false;
    template <int N> static void *EarlyDetour(API::UObject *self, FFrame *frame, void *const result)
    {
        void *r = EarlyOrig[N](self, frame, result); // vanillahud-120: always set before this detour is reachable
        if (Instance && !Instance->InGameTick)
            Instance->TemplatesTick();
        return r;
    }
    void InstallEarly()
    {
        if (EarlyInstalled)
            return;
        EarlyInstalled = true;
        static const wchar_t *kFns[kEarlyCount] = {
            L"Function /Script/UMG.Widget.SetVisibility",      L"Function /Script/UMG.Widget.SetRenderOpacity",
            L"Function /Script/UMG.TextBlock.SetText",         L"Function /Script/UMG.Widget.SetRenderTranslation",
            L"Function /Script/UMG.Image.SetColorAndOpacity",  L"Function /Script/UMG.CanvasPanelSlot.SetPosition"};
        static const BP_FUNC kDet[kEarlyCount] = {&EarlyDetour<0>, &EarlyDetour<1>, &EarlyDetour<2>,
                                                  &EarlyDetour<3>, &EarlyDetour<4>, &EarlyDetour<5>};
        std::wstring got;
        for (int k = 0; k < kEarlyCount; ++k)
        {
            auto *fn = API::get()->find_uobject<API::UFunction>(kFns[k]);
            if (!fn)
                continue;
            auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
            if (!*slot || *slot == kDet[k])
                continue;
            EarlyOrig[k] = *slot;
            std::atomic_thread_fence(std::memory_order_seq_cst); // vanillahud-120: the original is saved before the detour is seen
            *reinterpret_cast<BP_FUNC volatile *>(slot) = kDet[k];
            got += (got.empty() ? L"" : L", ") + std::wstring(wcsrchr(kFns[k], L'.') + 1);
        }
        LogInfo("early driver: detoured %ls", got.empty() ? L"NOTHING - templates only from the HUD tick" : got.c_str());
    }

    void ApplyGamePieces(API::UObject *pc)
    {
        (void)pc;
        TemplatesTick(); // vanillahud-118/119: world markers, target box, HUD templates
        std::vector<API::UObject *> ws;
        for (int i = 0; i < kPieceCount; ++i)
        {
            if (kPieces[i].Special >= 2)
                continue; // vanillahud-118: hidden at the template (above)
            // vanillahud-113: the game's target box also hides while Enhanced Imaging is up (on, or its
            // closing wave still running) - EI draws its own bracket and the two doubled up. Off again: back.
            const bool eiUp = kPieces[i].Special == 3 && Block && (Block->EiVisionOn || Block->EiVisWant);
            const bool hide = !PieceShown[i] || eiUp;
            if (!hide && !PieceHidden[i])
                continue; // shown and never touched: nothing to do
            PieceWidgets(pc, kPieces[i], ws);
            const float op = hide ? 0.0f : 1.0f;
            for (auto *w : ws)
            {
                float cur = -1.0f;
                if (!(CallRet(w, L"GetRenderOpacity", {}, &cur, sizeof(cur)) && cur == op))
                    Call(w, L"SetRenderOpacity", {{L"InOpacity", &op, sizeof(op)}});
                if (!hide && kPieces[i].Special == 1)
                    Opaque(TreeRootOf(w)); // vanillahud-119: born with its template root at opacity 0
            }
            if (PieceFirst[i])
                PieceFirst[i] = false; // vanillahud-119: the first pass of a HUD only makes sure; not a change
            else if (hide != PieceHidden[i] && PieceLogs++ < 40)
                LogInfo("game pieces: %ls %ls (%zu widget(s))", kPieces[i].Key, hide ? L"hidden" : L"shown again", ws.size());
            PieceHidden[i] = hide;
        }
    }

    // A hosted panel back onto the game's HUD: its component emptied and destroyed, the widget
    // returned where it came from.
    void GiveBack(int s, const std::vector<API::UObject *> &live, bool haveList)
    {
        SlotState &st = S[s];
        if (st.State != VHS_Hosted)
            return;
        ClearSource(s);
        if (st.Comp && haveList && std::find(live.begin(), live.end(), st.Comp) != live.end())
        {
            API::UObject *none = nullptr;
            Call(st.Comp, L"SetWidget", {{L"Widget", &none, sizeof(none)}});
            DestroyComponent(st.Comp);
        }
        st.Comp = nullptr;
        Restore(s);
        st.State = VHS_Idle;
        st.Fails = 0;
        // vanillahud-106: what it looks like now - the compass came back and could not be seen.
        uint8_t vis = 0xFF, pvis = 0xFF;
        float   op = -1.0f;
        API::UObject *parent = nullptr;
        CallRet(st.Widget, L"GetVisibility", {}, &vis, 1);
        CallRet(st.Widget, L"GetRenderOpacity", {}, &op, sizeof(op));
        CallRet(st.Widget, L"GetParent", {}, &parent, sizeof(parent));
        if (parent)
            CallRet(parent, L"GetVisibility", {}, &pvis, 1);
        LogInfo("%ls given back to the game's HUD (panel switch) - visibility %u, opacity %.2f, parent %ls visibility %u",
                VanillaSourceName(s), (unsigned)vis, op, parent ? ClassName(parent).c_str() : L"none", (unsigned)pvis);
    }

    // Which panel a decorative frame belongs with, by its class: it stays visible while that panel
    // is on the game's HUD. The paperdoll frame cannot tell target from own - either keeps it.
    bool FrameInGame(API::UObject *w)
    {
        const std::wstring cn = ClassName(w);
        if (cn.find(L"WeaponList") != std::wstring::npos) return !WantHosted(VS_Weapons);
        // vanillahud-106: the paperdoll frame holds the OWN panel (its NamedSlot is the own paperdoll's parent);
        // the target panel sits on the main canvas (top right) with no frame of its own.
        if (cn.find(L"PaperDoll") != std::wstring::npos)  return !WantHosted(VS_Owner);
        if (cn.find(L"MiniMap") != std::wstring::npos)    return !WantHosted(VS_Map);
        if (cn.find(L"Compass") != std::wstring::npos)    return !WantHosted(VS_Compass);
        return false;
    }

    // Frames are only collapsed, never moved. Re-asserted every tick because the HUD's own logic
    // may show them again. vanillahud-105: a frame whose panel is left in the game is shown again
    // (as it was before we collapsed it).
    void CollapseFrames()
    {
        for (auto &f : Frames)
        {
            if (*reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(Hud) + f.MemberOff) != f.W)
                continue; // replaced; the next scan picks the new one up
            uint8_t vis = 0;
            if (!CallRet(f.W, L"GetVisibility", {}, &vis, 1))
                continue;
            if (FrameInGame(f.W))
            {
                if (f.Orig != 0xFF && vis == kCollapsed && f.Orig != kCollapsed)
                    Call(f.W, L"SetVisibility", {{L"InVisibility", &f.Orig, 1}});
                Opaque(f.W); // vanillahud-119: born at opacity 0 when its panel was going to HUDDisplay
                continue;
            }
            if (vis != kCollapsed)
            {
                if (f.Orig == 0xFF)
                    f.Orig = vis;
                Call(f.W, L"SetVisibility", {{L"InVisibility", &kCollapsed, 1}});
            }
        }
    }

    // ---- hosting -------------------------------------------------------------------------
    // The on-screen size an element had: its cached geometry, which is what the frame gave it.
    // The first build drew at the element's DESIRED size instead, and the owner paperdoll - which
    // is laid out by its frame, not by its content - came out cropped.
    bool OnScreenSize(API::UObject *w, float &x, float &y)
    {
        x = y = 0.0f;
        API::UFunction *gcg = FindFn(w, L"GetCachedGeometry");
        if (!gcg || !SlateLib)
            return false;
        const int32_t goff = ParamOffset(gcg, L"ReturnValue");
        const int32_t gsz  = gcg->get_properties_size() - goff;
        if (goff < 0 || gsz <= 0 || gsz > 256)
            return false;
        uint8_t geom[256] = {};
        if (!CallRet(w, L"GetCachedGeometry", {}, geom, (size_t)gsz))
            return false;
        float v[2] = {};
        if (!CallRet(SlateLib, L"GetLocalSize", {{L"Geometry", geom, (size_t)gsz}}, v, sizeof(v)))
            return false;
        x = v[0];
        y = v[1];
        float d[2] = {};
        if (CallRet(w, L"GetDesiredSize", {}, d, sizeof(d)))
        {
            x = (d[0] > x) ? d[0] : x;
            y = (d[1] > y) ? d[1] : y;
        }
        return x >= 1.0f && y >= 1.0f;
    }

    bool HostElement(int s, API::UObject *pc)
    {
        if (!StillCurrent(s))
        {
            Scanned = false;
            return false;
        }
        return HostChild(s, pc);
    }

    // S[s].Widget out of its parent panel into its own off-screen component.
    bool HostChild(int s, API::UObject *pc)
    {
        SlotState &st = S[s];
        API::UObject *parent = nullptr;
        if (!CallRet(st.Widget, L"GetParent", {}, &parent, sizeof(parent)) || !parent)
        {
            LogError("%ls: no parent panel - left on the HUD", VanillaSourceName(s));
            return false;
        }
        int32_t index = -1;
        CallRet(parent, L"GetChildIndex", {{L"Content", &st.Widget, sizeof(void *)}}, &index, sizeof(index));
        SlotSnap snap;
        if (auto *slot = ObjProp(st.Widget, L"Slot"))
            Snapshot(slot, snap);

        float w = 0, h = 0;
        OnScreenSize(st.Widget, w, h);
        if (s >= VS_ELEMENT_COUNT)
            ; // the transmission's size follows its text - measured every time
        else if (FirstSet[s]) // vanillahud-122: the size it was first taken at
        {
            w = FirstW[s];
            h = FirstH[s];
        }
        else
        {
            FirstSet[s] = true;
            FirstW[s]   = w;
            FirstH[s]   = h;
        }
        const float h0 = h;
        // ROOM TO OVERFLOW. Several elements paint well outside their own box - the owner panel's
        // rear paperdoll, the heat and throttle gauges - and on the HUD nothing clips them. A
        // component's render target does. So the canvas is made generously larger than the box and
        // HUDDisplay trims each image to what was actually drawn (its alpha), which also drops the
        // empty margin. vanillahud-3 drew at the box size and cropped exactly those parts.
        if (w >= 1.0f && h >= 1.0f)
        {
            // The owner panel draws its rear paperdoll far to the right of its own box - the
            // standard margin still cut it off - so it gets extra width.
            const float sx = (s == VS_Owner) ? 2.4f : kOverflowScale;
            w = w * sx + kOverflowPad;
            h = h * kOverflowScale + kOverflowPad;
        }

        API::UObject *comp = MakeComponent(pc, w, h);
        if (!comp)
        {
            LogError("%ls: could not create a widget component", VanillaSourceName(s));
            return false;
        }
        if (!Call(st.Widget, L"RemoveFromParent", {}))
        {
            DestroyComponent(comp);
            return false;
        }
        st.Parent = parent;
        st.Index  = index;
        st.Snap   = std::move(snap);
        if (!Call(comp, L"SetWidget", {{L"Widget", &st.Widget, sizeof(void *)}}))
        {
            DestroyComponent(comp);
            Restore(s);
            return false;
        }
        st.Comp = comp;
        Opaque(st.Widget); // vanillahud-119: born at opacity 0 (HUD_C's template) - full inside HUDDisplay's capture
        LogInfo("%ls hosted (%.0fx%.0f, parent %ls index %d)", VanillaSourceName(s), w, h, ClassName(parent).c_str(),
                index);
        // The owner panel's rear paperdoll was cut by the same amount whatever size its canvas
        // was given - so the cut is INSIDE the widget: a panel in its own tree clips to its
        // bounds. Every clipping widget in that tree is set to inherit (no clip of its own), and
        // the tree is logged with what each one was, and with any size overrides it carries.
        // The gauges too: the heat and jump jet bars stopped short of the top the same way.
        // The heat and jump-jet bars also draw ABOVE their own top edge, which an off-screen
        // canvas cuts off however large it is (the extra room is all below). Shift the gauges'
        // drawing down into the canvas; Restore puts it back.
        if (IsGauge(s) && h0 >= 1.0f)
        {
            const float t[2] = {0.0f, h0 * 0.35f};
            Call(st.Widget, L"SetRenderTranslation", {{L"Translation", t, sizeof(t)}});
        }
        if (s == VS_Owner || s == VS_Heat || s == VS_Throttle || s == VS_JumpJets)
        {
            std::wstring tree;
            int changed = 0;
            Undo[s].clear();
            UndoNow = &Undo[s];
            Unclip(st.Widget, tree, changed);
            UndoNow = nullptr;
            LogInfo("%ls tree (clipping / retaining switched off on %d): %ls", VanillaSourceName(s), changed, tree.c_str());
        }
        return true;
    }

    // The HUD itself, at full viewport size in slate units, so every screen-positioned item in it
    // (crosshair, lock ring, hit arrows, prompts) keeps its place relative to the whole view.
    bool HostOverlay(API::UObject *pc) { return HostFullscreen(VS_Overlay, Hud, pc); }

    bool HostFullscreen(int slot, API::UObject *widget, API::UObject *pc)
    {
        SlotState &st = S[slot];
        st.Widget = widget;
        API::UObject *Hud = widget; // the body below was written for the HUD; it applies as-is
        float vp[2] = {}, scale = 1.0f;
        if (!LayoutLib ||
            !CallRet(LayoutLib, L"GetViewportSize", {{L"WorldContextObject", &Hud, sizeof(void *)}}, vp, sizeof(vp)) ||
            vp[0] < 16.0f || vp[1] < 16.0f)
        {
            LogError("%ls: viewport size unavailable - left on screen", VanillaSourceName(slot));
            return false;
        }
        CallRet(LayoutLib, L"GetViewportScale", {{L"WorldContextObject", &Hud, sizeof(void *)}}, &scale, sizeof(scale));
        if (scale <= 0.01f)
            scale = 1.0f;
        const float w = vp[0] / scale, h = vp[1] / scale;

        API::UObject *parent = nullptr;
        CallRet(Hud, L"GetParent", {}, &parent, sizeof(parent));
        bool inViewport = false;
        CallRet(Hud, L"IsInViewport", {}, &inViewport, 1);
        if (parent)
        {
            CallRet(parent, L"GetChildIndex", {{L"Content", &Hud, sizeof(void *)}}, &st.Index, sizeof(int32_t));
            if (auto *slot = ObjProp(Hud, L"Slot"))
                Snapshot(slot, st.Snap);
        }

        API::UObject *comp = MakeComponent(pc, w, h);
        if (!comp)
            return false;
        if (!Call(Hud, L"RemoveFromParent", {}))
        {
            DestroyComponent(comp);
            return false;
        }
        st.Parent        = parent;
        st.WasInViewport = !parent && inViewport;
        if (!Call(comp, L"SetWidget", {{L"Widget", &Hud, sizeof(void *)}}))
        {
            DestroyComponent(comp);
            Restore(slot);
            return false;
        }
        st.Comp = comp;
        Opaque(widget); // vanillahud-119
        LogInfo("%ls hosted (%.0fx%.0f slate units = %.0fx%.0f px at scale %.2f, %ls)", VanillaSourceName(slot), w, h, vp[0], vp[1],
                scale, parent ? ClassName(parent).c_str() : (inViewport ? L"was in the viewport" : L"had no parent"));
        return true;
    }

    // Back where it came from, with its slot layout and its place in the child order.
    static bool IsGauge(int s) { return s == VS_Heat || s == VS_Throttle || s == VS_JumpJets; }

    void Restore(int s)
    {
        SlotState &st = S[s];
        if (!StillCurrent(s))
        {
            LogError("%ls: cannot return it (the HUD changed underneath) - it will be rebuilt with the HUD",
                     VanillaSourceName(s));
            return;
        }
        if (IsGauge(s))
        {
            const float t[2] = {0.0f, 0.0f};
            Call(st.Widget, L"SetRenderTranslation", {{L"Translation", t, sizeof(t)}});
        }
        // vanillahud-106: clipping / retaining back as the game had them (newest first).
        for (auto it = Undo[s].rbegin(); it != Undo[s].rend(); ++it)
        {
            if (it->Kind == 0)
            {
                const bool v = it->Old != 0;
                if (API::UFunction *fn = FindFn(it->W, L"SetRetainRendering"))
                {
                    const std::wstring pn = FirstParamName(fn);
                    if (!pn.empty())
                        Call(it->W, L"SetRetainRendering", {{pn.c_str(), &v, 1}});
                }
            }
            else
                Call(it->W, L"SetClipping", {{L"InClipping", &it->Old, 1}});
        }
        if (!Undo[s].empty())
            LogInfo("%ls: %zu clipping / retaining change(s) put back", VanillaSourceName(s), Undo[s].size());
        Undo[s].clear();
        if (!st.Parent)
        {
            if (st.WasInViewport)
            {
                int32_t z = 0;
                Call(st.Widget, L"AddToViewport", {{L"ZOrder", &z, sizeof(z)}});
                LogInfo("%ls returned to the viewport", VanillaSourceName(s));
            }
            return;
        }

        int32_t count = 0;
        CallRet(st.Parent, L"GetChildrenCount", {}, &count, sizeof(count));
        const int32_t at = (st.Index < 0 || st.Index > count) ? count : st.Index;

        // AddChild appends: lift the siblings after the original index off first, then put them
        // back, each with its own layout.
        struct Trailing
        {
            API::UObject *W;
            SlotSnap      Snap;
        };
        std::vector<Trailing> trailing;
        for (int32_t i = at; i < count; ++i)
        {
            API::UObject *c = nullptr;
            int32_t idx = i;
            if (!CallRet(st.Parent, L"GetChildAt", {{L"Index", &idx, sizeof(idx)}}, &c, sizeof(c)) || !c)
                continue;
            Trailing t{c, {}};
            if (auto *cs = ObjProp(c, L"Slot"))
                Snapshot(cs, t.Snap);
            trailing.push_back(std::move(t));
        }
        for (auto &t : trailing)
            Call(t.W, L"RemoveFromParent", {});

        API::UObject *ns = nullptr;
        CallRet(st.Parent, L"AddChild", {{L"Content", &st.Widget, sizeof(void *)}}, &ns, sizeof(ns));
        if (ns)
            Apply(ns, st.Snap);
        for (auto &t : trailing)
        {
            API::UObject *ts = nullptr;
            CallRet(st.Parent, L"AddChild", {{L"Content", &t.W, sizeof(void *)}}, &ts, sizeof(ts));
            if (ts)
                Apply(ts, t.Snap);
        }
        // It spent time laid out by a component; make Slate measure it afresh in its old home.
        // (The first build returned the owner paperdoll still cropped to its component's size.)
        Call(st.Widget, L"ForceLayoutPrepass", {});
        Call(st.Widget, L"InvalidateLayoutAndVolatility", {});

        LogInfo("%ls returned to the HUD (index %d, %zu siblings re-seated)", VanillaSourceName(s), at,
                trailing.size());
        st.Parent = nullptr;
        st.Index  = -1;
        st.Snap.Items.clear();
    }

    API::UObject *MakeComponent(API::UObject *actor, float w, float h)
    {
        alignas(16) float xf[12] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0}; // FTransform: quat, pos, scale
        bool manual = false, deferred = false;
        API::UObject *comp = nullptr;
        if (!CallRet(actor, L"AddComponentByClass",
                     {{L"Class", &WidgetCompClass, sizeof(void *)},
                      {L"bManualAttachment", &manual, 1},
                      {L"RelativeTransform", xf, sizeof(xf)},
                      {L"bDeferredFinish", &deferred, 1}},
                     &comp, sizeof(comp)) ||
            !comp)
            return nullptr;

        uint8_t world = 0, noColl = 0; // EWidgetSpace::World, ECollisionEnabled::NoCollision
        bool yes = true, no = false;
        Call(comp, L"SetWidgetSpace", {{L"NewSpace", &world, 1}});
        Call(comp, L"SetOwnerNoSee", {{L"bNewOwnerNoSee", &yes, 1}});
        Call(comp, L"SetCastShadow", {{L"NewCastShadow", &no, 1}});
        Call(comp, L"SetCollisionEnabled", {{L"NewType", &noColl, 1}});
        if (!Call(comp, L"SetTickWhenOffscreen", {{L"bWantTickWhenOffscreen", &yes, 1}}) && TickOffscreenProp)
            TickOffscreenProp->set_value_in_object(comp, true);
        if (w >= 1.0f && h >= 1.0f)
        {
            float size[2] = {w, h};
            Call(comp, L"SetDrawAtDesiredSize", {{L"bInDrawAtDesiredSize", &no, 1}});
            Call(comp, L"SetDrawSize", {{L"Size", size, sizeof(size)}});
        }
        else
        {
            Call(comp, L"SetDrawAtDesiredSize", {{L"bInDrawAtDesiredSize", &yes, 1}});
        }
        float down[3] = {0, 0, kHideDepthCm};
        Call(comp, L"K2_SetRelativeLocation", {{L"NewLocation", down, sizeof(down)}});
        return comp;
    }

    void DestroyComponent(API::UObject *comp)
    {
        Call(comp, L"K2_DestroyComponent", {{L"Object", &comp, sizeof(void *)}});
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
    // WORLD MARKERS - nav points, objective and unit markers, target boxes
    //
    // These are the game's native MWMarkerWidget / MWTargetBoxWidget widgets, positioned on screen
    // over the thing they mark. EVERY live marker is placed (vanillahud-5 looked at the first one
    // only - the minimap's own-mech icon - and wrongly concluded they all lived in the HUD):
    //   * inside the HUD        -> already off the main view with the HUD overlay and its elements;
    //   * in a shared layer     -> that layer is hosted at full viewport size, exactly like the
    //                              HUD, and HUDDisplay lays it over the Targeting screen;
    //   * top-level on their own-> made fully transparent on the main view (and so are not on the
    //                              Targeting screen either).
    // A marker's placement is worked out once per widget and remembered.
    // =====================================================================================
    API::UObject *FirstWidgetOf(API::UObject *pc, API::UClass *cls, std::vector<API::UObject *> *all = nullptr,
                                bool topLevelOnly = false)
    {
        if (!cls || !WidgetLib)
            return nullptr;
        if (!GawFn)
        {
            GawFn = FindFn(WidgetLib, L"GetAllWidgetsOfClass");
            if (!GawFn)
                return nullptr;
            GawWorld = ParamOffset(GawFn, L"WorldContextObject");
            GawOut   = ParamOffset(GawFn, L"FoundWidgets");
            GawClass = ParamOffset(GawFn, L"WidgetClass");
            GawTop   = ParamOffset(GawFn, L"TopLevelOnly");
            const int32_t frame = GawFn->get_properties_size();
            if (GawWorld < 0 || GawOut < 0 || GawClass < 0 || GawTop < 0 || frame <= 0)
            {
                LogError("GetAllWidgetsOfClass has an unexpected frame - markers left alone");
                GawFn = nullptr;
                WidgetLib = nullptr;
                return nullptr;
            }
            GawBuf.assign((size_t)frame + 16, 0);
        }
        // Inputs rewritten every call; the out-array is left alone so the engine reuses it.
        std::memcpy(GawBuf.data() + GawWorld, &pc, sizeof(void *));
        std::memcpy(GawBuf.data() + GawClass, &cls, sizeof(void *));
        GawBuf[GawTop] = topLevelOnly ? 1 : 0;
        WidgetLib->process_event(GawFn, GawBuf.data());
        struct Arr
        {
            API::UObject **Data;
            int32_t        Num, Max;
        } arr{};
        std::memcpy(&arr, GawBuf.data() + GawOut, sizeof(arr));
        API::UObject *first = nullptr;
        for (int32_t i = 0; arr.Data && i < arr.Num; ++i)
            if (arr.Data[i])
            {
                if (!first)
                    first = arr.Data[i];
                if (all)
                    all->push_back(arr.Data[i]);
            }
        return first;
    }

    // Up through parent panels, and out of each widget tree to the user widget that owns it.
    API::UObject *LayerOf(API::UObject *w, std::wstring &chain)
    {
        API::UObject *cur = w;
        chain = ClassName(cur);
        for (int i = 0; i < 24; ++i)
        {
            API::UObject *p = nullptr;
            if (CallRet(cur, L"GetParent", {}, &p, sizeof(p)) && p)
            {
                cur = p;
                chain += L" < " + ClassName(cur);
                continue;
            }
            API::UObject *o = cur->get_outer();
            if (o && ClassName(o) == L"WidgetTree")
            {
                API::UObject *u = o->get_outer();
                if (u && u != cur)
                {
                    cur = u;
                    chain += L" < " + ClassName(cur);
                    continue;
                }
            }
            break;
        }
        return cur;
    }

    enum MarkerPlace : uint8_t
    {
        MP_InHud = 0,
        MP_Layer,
        MP_Alone,
    };

    struct MarkerVerdict
    {
        API::UObject *W;
        const void   *Cls;
        API::UObject *Root;
        uint8_t       Place;
    };

    std::vector<MarkerVerdict> MarkerVerdicts;

    std::wstring               MarkerSummary;

    ULONGLONG                  MarkerSummaryMs = 0;

    const MarkerVerdict *VerdictOf(API::UObject *w)
    {
        const void *cls = w->get_class();
        for (auto &v : MarkerVerdicts)
            if (v.W == w && v.Cls == cls)
                return &v;
        return nullptr;
    }

    void SilenceEachMarker(API::UObject *pc)
    {
        const float zero = 0.0f;
        std::vector<API::UObject *> all;
        FirstWidgetOf(pc, MarkerClass, &all);
        FirstWidgetOf(pc, TargetBoxClass, &all);
        for (auto *m : all)
            if (const MarkerVerdict *v = VerdictOf(m))
                if (v->Place == MP_Alone)
                    Call(m, L"SetRenderOpacity", {{L"InOpacity", &zero, sizeof(zero)}});
    }

    void HandleMarkers(API::UObject *pc, const std::vector<API::UObject *> &live, bool haveList)
    {
        if (!WidgetLib || (!MarkerClass && !TargetBoxClass))
            return;
        SlotState &mk = S[VS_Markers];
        if (mk.State == VHS_Hosted && haveList && std::find(live.begin(), live.end(), mk.Comp) == live.end())
        {
            ClearSource(VS_Markers);
            mk.Comp = nullptr;
            Restore(VS_Markers);
            mk.State = VHS_Idle;
        }
        if (MarkersEach)
            SilenceEachMarker(pc);

        const ULONGLONG now = GetTickCount64();
        if (now - LastMarkerScan < 1000)
            return;
        LastMarkerScan = now;

        // Place every live marker (new ones only; the rest are remembered).
        std::vector<API::UObject *> all;
        FirstWidgetOf(pc, MarkerClass, &all);
        FirstWidgetOf(pc, TargetBoxClass, &all);
        std::vector<MarkerVerdict> next;
        next.reserve(all.size());
        int inHud = 0, inLayer = 0, alone = 0;
        API::UObject *layer = nullptr;
        std::wstring layerChain;
        for (auto *w : all)
        {
            MarkerVerdict v{};
            if (const MarkerVerdict *old = VerdictOf(w))
                v = *old;
            else
            {
                std::wstring chain;
                API::UObject *root = LayerOf(w, chain);
                v = {w, w->get_class(), root, MP_Alone};
                if (root == Hud)
                    v.Place = MP_InHud;
                else if (root != w)
                {
                    bool inViewport = false;
                    API::UObject *rootParent = nullptr;
                    const bool isUserWidget = CallRet(root, L"IsInViewport", {}, &inViewport, 1);
                    CallRet(root, L"GetParent", {}, &rootParent, sizeof(rootParent));
                    if (isUserWidget && (inViewport || rootParent || root == MarkerRoot))
                        v.Place = MP_Layer;
                }
                if (v.Place == MP_Layer && !layer)
                    layerChain = chain;
            }
            if (v.Place == MP_Layer && !layer)
                layer = v.Root;
            inHud += v.Place == MP_InHud;
            inLayer += v.Place == MP_Layer;
            alone += v.Place == MP_Alone;
            next.push_back(v);
        }
        MarkerVerdicts.swap(next);
        MarkersEach = alone > 0;

        if (!all.empty())
        {
            wchar_t buf[160];
            swprintf(buf, 160, L"%d inside the HUD, %d in a layer of their own, %d top-level (hidden one by one)", inHud,
                     inLayer, alone);
            if (MarkerSummary != buf && now - MarkerSummaryMs >= 30000)
            {
                MarkerSummaryMs = now;
                MarkerSummary = buf;
                LogInfo("world markers: %ls", buf);
            }
        }
        if (!layer)
            return;

        if (layer != MarkerRoot)
        {
            if (mk.State == VHS_Hosted)
            {
                ClearSource(VS_Markers);
                if (haveList && std::find(live.begin(), live.end(), mk.Comp) != live.end())
                {
                    API::UObject *none = nullptr;
                    Call(mk.Comp, L"SetWidget", {{L"Widget", &none, sizeof(none)}});
                    DestroyComponent(mk.Comp);
                }
            }
            mk         = SlotState{};
            MarkerRoot = layer;
            LogInfo("world markers found in a layer of their own: %ls", layerChain.empty() ? ClassName(layer).c_str() : layerChain.c_str());
        }
        bool inViewport = false;
        CallRet(layer, L"IsInViewport", {}, &inViewport, 1);
        if ((mk.State == VHS_Idle || mk.State == VHS_Failed) && mk.Fails < kMaxHostFails)
        {
            if (HostFullscreen(VS_Markers, layer, pc))
            {
                mk.State  = VHS_Hosted;
                HostActor = pc;
            }
            else
            {
                ++mk.Fails;
                mk.State = VHS_Failed;
            }
        }
        else if (mk.State == VHS_Hosted && inViewport)
        {
            LogInfo("the game re-added the marker layer to the viewport - moving it back");
            Call(layer, L"RemoveFromParent", {});
            Call(mk.Comp, L"SetWidget", {{L"Widget", &layer, sizeof(void *)}});
        }
    }

    // =====================================================================================
    // MISSION DIALOGUE - the priority transmission box and subtitles
    //
    // Not part of the HUD, so the HUD overlay does not take them. Found by class NAME among the
    // live user widgets (their package path is not assumed), then moved off the main view exactly
    // like a HUD element: taken out of its panel into an off-screen component at its on-screen
    // size - or, if it sits on the viewport by itself, hosted at full viewport size like the HUD.
    // HUDDisplay shows them on the Objectives screen while they draw anything.
    // Every change in the set of top-level widgets is logged, so a dialogue box by another name
    // can be identified from the log.
    // =====================================================================================
    static constexpr int kDlgCount = 2;

    static inline const wchar_t *kDlgClassName[kDlgCount] = {L"PriorityTransmissionWidget_C", L"SubtitleWidget_C"};

    API::UClass  *UserWidgetClass       = nullptr;

    API::UClass  *DlgClass[kDlgCount]   = {};

    API::UObject *DlgWidget[kDlgCount]  = {};

    ULONGLONG     DlgScanMs = 0, DlgFindMs = 0;

    std::wstring  TopLevelSummary;

    void ForgetDialogue()
    {
        for (int d = 0; d < kDlgCount; ++d)
        {
            DlgClass[d]  = nullptr;
            DlgWidget[d] = nullptr;
        }
        DlgScanMs = DlgFindMs = 0;
        TopLevelSummary.clear();
    }

    void HandleDialogue(API::UObject *pc, const std::vector<API::UObject *> &live, bool haveList)
    {
        if (!WidgetLib || !UserWidgetClass)
            return;
        for (int d = 0; d < kDlgCount; ++d)
        {
            SlotState &st = S[VS_Dialogue + d];
            if (st.State == VHS_Hosted && haveList && std::find(live.begin(), live.end(), st.Comp) == live.end())
            {
                ClearSource(VS_Dialogue + d);
                st.Comp = nullptr;
                Restore(VS_Dialogue + d);
                st.State = VHS_Idle;
            }
        }

        const ULONGLONG now = GetTickCount64();
        if (now - DlgScanMs < 1000)
            return;
        DlgScanMs = now;

        // Diagnostics: what sits on the viewport by itself.
        {
            std::vector<API::UObject *> top;
            FirstWidgetOf(pc, UserWidgetClass, &top, true);
            std::vector<std::wstring> names;
            for (auto *w : top)
                if (w != Hud)
                    names.push_back(ClassName(w));
            std::sort(names.begin(), names.end());
            std::wstring sum;
            for (auto &n : names)
                sum += (sum.empty() ? L"" : L", ") + n;
            if (sum != TopLevelSummary)
            {
                TopLevelSummary = sum;
                LogInfo("top-level widgets on the viewport: %ls", sum.empty() ? L"(none)" : sum.c_str());
            }
        }

        // Resolve the dialogue classes by name from the live widgets (every 3 s until found).
        if (((!DlgClass[0] && DlgMemberOff[0] < 0) || (!DlgClass[1] && DlgMemberOff[1] < 0)) && now - DlgFindMs >= 3000)
        {
            DlgFindMs = now;
            std::vector<API::UObject *> every;
            FirstWidgetOf(pc, UserWidgetClass, &every, false);
            for (auto *w : every)
            {
                const std::wstring cn = ClassName(w);
                for (int d = 0; d < kDlgCount; ++d)
                    if (!DlgClass[d] && cn == kDlgClassName[d])
                    {
                        DlgClass[d] = w->get_class();
                        std::wstring chain;
                        LayerOf(w, chain);
                        LogInfo("dialogue widget found: %ls", chain.c_str());
                    }
            }
        }

        for (int d = 0; d < kDlgCount; ++d)
        {
            const int  slot = VS_Dialogue + d;
            SlotState &st   = S[slot];
            API::UObject *w = nullptr;
            if (DlgMemberOff[d] >= 0 && Hud)
                w = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(Hud) + DlgMemberOff[d]);
            else if (DlgClass[d])
                w = FirstWidgetOf(pc, DlgClass[d]);
            if (!w)
                continue;

            // vanillahud-105: the transmission left in the game by the player's choice.
            if (!WantHosted(slot))
            {
                if (w == DlgWidget[d])
                    GiveBack(slot, live, haveList);
                if (st.State != VHS_InGame)
                    Opaque(w); // vanillahud-119
                st.State = VHS_InGame;
                continue;
            }
            if (st.State == VHS_InGame)
            {
                st.State = VHS_Idle;
                st.Fails = 0;
            }
            if (w != DlgWidget[d])
            {
                // A new instance: let the old one go with its component, take this one.
                if (st.State == VHS_Hosted)
                {
                    ClearSource(slot);
                    if (haveList && std::find(live.begin(), live.end(), st.Comp) != live.end())
                    {
                        API::UObject *none = nullptr;
                        Call(st.Comp, L"SetWidget", {{L"Widget", &none, sizeof(none)}});
                        DestroyComponent(st.Comp);
                    }
                }
                st           = SlotState{};
                DlgWidget[d] = w;
            }

            if ((st.State == VHS_Idle || st.State == VHS_Failed) && st.Fails < kMaxHostFails)
            {
                API::UObject *parent = nullptr;
                CallRet(w, L"GetParent", {}, &parent, sizeof(parent));
                st.Widget = w;
                const bool ok = parent ? HostChild(slot, pc) : HostFullscreen(slot, w, pc);
                if (ok)
                {
                    st.State  = VHS_Hosted;
                    HostActor = pc;
                }
                else
                {
                    ++st.Fails;
                    st.State = VHS_Failed;
                    if (st.Fails >= kMaxHostFails)
                        Opaque(w); // vanillahud-119
                }
            }
            else if (st.State == VHS_Hosted)
            {
                // Put back on screen by the game (a new transmission)? Take it off again.
                API::UObject *parent = nullptr;
                bool inViewport = false;
                CallRet(w, L"GetParent", {}, &parent, sizeof(parent));
                CallRet(w, L"IsInViewport", {}, &inViewport, 1);
                if (parent || inViewport)
                {
                    LogInfo("the game put the %ls back on screen - moving it back", VanillaSourceName(slot));
                    Call(w, L"RemoveFromParent", {});
                    Call(st.Comp, L"SetWidget", {{L"Widget", &w, sizeof(void *)}});
                }
            }
        }
    }

    // ---- slot layout ---------------------------------------------------------------------
    void Snapshot(API::UObject *slot, SlotSnap &out)
    {
        static const wchar_t *k[][2] = {
            {L"LayoutData", L"SetLayout"}, {L"bAutoSize", L"SetAutoSize"}, {L"ZOrder", L"SetZOrder"},
            {L"Padding", L"SetPadding"},   {L"Size", L"SetSize"},
            {L"HorizontalAlignment", L"SetHorizontalAlignment"}, {L"VerticalAlignment", L"SetVerticalAlignment"},
        };
        auto *cls = slot->get_class();
        for (auto &e : k)
        {
            auto *prop = FindProp(cls, e[0]);
            if (!prop)
                continue;
            auto *fn = FindFn(slot, e[1]);
            if (!fn)
                continue;
            const int32_t size = FirstParamSize(fn);
            if (size <= 0 || size > 64)
                continue;
            SlotSnap::Item it{e[1], {}, size};
            if (prop->get_class() && prop->get_class()->get_name() == L"BoolProperty")
                it.Bytes[0] = reinterpret_cast<API::FBoolProperty *>(prop)->get_value_from_object(slot) ? 1 : 0;
            else
                std::memcpy(it.Bytes, reinterpret_cast<uint8_t *>(slot) + prop->get_offset(), (size_t)size);
            out.Items.push_back(it);
        }
    }

    void Apply(API::UObject *slot, const SlotSnap &snap)
    {
        for (const auto &it : snap.Items)
        {
            auto *fn = FindFn(slot, it.Setter.c_str());
            if (!fn)
                continue;
            const std::wstring pname = FirstParamName(fn);
            if (!pname.empty())
                Call(slot, it.Setter.c_str(), {{pname.c_str(), it.Bytes, (size_t)it.Size}});
        }
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

    static int32_t FirstParamOffset(API::UFunction *fn)
    {
        for (auto *f = fn->get_child_properties(); f; f = f->get_next())
        {
            auto *p = reinterpret_cast<API::FProperty *>(f);
            if (p->is_param() && !p->is_return_param())
                return p->get_offset();
        }
        return -1;
    }

    static std::wstring FirstParamName(API::UFunction *fn)
    {
        for (auto *f = fn->get_child_properties(); f; f = f->get_next())
        {
            auto *p = reinterpret_cast<API::FProperty *>(f);
            if (p->is_param() && !p->is_return_param())
                return f->get_fname()->to_string();
        }
        return L"";
    }

    static int32_t FirstParamSize(API::UFunction *fn)
    {
        const int32_t off = FirstParamOffset(fn);
        if (off < 0)
            return -1;
        int32_t next = fn->get_properties_size();
        for (auto *f = fn->get_child_properties(); f; f = f->get_next())
        {
            const int32_t o = reinterpret_cast<API::FProperty *>(f)->get_offset();
            if (o > off && o < next)
                next = o;
        }
        return next - off;
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
        if (!CopyFrame) // vanillahud-110: not a copy frame - no mutex touched
            return;
        const double a0 = PpNow(); // vanillahud-109: timed
        if (AbNew && SlotBusyRun[s] >= kBusyBackoffAfter && a0 < SlotRetryAt[s])
        {
            ++PpBackedOff; // not taken lately: tried every 250 ms only
            return;
        }
        ComPtr<IDXGIKeyedMutex> m;
        if (FAILED(Shared[s].As(&m)))
            return;
        ++Pp[s].Tries;
        const HRESULT hr = m->AcquireSync(0, 0);
        const double a1 = PpNow();
        Pp[s].Acq += a1 - a0;
        if (hr != S_OK)
        {
            ++Pp[s].Busy;
            if (SlotBusyRun[s] < kBusyBackoffAfter)
                ++SlotBusyRun[s];
            SlotRetryAt[s] = a1 + kBusyRetryMs;
            PpCopyMs += a1 - a0;
            return;
        }
        SlotBusyRun[s] = 0; // taken: every copy frame again
        ctx->CopyResource(Shared[s].Get(), src);
        const double a2 = PpNow();
        if (pose)
            Block->DepthCam = *pose;
        if (lock)
            Block->DepthLock = *lock;
        m->ReleaseSync(1);
        const double a3 = PpNow();
        Pp[s].Copy += a2 - a1;
        Pp[s].Rel += a3 - a2;
        Pp[s].Max = (std::max)(Pp[s].Max, a3 - a0);
        ++Pp[s].Copies;
        PpCopyMs += a3 - a0;
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
std::unique_ptr<VanillaHud> g_plugin{new VanillaHud()};
