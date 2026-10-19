// LidarView.cpp
//
// The LIDAR tactical scope, ported into HUDDisplay as a selectable view.
//
// This is a COPY of src/lidar_display/Main.cpp. The standalone LidarDisplay.exe and
// its source are deliberately left untouched; changes here must never be pushed back
// into it. The port is mechanical:
//
//   * ID2D1HwndRenderTarget* -> ID2D1RenderTarget*. The host owns the swap chain and
//     hands us a DXGI-surface render target, so D2D draws into HUDDisplay's back
//     buffer and HUDDisplay's Present stays the single presentation point.
//   * Init()/Run()/WndProc/WinMain removed - the host owns the window and the loop.
//   * The options menu, its click hit-testing, the number-key layer toggles and the
//     lidar_view.cfg reader/writer are removed. Layers, projectile interval and alarm
//     volume are now host-set fields, persisted in HUDDisplay's own config and edited
//     from its edit-mode menu.
//
// Everything else - the MMF reader, the seqlock, track/fade state, projectile and
// smoke-trail handling, landed-round cleanup, incoming detection and the synthesized
// alarms - is copied verbatim.

#include "LidarView.h"

#include "TerrainUnderlay.h"
#include "DropshipOutline.h" // DROPSHIPS (2026-10-08)
#include "ThreatAlarm.h"

#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <LidarShared.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// <windows.h> #defines DrawText -> DrawTextW, which collides with the Direct2D
// ID2D1RenderTarget::DrawText method. Undo the macro so we can call it.
#undef DrawText

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct ProjPos { float X, Y, Z; };

// Colors (0xRRGGBB).
namespace Col {
    constexpr UINT32 Background = 0x070D0B;
    constexpr UINT32 Ring       = 0x1F5249;
    constexpr UINT32 Sweep      = 0x3FE6CD;
    constexpr UINT32 Player     = 0x7DFF6A;
    constexpr UINT32 Friendly   = 0x5DFFA0;
    constexpr UINT32 Hostile    = 0xFF5A4D;
    constexpr UINT32 Neutral    = 0xF0A93C;
    constexpr UINT32 Unknown    = 0x35E0C8;
    constexpr UINT32 Text       = 0x3FE6CD;
    constexpr UINT32 Selected   = 0xFFFFFF; // locked-target highlight ring
    constexpr UINT32 Panel      = 0x0E1A17; // options-menu background
    constexpr UINT32 PanelEdge  = 0x2A5F54; // options-menu border / checkbox outline
    constexpr UINT32 MenuText   = 0xBFF5E8;
    constexpr UINT32 MenuDim    = 0x4C6E66; // disabled / hint text
    constexpr UINT32 CheckOn    = 0x5DFFA0; // ticked checkbox fill
    constexpr UINT32 ProjBallistic = 0x9A6A33; // autocannon/gauss shells (brown)
    constexpr UINT32 ProjPPC       = 0x4D8CFF; // PPC energy bolt (blue)
    constexpr UINT32 ProjMissile   = 0xF0503C; // missiles + Arrow IV (red-ish)
    constexpr UINT32 Smoke         = 0x9AA0A0; // missile smoke trail (gray)
    constexpr UINT32 ProjBound      = 0xFFD11A; // the round tracked as inbound on US (amber)
}

// ---- View-layer framework -------------------------------------------------
// Each toggleable view is a "layer" with a bit in EnabledLayers. Adding a layer
// = add an enum entry + a row here. In this port the host owns the toggles: HUDDisplay
// reads kLayers through LidarView::LayerName() to build its edit-mode checkboxes and
// pushes the resulting bitmask back in via SetLayers(). `defaultKey` is unused here and
// kept only so this table stays diffable against the standalone app's copy.
enum ViewLayer : uint32_t {
    VL_RangeRings = 0,
    VL_PingSweep,
    VL_AimLine,
    VL_Mechs,
    VL_Vehicles,
    VL_WeightDots,
    VL_TargetHighlight,
    VL_Projectiles, // ballistic + PPC bolts (data-producing: sets the control bit)
    VL_Missiles,    // missiles           (data-producing: sets the control bit)
    VL_ThreatAlarm, // audible incoming-missile warning (data-producing: LIDAR_CTRL_THREAT)
    // APPEND ONLY BELOW THIS LINE. The layer mask is stored in the config as an integer, so
    // inserting anywhere above silently repoints every saved toggle.
    VL_Terrain,     // ground wireframe under the scope, drawn by TopoView via ITerrainUnderlay
    VL_COUNT
};

struct LayerDef {
    const wchar_t* name;
    UINT           defaultKey;  // VK / char for future keybinds (displayed now)
    bool           implemented; // false => shown but not toggleable ("soon")
};

static const LayerDef kLayers[VL_COUNT] = {
    { L"Range rings",      '1', true  },
    { L"Ping sweep",       '2', true  },
    { L"Torso aim line",   '3', true  },
    { L"Mechs",            '4', true  },
    { L"Vehicles",         '5', true  },
    { L"Weight dots",      '6', true  },
    { L"Target highlight", '7', true  },
    { L"Projectiles",      '8', true  },
    { L"Missiles",         '9', true  },
    { L"Missile alarm",    '0', true  },
    { L"Terrain",          'T', true  },
};

// Default: every implemented contact layer on, projectiles/missiles off, and the alarm ON
// - it costs one cheap probe, works on every page, and a warning nobody switched on is a
// warning nobody gets. Appending it as the LAST bit keeps every saved layer mask valid.
static constexpr uint32_t kDefaultLayers = 0x7F | (1u << VL_ThreatAlarm) | (1u << VL_Terrain);

// Layer metadata for the host's edit-mode menu.
int            LidarView::LayerCount()          { return VL_COUNT; }
const wchar_t* LidarView::LayerName(int i)      { return (i >= 0 && i < (int)VL_COUNT) ? kLayers[i].name : L""; }
uint32_t       LidarView::DefaultLayers()       { return kDefaultLayers; }
uint32_t       LidarView::AlarmLayerMask()      { return 1u << VL_ThreatAlarm; }

class LidarViewImpl {
    // Borrowed, never owned. The host wires this to its TopoView; null simply means no terrain,
    // which is what happens if the terrain producer is not running.
    ITerrainUnderlay*       Terrain    = nullptr;
    uint32_t                TerrainSegs = 0;   // last frame's segment count, for the host's log

    ID2D1Factory*           Factory    = nullptr;
    ID2D1RenderTarget*      RT         = nullptr; // host-owned DXGI surface target
    IDWriteFactory*         DW         = nullptr;
    IDWriteTextFormat*      TextFmt    = nullptr;
    ID2D1SolidColorBrush*   Brush      = nullptr;

    HANDLE                  Mmf        = nullptr;
    LidarExportBlock*       Shared     = nullptr; // mapped read/write (we write ControlLayers)

    // Several HUDDisplay instances can run at once (one per screen, distinguished by the
    // command-line modifier). The export block has ONE control channel and the alarm is
    // audible process-wide, so exactly one instance may drive them - otherwise two scopes
    // stamp conflicting ControlLayers over each other every frame and the incoming-missile
    // alarm plays twice, out of phase. A named mutex elects the owner: whoever is showing
    // the scope grabs it, and releases it on switching away or exiting so another instance
    // can take over. Non-owners still render everything the plugin publishes; they simply
    // stay silent and keep their hands off the control channel.
    HANDLE                  ControlMutex = nullptr;
    bool                    OwnsControl  = false;

    // Scope-priority arbitration. Control ownership used to be claimed in Render(), so only
    // an instance actually drawing the scope could hold it. Moving the claim into Poll() to
    // make the alarm view-independent broke that: an instance with the alarm layer on but
    // the scope NOT showing could win the mutex, and then wrote ControlLayers without the
    // projectile bit (its own scopeLive being false) - starving the instance that WAS
    // showing the scope of the array, and with it all per-round binding.
    //
    // This second mutex is held by any instance whose scope is live. A non-scope instance
    // checks it before claiming control and stands aside if someone is drawing the scope.
    HANDLE                  ScopeMutex   = nullptr;
    bool                    HoldsScope   = false;

    void EnsureScopeMutex() {
        if (!ScopeMutex)
            ScopeMutex = CreateMutexW(nullptr, FALSE, L"Local\\MW5_LidarScopeLive");
    }
    void AcquireScope() {
        if (HoldsScope) return;
        EnsureScopeMutex();
        if (!ScopeMutex) return;
        const DWORD r = WaitForSingleObject(ScopeMutex, 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) HoldsScope = true;
    }
    void ReleaseScope() {
        if (!HoldsScope) return;
        HoldsScope = false;
        if (ScopeMutex) ReleaseMutex(ScopeMutex);
    }
    // True when SOMEONE ELSE is drawing the scope: we could not take the flag, and we are
    // not the holder. Momentary acquisition is released immediately - this is a test, not
    // a claim.
    bool AnotherInstanceHasScope() {
        if (HoldsScope) return false;
        EnsureScopeMutex();
        if (!ScopeMutex) return false;
        const DWORD r = WaitForSingleObject(ScopeMutex, 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) { ReleaseMutex(ScopeMutex); return false; }
        return true;
    }

    void EnsureControlMutex() {
        if (!ControlMutex)
            ControlMutex = CreateMutexW(nullptr, FALSE, L"Local\\MW5_LidarControlOwner");
    }

    // Non-blocking: if another instance holds it we simply don't own it this frame.
    // WAIT_ABANDONED means the previous owner died without releasing - we inherit it.
    bool TryClaimControl() {
        if (OwnsControl) return true;
        EnsureControlMutex();
        if (!ControlMutex) return true; // no mutex (shouldn't happen) - behave as before
        const DWORD r = WaitForSingleObject(ControlMutex, 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
            OwnsControl = true;
            printf("[LidarView] took LIDAR control/alarm ownership\n");
        }
        return OwnsControl;
    }

    void DropControl() {
        if (!OwnsControl) return;
        if (Shared) Shared->ControlLayers = 0; // only the owner may clear it
        OwnsControl = false;
        if (ControlMutex) ReleaseMutex(ControlMutex);
        printf("[LidarView] released LIDAR control/alarm ownership\n");
    }

    ULONGLONG               StartTick  = 0;
    bool                    Running    = true;

    // Ping-sweep state: a new *scan* (ScanCounter change, ~3 s) restarts the outward
    // pulse. Player-transform-only updates (~5 Hz) bump FrameCounter but not
    // ScanCounter, so they refresh heading/torso without retriggering the sweep.
    uint32_t                LastScan   = 0xFFFFFFFF;

    // ---- scene state, ticked once a frame by Poll() and read by every renderer ----------
    // This used to be locals inside Render(). It is shared state now because there are two
    // renderers, and the one that happens to be on screen must not be the one that decides
    // whether the other one's data advances.
    LidarExportBlock        SceneBlk{};
    bool                    SceneHave    = false;
    float                   ScenePulseCm = 0.0f;
    std::vector<LidarSceneContact>    SceneContacts;
    std::vector<LidarSceneProjectile> SceneProjectiles;
    std::vector<LidarSceneTrailPt>    SceneTrail;
    double                  PulseStart = 0.0; // high-res ms (QueryPerformanceCounter)
    // Persistent blips: last-known position per stable id, kept on screen until the
    // ping reaches (and updates) each target's new position.
    std::unordered_map<uint16_t, LidarContact> Tracks;
    std::unordered_set<uint16_t>               CurIds; // ids present in the current snapshot
    // Enemy fade: reveal tick per id; enemies fade to nothing over kFadeMs after the
    // ping first reaches them. RevealedThisSweep prevents re-arming mid-sweep.
    std::unordered_map<uint16_t, double>       RevealTime; // high-res ms
    std::unordered_set<uint16_t>               RevealedThisSweep;
    static constexpr float kFadeMs = 1000.0f;  // enemy fade duration

    // Projectile streak state: last two published world positions per id so we can draw
    // a short motion streak, advanced only when ProjCounter changes (~15 Hz).
    uint32_t                                       LastProj = 0xFFFFFFFF;
    // Wall-clock of the last projectile publish and the measured gap to the one before it.
    // The MEASURED gap is used rather than the nominal interval because publishes arrive
    // irregularly, and dividing by the wrong dt gives a wrong velocity.
    double                                         LastProjPublishMs = 0.0;
    double                                         ProjPublishDeltaMs = 0.0;
    // Display-only smoothing between publishes. Never feeds the incoming/closing tests -
    // those stay on published data, same principle as DeadProj.
    bool                                           ProjSmoothing = true;
    std::unordered_map<uint16_t, std::pair<float,float>> ProjPrev;       // latest update's pos (XY)
    std::unordered_map<uint16_t, std::pair<float,float>> ProjStreakFrom; // one update earlier (XY)
    std::unordered_map<uint16_t, float>            ProjPrevZ;            // parallel Z (latest update)
    std::unordered_map<uint16_t, float>            ProjStreakFromZ;      // parallel Z (one earlier)
    // Missile smoke: recent world positions per missile id (fading gray trail). MissileTrailZ is a
    // parallel altitude history (same indices) so the closing test can measure 3D distance - a
    // vertically-dropping Arrow IV closes in Z with almost no XY change, which an XY-only test misses.
    std::unordered_map<uint16_t, std::vector<std::pair<float,float>>> MissileTrail;
    std::unordered_map<uint16_t, std::vector<float>>                  MissileTrailZ;
    static constexpr int kTrailLen = 12; // trail length in samples
    // How long a trail survives its round vanishing from the published array.
    //
    // NOT a cosmetic preference and deliberately not user-adjustable: it is one half of a
    // CROSS-BINARY INVARIANT. The plugin recycles projectile ids, and holds a freed id in
    // quarantine (LidarExport kIdQuarantineMs) for longer than this value before handing it
    // to a new round. That ordering is what stops a fresh missile inheriting a dead one's
    // trail and being drawn with a smoke line leading back to where it never flew.
    //
    // RAISING THIS ABOVE THE PLUGIN'S QUARANTINE REINTRODUCES THAT BUG, silently, and only
    // in heavy combat where ids recycle fast. If this ever needs to grow, grow the plugin's
    // quarantine first.
    //
    // It is not zero because rounds legitimately fall out of one publish - the array is
    // truncated at LIDAR_MAX_PROJECTILES and the tail churns - and erasing on a single
    // absence wiped all 12 samples, leaving the trail and the closing test blind.
    static constexpr double kTrailRetainMs = 1000.0;
    // Landed-projectile cleanup: a pooled projectile actor lingers after impact (still enumerated
    // in-game, not yet despawned) and would keep reading as "active". If it hasn't moved since the
    // previous publish for kDeadFrames in a row, it has landed - hide it and stop counting it as
    // incoming. Renderer-only; the game keeps the actor, so this is safe.
    std::unordered_map<uint16_t, int> ProjStill;   // consecutive stationary publishes per id
    std::unordered_set<uint16_t>      DeadProj;     // ids currently landed/stationary -> hidden
    static constexpr int   kDeadFrames = 3;
    static constexpr float kStillEpsCm = 100.0f;    // < 1 m moved since last publish = stationary

    static constexpr float kPulseCmPerSec = 50000.0f; // 500 m/s outward ping

    // Display range (scope radius). Adjustable from the host; the plugin still scans to a
    // fixed 1000 m, so a range set beyond that is empty margin, not more contacts.
    float RangeCm = 100000.0f;

    // Fraction of the window the host keeps clear around the edge for its bezel legends.
    // The scope radius and the text overlays are pulled in by the same amount so the
    // whole view sits inside the bezel, exactly like the widget textures do.
    float Inset = 0.0f;
    LidarView::NavPoint NavPts[16] = {};
    LidarView::Dropship Ships[8] = {};
    int                 ShipN = 0;
    int                 NavN = 0;

    // Staleness watchdog. A stopped writer still leaves a self-consistent block, so
    // Snapshot keeps succeeding forever and the last frame would freeze on screen as
    // though it were live. Dying, escaping to the menu, ending the mission or closing the
    // game all stop the plugin publishing.
    uint32_t LastSeenFrame     = 0xFFFFFFFF;
    double   LastFrameChangeMs = 0.0;
    static constexpr double kDataStaleMs = 1500.0; // the fast tick is ~16 ms, so generous

    std::unordered_map<uint16_t, double> TrailSeenMs; // last publish each trail appeared in

    // --- threat alarm (provisional feature; see ThreatAlarm.h) --------------------------
    // Driven from Poll(), which the host calls every frame regardless of the active view,
    // so the warning sounds on the Weapons or Map page too. That is only affordable
    // because the plugin publishes the lock as a block-level flag: reading it costs a
    // handful of bytes, where the per-missile version needed the whole projectile array
    // and therefore tied the warning to the scope being on screen.
    ThreatAlarm ThreatSfx;
    uint32_t    ThreatLastFrame    = 0xFFFFFFFF;
    double      ThreatLastChangeMs = 0.0;

    // --- inbound salvo tracking ---------------------------------------------------------
    // There is no binding logic here any more, and there must not be. The plugin decides
    // per ROUND whether it belongs to a salvo guiding on us and says so in LPROJ_INCOMING;
    // this class only reads the flag.
    //
    // Everything that used to live here - a launch window, a spawn-gated first-round bind,
    // an absence timer, a maximum lifetime, an id-recycle backstop - existed to reconstruct
    // attribution the renderer could never actually derive. LidarProjectile.Id is a 16-bit
    // hash of the actor pointer: with ~250 rounds airborne two live rounds collide often,
    // and a pooled actor re-fired after impact hands its id straight to an unrelated round.
    // So the machinery was guessing, and the guard rails were there to bound how wrong the
    // guess could get. Attribution is now decided where the real pointer lives; the id is
    // back to being nothing but a drawing key.
    //
    // Consequence worth stating: this now tracks the WHOLE salvo, not one representative
    // round, so the alarm runs until the last attributed round is gone rather than stopping
    // when one arbitrary member lands.
    // NOTE there is no id-keyed mirror of the flagged rounds here, and there must not be.
    // One used to exist (ThreatMine, a std::unordered_set<uint16_t>): the publish loop turned
    // pj.Flags into a set of ids and the draw loop turned it back with ThreatMine.count(id).
    // Both loops walk the SAME array, so that round trip bought nothing and cost correctness
    // - LidarProjectile.Id is a 16-bit hash of the actor pointer, so any unflagged round
    // sharing an id with a flagged one was drawn as inbound. Worse than random: pooled actors
    // are allocated close together, so ids collide in clusters rather than independently.
    //
    // The alarm never showed this because it only asks whether the set is non-empty, and
    // laundering through ids does not change emptiness. The flash asks per round, so it
    // showed the errors immediately. Read pj.Flags directly; it is always to hand.
    int    ThreatMineCount     = 0;     // flagged rounds in the last publish (diagnostics)
    double ThreatSeenPublishMs = 0.0;  // last publish that carried projectile data
    bool   ThreatHold          = false; // at least one attributed round is alive
    // Range to the nearest attributed round; -1 when there is none. The single input that
    // selects the alarm's cadence.
    float  ThreatNearestCm     = -1.0f;
    // NOTE the close-range threshold is deliberately NOT here. ThreatAlarm::kCloseCm owns
    // it, because the two-cadence rule is that class's contract: a second opinion about what
    // "close" means, kept in the caller, is how the two would quietly drift apart. This
    // class publishes ThreatNearestCm and lets the alarm decide what to do with it.
    //
    // If the projectile array stops flowing (scope not the active view, or the projectile
    // layers are off) the flags are stale within a few frames and the alarm goes silent.
    // That is by design: without the array there is no sensor, and a warning with nothing
    // behind it is worse than none.
    static constexpr double kProjFreshMs = 500.0;
    // Radius multiplier for a round guiding on us, on the tone's ON frames.
    static constexpr float kSalvoFlashScale = 4.0f;

    // Render() stamps this; Poll() uses it to tell whether the scope is the live view and
    // therefore whether the expensive projectile scan should still be requested.
    double      LastRenderMs       = 0.0;
    static constexpr double kScopeActiveMs = 250.0;

    uint32_t      EnabledLayers = kDefaultLayers;
    // Projectile sample interval (ms), set by the host. Capped by
    // the plugin's 25 ms fast tick (~40 Hz), so 25 ms is the effective floor.
    // FIXED, and no longer user-adjustable. It used to be a 25-100 ms stepper in the options
    // panel, which quietly broke the landed-round detection on both sides of the MMF: those
    // tests measure "moved less than 1 m since the last sample", a DISTANCE per sample, so
    // halving the interval halves how far a live round travels between samples and a slow
    // one starts reading as landed. That retires it mid-flight and the alarm goes silent -
    // a failure with no visible cause, triggered by a slider the pilot had every reason to
    // think was only about smoothness. One rate, and the thresholds can rely on it.
    static constexpr uint32_t kProjIntervalMs = 33; // 30 Hz

public:
    void SetDropships(const LidarView::Dropship* d, int n) { // DROPSHIPS (2026-10-08)
        ShipN = (d && n > 0) ? (n < 8 ? n : 8) : 0;
        for (int i = 0; i < ShipN; ++i) Ships[i] = d[i];
    }
    void SetNavPoints(const LidarView::NavPoint* pts, int n) { // NAV POINTS (2026-10-07k)
        NavN = (pts && n > 0) ? (n < 16 ? n : 16) : 0;
        for (int i = 0; i < NavN; ++i) NavPts[i] = pts[i];
    }
    // ---- Host-facing surface ---------------------------------------------
    // Factories and text formats come from the host so there is exactly one D2D/DWrite
    // stack in the process. The render target arrives separately because it is torn
    // down and rebuilt around every swap-chain resize.
    void SetTerrainUnderlay(ITerrainUnderlay* t) { Terrain = t; }
    uint32_t TerrainSegments() const { return TerrainSegs; }

    bool InitResources(ID2D1Factory* factory, IDWriteFactory* dw) {
        Factory = factory;
        DW      = dw;
        if (!Factory || !DW) return false;
        DW->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             14.0f, L"en-us", &TextFmt);
        return true;
    }

    // Null on resize (before ResizeBuffers), the new target afterwards. The brush is
    // device-bound, so it is recreated alongside.
    void SetRenderTarget(ID2D1RenderTarget* rt) {
        if (RT == rt) return;
        SafeRelease(Brush);
        RT = rt;
        if (RT) RT->CreateSolidColorBrush(D2D1::ColorF(Col::Background), &Brush);
    }

    void Shutdown() {
        ThreatSfx.Shutdown();
        DropControl();
        ReleaseScope();
        if (ScopeMutex)   { CloseHandle(ScopeMutex);   ScopeMutex   = nullptr; }
        if (ControlMutex) { CloseHandle(ControlMutex); ControlMutex = nullptr; }
        SafeRelease(Brush);
        SafeRelease(TextFmt);
        if (Shared) { UnmapViewOfFile(Shared); Shared = nullptr; }
        if (Mmf)    { CloseHandle(Mmf);        Mmf    = nullptr; }
        RT = nullptr;
    }

    // Called every frame by the host, whether or not the LIDAR view is on screen, so the
    // MMF connects in the background. Throttled to ~1 Hz like the host's HUD-export poll:
    // unthrottled this was a failing OpenFileMapping 60 times a second for as long as the
    // game wasn't running, which is now a state the app can sit in indefinitely.
    ULONGLONG LastOpenAttempt = 0;
    void Poll() {
        if (!Shared) {
            const ULONGLONG tick = GetTickCount64();
            if (tick - LastOpenAttempt < 1000) return;
            LastOpenAttempt = tick;
            TryOpenMmf();
            // Re-baseline the staleness tracker on every (re)connect attempt, so a fresh
            // connection can never inherit the previous session's notion of "recent".
            ThreatLastFrame    = 0xFFFFFFFF;
            ThreatLastChangeMs = 0.0;
            if (!Shared) { ThreatSfx.Update(false, -1.0f, false, false, NowMs()); return; }
        }

        const double now = NowMs();

        // Sole writer of the control channel. This used to live in Render(), which meant
        // switching away from the scope stopped the plugin's probe entirely; doing it here
        // keeps the cheap threat request alive on every page while the expensive
        // projectile scan still stops the moment the scope is no longer being drawn.
        const bool alarmOn    = layerOn(VL_ThreatAlarm);
        const bool scopeLive  = (now - LastRenderMs) < kScopeActiveMs;
        const bool wantAny    = alarmOn || scopeLive || Far; // Far: the 3D / TacOps maps need the far contacts too

        // Publish whether this instance is drawing the scope, then stand aside if another
        // instance is and we are not - it has the array data and must be the one that owns
        // the control channel and sounds the alarm.
        if (scopeLive) AcquireScope(); else ReleaseScope();
        const bool yieldToScope = !scopeLive && AnotherInstanceHasScope();
        if (yieldToScope) {
            DropControl();
            ThreatSfx.Update(false, -1.0f, false, false, now);
            return;
        }

        // Nothing to ask the plugin for: let go of the mutex so a sibling instance whose
        // scope IS showing can take over. Holding it while idle would lock the other
        // instance out of the control channel for as long as this one runs.
        if (!wantAny) {
            DropControl();
            ThreatSfx.Update(false, -1.0f, false, false, now);
            return;
        }

        const bool owner = TryClaimControl();
        if (owner) {
            uint32_t ctrl = 0;
            if (alarmOn) ctrl |= LIDAR_CTRL_THREAT;
            // Not gated on scopeLive any more. We only reach here as the scope-showing
            // instance (or the only one), so the layer toggles alone decide.
            //
            // The alarm implies the scan: it is now driven entirely by bound rounds, so
            // "alarm on, projectile layers off" would otherwise be a silently dead config -
            // the warning enabled and structurally incapable of firing.
            const bool pilotRounds = AllRoundsMs && GetTickCount64() - AllRoundsMs < 2000;
            if (layerOn(VL_Projectiles) || layerOn(VL_Missiles) || alarmOn || pilotRounds)
                ctrl |= LIDAR_CTRL_PROJECTILES;
            if (Far) {
                ctrl |= LIDAR_CTRL_FAR;
                Shared->ControlPad = ++FarBeat; // heartbeat: the plugin drops the far ranges when this stops moving
            }
            Shared->ControlLayers         = ctrl;
            Shared->ControlProjIntervalMs = (uint16_t)kProjIntervalMs;
        }

        // Publisher-liveness read only. Neither ThreatFlags nor ThreatSalvos is consumed
        // here any more: the alarm's arming, sustain and cadence all come from the
        // per-round LPROJ_INCOMING flags on the render path. Both block fields are still
        // published, and GetDiag() still reports the lock, but only as diagnostics.
        uint32_t frame = 0;
        bool     got   = false;
        for (int t = 0; t < 8; ++t) {
            const uint32_t e = *(volatile const uint32_t*)&Shared->FrameCounterEnd;
            MemoryBarrier();
            frame  = *(volatile const uint32_t*)&Shared->FrameCounter;
            MemoryBarrier();
            if (frame == e) { got = true; break; }
        }

        // Same staleness rule as the scope: a stopped publisher leaves a perfectly
        // self-consistent block behind, so "the read succeeded" proves nothing about
        // whether the game is still running. Only an ADVANCING FrameCounter does.
        // The FIRST read only establishes a baseline - it never counts as freshness. If
        // the game crashed while a salvo was tracking us, the abandoned block still carries
        // flagged rounds and is perfectly self-consistent; trusting it on connect would
        // greet the pilot with an alarm for nothing. Requiring one observed advance costs
        // ~200 ms at the plugin's fast tick and removes that case entirely.
        if (got) {
            if (ThreatLastFrame == 0xFFFFFFFF) {
                ThreatLastFrame = frame;          // baseline only - not yet trusted
            } else if (frame != ThreatLastFrame) {
                ThreatLastFrame    = frame;
                ThreatLastChangeMs = now;
            }
        }
        const bool fresh = got && ThreatLastChangeMs > 0.0 &&
                           (now - ThreatLastChangeMs) <= kDataStaleMs;

        // The projectile array only flows while the scope is drawing with a projectile
        // layer on. When it stops, the flags are stale within a few frames and the alarm
        // goes silent - by design. Without the array there is no sensor, and there is no
        // lock-only fallback: a warning with nothing behind it is worse than none.
        // LIDAR_CTRL_THREAT is consequently vestigial - nothing consumes the block flag
        // without the array.
        const bool projFresh = ThreatSeenPublishMs > 0.0 &&
                               (now - ThreatSeenPublishMs) < kProjFreshMs;
        if (!projFresh && ThreatMineCount > 0) ClearThreatState();

        // THE test, in full: is at least one round that the plugin attributed to us still
        // alive? Not the lock - a MWMissilePath winks out mid-flight and lingers after
        // impact, and both of those were audible bugs. A round's lifetime IS the threat's
        // lifetime, and the whole salvo is tracked, so the tone runs until the last of the
        // volley is gone rather than stopping when one arbitrary member lands.
        const bool sustain = projFresh && ThreatHold;

        // Range to the nearest attributed round, or -1 when the array is not flowing or
        // nothing is attributed. -1 means "no measurement", NOT "far": the alarm holds its
        // current cadence through it rather than stepping down for the final few frames of
        // an episode, which would be a rate change carrying no information.
        const float nearestCm = projFresh ? ThreatNearestCm : -1.0f;

        // Only the control owner sounds the alarm - otherwise two HUDDisplay instances
        // beep out of phase at the same pilot.
        ThreatSfx.Update(sustain, nearestCm, fresh, alarmOn && OwnsControl, now);

        // Last, because it consumes everything above: one tick of the scene state machine,
        // whichever view is on screen. See UpdateScene.
        UpdateScene();
    }

    bool IsConnected() const { return Shared != nullptr; }

    uint32_t GetLayers()         const { return EnabledLayers; }

    void SetLayers(uint32_t v) { EnabledLayers = v; }

    // Volume is applied straight to the open device, so a slider drag mid-combat costs
    // nothing and re-synthesises nothing.
    void SetAlarmVolume(int pct)   { ThreatSfx.SetVolume(pct); }
    void SetAlarmSound(int s)      { ThreatSfx.SetSound(s); }
    int  GetAlarmVolume() const    { return ThreatSfx.GetVolume(); }
    bool IsAlarmSounding() const   { return ThreatSfx.IsSounding(); }

    LidarView::Diag GetDiag() const {
        LidarView::Diag d{};
        d.Connected    = Shared != nullptr;
        d.OwnsControl  = OwnsControl;
        d.HoldsScope   = HoldsScope;
        d.ProjRequested = Shared ? ((Shared->ControlLayers & LIDAR_CTRL_PROJECTILES) != 0) : false;
        d.Locked       = Shared ? ((Shared->ThreatFlags & LTHREAT_LOCKED) != 0) : false;
        d.BoundRounds  = ThreatMineCount;
        d.BoundAny     = ThreatHold;
        d.Sounding     = ThreatSfx.IsSounding();
        return d;
    }

    bool GetProjectileSmoothing() const { return ProjSmoothing; }
    void SetProjectileSmoothing(bool on) { ProjSmoothing = on; }

    // COMPASS (2026-10-12): the ring, clock positions and rim dots - pushed in by the host (GlobalSettings + CMPS).
    bool CmpRing = false, CmpClock = false, CmpDots = false;
    void SetCompass(bool ring, bool clock, bool dots) { CmpRing = ring; CmpClock = clock; CmpDots = dots; }

    // RADAR MODE (2026-10-12): the host's hostile-contact filter (Vanilla mode); null = Full LIDAR.
    LidarView::ContactKeepFn KeepFn = nullptr;
    void* KeepCtx = nullptr;
    void SetContactFilter(LidarView::ContactKeepFn fn, void* ctx) { KeepFn = fn; KeepCtx = ctx; }

    void SetContentInset(float inset) {
        if (inset < 0.0f)  inset = 0.0f;
        if (inset > 0.25f) inset = 0.25f;
        Inset = inset;
    }

    // Scope range in metres, stepped by the host. Clamped to the supported band.
    uint32_t GetRangeM() const { return (uint32_t)(RangeCm / 100.0f + 0.5f); }
    void SetRangeM(uint32_t metres) {
        if (metres < LidarView::kMinRangeM) metres = LidarView::kMinRangeM;
        const uint32_t top = Far ? LidarView::kMaxRangeFarM : LidarView::kMaxRangeM;
        if (metres > top) metres = top;
        RangeCm = (float)metres * 100.0f;
    }
    // THE 5 KM RADAR MODE (2026-10-15): the plugin's far ranges are asked for while this is on (Poll)
    bool     Far     = false;
    uint16_t FarBeat = 0;
    void SetFar(bool on) {
        Far = on;
        if (!on && RangeCm > LidarView::kMaxRangeM * 100.0f) RangeCm = LidarView::kMaxRangeM * 100.0f;
    }

    // The host calls this when the scope stops being the active view. Poll() already drops
    // the projectile bit on its next tick once Render() stops stamping LastRenderMs, so
    // the expensive scan ends either way - and if the alarm is on we deliberately KEEP
    // ownership, because the threat probe surviving a page change is the entire point of
    // moving it off the render path.
    void ReleaseControl() {
        if (layerOn(VL_ThreatAlarm)) return;
        DropControl();
    }

    static double NowMs() {
        static const double freq = [] {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            return (double)f.QuadPart / 1000.0; // ticks per millisecond
        }();
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (double)c.QuadPart / freq;
    }

private:
    void TryOpenMmf() {
        // Read + write: we read the export body and write ControlLayers back.
        Mmf = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, LIDAR_MMF_NAME);
        if (!Mmf) return;
        Shared = static_cast<LidarExportBlock*>(
            MapViewOfFile(Mmf, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LidarExportBlock)));
        if (!Shared) { CloseHandle(Mmf); Mmf = nullptr; return; }
        printf("[LidarView] connected to %S\n", LIDAR_MMF_NAME);
    }

    // Seqlock snapshot: read End first, copy, read Counter last; consistent iff equal.
    bool Snapshot(LidarExportBlock& out) {
        if (!Shared) return false;
        for (int t = 0; t < 8; ++t) {
            const uint32_t e = *(volatile const uint32_t*)&Shared->FrameCounterEnd;
            MemoryBarrier();
            memcpy(&out, Shared, sizeof(out));
            MemoryBarrier();
            const uint32_t b = *(volatile const uint32_t*)&Shared->FrameCounter;
            if (b == e && out.Magic == LIDAR_MAGIC) return true;
        }
        return false;
    }

    void SetColor(UINT32 rgb, float a = 1.0f) { if (Brush) Brush->SetColor(D2D1::ColorF(rgb, a)); }
    // Refresh the inbound-salvo state from one projectile publish.
    //
    // This is a READ of the plugin's decision, not a decision of its own. `mine` holds the
    // ids the plugin flagged LPROJ_INCOMING - rounds it attributed to a salvo guiding on us
    // at the moment they spawned, by actor pointer. Nothing here may add to that set or
    // hold an id past the publish that stopped carrying it: the whole class of bugs this
    // replaced came from the renderer trying to maintain membership across publishes using
    // a 16-bit id that is neither unique nor stable.
    //
    // Rebuilt from scratch each publish. That is what makes retirement automatic - a round
    // that impacted is simply absent, so it is gone from the set with no timer to expire.
    void UpdateThreatState(int mineCount, float mineNearestCm, double now)
    {
        ThreatMineCount     = mineCount;
        ThreatNearestCm     = mineNearestCm;
        ThreatHold          = mineCount > 0;
        ThreatSeenPublishMs = now;
    }

    void ClearThreatState()
    {
        ThreatMineCount = 0;
        ThreatHold      = false;
        ThreatNearestCm = -1.0f;
    }


    // ---- View layers ------------------------------------------------------
    bool layerOn(ViewLayer l) const { return (EnabledLayers >> l) & 1u; }

    // Small text helper (mirrors the readout's layout+draw).
    void DrawLabel(float x, float y, float w, const wchar_t* s, UINT32 color, float a = 1.0f) {
        if (!TextFmt || !DW || !RT) return;
        SetColor(color, a);
        IDWriteTextLayout* layout = nullptr;
        if (SUCCEEDED(DW->CreateTextLayout(s, (UINT32)wcslen(s), TextFmt, w, 22.0f, &layout))) {
            RT->DrawTextLayout(D2D1::Point2F(x, y), layout, Brush);
            layout->Release();
        }
    }


    // filled=true fills the triangle; filled=false draws just the outline (hollow).
    void DrawTriangle(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c,
                      bool filled, float stroke = 1.6f) {
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(Factory->CreatePathGeometry(&geo))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(geo->Open(&sink))) {
            sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
            D2D1_POINT_2F pts[2] = { b, c };
            sink->AddLines(pts, 2);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            if (filled) RT->FillGeometry(geo, Brush);
            else        RT->DrawGeometry(geo, Brush, stroke);
        }
        geo->Release();
    }

    void FillTriangle(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c) {
        DrawTriangle(a, b, c, true);
    }

    // Draw one contact at its world position, projected heading-up against the given
    // player transform: hollow orientation triangle (mech full size, vehicles small),
    // weight-class dots for mechs, and a highlight ring if it's the locked target.
    // Returns false if the contact lies outside the current scope range, so nothing is
    // ever painted past the outer ring and the contact count matches what is visible.
    bool DrawContact(const LidarContact& c, float cx, float cy, float radX, float radY,
                     float playerX, float playerY, float playerYaw, float cs, float sn,
                     bool selected, float alpha = 1.0f) {
        const float dx = c.X - playerX, dy = c.Y - playerY;
        if (dx * dx + dy * dy > RangeCm * RangeCm) return false;
        const float fwd   =  dx * cs + dy * sn;
        const float right = -dx * sn + dy * cs;
        const float sx = cx + (right / RangeCm) * radX;
        const float sy = cy - (fwd   / RangeCm) * radY;

        UINT32 color = Col::Unknown;
        switch (c.Type) {
            case LCT_FriendlyMech: color = Col::Friendly; break;
            case LCT_HostileMech:  color = Col::Hostile;  break;
            case LCT_NeutralMech:  color = Col::Neutral;  break;
            default:               color = Col::Unknown;  break;
        }

        // Orientation triangle (hollow). Facing is relative to the player heading.
        const float a  = (c.Yaw - playerYaw) * (float)(M_PI / 180.0);
        const float fx = std::sin(a),  fy = -std::cos(a); // forward (screen)
        const float px = std::cos(a),  py =  std::sin(a); // right (perp)
        const float scale = (c.Kind == LK_Mech) ? 0.8f : 0.39f;
        const float len = 12.0f * scale, back = 7.5f * scale, wid = 6.5f * scale;
        const D2D1_POINT_2F tip = D2D1::Point2F(sx + fx * len,  sy + fy * len);
        const D2D1_POINT_2F bl  = D2D1::Point2F(sx - fx * back + px * wid, sy - fy * back + py * wid);
        const D2D1_POINT_2F br  = D2D1::Point2F(sx - fx * back - px * wid, sy - fy * back - py * wid);
        SetColor(color, alpha);
        if (c.Kind == LK_BattleArmor) {
            // 2026-10-17 (user): a battle armour suit - a small filled diamond (facing means nothing at this size)
            const float r = 3.0f;
            const D2D1_POINT_2F dt = D2D1::Point2F(sx, sy - r), db = D2D1::Point2F(sx, sy + r);
            const D2D1_POINT_2F dl = D2D1::Point2F(sx - r, sy), dr = D2D1::Point2F(sx + r, sy);
            DrawTriangle(dt, dl, dr, /*filled=*/true, 1.0f);
            DrawTriangle(db, dl, dr, /*filled=*/true, 1.0f);
        } else
            DrawTriangle(tip, bl, br, /*filled=*/false, (c.Kind == LK_Mech) ? 1.8f : 1.2f);

        // Weight-class dots (mechs only): medium=1, heavy=2, assault=3; light=0.
        if (layerOn(VL_WeightDots) && c.Kind == LK_Mech && c.Weight >= LW_Medium && c.Weight <= LW_Assault) {
            const int dots = (int)c.Weight;
            const float dotR = 1.5f, spacing = 3.2f;
            const float start = -(dots - 1) * 0.5f * spacing + 1.0f;
            for (int d = 0; d < dots; ++d) {
                const float off = start + d * spacing;
                RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx + fx * off, sy + fy * off), dotR, dotR), Brush);
            }
        }

        // Locked target: bright ring centered on the unit (either team). Driven by the
        // block's SelectedIds (fast tick), not the per-contact flag, so it tracks
        // selection changes at transform rate rather than the 3 s scan.
        if (selected && layerOn(VL_TargetHighlight)) {
            SetColor(Col::Selected, 0.95f * alpha);
            const float ringR = (c.Kind == LK_Mech) ? 14.0f : 7.0f;
            RT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), ringR, ringR), Brush, 2.0f);
        }
        return true;
    }

public:
    // -------------------------------------------------------------------------------------
    // ONE TICK OF THE SCENE, called from Poll() every frame whatever view is up.
    //
    // Snapshot, staleness, ping sweep advance, track reveal and expiry. Every line of this was
    // lifted verbatim out of Render(); none of the thresholds or the ordering changed, because
    // this is the part of the scope that took two missions to get right and rewriting it to
    // gain a second renderer would have been a poor trade.
    // -------------------------------------------------------------------------------------
    void UpdateScene() {
        LidarExportBlock blk{};
        bool haveData = Snapshot(blk);

        // Treat a publisher that has stopped advancing as no data at all, so the scope
        // falls back to its "waiting for game" state instead of freezing the last frame.
        if (haveData) {
            const double now = NowMs();
            if (blk.FrameCounter != LastSeenFrame) {
                LastSeenFrame     = blk.FrameCounter;
                LastFrameChangeMs = now;
            } else if (LastFrameChangeMs > 0.0 && now - LastFrameChangeMs > kDataStaleMs) {
                haveData = false;
            }
        }

        // RADAR MODE (2026-10-12): Vanilla drops the hostiles the game's own detection does not have, here at
        // the one place every consumer (scope, 3D / TacOps scene, rim dots) reads from. A dropped contact
        // behaves exactly like one that left the scan: its track clears when the ping next sweeps its spot.
        if (haveData && KeepFn) {
            const uint32_t cnt = blk.ContactCount < LIDAR_MAX_CONTACTS ? blk.ContactCount : LIDAR_MAX_CONTACTS;
            uint32_t n = 0;
            for (uint32_t i = 0; i < cnt; ++i) {
                const LidarContact& c = blk.Contacts[i];
                if (c.Type == LCT_FriendlyMech || c.Selected || KeepFn(KeepCtx, c.X, c.Y, c.Z))
                    blk.Contacts[n++] = c;
            }
            blk.ContactCount = n;
        }

        // Advance the ping sweep: a new snapshot (ScanCounter change) restarts the outward
        // pulse. pulseCm is how far the ring has travelled (500 m/s).
        if (haveData && blk.ScanCounter != LastScan) {
            LastScan   = blk.ScanCounter;
            PulseStart = NowMs();
            // Remember which ids this scan contains, so tracks that vanished can be cleared
            // once the ping sweeps past where they used to be.
            CurIds.clear();
            for (uint32_t i = 0; i < blk.ContactCount && i < LIDAR_MAX_CONTACTS; ++i)
                CurIds.insert(blk.Contacts[i].Id);
            RevealedThisSweep.clear(); // re-arm fades for the new sweep
        }
        // 5 km mode (2026-10-15): the ping crosses the whole scope in the same 2 s whatever the range, so far contacts
        // are still revealed before the next scan (500 m/s would take 10 s to reach 5 km - longer than a scan).
        const float pulseCm = PulseStart
            ? (float)((NowMs() - PulseStart) / 1000.0 * kPulseCmPerSec * (RangeCm > 100000.0f ? RangeCm / 100000.0f : 1.0f))
            : 0.0f;

        if (haveData) {
            const double now = NowMs();
            for (uint32_t i = 0; i < blk.ContactCount && i < LIDAR_MAX_CONTACTS; ++i) {
                const LidarContact& c = blk.Contacts[i];
                const float dx = c.X - blk.PlayerX, dy = c.Y - blk.PlayerY;
                if (std::sqrt(dx * dx + dy * dy) <= pulseCm) {
                    Tracks[c.Id] = c; // reveal / refresh to the new position
                    if (!RevealedThisSweep.count(c.Id)) { // first crossing this sweep
                        RevealedThisSweep.insert(c.Id);
                        RevealTime[c.Id] = now;           // (re)start its fade
                    }
                }
            }
            for (auto it = Tracks.begin(); it != Tracks.end(); ) {
                const float dx = it->second.X - blk.PlayerX, dy = it->second.Y - blk.PlayerY;
                if (!CurIds.count(it->first) && std::sqrt(dx * dx + dy * dy) <= pulseCm) {
                    RevealTime.erase(it->first);
                    it = Tracks.erase(it); // gone target, ping swept past its old spot
                } else {
                    ++it;
                }
            }
        }

        // ---- per-publish projectile bookkeeping ------------------------------------------
        //
        // Streak sources, landed-round detection and missile smoke trails. Every line of this
        // was lifted verbatim out of Render(), for the second time in this file and for exactly
        // the same reason the ping-reveal machinery moved: it is STATE, not drawing, and state
        // that only advances while one particular page is on screen is not state at all.
        //
        // Leaving it behind is what produced three separate-looking reports from the 3D page -
        // rounds that never despawned (DeadProj never repopulated), missiles with no smoke
        // (MissileTrail never appended to) and ballistics with no streak (ProjStreakFrom never
        // shifted). One omission, three symptoms, none of them obviously related.
        if (haveData && (layerOn(VL_Projectiles) || layerOn(VL_Missiles) || layerOn(VL_ThreatAlarm))) {
            auto isMissileType = [](uint8_t t) { return t == LPT_Missile || t == LPT_ArrowIV; };
        if (blk.ProjCounter != LastProj) {
            LastProj = blk.ProjCounter;
            {
                const double pubNow = NowMs();
                if (LastProjPublishMs > 0.0) ProjPublishDeltaMs = pubNow - LastProjPublishMs;
                LastProjPublishMs = pubNow;
            }
            ProjStreakFrom = ProjPrev; ProjStreakFromZ = ProjPrevZ; // shift: prev update = streak source
            ProjPrev.clear(); ProjPrevZ.clear();
            DeadProj.clear();
            std::unordered_set<uint16_t> curMissiles;
            std::unordered_map<uint16_t, ProjPos> curMissilePos;   // for the close-in test
            int   mineCount   = 0;      // rounds flagged LPROJ_INCOMING this publish
            float mineNearest = -1.0f;  // range to the nearest of them, cm
            for (uint32_t i = 0; i < blk.ProjectileCount && i < LIDAR_MAX_PROJECTILES; ++i) {
                const LidarProjectile& pj = blk.Projectiles[i];
                // Landed detection: no movement since the previous publish. After kDeadFrames
                // in a row the round has impacted (pooled actor lingering) -> mark it dead.
                // 3D so a round dropping vertically (Arrow IV) isn't mistaken for landed.
                auto pf = ProjStreakFrom.find(pj.Id);
                if (pf != ProjStreakFrom.end()) {
                    const float mdx = pj.X - pf->second.first, mdy = pj.Y - pf->second.second;
                    auto pfz = ProjStreakFromZ.find(pj.Id);
                    const float mdz = (pfz != ProjStreakFromZ.end()) ? pj.Z - pfz->second : 0.0f;
                    if (mdx * mdx + mdy * mdy + mdz * mdz < kStillEpsCm * kStillEpsCm) {
                        if (++ProjStill[pj.Id] >= kDeadFrames) DeadProj.insert(pj.Id);
                    } else ProjStill[pj.Id] = 0;
                }
                ProjPrev[pj.Id] = { pj.X, pj.Y }; ProjPrevZ[pj.Id] = pj.Z;
                if (isMissileType(pj.Type) && !DeadProj.count(pj.Id)) {
                    curMissiles.insert(pj.Id);
                    curMissilePos[pj.Id]  = { pj.X, pj.Y, pj.Z };
                    // The plugin's verdict on this round, decided at its spawn from the
                    // actor pointer. DeadProj is still honoured on top of it so a landed
                    // round the plugin has not yet seen impact cannot hold the alarm.
                    // Consumed here as a flag and never converted into an id.
                    if (pj.Flags & LPROJ_INCOMING) {
                        ++mineCount;
                        const float mdx2 = pj.X - blk.PlayerX;
                        const float mdy2 = pj.Y - blk.PlayerY;
                        const float mdz2 = pj.Z - blk.PlayerZ;
                        const float md = std::sqrt(mdx2 * mdx2 + mdy2 * mdy2 + mdz2 * mdz2);
                        if (mineNearest < 0.0f || md < mineNearest) mineNearest = md;
                    }
                    auto& tr  = MissileTrail[pj.Id];  tr.push_back({ pj.X, pj.Y });
                    auto& trz = MissileTrailZ[pj.Id]; trz.push_back(pj.Z);
                    if ((int)tr.size()  > kTrailLen) tr.erase(tr.begin());
                    if ((int)trz.size() > kTrailLen) trz.erase(trz.begin());
                }
            }
            // Age trails out rather than erasing on a single absence. One missed
            // publish used to wipe all 12 samples, leaving the closing test blind for
            // at least another publish - and under LIDAR_MAX_PROJECTILES truncation the
            // same rounds fall in and out of the array tail, so their trails could
            // never reach the 2 samples the test needs.
            {
                const double nowT = NowMs();
                for (uint16_t id : curMissiles) TrailSeenMs[id] = nowT;
                for (auto it = MissileTrail.begin(); it != MissileTrail.end(); ) {
                    auto seen = TrailSeenMs.find(it->first);
                    const bool stale = (seen == TrailSeenMs.end()) || (nowT - seen->second > kTrailRetainMs);
                    it = stale ? MissileTrail.erase(it) : std::next(it);
                }
                for (auto it = MissileTrailZ.begin(); it != MissileTrailZ.end(); ) {
                    auto seen = TrailSeenMs.find(it->first);
                    const bool stale = (seen == TrailSeenMs.end()) || (nowT - seen->second > kTrailRetainMs);
                    it = stale ? MissileTrailZ.erase(it) : std::next(it);
                }
                for (auto it = TrailSeenMs.begin(); it != TrailSeenMs.end(); )
                    it = (nowT - it->second > kTrailRetainMs) ? TrailSeenMs.erase(it) : std::next(it);
            }
            // Prune the stationary counters for ids no longer present.
            for (auto it = ProjStill.begin(); it != ProjStill.end(); )
                it = ProjPrev.count(it->first) ? std::next(it) : ProjStill.erase(it);

            // Hand the plugin's per-round verdict straight to the alarm state. No
            // attribution happens on this side - see UpdateThreatState.
            UpdateThreatState(mineCount, mineNearest, NowMs());
        }
        }

        SceneBlk     = blk;
        SceneHave    = haveData;
        ScenePulseCm = pulseCm;
    }

    // Flatten what UpdateScene left into a renderer-agnostic list: every contact that should be
    // visible, with its ping fade and selection already resolved. A second renderer has no
    // business re-deriving any of that, and a POD list keeps LidarShared.h out of the header.
    ULONGLONG AllRoundsMs = 0; // last BuildScene(allRounds) - keeps the projectile scan asked for
    void BuildScene(LidarScene& out, bool allRounds = false) {
        if (allRounds) AllRoundsMs = GetTickCount64();
        auto roundsOn = [&](ViewLayer l) { return allRounds || layerOn(l); };
        SceneContacts.clear();
        SceneProjectiles.clear();
        SceneTrail.clear();

        out = LidarScene{};
        out.Valid = SceneHave;
        out.RangeCm = (float)RangeCm;
        out.Layers = EnabledLayers;
        out.PulseCm = ScenePulseCm;
        if (!SceneHave) return;

        const LidarExportBlock& blk = SceneBlk;
        out.PlayerX = blk.PlayerX; out.PlayerY = blk.PlayerY; out.PlayerZ = blk.PlayerZ;
        out.PlayerYaw = blk.PlayerYaw; out.PlayerTorsoYaw = blk.PlayerTorsoYaw;

        const double now = NowMs();
        auto push = [&](const LidarContact& c, bool sel, float alpha) {
            LidarSceneContact s{};
            s.X = c.X; s.Y = c.Y; s.Z = c.Z; s.Yaw = c.Yaw;
            s.Type = c.Type; s.Kind = c.Kind; s.Weight = c.Weight;
            s.Selected = sel ? 1 : 0; s.Alpha = alpha;
            s.Id = c.Id;
            SceneContacts.push_back(s);
        };

        for (auto& kv : Tracks) {
            const bool isMech = kv.second.Kind == LK_Mech;
            if (isMech  && !layerOn(VL_Mechs))    continue;
            if (!isMech && !layerOn(VL_Vehicles)) continue;
            auto rt = RevealTime.find(kv.first);
            if (rt == RevealTime.end()) continue;
            const float alpha = 1.0f - (float)((now - rt->second) / kFadeMs);
            if (alpha <= 0.0f) continue;
            bool sel = false;
            for (uint8_t k = 0; k < blk.SelectedCount && k < 4; ++k)
                if (blk.SelectedIds[k] == kv.first) { sel = true; break; }
            push(kv.second, sel, alpha);
        }
        for (uint32_t i = 0; i < blk.AllyCount && i < LIDAR_MAX_ALLIES; ++i) {
            const LidarContact& a = blk.Allies[i];
            const bool isMech = a.Kind == LK_Mech;
            if (isMech  && !layerOn(VL_Mechs))    continue;
            if (!isMech && !layerOn(VL_Vehicles)) continue;
            push(a, a.Selected != 0, 1.0f);
        }

        // Rounds, styled the way the scope styles them. Every judgement the 2D renderer makes -
        // dead rounds hidden, range clip, smoothing, streak origin, salvo flash in phase with
        // the tone - is made HERE, once, so the two pages cannot disagree about a round.
        if (roundsOn(VL_Projectiles) || roundsOn(VL_Missiles)) {
            // NOTE what is deliberately absent: a range filter.
            //
            // The scope clips rounds to its disc because it has to - there is nowhere on a flat
            // disc to draw something beyond the rim. That is a RENDERING constraint, and baking
            // it into the shared scene made it a DATA constraint for every renderer. The 3D page
            // then lost rounds the plugin was still happily reporting, purely because the pilot
            // had zoomed the camera in: the zoom control was quietly doubling as a sensor filter.
            //
            // So the scene now carries everything the plugin publishes (it scans a fixed 1000 m
            // regardless of what the scope is set to) and each renderer clips for itself. The 2D
            // scope is unaffected - it draws from blk.Projectiles directly and does its own
            // clipping, exactly as before.
            const bool alarmBeep = ThreatSfx.IsBeeping(now);
            // A pooled id reused for a fresh round teleports; the same guard the scope uses.
            constexpr float kJump = 15000.0f;

            for (uint32_t i = 0; i < blk.ProjectileCount && i < LIDAR_MAX_PROJECTILES; ++i) {
                const LidarProjectile& pj = blk.Projectiles[i];
                if (DeadProj.count(pj.Id)) continue;
                const bool isMissile = (pj.Type == LPT_Missile || pj.Type == LPT_ArrowIV);
                if (isMissile  && !roundsOn(VL_Missiles))    continue;
                if (!isMissile && !roundsOn(VL_Projectiles)) continue;

                LidarSceneProjectile s{};
                s.Type = pj.Type;
                s.Flags = pj.Flags;
                s.IsMissile = isMissile ? 1 : 0;
                s.IsArrowIV = (pj.Type == LPT_ArrowIV) ? 1 : 0;
                s.ColorIdx = isMissile ? 2 : ((pj.Type == LPT_PPC) ? 1 : 0);
                s.Flashing = (isMissile && alarmBeep && (pj.Flags & LPROJ_INCOMING) != 0) ? 1 : 0;
                s.TrailStart = -1;
                s.TrailCount = 0;
                s.X = pj.X; s.Y = pj.Y; s.Z = pj.Z;

                auto prev = ProjStreakFrom.find(pj.Id);
                const bool haveStreak = (prev != ProjStreakFrom.end());
                float ddx = 0.0f, ddy = 0.0f, ddz = 0.0f;
                if (haveStreak) {
                    ddx = pj.X - prev->second.first;
                    ddy = pj.Y - prev->second.second;
                    auto pz = ProjStreakFromZ.find(pj.Id);
                    ddz = (pz != ProjStreakFromZ.end()) ? (pj.Z - pz->second) : 0.0f;
                    if (ddx * ddx + ddy * ddy < kJump * kJump) {
                        s.HasStreak = 1;
                        s.FromX = prev->second.first;
                        s.FromY = prev->second.second;
                        s.FromZ = (pz != ProjStreakFromZ.end()) ? pz->second : pj.Z;
                    }
                }
                // Same extrapolation as the scope, extended to Z because a 3D view shows the arc
                // and a round that lags in height reads as floating.
                if (ProjSmoothing && ProjPublishDeltaMs > 1.0 && haveStreak &&
                    ddx * ddx + ddy * ddy < kJump * kJump) {
                    double age = NowMs() - LastProjPublishMs;
                    if (age < 0.0) age = 0.0;
                    if (age > ProjPublishDeltaMs) age = ProjPublishDeltaMs;
                    const float f = (float)(age / ProjPublishDeltaMs);
                    s.X = pj.X + ddx * f;
                    s.Y = pj.Y + ddy * f;
                    s.Z = pj.Z + ddz * f;
                }

                if (isMissile) {
                    auto tt = MissileTrail.find(pj.Id);
                    auto tz = MissileTrailZ.find(pj.Id);
                    if (tt != MissileTrail.end() && tt->second.size() >= 2) {
                        const auto& pts = tt->second;
                        // EVERY point is emitted, with Break marking the segments the scope
                        // would not have drawn. Keeping the sequence whole is what preserves
                        // both the gaps and the fade ramp; see LidarSceneTrailPt::Break.
                        const int start = (int)SceneTrail.size();
                        int drawable = 0;
                        for (size_t k = 0; k < pts.size(); ++k) {
                            LidarSceneTrailPt tp{};
                            tp.X = pts[k].first;
                            tp.Y = pts[k].second;
                            tp.Z = (tz != MissileTrailZ.end() && k < tz->second.size()) ? tz->second[k] : pj.Z;
                            tp.Break = 1;
                            if (k > 0) {
                                const float wdx = pts[k].first - pts[k-1].first;
                                const float wdy = pts[k].second - pts[k-1].second;
                                // Only the jump guard survives here. The scope's other test -
                                // clip at the rim so a missile arriving from outside does not
                                // trail a line across the surround - is a disc problem, and a
                                // 3D view has no rim for a trail to cross.
                                const bool jumped = (wdx * wdx + wdy * wdy >= kJump * kJump);
                                if (!jumped) { tp.Break = 0; ++drawable; }
                            }
                            SceneTrail.push_back(tp);
                        }
                        if (drawable >= 1) { s.TrailStart = start; s.TrailCount = (int)pts.size(); }
                        else SceneTrail.resize((size_t)start);
                    }
                }
                SceneProjectiles.push_back(s);
            }
        }

        out.ContactCount = (int)SceneContacts.size();
        out.Contacts = SceneContacts.empty() ? nullptr : SceneContacts.data();
        out.ProjectileCount = (int)SceneProjectiles.size();
        out.Projectiles = SceneProjectiles.empty() ? nullptr : SceneProjectiles.data();
        out.TrailPointCount = (int)SceneTrail.size();
        out.Trail = SceneTrail.empty() ? nullptr : SceneTrail.data();
    }

    // A renderer other than this one is on screen and wants the data to keep flowing. Without
    // this the control channel stops asking for the projectile scan the moment the 2D scope
    // stops drawing, and the 3D page would quietly lose projectiles and missiles.
    void NotifyRendered() { LastRenderMs = NowMs(); }

    // The host drives this; everything above stays private to the port.
    void Render() {
        if (!RT || !Brush) return;   // host has torn the target down (resize)

        // The control channel is written by Poll(), which runs on every frame whether or
        // not the scope is the active view - the alarm needs the probe requested even when
        // this view is not drawing. All Render() does is mark that the scope IS live, so
        // Poll knows to keep asking for the expensive projectile scan.
        LastRenderMs = NowMs();

        RT->BeginDraw();
        RT->Clear(D2D1::ColorF(Col::Background));

        const D2D1_SIZE_F sz = RT->GetSize();
        const float cx = sz.width * 0.5f;
        const float cy = sz.height * 0.5f;         // centered: top-down view
        const float radX = (sz.width < sz.height ? sz.width : sz.height) * (0.5f - Inset);
        const float radY = radX;                   // equal radii = round top-down scope

        // The scene state - snapshot, staleness, ping sweep, track reveal and expiry - is
        // updated in Poll() now, not here. It had to move: all of it used to live inside this
        // function, which was fine while the scope was the only thing rendering LIDAR data and
        // wrong the moment a second view existed. With the 3D page up, Render() never runs, and
        // tracks would have sat frozen at whatever the last 2D frame left behind.
        //
        // Poll() runs every frame for every view, so the state machine now ticks exactly once a
        // frame regardless of which page is on screen, and both renderers read the same result.
        const LidarExportBlock& blk = SceneBlk;
        const bool  haveData = SceneHave;
        const float pulseCm  = ScenePulseCm;

        // ---- terrain, beneath everything ---------------------------------------------------
        //
        // The scope hands over its own centre, radius, range and heading, so the ground and the
        // contacts are projected through identical numbers and cannot disagree about where a
        // world position is. TopoView clips itself to the disc and draws no chrome - no tile
        // grid, no centre mark - because those are the scope's job and a second set would be
        // clutter.
        //
        // Note this needs no change to the Clear above. Drawing the terrain INSIDE this function,
        // after the clear and before everything else, was the simpler half of the merge than
        // making the scope's background conditional so something else could paint underneath it.
        TerrainSegs = 0;
        if (Terrain && layerOn(VL_Terrain)) {
            TerrainSegs = Terrain->DrawTerrainUnderlay(RT, cx, cy, radX, RangeCm,
                                                       haveData ? blk.PlayerYaw : 0.0f);
        }

        // Range rings (1/3, 2/3, full).
        if (layerOn(VL_RangeRings)) {
            SetColor(Col::Ring, 0.7f);
            for (int i = 1; i <= 3; ++i) {
                const float f = i / 3.0f;
                RT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radX * f, radY * f), Brush, 1.0f);
            }
        }

        // Aim line: points where the mech torso is twisted, relative to the body
        // (which is "up" on the disc). Flip the sign on `ang` if it reads mirrored.
        if (haveData && layerOn(VL_AimLine)) {
            const float ang = blk.PlayerTorsoYaw * (float)(M_PI / 180.0);
            const float ex  = cx + radX * std::sin(ang);
            const float ey  = cy - radY * std::cos(ang);
            SetColor(Col::Sweep, 0.85f);
            RT->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(ex, ey), Brush, 1.5f);
        }

        // Outward ping ring, expanding from the player at 500 m/s. Fades as it grows.
        if (haveData && layerOn(VL_PingSweep) && pulseCm > 0.0f && pulseCm <= RangeCm) {
            const float pr = (pulseCm / RangeCm) * radX;
            const float fade = 1.0f - (pulseCm / RangeCm);
            SetColor(Col::Sweep, 0.12f + 0.55f * fade);
            RT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), pr, pr), Brush, 2.0f);
        }

        // Enemies (Contacts): revealed by the ping, then fade to nothing over ~1 s. The reveal
        // and expiry themselves happen in UpdateScene; this only draws what they left.
        int shown = 0;
        if (haveData) {
            const double now = NowMs();
            const float yaw = blk.PlayerYaw * (float)(M_PI / 180.0);
            const float cs = std::cos(yaw), sn = std::sin(yaw);
            for (auto& kv : Tracks) {
                const bool isMech = kv.second.Kind == LK_Mech;
                if (isMech  && !layerOn(VL_Mechs))    continue;
                if (!isMech && !layerOn(VL_Vehicles)) continue;
                auto rt = RevealTime.find(kv.first);
                if (rt == RevealTime.end()) continue;               // not yet reached by the ping
                const float alpha = 1.0f - (float)((now - rt->second) / kFadeMs);
                if (alpha <= 0.0f) continue;                        // fully faded
                bool sel = false;
                for (uint8_t s = 0; s < blk.SelectedCount && s < 4; ++s)
                    if (blk.SelectedIds[s] == kv.first) { sel = true; break; }
                if (DrawContact(kv.second, cx, cy, radX, radY, blk.PlayerX, blk.PlayerY, blk.PlayerYaw, cs, sn, sel, alpha))
                    ++shown;
            }

            // Allies (fast path): drawn every frame at full brightness, no ping / fade.
            for (uint32_t i = 0; i < blk.AllyCount && i < LIDAR_MAX_ALLIES; ++i) {
                const LidarContact& a = blk.Allies[i];
                const bool isMech = a.Kind == LK_Mech;
                if (isMech  && !layerOn(VL_Mechs))    continue;
                if (!isMech && !layerOn(VL_Vehicles)) continue;
                if (DrawContact(a, cx, cy, radX, radY, blk.PlayerX, blk.PlayerY, blk.PlayerYaw, cs, sn, a.Selected != 0))
                    ++shown;
            }
        }

        // COMPASS (2026-10-12, user): degrees round the rim, turning with the mech (the scope is heading-up on the
        // legs), N in amber, the heading in a box at the top; clock positions from the torso ("my 3 o'clock" is where
        // the torso faces); a dot just outside the rim at the bearing of each lancemate and each enemy on the scope.
        // Bearings are the voice orders' own: X north, Y east, the angle from north towards east.
        if (haveData && (CmpRing || CmpClock || CmpDots)) {
            const float body = blk.PlayerYaw;
            const float k = (float)(M_PI / 180.0);
            auto rimPt = [&](float relDeg, float r, float& x, float& y) {
                x = cx + std::sin(relDeg * k) * r;
                y = cy - std::cos(relDeg * k) * r;
            };
            if (CmpRing) {
                for (int d = 0; d < 360; d += 5) {
                    const float rel = (float)d - body;
                    const float len = (d % 30 == 0) ? 16.0f : ((d % 10 == 0) ? 10.0f : 5.0f);
                    float x0, y0, x1, y1;
                    rimPt(rel, radX, x0, y0);
                    rimPt(rel, radX - len, x1, y1);
                    SetColor(d == 0 ? 0xFFC04D : Col::Sweep, d == 0 ? 1.0f : 0.8f);
                    RT->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), Brush, d == 0 ? 3.0f : 1.2f);
                    if (d % 30 != 0)
                        continue;
                    const float onScr = std::fmod(rel + 720.0f, 360.0f);
                    if (onScr < 12.0f || onScr > 348.0f)
                        continue; // under the heading box
                    wchar_t lab[8];
                    const bool card = d % 90 == 0;
                    if (card)
                        swprintf_s(lab, L"%s", d == 0 ? L"N" : (d == 90 ? L"E" : (d == 180 ? L"S" : L"W")));
                    else
                        swprintf_s(lab, L"%03d", d);
                    float lx, ly;
                    rimPt(rel, radX - 30.0f, lx, ly);
                    DrawLabel(lx - (card ? 5.0f : 11.0f), ly - 9.0f, 40.0f, lab, d == 0 ? 0xFFC04D : (card ? 0x9CFFC0 : Col::Sweep), 0.95f);
                }
                wchar_t hb[8];
                swprintf_s(hb, L"%03d", ((int)std::lround(body) % 360 + 360) % 360);
                const D2D1_RECT_F box = D2D1::RectF(cx - 24.0f, cy - radY - 30.0f, cx + 24.0f, cy - radY - 8.0f);
                SetColor(Col::Background, 1.0f);
                RT->FillRectangle(box, Brush);
                SetColor(0xFFFFFF, 1.0f);
                RT->DrawRectangle(box, Brush, 1.5f);
                DrawLabel(cx - 13.0f, cy - radY - 30.0f, 48.0f, hb, 0xFFFFFF, 1.0f);
            }
            if (CmpClock) {
                static const wchar_t* kClock[4] = {L"12", L"3", L"6", L"9"};
                for (int q = 0; q < 4; ++q) {
                    float lx, ly;
                    rimPt(blk.PlayerTorsoYaw + 90.0f * (float)q, radX * 0.68f, lx, ly);
                    DrawLabel(lx - (q == 0 ? 7.0f : 4.0f), ly - 9.0f, 30.0f, kClock[q], 0x9CFFC0, 0.5f);
                }
            }
            if (CmpDots) {
                auto dot = [&](float wx, float wy, UINT32 col) {
                    const float dx = wx - blk.PlayerX, dy = wy - blk.PlayerY;
                    if (dx * dx + dy * dy < 1.0f)
                        return;
                    const float bearing = std::atan2(dy, dx) / k;
                    float x, y;
                    rimPt(bearing - body, radX + 9.0f, x, y);
                    SetColor(col, 0.95f);
                    RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 3.5f, 3.5f), Brush);
                };
                const double now = NowMs();
                for (auto& kv : Tracks) { // enemies the ping has shown (not ones it has not reached)
                    auto rt = RevealTime.find(kv.first);
                    if (rt == RevealTime.end() || now - rt->second > kFadeMs)
                        continue;
                    dot(kv.second.X, kv.second.Y, 0xFF5A4D);
                }
                for (uint32_t i = 0; i < blk.AllyCount && i < LIDAR_MAX_ALLIES; ++i)
                    dot(blk.Allies[i].X, blk.Allies[i].Y, 0x5DFFA0);
            }
        }

        // DROPSHIPS (2026-10-08): the hull seen from above, turned like the ship, clipped to the disc so a ship at
        // the edge is cut by the ring rather than spilling over it. Under the contacts' symbols, over the terrain.
        if (haveData && ShipN > 0 && Factory) {
            const float pyaw = blk.PlayerYaw * (float)(M_PI / 180.0);
            const float pc = std::cos(pyaw), ps = std::sin(pyaw);
            auto toScr = [&](float wx, float wy, float& sx, float& sy) {
                const float dx = wx - blk.PlayerX, dy = wy - blk.PlayerY;
                const float fwd = dx * pc + dy * ps, right = -dx * ps + dy * pc;
                sx = cx + (right / RangeCm) * radX;
                sy = cy - (fwd / RangeCm) * radY;
            };
            ID2D1EllipseGeometry* disc = nullptr;
            ID2D1Layer* layer = nullptr;
            if (SUCCEEDED(Factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), radX, radY), &disc)) &&
                SUCCEEDED(RT->CreateLayer(nullptr, &layer))) {
                RT->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), disc), layer);
                for (int i = 0; i < ShipN; ++i) {
                    const LidarView::Dropship& d = Ships[i];
                    const UINT32 col = d.Side == 1 ? 0x4DA6FF : (d.Side == 2 ? 0xFF4D4D : 0x9AA3A8);
                    const float c = d.Kind ? std::cos(d.Yaw * (float)(M_PI / 180.0)) : 1.0f;
                    const float s = d.Kind ? std::sin(d.Yaw * (float)(M_PI / 180.0)) : 0.0f;
                    std::vector<float> box;
                    const std::vector<std::vector<float>>* polys = nullptr;
                    std::vector<std::vector<float>> boxPolys;
                    if (const DropshipOutline* o = DropshipOutlineFor(d.Kind)) polys = &o->Top;
                    else {
                        boxPolys.push_back({ -d.EX, -d.EY, d.EX, -d.EY, d.EX, d.EY, -d.EX, d.EY });
                        polys = &boxPolys;
                    }
                    SetColor(col, 0.9f);
                    for (const auto& p : *polys) {
                        const size_t np = p.size() / 2;
                        for (size_t k = 0; k < np; ++k) {
                            const float ax = p[2 * k], ay = p[2 * k + 1];
                            const float bx = p[2 * ((k + 1) % np)], by = p[2 * ((k + 1) % np) + 1];
                            float s1x, s1y, s2x, s2y;
                            toScr(d.X + ax * c - ay * s, d.Y + ax * s + ay * c, s1x, s1y);
                            toScr(d.X + bx * c - by * s, d.Y + bx * s + by * c, s2x, s2y);
                            RT->DrawLine(D2D1::Point2F(s1x, s1y), D2D1::Point2F(s2x, s2y), Brush, 1.4f);
                        }
                    }
                    float lx, ly;
                    toScr(d.X, d.Y, lx, ly);
                    DrawLabel(lx - 40.0f, ly - 9.0f, 120.0f, d.Side == 2 ? L"ENEMY DROPSHIP" : L"DROPSHIP", col, 0.8f);
                }
                RT->PopLayer();
            }
            if (layer) layer->Release();
            if (disc) disc->Release();
        }

        // NAV POINTS (2026-10-07k): a diamond and its name; past the range, pinned to the outer ring (hollow,
        // with the distance). Always shown - no layer of their own.
        if (haveData) {
            const float yaw = blk.PlayerYaw * (float)(M_PI / 180.0);
            const float cs = std::cos(yaw), sn = std::sin(yaw);
            for (int i = 0; i < NavN; ++i) {
                const LidarView::NavPoint& n = NavPts[i];
                const float dx = n.X - blk.PlayerX, dy = n.Y - blk.PlayerY;
                const float fwd = dx * cs + dy * sn, right = -dx * sn + dy * cs;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const bool pinned = dist > RangeCm;
                const float k = pinned ? (dist > 1.0f ? RangeCm / dist : 0.0f) : 1.0f;
                const float sx = cx + (right * k / RangeCm) * radX;
                const float sy = cy - (fwd * k / RangeCm) * radY;
                const UINT32 col = n.Mine ? 0xE07BFF : 0xFFD54D;
                const float h = pinned ? 6.0f : 7.0f;
                D2D1_POINT_2F dm[4] = {{sx, sy - h}, {sx + h, sy}, {sx, sy + h}, {sx - h, sy}};
                SetColor(col, 0.95f);
                for (int e = 0; e < 4; ++e)
                    RT->DrawLine(dm[e], dm[(e + 1) & 3], Brush, 1.8f);
                if (!pinned) {
                    SetColor(col, 0.35f);
                    RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 3.5f, 3.5f), Brush);
                }
                wchar_t lab[48];
                if (pinned) {
                    if (dist >= 100000.0f) swprintf_s(lab, L"%s %.1f km", n.Name, dist / 100000.0f);
                    else                   swprintf_s(lab, L"%s %d m", n.Name, (int)(dist / 100.0f));
                } else {
                    swprintf_s(lab, L"%s", n.Name);
                }
                // the name inside the ring when pinned: pulled toward the centre
                const float lx = pinned ? sx + (cx - sx) * 0.12f - 40.0f : sx + 9.0f;
                const float ly = pinned ? sy + (cy - sy) * 0.12f - 10.0f : sy - 10.0f;
                DrawLabel(lx, ly, 130.0f, lab, col, 0.95f);
            }
        }

        // Projectiles: small dots with a short motion streak. Ballistic + PPC belong to
        // the Projectiles layer; missiles to the Missiles layer. Streaks advance only on
        // a new projectile publish (ProjCounter change), so motion reads correctly.
        // Alarm included for the same reason as the control bit above - the binder lives in
        // here. Drawing stays correctly gated: each round is individually filtered by its
        // own layer further down, so widening this does not draw hidden projectiles.
        if (haveData && (layerOn(VL_Projectiles) || layerOn(VL_Missiles) || layerOn(VL_ThreatAlarm))) {
            auto isMissileType = [](uint8_t t) { return t == LPT_Missile || t == LPT_ArrowIV; };
            // The per-publish bookkeeping that used to live here - streak sources, landed-round
            // detection, missile trails - now runs in UpdateScene, from Poll(), because it has
            // to tick whether or not this renderer is the one on screen. See UpdateScene.
            const float yaw = blk.PlayerYaw * (float)(M_PI / 180.0);
            const float cs = std::cos(yaw), sn = std::sin(yaw);
            auto project = [&](float wx, float wy, float& sx, float& sy) {
                const float dx = wx - blk.PlayerX, dy = wy - blk.PlayerY;
                const float fwd =  dx * cs + dy * sn, right = -dx * sn + dy * cs;
                sx = cx + (right / RangeCm) * radX;
                sy = cy - (fwd   / RangeCm) * radY;
            };
            // Anything past the scope edge is dropped rather than drawn outside the ring.
            auto inRange = [&](float wx, float wy) {
                const float dx = wx - blk.PlayerX, dy = wy - blk.PlayerY;
                return dx * dx + dy * dy <= RangeCm * RangeCm;
            };
            // Phase of the alarm's tone burst, sampled ONCE for the whole frame. It used to
            // be queried per round inside the loop, and IsBeeping() asks the audio driver
            // (waveOutGetPosition) - with a couple of hundred rounds on screen that was a
            // couple of hundred driver calls every frame. One value also guarantees every
            // marked round flashes on exactly the same frames as every other.
            const bool alarmBeep = ThreatSfx.IsBeeping(NowMs());

            for (uint32_t i = 0; i < blk.ProjectileCount && i < LIDAR_MAX_PROJECTILES; ++i) {
                const LidarProjectile& pj = blk.Projectiles[i];
                if (DeadProj.count(pj.Id)) continue;                 // landed -> hidden
                if (!inRange(pj.X, pj.Y)) continue;                  // outside the scope
                const bool isMissile = isMissileType(pj.Type);
                if (isMissile  && !layerOn(VL_Missiles))    continue;
                if (!isMissile && !layerOn(VL_Projectiles)) continue;

                UINT32 color = Col::ProjBallistic;
                if (pj.Type == LPT_PPC) color = Col::ProjPPC;
                else if (isMissile)     color = Col::ProjMissile;

                // The threatening salvo flashes amber, in phase with the tone.
                //
                // Membership is now the plugin's per-round flag, so this is exact: every
                // round guiding on us is marked and nothing else is. The 10 m proximity
                // sweep that used to stand in for this is gone - it existed only because a
                // single arbitrary round was bound and its salvo-mates had to be inferred
                // from flying in formation with it.
                const bool flashing = isMissile && alarmBeep && (pj.Flags & LPROJ_INCOMING) != 0;
                if (flashing) color = Col::ProjBound;

                // Rounds are published at ~30 Hz, so without this they hop tens of metres
                // at a time - on a 1000 m scope, roughly ten pixels per publish. The
                // PLAYER transform is not extrapolated and does not need to be: position
                // and heading ride the plugin's 60 Hz fast tick, so turning the mech
                // rotates the whole scope from live data. Only the round's own world-space
                // travel is predicted, which is a near-straight line at near-constant
                // speed with no player input in it.
                float wx = pj.X, wy = pj.Y;
                if (ProjSmoothing && ProjPublishDeltaMs > 1.0) {
                    auto prev = ProjStreakFrom.find(pj.Id);
                    if (prev != ProjStreakFrom.end()) {
                        const float ddx = pj.X - prev->second.first;
                        const float ddy = pj.Y - prev->second.second;
                        // Same jump guard the streaks use: a pooled id reused for a fresh
                        // round teleports, and must not be read as enormous velocity.
                        if (ddx * ddx + ddy * ddy < 15000.0f * 15000.0f) {
                            double age = NowMs() - LastProjPublishMs;
                            if (age < 0.0) age = 0.0;
                            // Never predict further than one publish ahead: past that the
                            // round may have impacted and we would fly a ghost onward.
                            if (age > ProjPublishDeltaMs) age = ProjPublishDeltaMs;
                            const float f = (float)(age / ProjPublishDeltaMs);
                            wx = pj.X + ddx * f;
                            wy = pj.Y + ddy * f;
                        }
                    }
                }

                float sx, sy; project(wx, wy, sx, sy);

                if (isMissile) {
                    // Fading gray smoke trail: oldest segment faintest, newest brightest.
                    auto tt = MissileTrail.find(pj.Id);
                    if (tt != MissileTrail.end() && tt->second.size() >= 2) {
                        const auto& pts = tt->second;
                        for (size_t k = 1; k < pts.size(); ++k) {
                            const float wdx = pts[k].first - pts[k-1].first;
                            const float wdy = pts[k].second - pts[k-1].second;
                            if (wdx * wdx + wdy * wdy >= 15000.0f * 15000.0f) continue; // jump guard
                            // Clip the trail at the scope edge too, so a missile flying in
                            // from beyond range doesn't trail a line across the surround.
                            if (!inRange(pts[k-1].first, pts[k-1].second) ||
                                !inRange(pts[k].first,   pts[k].second)) continue;
                            float ax, ay, bx, by;
                            project(pts[k-1].first, pts[k-1].second, ax, ay);
                            project(pts[k].first,   pts[k].second,   bx, by);
                            const float a = 0.45f * (float)k / (float)(pts.size() - 1); // fade to tail
                            SetColor(Col::Smoke, a);
                            RT->DrawLine(D2D1::Point2F(ax, ay), D2D1::Point2F(bx, by), Brush, 1.4f);
                        }
                    }
                    // Arrow IV reads 200% larger (3x) than a regular missile.
                    float r = (pj.Type == LPT_ArrowIV) ? 3.3f : 1.1f;
                    // On the tone's ON frames a round guiding on us swells to 4x radius as
                    // well as changing colour. Size is what carries at a glance: amber
                    // against red is a weak contrast in peripheral vision, and at 1.1 px a
                    // colour change on a scope full of traffic is easy to miss entirely.
                    // The two cues share `alarmBeep`, so the flash is in phase with the beep
                    // rather than merely at the same rate.
                    if (flashing) r *= kSalvoFlashScale;
                    SetColor(color, 0.95f);
                    RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), r, r), Brush);
                } else {
                    // Ballistic / PPC: short colored streak + dot.
                    SetColor(color, 0.95f);
                    auto it = ProjStreakFrom.find(pj.Id);
                    if (it != ProjStreakFrom.end()) {
                        const float wdx = pj.X - it->second.first, wdy = pj.Y - it->second.second;
                        if (wdx * wdx + wdy * wdy < 15000.0f * 15000.0f &&
                            inRange(it->second.first, it->second.second)) {
                            float px, py; project(it->second.first, it->second.second, px, py);
                            RT->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(sx, sy), Brush, 0.9f);
                        }
                    }
                    RT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 1.1f, 1.1f), Brush);
                }
            }
        }

        // Player marker (triangle pointing forward/up).
        SetColor(Col::Player);
        FillTriangle(D2D1::Point2F(cx, cy - 9), D2D1::Point2F(cx - 6, cy + 6), D2D1::Point2F(cx + 6, cy + 6));

        // Readout.
        if (TextFmt) {
            wchar_t buf[128];
            if (haveData)
                swprintf_s(buf, L"LIDAR  RNG %um  CONTACTS %d   (Numpad . : options)",
                           GetRangeM(), shown);
            else
                swprintf_s(buf, L"LIDAR  waiting for game...   (Numpad . : options)");
            SetColor(Col::Text);
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(DW->CreateTextLayout(buf, (UINT32)wcslen(buf), TextFmt,
                                               sz.width - 24.0f, 24.0f, &layout))) {
                RT->DrawTextLayout(D2D1::Point2F(12 + sz.width * Inset, 8 + sz.height * Inset),
                                   layout, Brush);
                layout->Release();
            }
        }

        // The options menu lives in HUDDisplay's edit-mode panel now, not here.

        const HRESULT hr = RT->EndDraw();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
            // The render target is the host's - borrowed, never AddRef'd here - so drop
            // the pointer without releasing it and let the host hand us a new one. The
            // brush is ours and is device-bound, so that one really does get released.
            SafeRelease(Brush);
            RT = nullptr;
        }
    }
};

// ---- Pimpl forwarding -----------------------------------------------------

LidarView::LidarView()  : P(new LidarViewImpl()) {}
LidarView::~LidarView() { if (P) { P->Shutdown(); delete P; P = nullptr; } }

void LidarView::SetTerrainUnderlay(ITerrainUnderlay* t)            { P->SetTerrainUnderlay(t); }
void LidarView::BuildScene(LidarScene& out, bool allRounds)       { P->BuildScene(out, allRounds); }
void LidarView::NotifyRendered()                                  { P->NotifyRendered(); }
uint32_t LidarView::TerrainSegments() const                       { return P->TerrainSegments(); }
bool LidarView::InitResources(ID2D1Factory* f, IDWriteFactory* dw) { return P->InitResources(f, dw); }
void LidarView::SetRenderTarget(ID2D1RenderTarget* rt)            { P->SetRenderTarget(rt); }
void LidarView::Render()                                          { P->Render(); }
void LidarView::Poll()                                            { P->Poll(); }
void LidarView::Shutdown()                                        { P->Shutdown(); }
void LidarView::ReleaseControl()                                  { P->ReleaseControl(); }
void LidarView::SetAlarmVolume(int pct)                           { P->SetAlarmVolume(pct); }
void LidarView::SetAlarmSound(int s)                              { P->SetAlarmSound(s); }
int  LidarView::GetAlarmVolume() const                            { return P->GetAlarmVolume(); }
bool LidarView::IsAlarmSounding() const                           { return P->IsAlarmSounding(); }
LidarView::Diag LidarView::GetDiag() const                        { return P->GetDiag(); }
bool LidarView::IsConnected()                       const         { return P->IsConnected(); }
uint32_t LidarView::GetLayers()                     const         { return P->GetLayers(); }
void LidarView::SetLayers(uint32_t v)                             { P->SetLayers(v); }
uint32_t LidarView::GetRangeM()                     const         { return P->GetRangeM(); }
void LidarView::SetRangeM(uint32_t metres)                        { P->SetRangeM(metres); }
void LidarView::SetFar(bool on)                                   { P->SetFar(on); }
void LidarView::SetCompass(bool ring, bool clock, bool dots)       { P->SetCompass(ring, clock, dots); }
void LidarView::SetContactFilter(ContactKeepFn fn, void* ctx)      { P->SetContactFilter(fn, ctx); }
void LidarView::SetContentInset(float inset)                      { P->SetContentInset(inset); }
void LidarView::SetNavPoints(const NavPoint* pts, int n)            { P->SetNavPoints(pts, n); }
void LidarView::SetDropships(const Dropship* d, int n)              { P->SetDropships(d, n); }
void LidarView::SetProjectileSmoothing(bool on)                   { P->SetProjectileSmoothing(on); }
