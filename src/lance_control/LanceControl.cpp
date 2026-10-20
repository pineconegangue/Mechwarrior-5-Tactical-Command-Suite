// LanceControl.cpp - the lance: TacOps orders, the lance map, the player's autopilot and torso,
// holds, fall back, indirect fire, insane allies, fire settings, the missile arc and range checks,
// the lance status feed for VoiceCommand, and the discovery probes (ai_probe.log).
//
// Split out of VanillaHud (vanillahud-102) unchanged, apart from what drives it: the player
// cockpit's own ReceiveTick (as LidarExport), not the vanilla HUD's - so it runs under MWVR too.
// It shares the MW5_VanillaHUDExport block with VanillaHud and the other split plugin, writing only
// its own part of it; HUDDisplay and VoiceCommand read it as before.
//
// Reflected calls, threads and the leak rules are as described at the top of VanillaHud.cpp.

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
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include "SlowTimer.h"

#include <mw5/UTextureRenderTarget2D.h>
#include <mw5/FTextureResource.h>
#include <mw5/FRHITexture2D.h>

#include <VanillaHudShared.h>
#include <LanceStatusShared.h>

using Microsoft::WRL::ComPtr;

class LanceControl final : public PluginExtension
{
    static constexpr ULONGLONG kGameTickMs   = 100;  // decisions at 10 Hz

    // The discovery probes (lance orders, lancemate move traces, the player's movement). They did
    // their job; off, they cost nothing. Flip to true to have them log again.
    static constexpr bool kDiscoveryProbes = false;

    // ---- shared block --------------------------------------------------------------------
    HANDLE              MmfHandle   = nullptr;

    HANDLE              WriterMutex = nullptr;

    VanillaExportBlock *Block       = nullptr;

    ULONGLONG           LastSetupMs = 0;

    ULONGLONG     LastGameMs = 0;

    struct Frame
    {
        API::UObject *W;
        int32_t       MemberOff;
    };

    ComPtr<ID3D11Texture2D> Src[VS_COUNT];

    // A RING of captures, one rendered per frame in turn (manual CaptureScene, not every-frame), so
    // the image handed on is always one the render thread has certainly finished - two frames old -
    // and the camera pose it was rendered from is known exactly and travels with it (DepthCam).
    // Lasers of one frame (see ReadBeams).
    struct BeamSet
    {
        uint32_t                  Count = 0;
        VanillaExportBlock::Beam  B[VanillaExportBlock::kMaxBeams];
    };

    API::UClass                 *PawnClass      = nullptr;

    API::UObject                *GameplayStatics = nullptr;

    API::UFunction              *GaacFn         = nullptr;

    std::vector<uint8_t>         GaacBuf;            // its out-array is never zeroed (leak rule)

    int32_t                      GaacWorld = -1, GaacClass = -1, GaacOut = -1;

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

    static constexpr const wchar_t *kWriterMutexName = L"Local\\MW5_LanceControl_Writer";

  public:
    inline static LanceControl *Instance;
    SlowTimer SlowT{"LanceControl"}; // SLOW-STEP TIMER (lancecontrol-24): hitch hunting

    LanceControl()
    {
        Instance                  = this;
        PluginExtension::Instance = this;
        Name       = "LanceControl";
        Version    = "0.1.0";
        VersionInt = 1;
    }

    ~LanceControl() override
    {
        // Nothing of the game is restored here: this runs at process exit (see VanillaHud).
        CloseBlock(false);
        if (Ls)
        {
            Ls->Enabled = 0;
            UnmapViewOfFile(Ls);
            Ls = nullptr;
        }
        if (LsMap)
            CloseHandle(LsMap);
    }

    void on_initialize() override
    {
        LogInfo("%s %s initialized", Name, Version);
        LogInfo("BUILD=lancecontrol-83 (MELEE SWING 2026-10-20: the plugin swings punches / melee weapons when the target is in reach - centre to the nearest point of its collision box within the weapon's own MaxRange - and within 30 deg of the torso; the game's check only for busy / cooldown; no toggle, hold fire applies; no rush in) (lancecontrol-82 (WEAPON CATEGORIES BY WEAPONTYPE 2026-10-20: fire-settings category from the WeaponType tag - flamers in laser slots, ACs in MG slots, SRMediumRifle fixed; plasma / TSEMP = PPC; name rules only without a known tag) (lancecontrol-81 (TAG PAINTS 2026-10-20: TAG known by its WeaponType tag (Weapon.Energy.TAG*), held on the target past the heat / friendly / 3 deg gates - 10 deg allowed) (lancecontrol-80 (FLANK ANGLE FIRST 2026-10-20: no backstep; in position / held at any distance - opening up only while still swinging round, for mechs that can twist past 90) (lancecontrol-79 (FLANK HOLD BOX 2026-10-20: in position only from 0.8x the stand-off out (mechs that can open up) - it flip-flopped hold / swing at 0.53x) (lancecontrol-78 (FLANK HELD 2026-10-20: in position the lancemate holds its side and swings round again when it moves - no attack hand-over, no 120 s limit; far-out hook at least 75 deg off the line) (lancecontrol-77 (HOOK SWINGS EARLY 2026-10-20: well outside the ring (1.5x stand-off) the hook keeps at least 60 deg off the line to the target) (lancecontrol-76 (FLANK STEADIER 2026-10-20: the side bearing smoothed (~4 s), in position only after 3 checks in a row, auto side fixed at the order; WIDE HOOK: outside the ring with 30+ deg to go, straight for a point on the side at 1.3x the stand-off) (lancecontrol-75 (FLANK / PINCER 2026-10-20: TCO_Flank - a lancemate swings round a unit firing on the move, its legs kept inside its own torso twist limit, stand-off = most damage in range or ordered, then attacks it) (lancecontrol-74 (QUIET SLOW LINES 2026-10-18: one-by-one slow: lines only with [debug] verbose_log, hitches over 50 ms always; 60 s summary kept) (lancecontrol-73 QUIET LOG 2026-10-18: periodic profiles / status lines only with GlobalSettings [debug] verbose_log = 1) (lancecontrol-72 BUG SWEEP 2 2026-10-18: the render thread looks for the cockpit only while its tick is not reaching us; the death latch is trusted only while the unit keeps being checked; class caches and pending moves reset per mission) (lancecontrol-71 BUG SWEEP 2026-10-17: the cached death test checks the unit and its living component still exist (the lc63 crash path); a held indirect-fire order gets its suppression back after a takeover) (lancecontrol-70 MOVE JOB HOOK KEPT PER FUNCTION: Ally_GoToLocationJob K2_BeginWorker hooked once per function for the session, its own original found by the running function, loop guard - audit item B, no behaviour change, 2026-10-17) (lancecontrol-69 (RANGE ALWAYS CM: a weapon's rated range is WeaponRanges.MaxRange (StatsData.Range / .Ranges) in cm - the under-20000-is-metres rule read a 195 m MG as 19.5 km, 2026-10-17) (lancecontrol-68 (LOOP GUARD on the cockpit tick chain, 2026-10-17) (lancecontrol-67 BATTLE ARMOUR 15 M RULE OFF: the lance fires on Elementals even beside a friendly, user 2026-10-17) (lancecontrol-66 TEAM FALLBACK REMOVED: a suit's team reads as \"TeamID\" like every unit - shipping names ignore case, 2026-10-17) (lancecontrol-65 LIVE LIST CHECKED: Live()'s cached pawns are checked against the object table on every use - a freed pawn is dropped, 2026-10-17) (lancecontrol-64 BATTLE ARMOUR: KelClanElemental suits are combat units (targets, friendly-fire checks, the lance feed); no plugin shot at a suit within 15 m of a friendly (cockpit attacks), 2026-10-17) (lancecontrol-63 LIVING COMPONENT CHECKED TOO: LivingDead checks the mech's living component against the object table before reading it - a component freed during the mech's death crashed the game, 2026-10-16) (lancecontrol-62 OBJECT CHECK: IsDeadUnit first asks the engine's object table whether the pointer is still that object - a pawn freed inside Live()'s 250 ms cache crashed the game in ArcBlocked's friendly check, 2026-10-16; self-tested on your pawn, off if that fails) (lancecontrol-61 EXACT UNIT FOR NAMED ORDERS: TCO_Attack / TCO_CalledShot accept the unit's address (Loc[2] == kTacLocActor, as the patch-through lock) - named attacks and called shots go to that unit, not the one nearest its point (a dropship turret beside a King Crab), 2026-10-16) (lancecontrol-60 ATTACK ORDER 10 S: an attack order (and its called shot) ends when no lancemate has seen the unit for 10 s - lost or never seen - was 1 s, user 2026-10-16) (lancecontrol-59 NO STALE AIM POINTS: the insane-allies aim spread keeps the part and side, not the aim hint pointer, and looks the hint up again on the target every call; vanilla called shots check the cached hint is still on the target every frame - a destroyed part's hint was freed while cached and crashed the game, 2026-10-16) (lancecontrol-58 HOLD FIRE KEPT ACROSS ORDERS: ending an indirect hold no longer hands firing back to the game while insane allies / fire settings hold it) (lancecontrol-57 NAV NAMES NATO: Alpha, Bravo, Charlie... - Kilo and Victor never given out) (lancecontrol-56 Greek skips, retired) (lancecontrol-55 NAV LETTERS REUSED: a nav point takes the lowest letter not in use - VoiceCommand's hill candidates are marked and unmarked) (lancecontrol-54 ADVANCE (FALL BACK TO ME) RETIRED: AfbTick and the order removed; TCO_AdvanceFallback is ignored (slot reserved)) (lancecontrol-53 FORMAT CALLS TRUNCATE: every swprintf_s / sprintf_s into a fixed buffer is _snwprintf_s / _snprintf_s with _TRUNCATE - a long game name was a fast-fail crash (insane allies target-switch note, why[64], UTL_VOL_Industrial_Military_SphericalTank_C)) (lancecontrol-52b world gravity from WorldSettings / PhysicsSettings) (lancecontrol-52 WEAPON STATS FROM StatsData + LEAD AND DROP: kinds / speed / burst / beam duration / GravityScale read from the game's StatsData again; projectile weapons aim at our own aim mark (TargetPoint pool) at the lead point raised by the drop over the flight time) (lc51 LSTICK WEAPONS + ENEMIES CACHED: ammo bins listed once per lancemate, ammo / melee / weapon name decided per class, AmmoForWeapon re-run every 64th; enemy pass: cached dead test, team offset, relation-table layout per class) (lc50 IATICK CACHED READS: dead / controller / target / lock / hold flag / team / position by the lc46 cached readers, heat component found once per class and pass, weapon fire checks timed) (lc49 IATICK FRIENDS PER PASS: the friendly-in-line checks of one insane-allies pass share one friendly list (team by cached offset, dead and position by the lc46 reads), the old check re-run every 32nd and differences logged; profile splits setup into map / setup / read) (lc48 IATICK PROFILE: insane allies firing timed by part - setup, target check, lock-on volleys, direct fire, with arc traces / friendly-in-line / reach checks inside - 'iatick profile' / 'iatick content' every 30 s; measuring only) (lc47 LSTICK SLOT FROM THE SCAN: each lancemate line takes dead / position / controller / visible list for the unit and its target from the pass's scan, heat component found once per unit, cached offsets; old calls re-run every 64th run per slot and differences logged as lstick slot check) (lc46 LSTICK SCAN: positions from the unattached root component's RelativeLocation, checked against the game's call per class (5 checks, then every 64th; off by > 0.5 m -> the call for that class); VTOL per class; dead test with cached lookups) (lc45 shared scan reads only hostiles and our lance - friendly / teamless pawns skipped before any costly read) (lc44 LSTICK SHARED SCAN: one snapshot of every pawn per lance-status pass, used by the hostile list and every lancemate's seen-by; property offsets cached per class) (lc43 LSTICK PROFILE: the lance status feed timed by section + what it published, 'lstick profile' / 'lstick content' every 30 s - the A/B baseline) (lc42 VANILLA CALLED SHOTS MOVE THE TARGET'S AI AIM POINT onto the called part, so the game's own per-shot aim lands there too; called shots on non-mechs refused; NO-aim-hint lookup every 2 s, logged once) (lc41 DEBUG PROBES from GlobalSettings [debug], both off by default: ai_probe = the AI probe + ai_probe.log (was always on), aim_probe = the aim-writer probe - what puts the game's aim back during a vanilla called shot) (lc40 lock broadcast: only *Changed / *Acquired delegates, never Destroyed / Lost) (lc39 LOCK -> V-TGT: a patched-through lock also fires the target-changed multicast delegates of your mech / its TargetTracking / its MechViewComponent, so the game's and MWVR's target panels follow it - 'lock probe: delegate' lines; AUTOPILOT: a partial navmesh path ending where you stand is ignored - it went straight instead of arriving at once; PATCH-THROUGH ONLY ON A DETECTED UNIT: a lock (TCO_LockTarget, voice or the TacOps map) is refused unless you or a lancemate detects that unit - line of sight, sensors or relayed; VANILLA CALLED SHOTS: the game's own firing aims at the called part, every frame, only while its target is the called unit; attack orders end when the target dies or no lancemate has seen it for 1 s, then attack the nearest enemy in sight; a move off walkable ground goes to the farthest walkable point on the way; CALLED SHOT ORDERS - TCO_CalledShot: a part on one unit (your lock or named), plus the attack; the ini probe no longer aims; old point spread removed; direct fire aims at the centre torso by default; insane aim spread = a random part per slot, held 2 s; called shot aims direct fire at the aim hint component of the part; per-chassis aim-hint map, clavicle = side torso, part condition read fixed; CALLED SHOT PROBE, the game's own MWAimHintComponents first, bones as fallback: [lance] called_shot_probe = off | legs | arms | ct | head | sides; ally mode [lance] insane_allies 0 vanilla / 1 insane (aim spread, 70 %% hits) / 2 impossibly strong (exact aim); insane allies aim spread: each volley a scattered hit around the middle of the target (normal spread, [lance] ally_scatter) or a near miss (chance 1 - [lance] ally_hit, default 70 %% hits), on the target's AI aim component, lead on top - they cockpitted nearly everything; nav points: the game's nav point is a TravelLocationObjectiveMarker_C (the -26 log; the others are Comms / Blank markers) - taken by that name, distance from you logged; nav points: GetObjectiveMarkerWidget returns the marker's CLASS, so a nav point is told by that class's own name (contains \"Nav\"), not by the class of the class; nav points diagnosed: what the objective indicators are and why each is or is not taken as a nav point, logged when it changes; slow-step timer: any of our per-frame steps over 2 ms logged as 'slow:'; death: the game's own MWLivingActorComponent.bIsDead, latched - a legged mech is dead too; nav points: the mission's nav markers + your TacOps marks, named Nav Alpha / Beta..., published in VHUD v19; nav probe removed; flanked event: an enemy within 100 m in a lancemate's rear arc; new missions told by the game's own mission events (MWSquad OnMissionStart / OnMissionCleanup), the old guesses only if those cannot be hooked; the lance switches read straight from GlobalSettings.ini (found via GlobalSettingsPath.txt) - no app needs to run; the game's voices muted only when that setting says so; mission probe: the game's mission start / cleanup events and your mech's level, logged; lance numbers wait up to 5 s for the game's lance panel; pilot names converted safely; takeover: switching into a lance mech keeps the lance (no new-mission reset); lance numbers and pilot names from the game's V-comms panel (status feed v6); the game's lancemate voice lines muted (HUDDisplay option); probes out; lock probe dumps and blueprint hooks removed - the patch-through lock works; lance status v5: who fires rounds at you, traced by each round's Instigator; lock probe 2: the exact unit; CurrentTarget + selected flag written, target-changed handlers called; units feed: who sees it from each lancemate's own line-of-sight flag, lance / player radar flags, a 10 s summary; lock probe: patch-through lock + target-function dump; units feed: who targets you, which lancemates see it; hold at a point: walks there firing on the move; status feed v4: every hostile position, any range, and whether the lance sees it; exit crash fixed: the [rdt] probe touched a destroyed unit - now off and guarded; attacker in the took-damage event, player-hit / enemy-on-player events; probe: RecentDamageTarget on every unit, [rdt] lines in ai_probe.log; UAV launched event; world clock in the world key, status feed v3: ammo bins by type, recovers from an old block; lance status v2: condition at any distance; sticky lance numbers, attack releases hold fire; world key: controller + level + mech; split from vanillahud-102: TacOps, lance map, insane allies, fire settings, arc and range checks, status feed)");
        EnsureBlock();
    }

    // Render thread: only the block and the driver hook (nothing of the game is touched here).
    void on_present() override
    {
        if (!Block && !EnsureBlock())
            return;
        Block->ProducerTickMs = GetTickCount64(); // alive, with or without VanillaHud
        DrvInstall();
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
                API::get()->log_error("[LanceControl] game-thread driver: the cockpit tick chain loops back into LanceControl - cut");
            }
            return nullptr;
        }
        ++loopDepth;
        void *r = orig ? orig(self, frame, result) : nullptr; // the cockpit (and anyone before us) first
        --loopDepth;
        // Every mech has a cockpit actor of this class; only the player's drives us.
        if (Instance && self && self == LocalCockpit())
        {
            Instance->DrvSeenMs = GetTickCount64();
            Instance->GameTick();
        }
        return r;
    }

    // THE TACOPS OVERLAY'S INPUT STATE (CameraCapture runs the overlay since the split). The torso
    // and pilot watches only need to know whether it is up and when it closed: read from the same
    // request CameraCapture acts on (HUDDisplay's OvWantMs / OvHandle).
    static constexpr bool kOvGameInput = true; // as CameraCapture: the game keeps its input while open
    std::atomic<bool> OvInputOn{false};
    ULONGLONG         OvInputOffMs = 0; // when the overlay last gave the game its input back
    void OvStateTick(ULONGLONG now)
    {
        const uint64_t want = Block->OvWantMs;
        const bool on = want && Block->OvHandle && (int64_t)(now - want) < 500;
        if (OvInputOn.load() && !on)
            OvInputOffMs = now;
        OvInputOn.store(on);
    }

    // =====================================================================================
    // GAME THREAD - driven by the player cockpit's tick (every frame), not the HUD: works
    // under MWVR too.
    // =====================================================================================
    API::UObject *LancePc = nullptr; // the controller of the world we are working in
    // lancecontrol-2: the controller alone is not a reliable world key - a second mission in the
    // same session kept the same controller, so nothing was reset (insane allies fired nothing,
    // fire settings carried over). The controller's level and the player's mech are keyed too.
    // GameTick only runs off the player cockpit's tick, so the local pawn here is always the mech.
    API::UObject *LanceLevel = nullptr;
    API::UObject *LancePawn  = nullptr;
    // lancecontrol-5: and the WORLD CLOCK. Addresses can be reused by the next level (most likely
    // what kept the controller "the same" in the first place), so pointers alone are not proof.
    // GetTimeSeconds restarts at 0 in every new world and never runs backwards inside one (a pause
    // stops it, it never goes back) - seen going backwards, it is a new mission whatever the pointers say.
    float LanceWorldT = -1.0f;

    float WorldSeconds(API::UObject *pc)
    {
        static API::UObject *gs = nullptr;
        if (!gs)
            if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameplayStatics"))
                gs = c->get_class_default_object();
        static bool off = false; // not callable here: pointers only, said once (no log every tick)
        float t = -1.0f;
        if (off || !gs)
            return -1.0f;
        if (CallRet(gs, L"GetTimeSeconds", {{L"WorldContextObject", &pc, sizeof(pc)}}, &t, sizeof(t)))
            return t;
        off = true;
        LogWarn("world clock: GameplayStatics.GetTimeSeconds could not be called - new missions are told by pointers only");
        return -1.0f;
    }
    void GameTick()
    {
        if (!Block)
            return;
        const ULONGLONG now = GetTickCount64();
        OvStateTick(now);
        TorsoTick(now); // every frame, not at the 10 Hz of the rest: the slew must be smooth

        if (now - LastGameMs < kGameTickMs)
            return;
        LastGameMs = now;

        auto *pc = API::get()->get_player_controller(0);
        if (!pc)
            return;
        // A different controller: a new world. Everything of the old one is handed back / let go
        // (the first world too, as VanillaHud did: it also starts ai_probe.log's mission marker).
        API::UObject *level = pc->get_outer();
        API::UObject *pawn  = API::get()->get_local_pawn(0);
        const float   wt    = WorldSeconds(pc);
        const bool newPc    = pc != LancePc;
        const bool newLevel = !newPc && level != LanceLevel;
        const bool newPawn  = !newPc && !newLevel && pawn && LancePawn && pawn != LancePawn;
        // lancecontrol-20: the game's own mission events (MWSquad OnMissionStart / OnMissionCleanup, see MISSION
        // EVENTS) are THE new-mission signal. Only when they could not be hooked do the old guesses (a mech the
        // lance never had, the world clock going backwards) still count. A new controller or level is a new world
        // whatever happens (every pointer of the old one is gone).
        const bool missionEvt = InterlockedExchange(&MpPending, 0) != 0;
        const bool guess    = MpCount == 0;
        const bool newClock = guess && !newPc && !newLevel && wt >= 0.0f && LanceWorldT >= 0.0f && wt + 1.0f < LanceWorldT;
        if (wt >= 0.0f)
            LanceWorldT = wt;
        // lancecontrol-17: a new player mech that is one of the LANCE's own mechs is a takeover (you
        // switched mechs, or took a lancemate's when yours went down) - the same mission. Only a mech
        // the lance never had (the next mission's drop) still counts as a new world.
        bool takeover = false;
        if (newPawn && !newClock)
            for (int s = 2; s <= VanillaExportBlock::kTacMaxMembers; ++s)
                if (TacUnit[s] == pawn && !TacIsPlayer[s])
                    takeover = true;
        if (missionEvt || newPc || newLevel || (guess && newPawn && !takeover) || newClock)
        {
            if (LancePc)
                LogInfo("new world (%s) - per-mission state reset",
                        missionEvt ? "the game's mission event" : newPc ? "controller changed" : newLevel ? "level changed"
                        : newPawn ? "player mech changed, not to one of the lance's" : "world clock restarted");
            ForgetWorldLance(pc);
            LancePc    = pc;
            LanceLevel = level;
            LancePawn  = nullptr;
        }
        else if (takeover)
            TakeoverLance(LancePawn, pawn);
        if (pawn)
            LancePawn = pawn;

        SlowT.Begin(); // SLOW-STEP TIMER (lancecontrol-24)
        ProbeLance(pc, now); SlowT.Step("ProbeLance"); // discovery only - logs lance orders, issues none
        TacPublish(pc, now); SlowT.Step("TacPublish");  // TacOps: the lance (every frame while the page is up, 4 Hz otherwise)
        AmmoProbeTick(pc); SlowT.Step("AmmoProbeTick");    // discovery: where each unit's ammo lives (ai_probe.log, read only)
        LsTick(pc); LsPfEnd(); SlowT.Step("LsTick");           // the lance status feed and event list (MW5_LanceStatus), 4 Hz
        ShotTick(pc, GetTickCount64()); SlowT.Step("ShotTick"); // who fires rounds at you (lance status v5), 10 Hz
        GsTick(); SlowT.Step("GsTick");             // the lance switches from GlobalSettings.ini (lancecontrol-19)
        GvTick(pc); SlowT.Step("GvTick");           // the game's own lancemate voice lines: muted when GlobalSettings says so (lancecontrol-17)
        MpTick(); SlowT.Step("MpTick");             // the game's mission events, hooked once (lancecontrol-20: they reset the lance)
        NavTick(pc, now); SlowT.Step("NavTick");     // NAV POINTS (lancecontrol-22): the mission's nav points -> VHUD v19 Navs[]
        if (API::get()->get_local_pawn(0))
            ArcLearn(pc);     // missile path shapes, from the game's own path blueprints
        SlowT.Step("ArcLearn");
        GoToInstallHook(); SlowT.Step("GoToInstallHook");    // so a TacOps move goes to the map point, not the reticle
        TacRunCommands(pc); SlowT.Step("TacRunCommands");   // and the orders it queued
        GoToPendTick(pc); SlowT.Step("GoToPendTick");     // moves whose job never (re)started: put on the point directly
        ApTick(pc); SlowT.Step("ApTick");           // the player's autopilot, corner by corner
        LTraceTick(pc); SlowT.Step("LTraceTick");       // discovery: lancemates walking a TacOps move
        FlankTick(pc); SlowT.Step("FlankTick");      // lancecontrol-75: flank / pincer - the next leg round the target
        FomTick(pc); SlowT.Step("FomTick");          // fire on the move: hold the combat flag off (after LTraceTick's refresh)
        HoldTick(pc); SlowT.Step("HoldTick");         // hold here: Wait on at the point, and a target to shoot
        IcTick(pc); SlowT.Step("IcTick");           // indirect fire from cover: hide when any enemy sees it
        IaTick(pc); SlowT.Step("IaTick");           // insane allies: the plugin fires every ready lancemate weapon
        CsTick(); SlowT.Step("CsTick");
        CsVanillaTick(); SlowT.Step("CsVanillaTick"); // lancecontrol-36: vanilla allies' called shots, every frame
        AtkWatchTick(pc); SlowT.Step("AtkWatchTick"); // lancecontrol-34: attack orders end with their target             // lancecontrol-31: called shot probe (log + aim at a body part)
        AimWatch(pc); SlowT.Step("AimWatch");         // what the game puts in AIOverrideAimTarget (lancemates it fires for)
        AiProbeTick(pc); SlowT.Step("AiProbeTick");      // discovery: everything a lancemate does, to ai_probe.log (read only)
        MoveProbe(pc, now); SlowT.Step("MoveProbe");   // discovery only - what the stick drives, for the autopilot
        SlowT.Flush([&](const char *line) { LogInfo("%s", line); });
    }

    // TAKEOVER (lancecontrol-17). The player's mech changed to one of the lance's own mechs. The game
    // moves PILOTS between mechs (its V-comms panel: each pilot keeps its member id and name, only
    // PlayerPawn changes - seen in the lancecontrol-16 squad probe), and the order numbers follow the
    // pilots too. So what belongs to the pilot or the mission stays - lance numbers, fire settings,
    // orders, holds, attack targets - and only what was learned about a MECH is let go and read again.
    void TakeoverLance(API::UObject *oldPawn, API::UObject *newPawn)
    {
        LivePawnsMs = 0;
        // Plugin firing handed back first, as at a world change: the mechs changed hands.
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            if (IcMaskSet[s] && TacUnit[s] && IsLive(TacUnit[s]) && !IsDeadUnit(TacUnit[s]))
            {
                API::UObject *u = TacUnit[s];
                IcDirStop(s, true, 0);
                for (auto *w : IcFiring[s])
                    if (OnMech(u, w))
                        IcCallNoArgs(w, L"K2_StopFire");
                IcSuppress(u, IcMaskOld[s] == 1);
            }
        IcRestoreAims(0, true);
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            IcMaskSet[s] = false;
            IcFiring[s].clear();
            IaSet[s] = false;
            IaTgt[s] = nullptr;
            IaOkMs[s] = IaNoneLogMs[s] = 0;
            IcWpns[s].clear();
            IcMelee[s].clear(); // lancecontrol-83
            IcWpnList[s].clear();
            IcDirFiring[s].clear();
            IcDirFires[s] = IcOwnFires[s] = IcDirectShots[s] = 0;
            IcDrop[s].clear();
            IcTT[s] = nullptr;
            IcTTOk[s] = IcTTLogMs[s] = 0;
            IcWatchTgt[s] = nullptr;
            IcNoFire[s] = nullptr;
            IcMaskOld[s] = 0;
            IcHeatProjSet[s] = false;
            IaRetryMs[s] = 0;
            IcCantFire[s] = IcNoFireLogged[s] = nullptr;
        }
        OnMechPawn = nullptr;
        OnMechList.clear();
        IcMoved.clear();
        for (auto &t : IcSpreadTgt) // lancecontrol-59
            t = nullptr;
        FsShooters.clear();
        LivePawns.clear();
        AimWatchLast.clear();
        AmmoSeen.clear();
        AmmoLast.clear();
        for (int s = 0; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            LsPrevUnit[s] = LsPrevTgt[s] = LsFireTgt[s] = nullptr;
            LsDmgInit[s] = false; // damage is measured afresh on the mech the pilot is in now
            LsWpns[s].clear();
            LsCondUnit[s] = nullptr;
            LsCondMs[s] = 0;
            LsUav[s] = -1;
        }
        LsPrevLock = nullptr;
        LockWant = nullptr;
        TorsoMode = TMO_Off; // the torso hold and the autopilot belonged to the old mech
        TorsoTgt = nullptr;
        ApOn = false;
        ApPath.clear();
        MovePawn = nullptr;
        MoveWatches.clear();
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            if (AiSt[s].Pawn)
                AiEpisodeEnd(s, "takeover");
        for (auto &a : AiSt)
            a = AiSlot{};
        AiLog(L"-------------------- TAKEOVER (pilots changed mechs - same mission) --------------------");
        // The lance map: pawns asked for afresh (the order numbers, sticky numbers and settings stay).
        for (auto &u : TacUnit)
            u = nullptr;
        TacMapMs = 0;
        TacScanMs = 0;
        TacPosCache.clear();
        TacMapSig.clear();
        DiMs = 0;
        LancePawn = newPawn;
        LogInfo("takeover: you are now in %ls (was %ls) - same mission: lance numbers, orders and fire settings kept",
                ObjLabel(newPawn).c_str(), ObjLabel(oldPawn).c_str());
    }

    // THE LANCE SWITCHES (lancecontrol-19), straight from GlobalSettings.ini: our apps write its full path into
    // %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\GlobalSettingsPath.txt (gs::PublishPath), so no app has to
    // be running. Read once a second. No such file yet: both off (the game's voices play). In the file but a
    // key missing: GlobalSettings' own defaults (insane allies off, mute on).
    bool         GsInsane = false, GsMute = false;
    bool         GsExact = false; // lancecontrol-29: "impossibly strong allies" - every shot on the aim hint, no spread
    ULONGLONG    GsMs = 0, GsPathMs = 0;
    std::wstring GsIni;
    std::wstring GsLogged;
    ULONGLONG    GsStamp = 0; // the file's write time when last read: re-read only when it changes

    void GsTick()
    {
        const ULONGLONG now = GetTickCount64();
        if (GsMs && now - GsMs < 1000)
            return;
        GsMs = now;
        if (GsIni.empty() || now - GsPathMs > 10000)
        {
            GsPathMs = now;
            const std::wstring was = GsIni;
            GsIni.clear();
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
                            GsIni.resize((size_t)k);
                            MultiByteToWideChar(CP_UTF8, 0, b, (int)got, &GsIni[0], k);
                            while (!GsIni.empty() && (GsIni.back() == L'\n' || GsIni.back() == L'\r' || GsIni.back() == L' '))
                                GsIni.pop_back();
                        }
                    }
                    CloseHandle(h);
                }
            }
            if (GsIni != was)
                GsStamp = 0; // another file: read it
            if (GsIni != GsLogged)
            {
                GsLogged = GsIni;
                LogInfo("lance switches: %ls", GsIni.empty() ? L"GlobalSettingsPath.txt not found - insane allies off, the game's voices play"
                                                             : (L"from " + GsIni).c_str());
            }
        }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GsIni.empty() || !GetFileAttributesExW(GsIni.c_str(), GetFileExInfoStandard, &fa))
        {
            GsInsane = GsMute = false;
            AiProbeOn = AimProbeOn = false; // lancecontrol-41
            GsStamp = 0;
            return;
        }
        const ULONGLONG st = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        if (st == GsStamp)
            return; // unchanged: one file-system call a second, nothing more
        GsStamp = st;
        // lancecontrol-29: [lance] insane_allies is the ALLY MODE - 0 vanilla allies (the game fires), 1 insane allies
        // (the plugin fires every ready weapon, aim spread + misses), 2 impossibly strong allies (exact aim, as
        // before -28). An old "1" stays insane allies.
        const int mode = (int)GetPrivateProfileIntW(L"lance", L"insane_allies", 0, GsIni.c_str());
        GsInsane = mode != 0;
        GsExact = mode >= 2;
        GsMute = GetPrivateProfileIntW(L"lance", L"mute_game_voices", 1, GsIni.c_str()) != 0;
        {
            // lancecontrol-31: CALLED SHOT PROBE - which body part the lancemates aim at (off = as before)
            wchar_t cs[32] = L"";
            GetPrivateProfileStringW(L"lance", L"called_shot_probe", L"off", cs, 32, GsIni.c_str());
            const std::wstring v = cs;
            const int was = CsPart;
            CsPart = v == L"legs" ? 0 : v == L"arms" ? 1 : v == L"ct" ? 2 : v == L"head" ? 3 : v == L"sides" ? 4 : -1;
            if (CsPart != was)
                LogInfo("called shot probe: %ls", CsPart < 0 ? L"off" : cs);
        }
        {
            // lancecontrol-41: DEBUG PROBES (GlobalSettings > DEBUG), both off by default.
            const bool ai = GetPrivateProfileIntW(L"debug", L"ai_probe", 0, GsIni.c_str()) != 0;
            const bool aim = GetPrivateProfileIntW(L"debug", L"aim_probe", 0, GsIni.c_str()) != 0;
            if (ai != AiProbeOn)
            {
                if (!ai)
                    AiFlush(); // what was collected still reaches ai_probe.log
                AiProbeOn = ai;
                LogInfo("debug: AI probe (ai_probe.log) %s", ai ? "ON" : "off");
            }
            if (aim != AimProbeOn)
            {
                AimProbeOn = aim;
                LogInfo("debug: aim-writer probe %s", aim ? "ON" : "off");
            }
        }

    }

    // GAME LANCEMATE VOICES (lancecontrol-17). Every comms line the game plays is announced to the
    // squad: MWSquad::HandleDialogueMessageCreated(ComsMessage Message) - SourceActor (the speaking
    // mech; null for Ryana / mission speakers), LineName, PriorityData.Context (the reason:
    // DialogueContext.Spot.Enemy.Vehicle, KillBlow.Cheer, ...). Found by the lancecontrol-16 probe: it
    // reaches its Func through ProcessEvent, so the detour sees its own frame. A line from a lancemate
    // (not the player) is stopped on the next tick with the game's own
    // MWPlayerComsSystem::StopAllMessagesFromUnit - mission dialogue is never touched. On unless a
    // HUDDisplay instance asks for the game's voices (kVhudOptGameVoices, its Esc menu).
    API::UFunction *GvFn = nullptr;
    BP_FUNC         GvOrig = nullptr;
    bool            GvTried = false;
    int32_t         GvMsg = -1, GvSrc = -1, GvLine = -1, GvCtx = -1; // param, then offsets inside it
    std::vector<API::UObject *> GvStop;
    API::UObject   *GvComs = nullptr;
    API::UFunction *GvStopFn = nullptr;
    int32_t         GvStopUnit = -1;
    int             GvLogged = 0;
    static constexpr int kGvMaxLog = 40;

    static void *OnGvCreated(API::UObject *self, FFrame *frame, void *const result)
    {
        if (Instance)
            GvSeeSafe(frame);
        return (Instance && Instance->GvOrig) ? Instance->GvOrig(self, frame, result) : nullptr;
    }

    static void GvSeeSafe(FFrame *frame)
    {
        __try
        {
            Instance->GvSee(frame);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void GvSee(FFrame *frame)
    {
        if (!frame || frame->Node != GvFn || !frame->Locals || GvMsg < 0 || GvSrc < 0)
            return; // only its own frame can be read (a call from script runs on the caller's)
        uint8_t *m = frame->Locals + GvMsg;
        API::UObject *src = *reinterpret_cast<API::UObject **>(m + GvSrc);
        if (!src)
            return; // Ryana, the client, the mission: not a lancemate
        int slot = 0;
        for (int s = 2; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            if (TacUnit[s] == src && !TacIsPlayer[s])
                slot = s;
        if (!slot)
            return;
        const bool play = !GsMute; // lancecontrol-19: GlobalSettings.ini [lance] mute_game_voices
        if (!play)
            GvStop.push_back(src);
        if (GvLogged < kGvMaxLog)
        {
            if (++GvLogged == kGvMaxLog)
                LogInfo("game voices: %d lines logged - quiet for the rest of this mission", kGvMaxLog);
            const std::wstring line = GvLine >= 0 ? reinterpret_cast<API::FName *>(m + GvLine)->to_string() : L"?";
            const std::wstring ctx = GvCtx >= 0 ? reinterpret_cast<API::FName *>(m + GvCtx)->to_string() : L"?";
            LogInfo("game voices: lancemate %d - %ls (%ls) - %ls", slot, line.c_str(), ctx.c_str(),
                    play ? L"left playing (game voices on in HUDDisplay)" : L"muted");
        }
    }

    void GvInstall()
    {
        if (GvTried)
            return;
        GvTried = true;
        auto *cls = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWSquad");
        GvFn = cls ? cls->find_function(L"HandleDialogueMessageCreated") : nullptr;
        API::UStruct *msg = nullptr;
        if (GvFn)
            for (auto *f = GvFn->get_child_properties(); f; f = f->get_next())
                if (f->get_fname()->to_string() == L"Message" && PropType(reinterpret_cast<API::FProperty *>(f)) == L"StructProperty")
                {
                    GvMsg = reinterpret_cast<API::FProperty *>(f)->get_offset();
                    msg = reinterpret_cast<API::FStructProperty *>(f)->get_struct();
                }
        if (msg)
        {
            GvSrc  = StructMember(msg, L"SourceActor");
            GvLine = StructMember(msg, L"LineName");
            if (auto *pd = msg->find_property(L"PriorityData"))
                if (PropType(pd) == L"StructProperty")
                    if (auto *pds = reinterpret_cast<API::FStructProperty *>(pd)->get_struct())
                        if (auto *cx = pds->find_property(L"Context"))
                            if (PropType(cx) == L"StructProperty")
                            {
                                const int32_t t = StructMember(reinterpret_cast<API::FStructProperty *>(cx)->get_struct(), L"TagName");
                                if (t >= 0)
                                    GvCtx = pd->get_offset() + cx->get_offset() + t;
                            }
        }
        if (!GvFn || GvMsg < 0 || GvSrc < 0)
        {
            LogWarn("game voices: MWSquad::HandleDialogueMessageCreated or its Message.SourceActor not found - the game's voices stay on");
            GvFn = nullptr;
            return;
        }
        auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(GvFn) + NATIVE_FUNCTION_OFFSET);
        GvOrig = *slot;
        *slot = &OnGvCreated;
        // Its stop: MWPlayerComsSystem::StopAllMessagesFromUnit(<the unit>).
        if (auto *cc = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWPlayerComsSystem"))
            GvStopFn = cc->find_function(L"StopAllMessagesFromUnit");
        std::wstring sig;
        if (GvStopFn)
            for (auto *f = GvStopFn->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                if (!p->is_param())
                    continue;
                sig += f->get_fname()->to_string() + L":" + PropType(p) + L" ";
                if (GvStopUnit < 0 && !p->is_return_param() && PropType(p) == L"ObjectProperty")
                    GvStopUnit = p->get_offset();
            }
        LogInfo("game voices: hooked MWSquad::HandleDialogueMessageCreated (message +%d, source +%d, line +%d, context +%d); "
                "StopAllMessagesFromUnit(%ls)%ls",
                GvMsg, GvSrc, GvLine, GvCtx, sig.c_str(), GvStopUnit < 0 ? L" - NO object parameter: lines cannot be stopped" : L"");
    }

    // The game's comms system: MWPlayerComsSystem::GetPlayerComsSystem(<world context>).
    API::UObject *GvComsSystem(API::UObject *pc)
    {
        if (GvComs)
            return GvComs;
        auto *cc = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWPlayerComsSystem");
        API::UFunction *gf = cc ? cc->find_function(L"GetPlayerComsSystem") : nullptr;
        API::UObject *cdo = cc ? cc->get_class_default_object() : nullptr;
        if (!gf || !cdo)
            return nullptr;
        const int32_t frame = gf->get_properties_size();
        const int32_t ret = ParamOffset(gf, L"ReturnValue");
        if (ret < 0 || frame <= 0 || frame > (int32_t)sizeof(CallBuf) - 16)
            return nullptr;
        std::memset(CallBuf, 0, (size_t)frame + 16);
        for (auto *f = gf->get_child_properties(); f; f = f->get_next())
        {
            auto *p = reinterpret_cast<API::FProperty *>(f);
            if (p->is_param() && !p->is_return_param() && PropType(p) == L"ObjectProperty")
            {
                *reinterpret_cast<API::UObject **>(CallBuf + p->get_offset()) = pc; // the world context
                break;
            }
        }
        cdo->process_event(gf, CallBuf);
        GvComs = *reinterpret_cast<API::UObject **>(CallBuf + ret);
        if (GvComs)
            LogInfo("game voices: comms system %ls", ObjLabel(GvComs).c_str());
        return GvComs;
    }

    void GvTick(API::UObject *pc)
    {
        GvInstall();
        if (GvStop.empty())
            return;
        std::vector<API::UObject *> units;
        units.swap(GvStop);
        API::UObject *coms = (GvStopFn && GvStopUnit >= 0) ? GvComsSystem(pc) : nullptr;
        if (!coms)
            return;
        const int32_t frame = GvStopFn->get_properties_size();
        if (frame <= 0 || frame > (int32_t)sizeof(CallBuf) - 16)
            return;
        for (auto *u : units)
        {
            if (!IsLive(u))
                continue;
            std::memset(CallBuf, 0, (size_t)frame + 16);
            *reinterpret_cast<API::UObject **>(CallBuf + GvStopUnit) = u;
            coms->process_event(GvStopFn, CallBuf);
        }
    }

    // =====================================================================================
    // NAV POINTS (lancecontrol-22; VHUD v19). The mission's nav points are the game's MWObjectiveIndicator
    // actors whose marker widget is a NavPointObjectiveMarker (no widget yet and no unit attached counts too) -
    // real world positions, read once a second. Named Nav Alpha, Bravo, Charlie... (VhudNavName, NATO since lancecontrol-57): by the game's own
    // name when one of its text fields says "Nav <letter>", else by distance from the drop point (where your mech
    // first stood this mission), the nearest first; a name, once given, stays for the mission. Your own (TacOps
    // right-click: TCO_NavMark / TCO_NavUnmark) carry on the letters. Published under NavSeq; cleared per mission.
    struct NavKnown
    {
        API::UObject *Ind = nullptr; // the game's indicator (compared only); null = yours
        float         Loc[3] = {};
        int           Letter = 0;
    };
    std::vector<NavKnown> NavList;
    int       NavNext = 0;             // the next free letter
    float     NavDrop[3] = {};
    bool      NavHaveDrop = false, NavLoggedHow = false;
    ULONGLONG NavMs = 0;

    void NavForget()
    {
        NavList.clear();
        NavNext = 0;
        NavHaveDrop = NavLoggedHow = false;
        NavMs = 0;
        NavDiagLast.clear();
        NavDiagN = 0;
        NavPublish();
    }

    bool NavLetterUsed(int l) const
    {
        for (auto &n : NavList)
            if (n.Letter == l)
                return true;
        return false;
    }

    int NavTakeLetter()
    {
        // lancecontrol-55 (user): the LOWEST letter not in use - one freed by an unmark (VoiceCommand's hill
        // candidates not picked) is given again; never two nav points with one letter at once.
        // lancecontrol-57 (user): NATO names; Kilo ("two kilo" is a distance) and Victor (a mech) are never given out.
        // A nav the game itself names that way keeps its name (that path does not come here).
        static constexpr bool kSkip[kVhudNavLetters] = {false, false, false, false, false, false, false, false,
                                                        false, false, true /*Kilo*/, false, false, false, false, false,
                                                        false, false, false, false, false, true /*Victor*/, false, false,
                                                        false, false};
        for (int l = 0; l < kVhudNavLetters; ++l)
            if (!kSkip[l] && !NavLetterUsed(l))
                return l;
        return -1;
    }

    void NavPublish()
    {
        if (!Block)
            return;
        Block->NavSeq = Block->NavSeq + 1; // odd: writing
        MemoryBarrier();
        uint32_t k = 0;
        for (auto &n : NavList)
        {
            if (k >= (uint32_t)VanillaExportBlock::kNavMax)
                break;
            VanillaExportBlock::NavPoint &o = Block->Navs[k++];
            std::memcpy(o.Loc, n.Loc, sizeof(o.Loc));
            o.Letter = (uint8_t)n.Letter;
            o.Mine = n.Ind ? 0 : 1;
            o.Pad[0] = o.Pad[1] = 0;
        }
        Block->NavCount = k;
        MemoryBarrier();
        Block->NavSeq = Block->NavSeq + 1;
    }

    // "Nav Beta" in any text field of the indicator, its marker widget or its owner: that letter, else -1.
    int NavGameLetter(API::UObject *ind, API::UObject *widget)
    {
        std::string t = NavTexts(ind);
        if (widget)
            t += NavTexts(widget);
        if (API::UObject *ow = ObjProp(ind, L"Owner"))
            t += NavTexts(ow);
        for (auto &ch : t)
            ch = (char)std::tolower((unsigned char)ch);
        for (int l = 0; l < kVhudNavLetters; ++l)
        {
            std::string want = "nav " + WideToNarrow(VhudNavName(l));
            for (auto &ch : want)
                ch = (char)std::tolower((unsigned char)ch);
            const size_t at = t.find(want);
            if (at != std::string::npos && (at + want.size() >= t.size() || !std::isalpha((unsigned char)t[at + want.size()])))
                return l;
        }
        return -1;
    }

    // lancecontrol-25: nav points showed on no map - the indicators, logged when what is seen changes
    // (positions aside), at most 12 times a mission.
    std::wstring NavDiagLast;
    int          NavDiagN = 0;
    void NavDiag(const wchar_t *d)
    {
        if (NavDiagN >= 12 || NavDiagLast == d)
            return;
        NavDiagLast = d;
        ++NavDiagN;
        LogInfo("nav points: %ls", d);
    }

    void NavTick(API::UObject *pc, ULONGLONG now)
    {
        if (now - NavMs < 1000)
            return;
        NavMs = now;
        API::UObject *pawn = API::get()->get_local_pawn(0);
        if (!NavHaveDrop && pawn && CallRet(pawn, L"K2_GetActorLocation", {}, NavDrop, sizeof(NavDrop)))
            NavHaveDrop = true;
        static API::UClass *ic = nullptr;
        if (!ic)
            ic = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWObjectiveIndicator");
        std::vector<API::UObject *> inds;
        if (!ic || !ActorsOf(pc, ic, inds))
        {
            NavDiag(ic ? L"objective indicators: enumeration failed" : L"MWObjectiveIndicator class NOT FOUND");
            return;
        }
        std::wstring diag = L"objective indicators: " + std::to_wstring(inds.size());
        struct Fresh
        {
            API::UObject *Ind;
            float         Loc[3];
            int           Game;
            float         D2;
        };
        std::vector<Fresh> fresh;
        bool changed = false;
        std::vector<bool> seen(NavList.size(), false);
        for (auto *ind : inds)
        {
            if (!ind)
                continue;
            size_t k = 0;
            while (k < NavList.size() && NavList[k].Ind != ind)
                ++k;
            float p[3] = {};
            if (!CallRet(ind, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            if (k < NavList.size())
            {
                seen[k] = true;
                if (std::fabs(p[0] - NavList[k].Loc[0]) + std::fabs(p[1] - NavList[k].Loc[1]) > 100.0f)
                {
                    std::memcpy(NavList[k].Loc, p, sizeof(p));
                    changed = true;
                }
                continue;
            }
            API::UObject *w = nullptr, *as = nullptr;
            CallRet(ind, L"GetObjectiveMarkerWidget", {}, &w, sizeof(w));
            CallRet(ind, L"GetAssociatedActor", {}, &as, sizeof(as));
            // lancecontrol-26: GetObjectiveMarkerWidget hands back the marker's widget CLASS (the -25 log: every one
            // "WidgetBlueprintGeneratedClass"), so its kind is that class's OWN name - NavPointObjectiveMarker_C and
            // the like. An instance (older game builds) is still read by its class.
            std::wstring wname;
            if (w)
            {
                const std::wstring wc = ClassName(w);
                wname = (wc.find(L"Class") != std::wstring::npos && w->get_fname()) ? w->get_fname()->to_string() : wc;
            }
            // lancecontrol-27: the game's own nav point is a TravelLocationObjectiveMarker_C.
            const bool nav = w ? (wname.find(L"Nav") != std::wstring::npos || wname.find(L"TravelLocation") != std::wstring::npos)
                               : as == nullptr;
            // lancecontrol-25: what each indicator is, for the log
            diag += L" | " + ClassName(ind) + L" widget=" + (w ? wname : std::wstring(L"none")) + L" actor=" +
                    (as ? ClassName(as) : std::wstring(L"none")) + (nav ? L" -> NAV" : L" -> skipped");
            if (!nav)
                continue;
            Fresh f{ind, {p[0], p[1], p[2]}, NavGameLetter(ind, w), 0.0f};
            const float dx = p[0] - NavDrop[0], dy = p[1] - NavDrop[1];
            f.D2 = NavHaveDrop ? dx * dx + dy * dy : 0.0f;
            fresh.push_back(f);
        }
        NavDiag(diag.c_str());
        // the game's that are gone (their objective cleared away); yours stay
        for (size_t k = NavList.size(); k-- > 0;)
            if (NavList[k].Ind && !seen[k])
            {
                LogInfo("nav points: Nav %ls gone", VhudNavName(NavList[k].Letter));
                NavList.erase(NavList.begin() + (long)k);
                changed = true;
            }
        if (!fresh.empty())
        {
            std::sort(fresh.begin(), fresh.end(), [](const Fresh &a, const Fresh &b) { return a.D2 < b.D2; });
            int byGame = 0;
            for (auto &f : fresh) // the game's own names first
                if (f.Game >= 0 && !NavLetterUsed(f.Game) && (int)NavList.size() < VanillaExportBlock::kNavMax)
                {
                    NavKnown n;
                    n.Ind = f.Ind;
                    std::memcpy(n.Loc, f.Loc, sizeof(n.Loc));
                    n.Letter = f.Game;
                    NavList.push_back(n);
                    f.Ind = nullptr;
                    ++byGame;
                }
            for (auto &f : fresh) // then the nearest to the drop point first
            {
                if (!f.Ind || (int)NavList.size() >= VanillaExportBlock::kNavMax)
                    continue;
                const int l = NavTakeLetter();
                if (l < 0)
                    break;
                NavKnown n;
                n.Ind = f.Ind;
                std::memcpy(n.Loc, f.Loc, sizeof(n.Loc));
                n.Letter = l;
                NavList.push_back(n);
            }
            LogInfo("nav points: %zu new (%d named by the game, the rest by distance from the drop point%s) - %zu in all", fresh.size(),
                    byGame, NavHaveDrop ? "" : " - drop point unknown", NavList.size());
            float me[3] = {};
            const bool haveMe = pawn && CallRet(pawn, L"K2_GetActorLocation", {}, me, sizeof(me));
            for (auto &n : NavList)
            {
                const float dx = n.Loc[0] - me[0], dy = n.Loc[1] - me[1];
                LogInfo("nav points:   Nav %ls%ls at (%.0f, %.0f, %.0f) - %.0f m from you", VhudNavName(n.Letter), n.Ind ? L"" : L" (yours)",
                        n.Loc[0], n.Loc[1], n.Loc[2], haveMe ? std::sqrt(dx * dx + dy * dy) * 0.01f : -1.0f);
            }
            changed = true;
        }
        if (changed)
            NavPublish();
    }

    // TCO_NavMark / TCO_NavUnmark from TacOps (run at once, not squad orders).
    void NavOrder(const VanillaExportBlock::TacCmd &c)
    {
        if (c.Order == VanillaExportBlock::TCO_NavMark)
        {
            if ((int)NavList.size() >= VanillaExportBlock::kNavMax)
            {
                LogWarn("nav points: mark ignored - %d already", VanillaExportBlock::kNavMax);
                return;
            }
            const int l = NavTakeLetter();
            if (l < 0)
                return;
            NavKnown n;
            std::memcpy(n.Loc, c.Loc, sizeof(n.Loc));
            n.Letter = l;
            NavList.push_back(n);
            LogInfo("nav points: Nav %ls marked by you at (%.0f, %.0f)", VhudNavName(l), c.Loc[0], c.Loc[1]);
        }
        else
        {
            size_t best = NavList.size();
            float bd = 6000.0f * 6000.0f;
            for (size_t k = 0; k < NavList.size(); ++k)
            {
                if (NavList[k].Ind)
                    continue;
                const float dx = NavList[k].Loc[0] - c.Loc[0], dy = NavList[k].Loc[1] - c.Loc[1];
                if (dx * dx + dy * dy < bd)
                {
                    bd = dx * dx + dy * dy;
                    best = k;
                }
            }
            if (best == NavList.size())
                return;
            LogInfo("nav points: Nav %ls (yours) removed", VhudNavName(NavList[best].Letter));
            NavList.erase(NavList.begin() + (long)best);
        }
        NavPublish();
    }

    std::string NavTexts(API::UObject *o)
    {
        std::string r;
        for (auto *st = static_cast<API::UStruct *>(o->get_class()); st; st = st->get_super_struct())
        {
            const std::wstring sn = st->get_fname() ? st->get_fname()->to_string() : L"";
            if (sn == L"Actor" || sn == L"Object" || sn == L"Info")
                break;
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
            {
                auto *q = reinterpret_cast<API::FProperty *>(f);
                uint8_t *a = reinterpret_cast<uint8_t *>(o) + q->get_offset();
                const std::wstring ty = PropType(q);
                auto one = [&](const std::wstring &nm, API::FProperty *pp, uint8_t *at) {
                    const std::wstring t2 = PropType(pp);
                    std::string v;
                    if (t2 == L"TextProperty")
                        v = DiText(at);
                    else if (t2 == L"NameProperty")
                        v = WideToNarrow(reinterpret_cast<API::FName *>(at)->to_string().c_str());
                    else
                        return;
                    if (!v.empty() && v != "None" && r.size() < 900)
                        r += " " + WideToNarrow(nm.c_str()) + "=\"" + v.substr(0, 80) + "\"";
                };
                if (ty == L"StructProperty")
                {
                    auto *ss = reinterpret_cast<API::FStructProperty *>(q)->get_struct();
                    for (auto *g = ss ? ss->get_child_properties() : nullptr; g; g = g->get_next())
                    {
                        auto *qq = reinterpret_cast<API::FProperty *>(g);
                        one(f->get_fname()->to_string() + L"." + g->get_fname()->to_string(), qq, a + qq->get_offset());
                    }
                }
                else
                    one(f->get_fname()->to_string(), q, a);
            }
        }
        return r;
    }

    // MISSION EVENTS (lancecontrol-20; the lancecontrol-18 probe showed they fire reliably, in the field and in
    // the hangar, on the player's MWSquad only). MWSquad::OnMissionStart and ::OnMissionCleanup are detoured by
    // their Func pointer; either one sets MpPending, and GameTick resets everything per mission on its next tick
    // (both firing just resets twice - harmless). A takeover between them keeps the lance.
    // =====================================================================================
    struct MpHook
    {
        API::UFunction *Fn = nullptr;
        BP_FUNC         Orig = nullptr;
        std::wstring    Label;
        int             Fired = 0;
    };

    static constexpr int kMpMax = 8, kMpMaxFired = 20; // per function, for the session

    MpHook MpHooks[kMpMax];

    int MpCount = 0;

    bool MpTried = false;

    volatile LONG MpPending = 0; // set by a mission event, taken by GameTick

    template <int N> static void *MpDetour(API::UObject *self, FFrame *frame, void *const result)
    {
        if (Instance)
            MpSeeSafe(N, self, frame);
        BP_FUNC o = Instance ? Instance->MpHooks[N].Orig : nullptr;
        return o ? o(self, frame, result) : nullptr;
    }

    static void MpSeeSafe(int n, API::UObject *self, FFrame *frame)
    {
        __try
        {
            Instance->MpSee(n, self, frame);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void MpSee(int n, API::UObject *self, FFrame *frame)
    {
        if (n < 0 || n >= MpCount)
            return;
        MpHook &h = MpHooks[n];
        InterlockedExchange(&MpPending, 1);
        if (++h.Fired > kMpMaxFired)
            return;
        const bool own = frame && frame->Node == h.Fn;
        const std::wstring via = own ? L"" : (L" (called from script " + ((frame && frame->Node && frame->Node->get_fname()) ? frame->Node->get_fname()->to_string() : std::wstring(L"?")) + L")");
        LogInfo("mission event: %ls on %ls%ls - your mech %ls - the lance is reset", h.Label.c_str(), ObjLabel(self).c_str(), via.c_str(),
                ObjLabel(API::get()->get_local_pawn(0)).c_str());
    }

    void MpTick()
    {
        if (!MpTried)
        {
            MpTried = true;
            static BP_FUNC det[kMpMax] = {&MpDetour<0>, &MpDetour<1>, &MpDetour<2>, &MpDetour<3>,
                                          &MpDetour<4>, &MpDetour<5>, &MpDetour<6>, &MpDetour<7>};
            static const wchar_t *want[][2] = {{L"MWSquad", L"OnMissionStart"}, {L"MWSquad", L"OnMissionCleanup"}};
            for (auto &w : want)
            {
                if (MpCount >= kMpMax)
                    break;
                const std::wstring label = std::wstring(w[0]) + L"::" + w[1];
                auto *cls = API::get()->find_uobject<API::UClass>(std::wstring(L"Class /Script/MechWarrior.") + w[0]);
                API::UFunction *fn = cls ? cls->find_function(w[1]) : nullptr;
                if (!fn)
                {
                    LogWarn("mission events: %ls not found - new missions told by the old guesses", label.c_str());
                    continue;
                }
                auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
                MpHook &h = MpHooks[MpCount];
                h.Fn = fn;
                h.Orig = *slot;
                h.Label = label;
                *slot = det[MpCount];
                ++MpCount;
                LogInfo("mission events: hooked %ls", label.c_str());
            }
        }
    }

    // A new HUD, controller or world: everything held from the old one is gone or about to be.
    // Only objects still verifiably on the CURRENT controller are touched; the rest are forgotten.
    void ForgetWorldLance(API::UObject *pc)
    {
        LsOffs.clear(); // lancecontrol-44: offsets per class, per mission (a class address can be reused later)
        AimMarks.clear(); // lancecontrol-52: the marks were actors of the level just left
        AmGravZ = 0.0f;
        LsPosT.clear(); // lancecontrol-46: positions re-checked per class each mission
        LsVtolC.clear();
        LsBoolP.clear();
        LsPosLogs = 0;
        LsSnap.clear();
        LsSnapOk = false;
        AimProbeInvDone = false; // lancecontrol-41: aim-writer probe, per mission
        AimProbeLines = 0;
        AimProbeMine.clear();
        for (auto &h : ProbeHooks)
            if (h.Label.rfind(L"aim probe: ", 0) == 0)
                h.Fired = 0;
        NavForget(); // NAV POINTS (lancecontrol-22): per mission
        DeadLatch.clear(); // lancecontrol-23
        // lancecontrol-72 (bug sweep 24): caches keyed by a class address - a class can be freed with the level and another
        // class put at its address (a non-weapon component then taken for a weapon, a stale UFunction called on it).
        AiWpnClassIs.clear();
        Fns.clear();
        LsMeleeCls.clear();
        LsAmmoCls.clear();
        IcHeatCls.clear();
        BaClassCache.clear();
        // ... and moves still pending from the last mission (GoToPendTick / GoToFix sent the new lancemate to the old map)
        for (int s = 0; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            TacPendMs[s] = 0;
            TacPendDirect[s] = 0;
        }
        for (int k = 0; k < 9; ++k) // FLANKED (lancecontrol-21)
        {
            LsFlankCand[k] = nullptr;
            LsFlankMs[k] = 0;
        }
        // FIRST (vanillahud-92): a lancemate we fire for that is STILL in the level (a HUD or
        // controller change mid-mission) is handed back properly - stopped, the game's firing as it
        // was - and moved aim points go back. The live list is re-read, so the old world's units
        // (gone with it) are never touched.
        LivePawnsMs = 0;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            if (IcMaskSet[s] && TacUnit[s] && IsLive(TacUnit[s]) && !IsDeadUnit(TacUnit[s]))
            {
                API::UObject *u = TacUnit[s];
                IcDirStop(s, true, 0);
                for (auto *w : IcFiring[s])
                    if (OnMech(u, w))
                        IcCallNoArgs(w, L"K2_StopFire");
                IcSuppress(u, IcMaskOld[s] == 1);
                TacLog("slot %d - handed back to the game (controller changed)", s);
            }
        IcRestoreAims(0, true);
        (void)pc;
        ProbeHooks.clear(); // re-listed (and re-counted) on the next probe pass
        GvLogged = 0; // game voices (lancecontrol-17): the hook stays, the log cap is per mission
        GvStop.clear();
        GvComs = nullptr;
        Di.clear();   // the game's lance panel: read afresh
        DiMs = 0;
        ProbeNextMs = 0;
        // TacOps: the squad and its units belong to the old world. The layout (functions,
        // offsets, order classes) is re-resolved too - the order classes can be reloaded.
        for (auto &u : TacUnit)
            u = nullptr;
        TacMapMs       = 0;
        TacMapLogged   = false;
        TacScanMs      = 0; // the order numbers are asked for again in the new world
        for (auto &r : TacRecipOf)
            r = 0;
        TacSticky.clear(); // lance numbers are kept for one mission only (lancecontrol-3)
        TacPanelWaitMs = 0;
        TacPanelFallbackLogged = false;
        TacPosCache.clear();
        TacMapSig.clear();
        TacLayoutTried = false;
        GoToBeginFn = nullptr; // re-found (and re-hooked if the class was reloaded) in the new world
        GoToNextTryMs = 0;
        ApOn = false; // the autopilot's mech and path belong to the old world
        ApPath.clear();
        // Move echoes belong to the old world too: a new mission filling the same slots must not
        // publish them.
        for (auto &q : TacLastSeq)
            q = 0;
        std::memset(TacLastMove, 0, sizeof(TacLastMove));
        // The pilot watch's cached handler layout: re-read from the new world's handler.
        PwCls = nullptr;
        PwFields.clear();
        for (auto &h : HoldOn)
            h = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            IcOn[s] = false; // the mechs (and their weapon systems) are gone with the world
            IcMaskSet[s] = false;
            IcFiring[s].clear();
            HoldIndirect[s] = false;
            // Insane allies and the indirect hold: every per-lancemate thing, reset only - nothing
            // of the old world is called (vanillahud-88: the next mission started with stale state).
            IaSet[s] = false;
            IaTgt[s] = nullptr;
            IaOkMs[s] = IaNoneLogMs[s] = 0;
            IaSwitches[s] = 0;
            AtkTgt[s] = nullptr;
            AtkByLock[s] = false;
            AtkSinceMs[s] = AtkSeenMs[s] = AtkLockLostMs[s] = 0;
            FlOn[s] = FlBack[s] = FlGoalSet[s] = false; // lancecontrol-75: flanks are per mission
            FlTgt[s] = nullptr;
            CsOrdPart[s] = 0; // lancecontrol-32: called shots are per mission
            CsOrdTgt[s] = nullptr;
            IcWpns[s].clear();
            IcMelee[s].clear(); // lancecontrol-83
            IcWpnList[s].clear();
            IcDirFiring[s].clear();
            IcDirFires[s] = IcOwnFires[s] = IcDirectShots[s] = 0;
            IcDrop[s].clear();
            IcTT[s] = nullptr;
            IcTTOk[s] = IcTTLogMs[s] = 0;
            IcTTSwitches[s] = 0;
            IcWatchTgt[s] = nullptr;
            IcNoFire[s] = nullptr;
            IcMaskOld[s] = 0;
            IcSpotter[s] = false;
            IcFfLogMs[s] = IcAimLogMs[s] = 0;
        }
        OnMechPawn = nullptr;
        OnMechList.clear();
        IcMoved.clear(); // anything not put back above belonged to the old world
        for (auto &t : IcSpreadTgt) // lancecontrol-59
            t = nullptr;
        FsShooters.clear();
        WStats.clear(); // class addresses can be reused by the next level
        // Fire settings reset every mission (vanillahud-92).
        bool hadFs = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            hadFs |= FsCustom(s);
            FsDisc[s] = 0;
            FsMask[s] = kFcAll;
            FsHeat[s] = 0;
            IcHeatProjSet[s] = false;
            IaRetryMs[s] = 0;
            IcCantFire[s] = IcNoFireLogged[s] = nullptr;
        }
        if (hadFs)
            TacLog("fire settings - reset to defaults for the new mission");
        LivePawns.clear();
        LivePawnsMs = 0;
        AimWatchLast.clear();
        AmmoSeen.clear();
        AmmoLast.clear();
        for (int s = 0; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            LsPrevUnit[s] = LsPrevTgt[s] = LsFireTgt[s] = nullptr;
            LsPrevAlive[s] = LsHot[s] = LsDmgInit[s] = false;
            LsPrevSeen[s] = 0;
            LsWpns[s].clear();
        }
        LsPrevLock = nullptr;
        LockWant = nullptr; // (a new world's units are new)
        ShotPrev.clear();
        ShotBy.clear();
        LsEnemyPrev.clear();
        LsClasses.clear();
        for (int s = 0; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            LsCondUnit[s] = nullptr; // condition of the old world's units: read afresh
            LsCondMs[s] = 0;
            LsUav[s] = -1;
        }
        LsLivingLayouts.clear(); // class addresses can be reused by the next level
        LsContLayouts.clear();
        ArcTracks.clear();
        ArcSeen.clear();
        ArcPerClass.clear();
        ArcLearnSeen.clear();
        ArcUnknownLogged.clear();
        ArcWpnInfos.clear(); // class addresses can be reused by the next level (learned profiles are kept: by name)
        IcLockLearned.clear();
        IcRangeCache.clear();
        UnitClassCache.clear();
        for (auto &v : ArcVerdicts)
            v.clear();
        for (auto &b : AmmoMechLogged)
            b = false;
        AiCvarsLogged = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            if (AiSt[s].Pawn)
            {
                AiEpisodeEnd(s, "mission ended");
                AiCondReport(s);
            }
        // ai_probe.log carries on in the same file: a marker, and the console settings again.
        AiLog(L"==================== NEW MISSION (controller changed) ====================");
        AiRdt.clear();
        AiRdtClasses.clear();
        AiRdtYou = nullptr;
        for (auto &a : AiSt)
            a = AiSlot{};
        AiProjTried = false; // re-resolved in the new world
        AiProjCls = AiPathCls = nullptr;
        AiProjSeen.clear();
        AiPathSeen.clear();
        AiPendingLock.clear();
        AiTeamShared.clear();
        AiTeamSharer = nullptr;
        AiBbAsset = nullptr;
        AiScans = 0;   // blueprint node classes of the new world: scanned again
        AiScanMs = 0;
        AiRates.clear();
        TorsoMode = TMO_Off; // its mech and target belonged to the old world
        TorsoTgt = nullptr;
        for (auto &f : FomOn)
            f = false;
        MovePawn = nullptr; // the movement probe's watch list points into the old mech
        MoveWatches.clear();
        LTraces.clear();    // and the lancemate traces into the old lance
        LDumped.clear();
        LTraceLines = 0;    // the cap is per mission (it ran out in an earlier one last time)
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

    API::UClass *RayClass = nullptr;

    bool         RayTried = false;

    struct RayLayout
    {
        const void         *Cls;
        int32_t             Hit;
        API::FBoolProperty *Active;
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
            if (to[0] == 0.0f && to[1] == 0.0f && to[2] == 0.0f)
                continue;
            float from[3] = {};
            API::UObject *fx = ObjProp(r, L"EffectsLocationComponent");
            if (!(fx && CallRet(fx, L"K2_GetComponentLocation", {}, from, sizeof(from))))
                CallRet(r, L"K2_GetActorLocation", {}, from, sizeof(from));
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

    // =====================================================================================
    // LANCE ORDER PROBE (build vanillahud-19) - DISCOVERY ONLY: it logs, it never issues an order.
    //
    // What the game's files say (Dpad_MoveToLogic / Dpad_BaseLanceOrder / SquadOrders/*):
    //   D-pad order -> MWBlueprintUtils::GetSquadCommandOrderContext(LocalController)
    //              -> make MWSquadCommandOrderInfo {Instigator, OrderRecipients, Priority,
    //                 bPersistent, OrderContext}
    //              -> MWPlayerController::PostSquadOrderClass(OrderClass, Info)
    //   OrderClass is GoToLocationOrder_C / AttackOrder_C / FollowOrder_C /
    //   ToggleHoldFireWeaponsFreeOrder_C; its jobs read OrderContext.TargetLocation (move) and
    //   OrderContext.TargetActor / TargetUnit (attack).
    // Nothing names the native structs' layouts or what OrderRecipients holds. So:
    //   1. once per session: the two structs' layouts and the order-related functions' signatures;
    //   2. every time an order runs: the order object's event, with every parameter decoded.
    // Only BLUEPRINT functions are detoured (the same func-pointer swap as HUD_C::Tick), and only
    // those a native caller reaches through ProcessEvent, where the frame's Locals are the params.
    // =====================================================================================
    struct ProbeHook
    {
        API::UFunction *Fn;
        std::wstring    Label;
        int             Fired;
    };

    std::vector<ProbeHook> ProbeHooks;

    BP_FUNC   ProbeOrig        = nullptr; // UObject::ProcessInternal - every script function's Func

    bool      ProbeStaticDone  = false;

    ULONGLONG ProbeNextMs      = 0;

    int       ProbeLines       = 0;

    static constexpr int kProbeMaxFired = 12; // per hooked function, per world

    static constexpr uint32_t kFuncNative = 0x00000400;

    void ProbeLance(API::UObject *pc, ULONGLONG now)
    {
        if (!kDiscoveryProbes)
            return;
        if (now < ProbeNextMs)
            return;
        ProbeNextMs = now + 3000;
        if (!ProbeStaticDone)
            ProbeStaticDone = ProbeStatic(pc);
        ProbeInstallHooks();
    }

    void ProbeInstallHooks()
    {
        static const wchar_t *kOrders[] = {
            L"SquadOrderBase", L"GoToLocationOrder", L"AttackOrder", L"FollowOrder",
            L"ToggleHoldFireWeaponsFreeOrder", L"ToggleECMMode"};
        static const wchar_t *kLogic[] = {
            L"Dpad_BaseLanceOrder", L"Dpad_MoveToLogic", L"Dpad_AttackTargetLogic", L"Dpad_FormOnMeLogic",
            L"Dpad_ToggleHoldFireLogic", L"Dpad_LanceOrderDispatcher"};
        auto hookAll = [&](const std::wstring &dir, const wchar_t *const *names, size_t n,
                           std::initializer_list<const wchar_t *> fns) {
            for (size_t i = 0; i < n; ++i)
            {
                const std::wstring full = L"BlueprintGeneratedClass " + dir + names[i] + L"." + names[i] + L"_C";
                auto *cls = API::get()->find_uobject<API::UClass>(full);
                if (!cls)
                    continue;
                for (auto *fname : fns)
                    ProbeHookFn(cls, fname, std::wstring(names[i]) + L"::" + fname);
            }
        };
        hookAll(L"/Game/Modes/MissionDirectorSystem/MissionComponents/SquadOrders/", kOrders, 6,
                {L"ExecuteOrder", L"IssueOrder", L"StopIssuedOrder"});
        hookAll(L"/Game/UI/Mech/DPad/DpadActionLogic/LanceCommands/", kLogic, 6,
                {L"Activate", L"DispatchOrders", L"DispatchOrders_DLC", L"DispatchOrders_NoDLC"});
    }

    void ProbeHookFn(API::UClass *cls, const wchar_t *name, const std::wstring &label)
    {
        auto *fn = cls->find_function(name);
        if (!fn)
            return;
        for (auto &h : ProbeHooks)
            if (h.Fn == fn)
                return;
        if (fn->get_function_flags() & kFuncNative)
        {
            ProbeHooks.push_back({fn, label, 0});
            LogInfo("lance probe: %ls is native - not hooked", label.c_str());
            return;
        }
        auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
        if (*slot == &OnProbeFn)
        {
            ProbeHooks.push_back({fn, label, 0}); // still hooked from an earlier world
            return;
        }
        if (!ProbeOrig)
            ProbeOrig = *slot;
        if (*slot != ProbeOrig)
        {
            ProbeHooks.push_back({fn, label, 0});
            LogInfo("lance probe: %ls has a different Func (%p vs %p) - not hooked", label.c_str(), (void *)*slot, (void *)ProbeOrig);
            return;
        }
        *slot = &OnProbeFn;
        ProbeHooks.push_back({fn, label, 0});
        LogInfo("lance probe: hooked %ls (%p)", label.c_str(), (void *)fn);
    }

    static void *OnProbeFn(API::UObject *self, FFrame *frame, void *const result)
    {
        if (Instance)
            ProbeLogSafe(self, frame); // before the original: the params are untouched
        return (Instance && Instance->ProbeOrig) ? Instance->ProbeOrig(self, frame, result) : nullptr;
    }

    // No C++ object lives in this frame, so SEH may guard it: a bad pointer in a log costs a line,
    // never the game.
    static void ProbeLogSafe(API::UObject *self, FFrame *frame)
    {
        __try
        {
            Instance->ProbeLog(self, frame);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void ProbeLog(API::UObject *self, FFrame *frame)
    {
        if (!frame)
            return;
        API::UFunction *fn = frame->Node;
        ProbeHook *hook = nullptr;
        for (auto &h : ProbeHooks)
            if (h.Fn == fn)
                hook = &h;
        // lancecontrol-41: the aim-writer probe's hooks log only calls on our lancemates' objects (enemies use them too)
        if (hook && hook->Label.rfind(L"aim probe: ", 0) == 0 && !AimProbeOurs(self))
            return;
        if (!hook || hook->Fired >= kProbeMaxFired)
            return;
        if (++hook->Fired == kProbeMaxFired)
            LogInfo("lance probe: %ls fired %d times - quiet for the rest of this mission", hook->Label.c_str(), kProbeMaxFired);
        ProbeLines = 0;
        const std::wstring label = hook->Label;
        LogInfo("lance probe: FIRED %ls on %ls (self %p, locals %p)", label.c_str(), ClassName(self).c_str(),
                (void *)self, (void *)frame->Locals);
        if (fn && frame->Locals)
            for (auto *f = fn->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                if (p->is_param())
                    ProbeDump(f->get_fname()->to_string(), p, frame->Locals + p->get_offset(), 0);
            }
        // The D-pad logic object carries its own order settings (OrderClass, Priority...).
        if (self && self->get_class() && label.find(L"Dpad_") == 0)
        {
            int n = 0;
            for (auto *st = static_cast<API::UStruct *>(self->get_class()); st && n < 3; st = st->get_super_struct(), ++n)
                for (auto *f = st->get_child_properties(); f; f = f->get_next())
                {
                    auto *p = reinterpret_cast<API::FProperty *>(f);
                    ProbeDump(L"self." + f->get_fname()->to_string(), p, reinterpret_cast<uint8_t *>(self) + p->get_offset(), 1);
                }
        }
        if (label.find(L"Order::") != std::wstring::npos || label.find(L"Mode::") != std::wstring::npos)
            ProbeLanceMates();
    }

    static std::wstring PropType(API::FProperty *p)
    {
        return (p && p->get_class()) ? p->get_class()->get_name() : L"?";
    }

    // No liveness test here: UObjectHook::exists() answered "not live" for the player's own
    // controller in vanillahud-19, so it is not a usable test in this game. The SEH wrapper around
    // every probe log is the guard instead. Identity against the player's pawn and controller is
    // what TacOps needs to know about Instigator / IssuerPawn.
    static std::wstring ObjLabel(API::UObject *o)
    {
        if (!o)
            return L"null";
        wchar_t tag[64];
        std::swprintf(tag, 64, L"%p ", (void *)o);
        std::wstring r = tag;
        if (o == API::get()->get_player_controller(0))
            r += L"[= PLAYER CONTROLLER] ";
        if (o == API::get()->get_local_pawn(0))
            r += L"[= PLAYER PAWN] ";
        return r + o->get_full_name();
    }

    // One value, decoded by its reflected type. Structs recurse (depth 3), arrays show their first
    // elements. One log line per leaf, capped per event.
    void ProbeDump(const std::wstring &path, API::FProperty *p, uint8_t *a, int depth)
    {
        if (!p || !a || ProbeLines > 160)
            return;
        const std::wstring ty = PropType(p);
        char buf[512];
        buf[0] = 0;
        if (ty == L"IntProperty")
            std::snprintf(buf, sizeof(buf), "%d", *reinterpret_cast<int32_t *>(a));
        else if (ty == L"FloatProperty")
            std::snprintf(buf, sizeof(buf), "%g", *reinterpret_cast<float *>(a));
        else if (ty == L"BoolProperty")
            std::snprintf(buf, sizeof(buf), "%s", reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_propbase(a) ? "true" : "false");
        else if (ty == L"ByteProperty" || ty == L"EnumProperty")
            std::snprintf(buf, sizeof(buf), "%u", (unsigned)*a);
        else if (ty == L"NameProperty")
            std::snprintf(buf, sizeof(buf), "'%s'", WideToNarrow(reinterpret_cast<API::FName *>(a)->to_string().c_str()).c_str());
        else if (ty == L"ObjectProperty" || ty == L"ClassProperty" || ty == L"InterfaceProperty")
            std::snprintf(buf, sizeof(buf), "%s", WideToNarrow(ObjLabel(*reinterpret_cast<API::UObject **>(a)).c_str()).c_str());
        else if (ty == L"WeakObjectProperty")
        {
            const int32_t idx = *reinterpret_cast<int32_t *>(a);
            auto *arr = API::get()->get_uobject_array();
            API::UObject *o = (arr && idx > 0) ? arr->get_object(idx) : nullptr;
            std::snprintf(buf, sizeof(buf), "weak #%d -> %s", idx, WideToNarrow(ObjLabel(o).c_str()).c_str());
        }
        else if (ty == L"StrProperty")
        {
            const wchar_t *s = *reinterpret_cast<wchar_t **>(a);
            const int32_t  n = *reinterpret_cast<int32_t *>(a + 8);
            std::snprintf(buf, sizeof(buf), "\"%s\"", (s && n > 0 && n < 200) ? WideToNarrow(s).c_str() : "");
        }
        else if (ty == L"TextProperty") // lancecontrol-21: the text itself
            std::snprintf(buf, sizeof(buf), "text \"%s\"", DiText(a).substr(0, 200).c_str());
        else if (ty == L"StructProperty")
        {
            auto *st = reinterpret_cast<API::FStructProperty *>(p)->get_struct();
            const std::wstring sn = (st && st->get_fname()) ? st->get_fname()->to_string() : L"?";
            if (sn == L"Vector" || sn == L"Rotator")
            {
                const float *v = reinterpret_cast<float *>(a);
                std::snprintf(buf, sizeof(buf), "%s(%.1f, %.1f, %.1f)", WideToNarrow(sn.c_str()).c_str(), v[0], v[1], v[2]);
            }
            else if (st && depth < 3)
            {
                ++ProbeLines;
                LogInfo("lance probe:   %ls : struct %ls", path.c_str(), sn.c_str());
                for (auto *f = st->get_child_properties(); f; f = f->get_next())
                {
                    auto *q = reinterpret_cast<API::FProperty *>(f);
                    ProbeDump(path + L"." + f->get_fname()->to_string(), q, a + q->get_offset(), depth + 1);
                }
                return;
            }
            else
                std::snprintf(buf, sizeof(buf), "struct %s", WideToNarrow(sn.c_str()).c_str());
        }
        else if (ty == L"ArrayProperty")
        {
            uint8_t *data = *reinterpret_cast<uint8_t **>(a);
            const int32_t n = *reinterpret_cast<int32_t *>(a + 8);
            auto *inner = reinterpret_cast<API::FArrayProperty *>(p)->get_inner();
            const std::wstring it = PropType(inner);
            ++ProbeLines;
            LogInfo("lance probe:   %ls : array<%ls> x%d", path.c_str(), it.c_str(), n);
            if (!data || n <= 0 || n > 4096 || !inner)
                return;
            int32_t stride = 0;
            if (it == L"IntProperty" || it == L"FloatProperty")
                stride = 4;
            else if (it == L"ObjectProperty" || it == L"ClassProperty")
                stride = 8;
            else if (it == L"StructProperty")
            {
                auto *es = reinterpret_cast<API::FStructProperty *>(inner)->get_struct();
                if (es)
                {
                    const int32_t sz = es->get_properties_size(), al = es->get_min_alignment();
                    stride = al > 0 ? ((sz + al - 1) / al) * al : sz;
                }
            }
            if (stride <= 0 || depth >= 3)
                return;
            for (int32_t i = 0; i < n && i < 12; ++i)
                ProbeDump(path + L"[" + std::to_wstring(i) + L"]", inner, data + (size_t)i * stride, depth + 1);
            return;
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "(%s) %02x %02x %02x %02x %02x %02x %02x %02x", WideToNarrow(ty.c_str()).c_str(),
                          a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        }
        ++ProbeLines;
        LogInfo("lance probe:   %ls = %s", path.c_str(), buf);
    }

    // Every mech: whose it is, its controller, and every lance/squad-ish member on either, so
    // OrderRecipients' numbers can be matched to a mech.
    void ProbeLanceMates()
    {
        auto *pc = API::get()->get_player_controller(0);
        auto *mechCls = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWMech");
        std::vector<API::UObject *> mechs;
        if (!pc || !mechCls || !ActorsOf(pc, mechCls, mechs))
            return;
        auto *mine = API::get()->get_local_pawn(0);
        int logged = 0;
        for (auto *m : mechs)
        {
            if (!m || logged >= 16)
                break;
            API::UObject *ctl = ObjProp(m, L"Controller");
            const std::wstring cc = ClassName(ctl);
            auto *sq = m->get_class() ? FindProp(m->get_class(), L"bIsInPlayerSquad") : nullptr;
            const bool inSquad = sq && PropType(sq) == L"BoolProperty" &&
                                 reinterpret_cast<API::FBoolProperty *>(sq)->get_value_from_object(m);
            if (m != mine && !inSquad)
                continue; // enemies and neutrals - not lancemates
            ++logged;
            // The squad slot the game itself gives this unit - what OrderRecipients' numbers are.
            int32_t slot = -99;
            static API::UObject *utils = nullptr;
            if (!utils)
                if (auto *uc = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWBlueprintUtils"))
                    utils = uc->get_class_default_object();
            if (utils)
                CallRet(utils, L"GetIndexOfUnitInSquad", {{L"WorldContextObject", &pc, sizeof(void *)}, {L"Unit", &m, sizeof(void *)}},
                        &slot, sizeof(slot));
            LogInfo("lance probe: mech %p %ls%s  squad slot %d  controller %p %ls", (void *)m, m->get_full_name().c_str(),
                    m == mine ? " (PLAYER)" : "", slot, (void *)ctl, cc.c_str());
            ProbeLines = 0;
            for (API::UObject *o : {m, ctl})
                if (o && o->get_class())
                    for (auto *st = static_cast<API::UStruct *>(o->get_class()); st; st = st->get_super_struct())
                        for (auto *f = st->get_child_properties(); f; f = f->get_next())
                        {
                            const std::wstring n = f->get_fname()->to_string();
                            if (n.find(L"Lance") == std::wstring::npos && n.find(L"Squad") == std::wstring::npos &&
                                n.find(L"Order") == std::wstring::npos)
                                continue;
                            auto *p = reinterpret_cast<API::FProperty *>(f);
                            ProbeDump((o == m ? L"  pawn." : L"  ctl.") + n, p, reinterpret_cast<uint8_t *>(o) + p->get_offset(), 2);
                        }
        }
    }

    static std::wstring Signature(API::UFunction *fn)
    {
        std::wstring s = fn->get_fname()->to_string() + L"(";
        std::wstring ret;
        bool first = true;
        for (auto *f = fn->get_child_properties(); f; f = f->get_next())
        {
            auto *p = reinterpret_cast<API::FProperty *>(f);
            if (!p->is_param())
                continue;
            std::wstring one = PropType(p) + L" " + f->get_fname()->to_string() + L"@" + std::to_wstring(p->get_offset());
            if (p->get_class() && PropType(p) == L"StructProperty")
                if (auto *st = reinterpret_cast<API::FStructProperty *>(p)->get_struct())
                    one += L"<" + st->get_fname()->to_string() + L">";
            if (p->is_return_param())
            {
                ret = one;
                continue;
            }
            if (p->is_out_param())
                one = L"out " + one;
            s += (first ? L"" : L", ") + one;
            first = false;
        }
        s += L")";
        if (!ret.empty())
            s += L" -> " + ret;
        s += L"  frame " + std::to_wstring(fn->get_properties_size());
        return s;
    }

    void ProbeLayout(const wchar_t *label, API::UStruct *st, int depth)
    {
        if (!st)
        {
            LogInfo("lance probe: layout %ls: not loaded", label);
            return;
        }
        const int32_t size = st->get_properties_size();
        LogInfo("lance probe: layout %ls (%ls, %d bytes)", label, st->get_fname()->to_string().c_str(), size);
        for (auto *f = st->get_child_properties(); f; f = f->get_next())
        {
            auto *p = reinterpret_cast<API::FProperty *>(f);
            std::wstring extra;
            const std::wstring ty = PropType(p);
            API::UStruct *sub = nullptr;
            if (ty == L"StructProperty")
                sub = reinterpret_cast<API::FStructProperty *>(p)->get_struct();
            else if (ty == L"ArrayProperty")
            {
                auto *in = reinterpret_cast<API::FArrayProperty *>(p)->get_inner();
                extra = L" <" + PropType(in) + L">";
                if (in && PropType(in) == L"StructProperty")
                    sub = reinterpret_cast<API::FStructProperty *>(in)->get_struct();
            }
            if (sub)
                extra += L" {" + sub->get_fname()->to_string() + L"}";
            LogInfo("lance probe:   %*s@%-4d %ls %ls%ls", depth * 2, "", p->get_offset(), ty.c_str(),
                    f->get_fname()->to_string().c_str(), extra.c_str());
            if (sub && depth < 2)
            {
                const std::wstring sn = sub->get_fname()->to_string();
                if (sn != L"Vector" && sn != L"Rotator" && sn != L"Guid" && sn != L"LinearColor")
                    ProbeLayout(sn.c_str(), sub, depth + 1);
            }
        }
    }

    // Functions of `cls` itself (not its supers) whose name has any of `keys` (all when empty).
    void ProbeFunctions(const wchar_t *label, API::UClass *cls, std::initializer_list<const wchar_t *> keys)
    {
        if (!cls)
        {
            LogInfo("lance probe: class %ls not loaded", label);
            return;
        }
        auto *fnClass = API::UFunction::static_class();
        int n = 0;
        for (auto *f = cls->get_children(); f; f = f->get_next())
        {
            if (f->get_class() != fnClass)
                continue;
            auto *fn = reinterpret_cast<API::UFunction *>(f);
            const std::wstring name = fn->get_fname()->to_string();
            bool want = keys.size() == 0;
            for (auto *k : keys)
                want |= name.find(k) != std::wstring::npos;
            if (!want)
                continue;
            ++n;
            LogInfo("lance probe: fn %ls::%ls%s", label, Signature(fn).c_str(),
                    (fn->get_function_flags() & kFuncNative) ? " [native]" : "");
        }
        LogInfo("lance probe: %ls - %d matching function(s)", label, n);
    }

    bool ProbeStatic(API::UObject *pc)
    {
        auto *info = API::get()->find_uobject<API::UScriptStruct>(L"ScriptStruct /Script/MechWarrior.MWSquadCommandOrderInfo");
        auto *ctx  = API::get()->find_uobject<API::UScriptStruct>(L"ScriptStruct /Script/MechWarrior.MWSquadCommandOrderContext");
        if (!info && !ctx)
            return false; // not loaded yet: try again in 3 s
        LogInfo("lance probe: ---- static discovery (build vanillahud-19) ----");
        ProbeLayout(L"MWSquadCommandOrderInfo", info, 0);
        ProbeLayout(L"MWSquadCommandOrderContext", ctx, 0);

        auto cls = [](const wchar_t *n) { return API::get()->find_uobject<API::UClass>(n); };
        const std::initializer_list<const wchar_t *> keys = {L"Squad", L"Order", L"Lance", L"Command", L"Recipient", L"Member"};
        ProbeFunctions(L"MWPlayerController", cls(L"Class /Script/MechWarrior.MWPlayerController"), keys);
        ProbeFunctions(L"MWBlueprintUtils", cls(L"Class /Script/MechWarrior.MWBlueprintUtils"), keys);
        ProbeFunctions(L"MWSquadCommandOrder", cls(L"Class /Script/MechWarrior.MWSquadCommandOrder"), {});
        ProbeFunctions(L"MWSquad", cls(L"Class /Script/MechWarrior.MWSquad"), {});
        ProbeFunctions(L"MWAIController", cls(L"Class /Script/MechWarrior.MWAIController"), keys);
        ProbeFunctions(L"SquadOrderBase_C",
                       cls(L"BlueprintGeneratedClass /Game/Modes/MissionDirectorSystem/MissionComponents/SquadOrders/SquadOrderBase.SquadOrderBase_C"), {});
        ProbeFunctions(L"Dpad_LanceOrderDispatcher_C",
                       cls(L"BlueprintGeneratedClass /Game/UI/Mech/DPad/DpadActionLogic/LanceCommands/Dpad_LanceOrderDispatcher.Dpad_LanceOrderDispatcher_C"), {});
        ProbeFunctions(L"Dpad_BaseLanceOrder_C",
                       cls(L"BlueprintGeneratedClass /Game/UI/Mech/DPad/DpadActionLogic/LanceCommands/Dpad_BaseLanceOrder.Dpad_BaseLanceOrder_C"), {});

        // The controller's own squad/lance/order members.
        if (pc && pc->get_class())
        {
            ProbeLines = 0;
            for (auto *st = static_cast<API::UStruct *>(pc->get_class()); st; st = st->get_super_struct())
                for (auto *f = st->get_child_properties(); f; f = f->get_next())
                {
                    const std::wstring n = f->get_fname()->to_string();
                    if (n.find(L"Squad") == std::wstring::npos && n.find(L"Lance") == std::wstring::npos &&
                        n.find(L"Order") == std::wstring::npos)
                        continue;
                    auto *p = reinterpret_cast<API::FProperty *>(f);
                    ProbeDump(L"pc." + n, p, reinterpret_cast<uint8_t *>(pc) + p->get_offset(), 2);
                }
        }
        ProbeLanceMates();

        // SWITCH MECH: the game's own way of leaving your mech to the AI and taking a lancemate's
        // (PlayerControllerSwitchMech, driven by the D-pad's Dpad_SwitchMechLogic). What it can
        // do and what it holds, so a TacOps "take command of this mech" can be built on it.
        if (API::UObject *sw = ObjProp(pc, L"PlayerControllerSwitchMech"))
        {
            ProbeFunctions(L"PlayerControllerSwitchMech_C", sw->get_class(), {});
            ProbeLines = 0;
            for (auto *f = sw->get_class()->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                ProbeDump(L"switch." + f->get_fname()->to_string(), p, reinterpret_cast<uint8_t *>(sw) + p->get_offset(), 2);
            }
        }
        else
            LogInfo("lance probe: no PlayerControllerSwitchMech component on %ls", ClassName(pc).c_str());
        LogInfo("lance probe: ---- give a few lance orders now (move, attack, form on me, hold fire) ----");
        return true;
    }

    // =====================================================================================
    // TACOPS (build vanillahud-21): the lance, published; HUDDisplay's orders, posted.
    //
    // Everything here is the game's own order path, as the lance order probe recorded it for
    // the F1-F4 keys (vanillahud-19/20):
    //   MWPlayerController::PostSquadOrderClass(OrderClass, MWSquadCommandOrderInfo {
    //       Instigator      = the player controller,
    //       OrderRecipients = order slots (1..4 - "all" sent 1,2,3,4; one lancemate its own),
    //       Priority = 1, bPersistent = false,
    //       OrderContext    = { IssuerPawn = the player's mech, TargetActor, TargetLocation } })
    // Who stands in which slot is the game's answer too: MWSquad::GetMembersForOrder({slot}, pc).
    //
    // Heap rule: both calls take a TArray<int32>. Ours is allocated ONCE with the engine's
    // allocator and kept for the session; GetMembersForOrder's frame is never zeroed, so the
    // array it returns is freed by the engine when the next call assigns over it.
    // =====================================================================================
    // The slot -> unit map is asked for afresh on every use (10 Hz at most): a unit pointer kept
    // across ticks could outlive the unit, and reading a destroyed actor is a crash.
    uint32_t        TacDone      = 0;

    bool            TacDoneInit  = false;

    ULONGLONG       TacMapMs     = 0;

    API::UObject   *TacUnit[VanillaExportBlock::kTacMaxMembers + 1] = {}; // [slot] -> pawn

    bool            TacIsPlayer[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int32_t        *TacRecip     = nullptr; // FMalloc'd, 8 ints: PostSquadOrderClass's (read only there)

    int32_t        *TacRecipGmf  = nullptr; // and GetMembersForOrder's, which may replace it (out param)

    std::vector<uint8_t> TacGmfBuf;          // GetMembersForOrder's frame, never zeroed

    API::UFunction *TacGmfFn     = nullptr;

    int32_t         TacGmfRecip = -1, TacGmfInst = -1, TacGmfRet = -1;

    bool            TacLayoutOk  = false, TacLayoutTried = false;

    API::UFunction *TacPostFn    = nullptr;

    int32_t TacPClass = -1, TacPInfo = -1;                 // PostSquadOrderClass params

    int32_t TacIInst = -1, TacIRecip = -1, TacIPrio = -1, TacIPers = -1, TacICtx = -1; // in the info

    int32_t TacCIssuer = -1, TacCTarget = -1, TacCLoc = -1; // in the context

    API::UClass *TacOrderCls[4] = {};

    bool         TacMapLogged = false;

    float    TacLastMove[VanillaExportBlock::kTacMaxMembers + 1][3] = {};

    uint32_t TacLastSeq[VanillaExportBlock::kTacMaxMembers + 1] = {};

    struct TArr
    {
        void   *Data;
        int32_t Num, Max;
    };

    static int32_t StructMember(API::UStruct *st, const wchar_t *name)
    {
        if (!st)
            return -1;
        auto *p = st->find_property(name);
        return p ? p->get_offset() : -1;
    }

    bool TacResolve(API::UObject *pc)
    {
        if (TacLayoutTried)
            return TacLayoutOk;
        TacLayoutTried = true;
        API::UObject *squad = ObjProp(pc, L"Squad");
        TacGmfFn  = squad ? FindFn(squad, L"GetMembersForOrder") : nullptr;
        TacPostFn = FindFn(pc, L"PostSquadOrderClass");
        if (TacGmfFn)
        {
            TacGmfRecip = ParamOffset(TacGmfFn, L"OrderRecipients");
            TacGmfInst  = ParamOffset(TacGmfFn, L"OrderInstigator");
            TacGmfRet   = ParamOffset(TacGmfFn, L"ReturnValue");
            // The members must come back as OBJECTS (controllers or pawns) - anything else would
            // be read as pointers.
            for (auto *f = TacGmfFn->get_child_properties(); f; f = f->get_next())
                if (f->get_fname()->to_string() == L"ReturnValue")
                {
                    auto *in = PropType(reinterpret_cast<API::FProperty *>(f)) == L"ArrayProperty"
                                   ? reinterpret_cast<API::FArrayProperty *>(f)->get_inner() : nullptr;
                    const std::wstring it = in ? PropType(in) : L"?";
                    TacLog("tacops: GetMembersForOrder returns array<%ls>", it.c_str());
                    if (it != L"ObjectProperty")
                        TacGmfRet = -1;
                }
            // Kept across worlds: dropping it would orphan the array its last call returned.
            const size_t need = (size_t)TacGmfFn->get_properties_size() + 64;
            if (TacGmfBuf.size() < need)
                TacGmfBuf.assign(need, 0);
        }
        API::UStruct *info = nullptr, *ctx = nullptr;
        if (TacPostFn)
        {
            TacPClass = ParamOffset(TacPostFn, L"Class");
            TacPInfo  = ParamOffset(TacPostFn, L"SquadCommandOrderInfo");
            for (auto *f = TacPostFn->get_child_properties(); f; f = f->get_next())
                if (f->get_fname()->to_string() == L"SquadCommandOrderInfo" && PropType(reinterpret_cast<API::FProperty *>(f)) == L"StructProperty")
                    info = reinterpret_cast<API::FStructProperty *>(f)->get_struct();
        }
        if (info)
        {
            TacIInst  = StructMember(info, L"Instigator");
            TacIRecip = StructMember(info, L"OrderRecipients");
            TacIPrio  = StructMember(info, L"Priority");
            TacIPers  = StructMember(info, L"bPersistent");
            TacICtx   = StructMember(info, L"OrderContext");
            if (auto *cp = info->find_property(L"OrderContext"))
                if (PropType(cp) == L"StructProperty")
                    ctx = reinterpret_cast<API::FStructProperty *>(cp)->get_struct();
        }
        TacCIssuer = StructMember(ctx, L"IssuerPawn");
        TacCTarget = StructMember(ctx, L"TargetActor");
        TacCLoc    = StructMember(ctx, L"TargetLocation");
        static const wchar_t *kCls[4] = {
            L"BlueprintGeneratedClass /Game/Modes/MissionDirectorSystem/MissionComponents/SquadOrders/GoToLocationOrder.GoToLocationOrder_C",
            L"BlueprintGeneratedClass /Game/Modes/MissionDirectorSystem/MissionComponents/SquadOrders/AttackOrder.AttackOrder_C",
            L"BlueprintGeneratedClass /Game/Modes/MissionDirectorSystem/MissionComponents/SquadOrders/FollowOrder.FollowOrder_C",
            L"BlueprintGeneratedClass /Game/Modes/MissionDirectorSystem/MissionComponents/SquadOrders/ToggleHoldFireWeaponsFreeOrder.ToggleHoldFireWeaponsFreeOrder_C"};
        for (int i = 0; i < 4; ++i)
            TacOrderCls[i] = API::get()->find_uobject<API::UClass>(kCls[i]);
        if (API::FMalloc *fm = API::FMalloc::get())
        {
            if (!TacRecip)
                TacRecip = static_cast<int32_t *>(fm->malloc(8 * sizeof(int32_t), 4));
            if (!TacRecipGmf)
                TacRecipGmf = static_cast<int32_t *>(fm->malloc(8 * sizeof(int32_t), 4));
        }
        TacLayoutOk = TacGmfFn && TacGmfRecip >= 0 && TacGmfInst >= 0 && TacGmfRet >= 0 && TacPostFn && TacPClass >= 0 &&
                      TacPInfo >= 0 && TacIInst >= 0 && TacIRecip >= 0 && TacIPrio >= 0 && TacICtx >= 0 && TacCIssuer >= 0 &&
                      TacCTarget >= 0 && TacCLoc >= 0 && TacRecip && TacRecipGmf && TacOrderCls[0] && TacOrderCls[1] && TacOrderCls[2] &&
                      TacOrderCls[3];
        TacLog("tacops: layout %s - members fn %p (recip %d inst %d ret %d), post fn %p (class %d info %d), info inst %d "
                "recip %d prio %d pers %d ctx %d, ctx issuer %d target %d loc %d, order classes %d%d%d%d",
                TacLayoutOk ? "OK" : "INCOMPLETE", (void *)TacGmfFn, TacGmfRecip, TacGmfInst, TacGmfRet, (void *)TacPostFn,
                TacPClass, TacPInfo, TacIInst, TacIRecip, TacIPrio, TacIPers, TacICtx, TacCIssuer, TacCTarget, TacCLoc,
                TacOrderCls[0] != nullptr, TacOrderCls[1] != nullptr, TacOrderCls[2] != nullptr, TacOrderCls[3] != nullptr);
        return TacLayoutOk;
    }

    // The units the game would send an order for `slot` to.
    int TacMembersFor(API::UObject *squad, API::UObject *pc, int slot, API::UObject **out, int max)
    {
        uint8_t *f = TacGmfBuf.data();
        auto *ra = reinterpret_cast<TArr *>(f + TacGmfRecip);
        // Our array unless the engine has since replaced it (it is an out-parameter by reference).
        if (!ra->Data || ra->Max < 1)
        {
            ra->Data = TacRecipGmf;
            ra->Max  = 8;
        }
        static_cast<int32_t *>(ra->Data)[0] = slot;
        ra->Num = 1;
        std::memcpy(f + TacGmfInst, &pc, sizeof(void *));
        squad->process_event(TacGmfFn, f);
        const auto *ret = reinterpret_cast<const TArr *>(f + TacGmfRet);
        int n = 0;
        if (ret->Data && ret->Num > 0 && ret->Num < 64)
            for (int i = 0; i < ret->Num && n < max; ++i)
                if (auto *o = static_cast<API::UObject *const *>(ret->Data)[i])
                    out[n++] = o;
        return n;
    }

    // THE LANCE MAP (vanillahud-102). Two numbers per lancemate:
    //   - its LANCE NUMBER (our slot 2 / 3 / 4, what the HUD, VoiceCommand and the F-keys use): the
    //     game's own lance position, EMWSquadMemberId Member1..Member4 (the order the game's command
    //     menu lists them in - the mech bay's lance order), Member2 -> 2, Member3 -> 3, Member4 -> 4;
    //     the player is 1. Where that cannot be read: the order it joined the squad in.
    //   - its ORDER NUMBER (what GetMembersForOrder / PostSquadOrderClass address): the squad hands
    //     these out from a counter (NextMemberId) that instant action does not reset - mission 1
    //     gave 2, 3, 4, the next mission 8 and beyond. Found by asking the game who answers to
    //     order numbers 1 .. kTacScanMax (every 2 s, or at once when a known one stops answering).
    // Pawn pointers are still asked for afresh on every refresh: only the order numbers are kept.
    static constexpr int kTacScanMax = 64;

    int32_t   TacRecipOf[VanillaExportBlock::kTacMaxMembers + 1] = {}; // [lance number] -> order number, 0 none

    int       TacPosOf[VanillaExportBlock::kTacMaxMembers + 1] = {};   // [lance number] -> game lance position, -1 unknown

    // lancecontrol-3: STICKY LANCE NUMBERS. Once an order number has been given a lance number it
    // keeps it for the rest of the mission - the map used to be rebuilt in join order on every
    // refresh, so when lancemate 2 died, 3 became 2 and 4 became 3 (and took over those numbers'
    // fire settings, voice callsigns and per-slot state). A dead lancemate's number now stays empty
    // ("down" in the log); a newcomer gets a number never used this mission, else one whose holder
    // is gone. Cleared by ForgetWorldLance.
    std::vector<std::pair<int32_t, int>> TacSticky; // (order number, lance number)

    ULONGLONG TacPanelWaitMs = 0;          // lancecontrol-18: when this world's map first waited for the lance panel

    bool      TacPanelFallbackLogged = false;

    ULONGLONG TacScanMs = 0;

    std::wstring TacMapSig;

    struct PosEntry
    {
        API::UObject *Pawn;
        int           Pos; // 0..3 = Member1..Member4, -1 not found
        std::wstring  How;
    };

    std::vector<PosEntry> TacPosCache;

    // A no-argument function returning one byte (so calling it with an empty frame is safe).
    static bool OnlyByteReturn(API::UFunction *fn)
    {
        int n = 0;
        bool ok = false;
        for (auto *f = fn ? fn->get_child_properties() : nullptr; f; f = f->get_next())
        {
            auto *p = reinterpret_cast<API::FProperty *>(f);
            ++n;
            const std::wstring t = PropType(p);
            ok = p->get_fname()->to_string() == L"ReturnValue" && (t == L"ByteProperty" || t == L"EnumProperty");
        }
        return n == 1 && ok;
    }

    // THE GAME'S LANCE PANEL (lancecontrol-17). The V-comms lance panel is built from
    // MWSquad.GetSquadDisplayInfo() -> MWSquadDisplayInfo.SquadMemberDisplayInfo[] - one
    // MWSquadMemberDisplayInfo per pilot: SquadMemberId (1 = the player, 2..4), SquadMember (its
    // controller), PilotName (FText), PlayerPawn (the mech it is in NOW), SquadMemberDead. Read at
    // most twice a second. Lance numbers = SquadMemberId; names go into the status feed (v6).
    struct DiMember
    {
        API::UObject *Pawn = nullptr;
        API::UObject *Ctl  = nullptr;
        int           Id   = 0;
        bool          Dead = false;
        std::string   Name; // UTF-8
    };

    std::vector<DiMember> Di;

    ULONGLONG DiMs = 0;

    bool DiLogged = false;

    // KismetTextLibrary::Conv_TextToString, as LidarExport does it (the conversion the game's UI uses).
    API::UFunction      *DiTtsFn = nullptr;
    API::UObject        *DiTtsCdo = nullptr;
    int32_t              DiTtsIn = -1, DiTtsRet = -1;
    bool                 DiTtsTried = false;
    std::vector<uint8_t> DiTtsBuf;

    std::string DiText(const void *at)
    {
        if (!at)
            return "";
        if (!DiTtsTried)
        {
            DiTtsTried = true;
            if (auto *cls = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.KismetTextLibrary"))
            {
                DiTtsCdo = cls->get_class_default_object();
                DiTtsFn  = cls->find_function(L"Conv_TextToString");
            }
            if (DiTtsFn)
            {
                DiTtsIn  = ParamOffset(DiTtsFn, L"InText");
                DiTtsRet = ParamOffset(DiTtsFn, L"ReturnValue");
                if (DiTtsIn >= 0 && DiTtsRet >= 0)
                    DiTtsBuf.assign((size_t)DiTtsFn->get_properties_size() + 64, 0);
            }
            if (!DiTtsCdo || DiTtsBuf.empty())
            {
                DiTtsFn = nullptr;
                LogWarn("lance panel: Conv_TextToString not usable - no pilot names");
            }
        }
        if (!DiTtsFn)
            return "";
        const void *inner = nullptr;
        std::memcpy(&inner, at, sizeof(inner));
        if (!inner)
            return ""; // never initialised
        // Not zeroed between calls: the engine frees the previous return string when it assigns the new one.
        std::memcpy(DiTtsBuf.data() + DiTtsIn, at, 24);
        DiTtsCdo->process_event(DiTtsFn, DiTtsBuf.data());
        const wchar_t *w = *reinterpret_cast<wchar_t **>(DiTtsBuf.data() + DiTtsRet);
        const int32_t  n = *reinterpret_cast<int32_t *>(DiTtsBuf.data() + DiTtsRet + 8);
        if (!w || n <= 1 || n > 200)
            return "";
        return Utf8Of(w, n - 1);
    }

    // lancecontrol-18: wide -> UTF-8 that never throws (WideToNarrow's std::wstring_convert throws on a broken
    // surrogate pair - an exception in the game thread is a crash). Bad characters become U+FFFD.
    static std::string Utf8Of(const wchar_t *w, int n)
    {
        std::string u;
        if (!w || n <= 0)
            return u;
        const int k = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
        if (k <= 0)
            return u;
        u.resize((size_t)k);
        WideCharToMultiByte(CP_UTF8, 0, w, n, &u[0], k, nullptr, nullptr);
        return u;
    }

    // Copied into a fixed field without cutting a character in half (and always terminated).
    static void Utf8Fit(char *dst, size_t cap, const std::string &src)
    {
        if (!dst || !cap)
            return;
        size_t n = std::min(src.size(), cap - 1);
        if (n < src.size())
            while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
                --n; // back to the start of the character that did not fit
        std::memcpy(dst, src.data(), n);
        dst[n] = 0;
    }

    void DiRead(API::UObject *pc, ULONGLONG now)
    {
        if (now - DiMs < 500)
            return;
        DiMs = now;
        Di.clear();
        API::UObject *squad = pc ? ObjProp(pc, L"Squad") : nullptr;
        API::UFunction *gf = squad ? FindFn(squad, L"GetSquadDisplayInfo") : nullptr;
        if (!gf)
            return;
        bool objRet = false;
        for (auto *f = gf->get_child_properties(); f; f = f->get_next())
            if (f->get_fname()->to_string() == L"ReturnValue")
                objRet = PropType(reinterpret_cast<API::FProperty *>(f)) == L"ObjectProperty";
        API::UObject *info = nullptr;
        if (!objRet || !CallRet(squad, L"GetSquadDisplayInfo", {}, &info, sizeof(info)) || !info)
            return;
        auto *ap = FindProp(info->get_class(), L"SquadMemberDisplayInfo");
        if (!ap || PropType(ap) != L"ArrayProperty")
            return;
        const TArr *arr = reinterpret_cast<const TArr *>(reinterpret_cast<uint8_t *>(info) + ap->get_offset());
        if (arr->Num < 0 || arr->Num > 16 || (arr->Num && !arr->Data))
            return;
        for (int i = 0; i < arr->Num; ++i)
        {
            API::UObject *m = reinterpret_cast<API::UObject **>(arr->Data)[i];
            if (!m || !m->get_class())
                continue;
            DiMember d;
            d.Pawn = ObjProp(m, L"PlayerPawn");
            d.Ctl  = ObjProp(m, L"SquadMember");
            if (auto *ip = FindProp(m->get_class(), L"SquadMemberId"))
            {
                const std::wstring t = PropType(ip);
                if (t == L"ByteProperty" || t == L"EnumProperty")
                    d.Id = *(reinterpret_cast<uint8_t *>(m) + ip->get_offset());
            }
            if (auto *dp = FindProp(m->get_class(), L"SquadMemberDead"))
                if (PropType(dp) == L"BoolProperty")
                    d.Dead = reinterpret_cast<API::FBoolProperty *>(dp)->get_value_from_propbase(reinterpret_cast<uint8_t *>(m) + dp->get_offset());
            if (auto *np = FindProp(m->get_class(), L"PilotName"))
                if (PropType(np) == L"TextProperty")
                    d.Name = DiText(reinterpret_cast<uint8_t *>(m) + np->get_offset());
            Di.push_back(d);
        }
        if (!DiLogged && !Di.empty())
        {
            DiLogged = true;
            for (auto &d : Di)
                LogInfo("lance panel: member id %d = %s (%ls)%s", d.Id, d.Name.empty() ? "(no name)" : d.Name.c_str(),
                        ObjLabel(d.Pawn).c_str(), d.Dead ? " - dead" : "");
        }
    }

    // The game's lance position of a lancemate (EMWSquadMemberId, 0 = Member1), from wherever its
    // squad member info lives: a GetSquadMemberId(), a SquadMemberId field, or one inside a
    // SquadMemberDisplayInfo struct - on the pawn, its controller, the controller's PlayerState or
    // a component of either. Where it was found is logged once per lancemate.
    int LancePosOf(API::UObject *pawn, std::wstring &how)
    {
        // lancecontrol-17: the game's own lance panel first (not cached: a takeover moves pilots).
        for (auto &d : Di)
            if (d.Pawn == pawn && d.Id >= 1 && d.Id <= VanillaExportBlock::kTacMaxMembers)
            {
                how = L"the V-comms panel's SquadMemberId";
                return d.Id - 1;
            }
        for (auto &e : TacPosCache)
            if (e.Pawn == pawn)
            {
                how = e.How;
                return e.Pos;
            }
        int pos = -1;
        how = L"not found";
        auto tryObj = [&](API::UObject *o, const std::wstring &what) -> bool {
            if (!o || !o->get_class())
                return false;
            if (API::UFunction *fn = FindFn(o, L"GetSquadMemberId"))
                if (OnlyByteReturn(fn))
                {
                    uint8_t v = 0xFF;
                    if (CallRet(o, L"GetSquadMemberId", {}, &v, 1) && v < 8)
                    {
                        pos = v;
                        how = what + L".GetSquadMemberId()";
                        return true;
                    }
                }
            if (auto *p = FindProp(o->get_class(), L"SquadMemberId"))
            {
                const std::wstring t = PropType(p);
                if (t == L"ByteProperty" || t == L"EnumProperty")
                {
                    const uint8_t v = *(reinterpret_cast<uint8_t *>(o) + p->get_offset());
                    if (v < 8)
                    {
                        pos = v;
                        how = what + L".SquadMemberId";
                        return true;
                    }
                }
            }
            if (auto *p = FindProp(o->get_class(), L"SquadMemberDisplayInfo"))
                if (PropType(p) == L"StructProperty")
                    if (auto *st = reinterpret_cast<API::FStructProperty *>(p)->get_struct())
                        if (auto *q = StructProp(st, L"SquadMemberId"))
                        {
                            const std::wstring t = PropType(q);
                            if (t == L"ByteProperty" || t == L"EnumProperty")
                            {
                                const uint8_t v = *(reinterpret_cast<uint8_t *>(o) + p->get_offset() + q->get_offset());
                                if (v < 8)
                                {
                                    pos = v;
                                    how = what + L".SquadMemberDisplayInfo.SquadMemberId";
                                    return true;
                                }
                            }
                        }
            return false;
        };
        API::UObject *ctl = ObjProp(pawn, L"Controller");
        API::UObject *ps = ctl ? ObjProp(ctl, L"PlayerState") : nullptr;
        bool got = tryObj(pawn, L"mech") || tryObj(ctl, L"controller") || tryObj(ps, L"player state");
        if (!got)
        {
            static API::UClass *acc = nullptr;
            if (!acc)
                acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
            for (API::UObject *owner : {pawn, ctl})
            {
                std::vector<API::UObject *> comps;
                if (got || !owner || !acc || !ComponentsOn(owner, acc, comps))
                    continue;
                for (auto *c : comps)
                    if (!got)
                        got = tryObj(c, (owner == pawn ? L"mech " : L"controller ") + ClassName(c));
            }
        }
        TacPosCache.push_back({pawn, pos, how});
        return pos;
    }

    void TacRefreshMap(API::UObject *pc, ULONGLONG now)
    {
        // Not before the squad-order layout is resolved: TacMembersFor writes into its frame, which
        // does not exist until then (the vanillahud-65 crash: the AI probe asked first, right after
        // injecting mid-mission). Same readiness test as the order queue uses.
        if (!pc || !API::get()->get_local_pawn(0) || !TacResolve(pc))
            return;
        DiRead(pc, now); // lancecontrol-17: the game's lance panel - lance numbers by its member ids
        API::UObject *squad = ObjProp(pc, L"Squad");
        if (!squad)
            return;
        TacMapMs = now;
        API::UObject *mine = API::get()->get_local_pawn(0);
        // 1. Who answers to which order number. The known ones every time (a pawn pointer is never
        //    kept); all of 1 .. kTacScanMax every 2 s, or at once when a known one falls silent.
        std::vector<std::pair<int, API::UObject *>> found; // (order number, pawn)
        auto ask = [&](int r) -> API::UObject * {
            API::UObject *m[4];
            const int n = TacMembersFor(squad, pc, r, m, 4);
            if (n != 1) // nobody, or a group
                return nullptr;
            API::UObject *p = ObjProp(m[0], L"Pawn"); // a controller answers with its pawn
            API::UObject *pawn = p ? p : m[0];
            return (pawn == mine || m[0] == pc) ? nullptr : pawn;
        };
        bool scan = !TacScanMs || now - TacScanMs >= 2000;
        if (!scan)
            for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers && !scan; ++s)
                if (TacRecipOf[s])
                {
                    if (API::UObject *p = ask(TacRecipOf[s]))
                        found.push_back({TacRecipOf[s], p});
                    else
                        scan = true;
                }
        if (scan)
        {
            TacScanMs = now;
            found.clear();
            for (int r = 1; r <= kTacScanMax; ++r)
                if (API::UObject *p = ask(r))
                {
                    bool dup = false;
                    for (auto &f : found)
                        dup |= f.second == p;
                    if (!dup)
                        found.push_back({r, p});
                }
            std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.first < b.first; });
            // Forget lance positions of pawns no longer in the squad (an address can be reused).
            TacPosCache.erase(std::remove_if(TacPosCache.begin(), TacPosCache.end(),
                                             [&](const PosEntry &e) {
                                                 for (auto &f : found)
                                                     if (f.second == e.Pawn)
                                                         return false;
                                                 return true;
                                             }),
                              TacPosCache.end());
        }
        // 2. Lance numbers: one already given this mission first (sticky, lancecontrol-3), then the
        //    game's lance position, then join order into what is left.
        for (int s = 0; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            TacUnit[s] = nullptr;
            TacIsPlayer[s] = false;
            TacRecipOf[s] = 0;
            TacPosOf[s] = -1;
        }
        auto stickyOf = [&](int32_t r) -> int {
            for (auto &e : TacSticky)
                if (e.first == r)
                    return e.second;
            return 0;
        };
        // A lance number is free for a newcomer if nobody holds it now and it was never given out
        // this mission (pass 0), or if its holder is gone (pass 1 - only when no fresh one is left).
        auto reserved = [&](int s) -> bool {
            for (auto &e : TacSticky)
                if (e.second == s)
                    return true;
            return false;
        };
        std::vector<bool> placed(found.size(), false);
        for (size_t i = 0; i < found.size(); ++i)
        {
            const int s = stickyOf(found[i].first);
            if (s >= 2 && s <= VanillaExportBlock::kTacMaxMembers && !TacUnit[s])
            {
                TacUnit[s] = found[i].second;
                TacRecipOf[s] = found[i].first;
                placed[i] = true;
            }
        }
        // lancecontrol-18: a lancemate with no number yet waits for the game's lance panel (its member ids are
        // the numbers) - up to 5 s into the world, then join order as before, so a panel that cannot be read
        // never leaves the lance without numbers.
        if (!TacPanelWaitMs)
            TacPanelWaitMs = now;
        const bool waitPanel = Di.empty() && now - TacPanelWaitMs < 5000;
        if (Di.empty() && !waitPanel && !TacPanelFallbackLogged)
        {
            TacPanelFallbackLogged = true;
            TacLog("tacops: the game's lance panel could not be read in 5 s - lance numbers by join order");
        }
        std::vector<size_t> rest;
        std::vector<std::wstring> hows(found.size());
        std::vector<int> poss(found.size());
        // Member1 is expected to be the player (lancemates Member2..4 -> 2..4); if a lancemate
        // itself is Member1, the positions count lancemates only (Member1..3 -> 2..4).
        int offset = 1;
        for (size_t i = 0; i < found.size(); ++i)
            if ((poss[i] = LancePosOf(found[i].second, hows[i])) == 0)
                offset = 2;
        for (size_t i = 0; i < found.size(); ++i)
        {
            if (placed[i])
            {
                for (int s = 2; s <= VanillaExportBlock::kTacMaxMembers; ++s)
                    if (TacUnit[s] == found[i].second)
                        TacPosOf[s] = poss[i];
                continue;
            }
            if (waitPanel)
                continue; // no number until the panel is read (or 5 s have passed)
            const int pos = poss[i];
            const int s = pos + offset; // Member2 -> 2 ...
            if (pos >= 0 && s >= 2 && s <= VanillaExportBlock::kTacMaxMembers && !TacUnit[s] && !reserved(s))
            {
                TacUnit[s] = found[i].second;
                TacRecipOf[s] = found[i].first;
                TacPosOf[s] = pos;
                placed[i] = true;
            }
            else
                rest.push_back(i);
        }
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i : rest)
            {
                if (placed[i])
                    continue;
                for (int s = 2; s <= VanillaExportBlock::kTacMaxMembers; ++s)
                    if (!TacUnit[s] && (pass == 1 || !reserved(s)))
                    {
                        TacUnit[s] = found[i].second;
                        TacRecipOf[s] = found[i].first;
                        placed[i] = true;
                        break;
                    }
            }
        // Remember every lance number given (a reused one moves to its new order number).
        for (int s = 2; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            if (TacUnit[s] && TacRecipOf[s] && stickyOf(TacRecipOf[s]) != s)
            {
                TacSticky.erase(std::remove_if(TacSticky.begin(), TacSticky.end(),
                                               [&](const std::pair<int32_t, int> &e) { return e.second == s || e.first == TacRecipOf[s]; }),
                                TacSticky.end());
                TacSticky.push_back({TacRecipOf[s], s});
            }
        // THE PLAYER: GetMembersForOrder leaves out whoever issues the order. Lance number 1 (the
        // first free one if a lancemate somehow holds 1); only ever sent the autopilot, never a
        // squad order.
        if (mine)
            for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
                if (!TacUnit[s])
                {
                    TacUnit[s] = mine;
                    TacIsPlayer[s] = true;
                    break;
                }
        // 3. Logged whenever it changes.
        std::wstring sig;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            if (TacUnit[s])
            {
                wchar_t b[64];
                _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d:%p:%d ", s, (void *)TacUnit[s], TacRecipOf[s]);
                sig += b;
            }
        if (sig != TacMapSig)
        {
            TacMapSig = sig;
            std::wstring t;
            for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            {
                if (!TacUnit[s])
                {
                    // A lance number given out this mission whose lancemate no longer answers.
                    for (auto &e : TacSticky)
                        if (e.second == s)
                        {
                            t += L" | " + std::to_wstring(s) + L" = down (game order number " + std::to_wstring(e.first) + L", number kept)";
                            break;
                        }
                    continue;
                }
                if (TacIsPlayer[s])
                {
                    t += L" | " + std::to_wstring(s) + L" = you";
                    continue;
                }
                std::wstring how;
                const int pos = LancePosOf(TacUnit[s], how);
                wchar_t b[320];
                _snwprintf_s(b, _countof(b), _TRUNCATE, L" | %d = %ls (game order number %d, lance position %ls%ls%ls)", s, ObjTag(TacUnit[s]).c_str(), TacRecipOf[s],
                           pos >= 0 ? (L"Member" + std::to_wstring(pos + 1)).c_str() : L"unknown", pos >= 0 ? L" from " : L" - join order used, ",
                           how.c_str());
                t += b;
            }
            TacLog("tacops: lance map%ls", t.empty() ? L" - empty" : t.c_str());
        }
    }

    ULONGLONG TacPubMs = 0;

    void TacPublish(API::UObject *pc, ULONGLONG now)
    {
        // Published while the TacOps page asks - and, since vanillahud-93, 4 times a second anyway,
        // for any other reader (the voice module): the lance is always there to be read.
        const uint64_t want = Block->TacWantMs;
        const bool wanted = want && now >= want && now - want <= kVhudTargetingStaleMs;
        if (!wanted && (now - TacPubMs < 250 || !API::get()->get_local_pawn(0)))
            return;
        if (!TacResolve(pc))
            return;
        TacPubMs = now;
        TacRefreshMap(pc, now);

        VanillaExportBlock::TacMember mem[VanillaExportBlock::kTacMaxMembers] = {};
        uint32_t count = 0;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            API::UObject *u = TacUnit[s];
            if (!u)
                continue;
            auto &e    = mem[count++];
            e.Valid    = 1;
            e.IsPlayer = TacIsPlayer[s] ? 1 : 0;
            e.Slot     = (uint8_t)s;
            e.ActorLo  = (uint32_t)(reinterpret_cast<uintptr_t>(u) & 0xFFFFFFFFu);
            e.ActorHi  = (uint32_t)((uint64_t)reinterpret_cast<uintptr_t>(u) >> 32);
            e.FireDisc   = FsDisc[s];
            e.FireMask   = FsMask[s];
            e.FireHeat   = FsHeat[s];
            e.FirePlugin = (uint8_t)((IcMaskSet[s] ? 1 : 0) | (IcMaskSet[s] && IcMaskOld[s] == 1 ? 2 : 0));
            float rot[3] = {};
            if (!CallRet(u, L"K2_GetActorLocation", {}, e.Loc, sizeof(e.Loc)))
                e.Dead = 1;
            CallRet(u, L"K2_GetActorRotation", {}, rot, sizeof(rot));
            e.Yaw = rot[1];
            std::memcpy(e.LastMove, TacLastMove[s], sizeof(e.LastMove));
            e.LastMoveSeq = TacLastSeq[s];
        }
        Block->TacSeq++; // odd: writing
        MemoryBarrier();
        std::memcpy(Block->TacMembers, mem, sizeof(mem));
        Block->TacCount = count;
        MemoryBarrier();
        Block->TacSeq++;
    }

    // The unit nearest `loc` (cm) within 40 m that is not in the lance.
    API::UObject *TacTargetNear(API::UObject *pc, const float *loc)
    {
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> all;
        if (!PawnClass || !ActorsOf(pc, PawnClass, all))
            return nullptr;
        API::UObject *best = nullptr;
        float bestD = 4000.0f * 4000.0f;
        for (auto *a : all)
        {
            bool ours = a == API::get()->get_local_pawn(0);
            for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers && !ours; ++s)
                ours = TacUnit[s] == a;
            if (ours)
                continue;
            float p[3] = {};
            if (!CallRet(a, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            const float dx = p[0] - loc[0], dy = p[1] - loc[1], dz = p[2] - loc[2];
            const float d = dx * dx + dy * dy + dz * dz * 0.25f;
            if (d < bestD)
            {
                bestD = d;
                best  = a;
            }
        }
        return best;
    }

    // lancecontrol-61 (user, 2026-10-16): THE EXACT UNIT for TCO_Attack and TCO_CalledShot too - Loc[2] == kTacLocActor,
    // Loc[0] / Loc[1] the unit's address (as TCO_LockTarget, lancecontrol-13), found among the live pawns by comparison
    // only (never dereferenced). VoiceCommand's named attacks / called shots send it: the unit nearest the named mech's
    // point was a dropship turret beside it (long test run 3, row 240). nullptr when that unit is gone.
    API::UObject *TacActorExact(API::UObject *pc, const float *loc)
    {
        uint32_t lo = 0, hi = 0;
        std::memcpy(&lo, &loc[0], 4);
        std::memcpy(&hi, &loc[1], 4);
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> all;
        if (!PawnClass || !ActorsOf(pc, PawnClass, all))
            return nullptr;
        for (auto *a : all)
        {
            uint32_t l2 = 0, h2 = 0;
            LsSplit(a, l2, h2);
            if (l2 == lo && h2 == hi)
                return a;
        }
        return nullptr;
    }
    // The order's unit: the exact one when it came as an address, else the nearest to the point.
    API::UObject *TacTargetOf(API::UObject *pc, const float *loc)
    {
        return loc[2] == VanillaExportBlock::kTacLocActor ? TacActorExact(pc, loc) : TacTargetNear(pc, loc);
    }

    // THE MOVE POINT, ON THE NAVMESH. The lancemate's move job (Ally_GoToLocationJob) projects
    // the target onto navigation within a small box and refuses the order - "invalid location" -
    // when nothing is there. The F-keys aim at a surface the navmesh lies on; HUDDisplay's click
    // lands on its own heightfield, which is traced from above and can sit on a rock or a canopy,
    // metres off the walkable surface. So the point is projected here first, with the engine's
    // own NavigationSystemV1::K2_ProjectPointToNavigation and a tall search box (the height is
    // the uncertain part), widening once if the spot itself is not walkable.
    API::UObject   *NavLib = nullptr;

    API::UFunction *NavProjFn = nullptr;

    int32_t NavWorld = -1, NavPoint = -1, NavOut = -1, NavData = -1, NavFilter = -1, NavExtent = -1, NavRet = -1;

    bool    NavTried = false;

    bool TacOnNavMesh(API::UObject *pc, float *loc, bool quiet = false)
    {
        if (!NavTried)
        {
            NavTried = true;
            if (auto *cls = API::get()->find_uobject<API::UClass>(L"Class /Script/NavigationSystem.NavigationSystemV1"))
            {
                NavLib    = cls->get_class_default_object();
                NavProjFn = cls->find_function(L"K2_ProjectPointToNavigation");
            }
            if (NavProjFn)
            {
                NavWorld  = ParamOffset(NavProjFn, L"WorldContextObject");
                NavPoint  = ParamOffset(NavProjFn, L"Point");
                NavOut    = ParamOffset(NavProjFn, L"ProjectedLocation");
                NavData   = ParamOffset(NavProjFn, L"NavData");
                NavFilter = ParamOffset(NavProjFn, L"FilterClass");
                NavExtent = ParamOffset(NavProjFn, L"QueryExtent");
                NavRet    = ParamOffset(NavProjFn, L"ReturnValue");
            }
            TacLog("tacops: navmesh projection %s (world %d point %d out %d extent %d ret %d)",
                    (NavLib && NavProjFn && NavWorld >= 0 && NavPoint >= 0 && NavOut >= 0 && NavExtent >= 0 && NavRet >= 0)
                        ? "ready" : "UNAVAILABLE - moves sent as clicked",
                    NavWorld, NavPoint, NavOut, NavExtent, NavRet);
        }
        if (!NavLib || !NavProjFn || NavWorld < 0 || NavPoint < 0 || NavOut < 0 || NavExtent < 0 || NavRet < 0)
            return true; // cannot check: send it as it is, as before
        const int32_t frame = NavProjFn->get_properties_size();
        if (frame <= 0 || frame > 512)
            return true;
        // NEAR THE CLICK'S HEIGHT FIRST. vanillahud-22 searched 80 m up and down from the start,
        // and the log showed what that does on a plateau: a point 18,691 cm up snapped to 13,366 -
        // the valley floor 53 m below the edge - and the lancemate walked the long way down to it.
        // So: this height, then wider at this height, and only then the tall box.
        static const float kExtents[4][3] = {{300.0f, 300.0f, 600.0f},
                                             {1500.0f, 1500.0f, 600.0f},
                                             {3000.0f, 3000.0f, 1500.0f},
                                             {300.0f, 300.0f, 8000.0f}};
        for (const auto &ext : kExtents)
        {
            uint8_t f[512 + 16] = {};
            std::memcpy(f + NavWorld, &pc, sizeof(void *));
            std::memcpy(f + NavPoint, loc, 12);
            std::memcpy(f + NavExtent, ext, 12);
            NavLib->process_event(NavProjFn, f);
            if (f[NavRet])
            {
                float out[3];
                std::memcpy(out, f + NavOut, 12);
                if (!quiet)
                    TacLog("tacops: move point (%.0f, %.0f, %.0f) -> navmesh (%.0f, %.0f, %.0f)", loc[0], loc[1], loc[2], out[0],
                        out[1], out[2]);
                std::memcpy(loc, out, 12);
                return true;
            }
        }
        return false;
    }

    // =====================================================================================
    // AUTOPILOT: the player's own mech walking a navmesh path.
    //
    // MWMech::MoveTo walks in a STRAIGHT LINE to its point (the first test: no pathfinding at
    // all). So the route comes from the engine's own pathfinder,
    // NavigationSystemV1::FindPathToLocationSynchronously, with the mech as the pathfinding
    // context, and MoveTo is aimed at a point 40 m further along that path (see ApCarrotFor).
    // No progress for 5 s re-plans from where it stands; three re-plans in a row give up and stop.
    // =====================================================================================
    struct ApPoint { float P[3]; };

    std::vector<ApPoint> ApPath;

    bool          ApOn = false;

    API::UObject *ApPawn = nullptr;

    float         ApGoal[3] = {};

    float         ApKph = 64.0f;

    ULONGLONG     ApProgMs = 0;

    float         ApBestD = 0.0f;

    int           ApReplans = 0;

    int           ApSlot = 0; // the order slot the autopilot's move came in on (its echo, TacLastSeq)

    API::UFunction *NavPathFn = nullptr;

    int32_t NpWorld = -1, NpStart = -1, NpEnd = -1, NpCtx = -1, NpRet = -1, NpPoints = -1;

    bool    NavPathTried = false;

    static constexpr float kApArriveCm  = 1200.0f; // the last one counts as reached here

    bool ApFindPath(API::UObject *pc, API::UObject *pawn, const float *from, const float *to)
    {
        if (!NavPathTried)
        {
            NavPathTried = true;
            if (!NavTried)
            {
                float probe[3] = {to[0], to[1], to[2]};
                TacOnNavMesh(pc, probe); // resolves NavLib (normally done already by the move)
            }
            if (NavLib)
                if (auto *cls = NavLib->get_class())
                    NavPathFn = cls->find_function(L"FindPathToLocationSynchronously");
            if (NavPathFn)
            {
                NpWorld = ParamOffset(NavPathFn, L"WorldContextObject");
                NpStart = ParamOffset(NavPathFn, L"PathStart");
                NpEnd   = ParamOffset(NavPathFn, L"PathEnd");
                NpCtx   = ParamOffset(NavPathFn, L"PathfindingContext");
                NpRet   = ParamOffset(NavPathFn, L"ReturnValue");
            }
            TacLog("tacops: autopilot pathfinder %s (world %d start %d end %d ctx %d ret %d)",
                    (NavPathFn && NpWorld >= 0 && NpStart >= 0 && NpEnd >= 0 && NpRet >= 0) ? "ready" : "UNAVAILABLE",
                    NpWorld, NpStart, NpEnd, NpCtx, NpRet);
        }
        ApPath.clear();
        if (!NavPathFn || NpWorld < 0 || NpStart < 0 || NpEnd < 0 || NpRet < 0)
            return false;
        const int32_t frame = NavPathFn->get_properties_size();
        if (frame <= 0 || frame > 256)
            return false;
        uint8_t f[256 + 16] = {};
        std::memcpy(f + NpWorld, &pc, sizeof(void *));
        std::memcpy(f + NpStart, from, 12);
        std::memcpy(f + NpEnd, to, 12);
        if (NpCtx >= 0)
            std::memcpy(f + NpCtx, &pawn, sizeof(void *));
        NavLib->process_event(NavPathFn, f);
        API::UObject *path = *reinterpret_cast<API::UObject **>(f + NpRet);
        if (!path || !path->get_class())
            return false;
        if (NpPoints < 0)
            if (auto *pp = FindProp(path->get_class(), L"PathPoints"))
                NpPoints = pp->get_offset();
        if (NpPoints < 0)
            return false;
        const auto *arr = reinterpret_cast<const TArr *>(reinterpret_cast<uint8_t *>(path) + NpPoints);
        if (!arr->Data || arr->Num < 2 || arr->Num > 512)
            return false;
        const float *pts = static_cast<const float *>(arr->Data);
        // Point 0 is where the mech stands. Corners closer than 4 m to the last kept one add
        // nothing but a stutter.
        for (int32_t i = 1; i < arr->Num; ++i)
        {
            const float *q = pts + 3 * i;
            if (!ApPath.empty() && i + 1 < arr->Num)
            {
                const float *l = ApPath.back().P;
                if ((q[0] - l[0]) * (q[0] - l[0]) + (q[1] - l[1]) * (q[1] - l[1]) < 400.0f * 400.0f)
                    continue;
            }
            ApPath.push_back({{q[0], q[1], q[2]}});
        }
        // lancecontrol-38 (game test 2026-10-12): a PARTIAL path that ends where the mech already stands (the goal
        // not reachable on the navmesh from here) is no route - the autopilot "arrived" at once and the mech never
        // moved. Treated as no path: the caller goes straight at the goal.
        if (!ApPath.empty())
        {
            const float *e = ApPath.back().P;
            const float eh = std::sqrt((e[0] - from[0]) * (e[0] - from[0]) + (e[1] - from[1]) * (e[1] - from[1]));
            const float gh = std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]));
            if (eh < kApArriveCm && gh > kApArriveCm)
            {
                TacLog("tacops: autopilot - the navmesh path ends where you stand (%.0f m of %.0f m) - ignored",
                       eh * 0.01f, gh * 0.01f);
                ApPath.clear();
                return false;
            }
        }
        return !ApPath.empty();
    }

    // STEERING: a point 40 m further along the route ("carrot"), not the next corner. The path
    // is a polyline from where the mech stood when it was planned through every corner; each tick
    // the mech is placed on it and MoveTo is aimed 40 m ahead of that. The aim point slides round
    // a corner before the mech gets there, so the turn starts early and is spread out instead of
    // being a pivot at the corner. The last 40 m aim at the destination itself.
    static constexpr float kApLookCm = 4000.0f;

    float  ApOrigin[3] = {};

    size_t ApSeg = 0;          // the polyline segment the mech is on

    float  ApCarrot[3] = {};

    bool   ApCarrotSent = false;

    const float *ApVertex(size_t i) const { return i == 0 ? ApOrigin : ApPath[i - 1].P; }

    size_t ApVertexCount() const { return ApPath.size() + 1; }

    // Where along the route the mech is, then the point kApLookCm further on.
    void ApCarrotFor(const float *here, float *out)
    {
        const size_t nv = ApVertexCount();
        // Nearest point on the next few segments - never backwards.
        float bestD = 1.0e30f, bestT = 0.0f;
        size_t bestS = ApSeg;
        for (size_t sgi = ApSeg; sgi + 1 < nv && sgi < ApSeg + 4; ++sgi)
        {
            const float *a = ApVertex(sgi), *b = ApVertex(sgi + 1);
            const float ex = b[0] - a[0], ey = b[1] - a[1];
            const float len2 = ex * ex + ey * ey;
            float t = len2 > 1.0f ? ((here[0] - a[0]) * ex + (here[1] - a[1]) * ey) / len2 : 1.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float px = a[0] + ex * t - here[0], py = a[1] + ey * t - here[1];
            const float d = px * px + py * py;
            if (d < bestD)
            {
                bestD = d;
                bestS = sgi;
                bestT = t;
            }
        }
        ApSeg = bestS;
        // Walk kApLookCm along the polyline from there.
        float left = kApLookCm;
        size_t sgi = bestS;
        float t = bestT;
        while (sgi + 1 < nv)
        {
            const float *a = ApVertex(sgi), *b = ApVertex(sgi + 1);
            const float ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2];
            const float len = std::sqrt(ex * ex + ey * ey);
            const float rem = len * (1.0f - t);
            if (rem >= left && len > 1.0f)
            {
                const float tt = t + left / len;
                out[0] = a[0] + ex * tt; out[1] = a[1] + ey * tt; out[2] = a[2] + ez * tt;
                return;
            }
            left -= rem;
            ++sgi;
            t = 0.0f;
        }
        std::memcpy(out, ApVertex(nv - 1), 12); // within 40 m of the end: the destination
    }

    bool ApAim(API::UObject *pawn, const float *here)
    {
        float c[3];
        ApCarrotFor(here, c);
        // Only re-aimed when the point has actually moved, so MoveTo is not restarted every tick.
        if (ApCarrotSent && std::fabs(c[0] - ApCarrot[0]) + std::fabs(c[1] - ApCarrot[1]) < 300.0f)
            return true;
        std::memcpy(ApCarrot, c, sizeof(c));
        ApCarrotSent = Call(pawn, L"MoveTo", {{L"Location", c, sizeof(c)}, {L"MaxSpeedKph", &ApKph, 4}});
        return ApCarrotSent;
    }

    // What is left of the route from the mech, cm - the progress measure.
    float ApRemaining(const float *here) const
    {
        const size_t nv = ApVertexCount();
        const float *b = ApVertex(ApSeg + 1 < nv ? ApSeg + 1 : nv - 1);
        float r = std::sqrt((b[0] - here[0]) * (b[0] - here[0]) + (b[1] - here[1]) * (b[1] - here[1]));
        for (size_t i = ApSeg + 1; i + 1 < nv; ++i)
        {
            const float *p = ApVertex(i), *q = ApVertex(i + 1);
            r += std::sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]));
        }
        return r;
    }

    bool ApPlan(API::UObject *pc, API::UObject *pawn)
    {
        float here[3] = {};
        CallRet(pawn, L"K2_GetActorLocation", {}, here, sizeof(here));
        if (!ApFindPath(pc, pawn, here, ApGoal))
        {
            // No path (off the mesh, or no pathfinder): straight at it, as before.
            ApPath.assign(1, ApPoint{{ApGoal[0], ApGoal[1], ApGoal[2]}});
            TacLog("tacops: autopilot - no navmesh path, going straight");
        }
        std::memcpy(ApOrigin, here, sizeof(ApOrigin));
        ApSeg = 0;
        ApCarrotSent = false;
        ApBestD  = 1.0e30f;
        ApProgMs = GetTickCount64();
        return ApAim(pawn, here);
    }

    bool ApStart(API::UObject *pc, API::UObject *pawn, const float *goal)
    {
        std::memcpy(ApGoal, goal, sizeof(ApGoal));
        float maxCmS = 0.0f;
        CallRet(pawn, L"GetMechMaxSpeed", {}, &maxCmS, sizeof(maxCmS));
        ApKph = maxCmS * 0.036f;
        if (ApKph <= 1.0f)
            ApKph = 64.0f;
        ApPawn    = pawn;
        ApReplans = 0;
        ApOn      = ApPlan(pc, pawn);
        PwSettle(PWG_Legs);
        TacLog("tacops: autopilot -> (%.0f, %.0f, %.0f) at %.0f kph, %zu corner(s)%s", goal[0], goal[1], goal[2], ApKph,
                ApPath.size(), ApOn ? "" : " - MoveTo not available");
        return ApOn;
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

    static constexpr float kPwMoved = 0.05f; // axis units (-1..1): past a stick's jitter

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

    // The input in `group` the pilot has moved since the baseline, or nullptr. `from` / `to` say
    // how far. Only this group's baselines (and the unwatched ones') are touched: the torso check
    // runs every frame, and when it also re-took the LEGS baselines the autopilot's own check
    // never saw the throttle move while a torso order was running.
    const PwField *PwMoved(API::UObject *pawn, int group, float &from, float &to)
    {
        API::UObject *ih = PwHandler(pawn);
        if (!ih)
            return nullptr;
        const ULONGLONG now = GetTickCount64();
        const bool follow = (OvInputOn.load() && !kOvGameInput) || now < PwSettleUntil[group];
        const PwField *hit = nullptr;
        for (auto &f : PwFields)
        {
            const float v = *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(ih) + f.Prop->get_offset());
            if (std::fabs(v - f.Base) <= kPwMoved)
                continue;
            if (f.Group == group && !follow)
            {
                if (!hit)
                {
                    hit = &f;
                    from = f.Base;
                    to = v;
                }
                continue;
            }
            if (f.Group != group && f.Group != PWG_Other)
                continue; // another check's baseline
            if (f.Group == PWG_Other && !f.Noted)
            {
                f.Noted = true;
                TacLog("tacops: pilot watch - %ls moved %.2f -> %.2f (not watched)", f.Name.c_str(), f.Base, v);
            }
            f.Base = v;
        }
        return hit;
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

    // zero = false when the pilot has taken over: their throttle is live, and zeroing the
    // handler's throttle would throw away the lever they have just moved.
    void ApStop(API::UObject *pawn, const char *why, bool zero = true)
    {
        if (ApOn && pawn)
        {
            Call(pawn, L"CancelMoveTo", {});
            if (zero)
                ApZeroThrottle(pawn);
        }
        if (ApOn)
        {
            TacLog("tacops: autopilot off (%s)", why);
            // The echo goes: HUDDisplay reads that as the route being dropped and clears it.
            if (ApSlot >= 1 && ApSlot <= VanillaExportBlock::kTacMaxMembers)
                TacLastSeq[ApSlot] = 0;
        }
        ApOn = false;
        ApPath.clear();
    }

    void ApTick(API::UObject *pc)
    {
        if (!ApOn)
            return;
        API::UObject *pawn = API::get()->get_local_pawn(0);
        if (!pawn || pawn != ApPawn)
        {
            ApOn = false; // a different mech (or none): what we were steering is gone
            ApPath.clear();
            if (ApSlot >= 1 && ApSlot <= VanillaExportBlock::kTacMaxMembers)
                TacLastSeq[ApSlot] = 0; // and HUDDisplay drops the route
            return;
        }
        {
            float from = 0.0f, to = 0.0f;
            if (const PwField *f = PwMoved(pawn, PWG_Legs, from, to))
            {
                char why[128];
                snprintf(why, sizeof(why), "pilot input: %ls %.2f -> %.2f", f->Name.c_str(), from, to);
                ApStop(pawn, why, false);
                return;
            }
        }
        float here[3] = {};
        if (!CallRet(pawn, L"K2_GetActorLocation", {}, here, sizeof(here)) || ApPath.empty())
            return;
        const float *g = ApPath.back().P;
        const float dEnd = std::sqrt((g[0] - here[0]) * (g[0] - here[0]) + (g[1] - here[1]) * (g[1] - here[1]));
        if (dEnd < kApArriveCm)
        {
            // STOPPED HERE, not left to MoveTo: it keeps trying to put the mech's origin exactly
            // on the point, and a mech's turning circle is wider than that - it circled the spot.
            Call(pawn, L"CancelMoveTo", {});
            ApZeroThrottle(pawn);
            TacLog("tacops: autopilot arrived");
            ApOn = false;
            ApPath.clear();
            return;
        }
        ApAim(pawn, here);
        const float d = ApRemaining(here);
        const ULONGLONG now = GetTickCount64();
        if (d < ApBestD - 200.0f)
        {
            ApBestD  = d;
            ApProgMs = now;
            ApReplans = 0;
        }
        else if (now - ApProgMs > 5000)
        {
            if (++ApReplans > 3)
            {
                ApStop(pawn, "stuck - no progress after three re-plans");
                return;
            }
            TacLog("tacops: autopilot - no progress for 5 s, re-planning (%d)", ApReplans);
            ApPlan(pc, pawn);
        }
    }

    // =====================================================================================
    // LOCK PROBE (lancecontrol-11). "I have a visual on the Longbow - want me to patch it
    // through?" - "do it": the player's lock (TargetTracking.CurrentTarget, the one MWVR's head aim
    // and LidarExport read) set to a unit a lancemate sees, even one the player cannot see.
    // Unknown yet: which function the game uses to take a target, whether it accepts one from
    // outside, and whether it keeps it. So this build:
    //   - (lancecontrol-11..14 also dumped the tracker's functions and hooked the blueprint target ones;
    //     removed in lancecontrol-15 once the lock worked - see HANDOFF for what they showed);
    //   - logs every change of the lock with the player's own record of that unit (line of sight,
    //     sensor range = on radar, relayed from the team);
    //   - on TCO_LockTarget tries the tracker's own setter (the first of a list of likely names
    //     that exists and takes one object), else writes CurrentTarget directly, and checks 0.5 s
    //     and 3 s later whether the lock held.
    // Everything is logged with "lock probe:" (log.txt).
    // =====================================================================================
    API::UObject *LockWant = nullptr;     // compared only - never called on
    ULONGLONG     LockWantMs = 0;
    int           LockChecks = 0;
    const char   *LockHow = "";
    // lancecontrol-37: the units the lance detects now (a lancemate's line of sight, a lancemate's or your sensors) -
    // refreshed with the status feed's unit list, twice a second. Compared only, never called on.
    std::vector<API::UObject *> LockSeenNow;
    ULONGLONG LockSeenMs = 0;

    // The player's lock changed (from LsTick): who to, and what the player's tracker knows of it.
    void LockProbeChanged(API::UObject *me, API::UObject *lk)
    {
        if (!lk)
        {
            LogInfo("lock probe: your lock -> none");
            return;
        }
        float a[3] = {}, b[3] = {};
        float m = -1.0f;
        if (CallRet(me, L"K2_GetActorLocation", {}, a, sizeof(a)) && CallRet(lk, L"K2_GetActorLocation", {}, b, sizeof(b)))
            m = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1])) * 0.01f;
        const AiRel r = AiRelOf(me, lk);
        LogInfo("lock probe: your lock -> %ls, %.0f m%s - your tracker: known %d, line of sight %d, on sensors %d, relayed %d, "
                "selected %d", ClassName(lk).c_str(), m, lk == LockWant ? " (ours)" : "", r.Known, r.Los, r.Sensor, r.Relay, r.Sel);
    }

    // 0.5 s and 3 s after a patch-through: did the lock hold? (from LsTick)
    void LockProbeCheck(API::UObject *lk, ULONGLONG now)
    {
        if (!LockWant || now - LockWantMs < (LockChecks == 0 ? 500u : 3000u))
            return;
        LogInfo("lock probe: +%.1f s after patch-through (%s): the lock %s", (now - LockWantMs) / 1000.0f, LockHow,
                lk == LockWant ? "HELD" : (lk ? "was REPLACED" : "is gone"));
        if (++LockChecks >= 2)
            LockWant = nullptr;
    }

    // lancecontrol-13 (probe 2). Probe 1 (2026-10-06 21:53/21:54) showed OnLookActorChanged(unit) does NOT
    // move the lock - it only records what the reticle is on. Manual locks land on any unit the player's
    // tracker has on its sensors (also relayed, no line of sight). So this writes the lock itself: the
    // tracker's CurrentTarget, the bIsSelectedTarget flag of its ActorRelations record (that unit on, the
    // rest off), and then calls the player's mech's own blueprint handlers of the tracker's target-changed
    // event (BndEvt ... TargetChangedSignature), the way a broadcast would, so the HUD follows.
    // The unit comes as its address (Loc[2] == kTacLocActor), found among the live pawns by comparison only.
    // lancecontrol-39: every multicast delegate on `holder` whose name has "Target" in it is BROADCAST by hand - its
    // invocation list (TArray<FScriptDelegate {FWeakObjectPtr object (index, serial), FName function}>) walked, and
    // each bound function called on its object with the new target as the first object parameter and the old one
    // as the second. A bound function with any other kind of parameter is only logged. Returns the calls made.
    int BroadcastTargetDelegates(API::UObject *holder, const wchar_t *what, API::UObject *nu, API::UObject *old)
    {
        if (!holder || !holder->get_class() || !API::FUObjectArray::get())
            return 0;
        struct ScriptDelegate
        {
            int32_t Index, Serial;
            API::FName Function;
        };
        struct DelegateArray
        {
            ScriptDelegate *Data;
            int32_t Num, Max;
        };
        int calls = 0;
        for (API::UStruct *st = holder->get_class(); st; st = st->get_super_struct())
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring ty = PropType(p);
                if (ty != L"MulticastInlineDelegateProperty" && ty != L"MulticastDelegateProperty")
                    continue;
                const std::wstring nm = f->get_fname()->to_string();
                if (nm.find(L"Target") == std::wstring::npos)
                    continue;
                // lancecontrol-40: only the change / acquire events. Test 4 (2026-10-12) showed the plain "Target"
                // match also fired TargetTracking.OnTargetDestroyed (Betty's HandleTargetDestroyed) with the new target.
                if (nm.find(L"Changed") == std::wstring::npos && nm.find(L"Acquired") == std::wstring::npos)
                    continue;
                if (nm.find(L"Destroyed") != std::wstring::npos || nm.find(L"Lost") != std::wstring::npos)
                    continue;
                auto *arr = reinterpret_cast<DelegateArray *>(reinterpret_cast<uint8_t *>(holder) + p->get_offset());
                const int n = (arr->Data && arr->Num > 0 && arr->Num <= 32) ? arr->Num : 0;
                LogInfo("lock probe: delegate %ls.%ls - %d binding(s)", what, nm.c_str(), n);
                for (int i = 0; i < n; ++i)
                {
                    API::UObject *o = API::FUObjectArray::get()->get_object(arr->Data[i].Index);
                    const std::wstring fnName = arr->Data[i].Function.to_string();
                    API::UFunction *fn = (o && o->get_class()) ? o->get_class()->find_function(fnName) : nullptr;
                    if (!fn)
                    {
                        LogInfo("lock probe: delegate   %ls -> (unresolved)", fnName.c_str());
                        continue;
                    }
                    const int32_t frame = fn->get_properties_size();
                    if (frame <= 0 || frame > 4096)
                        continue;
                    std::vector<uint8_t> fb((size_t)frame + 16, 0);
                    int objs = 0;
                    bool ok = true;
                    for (auto *q = fn->get_child_properties(); q; q = q->get_next())
                    {
                        auto *pp = reinterpret_cast<API::FProperty *>(q);
                        if (!pp->is_param() || pp->is_return_param())
                            continue;
                        if (PropType(pp) == L"ObjectProperty" && objs < 2)
                        {
                            API::UObject *v = objs++ == 0 ? nu : old;
                            std::memcpy(fb.data() + pp->get_offset(), &v, sizeof(void *));
                        }
                        else
                            ok = false;
                    }
                    LogInfo("lock probe: delegate   %ls::%ls (%d object param(s))%s", ClassName(o).c_str(), fnName.c_str(), objs,
                            ok ? " - called" : " - other parameters, not called");
                    if (!ok)
                        continue;
                    o->process_event(fn, fb.data());
                    ++calls;
                }
            }
        return calls;
    }

    bool LockOrder(API::UObject *pc, API::UObject *pawn, const VanillaExportBlock::TacCmd &c)
    {
        API::UObject *u = nullptr;
        if (c.Loc[2] == VanillaExportBlock::kTacLocActor)
        {
            uint32_t lo = 0, hi = 0;
            std::memcpy(&lo, &c.Loc[0], 4);
            std::memcpy(&hi, &c.Loc[1], 4);
            if (!PawnClass)
                PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
            std::vector<API::UObject *> all;
            if (PawnClass && ActorsOf(pc, PawnClass, all))
                for (auto *a : all)
                {
                    uint32_t l2 = 0, h2 = 0;
                    LsSplit(a, l2, h2);
                    if (l2 == lo && h2 == hi)
                        u = a;
                }
        }
        else
            u = TacTargetNear(pc, c.Loc);
        if (!u || !IsLive(u) || IsDeadUnit(u))
        {
            LogInfo("lock probe: patch-through - that unit is gone");
            return false;
        }
        API::UObject *tt = ObjProp(pawn, L"TargetTracking");
        if (!tt)
        {
            LogInfo("lock probe: patch-through - you have no TargetTracking component");
            return false;
        }
        const AiRel r = AiRelOf(pawn, u);
        // lancecontrol-37 (user): only a unit someone detects - your own tracker (line of sight, sensors, relayed),
        // else the status feed's list (a lancemate's line of sight or sensors). An undetected unit is refused.
        {
            bool detected = r.Los > 0 || r.Sensor > 0 || r.Relay > 0;
            if (!detected && GetTickCount64() - LockSeenMs < 3000)
                for (auto *x : LockSeenNow)
                    detected = detected || x == u;
            if (!detected)
            {
                LogInfo("lock probe: patch-through refused - nobody in the lance detects %ls", ClassName(u).c_str());
                return false;
            }
        }
        LogInfo("lock probe: patch-through on %ls (from slot %u, %s) - your tracker: known %d, line of sight %d, on sensors %d, "
                "relayed %d, selected %d", ClassName(u).c_str(), c.Slot,
                c.Loc[2] == VanillaExportBlock::kTacLocActor ? "exact unit" : "nearest to a point", r.Known, r.Los, r.Sensor, r.Relay, r.Sel);
        // 1. CurrentTarget
        API::FProperty *cp = FindProp(tt->get_class(), L"CurrentTarget");
        if (!cp || PropType(cp) != L"ObjectProperty")
        {
            LogInfo("lock probe: patch-through - no CurrentTarget object property - nothing done");
            return false;
        }
        API::UObject *was = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(tt) + cp->get_offset());
        *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(tt) + cp->get_offset()) = u;
        // 2. the selected flag in the tracker's records
        int sel = 0, unsel = 0;
        {
            uint8_t *data = nullptr;
            int32_t num = 0, stride = 0;
            API::UScriptStruct *es = nullptr;
            std::wstring an;
            if (FpFindArray(tt->get_class(), reinterpret_cast<uint8_t *>(tt), L"ActorRelationInfo", nullptr, data, num, es, stride, an))
            {
                auto *ta = StructProp(es, L"TargetActor");
                auto *sp = StructProp(es, L"bIsSelectedTarget");
                if (ta && sp && PropType(sp) == L"BoolProperty")
                    for (int32_t i = 0; data && i < num && i < 256; ++i)
                    {
                        uint8_t *el = data + (size_t)i * stride;
                        auto *bp = reinterpret_cast<API::FBoolProperty *>(sp);
                        const bool me = *reinterpret_cast<API::UObject **>(el + ta->get_offset()) == u;
                        if (bp->get_value_from_object(el) != me)
                        {
                            bp->set_value_in_object(el, me);
                            (me ? sel : unsel)++;
                        }
                    }
            }
        }
        // 3. the mech's blueprint handlers of the target-changed event
        int calls = 0;
        auto *fnClass = API::UFunction::static_class();
        for (API::UStruct *st = pawn->get_class(); st; st = st->get_super_struct())
            for (auto *f = st->get_children(); f; f = f->get_next())
            {
                if (f->get_class() != fnClass)
                    continue;
                auto *fn = reinterpret_cast<API::UFunction *>(f);
                const std::wstring nm = fn->get_fname()->to_string();
                if (nm.find(L"TargetChangedSignature") == std::wstring::npos)
                    continue;
                if (CallWithObject(pawn, fn, u))
                    ++calls;
            }
        // 4. lancecontrol-39 (user 2026-10-12: the lock landed but V-TGT, the target panel, did not show it): the
        // panels listen to target-changed MULTICAST DELEGATES, which a raw CurrentTarget write never fires.
        calls += BroadcastTargetDelegates(pawn, L"mech", u, was);
        calls += BroadcastTargetDelegates(tt, L"TargetTracking", u, was);
        calls += BroadcastTargetDelegates(ObjProp(pawn, L"MechViewComponent"), L"MechViewComponent", u, was);
        LockHow = "wrote CurrentTarget";
        LockWant = u;
        LockWantMs = GetTickCount64();
        LockChecks = 0;
        LogInfo("lock probe: patch-through - CurrentTarget written (was %ls), selected flag set on %d / cleared on %d record(s), "
                "%d blueprint target-changed handler(s) called", was ? ClassName(was).c_str() : L"none", sel, unsel, calls);
        return true;
    }

    bool TacPost(API::UObject *pc, const VanillaExportBlock::TacCmd &c)
    {
        if (!FlInternal && c.Slot >= 1 && c.Slot <= VanillaExportBlock::kTacMaxMembers && FlOn[c.Slot])
            FlEnd(c.Slot, "replaced by a new order"); // lancecontrol-75: any other order ends a flank
        if (c.Order == VanillaExportBlock::TCO_Flank && c.Slot >= 1 && c.Slot <= VanillaExportBlock::kTacMaxMembers)
            return FlankOrder(pc, c); // lancecontrol-75: FLANK / PINCER
        if (c.Order == VanillaExportBlock::TCO_CalledShot && c.Slot >= 1 && c.Slot <= VanillaExportBlock::kTacMaxMembers)
            return CsOrder(pc, c); // lancecontrol-32: CALLED SHOTS
        if (c.Order > VanillaExportBlock::TCO_LockTarget || c.Slot < 1 || c.Slot > VanillaExportBlock::kTacMaxMembers)
            return false;
        API::UObject *pawn = API::get()->get_local_pawn(0);
        if (!pawn)
            return false;

        // THE PLAYER'S OWN MECH. MWMech::MoveTo(Location, MaxSpeedKph) and CancelMoveTo are the
        // mech's own native walk-there, found by the movement probe - no AI possession, no
        // simulated stick, and the pilot keeps the torso, the guns and the HUD.
        if (c.Order >= VanillaExportBlock::TCO_TorsoAngle && c.Order <= VanillaExportBlock::TCO_TorsoRelease)
            return TorsoOrder(pc, pawn, c);
        if (c.Order == VanillaExportBlock::TCO_FireSettings)
            return FsOrder(c); // not a squad order: nothing the lancemate is doing is replaced
        if (c.Order == VanillaExportBlock::TCO_LockTarget)
            return LockOrder(pc, pawn, c); // the PLAYER's lock (lancecontrol-11 probe)
        if (c.Order == VanillaExportBlock::TCO_SelfStop)
        {
            ApStop(pawn, "cancelled"); // clears the autopilot's own slot's echo, whatever c.Slot says
            return true;
        }
        if (c.Order == VanillaExportBlock::TCO_SelfMove)
        {
            float loc[3] = {c.Loc[0], c.Loc[1], c.Loc[2]};
            if (!TacOnNavMesh(pc, loc))
            {
                TacLog("tacops: autopilot - no walkable ground near (%.0f, %.0f, %.0f)", loc[0], loc[1], loc[2]);
                return false;
            }
            // A new leg replaces the old one: stopped properly first (CancelMoveTo, throttle), so a
            // failed start cannot leave the mech walking to the old point with nothing tracking it.
            if (ApOn)
                ApStop(pawn, "replaced by a new move");
            ApSlot = c.Slot;
            if (!ApStart(pc, pawn, loc))
                return false;
            std::memcpy(TacLastMove[c.Slot], loc, sizeof(loc));
            TacLastSeq[c.Slot] = c.Seq;
            return true;
        }
        // lc54 (user 2026-10-13): ADVANCE (FALL BACK TO ME) RETIRED - nothing sends it any more (its order
        // number stays reserved in VanillaHudShared.h); an old sender's order is dropped, not run.
        if (c.Order == VanillaExportBlock::TCO_AdvanceFallback)
        {
            TacLog("tacops: advance / fall back is retired - order for slot %u ignored", c.Slot);
            return true;
        }
        // INDIRECT FIRE FROM COVER: a plain move to the point, watched by IcTick.
        if (c.Order == VanillaExportBlock::TCO_IndirectCover)
        {
            API::UObject *unit = TacUnit[c.Slot];
            if (!unit)
                return false;
            if (IcOn[c.Slot])
                IcEnd(c.Slot, "replaced by a new cover order");
            VanillaExportBlock::TacCmd m = c;
            m.Order = VanillaExportBlock::TCO_Move;
            IcInternal = true;
            const bool ok = TacPost(pc, m);
            IcInternal = false;
            if (!ok)
                return false;
            const int s = c.Slot;
            IcOn[s] = true;
            IcPhase[s] = IC_Move;
            IcStartMs[s] = GetTickCount64();
            IcSeenCount[s] = 0;
            IcSpotter[s] = false;
            IcUnseenMs[s] = 0;
            IcTT[s] = nullptr;
            IcDrop[s].clear();
            IcTTSwitches[s] = 0;
            IcMaskOn(s, unit); // (looks up the fire-result call itself; the game keeps firing if it cannot)
            TacLog("tacops: hold (indirect fire), slot %d - moving to (%.0f, %.0f, %.0f); hides within 200 m when any enemy sees it", s,
                    TacLastMove[s][0], TacLastMove[s][1], TacLastMove[s][2]);
            return true;
        }
        if (!IcInternal && IcOn[c.Slot])
            IcEnd(c.Slot, "replaced by a new order");
        // Any new order for this lancemate ends a fire-on-the-move; this one may start it again.
        const bool fireMove = c.Order == VanillaExportBlock::TCO_MoveFire;
        const bool fallback = c.Order == VanillaExportBlock::TCO_Fallback;
        const bool hold = c.Order == VanillaExportBlock::TCO_HoldHere;
        const bool myTarget = c.Order == VanillaExportBlock::TCO_AttackMyTarget;
        const uint8_t order = (fireMove || fallback || hold) ? (uint8_t)VanillaExportBlock::TCO_Move
                                                             : (myTarget ? (uint8_t)VanillaExportBlock::TCO_Attack : c.Order);
        // The lancemate itself - `pawn` above is the PLAYER's (the order's issuer).
        if (FomOn[c.Slot])
            FomRelease(TacUnit[c.Slot], c.Slot, "new order");
        FomOn[c.Slot] = false;
        if (HoldOn[c.Slot])
            HoldEnd(c.Slot, "new order", true);
        API::UObject *target = nullptr;
        if (myTarget)
        {
            // The player's own lock (vanillahud-93, voice "attack my target").
            API::UObject *tt = ObjProp(pawn, L"TargetTracking");
            target = tt ? ObjProp(tt, L"CurrentTarget") : nullptr;
            if (!target || IsDeadUnit(target) || !IsLive(target))
            {
                TacLog("tacops: attack my target for slot %u - you have no target", c.Slot);
                return false;
            }
        }
        else if (order == VanillaExportBlock::TCO_Attack)
        {
            const bool exact = c.Loc[2] == VanillaExportBlock::kTacLocActor; // lancecontrol-61: the named unit itself
            target = TacTargetOf(pc, c.Loc);
            if (!target || (exact && (IsDeadUnit(target) || !IsLive(target))))
            {
                if (exact)
                    TacLog("tacops: attack for slot %u - the named unit is gone", c.Slot);
                else
                    TacLog("tacops: attack for slot %u - nothing within 40 m of (%.0f, %.0f, %.0f)", c.Slot, c.Loc[0], c.Loc[1], c.Loc[2]);
                return false;
            }
        }
        float loc[3] = {c.Loc[0], c.Loc[1], c.Loc[2]};
        if (order != VanillaExportBlock::TCO_Move)
        {
            if (!CallRet(target ? target : pawn, L"K2_GetActorLocation", {}, loc, sizeof(loc)) && loc[2] == VanillaExportBlock::kTacLocActor)
                return false; // lancecontrol-61: never post a unit's address as a point
        }
        else if (!TacOnNavMesh(pc, loc))
        {
            // lancecontrol-33: no walkable ground there (off the map, a cliff, water) - the farthest walkable point on
            // the line from the lancemate toward it, in 1/13 steps, instead of no move at all.
            float from[3] = {};
            bool ok = false;
            if (TacUnit[c.Slot] && CallRet(TacUnit[c.Slot], L"K2_GetActorLocation", {}, from, sizeof(from)))
                for (int k = 1; k <= 12 && !ok; ++k)
                {
                    const float f = 1.0f - k / 13.0f;
                    float p[3] = {from[0] + (c.Loc[0] - from[0]) * f, from[1] + (c.Loc[1] - from[1]) * f, c.Loc[2]};
                    if (TacOnNavMesh(pc, p, true))
                    {
                        std::memcpy(loc, p, sizeof(loc));
                        ok = true;
                        TacLog("tacops: MOVE for slot %u - no walkable ground at the point; the farthest walkable on the way, %.0f %% of the "
                               "distance (%.0f, %.0f, %.0f)", c.Slot, f * 100.0f, loc[0], loc[1], loc[2]);
                    }
                }
            if (!ok)
            {
                TacLog("tacops: MOVE for slot %u - no walkable ground near (%.0f, %.0f, %.0f) or on the way, not sent", c.Slot, c.Loc[0],
                        c.Loc[1], c.Loc[2]);
                return false;
            }
        }
        const int32_t frame = TacPostFn->get_properties_size();
        if (frame <= 0 || frame > 4096)
            return false;
        // Its own frame: CallRet (above) owns CallBuf.
        std::vector<uint8_t> fb((size_t)frame + 16, 0);
        uint8_t *f = fb.data();
        API::UClass *cls = TacOrderCls[order];
        std::memcpy(f + TacPClass, &cls, sizeof(void *));
        uint8_t *info = f + TacPInfo;
        std::memcpy(info + TacIInst, &pc, sizeof(void *));
        // The lancemate's own order number in the game's squad (vanillahud-102), not the lance number.
        TacRecip[0] = (c.Slot <= VanillaExportBlock::kTacMaxMembers && TacRecipOf[c.Slot]) ? TacRecipOf[c.Slot] : c.Slot;
        TArr ra{TacRecip, 1, 8};
        std::memcpy(info + TacIRecip, &ra, sizeof(ra));
        info[TacIPrio] = 1;
        if (TacIPers >= 0)
            info[TacIPers] = 0;
        uint8_t *ctx = info + TacICtx;
        std::memcpy(ctx + TacCIssuer, &pawn, sizeof(void *));
        std::memcpy(ctx + TacCTarget, &target, sizeof(void *));
        std::memcpy(ctx + TacCLoc, loc, sizeof(loc));
        pc->process_event(TacPostFn, f);
        if (!FlInternal) // lancecontrol-75: a flank's own legs and its closing attack are not announced
            LsEmit(LSE_OrderReceived, c.Slot, target, (float)c.Order);
        // An attack order's unit: insane allies' timer does not switch away from it (vanillahud-87).
        AtkTgt[c.Slot] = order == VanillaExportBlock::TCO_Attack ? target : nullptr;
        AtkByLock[c.Slot] = myTarget; // lancecontrol-34: through your lock - ends when your lock leaves it
        AtkSinceMs[c.Slot] = GetTickCount64();
        AtkSeenMs[c.Slot] = AtkLockLostMs[c.Slot] = 0;
        // lancecontrol-3: an attack order releases Hold Fire (ours, and the game's while the plugin
        // holds its flag) - a lancemate told to attack that then never shoots is never what's meant.
        if (order == VanillaExportBlock::TCO_Attack && c.Slot <= VanillaExportBlock::kTacMaxMembers)
        {
            const bool ours = FsDisc[c.Slot] == 2;
            // The game's own Hold Fire: while the plugin holds the flag, it is IcMaskOld (put back
            // on hand-back); otherwise (insane allies off) it is the live flag itself, switched off
            // here directly (lancecontrol-5).
            const bool game = IcMaskSet[c.Slot] ? IcMaskOld[c.Slot] == 1
                                                : (TacUnit[c.Slot] && IcSuppressedNow(TacUnit[c.Slot]) == 1);
            if (ours)
                FsDisc[c.Slot] = 0;
            if (game)
            {
                if (IcMaskSet[c.Slot])
                    IcMaskOld[c.Slot] = 0;
                else
                    IcSuppress(TacUnit[c.Slot], false);
            }
            if (ours || game)
                TacLog("slot %u - attack order: Hold Fire released (%s) - weapons free", c.Slot,
                        ours && game ? "fire settings and the game's" : ours ? "fire settings" : "the game's");
        }
        // TacOps' Hold Fire toggle while the plugin holds the flag: the hand-back follows it.
        if (order == VanillaExportBlock::TCO_HoldFire && IcMaskSet[c.Slot])
        {
            IcMaskOld[c.Slot] = IcMaskOld[c.Slot] ? 0 : 1;
            TacLog("slot %u - Hold Fire toggled while the plugin fires: obeyed - %s", c.Slot,
                    IcMaskOld[c.Slot] ? "HOLDING FIRE" : "weapons free");
        }
        if (order == VanillaExportBlock::TCO_Move)
        {
            // What the consumer must measure arrival against: where the order really went.
            std::memcpy(TacLastMove[c.Slot], loc, sizeof(loc));
            TacLastSeq[c.Slot] = c.Seq;
            LTraceStart(c.Slot, loc); // discovery: how this lancemate gets there
            std::memcpy(TacPendGoal[c.Slot], loc, sizeof(loc)); // for GoToFix when its job starts
            TacPendMs[c.Slot] = GetTickCount64();
            TacPendDirect[c.Slot] = 0;
            // lancecontrol-10: a hold at a point ("move 500 forward and hold", TacOps hold here) walks
            // there the fire-on-the-move way too - the AI's combat movement held off, re-sent on
            // course when it stalls - so a lancemate under fire keeps going instead of stopping to
            // fight short of the point. The watch ends at 20 m, where the hold takes over (HoldTick).
            bool holdWalk = false;
            if (hold && TacUnit[c.Slot] && IsLive(TacUnit[c.Slot]))
            {
                float here[3] = {};
                if (CallRet(TacUnit[c.Slot], L"K2_GetActorLocation", {}, here, sizeof(here)))
                {
                    const float hx = loc[0] - here[0], hy = loc[1] - here[1];
                    holdWalk = hx * hx + hy * hy > 3000.0f * 3000.0f; // not for "hold position" where it stands
                }
            }
            if (fireMove || fallback || holdWalk)
            {
                FomOn[c.Slot] = true;
                FomFallback[c.Slot] = fallback;
                std::memcpy(FomGoal[c.Slot], loc, sizeof(loc));
                FomStartMs[c.Slot] = GetTickCount64();
                FomForced[c.Slot] = 0;
                FomTarget[c.Slot] = nullptr;
                FomFace[c.Slot] = nullptr;
                FomFaceMs[c.Slot] = 0;
                FomLostMs[c.Slot] = 0;
                FomLosNextMs[c.Slot] = 0;
                FomKept[c.Slot] = 0;
                FomProgMs[c.Slot] = 0;
                FomResent[c.Slot] = 0;
            }
        }
        if (hold && order == VanillaExportBlock::TCO_Move)
        {
            HoldOn[c.Slot] = true;
            HoldIndirect[c.Slot] = IcHoldNext;
            HoldArmed[c.Slot] = false;
            HoldDrift[c.Slot] = false;
            std::memcpy(HoldGoal[c.Slot], loc, sizeof(loc));
            HoldStartMs[c.Slot] = GetTickCount64();
            HoldNextMs[c.Slot] = 0;
            HoldGiven[c.Slot] = 0;
            HoldRestarts[c.Slot] = 0;
            HoldLostMs[c.Slot] = 0;
            for (auto &r : Rejects[c.Slot])
                r = RejectEntry{};
        }
        static const char *kName[4] = {"MOVE", "ATTACK", "FOLLOW", "HOLD FIRE / WEAPONS FREE"};
        TacLog("tacops: %s%s -> slot %u at (%.0f, %.0f, %.0f)%s%ls", kName[order],
                fireMove ? " (FIRE ON THE MOVE)" : (fallback ? " (FALL BACK)" : (hold ? " (AND HOLD)" : "")),
                c.Slot, loc[0], loc[1], loc[2], target ? " target " : "", target ? ClassName(target).c_str() : L"");
        return true;
    }

    // =====================================================================================
    // FIRE ON THE MOVE (experiment). The lancemate probe (vanillahud-31) showed a moving lancemate
    // stop dead within a second or two of its AI setting bMoveWithRespectToCombatTarget - combat
    // manoeuvring taking over the move. So, while a TCO_MoveFire is under way, that flag is held
    // off. Targeting, torso aim and firing are separate parts of the AI and are left alone: the
    // question this build answers is whether they keep shooting while the legs keep walking.
    // The trace (LTrace) logs the flag being forced, the combat and torso targets, and heat.
    // =====================================================================================
    bool      FomOn[VanillaExportBlock::kTacMaxMembers + 1] = {};

    // FALL BACK: the same watch with the opposite rule - while the AI has a combat target, the
    // flag is held ON, so its movement stays oriented to the enemy (facing it, reversing when the
    // point is behind). With no enemy it is left alone and the lancemate simply walks there.
    bool      FomFallback[VanillaExportBlock::kTacMaxMembers + 1] = {};

    float     FomGoal[VanillaExportBlock::kTacMaxMembers + 1][3] = {};

    ULONGLONG FomStartMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       FomForced[VanillaExportBlock::kTacMaxMembers + 1] = {};

    // Keeping it on course. vanillahud-35: a fall-back held until its enemy died - then the AI
    // took a new enemy and its own combat movement replaced the walk to the point (holding the
    // flag cannot undo a move the AI has already ordered). So the walk is re-sent when the
    // combat target changes, and whenever the point has not got 5 m closer in 4 s.
    API::UObject *FomTarget[VanillaExportBlock::kTacMaxMembers + 1] = {};

    float     FomBestCm[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG FomProgMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       FomResent[VanillaExportBlock::kTacMaxMembers + 1] = {};

    API::FProperty *FomFlag = nullptr; // MWMechAIController's bMoveWithRespectToCombatTarget

    const void     *FomFlagCls = nullptr;

    // KEEPING AN ENEMY TO FACE (vanillahud-39). The trace showed a falling-back lancemate lose its
    // combat target again and again - twice in 7 s, then 14 s with none - most likely as backing
    // away takes the enemy out of sight. With no target the AI walks to the point facing forwards,
    // which is the turn-and-walk the user saw. So while a fall-back runs and the game has cleared
    // the target, the lancemate is handed one back: the enemy it last faced if that is still alive
    // and within kFomFaceCm of it, else the nearest live enemy that is. Handed back the way the
    // game's own Ally_TargetAttackJob does it: the enemy is made known to the AI's awareness
    // (AddKnownTarget), then set as its combat target (MWCombatControllerInterface::
    // K2_SetCombatTarget). Only enemies found in this tick's actor list are ever touched, so a
    // remembered pointer to a unit that has since been destroyed is never called on.
    static constexpr float kFomFaceCm = 80000.0f; // 800 m

    API::UObject *FomFace[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG FomFaceMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       FomKept[VanillaExportBlock::kTacMaxMembers + 1] = {};

    API::UFunction *FomSetTgtFn = nullptr, *FomKnownFn = nullptr;

    bool      FomFnsTried = false;

    int       FomFaceLog = 0;

    // A call with one object argument - the function's first parameter - in its own frame (CallBuf
    // may be in use further up). False when the first parameter is not an object.
    bool CallWithObject(API::UObject *obj, API::UFunction *fn, API::UObject *arg)
    {
        if (!obj || !fn)
            return false;
        const int32_t frame = fn->get_properties_size();
        const int32_t off = FirstParamOffset(fn);
        if (frame <= 0 || frame > 4096 || off < 0 || FirstParamSize(fn) != (int32_t)sizeof(void *))
            return false;
        std::vector<uint8_t> fb((size_t)frame + 16, 0);
        std::memcpy(fb.data() + off, &arg, sizeof(void *));
        obj->process_event(fn, fb.data());
        return true;
    }

    // TEAM (lancecontrol-66, user 2026-10-17): "TeamID" on every unit - mechs, vehicles, the player AND a battle armour
    // suit. The exe spells the suit's property "TeamId", but shipping FNames are case-insensitive and keep the first
    // spelling registered, so at runtime it reads "TeamID" like the rest (lancecontrol-65's "TeamId" fallback removed).
    static uint8_t TeamOf(API::UObject *a)
    {
        auto *p = a && a->get_class() ? FindProp(a->get_class(), L"TeamID") : nullptr;
        return p ? *(reinterpret_cast<uint8_t *>(a) + p->get_offset()) : (uint8_t)0xFF;
    }

    // Dead - including a WRECK: a destroyed mech stays in the world as an actor with its team,
    // and vanillahud-44's log showed wrecks being handed over again and again (the AI refused
    // them: "target now none" twice a second). DerivedMech has no IsDead of its own; the same
    // flags LidarExport marks a mech dead by are read instead.
    // THE GAME'S OWN DEATH FLAG (lancecontrol-23). Every unit's health lives in an MWLivingActorComponent -
    // MWMech.MechLivingActorComponent (MWMechLivingActorComponent), MWTank / MWTurret / MWVTOL
    // .SimpleLivingActorComponent - whose replicated bIsDead (OnRep_bIsDead, IsDead()) is set on EVERY kind of
    // death: cored, head, both legs, ammo, ejection (GetCausesOfDeath). MWTargetComponent.OnLivingActorDeath /
    // UpdateIsOwnerDead - what the game's own markers and target boxes use - follows it. The old test (MWMech has
    // no IsDead(); bIsPilotDead / bCenterTorsoDestroyed) missed a legged mech, which then "lived" on in the units
    // feed and the callouts. Read as a property (no call); once dead, latched for the mission.
    // lancecontrol-72 (bug sweep 24, 2026-10-18): the latch is trusted only while the unit keeps being checked (the status
    // feed asks for every hostile 4 times a second). A unit destroyed and freed mid-mission stops being checked; a new
    // unit of the same class (every mech is DerivedMech_C) put at the same address arrives after that gap and is checked
    // afresh, instead of being treated as dead - never targeted, published Dead - for the rest of the mission.
    struct DeadMark
    {
        API::UClass *Cls;
        ULONGLONG    Ms;
    };
    static constexpr ULONGLONG kDeadLatchGapMs = 1500;
    std::unordered_map<API::UObject *, DeadMark> DeadLatch;
    bool DeadLatchHit(API::UObject *a) // a is still the dead unit latched at this address
    {
        auto it = DeadLatch.find(a);
        if (it == DeadLatch.end())
            return false;
        const ULONGLONG now = GetTickCount64();
        if (it->second.Cls == a->get_class() && now - it->second.Ms <= kDeadLatchGapMs)
        {
            it->second.Ms = now;
            return true;
        }
        DeadLatch.erase(it); // another class, or a gap: maybe another unit now
        return false;
    }
    void DeadLatchSet(API::UObject *a)
    {
        if (DeadLatch.size() > 1024)
            DeadLatch.clear();
        DeadLatch[a] = DeadMark{a->get_class(), GetTickCount64()};
    }
    bool DeadWarned = false;

    bool LivingDead(API::UObject *a, bool &known)
    {
        known = false;
        // lancecontrol-64: a battle armour suit's living component may sit under the generic name - tried last
        for (const wchar_t *cn : {L"MechLivingActorComponent", L"SimpleLivingActorComponent", L"LivingActorComponent"})
            if (API::UObject *lc = ObjProp(a, cn))
                // lancecontrol-63: the component can already be freed while its mech is still an object (a mech being
                // torn down at its death) - its pointer read off the mech gave a garbage class and UEVR crashed (run 4,
                // 23:15). Checked like the unit itself; when it is gone the death flag is skipped (the tests below).
                if (!ObjStillThere(lc))
                    continue;
                else if (auto *p = FindProp(lc->get_class(), L"bIsDead"))
                    if (PropType(p) == L"BoolProperty")
                    {
                        known = true;
                        return reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(lc);
                    }
        return false;
    }

    // lancecontrol-62 (user, 2026-10-16): IS THIS POINTER STILL AN OBJECT? Live() is cached for 250 ms, so a pawn the
    // engine freed inside that window was still in it; IsDeadUnit then read its living component through the freed
    // memory and UEVR crashed on the garbage class (ArcBlocked's friendly check, long test run 4, 22:27). The engine's
    // own object table answers it: the object's InternalIndex (UObjectBase + 0x0C in UE4) must still hold this same
    // object. The read is under SEH. Existence only - no "pending kill" flag test (a destroyed, not yet freed actor is
    // valid memory and the death tests below handle it). Self-tested once on your pawn: if the table does not give it
    // back, the check switches itself off (the old behaviour) and says so in the log. The engine frees objects between
    // ticks, so a pointer that passes here stays valid for the rest of the tick.
    static bool ObjIndexRead(API::UObject *o, int32_t &idx)
    {
        __try
        {
            idx = *reinterpret_cast<const int32_t *>(reinterpret_cast<const uint8_t *>(o) + 0x0C);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int ObjCheckState = 0; // 0 not tested yet, 1 on, -1 off (self-test failed)

    bool ObjStillThere(API::UObject *o)
    {
        if (!o)
            return false;
        auto *arr = API::FUObjectArray::get();
        if (!arr)
            return true;
        if (ObjCheckState == 0)
        {
            API::UObject *me = API::get()->get_local_pawn(0);
            if (!me)
                return true; // tested once there is a pawn
            int32_t mi = -1;
            const bool ok = ObjIndexRead(me, mi) && mi >= 0 && mi < arr->get_object_count() && arr->get_object(mi) == me;
            ObjCheckState = ok ? 1 : -1;
            if (ok)
                LogInfo("object check: on (your pawn found in the object table at index %d)", mi);
            else
                LogWarn("object check: OFF - your pawn was not at its object index (read %d) - stale pointers are not caught", mi);
        }
        if (ObjCheckState < 0)
            return true;
        int32_t i = -1;
        if (!ObjIndexRead(o, i) || i < 0 || i >= arr->get_object_count())
            return false;
        return arr->get_object(i) == o;
    }

    bool IsDeadUnit(API::UObject *a)
    {
        if (!a || !ObjStillThere(a) || !a->get_class())
            return true;
        {
            if (DeadLatchHit(a)) // lancecontrol-72
                return true;
            bool known = false;
            if (LivingDead(a, known))
            {
                DeadLatchSet(a);
                return true;
            }
            if (!known && !DeadWarned)
            {
                DeadWarned = true;
                LogWarn("death flag: no living component's bIsDead on %ls - the old death test only", ClassName(a).c_str());
            }
        }
        if (FindFn(a, L"IsDead"))
        {
            uint8_t dead = 0;
            if (CallRet(a, L"IsDead", {}, &dead, 1) && dead)
                return true;
        }
        for (const wchar_t *n : {L"bIsPilotDead", L"bCenterTorsoDestroyed", L"bIsDead", L"bDead"})
            if (auto *p = FindProp(a->get_class(), n))
                if (PropType(p) == L"BoolProperty" && reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(a))
                    return true;
        return false;
    }

    // Enemies a lancemate's AI refused when handed over (set, then still no target): skipped for
    // 10 s, per lancemate, so the same one is not handed over twice a second.
    struct RejectEntry
    {
        API::UObject *A = nullptr;
        ULONGLONG     Ms = 0;
    };

    RejectEntry Rejects[VanillaExportBlock::kTacMaxMembers + 1][6] = {};

    bool Rejected(int s, API::UObject *a, ULONGLONG now) const
    {
        for (const auto &r : Rejects[s])
            if (r.A == a && now - r.Ms < 10000)
                return true;
        return false;
    }

    void Reject(int s, API::UObject *a, ULONGLONG now)
    {
        RejectEntry *slot = &Rejects[s][0];
        for (auto &r : Rejects[s])
        {
            if (r.A == a || now - r.Ms >= 10000)
            {
                slot = &r;
                break;
            }
            if (r.Ms < slot->Ms)
                slot = &r;
        }
        slot->A = a;
        slot->Ms = now;
    }

    // The enemy for slot s to face: its last one if still alive and in range, else the nearest.
    API::UObject *FomPickFace(API::UObject *pc, API::UObject *pawn, const float *here, int s)
    {
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> all;
        if (!PawnClass || !ActorsOf(pc, PawnClass, all))
            return nullptr;
        const uint8_t ours = TeamOf(pawn);
        API::UObject *best = nullptr;
        float bestD = kFomFaceCm * kFomFaceCm;
        for (auto *a : all)
        {
            if (!a || a == pawn || TeamOf(a) == ours || TeamOf(a) == 0xFF || Rejected(s, a, GetTickCount64()))
                continue;
            bool lance = false;
            for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers && !lance; ++k)
                lance = TacUnit[k] == a;
            if (lance)
                continue;
            float p[3] = {};
            if (!CallRet(a, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            const float dx = p[0] - here[0], dy = p[1] - here[1];
            const float d = dx * dx + dy * dy;
            if (d > kFomFaceCm * kFomFaceCm || IsDeadUnit(a) || !IsCombatUnit(a))
                continue;
            if (a == FomFace[s])
                return a; // the one it was facing - live, in range: keep it
            if (d < bestD)
            {
                bestD = d;
                best = a;
            }
        }
        return best;
    }

    void FomKeepTarget(API::UObject *pc, API::UObject *pawn, API::UObject *ctl, const float *here, int s,
                       ULONGLONG now)
    {
        if (now - FomFaceMs[s] < 500)
            return; // at most twice a second per lancemate
        FomFaceMs[s] = now;
        if (!ResolveSetTarget(ctl))
            return;
        API::UObject *face = FomPickFace(pc, pawn, here, s);
        if (!face)
            return; // nothing within 800 m: walking forwards is safe
        API::UObject *aware = nullptr;
        bool known = false;
        const bool set = GiveTarget(ctl, pawn, face, aware, known);
        API::UObject *now_tgt = ObjProp(ctl, L"CombatTarget");
        if (now_tgt != face)
            Reject(s, face, now);
        const bool same = face == FomFace[s];
        FomFace[s] = face;
        ++FomKept[s];
        if (FomFaceLog++ < 40)
            TacLog("tacops: fall back, slot %d +%.1fs - no combat target, handed back %s %ls (awareness %s, set %s, target now %ls)",
                    s, (now - FomStartMs[s]) / 1000.0f, same ? "its last enemy" : "the nearest enemy",
                    ClassName(face).c_str(), aware ? (known ? "added" : "call failed") : "not found",
                    set ? "ok" : "FAILED", now_tgt ? ClassName(now_tgt).c_str() : L"none");
    }

    bool ResolveSetTarget(API::UObject *ctl)
    {
        if (!FomFnsTried)
        {
            FomFnsTried = true;
            FomSetTgtFn = FindFn(ctl, L"K2_SetCombatTarget");
            if (!FomSetTgtFn)
                if (auto *ic = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWCombatControllerInterface"))
                    FomSetTgtFn = ic->find_function(L"K2_SetCombatTarget");
            TacLog("tacops: target handing - K2_SetCombatTarget %s%ls", FomSetTgtFn ? "found, first param " : "NOT FOUND",
                    FomSetTgtFn ? FirstParamName(FomSetTgtFn).c_str() : L"");
        }
        return FomSetTgtFn != nullptr;
    }

    // Hand `face` to the lancemate as its combat target, the way the game's Ally_TargetAttackJob
    // does: made known to the AI's awareness first (so it is not dropped at once), then set.
    bool GiveTarget(API::UObject *ctl, API::UObject *pawn, API::UObject *face, API::UObject *&aware, bool &known)
    {
        aware = nullptr;
        known = false;
        if (FindFn(ctl, L"GetAwarenessComponent"))
            CallRet(ctl, L"GetAwarenessComponent", {}, &aware, sizeof(aware));
        if (!aware && FindFn(pawn, L"GetAwarenessComponent"))
            CallRet(pawn, L"GetAwarenessComponent", {}, &aware, sizeof(aware));
        if (!aware)
        {
            // Not a function on either: the component itself (MWAwarenessComponent, which the
            // attack job's AddKnownTarget / RemoveKnownTarget are called on).
            static API::UClass *awCls = nullptr;
            if (!awCls)
                awCls = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWAwarenessComponent");
            std::vector<API::UObject *> comps;
            if (awCls && ComponentsOn(ctl, awCls, comps) && !comps.empty())
                aware = comps[0];
            else if (awCls && ComponentsOn(pawn, awCls, comps) && !comps.empty())
                aware = comps[0];
        }
        if (aware)
        {
            if (!FomKnownFn || FindFn(aware, L"AddKnownTarget") != FomKnownFn)
                FomKnownFn = FindFn(aware, L"AddKnownTarget");
            known = CallWithObject(aware, FomKnownFn, face);
        }
        return CallWithObject(ctl, FomSetTgtFn, face);
    }

    // =====================================================================================
    // HOLD HERE (vanillahud-40). Taken apart from the game's trees: the lancemate master tree
    // (BT_AllyMechMasterTree) runs, in order, Wait -> Guard (a move order: BT_MechGoToLocation)
    // -> Attack -> Follow. A move order sets Guard; within 55 m of its point BT_MechGoToLocation
    // goes into guard mode and keeps walking to random spots near it. The Wait branch just
    // stands. Firing is not in the ally tree at all (it shoots on the move with no fire task),
    // so a waiting lancemate still shoots its combat target - but target PICKING lives in the
    // move / follow trees, so while it waits this code keeps it supplied: nearest live enemy
    // within 800 m, ones it has line of sight to first.
    //   Arrival (20 m): Wait on, Guard cleared - clearing Guard aborts the guard branch at once
    //   (its decorator aborts Both); setting Wait alone would only bite after the wander ends.
    //   Ends: any new TacOps order (Wait cleared here), the game clearing Wait itself (an F-key
    //   order's job does), the unit gone, or no arrival within 4 min.
    // =====================================================================================
    bool      HoldOn[VanillaExportBlock::kTacMaxMembers + 1] = {};

    bool      HoldArmed[VanillaExportBlock::kTacMaxMembers + 1] = {};

    bool      HoldDrift[VanillaExportBlock::kTacMaxMembers + 1] = {};

    float     HoldGoal[VanillaExportBlock::kTacMaxMembers + 1][3] = {};

    float     HoldAt[VanillaExportBlock::kTacMaxMembers + 1][3] = {};

    ULONGLONG HoldStartMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG HoldNextMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       HoldGiven[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       HoldRestarts[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       HoldLog = 0;

    ULONGLONG HoldLostMs[VanillaExportBlock::kTacMaxMembers + 1] = {}; // current target out of sight since

    ULONGLONG FomLostMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG FomLosNextMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       SwitchLog = 0;

    // DON'T HYPERFIXATE (vanillahud-42). A target we handed over is KNOWN to the AI's awareness,
    // so it keeps it even behind cover - and ignored whoever was shooting it meanwhile. So: when
    // the current target has been out of line of sight for 2 s and another live enemy within
    // 800 m IS in sight, switch to the nearest such one, and take the hidden one off the known
    // list (RemoveKnownTarget, as the game's attack job does) so the AI does not flip straight
    // back; it is re-detected normally once it shows itself. With no enemy in sight the hidden
    // target is kept - in a fall-back that is what keeps the lancemate facing the threat.
    bool SwitchIfHidden(API::UObject *pc, API::UObject *pawn, API::UObject *ctl, const float *here, int s,
                        API::UObject *cur, ULONGLONG &lostMs, ULONGLONG now, const char *mode)
    {
        if (!cur || !FindFn(ctl, L"LineOfSightTo"))
            return false;
        uint8_t seen = 0;
        if (!CallRet(ctl, L"LineOfSightTo", {{L"Other", &cur, sizeof(cur)}}, &seen, 1) || seen)
        {
            lostMs = 0;
            return false;
        }
        if (!lostMs)
            lostMs = now;
        if (now - lostMs < 2000 || !ResolveSetTarget(ctl))
            return false;
        API::UObject *next = HoldPick(pc, pawn, ctl, here, cur, true, true, s);
        if (!next)
            return false;
        API::UObject *aware = nullptr;
        bool known = false;
        const bool set = GiveTarget(ctl, pawn, next, aware, known);
        bool forgot = false;
        if (aware)
            if (API::UFunction *rm = FindFn(aware, L"RemoveKnownTarget"))
                forgot = CallWithObject(aware, rm, cur);
        API::UObject *now_tgt = ObjProp(ctl, L"CombatTarget");
        if (now_tgt != next)
            Reject(s, next, now);
        lostMs = 0;
        if (SwitchLog++ < 60)
            TacLog("tacops: %s, slot %d - %ls out of sight %.1f s, switched to %ls in sight (set %s, old one %s, target now %ls)",
                    mode, s, ClassName(cur).c_str(), 2.0f, ClassName(next).c_str(), set ? "ok" : "FAILED",
                    forgot ? "forgotten" : "kept known", now_tgt ? ClassName(now_tgt).c_str() : L"none");
        return true;
    }

    // RESTART THE LANCEMATE'S TREE FROM ITS ROOT. vanillahud-40 set Wait and cleared Guard, and the
    // lancemates walked back to the player: clearing Guard aborts the guard branch, and a UE
    // selector then carries on with the NEXT child down - Attack (not set), then Follow - it
    // does not go back up to Wait, whose decorator only aborts Self and so never interrupts a
    // lower branch. From the root, Wait is checked first and wins.
    bool RestartTree(API::UObject *ctl, const char *&how)
    {
        how = "no brain component";
        API::UObject *brain = ObjProp(ctl, L"BrainComponent");
        if (!brain)
            return false;
        if (FindFn(brain, L"RestartLogic") && Call(brain, L"RestartLogic", {}))
        {
            how = "RestartLogic";
            return true;
        }
        if (FindFn(brain, L"RestartTree") && Call(brain, L"RestartTree", {}))
        {
            how = "RestartTree";
            return true;
        }
        how = "no restart function on the brain component";
        return false;
    }

    bool BbBool(API::UObject *ctl, const wchar_t *key, int set /* -1 read, 0 clear, 1 set */, bool *out = nullptr)
    {
        API::UObject *bb = ObjProp(ctl, L"Blackboard");
        if (!bb)
            return false;
        API::FName k(key, API::FName::EFindName::Find);
        if (set < 0)
        {
            uint8_t v = 0;
            if (!CallRet(bb, L"GetValueAsBool", {{L"KeyName", &k, sizeof(k)}}, &v, 1))
                return false;
            if (out)
                *out = v != 0;
            return true;
        }
        if (set == 0)
            return Call(bb, L"ClearValue", {{L"KeyName", &k, sizeof(k)}});
        bool yes = true;
        return Call(bb, L"SetValueAsBool", {{L"KeyName", &k, sizeof(k)}, {L"BoolValue", &yes, 1}});
    }

    void HoldEnd(int s, const char *why, bool clearWait)
    {
        if (clearWait && HoldArmed[s])
            if (API::UObject *u = TacUnit[s])
                if (API::UObject *ctl = ObjProp(u, L"Controller"))
                    BbBool(ctl, L"Wait", 0);
        TacLog("tacops: hold, slot %d - ended (%s) after %.0f s, %d target(s) handed over", s, why,
                (GetTickCount64() - HoldStartMs[s]) / 1000.0f, HoldGiven[s]);
        HoldOn[s] = false;
        HoldArmed[s] = false;
    }

    // The enemy for a holding lancemate: nearest live one within 800 m it can see, else nearest.
    API::UObject *HoldPick(API::UObject *pc, API::UObject *pawn, API::UObject *ctl, const float *here,
                           API::UObject *skip, bool skipOn, bool seenOnly = false, int slot = 0)
    {
        const ULONGLONG now = GetTickCount64();
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> all;
        if (!PawnClass || !ActorsOf(pc, PawnClass, all))
            return nullptr;
        const uint8_t ours = TeamOf(pawn);
        API::UObject *bestSeen = nullptr, *best = nullptr;
        float dSeen = kFomFaceCm * kFomFaceCm, dAny = kFomFaceCm * kFomFaceCm;
        const bool losFn = FindFn(ctl, L"LineOfSightTo") != nullptr;
        for (auto *a : all)
        {
            if (!a || a == pawn || TeamOf(a) == ours || TeamOf(a) == 0xFF || (skipOn && a == skip) ||
                (slot && Rejected(slot, a, now)))
                continue;
            float p[3] = {};
            if (!CallRet(a, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            const float dx = p[0] - here[0], dy = p[1] - here[1];
            const float d = dx * dx + dy * dy;
            if (d > kFomFaceCm * kFomFaceCm || IsDeadUnit(a) || !IsCombatUnit(a))
                continue;
            if (d < dAny)
            {
                dAny = d;
                best = a;
            }
            if (losFn && d < dSeen)
            {
                uint8_t seen = 0;
                if (CallRet(ctl, L"LineOfSightTo", {{L"Other", &a, sizeof(a)}}, &seen, 1) && seen)
                {
                    dSeen = d;
                    bestSeen = a;
                }
            }
        }
        return (bestSeen || seenOnly) ? bestSeen : best;
    }

    void HoldTick(API::UObject *pc)
    {
        bool any = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            any |= HoldOn[s];
        if (!any)
            return;
        TacRefreshMap(pc, GetTickCount64());
        const ULONGLONG now = GetTickCount64();
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            if (!HoldOn[s])
                continue;
            API::UObject *pawn = TacUnit[s];
            float here[3] = {};
            if (!pawn || !CallRet(pawn, L"K2_GetActorLocation", {}, here, sizeof(here)))
            {
                HoldEnd(s, "unit gone", false);
                continue;
            }
            API::UObject *ctl = ObjProp(pawn, L"Controller");
            if (!ctl)
                continue;
            if (!HoldArmed[s])
            {
                // Not while the order's own move job may still start: the job clears Wait when it
                // begins. vanillahud-44's log: a lancemate already at the point was armed in the
                // same tick the hold order was posted, its job started 4 ms later, and the hold
                // ended at once ("the game cleared Wait") - the lancemate then idled.
                if (TacPendMs[s])
                    continue;
                const float dx = HoldGoal[s][0] - here[0], dy = HoldGoal[s][1] - here[1];
                if (dx * dx + dy * dy > 2000.0f * 2000.0f)
                {
                    if (now - HoldStartMs[s] > 240000)
                        HoldEnd(s, "did not arrive in 4 min", false);
                    continue;
                }
                // Guard is left set: the restart below puts the tree in Wait regardless, and if
                // the game ever clears Wait the lancemate falls back to guarding this point.
                const bool w = BbBool(ctl, L"Wait", 1);
                const char *how = "";
                const bool r = w && RestartTree(ctl, how);
                HoldArmed[s] = w;
                HoldRestarts[s] = 1;
                std::memcpy(HoldAt[s], HoldGoal[s], sizeof(here)); // it finishes the walk to the point
                LsEmit(LSE_Arrived, s, nullptr, HoldIndirect[s] ? 2.0f : 1.0f);
                TacLog("tacops: hold, slot %d - arrived after %.1f s, %.0f m from the point: Wait %s, tree restart %s (%s)", s,
                        (now - HoldStartMs[s]) / 1000.0f, std::sqrt(dx * dx + dy * dy) * 0.01f, w ? "set" : "FAILED",
                        r ? "ok" : "FAILED", how);
                if (!w)
                    HoldEnd(s, "could not set Wait", false);
                continue;
            }
            bool waiting = true;
            if (BbBool(ctl, L"Wait", -1, &waiting) && !waiting)
            {
                HoldEnd(s, "the game cleared Wait - another order?", false);
                continue;
            }
            {
                const float dx = HoldAt[s][0] - here[0], dy = HoldAt[s][1] - here[1];
                if (dx * dx + dy * dy > 2500.0f * 2500.0f && now >= HoldNextMs[s])
                {
                    // Walking away from the point with Wait set: the tree is somewhere other than
                    // Wait. Restart it again (a few times at most) and say so.
                    const char *how = "";
                    const bool r = HoldRestarts[s] < 5 && RestartTree(ctl, how);
                    if (r)
                        ++HoldRestarts[s];
                    if (!HoldDrift[s] || r)
                        TacLog("tacops: hold, slot %d +%.1fs - %.0f m from the point with Wait set, tree restart %s (%s, %d so far)", s,
                                (now - HoldStartMs[s]) / 1000.0f, std::sqrt(dx * dx + dy * dy) * 0.01f,
                                r ? "ok" : "not done", how, HoldRestarts[s]);
                    HoldDrift[s] = true;
                    HoldNextMs[s] = now + 3000;
                }
            }
            if (now < HoldNextMs[s])
                continue;
            HoldNextMs[s] = now + 500;
            API::UObject *cur = ObjProp(ctl, L"CombatTarget");
            if (HoldIndirect[s])
            {
                // Indirect fire from cover: the game picks from what the team knows - except when it
                // has nothing (the tree restart that arms the hold clears the target) or has picked
                // something no LRM can reach. IcTick handles its own reach check; this one covers
                // the gap right after arming.
                if ((!cur || IsDeadUnit(cur)) && now - IcReachMs[s] > 2000)
                {
                    IcReachMs[s] = now;
                    TargetInReach(pc, s, pawn, ctl, here, nullptr, "hold (indirect)");
                }
                continue;
            }
            // Under insane allies its own timer switches targets (IaTargetCheck): no swaps here.
            if (IaSet[s] && cur && !IsDeadUnit(cur))
                continue;
            // A plain hold: a target beyond LRM reach is swapped for one in reach, every 2 s.
            if (cur && !IsDeadUnit(cur) && now - IcReachMs[s] > 2000)
            {
                IcReachMs[s] = now;
                if (TargetInReach(pc, s, pawn, ctl, here, cur, "hold"))
                    continue;
            }
            if (cur && !IsDeadUnit(cur))
            {
                SwitchIfHidden(pc, pawn, ctl, here, s, cur, HoldLostMs[s], now, "hold");
                continue;
            }
            HoldLostMs[s] = 0;
            if (!ResolveSetTarget(ctl))
                continue;
            API::UObject *face = HoldPick(pc, pawn, ctl, here, nullptr, false, false, s);
            if (!face)
                continue;
            API::UObject *aware = nullptr;
            bool known = false;
            const bool set = GiveTarget(ctl, pawn, face, aware, known);
            API::UObject *now_tgt = ObjProp(ctl, L"CombatTarget");
            ++HoldGiven[s];
            if (now_tgt != face)
                Reject(s, face, now); // not taken (a wreck, a VTOL): try others for 10 s
            if (HoldLog++ < 60)
                TacLog("tacops: hold, slot %d +%.1fs - %s, handed %ls (awareness %s, set %s, target now %ls)", s,
                        (now - HoldStartMs[s]) / 1000.0f, cur ? "target dead" : "no target", ClassName(face).c_str(),
                        aware ? (known ? "added" : "call failed") : "not found", set ? "ok" : "FAILED",
                        now_tgt ? ClassName(now_tgt).c_str() : L"none");
        }
    }

    // =====================================================================================
    // THE PLAYER'S TORSO (vanillahud-46): slew to an angle, to a point, or keep it on a target.
    // Driven through the pilot's VIEW target (MechViewComponent.TargetRelativeViewRotator), which
    // the torso follows: stepped every frame toward what is wanted at the torso's own twist rate
    // (TorsoTwistComponent.TorsoStats, as MWVR reads it) and kept inside its twist bounds so the
    // game does not clamp it under us. The pilot takes it back with any input (mouse delta, the
    // view target moved by something else, or the input handler's torso fields - see PwMoved);
    // a torso that has not moved in 3 s while still off target ends it too. Logged at the start.
    // =====================================================================================
    enum TorsoModeE { TMO_Off = 0, TMO_Angle, TMO_Point, TMO_Target, TMO_Centre };

    int           TorsoMode = TMO_Off;

    float         TorsoRel = 0.0f;       // TMO_Angle: yaw relative to the legs, degrees

    float         TorsoPoint[3] = {};    // TMO_Point

    API::UObject *TorsoTgt = nullptr;    // TMO_Target - only ever used after being found live again

    ULONGLONG     TorsoStartMs = 0, TorsoLastMs = 0;

    bool          TorsoViewWarned = false;

    ULONGLONG     TorsoProgMs = 0;        // the stall check's window: started at TorsoProgYaw

    float         TorsoProgYaw = 0.0f;

    bool          TorsoProgSet = false;

    float         TorsoWrote[2] = {}; // TargetRelativeViewRotator pitch, yaw as this left it last frame

    bool          TorsoWroteOk = false;

    // A call whose results are all out-parameters (no ReturnValue): the first `size` bytes of its frame.
    bool CallOut(API::UObject *obj, const wchar_t *name, void *out, size_t size)
    {
        API::UFunction *fn = FindFn(obj, name);
        if (!fn)
            return false;
        const int32_t frame = fn->get_properties_size();
        if (frame < (int32_t)size || frame > 1024)
            return false;
        std::vector<uint8_t> fb((size_t)frame + 16, 0);
        obj->process_event(fn, fb.data());
        std::memcpy(out, fb.data(), size);
        return true;
    }

    struct TwistBoundsV
    {
        float Rate[2];
        bool  YawUnbound, PitchUnbound;
        float Low[2], High[2];
    };

    static float Norm180(float a)
    {
        while (a > 180.0f) a -= 360.0f;
        while (a < -180.0f) a += 360.0f;
        return a;
    }

    // TorsoTwistComponent.TorsoTwist (yaw, pitch, relative) and TorsoStats.Torso.
    bool TorsoRead(API::UObject *pawn, float twist[2], TwistBoundsV *tb)
    {
        API::UObject *tc = ObjProp(pawn, L"TorsoTwistComponent");
        if (!tc || !tc->get_class())
            return false;
        bool ok = false;
        if (auto *p = FindProp(tc->get_class(), L"TorsoTwist"))
        {
            std::memcpy(twist, reinterpret_cast<uint8_t *>(tc) + p->get_offset(), 8);
            ok = true;
        }
        if (tb)
        {
            std::memset(tb, 0, sizeof(*tb));
            if (auto *p = FindProp(tc->get_class(), L"TorsoStats"))
            {
                const uint8_t *b = reinterpret_cast<uint8_t *>(tc) + p->get_offset();
                std::memcpy(tb->Rate, b, 8);
                tb->YawUnbound = b[8] != 0;
                tb->PitchUnbound = b[9] != 0;
                std::memcpy(tb->Low, b + 12, 8);
                std::memcpy(tb->High, b + 20, 8);
            }
        }
        return ok;
    }

    bool TorsoOrder(API::UObject *pc, API::UObject *pawn, const VanillaExportBlock::TacCmd &c)
    {
        static const char *kName[] = {"angle", "point", "target", "centre", "release"};
        const int k = c.Order - VanillaExportBlock::TCO_TorsoAngle;
        if (c.Order == VanillaExportBlock::TCO_TorsoRelease)
        {
            TorsoEnd("released from TacOps");
            return true;
        }
        float here[3] = {}, rot[3] = {};
        CallRet(pawn, L"K2_GetActorLocation", {}, here, sizeof(here));
        CallRet(pawn, L"K2_GetActorRotation", {}, rot, sizeof(rot));
        TorsoTgt = nullptr;
        switch (c.Order)
        {
        case VanillaExportBlock::TCO_TorsoAngle:
            TorsoMode = TMO_Angle;
            TorsoRel = Norm180(std::atan2(c.Loc[1] - here[1], c.Loc[0] - here[0]) * 57.29578f - rot[1]);
            break;
        case VanillaExportBlock::TCO_TorsoPoint:
            TorsoMode = TMO_Point;
            std::memcpy(TorsoPoint, c.Loc, sizeof(TorsoPoint));
            break;
        case VanillaExportBlock::TCO_TorsoTarget:
            TorsoTgt = TacTargetNear(pc, c.Loc);
            if (!TorsoTgt)
            {
                TacLog("tacops: torso - no unit within 40 m of (%.0f, %.0f, %.0f) to track", c.Loc[0], c.Loc[1], c.Loc[2]);
                return false;
            }
            TorsoMode = TMO_Target;
            break;
        default:
            TorsoMode = TMO_Centre;
            TorsoRel = 0.0f;
            break;
        }
        TorsoStartMs = TorsoLastMs = GetTickCount64();
        TorsoWroteOk = false;
        TorsoProgSet = false;
        PwSettle(PWG_Torso);
        float cr[3] = {}, tw[2] = {};
        TwistBoundsV tb{};
        CallRet(pc, L"GetControlRotation", {}, cr, sizeof(cr));
        const bool twOk = TorsoRead(pawn, tw, &tb);
        TacLog("tacops: torso %s%s%ls - legs yaw %.1f, control yaw %.1f pitch %.1f, torso twist %s(%.1f, %.1f), rate (%.0f, %.0f), yaw bounds %s[%.0f, %.0f]",
                kName[k], TorsoMode == TMO_Angle ? " (relative) " : " ", TorsoTgt ? ClassName(TorsoTgt).c_str() : L"",
                rot[1], cr[1], cr[0], twOk ? "" : "NOT READ ", tw[0], tw[1], tb.Rate[0], tb.Rate[1],
                tb.YawUnbound ? "unbound " : "", tb.Low[0], tb.High[0]);
        return true;
    }

    void TorsoEnd(const char *why)
    {
        if (TorsoMode == TMO_Off)
            return;
        TorsoMode = TMO_Off;
        TorsoTgt = nullptr;
        TacLog("tacops: torso - let go (%s) after %.1f s", why, (GetTickCount64() - TorsoStartMs) / 1000.0f);
    }

    void TorsoTick(ULONGLONG now)
    {
        if (TorsoMode == TMO_Off)
            return;
        API::UObject *pc = API::get()->get_player_controller(0);
        API::UObject *pawn = API::get()->get_local_pawn(0);
        if (!pc || !pawn)
        {
            TorsoEnd("no mech");
            return;
        }
        float dt = (now - TorsoLastMs) / 1000.0f;
        TorsoLastMs = now;
        if (dt <= 0.0f)
            return;
        if (dt > 0.1f)
            dt = 0.1f;

        float here[3] = {}, rot[3] = {}, cr[3] = {};
        if (!CallRet(pawn, L"K2_GetActorLocation", {}, here, sizeof(here)) ||
            !CallRet(pawn, L"K2_GetActorRotation", {}, rot, sizeof(rot)) ||
            !CallRet(pc, L"GetControlRotation", {}, cr, sizeof(cr)))
            return;

        // The pilot moved the mouse (only possible while the overlay is closed): theirs again. Read
        // from the controller's own mouse delta - vanillahud-46 compared the control rotation with
        // what it last set, and the legs turning moved that too, so it let go for nothing.
        // Not in the first 100 ms after the overlay closes: the game taking the mouse back makes the
        // cursor and the view jump (seen 80 ms after closing), which would read as the pilot and
        // release an order just given from the overlay. The view check below waits the same.
        const bool closeGrace = !OvInputOn.load() && GetTickCount64() - OvInputOffMs < 100;
        if (!OvInputOn.load() && !closeGrace)
        {
            float md[2] = {};
            if (CallOut(pc, L"GetInputMouseDelta", md, sizeof(md)) && std::fabs(md[0]) + std::fabs(md[1]) > 0.5f)
            {
                TacLog("tacops: torso - the pilot moved the mouse (%.2f, %.2f)", md[0], md[1]);
                TorsoEnd("pilot input");
                return;
            }
        }
        // Any other device (vanillahud-55). The view's target is written by the pilot's look input
        // and by this code, nothing else: moved from where this left it, someone else moved it.
        if (TorsoWroteOk && !closeGrace) // in the grace, the next write simply carries on from the jump
        {
            API::UObject *vc = ObjProp(pawn, L"MechViewComponent");
            API::FProperty *vp = (vc && vc->get_class()) ? FindProp(vc->get_class(), L"TargetRelativeViewRotator") : nullptr;
            if (vp)
            {
                const float *vr = reinterpret_cast<const float *>(reinterpret_cast<uint8_t *>(vc) + vp->get_offset());
                // Pitch only where this order drives it (point, target): for an angle or centre order
                // the game settling the view's pitch is not the pilot.
                const bool drivesPitch = TorsoMode == TMO_Point || TorsoMode == TMO_Target;
                const float dy = Norm180(vr[1] - TorsoWrote[1]), dp = Norm180(vr[0] - TorsoWrote[0]);
                if (std::fabs(dy) > 0.5f || (drivesPitch && std::fabs(dp) > 0.5f))
                {
                    TacLog("tacops: torso - the view was moved (yaw %+.1f, pitch %+.1f) by something else", dy, dp);
                    TorsoEnd("pilot input: view");
                    return;
                }
            }
        }
        {
            float from = 0.0f, to = 0.0f;
            if (const PwField *f = PwMoved(pawn, PWG_Torso, from, to))
            {
                TacLog("tacops: torso - the pilot moved %ls (%.2f -> %.2f)", f->Name.c_str(), from, to);
                TorsoEnd("pilot input");
                return;
            }
        }

        // Where it should look.
        float wantYaw = cr[1], wantPitch = cr[0];
        bool pitchToo = false;
        float aim[3] = {};
        if (TorsoMode == TMO_Angle || TorsoMode == TMO_Centre)
            wantYaw = rot[1] + TorsoRel;
        else if (TorsoMode == TMO_Point)
        {
            std::memcpy(aim, TorsoPoint, sizeof(aim));
            pitchToo = true;
        }
        else if (TorsoMode == TMO_Target)
        {
            // Found live again every frame before it is touched: a destroyed target may be freed.
            if (!PawnClass)
                PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
            std::vector<API::UObject *> all;
            bool live = false;
            if (PawnClass && ActorsOf(pc, PawnClass, all))
                for (auto *a : all)
                    live |= (a == TorsoTgt);
            if (!live || IsDeadUnit(TorsoTgt) || !CallRet(TorsoTgt, L"K2_GetActorLocation", {}, aim, sizeof(aim)))
            {
                TorsoEnd("target gone");
                return;
            }
            pitchToo = true;
        }
        if (pitchToo)
        {
            float eye[3] = {here[0], here[1], here[2]};
            if (API::UObject *cam = ObjProp(pc, L"PlayerCameraManager"))
                CallRet(cam, L"GetCameraLocation", {}, eye, sizeof(eye));
            const float dx = aim[0] - eye[0], dy = aim[1] - eye[1], dz = aim[2] - eye[2];
            wantYaw = std::atan2(dy, dx) * 57.29578f;
            wantPitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.29578f;
        }

        // Inside the torso's own reach, at its own speed.
        float tw[2] = {};
        TwistBoundsV tb{};
        const bool twOk = TorsoRead(pawn, tw, &tb);
        float rel = Norm180(wantYaw - rot[1]);
        if (twOk && !tb.YawUnbound && tb.High[0] > tb.Low[0])
            rel = (std::min)((std::max)(rel, tb.Low[0]), tb.High[0]);
        wantYaw = rot[1] + rel;
        if (twOk && pitchToo && !tb.PitchUnbound && tb.High[1] > tb.Low[1])
            wantPitch = (std::min)((std::max)(wantPitch, tb.Low[1]), tb.High[1]);
        const float rateY = (twOk && tb.Rate[0] > 1.0f) ? tb.Rate[0] : 90.0f;
        const float rateP = (twOk && tb.Rate[1] > 1.0f) ? tb.Rate[1] : 60.0f;

        // WHERE THE TORSO POINTS. vanillahud-48's log: the control rotation equals the LEGS' yaw in
        // this game (31.0 / 31.0) and never moves with the torso - so every earlier progress check
        // measured the wrong thing. The mech's own aim rotation (MWMech::GetAimTargetRotation) is
        // legs + TorsoTwist (17.2 = 31.0 - 13.8): that is the torso, and what is measured now.
        float aimR[3] = {};
        uint8_t aimSel = 0;
        const bool aimOk = CallRet(pawn, L"GetAimTargetRotation", {{L"EMechAimTarget", &aimSel, 1}}, aimR, sizeof(aimR));
        const float curYaw = aimOk ? aimR[1] : rot[1] + tw[0];
        const float curPitch = aimOk ? aimR[0] : tw[1];
        const float errY = Norm180(wantYaw - curYaw);
        const float errP = pitchToo ? Norm180(wantPitch - curPitch) : 0.0f;

        // THE VIEW (the only way in: the earlier fall-backs - TorsoInput, SetFirstPersonControlYaw,
        // AddYawInput - fought it and were removed).
        {
            // THE VIEW. vanillahud-50: with the overlay up, setting even the twist's own target did
            // nothing - the torso never left -45.3. The torso follows the pilot's VIEW, the
            // MechViewComponent's LastRelativeViewRotator (pitch, yaw, roll relative to the mech):
            // MWVR steers the torso through exactly this field from the headset (HeadAim.cpp:
            // LastViewRotator = HeadTarget + TorsoRotation). The mouse moves it too - which is why,
            // with the game in UI-only input, nothing moved it and our target writes were thrown
            // away. So the view is set, stepped at the twist rate so the camera turns smoothly
            // instead of jumping, and the torso follows at its own rate.
            // vanillahud-52's probe settled it: the mouse writes MechViewComponent.
            // TargetRelativeViewRotator; CurrentRelativeViewRotator and LastRelativeViewRotator ease
            // toward it, and the torso twist follows them. Writing Last (vanillahud-51) was eased
            // straight back toward Target - the fight. The view keeps easing with the overlay up,
            // so setting TARGET turns the torso even with the game in UI-only input.
            API::UObject *vc = ObjProp(pawn, L"MechViewComponent");
            API::FProperty *vp = (vc && vc->get_class()) ? FindProp(vc->get_class(), L"TargetRelativeViewRotator") : nullptr;
            if (vp)
            {
                float *vr = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(vc) + vp->get_offset());
                vr[1] = Norm180(vr[1] + (std::min)((std::max)(Norm180(rel - vr[1]), -rateY * dt), rateY * dt));
                if (pitchToo)
                    vr[0] += (std::min)((std::max)(wantPitch - vr[0], -rateP * dt), rateP * dt);
                TorsoWrote[0] = vr[0];
                TorsoWrote[1] = vr[1];
                TorsoWroteOk = true;
            }
            else if (!TorsoViewWarned)
            {
                TorsoViewWarned = true;
                TacLog("tacops: torso - MechViewComponent.TargetRelativeViewRotator not found");
            }
        }

        // A STALL ends the order: still more than 5 degrees off and the torso has not moved a
        // degree in 3 s (the view is not being followed - something else holds the torso).
        // Judged on the torso's movement, not on the error, so tracking a target that crosses
        // as fast as the torso turns does not count as stuck.
        if (!TorsoProgSet)
        {
            TorsoProgSet = true;
            TorsoProgMs = now;
            TorsoProgYaw = curYaw;
        }
        else if (now - TorsoProgMs > 3000)
        {
            const bool moved = std::fabs(Norm180(curYaw - TorsoProgYaw)) > 1.0f;
            TorsoProgMs = now;
            TorsoProgYaw = curYaw;
            if (!moved && std::fabs(errY) > 5.0f)
            {
                TacLog("tacops: torso - not moving for 3 s, %.1f degrees off", errY);
                TorsoEnd("torso not moving");
                return;
            }
        }
        const float dY = errY;
        (void)errP;
        (void)aimOk;

        // Centre: done once it is there.
        if (TorsoMode == TMO_Centre && std::fabs(dY) < 1.5f && now - TorsoStartMs > 300)
            TorsoEnd("centred");
    }

    // A fall-back is over: the combat flag goes back to the AI, whose own service
    // (BTService_MoveWithRespectToCombatTarget) sets it from here on.
    void FomRelease(API::UObject *pawn, int s, const char *why)
    {
        if (!FomFallback[s] || !FomFlag || !pawn)
            return;
        API::UObject *ctl = ObjProp(pawn, L"Controller");
        if (!ctl || ctl->get_class() != FomFlagCls)
            return;
        reinterpret_cast<API::FBoolProperty *>(FomFlag)->set_value_in_object(ctl, false);
        TacLog("tacops: fall back, slot %d - %s, combat flag handed back (enemy handed back %d time(s))", s, why, FomKept[s]);
    }

    void FomTick(API::UObject *pc)
    {
        bool any = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            any |= FomOn[s];
        if (!any)
            return;
        // The slot -> unit map, fresh: a unit pointer from an earlier tick may be gone.
        TacRefreshMap(pc, GetTickCount64());
        const ULONGLONG now = GetTickCount64();
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            if (!FomOn[s])
                continue;
            API::UObject *pawn = TacUnit[s];
            float here[3] = {};
            if (!pawn || !CallRet(pawn, L"K2_GetActorLocation", {}, here, sizeof(here)) || now - FomStartMs[s] > 180000)
            {
                if (pawn)
                    FomRelease(pawn, s, "3 min");
                FomOn[s] = false;
                TacLog("tacops: %s, slot %d - ended (%s), flag forced %d time(s)", FomFallback[s] ? "fall back" : "fire on the move",
                        s, pawn ? "3 min" : "unit gone", FomForced[s]);
                continue;
            }
            const float dx = FomGoal[s][0] - here[0], dy = FomGoal[s][1] - here[1];
            const float dCm = std::sqrt(dx * dx + dy * dy);
            if (dCm < 2000.0f)
            {
                FomRelease(pawn, s, "arrived");
                FomOn[s] = false;
                TacLog("tacops: %s, slot %d - arrived after %.1f s, flag forced %d time(s)",
                        FomFallback[s] ? "fall back" : "fire on the move", s, (now - FomStartMs[s]) / 1000.0f, FomForced[s]);
                continue;
            }
            API::UObject *ctl = ObjProp(pawn, L"Controller");
            if (!ctl || !ctl->get_class())
                continue;
            if (FomFlagCls != ctl->get_class())
            {
                FomFlagCls = ctl->get_class();
                FomFlag = FindProp(ctl->get_class(), L"bMoveWithRespectToCombatTarget");
                if (FomFlag && PropType(FomFlag) != L"BoolProperty")
                    FomFlag = nullptr;
                TacLog("tacops: fire on the move - combat flag %s on %ls", FomFlag ? "found" : "NOT FOUND",
                        ClassName(ctl).c_str());
            }
            // Back on course when the enemy changes, or when progress stalls.
            API::UObject *tgt = ObjProp(ctl, L"CombatTarget");
            const char *why = nullptr;
            if (tgt && tgt != FomTarget[s] && FomTarget[s])
                why = "new enemy";
            FomTarget[s] = tgt ? tgt : FomTarget[s];
            if (FomProgMs[s] == 0 || dCm < FomBestCm[s] - 500.0f)
            {
                FomBestCm[s] = dCm;
                FomProgMs[s] = now;
            }
            else if (now - FomProgMs[s] > 4000)
                why = "no progress for 4 s";
            if (why)
            {
                uint8_t res = 0xFF;
                SendToPoint(ctl, FomGoal[s], &res);
                FomProgMs[s] = now;
                FomBestCm[s] = dCm;
                if (FomResent[s]++ < 30)
                    TacLog("tacops: %s, slot %d +%.1fs - %s, sent back to the point (%.0f m to go, result %u)",
                            FomFallback[s] ? "fall back" : "fire on the move", s, (now - FomStartMs[s]) / 1000.0f, why,
                            dCm * 0.01f, (unsigned)res);
            }
            if (!FomFlag)
                continue;
            auto *bp = reinterpret_cast<API::FBoolProperty *>(FomFlag);
            if (FomFallback[s])
            {
                if (API::UObject *cur = ObjProp(ctl, L"CombatTarget"))
                {
                    FomFace[s] = cur; // remembered by address only - never called on unless seen live again
                    if (now >= FomLosNextMs[s])
                    {
                        FomLosNextMs[s] = now + 500; // line-of-sight traces twice a second, not ten times
                        // Under insane allies its 3 s timer does the switching (vanillahud-87).
                        if (!IaSet[s] && SwitchIfHidden(pc, pawn, ctl, here, s, cur, FomLostMs[s], now, "fall back"))
                            FomFace[s] = ObjProp(ctl, L"CombatTarget");
                    }
                }
                else
                {
                    FomLostMs[s] = 0;
                    FomKeepTarget(pc, pawn, ctl, here, s, now);
                }
                // Only with an enemy to face; without one (none within 800 m) the flag is the AI's own.
                if (ObjProp(ctl, L"CombatTarget") && !bp->get_value_from_object(ctl))
                {
                    bp->set_value_in_object(ctl, true);
                    if (FomForced[s]++ < 20)
                        TacLog("tacops: fall back, slot %d +%.1fs - enemy in sight, combat flag forced on", s,
                                (now - FomStartMs[s]) / 1000.0f);
                }
            }
            else if (bp->get_value_from_object(ctl))
            {
                bp->set_value_in_object(ctl, false);
                if (FomForced[s]++ < 20)
                    TacLog("tacops: fire on the move, slot %d +%.1fs - AI set the combat flag, forced off", s,
                            (now - FomStartMs[s]) / 1000.0f);
            }
        }
    }

    // =====================================================================================
    // =====================================================================================
    // FLANK / PINCER (lancecontrol-75, user 2026-10-20). TCO_Flank: the lancemate swings round a unit to one side
    // of it and attacks it from there, FIRING THE WHOLE WAY - the fire-on-the-move walk (FomOn), its next leg
    // re-aimed every 2.5 s (or sooner when the leg is nearly walked). The legs are steered so the target is never
    // more than the mech's own torso twist limit (TorsoStats yaw bounds - 5 deg; the chassis range 60..150, see
    // out\torso_twist_limits.tsv) off its heading, so the torso - and the guns - can stay on it all the way:
    //   far out   -> heading 50 deg off the line to the target, easing to the limit (or 90) at the stand-off ring;
    //   on it     -> 90 deg (circling), or the limit when that is less (a gentle inward drift);
    //   too close -> past 90 deg, up to the limit, while it swings round: mechs that can twist past 90 open up as they
    //   go (walking forward, torso back on the target). lancecontrol-80 (user 2026-10-20): the ANGLE comes first - it
    //   never backs off (the low-twist backstep is gone) and, once on its side, holds there at whatever distance.
    // Stand-off: the distance (150-600 m) where the most of its working, loaded weapons' damage per second is in
    // range (full to OptimalRange, falling to nothing at MaxRange; lock-on missiles nothing under 180 m), worked
    // out again every 5 s - or the order's own (Pad[1] >> 1, x 10 m). Side (Pad[0] & 7): 0 the nearer of the
    // target's own sides, 1 / 2 OUR left / right (the line you -> target), 3 / 4 ITS left / right (its facing), 5
    // behind it. Pad[0] & 8: one arm of a pincer (shown as such). Pad[1] & 1: the target is your lock.
    // In position (within 12 deg of the side, near the ring) -> an attack order on the unit. Ends on any other
    // order, the target's death or loss (10 s unseen -> the attack order, which handles that), 120 s (-> attack),
    // or a new mission. Only the order itself is announced (LSE_OrderReceived, Value TCO_Flank); its legs are not.
    // =====================================================================================
    bool          FlOn[VanillaExportBlock::kTacMaxMembers + 1] = {};
    API::UObject *FlTgt[VanillaExportBlock::kTacMaxMembers + 1] = {};
    uint8_t       FlSide[VanillaExportBlock::kTacMaxMembers + 1] = {};
    bool          FlPincer[VanillaExportBlock::kTacMaxMembers + 1] = {};
    float         FlStandCm[VanillaExportBlock::kTacMaxMembers + 1] = {};
    bool          FlStandOrd[VanillaExportBlock::kTacMaxMembers + 1] = {};
    float         FlMinCm[VanillaExportBlock::kTacMaxMembers + 1] = {};
    float         FlLimit[VanillaExportBlock::kTacMaxMembers + 1] = {};
    int           FlDir[VanillaExportBlock::kTacMaxMembers + 1] = {};
    bool          FlBack[VanillaExportBlock::kTacMaxMembers + 1] = {};
    ULONGLONG     FlStartMs[VanillaExportBlock::kTacMaxMembers + 1] = {}, FlNextMs[VanillaExportBlock::kTacMaxMembers + 1] = {},
                  FlStandMs[VanillaExportBlock::kTacMaxMembers + 1] = {}, FlLegMs[VanillaExportBlock::kTacMaxMembers + 1] = {},
                  FlSeenMs[VanillaExportBlock::kTacMaxMembers + 1] = {}, FlBackMs[VanillaExportBlock::kTacMaxMembers + 1] = {};
    int           FlLegs[VanillaExportBlock::kTacMaxMembers + 1] = {};
    // lancecontrol-76 (user, 2026-10-20 test): the side's bearing is SMOOTHED (a target looking / turning about must not
    // swing the goal - the test saw 9 -> 58 deg to go in 3 s) and "in position" must hold for 3 checks in a row (2.25 s)
    // - an incidental line-up as the target turns does not count. Auto side is chosen once, at the order.
    float         FlGoalX[VanillaExportBlock::kTacMaxMembers + 1] = {}, FlGoalY[VanillaExportBlock::kTacMaxMembers + 1] = {};
    bool          FlGoalSet[VanillaExportBlock::kTacMaxMembers + 1] = {};
    int           FlInPos[VanillaExportBlock::kTacMaxMembers + 1] = {};
    // lancecontrol-78 (user, 2026-10-20): IN POSITION IS HELD - no hand-over to an attack order. It stands and fights on
    // its side; when the side moves (you strafe, the target turns or walks) out of the box it swings round again. Until
    // another order, the target's death, or 10 s with nobody seeing it. No 120 s limit.
    bool          FlHeld[VanillaExportBlock::kTacMaxMembers + 1] = {};
    bool          FlInternal = false; // TacPost is running one of the flank's own orders (no announcement, no end)

    struct FlWpnStat
    {
        float Opt = 0.0f, Max = 0.0f, Dps = 0.0f;
        bool  Lock = false, Ok = false;
    };
    std::vector<std::pair<API::UClass *, FlWpnStat>> FlWpnCache;

    static float FlWrap(float a)
    {
        while (a > 180.0f)
            a -= 360.0f;
        while (a < -180.0f)
            a += 360.0f;
        return a;
    }

    FlWpnStat FlWeapon(API::UObject *w)
    {
        for (auto &e : FlWpnCache)
            if (e.first == w->get_class())
                return e.second;
        FlWpnStat st;
        API::UObject *da = nullptr;
        if (FindFn(w, L"GetWeaponDataAsset"))
            CallRet(w, L"GetWeaponDataAsset", {}, &da, sizeof(da));
        if (da && da->get_class())
        {
            uint8_t *b = reinterpret_cast<uint8_t *>(da);
            API::UStruct *cls = da->get_class();
            std::wstring path;
            float opt = 0.0f, mx = 0.0f, dmg = 0.0f, n = 0.0f, t = 0.0f, cd = 0.0f;
            WsFindNum(cls, b, 0, L"OptimalRange", opt);
            WsFindRange(cls, b, 0, false, mx, path);
            WsFindNum(cls, b, 0, L"Damage", dmg);
            WsFindNum(cls, b, 0, L"NumberOfProjectiles", n);
            WsFindNum(cls, b, 0, L"NumberOfTimesToFire", t);
            WsFindNum(cls, b, 0, L"Cooldown", cd);
            if (mx > 0.0f)
            {
                st.Max = mx;
                st.Opt = (opt > 0.0f && opt <= mx) ? opt : mx;
                st.Dps = (dmg > 0.0f ? dmg : 1.0f) * (std::max)(1.0f, n) * (std::max)(1.0f, t) / (std::max)(cd, 0.5f);
                const wchar_t *how = L"";
                st.Lock = IcLockWeapon(w, how) == 1;
                st.Ok = true;
            }
            TacLog("tacops: flank - %ls: optimal %.0f m, max %.0f m, %.1f dmg/s%s", ClassName(w).c_str(), st.Opt * 0.01f,
                   st.Max * 0.01f, st.Dps, st.Lock ? " (lock-on)" : "");
        }
        FlWpnCache.push_back({w->get_class(), st});
        return st;
    }

    // The stand-off: where the most of its working, loaded weapons' damage per second is in range.
    float FlStandoffCm(int s, API::UObject *pawn, float &minCm)
    {
        std::vector<FlWpnStat> ws;
        float all = 0.0f, lockDps = 0.0f;
        for (auto &e : LsWpns[s])
        {
            if (!e.W || e.State == LSW_Destroyed || e.State == LSW_NoAmmo || !OnMech(pawn, e.W))
                continue;
            const FlWpnStat st = FlWeapon(e.W);
            if (!st.Ok)
                continue;
            ws.push_back(st);
            all += st.Dps;
            if (st.Lock)
                lockDps += st.Dps;
        }
        minCm = (all > 0.0f && lockDps > all * 0.5f) ? 18000.0f : 10000.0f;
        if (ws.empty())
            return 30000.0f;
        float best = -1.0f, bestD = 30000.0f;
        for (float d = 15000.0f; d <= 60001.0f; d += 2500.0f)
        {
            float sc = 0.0f;
            for (auto &w : ws)
            {
                float f = d <= w.Opt ? 1.0f : (d >= w.Max ? 0.0f : 1.0f - (d - w.Opt) / (w.Max - w.Opt));
                if (w.Lock && d < 18000.0f)
                    f = 0.0f;
                sc += w.Dps * f;
            }
            if (sc > best * 1.02f)
            {
                best = sc;
                bestD = d;
            }
            else if (sc >= best * 0.98f)
                bestD = d; // as good, further out: further out
        }
        return (std::max)(bestD, minCm + 3000.0f);
    }

    // The side's bearing FROM the target, world yaw (deg). UE yaw: +90 is to the right of yaw 0.
    float FlGoalYaw(int s, const float *T, float tyaw, bool haveYaw, const float *you, float theta)
    {
        const int side = FlSide[s];
        float line = tyaw; // the line you -> target
        const float ux = T[0] - you[0], uy = T[1] - you[1];
        if (ux * ux + uy * uy > 100.0f * 100.0f)
            line = std::atan2(uy, ux) * 57.2957795f;
        if (side == 1 || side == 2)
            return FlWrap(line + (side == 1 ? -90.0f : 90.0f));
        const float face = haveYaw ? tyaw : line;
        if (side == 3 || side == 4)
            return FlWrap(face + (side == 3 ? -90.0f : 90.0f));
        if (side == 5)
            return FlWrap(face + 180.0f);
        const float a = FlWrap(face - 90.0f), b = FlWrap(face + 90.0f); // auto: the nearer of its own sides
        return std::fabs(FlWrap(a - theta)) <= std::fabs(FlWrap(b - theta)) ? a : b;
    }

    // The next leg's end: from P, at `beta` deg off the line to the target, `len` cm, on walkable ground - else
    // turned toward the target in 10 deg steps (still inside the twist limit), then shortened.
    bool FlLegPoint(API::UObject *pc, const float *P, float theta, int dir, float beta, float len, float *out)
    {
        for (int k = 0; k < 6; ++k)
        {
            const float bt = beta - k * 10.0f;
            if (bt < 0.0f)
                break;
            const float h = (theta + 180.0f - dir * bt) / 57.2957795f;
            for (float L = len; L >= 2000.0f; L *= 0.6f)
            {
                float p[3] = {P[0] + std::cos(h) * L, P[1] + std::sin(h) * L, P[2]};
                if (TacOnNavMesh(pc, p, true))
                {
                    std::memcpy(out, p, sizeof(p));
                    return true;
                }
            }
        }
        return false;
    }

    void FlEnd(int s, const char *why)
    {
        if (!FlOn[s])
            return;
        FlOn[s] = false;
        if (FlBack[s])
        {
            FomRelease(TacUnit[s], s, "flank over");
            FomFallback[s] = false;
            FlBack[s] = false;
        }
        if (AtkTgt[s] == FlTgt[s] && !AtkSinceMs[s])
            AtkTgt[s] = nullptr;
        TacLog("tacops: flank, slot %d - ended (%s) after %.1f s, %d leg(s)", s, why, (GetTickCount64() - FlStartMs[s]) / 1000.0f, FlLegs[s]);
    }

    // In position (or out of time): an attack order on the unit itself.
    void FlAttack(API::UObject *pc, int s, const char *why)
    {
        API::UObject *tgt = FlTgt[s];
        FlEnd(s, why);
        if (!tgt || !IsLive(tgt) || IsDeadUnit(tgt))
            return;
        VanillaExportBlock::TacCmd a = {};
        a.Order = VanillaExportBlock::TCO_Attack;
        a.Slot = (uint8_t)s;
        const uint64_t v = (uint64_t)(uintptr_t)tgt;
        const uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
        std::memcpy(&a.Loc[0], &lo, 4);
        std::memcpy(&a.Loc[1], &hi, 4);
        a.Loc[2] = VanillaExportBlock::kTacLocActor;
        FlInternal = true;
        const bool ok = TacPost(pc, a);
        FlInternal = false;
        TacLog("tacops: flank, slot %d - %s: attacking %ls (%s)", s, why, ClassName(tgt).c_str(), ok ? "sent" : "FAILED");
    }

    // Its next leg as the fire-on-the-move point.
    void FlSendLeg(API::UObject *ctl, int s, const float *W, ULONGLONG now)
    {
        if (!FomOn[s])
        {
            FomOn[s] = true;
            FomFallback[s] = false;
            FomStartMs[s] = now;
            FomForced[s] = 0;
            FomTarget[s] = nullptr;
            FomFace[s] = nullptr;
            FomFaceMs[s] = FomLostMs[s] = FomLosNextMs[s] = 0;
            FomKept[s] = FomResent[s] = 0;
        }
        std::memcpy(FomGoal[s], W, sizeof(float) * 3);
        std::memcpy(TacLastMove[s], W, sizeof(float) * 3);
        FomProgMs[s] = 0;
        uint8_t res = 0xFF;
        SendToPoint(ctl, W, &res);
        FlLegMs[s] = now;
        ++FlLegs[s];
    }

    bool FlankOrder(API::UObject *pc, const VanillaExportBlock::TacCmd &c)
    {
        const int s = c.Slot;
        const ULONGLONG now = GetTickCount64();
        TacRefreshMap(pc, now);
        API::UObject *me = API::get()->get_local_pawn(0);
        API::UObject *pawn = TacUnit[s];
        if (!pawn || !IsLive(pawn) || IsDeadUnit(pawn))
        {
            TacLog("tacops: flank for slot %d - no lancemate in that slot", s);
            return false;
        }
        API::UObject *tgt = nullptr;
        if (c.Pad[1] & 1)
        {
            API::UObject *tt = me ? ObjProp(me, L"TargetTracking") : nullptr;
            tgt = tt ? ObjProp(tt, L"CurrentTarget") : nullptr;
        }
        else
            tgt = TacTargetOf(pc, c.Loc);
        if (!tgt || tgt == pawn || !IsLive(tgt) || IsDeadUnit(tgt))
        {
            TacLog("tacops: flank for slot %d - no target (%s)", s, (c.Pad[1] & 1) ? "you have no lock" : "nothing there");
            return false;
        }
        float P[3] = {}, T[3] = {}, rot[3] = {}, you[3] = {};
        if (!CallRet(pawn, L"K2_GetActorLocation", {}, P, sizeof(P)) || !CallRet(tgt, L"K2_GetActorLocation", {}, T, sizeof(T)))
            return false;
        const bool haveYaw = CallRet(tgt, L"K2_GetActorRotation", {}, rot, sizeof(rot));
        if (!me || !CallRet(me, L"K2_GetActorLocation", {}, you, sizeof(you)))
            std::memcpy(you, P, sizeof(you));
        if (FlOn[s])
            FlEnd(s, "replaced by a new flank");
        FlTgt[s] = tgt;
        FlSide[s] = (uint8_t)(c.Pad[0] & 7);
        FlPincer[s] = (c.Pad[0] & 8) != 0;
        float mn = 10000.0f;
        const float autoCm = FlStandoffCm(s, pawn, mn);
        const int said = (c.Pad[1] >> 1) & 0x7F;
        FlStandOrd[s] = said > 0;
        FlMinCm[s] = mn;
        FlStandCm[s] = said > 0 ? (std::max)(said * 1000.0f, mn) : autoCm;
        float tw[2] = {};
        TwistBoundsV tb = {};
        float lim = 90.0f;
        const bool twOk = TorsoRead(pawn, tw, &tb);
        if (twOk)
            lim = tb.YawUnbound ? 180.0f : (std::min)(std::fabs(tb.Low[0]), std::fabs(tb.High[0]));
        if (!(lim > 10.0f && lim <= 360.0f))
            lim = 90.0f;
        FlLimit[s] = (std::min)(lim, 175.0f) - 5.0f;
        const float dx = P[0] - T[0], dy = P[1] - T[1];
        const float theta = std::atan2(dy, dx) * 57.2957795f, r = std::sqrt(dx * dx + dy * dy);
        const float goal = FlGoalYaw(s, T, rot[1], haveYaw, you, theta);
        if (FlSide[s] == 0) // auto: the nearer of its own sides NOW - kept for the whole flank (lancecontrol-76)
            FlSide[s] = (haveYaw && FlWrap(goal - rot[1]) < 0.0f) ? 3 : 4;
        FlDir[s] = FlWrap(goal - theta) >= 0.0f ? 1 : -1;
        FlGoalX[s] = std::cos(goal / 57.2957795f);
        FlGoalY[s] = std::sin(goal / 57.2957795f);
        FlGoalSet[s] = true;
        FlInPos[s] = 0;
        FlHeld[s] = false;
        // The first leg goes out as an ordinary fire-on-the-move order (ending whatever it was doing).
        float W[3] = {};
        if (!FlLegPoint(pc, P, theta, FlDir[s], (std::min)(FlLimit[s], 60.0f), (std::max)(3000.0f, (std::min)(r * 0.35f, 8000.0f)), W))
        {
            TacLog("tacops: flank for slot %d - no walkable ground to swing round on", s);
            return false;
        }
        VanillaExportBlock::TacCmd m = c;
        m.Order = VanillaExportBlock::TCO_MoveFire;
        m.Pad[0] = m.Pad[1] = 0;
        std::memcpy(m.Loc, W, sizeof(W));
        FlInternal = true;
        const bool ok = TacPost(pc, m);
        FlInternal = false;
        if (!ok)
            return false;
        FlOn[s] = true;
        FlBack[s] = false;
        FlStartMs[s] = FlStandMs[s] = FlLegMs[s] = FlSeenMs[s] = now;
        FlNextMs[s] = now + 750;
        FlLegs[s] = 1;
        AtkTgt[s] = tgt; // insane allies: no switching away while it lives
        AtkSinceMs[s] = 0; // (the attack-order watch stays off - the flank watches its own target)
        if (API::UObject *ctl = ObjProp(pawn, L"Controller"))
            if (ResolveSetTarget(ctl))
            {
                API::UObject *aw = nullptr;
                bool kn = false;
                GiveTarget(ctl, pawn, tgt, aw, kn);
            }
        LsEmit(LSE_OrderReceived, s, tgt, (float)VanillaExportBlock::TCO_Flank);
        static const char *kSide[8] = {"the nearer side", "your left", "your right", "its left", "its right", "behind it", "?", "?"};
        TacLog("tacops: FLANK%s -> slot %d on %ls, %s; %.0f m away, stand-off %.0f m (%s), hard minimum %.0f m, twist limit %.0f "
               "(usable %.0f%s), going %s",
               FlPincer[s] ? " (PINCER)" : "", s, ClassName(tgt).c_str(), kSide[FlSide[s]], r * 0.01f, FlStandCm[s] * 0.01f,
               said > 0 ? "ordered" : "most damage in range", FlMinCm[s] * 0.01f, lim, FlLimit[s], twOk ? "" : ", NOT READ - 90 assumed",
               FlDir[s] > 0 ? "anticlockwise" : "clockwise");
        return true;
    }

    void FlankTick(API::UObject *pc)
    {
        bool any = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            any |= FlOn[s];
        if (!any)
            return;
        const ULONGLONG now = GetTickCount64();
        TacRefreshMap(pc, now);
        API::UObject *me = API::get()->get_local_pawn(0);
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            if (!FlOn[s] || now < FlNextMs[s])
                continue;
            FlNextMs[s] = now + 750;
            API::UObject *pawn = TacUnit[s], *tgt = FlTgt[s];
            if (!pawn || !IsLive(pawn) || IsDeadUnit(pawn))
            {
                FlEnd(s, "lancemate gone");
                continue;
            }
            if (!tgt || !IsLive(tgt) || IsDeadUnit(tgt))
            {
                FlEnd(s, "target down or gone");
                continue;
            }
            API::UObject *ctl = ObjProp(pawn, L"Controller");
            if (!ctl)
                continue;
            // Lost: no lancemate has seen it for 10 s - the attack order takes over (it drops it the same way).
            bool seen = false;
            for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers && !seen; ++k)
                if (TacUnit[k] && IsLive(TacUnit[k]) && !IsDeadUnit(TacUnit[k]))
                    if (API::UObject *c2 = ObjProp(TacUnit[k], L"Controller"))
                        seen = AiGameVisible(c2, tgt) == 1;
            if (seen)
                FlSeenMs[s] = now;
            else if (now - FlSeenMs[s] > 10000)
            {
                FlAttack(pc, s, "lost sight of it for 10 s");
                continue;
            }
            float P[3] = {}, T[3] = {}, rot[3] = {}, you[3] = {};
            if (!CallRet(pawn, L"K2_GetActorLocation", {}, P, sizeof(P)) || !CallRet(tgt, L"K2_GetActorLocation", {}, T, sizeof(T)))
                continue;
            const bool haveYaw = CallRet(tgt, L"K2_GetActorRotation", {}, rot, sizeof(rot));
            if (!me || !CallRet(me, L"K2_GetActorLocation", {}, you, sizeof(you)))
                std::memcpy(you, P, sizeof(you));
            if (!FlStandOrd[s] && now - FlStandMs[s] > 5000)
            {
                FlStandMs[s] = now;
                float mn = FlMinCm[s];
                const float d = FlStandoffCm(s, pawn, mn);
                if (std::fabs(d - FlStandCm[s]) > 2000.0f)
                    TacLog("tacops: flank, slot %d - stand-off now %.0f m (was %.0f m)", s, d * 0.01f, FlStandCm[s] * 0.01f);
                FlStandCm[s] = d;
                FlMinCm[s] = mn;
            }
            // Its target, kept: the flank's unit is its combat target the whole way.
            if (ObjProp(ctl, L"CombatTarget") != tgt && ResolveSetTarget(ctl))
            {
                API::UObject *aw = nullptr;
                bool kn = false;
                GiveTarget(ctl, pawn, tgt, aw, kn);
            }
            AtkTgt[s] = tgt;
            const float dx = P[0] - T[0], dy = P[1] - T[1];
            const float r = std::sqrt(dx * dx + dy * dy), theta = std::atan2(dy, dx) * 57.2957795f;
            // the side's bearing, smoothed (time constant ~4 s at one check per 750 ms)
            float goal = FlGoalYaw(s, T, rot[1], haveYaw, you, theta);
            {
                const float gx = std::cos(goal / 57.2957795f), gy = std::sin(goal / 57.2957795f);
                if (!FlGoalSet[s])
                {
                    FlGoalX[s] = gx;
                    FlGoalY[s] = gy;
                    FlGoalSet[s] = true;
                }
                else
                {
                    FlGoalX[s] += (gx - FlGoalX[s]) * 0.18f;
                    FlGoalY[s] += (gy - FlGoalY[s]) * 0.18f;
                }
                if (FlGoalX[s] * FlGoalX[s] + FlGoalY[s] * FlGoalY[s] > 1e-4f)
                    goal = std::atan2(FlGoalY[s], FlGoalX[s]) * 57.2957795f;
            }
            float rem = FlWrap(goal - theta) * FlDir[s];
            if (rem < -20.0f) // overshot, or the target turned: the short way round from here
            {
                FlDir[s] = FlWrap(goal - theta) >= 0.0f ? 1 : -1;
                rem = std::fabs(FlWrap(goal - theta));
            }
            rem = std::fabs(rem);
            const float rd = FlStandCm[s], lim = FlLimit[s], ratio = r / (std::max)(rd, 1000.0f);
            // In position - or as far round as a mech that cannot open up will get without crowding it.
            // ... for 3 checks in a row (2.25 s): a line-up while the target turns about is not "in position"
            // lancecontrol-80 (user, 2026-10-20): THE ANGLE COMES FIRST, NOT THE DISTANCE - in position at any distance inside
            // 1.3x the stand-off; nobody backs off or opens up to keep the stand-off (the stand-off only shapes the approach).
            const bool inBox = (rem < 12.0f && ratio < 1.3f) || (lim < 91.0f && ratio < 0.75f && rem < 45.0f);
            if (FlHeld[s])
            {
                // Holding its side: stays while the side is still about here (wider box - no fidgeting), else swings again.
                const bool stillOk = (rem < 25.0f && ratio < 1.45f) || (lim < 91.0f && ratio < 0.85f && rem < 55.0f);
                if (stillOk)
                    continue;
                FlHeld[s] = false;
                FlInPos[s] = 0;
                FlLegMs[s] = 0; // a fresh leg now
                TacLog("tacops: flank, slot %d +%.1fs - the side moved (%.0f deg to go, %.0f m out): swinging round again", s,
                       (now - FlStartMs[s]) / 1000.0f, rem, r * 0.01f);
            }
            else if (inBox)
            {
                if (++FlInPos[s] >= 3)
                {
                    FlHeld[s] = true;
                    uint8_t res = 0xFF;
                    SendToPoint(ctl, P, &res); // stop here and fight (fire on the move stays armed, its point = here)
                    std::memcpy(FomGoal[s], P, sizeof(P));
                    std::memcpy(TacLastMove[s], P, sizeof(P));
                    TacLog("tacops: flank, slot %d +%.1fs - in position (%.0f deg off the side, %.0f m out): holding it", s,
                           (now - FlStartMs[s]) / 1000.0f, rem, r * 0.01f);
                    continue;
                }
            }
            else
                FlInPos[s] = 0;
            // (lancecontrol-80: the emergency backstep for low-twist mechs is gone - no backing up; LRM boats are not meant
            // to be told to flank.)
            // The angle off the line to the target, eased by distance (never past the twist limit).
            const float lim90 = (std::min)(lim, 90.0f);
            float beta;
            if (ratio > 1.15f)
            {
                const float k = (std::max)(0.0f, (std::min)(1.0f, (2.0f - ratio) / 0.85f));
                beta = lim90 < 50.0f ? lim90 : 50.0f + (lim90 - 50.0f) * k;
            }
            else if (ratio >= 0.9f)
                beta = lim90;
            else // inside the ring, still swinging round: a mech that can twist past 90 opens up as it goes (walking forward,
                 // torso back on the target - never reversing); one that cannot circles at the distance it has
                beta = (std::min)(lim, 90.0f + 40.0f * (std::max)(0.0f, (std::min)(1.0f, (0.9f - ratio) / 0.4f)));
            if (rem < 25.0f && ratio > 1.0f)
                beta = (std::min)(beta, 60.0f); // nearly round: head in for the side itself
            // lancecontrol-76 (user, 2026-10-20): THE WIDE HOOK. Outside the ring with 30+ deg to go: straight for a point
            // on the side at 1.3x the stand-off (it stays outside the ring all the way - no spiral creeping round), the
            // target kept inside the twist limit (else the limit). Within 30 deg / at the ring: the rules above.
            float hookCm = 0.0f;
            if (ratio > 1.0f && rem >= 30.0f)
            {
                // the aim point's bearing: the side itself, or a quarter turn round when it is further (a side straight
                // behind it would put the line through the target)
                const float g = (theta + FlDir[s] * (std::min)(rem, 90.0f)) / 57.2957795f;
                const float ax = T[0] + std::cos(g) * rd * 1.3f, ay = T[1] + std::sin(g) * rd * 1.3f;
                const float hA = std::atan2(ay - P[1], ax - P[0]) * 57.2957795f;
                float b = -FlWrap(hA - (theta + 180.0f)) * FlDir[s]; // off the line to the target, toward the side
                // lancecontrol-77 (user, 2026-10-20 test 2): from far out the straight line pointed mostly AT the target
                // (28-46 deg off it) - it closed first and swung late. Well outside the ring (1.5x+): at least 60 deg off
                // the line (or its twist limit, if less) - sideways at once, turning in near the ring.
                if (ratio > 1.5f)
                    b = (std::max)(b, (std::min)(75.0f, lim)); // lancecontrol-78: 75 (was 60) - sideways first
                b = (std::max)(0.0f, (std::min)(b, lim));
                beta = b;
                hookCm = std::sqrt((ax - P[0]) * (ax - P[0]) + (ay - P[1]) * (ay - P[1]));
            }
            const float gx = FomGoal[s][0] - P[0], gy = FomGoal[s][1] - P[1];
            if (FomOn[s] && FlLegMs[s] && now - FlLegMs[s] < 2500 && gx * gx + gy * gy > 3500.0f * 3500.0f)
                continue; // the current leg is still good
            float len = (std::max)(3000.0f, (std::min)(r * 0.35f, 8000.0f));
            if (hookCm > 0.0f)
                len = (std::max)(3000.0f, (std::min)(hookCm, 8000.0f));
            float W[3] = {};
            if (!FlLegPoint(pc, P, theta, FlDir[s], beta, len, W))
            {
                if (FlLegs[s] < 200)
                    TacLog("tacops: flank, slot %d - no walkable ground for the next leg (%.0f m out, %.0f deg to go)", s, r * 0.01f, rem);
                continue;
            }
            FlSendLeg(ctl, s, W, now);
            if (FlLegs[s] <= 60)
                TacLog("tacops: flank, slot %d +%.1fs - %.0f m out (stand-off %.0f), %.0f deg to go, heading %.0f deg off it%s", s,
                       (now - FlStartMs[s]) / 1000.0f, r * 0.01f, rd * 0.01f, rem, beta, hookCm > 0.0f ? " (hook)" : "");
        }
    }

    // HOLD POSITION, INDIRECT FIRE ONLY (TCO_IndirectCover; vanillahud-70, cover seeking removed in
    // 76). The lancemate walks to the point and holds there (Wait). For the whole order the game's
    // own firing is suppressed and the plugin fires its lock-on weapons (IcOwnFire: lock, the
    // weapon's own fire check, 900 m, heat) - on the way as well as at the point. If ANY
    // (non-VTOL) enemy sees it - by that enemy's own visible list - it is EXPOSED: the game's
    // firing comes back on for every weapon and its target becomes the nearest enemy seeing it,
    // until nobody has seen it for 2 s. A target beyond LRM reach is swapped for a known enemy in
    // reach. No timer: it runs until another order, or the lancemate dies.
    // =====================================================================================
    enum IcPhaseE { IC_Move = 0, IC_Engage };

    static constexpr float kIcMinRangeCm = 20000.0f; // LRM minimum range and a little

    static constexpr float kIcMaxRangeCm = 90000.0f;  // LRM maximum range (the AI's weapon records: 900 m)

    bool      IcOn[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       IcPhase[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG IcStartMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int       IcSeenCount[VanillaExportBlock::kTacMaxMembers + 1] = {};

    bool      IcInternal = false;

    bool      HoldIndirect[VanillaExportBlock::kTacMaxMembers + 1] = {}; // a hold that leaves targeting to the game

    bool      IcHoldNext = false; // the hold TacPost is about to arm is one of ours

    ULONGLONG IcTickMs = 0;

    bool IcVisChecked = false;

    bool      IcSpotter[VanillaExportBlock::kTacMaxMembers + 1] = {};   // no cover: fighting the spotters, direct fire allowed

    ULONGLONG IcUnseenMs[VanillaExportBlock::kTacMaxMembers + 1] = {};  // when it was last seen (spotter mode ends 2 s after)

    ULONGLONG IcReachMs[VanillaExportBlock::kTacMaxMembers + 1] = {};   // target-reach check, every 2 s

    ULONGLONG IcSpotMs[VanillaExportBlock::kTacMaxMembers + 1] = {};    // spotter targeting, at most once a second

    // The nearest enemy that sees this lancemate (by the enemies' own visible lists) becomes its
    // combat target - unless its target already is one of them.
    API::UObject *IcTargetSpotter(int s, API::UObject *pawn, API::UObject *ctl, const float *here,
                                  const std::vector<API::UObject *> &seers, API::UObject *tgt, bool &set)
    {
        set = false;
        if (std::find(seers.begin(), seers.end(), tgt) != seers.end())
            return tgt;
        API::UObject *nearest = nullptr;
        float dn = 1e30f;
        for (auto *e : seers)
        {
            float p[3] = {};
            if (!CallRet(e, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            const float d = (p[0] - here[0]) * (p[0] - here[0]) + (p[1] - here[1]) * (p[1] - here[1]);
            if (d < dn)
            {
                dn = d;
                nearest = e;
            }
        }
        if (nearest && ctl && ResolveSetTarget(ctl))
        {
            API::UObject *aware = nullptr;
            bool known = false;
            set = GiveTarget(ctl, pawn, nearest, aware, known);
        }
        return nearest;
    }

    // COMBAT UNITS ONLY (vanillahud-101): the plugin hands lancemates targets from what they know,
    // and the game's lists also carry non-units on the enemy team - an invisible bombardment area
    // (invisiblebombarea_C) was handed over and held for 86 s. A target the plugin picks must be a
    // mech, tank, turret or VTOL: MWMech / MWTank / MWTurret / MWVTOL in its class chain (the
    // bases LidarExport sorts units by). Decided once per class; a class turned away is logged once.
    std::vector<std::pair<API::UClass *, bool>> UnitClassCache;

    bool IsCombatUnit(API::UObject *a)
    {
        if (!a || !a->get_class())
            return false;
        API::UClass *c = a->get_class();
        for (auto &e : UnitClassCache)
            if (e.first == c)
                return e.second;
        bool unit = false;
        std::wstring chain;
        for (auto *st = static_cast<API::UStruct *>(c); st; st = st->get_super_struct())
        {
            const std::wstring n = st->get_fname()->to_string();
            chain += (chain.empty() ? L"" : L" < ") + n;
            // lancecontrol-64 (user, 2026-10-17): battle armour too - KelClanElemental is one suit (Clan Elemental / IS battle
            // armour); the squad (KelClanElementalGroup) and infantry (MWInfantry) stay out
            if (n == L"MWMech" || n == L"MWTank" || n == L"MWTurret" || n == L"MWVTOL" || n == L"KelClanElemental")
                unit = true;
            if (n == L"Pawn" || n == L"Actor")
                break;
        }
        UnitClassCache.push_back({c, unit});
        if (!unit)
            TacLog("target filter: %ls is not a combat unit (%ls) - never handed to a lancemate", ClassName(a).c_str(), chain.c_str());
        return unit;
    }

    // lancecontrol-64: a battle armour suit = KelClanElemental in its class chain (answered once per class).
    std::vector<std::pair<API::UClass *, bool>> BaClassCache;

    bool IsBattleArmor(API::UObject *a)
    {
        if (!a || !a->get_class())
            return false;
        API::UClass *c = a->get_class();
        for (auto &e : BaClassCache)
            if (e.first == c)
                return e.second;
        bool ba = false;
        for (auto *st = static_cast<API::UStruct *>(c); st && !ba; st = st->get_super_struct())
        {
            const std::wstring n = st->get_fname()->to_string();
            if (n == L"KelClanElemental")
                ba = true;
            if (n == L"Pawn" || n == L"Actor")
                break;
        }
        BaClassCache.push_back({c, ba});
        return ba;
    }

    // lancecontrol-42: a mech = its class derives from MWMech (vehicles, turrets and VTOLs do not).
    static bool IsMechUnit(API::UObject *a)
    {
        if (!a || !a->get_class())
            return false;
        for (API::UStruct *st = a->get_class(); st; st = st->get_super_struct())
            if (st->get_fname() && st->get_fname()->to_string() == L"MWMech")
                return true;
        return false;
    }

    static bool IsVtol(API::UObject *a)
    {
        const std::wstring n = ClassName(a);
        return n.find(L"VTOL") != std::wstring::npos || n.find(L"Vtol") != std::wstring::npos;
    }

    // The units this lancemate's own tracker knows (ActorRelations), for choosing a target in reach.
    std::vector<API::UObject *> KnownActors(API::UObject *pawn)
    {
        std::vector<API::UObject *> out;
        API::UObject *tt = pawn ? ObjProp(pawn, L"TargetTracking") : nullptr;
        if (!tt)
            return out;
        uint8_t *data = nullptr;
        int32_t num = 0, stride = 0;
        API::UScriptStruct *es = nullptr;
        std::wstring an;
        if (!FpFindArray(tt->get_class(), reinterpret_cast<uint8_t *>(tt), L"ActorRelationInfo", nullptr, data, num, es, stride, an))
            return out;
        auto *ta = StructProp(es, L"TargetActor");
        if (!ta || PropType(ta) != L"ObjectProperty")
            return out;
        for (int32_t i = 0; data && i < num && i < 256; ++i)
            if (auto *a = *reinterpret_cast<API::UObject **>(data + (size_t)i * stride + ta->get_offset()))
                out.push_back(a);
        return out;
    }

    // TARGET IN REACH (E). A lancemate holding for indirect fire whose combat target is beyond LRM
    // reach (the game happily picks relayed targets 1.9 km away and then never fires) is handed
    // the nearest enemy it knows within reach instead - at least the LRM minimum range if there is
    // one, else the nearest within reach. Only enemies found in this tick's actor list are used.
    bool TargetInReach(API::UObject *pc, int s, API::UObject *pawn, API::UObject *ctl, const float *here, API::UObject *cur,
                       const char *who)
    {
        float tp[3] = {};
        const float reach = IcReachCm(s, pawn); // vanillahud-97: the lancemate's own lock-on weapons
        if (cur && !IsDeadUnit(cur) && CallRet(cur, L"K2_GetActorLocation", {}, tp, sizeof(tp)))
        {
            const float dx = tp[0] - here[0], dy = tp[1] - here[1];
            if (dx * dx + dy * dy <= reach * reach)
                return false; // in reach already
        }
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> live;
        if (!PawnClass || !ActorsOf(pc, PawnClass, live))
            return false;
        const uint8_t ours = TeamOf(pawn);
        API::UObject *best = nullptr, *bestClose = nullptr;
        float dBest = reach * reach, dClose = reach * reach;
        for (auto *k : KnownActors(pawn))
        {
            if (std::find(live.begin(), live.end(), k) == live.end() || TeamOf(k) == ours || TeamOf(k) == 0xFF || IsDeadUnit(k) || !IsCombatUnit(k) ||
                k == cur)
                continue;
            float p[3] = {};
            if (!CallRet(k, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            const float dx = p[0] - here[0], dy = p[1] - here[1], d = dx * dx + dy * dy;
            if (d >= kIcMinRangeCm * kIcMinRangeCm && d < dBest)
            {
                dBest = d;
                best = k;
            }
            else if (d < kIcMinRangeCm * kIcMinRangeCm && d < dClose)
            {
                dClose = d;
                bestClose = k;
            }
        }
        API::UObject *pick = best ? best : bestClose;
        if (!pick || !ResolveSetTarget(ctl))
            return false;
        API::UObject *aware = nullptr;
        bool known = false;
        const bool set = GiveTarget(ctl, pawn, pick, aware, known);
        TacLog("tacops: %s, slot %d - target %ls %s; handed %ls %.0f m away (set %s, target now %ls)", who, s,
                cur ? ClassName(cur).c_str() : L"none", cur ? "out of LRM reach" : "missing", ClassName(pick).c_str(),
                std::sqrt(best ? dBest : dClose) * 0.01f, set ? "ok" : "FAILED", ClassName(ObjProp(ctl, L"CombatTarget")).c_str());
        return set;
    }

    // ---- TARGET TIMER for "hold position (indirect fire only)" (vanillahud-86). FIREABLE: the
    // missile lock on the target is complete and a lock-on weapon's own fire check does not rule
    // it out (range, minimum range, ammo, destroyed) - cooldown and heat do not count against it.
    // Not fireable for kIcTTMs -> the nearest enemy the lancemate knows (its tracker: its own
    // senses and what the team shares - a hidden lancemate usually sees nobody), at least the LRM
    // minimum range away if there is one, skipping targets dropped in the last kIcDropMs. Every new
    // target - ours or the game's - gets kIcTTMs before it is judged (the lock has to form).
    static constexpr ULONGLONG kIcTTMs = 5000, kIcDropMs = 20000;

    API::UObject *IcTT[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG     IcTTOk[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG     IcTTLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int           IcTTSwitches[VanillaExportBlock::kTacMaxMembers + 1] = {};

    std::vector<std::pair<API::UObject *, ULONGLONG>> IcDrop[VanillaExportBlock::kTacMaxMembers + 1]; // (target, until)

    API::UObject *IcTargetTimer(API::UObject *pc, int s, API::UObject *pawn, API::UObject *ctl, const float *here,
                                API::UObject *tgt, ULONGLONG now)
    {
        if (tgt != IcTT[s])
        {
            IcTT[s] = tgt;
            IcTTOk[s] = now; // grace: judged kIcTTMs from now
        }
        if (tgt && LockedOn(pawn, tgt) && IaReach(s, tgt, 1))
        {
            IcTTOk[s] = now;
            return tgt;
        }
        if (tgt && now - IcTTOk[s] <= kIcTTMs)
            return tgt;
        // Pick the next one.
        auto &drop = IcDrop[s];
        drop.erase(std::remove_if(drop.begin(), drop.end(), [&](auto &d) { return now >= d.second; }), drop.end());
        if (tgt)
            drop.push_back({tgt, now + kIcDropMs});
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> live;
        if (!PawnClass || !ActorsOf(pc, PawnClass, live))
            return tgt;
        const uint8_t ours = TeamOf(pawn);
        API::UObject *pFar = nullptr, *pNear = nullptr;
        float dFar = 1e30f, dClose = 1e30f;
        for (auto *k : KnownActors(pawn))
        {
            if (k == tgt || std::find(live.begin(), live.end(), k) == live.end() || TeamOf(k) == ours || TeamOf(k) == 0xFF ||
                IsDeadUnit(k) || !IsCombatUnit(k) || std::find_if(drop.begin(), drop.end(), [&](auto &d) { return d.first == k; }) != drop.end())
                continue;
            float p[3] = {};
            if (!CallRet(k, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            const float dx = p[0] - here[0], dy = p[1] - here[1], d = dx * dx + dy * dy;
            if (d >= kIcMinRangeCm * kIcMinRangeCm)
            {
                if (d < dFar)
                {
                    dFar = d;
                    pFar = k;
                }
            }
            else if (d < dClose)
            {
                dClose = d;
                pNear = k;
            }
        }
        API::UObject *pick = pFar ? pFar : pNear;
        if (!pick)
        {
            if (now - IcTTLogMs[s] > 5000)
            {
                IcTTLogMs[s] = now;
                TacLog("tacops: hold (indirect fire), slot %d - target %ls not fired on for 5 s, no other known enemy: kept", s,
                        tgt ? ClassName(tgt).c_str() : L"none");
            }
            IcTTOk[s] = now; // look again after another 5 s
            return tgt;
        }
        bool set = false;
        if (ResolveSetTarget(ctl))
        {
            API::UObject *aware = nullptr;
            bool known = false;
            set = GiveTarget(ctl, pawn, pick, aware, known);
        }
        API::UObject *nt = ObjProp(ctl, L"CombatTarget");
        ++IcTTSwitches[s];
        TacLog("tacops: hold (indirect fire), slot %d - target %ls%s: switched to %ls, %.0f m (set %s, target now %ls)", s,
                tgt ? ClassName(tgt).c_str() : L"none", tgt ? " not fired on for 5 s" : "", ClassName(pick).c_str(),
                std::sqrt(pFar ? dFar : dClose) * 0.01f, set ? "ok" : "FAILED", ClassName(nt).c_str());
        IcTT[s] = nt;
        IcTTOk[s] = now;
        return (nt && !IsDeadUnit(nt)) ? nt : nullptr;
    }

    bool IcSend(API::UObject *pc, int s, uint8_t order, const float *loc, bool indirectHold = false)
    {
        VanillaExportBlock::TacCmd m{};
        m.Order = order;
        m.Slot = (uint8_t)s;
        if (loc)
            std::memcpy(m.Loc, loc, sizeof(m.Loc));
        IcInternal = true;
        IcHoldNext = indirectHold;
        const bool ok = TacPost(pc, m);
        IcHoldNext = false;
        IcInternal = false;
        return ok;
    }

    // ---- which enemies see this lancemate, by their own visible lists ----------------------
    int IcSeenBy(API::UObject *pc, API::UObject *pawn, const float *here, std::vector<API::UObject *> &seers,
                 std::vector<API::UObject *> &enemies)
    {
        seers.clear();
        enemies.clear();
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> all;
        if (!PawnClass || !ActorsOf(pc, PawnClass, all))
            return 0;
        const uint8_t ours = TeamOf(pawn);
        int withList = 0, without = 0;
        for (auto *a : all)
        {
            // VTOLs do not count: nobody can hide from an aircraft, and it is not worth moving for.
            if (!a || a == pawn || TeamOf(a) == ours || TeamOf(a) == 0xFF || IsDeadUnit(a) || IsVtol(a))
                continue;
            float p[3] = {};
            if (!CallRet(a, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            const float dx = p[0] - here[0], dy = p[1] - here[1];
            if (dx * dx + dy * dy > 150000.0f * 150000.0f) // 1.5 km
                continue;
            enemies.push_back(a);
            API::UObject *ctl = ObjProp(a, L"Controller");
            API::UObject *tvc = ctl ? ObjProp(ctl, L"TargetVisibilityComponent") : nullptr;
            API::FProperty *p2 = tvc ? FindProp(tvc->get_class(), L"CurrentVisibleActors") : nullptr;
            if (!p2 || PropType(p2) != L"ArrayProperty")
            {
                ++without;
                continue;
            }
            ++withList;
            const auto *arr = reinterpret_cast<const TArr *>(reinterpret_cast<const uint8_t *>(tvc) + p2->get_offset());
            for (int32_t i = 0; arr->Data && i < arr->Num && i < 64; ++i)
                if (static_cast<API::UObject *const *>(arr->Data)[i] == pawn)
                {
                    seers.push_back(a);
                    break;
                }
        }
        if (!IcVisChecked && withList + without > 0)
        {
            IcVisChecked = true;
            TacLog("tacops: hold (indirect fire) - enemy visible lists: %d enemy unit(s) readable, %d without one%s", withList, without,
                    withList ? "" : " - NO ENEMY LIST READABLE: being seen cannot be detected");
        }
        return (int)seers.size();
    }

    // ---- holding direct fire: the weapon system's suppression mask (experiment) -----------
    // Which weapons are lock-on (LRM-type, the ones that fire indirectly): the weapon's own data
    // asset - bRequiresLockOnTarget / bCanLockOnToTargets (names from the game's reflection data;
    // K2_IsIndirectFire lives on the missile emitter, not the weapon, which is why vanillahud-70
    // could not call it). Fallback: "Missile" in the weapon's class name. The weapon system's
    // WeaponRegistry gives each weapon's WeaponGroupFlags (a weak pointer to the weapon - resolved
    // here); the mask tried is SuppressedWeaponMask = the groups holding direct weapons and no
    // lock-on weapon. Whether the AI's firing respects it is what this build finds out: every
    // direct weapon's fire result is watched, and a shot while the mask is on is logged as
    // "NOT HELD". The mask is put back as it was when the order ends.
    struct IcWpn
    {
        API::UObject *W;
        std::wstring  Name;
        int           Indirect; // 1 / 0 / -1 unknown
        unsigned      Last;
        ULONGLONG     OwnMs; // last time the plugin started it (not a game shot)
        uint8_t       Cat;   // its fire-settings category (kFc* bit), vanillahud-91
    };

    std::vector<IcWpn> IcWpns[VanillaExportBlock::kTacMaxMembers + 1];
    std::vector<API::UObject *> IcMelee[VanillaExportBlock::kTacMaxMembers + 1]; // lancecontrol-83: punches / melee weapons

    int     IcMaskOld[VanillaExportBlock::kTacMaxMembers + 1] = {};   // the game's hold-fire flag before we took over

    const char *IcNoFire[VanillaExportBlock::kTacMaxMembers + 1] = {}; // why the plugin cannot fire for it (game keeps firing)

    const char *IcCantFire[VanillaExportBlock::kTacMaxMembers + 1] = {};     // same, even while only holding fire for it

    const char *IcNoFireLogged[VanillaExportBlock::kTacMaxMembers + 1] = {}; // last reason logged (said once)

    bool    IcMaskSet[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int     IcDirectShots[VanillaExportBlock::kTacMaxMembers + 1] = {};

    std::wstring IcWpnList[VanillaExportBlock::kTacMaxMembers + 1];            // the last IcMaskOn's weapon list

    API::UObject *IcWatchTgt[VanillaExportBlock::kTacMaxMembers + 1] = {};    // IcWatchDirect's target last time

    // Melee (the punches: Weapon_Melee_MechPunch*): "Melee" in the class or a class it is built on.
    // The plugin never fires these (vanillahud-82: they were fired as direct weapons).
    static bool IsMeleeWeapon(API::UObject *w)
    {
        for (auto *st = w ? static_cast<API::UStruct *>(w->get_class()) : nullptr; st; st = st->get_super_struct())
            if (st->get_fname()->to_string().find(L"Melee") != std::wstring::npos)
                return true;
        return false;
    }

    // 1 lock-on weapon, 0 direct, -1 unknown; `how` says how it was decided.
    int IcLockWeapon(API::UObject *w, const wchar_t *&how)
    {
        how = L"?";
        API::UObject *da = nullptr;
        if (FindFn(w, L"GetWeaponDataAsset"))
            CallRet(w, L"GetWeaponDataAsset", {}, &da, sizeof(da));
        if (da && da->get_class())
        {
            int r = -1;
            for (const wchar_t *n : {L"bRequiresLockOnTarget", L"bCanLockOnToTargets"})
                if (auto *p = FindProp(da->get_class(), n))
                    if (PropType(p) == L"BoolProperty")
                    {
                        const bool v = reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(da);
                        r = (r == 1 || v) ? 1 : 0;
                    }
            if (r >= 0)
            {
                how = L"data asset";
                return r;
            }
        }
        // vanillahud-97: the game's data assets carry neither flag (every weapon fell through to the
        // class name, and Arrow IVs - "..._Arrow_C" - were taken for direct weapons). In order:
        //   1. seen asking for a lock (its fire check said LockOnRequired) this session;
        //   2. its data asset's MissilePath (vanillahud-98): an arcing or homing path locks on, a
        //      spread path (SRMs) is dumb-fire, no MissilePath at all is not a missile weapon;
        //   3. only when that cannot be read, the class name: Missile, LRM, ATM, Arrow.
        if (std::find(IcLockLearned.begin(), IcLockLearned.end(), w->get_class()) != IcLockLearned.end())
        {
            how = L"asked for a lock";
            return 1;
        }
        {
            const ArcWpnInfo inf = ArcWeapon(w);
            std::wstring lp = inf.Path;
            for (auto &ch : lp)
                ch = (wchar_t)towlower(ch);
            if (inf.Src == 2)
            {
                how = inf.Kind == 0 ? L"arcing flight path" : (inf.Kind == 1 ? L"homing flight path" : L"spread flight path");
                return inf.Kind == 2 ? 0 : 1;
            }
            if (inf.Src == 1)
            {
                how = L"not a missile weapon";
                return 0;
            }
        }
        how = L"class name";
        std::wstring cn = ClassName(w);
        for (auto &ch : cn)
            ch = (wchar_t)towlower(ch);
        for (const wchar_t *k : {L"missile", L"lrm", L"atm", L"arrow"})
            if (cn.find(k) != std::wstring::npos)
                return 1;
        return 0;
    }

    // A weapon taken for direct fire whose fire check asks for a lock is a lock-on weapon: learned
    // for the session (by class), recategorised at once, logged once.
    std::vector<API::UClass *> IcLockLearned;

    void IcLearnLock(int s, IcWpn &x, unsigned fireResult)
    {
        if (fireResult != 5 || x.Indirect == 1)
            return;
        if (std::find(IcLockLearned.begin(), IcLockLearned.end(), x.W->get_class()) == IcLockLearned.end())
        {
            IcLockLearned.push_back(x.W->get_class());
            TacLog("tacops: slot %d - %ls asked for a missile lock: a LOCK-ON weapon from now on (fired by the plugin, arc-checked)", s,
                    ClassName(x.W).c_str());
        }
        x.Indirect = 1;
        x.Cat = WeaponCat(x.W, 1);
    }

    // REACH (vanillahud-97): the longest maximum range among this lancemate's lock-on weapons, from
    // their data assets (MaxRange / MaximumRange / Range, metres or cm); 900 m when none is found.
    std::vector<std::pair<API::UClass *, float>> IcRangeCache;

    ULONGLONG IcRangeLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    float IcWeaponRangeCm(API::UObject *w)
    {
        for (auto &e : IcRangeCache)
            if (e.first == w->get_class())
                return e.second;
        float r = -1.0f;
        std::wstring path;
        API::UObject *da = nullptr;
        if (FindFn(w, L"GetWeaponDataAsset"))
            CallRet(w, L"GetWeaponDataAsset", {}, &da, sizeof(da));
        if (da && da->get_class())
        {
            float v = 0.0f; // lancecontrol-69: WeaponRanges.MaxRange, always cm
            if (WsFindRange(da->get_class(), reinterpret_cast<uint8_t *>(da), 0, false, v, path) && v > 0.0f)
                r = v;
        }
        TacLog("tacops: reach - %ls: %s", ClassName(w).c_str(),
                r > 0.0f ? (std::string("max range ") + std::to_string((int)(r * 0.01f)) + " m (" + std::string(path.begin(), path.end()) + ")").c_str()
                         : "no range in its data (900 m assumed)");
        IcRangeCache.push_back({w->get_class(), r});
        return r;
    }

    float IcReachCm(int s, API::UObject *pawn)
    {
        float best = -1.0f;
        for (auto &x : IcWpns[s])
            if (x.Indirect == 1 && OnMech(pawn, x.W))
                best = (std::max)(best, IcWeaponRangeCm(x.W));
        return best > 0.0f ? best : kIcMaxRangeCm;
    }

    void IcMaskOn(int s, API::UObject *pawn, bool quiet = false)
    {
        IcWpns[s].clear();
        IcMelee[s].clear(); // lancecontrol-83
        if (!quiet)
            IcDirectShots[s] = 0;
        API::UObject *ws = ObjProp(pawn, L"WeaponSystemComponent");
        uint32_t directGroups = 0, lockGroups = 0;
        std::wstring list;
        if (ws)
            if (auto *rp = FindProp(ws->get_class(), L"WeaponRegistry"))
                if (PropType(rp) == L"ArrayProperty")
                    if (auto *in = reinterpret_cast<API::FArrayProperty *>(rp)->get_inner())
                        if (PropType(in) == L"StructProperty")
                        {
                            auto *es = reinterpret_cast<API::FStructProperty *>(in)->get_struct();
                            const int32_t stride = es ? es->get_struct_size() : 0;
                            uint8_t *at = reinterpret_cast<uint8_t *>(ws) + rp->get_offset();
                            uint8_t *data = *reinterpret_cast<uint8_t **>(at);
                            const int32_t num = *reinterpret_cast<int32_t *>(at + sizeof(void *));
                            auto *wp = es ? StructProp(es, L"Weapon") : nullptr;
                            auto *gp = es ? StructProp(es, L"WeaponGroupFlags") : nullptr;
                            for (int32_t i = 0; es && stride > 0 && data && i < num && i < 32; ++i)
                            {
                                uint8_t *el = data + (size_t)i * stride;
                                API::UObject *w = nullptr;
                                if (wp && PropType(wp) == L"WeakObjectProperty")
                                    w = WeakGet(el + wp->get_offset());
                                else if (wp && PropType(wp) == L"ObjectProperty")
                                    w = *reinterpret_cast<API::UObject **>(el + wp->get_offset());
                                if (w && !AiIsWeaponComp(w))
                                    w = nullptr;
                                if (w && IsMeleeWeapon(w))
                                {
                                    // lancecontrol-83: swung by the plugin only in reach (IcMeleeFire), never by its fire check
                                    if (std::find(IcMelee[s].begin(), IcMelee[s].end(), w) == IcMelee[s].end())
                                        IcMelee[s].push_back(w);
                                    list += L" | " + ClassName(w) + L": melee - swung by the plugin in reach";
                                    continue;
                                }
                                uint32_t g = 0;
                                if (gp)
                                    g = PropType(gp) == L"ByteProperty" ? *(el + gp->get_offset())
                                                                        : *reinterpret_cast<uint32_t *>(el + gp->get_offset());
                                const wchar_t *how = L"?";
                                const int lockw = w ? IcLockWeapon(w, how) : -1;
                                if (w)
                                {
                                    IcWpns[s].push_back({w, w->get_fname()->to_string(), lockw, 0xFF, 0, 0});
                                    IcWpns[s].back().Cat = WeaponCat(w, lockw);
                                }
                                if (lockw == 1)
                                    lockGroups |= g;
                                else if (lockw == 0)
                                    directGroups |= g;
                                wchar_t b[48];
                                _snwprintf_s(b, _countof(b), _TRUNCATE, L" groups 0x%X", g);
                                list += L" | " + (w ? ClassName(w) : std::wstring(L"?")) + L": " +
                                        (lockw == 1 ? L"LOCK-ON" : (lockw == 0 ? L"direct" : L"unknown")) + L" (" + how + L")" + b +
                                        (w ? std::wstring(L" [") + FcName(IcWpns[s].back().Cat) + L"]" : std::wstring());
                            }
                        }
        // THE HOLD (vanillahud-73): the mask was ignored by the AI (vanillahud-72: 18 direct shots
        // while "held"), so the game's own firing is switched off instead - the weapon controller's
        // hold-fire (the controller's SetSuppressFiring, else the component's bSuppressFiring) - and
        // the lock-on weapons are fired by this plugin (IcOwnFire). The split is the game's: a
        // weapon that locks on (its data asset, else "Missile" in its class) is indirect.
        (void)directGroups;
        (void)lockGroups;
        IcWpnList[s] = list;
        // NO SILENT LANCEMATES (vanillahud-88): the game's firing is only switched off if the plugin
        // can fire for this lancemate - otherwise the game keeps firing and the log says why.
        if (!AiFrTried && !IcWpns[s].empty())
            AiPrepWeaponFns(IcWpns[s][0].W);
        IcNoFire[s] = nullptr;
        bool anyLock = false, anyDirect = false, aimOk = false;
        for (auto &x : IcWpns[s])
        {
            anyLock |= x.Indirect == 1;
            anyDirect |= x.Indirect == 0;
            aimOk |= x.Indirect == 0 && AimOverrideOff(x.W) >= 0;
        }
        bool lockOk = false;
        if (API::UObject *ws = ObjProp(pawn, L"WeaponSystemComponent"))
            if (API::UObject *lc = ObjProp(ws, L"LockOnComponent"))
                lockOk = FindProp(lc->get_class(), L"LockOnState") != nullptr;
        if (IcWpns[s].empty())
            IcNoFire[s] = "no weapons found";
        else if (!AiFrOk || !AiFrFn)
            IcNoFire[s] = "the weapons' fire check was not found or not understood";
        else if (anyDirect && !aimOk)
            IcNoFire[s] = "the weapons' aim target (AIOverrideAimTarget) was not found";
        else if (anyLock && !lockOk)
            IcNoFire[s] = "the missile lock state was not found";
        // M9 (vanillahud-92): a lancemate that is to fire nothing at all (hold fire, no categories, or
        // on the game's Hold Fire) needs only the suppression - not the ability to fire.
        const int flagNow = IcSuppressedNow(pawn);
        IcCantFire[s] = IcNoFire[s]; // kept: if the hold is lifted, the game takes the firing back
        if (FsDisc[s] == 2 || FsMask[s] == 0 || flagNow == 1)
            IcNoFire[s] = nullptr;
        bool ok = false;
        if (!IcNoFire[s])
        {
            // HOLD FIRE KEPT (vanillahud-88): what the game had before we took over, put back after.
            if (!IcMaskSet[s])
                IcMaskOld[s] = flagNow == 1 ? 1 : 0;
            ok = IcSuppress(pawn, true);
            if (!ok)
                IcNoFire[s] = "the game's firing could not be switched off";
        }
        IcMaskSet[s] = ok;
        if (IcNoFire[s] && IcNoFire[s] != IcNoFireLogged[s])
            TacLog("slot %d - cannot fire for this lancemate (%s): the game keeps firing", s, IcNoFire[s]);
        IcNoFireLogged[s] = IcNoFire[s];
        if (!quiet)
            TacLog("tacops: hold (indirect fire), slot %d - game firing %s; the plugin fires the lock-on weapons. Weapons:%ls", s,
                    ok ? "SUPPRESSED" : "COULD NOT BE SUPPRESSED (direct fire not held)", list.c_str());
    }

    // The game's hold fire on this lancemate's weapon controller: on / off.
    bool IcSuppress(API::UObject *pawn, bool on)
    {
        if (pawn && !IsLive(pawn))
            LivePawnsMs = 0; // the list may be up to 250 ms old: look again before giving up
        API::UObject *ctl = (pawn && IsLive(pawn)) ? ObjProp(pawn, L"Controller") : nullptr;
        if (!ctl)
            return false;
        if (API::UFunction *fn = FindFn(ctl, L"SetSuppressFiring"))
        {
            const int32_t off = FirstParamOffset(fn), frame = fn->get_properties_size();
            if (off >= 0 && frame > 0 && frame <= 256)
            {
                std::vector<uint8_t> f((size_t)frame + 16, 0);
                f[off] = on ? 1 : 0;
                ctl->process_event(fn, f.data());
                return true;
            }
        }
        if (auto *wp = FindProp(ctl->get_class(), L"WeaponControlSystem"))
            if (PropType(wp) == L"ObjectProperty")
                if (API::UObject *w = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(ctl) + wp->get_offset()))
                    if (auto *bp = FindProp(w->get_class(), L"bSuppressFiring"))
                        if (PropType(bp) == L"BoolProperty")
                        {
                            reinterpret_cast<API::FBoolProperty *>(bp)->set_value_in_object(w, on);
                            return true;
                        }
        return false;
    }

    void IcMaskOff(int s, API::UObject *pawn, bool quiet = false)
    {
        if (!IcMaskSet[s])
            return;
        IcMaskSet[s] = false;
        const bool keep = IcMaskOld[s] == 1; // Hold Fire was on before we took over: leave it on
        IcSuppress(pawn, keep);
        if (keep)
            TacLog("slot %d - handed back with the game's Hold Fire still ON (it was on before)", s);
        if (!quiet)
            TacLog("tacops: hold (indirect fire), slot %d - game firing restored; direct shots while suppressed: %d%s; own LRM "
                "volleys started: %d", s, IcDirectShots[s], IcDirectShots[s] ? " - SUPPRESSION DID NOT HOLD DIRECT FIRE" : "",
                IcOwnFires[s]);
    }

    // ---- OUR OWN LRM FIRE (test). Every 250 ms, each lock-on weapon of a lancemate holding under
    // this order is started when: its missile lock is Locked, the weapon's own fire check says
    // Success (cooldown, ammo, jammed, minimum range), the target is within 900 m (the LRM's
    // maximum range as the AI's own weapon records give it), and its heat is under 85 % (the
    // game's AI stops at 90 %). K2_StartFire on the weapon component; K2_StopFire on the next tick.
    // Each start is logged, and the probe's MISSILE VOLLEY lines show whether missiles came out.
    int        IcOwnFires[VanillaExportBlock::kTacMaxMembers + 1] = {};

    std::vector<API::UObject *> IcFiring[VanillaExportBlock::kTacMaxMembers + 1]; // started last tick: stop now

    bool       IcFireFnLogged = false;

    float IcHeatFrac(API::UObject *pawn)
    {
        float cur = 0.0f, mx = 0.0f; // lancecontrol-50: one heat-component lookup (IcHeatRead)
        if (!IcHeatRead(pawn, cur, mx))
            return -1.0f;
        return mx > 0.0f ? cur / mx : -1.0f;
    }

    // A weapon is only ever called while it is still one of the unit's components (vanillahud-88):
    // a part shot off, or a unit from the last mission, is never touched. The list is read once per
    // clock tick per unit.
    API::UObject *OnMechPawn = nullptr;

    ULONGLONG     OnMechMs   = 0;

    std::vector<API::UObject *> OnMechList;

    bool OnMech(API::UObject *pawn, API::UObject *w)
    {
        if (!pawn || !w)
            return false;
        const ULONGLONG now = GetTickCount64();
        if (pawn != OnMechPawn || now != OnMechMs)
        {
            static API::UClass *acc = nullptr;
            if (!acc)
                acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
            OnMechPawn = pawn;
            OnMechMs = now;
            OnMechList.clear();
            if (!acc || !IsLive(pawn) || !ComponentsOn(pawn, acc, OnMechList))
                OnMechList.clear();
        }
        return std::find(OnMechList.begin(), OnMechList.end(), w) != OnMechList.end();
    }

    // The game's own hold-fire flag on this lancemate's weapon control (wcs.bSuppressFiring in the
    // probe - the flag SetSuppressFiring sets): 1 / 0, -1 unreadable.
    int IcSuppressedNow(API::UObject *pawn)
    {
        API::UObject *ctl = pawn ? LsObj(pawn, L"Controller") : nullptr; // lancecontrol-50: cached lookups
        API::UObject *w = ctl ? LsObj(ctl, L"WeaponControlSystem") : nullptr;
        auto *bp = w ? LsBool(w->get_class(), L"bSuppressFiring") : nullptr;
        if (!bp)
            return -1;
        return reinterpret_cast<API::FBoolProperty *>(bp)->get_value_from_object(w) ? 1 : 0;
    }

    // Heat in the game's own units (vanillahud-92, for the heat budget): current and capacity.
    bool IcHeatAbs(API::UObject *pawn, float &cur, float &mx)
    {
        return IcHeatRead(pawn, cur, mx) && mx > 0.0f; // lancecontrol-50
    }

    // HEAT COMPONENT (lancecontrol-50). IcHeatFrac and IcHeatAbs each walked every component of the mech and built
    // its class name as a string, twice per lancemate per insane-allies pass. Now: whether a class is the heat
    // component is decided once per class (name holds "Heat", float CurrentHeat + MaxHeatCapacity - the old test),
    // and inside one IaTick pass the component found for a mech is reused (never across passes: the pointer is
    // only trusted within the tick it was read in).
    struct IcHeatClsE { API::UClass *Cls; bool Heat; };
    std::vector<IcHeatClsE> IcHeatCls;
    struct IcHeatMemoE { API::UObject *Pawn, *Comp; unsigned Pass; };
    std::vector<IcHeatMemoE> IcHeatMemo;
    unsigned IaPassNo = 0;

    bool IcHeatRead(API::UObject *pawn, float &cur, float &mx)
    {
        API::UObject *hc = nullptr;
        bool memo = false;
        if (IaInPass)
            for (auto &m : IcHeatMemo)
                if (m.Pawn == pawn && m.Pass == IaPassNo)
                {
                    hc = m.Comp;
                    memo = true;
                    break;
                }
        if (!memo)
        {
            static API::UClass *acc = nullptr;
            if (!acc)
                acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
            std::vector<API::UObject *> comps;
            if (!acc || !ComponentsOn(pawn, acc, comps))
                return false;
            for (auto *c : comps)
            {
                API::UClass *cls = c ? c->get_class() : nullptr;
                if (!cls)
                    continue;
                int k = -1;
                for (size_t i = 0; i < IcHeatCls.size(); ++i)
                    if (IcHeatCls[i].Cls == cls)
                        k = (int)i;
                if (k < 0)
                {
                    const bool h = ClassName(c).find(L"Heat") != std::wstring::npos &&
                                   LsOff(c, L"CurrentHeat", L"FloatProperty") >= 0 && LsOff(c, L"MaxHeatCapacity", L"FloatProperty") >= 0;
                    if (IcHeatCls.size() > 256)
                        IcHeatCls.clear();
                    IcHeatCls.push_back({cls, h});
                    k = (int)IcHeatCls.size() - 1;
                }
                if (IcHeatCls[k].Heat)
                {
                    hc = c;
                    break;
                }
            }
            if (IaInPass)
            {
                if (IcHeatMemo.size() > 16)
                    IcHeatMemo.clear();
                IcHeatMemo.push_back({pawn, hc, IaPassNo});
            }
        }
        if (!hc)
            return false;
        const int32_t co = LsOff(hc, L"CurrentHeat", L"FloatProperty"), mo = LsOff(hc, L"MaxHeatCapacity", L"FloatProperty");
        if (co < 0 || mo < 0)
            return false;
        const uint8_t *b = reinterpret_cast<uint8_t *>(hc);
        cur = *reinterpret_cast<const float *>(b + co);
        mx = *reinterpret_cast<const float *>(b + mo);
        return true;
    }

    float     IcHeatProj[VanillaExportBlock::kTacMaxMembers + 1] = {};    // heat committed by this tick's missiles

    bool      IcHeatProjSet[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG IcHeatProjMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    // HOLD FIRE, OBEYED (vanillahud-92). While the plugin holds a lancemate's firing, IcMaskOld is
    // the GAME's Hold Fire state for it: set when we took over (the flag as it was), toggled by
    // TacOps' Hold Fire order, cleared when the flag is seen dropping to 0 (a Weapons Free toggle -
    // the in-game D-pad included; the suppression is then put back at once). While it is on, the
    // plugin fires nothing; the hand-back leaves the flag as it says. (A D-pad Hold Fire while we
    // hold the flag cannot be seen: the flag is already on.)
    ULONGLONG IcHoldLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    bool IcHoldNow(int s) const { return FsDisc[s] == 2 || IcMaskOld[s] == 1; }

    void IcWatchHold(int s, API::UObject *pawn)
    {
        if (!IcMaskSet[s] || !pawn)
            return;
        if (IcSuppressedNow(pawn) == 0)
        {
            const bool was = IcMaskOld[s] == 1;
            IcMaskOld[s] = 0;
            IcSuppress(pawn, true);
            const ULONGLONG now = GetTickCount64();
            if (!was && now - IcHoldLogMs[s] < 5000)
                return; // (the game clearing it again and again: said once every 5 s)
            IcHoldLogMs[s] = now;
            TacLog("slot %d - the game's firing was switched back on (Weapons Free%s): obeyed, the plugin keeps control%s", s,
                    was ? " after Hold Fire" : "", was ? " and fires again" : "");
        }
    }

    bool IcCallNoArgs(API::UObject *w, const wchar_t *name)
    {
        API::UFunction *fn = FindFn(w, name);
        if (!fn)
            return false;
        const int32_t frame = fn->get_properties_size();
        if (frame < 0 || frame > 512)
            return false;
        std::vector<uint8_t> f((size_t)frame + 16, 0); // any parameters left zeroed
        w->process_event(fn, f.data());
        return true;
    }

    void IcOwnFire(int s, API::UObject *pawn, API::UObject *ctl, API::UObject *tgt, const float *here)
    {
        // Stop what was started last tick (only weapons still on the mech).
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        std::vector<API::UObject *> fresh;
        if (!acc || !ComponentsOn(pawn, acc, fresh))
            return;
        for (auto *w : IcFiring[s])
            if (std::find(fresh.begin(), fresh.end(), w) != fresh.end())
                IcCallNoArgs(w, L"K2_StopFire");
        IcFiring[s].clear();
        if (!IcMaskSet[s] || !tgt || IaDead(tgt) || !AiFrOk || !AiFrFn)
            return;
        if (!IcFireFnLogged && !IcWpns[s].empty())
        {
            IcFireFnLogged = true;
            API::UFunction *a = FindFn(IcWpns[s][0].W, L"K2_StartFire"), *b = FindFn(IcWpns[s][0].W, L"K2_StopFire");
            TacLog("tacops: own fire - %ls / %ls", a ? Signature(a).c_str() : L"K2_StartFire NOT FOUND",
                    b ? Signature(b).c_str() : L"K2_StopFire NOT FOUND");
        }
        // Missile lock on the target?
        int lockState = -1;
        API::UObject *pending = nullptr;
        if (API::UObject *ws = LsObj(pawn, L"WeaponSystemComponent")) // lancecontrol-50: cached offsets
            if (API::UObject *lc = LsObj(ws, L"LockOnComponent"))
            {
                const int32_t lo = LsOff(lc, L"LockOnState", nullptr);
                if (lo >= 0)
                    lockState = *(reinterpret_cast<uint8_t *>(lc) + lo);
                pending = LsObj(lc, L"PendingLockTarget");
            }
        if (lockState != 3 || (pending && pending != tgt))
            return;
        if (IcHoldNow(s) || !FsTargetOk(s, tgt)) // hold fire / return fire: not this one
            return;
        float tp[3] = {};
        if (!LsPos(tgt, tp)) // lancecontrol-50
            return;
        const float dx = tp[0] - here[0], dy = tp[1] - here[1], dz = tp[2] - here[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz); // (no range cap since vanillahud-85: each
        const float heat = IcHeatFrac(pawn);                         // weapon's own fire check says OutsideMaximumRange)
        if (heat > FsHeatLimit(s))
            return;
        float hcur = 0.0f, hmax = 0.0f;
        const bool hAbs = IcHeatAbs(pawn, hcur, hmax);
        const float hlim = FsHeatLimit(s);
        std::vector<std::pair<float, size_t>> order; // damage per heat first (vanillahud-92)
        for (size_t i = 0; i < IcWpns[s].size(); ++i)
        {
            if (std::find(fresh.begin(), fresh.end(), IcWpns[s][i].W) == fresh.end())
                continue; // shot off: never touched
            const WStat ws = WeaponStat(IcWpns[s][i].W);
            order.push_back({(ws.Heat > 0.0f && ws.Dmg > 0.0f) ? ws.Dmg / ws.Heat : (ws.Heat > 0.0f ? 0.0f : -1.0f), i});
        }
        std::stable_sort(order.begin(), order.end(), [](auto &a, auto &b) { return a.first > b.first; });
        for (auto &oi : order)
        {
            auto &x = IcWpns[s][oi.second];
            if (x.Indirect != 1 || !(FsMask[s] & x.Cat) || std::find(fresh.begin(), fresh.end(), x.W) == fresh.end())
                continue;
            const float wh = WeaponStat(x.W).Heat;
            if (hAbs && wh > 0.0f && hcur + wh > hlim * hmax)
                continue; // would pass the heat ceiling
            // RATED RANGE (vanillahud-100): lock-on missiles may be fired past maximum range by the game
            // (bCanFireOutsideMaxRange on LRMs / Arrow IV), so their own fire check never says so - the
            // weapon's rated maximum range from its data is the limit here.
            {
                const float rr = IcWeaponRangeCm(x.W);
                if (rr > 0.0f && dist > rr)
                {
                    const ULONGLONG t = GetTickCount64();
                    if (t - IcRangeLogMs[s] > 5000)
                    {
                        IcRangeLogMs[s] = t;
                        TacLog("tacops: own fire, slot %d - %ls held: %ls %.0f m away, rated range %.0f m", s, x.Name.c_str(),
                                ClassName(tgt).c_str(), dist * 0.01f, rr * 0.01f);
                    }
                    continue;
                }
            }
            std::vector<uint8_t> f(AiFrBuf.size(), 0);
            for (int32_t off : AiFrObjParams)
                std::memcpy(f.data() + off, &tgt, sizeof(tgt));
            IaCanFire(x.W, f.data()); // lancecontrol-50: timed
            if (f[AiFrRet] != 0) // not Success: cooldown, ammo, minimum range ...
                continue;
            // The predicted flight path must be clear (vanillahud-96).
            std::wstring why;
            if (ArcBlocked(s, pawn, x.W, tgt, why))
            {
                ++ArcHolds[s];
                const ULONGLONG t = GetTickCount64();
                if (t - ArcLogMs[s] > 3000)
                {
                    ArcLogMs[s] = t;
                    TacLog("missile arc - slot %d held %ls at %ls, %.0f m: %ls (%d held so far)", s, x.Name.c_str(), ClassName(tgt).c_str(),
                            dist * 0.01f, why.c_str(), ArcHolds[s]);
                }
                continue;
            }
            ++IpFireN; // lancecontrol-48
            if (!IcCallNoArgs(x.W, L"K2_StartFire"))
                continue;
            IcFiring[s].push_back(x.W);
            if (wh > 0.0f)
                hcur += wh;
            IcHeatProj[s] = hcur;
            IcHeatProjSet[s] = hAbs;
            IcHeatProjMs[s] = GetTickCount64();
            ++IcOwnFires[s];
            if (IcOwnFires[s] <= 40)
                TacLog("tacops: own fire, slot %d - %ls STARTED at %ls, %.0f m, heat %.0f %%", s, x.Name.c_str(),
                        ClassName(tgt).c_str(), dist * 0.01f, heat * 100.0f);
        }
    }

    // =====================================================================================
    // MISSILE ARC CHECK (vanillahud-96, paths per weapon since vanillahud-98): predict a lock-on
    // volley's flight path BEFORE firing and hold the launch when it would hit terrain, a structure
    // or a friendly.
    //
    // Each missile weapon names its own path: the weapon data asset's MissilePath (a soft class
    // reference, top level of MWMissileWeaponDataAsset - checked in the paks: LRM10 ->
    // LongRangeMissilePathV2, ArrowIV -> ArrowIVMissilePath, C-StreakSRM4 -> HomingStreakPath).
    // A long-range path blueprint carries reference splines "<N>m_Range", each laid from the
    // launcher towards a target: (distance along, height) per point - 9 points on the LRM path, 14
    // on the Arrow IV path. The flight path is the two references bracketing the target's level
    // distance D - by where each reference really ends (the Arrow IV "900m_Range" ends at 750 m) -
    // blended point by point, t = (D - Ra) / (Rb - Ra), the height tilted linearly to the target's
    // height difference; then a dive past the target. Checked against five LRM volleys and an
    // Arrow IV volley (1059 m): every point within a metre or two.
    // Homing paths (Streaks) fly straight at the target; spread paths (SRMs) are dumb-fire direct
    // weapons, already held to line of sight.
    //
    // The references are the path blueprint's own: learned from the first path actor of each class
    // seen in a mission (anyone's volley). The LRM V2 and Arrow IV references are built in (read
    // from the logged paths) until then. A path of any other class is not checked until one has
    // been seen.
    struct ArcRef
    {
        float R;                  // where it ends, along (cm)
        std::vector<float> A, H;  // along, height per point (cm)
    };

    struct ArcProfile
    {
        std::wstring Cls;
        std::vector<ArcRef> Refs; // sorted by R, all with the same point count
        bool Learned;
    };

    std::vector<ArcProfile> ArcProfiles;

    std::vector<void *> ArcLearnSeen;

    ULONGLONG ArcLearnMs = 0;

    API::UClass *ArcPathCls = nullptr;

    struct ArcWpnInfo
    {
        API::UClass *W;
        std::wstring Path; // the path class name ("ArrowIVMissilePath_C"), "" unknown
        int Kind;          // 0 spline path, 1 homing (straight), 2 spread / other (no check)
        int Src;           // 0 no data asset, 1 data asset without a MissilePath (not a missile weapon), 2 path read, 3 not read
    };

    std::vector<ArcWpnInfo> ArcWpnInfos;

    std::vector<std::wstring> ArcUnknownLogged;

    // per slot: last verdict (re-used while shooter and target move less than 20 m, for 1 s)
    struct ArcVerdict
    {
        API::UObject *Tgt;
        float From[3], To[3];
        ULONGLONG Ms;
        int Kind;
        bool Blocked;
    };

    std::vector<ArcVerdict> ArcVerdicts[VanillaExportBlock::kTacMaxMembers + 1];

    ULONGLONG ArcLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int ArcHolds[VanillaExportBlock::kTacMaxMembers + 1] = {};

    void ArcAddBuiltin(const wchar_t *cls, int nRef, int nPt, const int *A, const int *H)
    {
        ArcProfile p{cls, {}, false};
        for (int r = 0; r < nRef; ++r)
        {
            ArcRef ref{};
            for (int i = 0; i < nPt; ++i)
            {
                ref.A.push_back(A[r * nPt + i] * 100.0f);
                ref.H.push_back(H[r * nPt + i] * 100.0f);
            }
            ref.R = ref.A.back();
            p.Refs.push_back(ref);
        }
        ArcProfiles.push_back(p);
    }

    void ArcBuiltin()
    {
        if (!ArcProfiles.empty())
            return;
        // LongRangeMissilePathV2 (LRMs): 15 30 60 120 240 480 900 2000 m, 9 points.
        static const int kLA[8 * 9] = {0, 5, 7, 10, 12, 13, 14, 14, 15,          0, 7, 14, 21, 29, 31, 33, 34, 35,
                                       0, 8, 25, 40, 51, 54, 56, 59, 60,         0, 18, 46, 76, 99, 107, 113, 118, 120,
                                       0, 48, 84, 135, 179, 199, 218, 230, 240,  0, 68, 155, 250, 352, 401, 437, 461, 480,
                                       0, 111, 250, 467, 694, 777, 830, 870, 899, 0, 87, 250, 467, 1780, 1853, 1926, 1973, 2000};
        static const int kLH[8 * 9] = {0, 0, 1, 1, 1, 1, 1, 0, 0,     0, 1, 1, 2, 3, 3, 2, 1, 0,     0, 1, 2, 4, 6, 5, 4, 2, 0,
                                       0, 2, 5, 9, 12, 11, 8, 4, 0,   0, 5, 10, 16, 21, 22, 18, 10, 0, 0, 8, 18, 29, 45, 43, 34, 18, 0,
                                       0, 13, 29, 65, 67, 59, 47, 28, 0, 0, 10, 29, 65, 75, 72, 52, 22, 0};
        ArcAddBuiltin(L"LongRangeMissilePathV2_C", 8, 9, kLA, kLH);
        // ArrowIVMissilePath: references ending at 100 450 750 3000 6000 m, 14 points - a near
        // vertical climb to ~200 m, level, then the dive.
        static const int kAA[5 * 14] = {
            0, 4, 19, 26, 33, 35, 40, 47, 56, 64, 74, 85, 92, 100,
            0, 4, 19, 26, 33, 35, 89, 143, 197, 250, 314, 383, 423, 450,
            0, 4, 19, 26, 33, 35, 89, 154, 246, 344, 503, 617, 713, 750,
            0, 4, 19, 26, 33, 35, 89, 154, 246, 1302, 2697, 2843, 2946, 3000,
            0, 4, 19, 26, 33, 35, 89, 154, 246, 2886, 5722, 5857, 5944, 6000};
        static const int kAH[5 * 14] = {
            0, 1, 2, 5, 8, 11, 19, 28, 32, 32, 32, 28, 21, 0,
            0, 1, 2, 5, 8, 11, 107, 146, 165, 165, 165, 140, 87, 0,
            0, 1, 2, 5, 8, 11, 107, 171, 200, 200, 200, 177, 98, 0,
            0, 1, 2, 5, 8, 11, 107, 171, 200, 200, 200, 171, 95, 0,
            0, 1, 2, 5, 8, 11, 107, 171, 200, 200, 200, 169, 99, 0};
        ArcAddBuiltin(L"ArrowIVMissilePath_C", 5, 14, kAA, kAH);
    }

    // Learn a path class's references from a live path actor (read only).
    void ArcLearn(API::UObject *pc)
    {
        const ULONGLONG now = GetTickCount64();
        if (now - ArcLearnMs < 250)
            return;
        ArcLearnMs = now;
        ArcBuiltin();
        if (!ArcPathCls)
            ArcPathCls = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWMissilePath");
        static API::UClass *splineCls = nullptr;
        if (!splineCls)
            splineCls = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SplineComponent");
        std::vector<API::UObject *> paths;
        if (!ArcPathCls || !splineCls || !ActorsOf(pc, ArcPathCls, paths))
            return;
        std::vector<void *> seen;
        for (auto *mp : paths)
        {
            if (!mp)
                continue;
            seen.push_back(mp);
            if (std::find(ArcLearnSeen.begin(), ArcLearnSeen.end(), (void *)mp) != ArcLearnSeen.end())
                continue;
            const std::wstring cls = ClassName(mp);
            bool have = false;
            for (auto &p : ArcProfiles)
                have |= p.Cls == cls && p.Learned;
            if (have)
                continue;
            std::vector<API::UObject *> sc;
            if (!ComponentsOn(mp, splineCls, sc))
                continue;
            ArcProfile prof{cls, {}, true};
            int npAll = -1;
            for (auto *c : sc)
            {
                const std::wstring nm = c->get_fname()->to_string();
                const size_t k = nm.find(L"m_Range");
                if (k == std::wstring::npos || k == 0)
                    continue;
                int32_t np = 0;
                CallRet(c, L"GetNumberOfSplinePoints", {}, &np, sizeof(np));
                if (np < 3 || np > 64 || (npAll >= 0 && np != npAll))
                    continue;
                std::vector<std::array<float, 3>> P((size_t)np);
                uint8_t world = 1;
                bool ok = true;
                for (int32_t i = 0; i < np && ok; ++i)
                    ok = CallRet(c, L"GetLocationAtSplinePoint", {{L"PointIndex", &i, 4}, {L"CoordinateSpace", &world, 1}}, P[i].data(), 12);
                if (!ok)
                    continue;
                const float dx = P[np - 1][0] - P[0][0], dy = P[np - 1][1] - P[0][1], D = std::sqrt(dx * dx + dy * dy);
                if (D < 100.0f)
                    continue;
                ArcRef ref{};
                for (int i = 0; i < np; ++i)
                {
                    ref.A.push_back(((P[i][0] - P[0][0]) * dx + (P[i][1] - P[0][1]) * dy) / D);
                    ref.H.push_back(P[i][2] - P[0][2]);
                }
                ref.R = ref.A.back();
                npAll = np;
                prof.Refs.push_back(ref);
            }
            if (prof.Refs.size() < 2)
                continue;
            std::sort(prof.Refs.begin(), prof.Refs.end(), [](const ArcRef &a, const ArcRef &b) { return a.R < b.R; });
            bool replaced = false;
            for (auto &p : ArcProfiles)
                if (p.Cls == cls)
                {
                    p = prof;
                    replaced = true;
                }
            if (!replaced)
                ArcProfiles.push_back(prof);
            std::wstring rs;
            for (auto &r : prof.Refs)
                rs += L" " + std::to_wstring((int)(r.R / 100.0f));
            TacLog("missile arc - learned %ls: %zu reference splines of %d points, ending at (m):%ls", cls.c_str(), prof.Refs.size(), npAll,
                    rs.c_str());
        }
        ArcLearnSeen.swap(seen);
    }

    // Which path a missile weapon flies: its data asset's MissilePath - a soft class reference
    // (TSoftClassPtr: weak pointer 8, tag 4 + pad 4, then the FSoftObjectPath whose FName is the
    // "/Game/.../ArrowIVMissilePath.ArrowIVMissilePath_C" path), else a hard class / object one.
    ArcWpnInfo ArcWeapon(API::UObject *w)
    {
        for (auto &e : ArcWpnInfos)
            if (e.W == w->get_class())
                return e;
        ArcWpnInfo inf{w->get_class(), L"", 2, 0};
        API::UObject *da = nullptr;
        if (FindFn(w, L"GetWeaponDataAsset"))
            CallRet(w, L"GetWeaponDataAsset", {}, &da, sizeof(da));
        std::wstring raw, how = L"no MissilePath (not a missile weapon)";
        if (da)
            inf.Src = 1;
        if (da)
            if (auto *pp = FindProp(da->get_class(), L"MissilePath"))
            {
                inf.Src = 3;
                const std::wstring t = PropType(pp);
                uint8_t *at = reinterpret_cast<uint8_t *>(da) + pp->get_offset();
                if (t == L"SoftClassProperty" || t == L"SoftObjectProperty")
                {
                    raw = reinterpret_cast<API::FName *>(at + 16)->to_string();
                    if (raw.empty() || raw[0] != L'/' || raw == L"None")
                        raw.clear();
                }
                else if (t == L"ClassProperty" || t == L"ObjectProperty")
                {
                    if (auto *c = *reinterpret_cast<API::UObject **>(at))
                        raw = c->get_fname()->to_string();
                }
                how = raw.empty() ? L"MissilePath (" + t + L") empty or not read" : L"MissilePath";
            }
        if (!raw.empty())
        {
            inf.Src = 2;
            const size_t dot = raw.find_last_of(L'.');
            inf.Path = dot == std::wstring::npos ? raw : raw.substr(dot + 1);
            if (inf.Path.size() < 2 || inf.Path.compare(inf.Path.size() - 2, 2, L"_C") != 0)
                inf.Path += L"_C";
            std::wstring lo = inf.Path;
            for (auto &ch : lo)
                ch = (wchar_t)towlower(ch);
            inf.Kind = (lo.find(L"homing") != std::wstring::npos || lo.find(L"streak") != std::wstring::npos) ? 1
                       : (lo.find(L"spread") != std::wstring::npos ? 2 : 0);
        }
        TacLog("missile arc - %ls flies %ls (%ls: %s)", ClassName(w).c_str(), inf.Path.empty() ? L"-" : inf.Path.c_str(), how.c_str(),
                inf.Kind == 1 ? "straight, homing" : (inf.Kind == 2 ? "not checked" : "arc from its path's reference splines"));
        ArcWpnInfos.push_back(inf);
        return inf;
    }

    // The predicted path (world cm), launch to target: one point per reference point.
    bool ArcPredict(const std::wstring &pathCls, const float *from, const float *to, std::vector<std::array<float, 3>> &out)
    {
        ArcBuiltin();
        const ArcProfile *prof = nullptr;
        for (auto &p : ArcProfiles)
            if (p.Cls == pathCls)
                prof = &p;
        if (!prof)
        {
            if (std::find(ArcUnknownLogged.begin(), ArcUnknownLogged.end(), pathCls) == ArcUnknownLogged.end())
            {
                ArcUnknownLogged.push_back(pathCls);
                TacLog("missile arc - %ls: path shape not known yet - its volleys fire unchecked until one has been seen", pathCls.c_str());
            }
            return false;
        }
        const auto &R = prof->Refs;
        if (R.empty())
            return false;
        const float dx = to[0] - from[0], dy = to[1] - from[1], D = std::sqrt(dx * dx + dy * dy);
        if (D < 100.0f)
            return false;
        const float ux = dx / D, uy = dy / D, dz = to[2] - from[2];
        size_t a = 0, b = 0;
        if (D <= R.front().R)
            a = b = 0;
        else if (D >= R.back().R)
            a = b = R.size() - 1;
        else
            for (size_t i = 0; i + 1 < R.size(); ++i)
                if (D >= R[i].R && D <= R[i + 1].R)
                {
                    a = i;
                    b = i + 1;
                }
        const float t = (b == a) ? 0.0f : (D - R[a].R) / (R[b].R - R[a].R);
        // Outside the references' span: the nearest one scaled along to D.
        const float scale = (b == a) ? D / R[a].R : 1.0f;
        const size_t n = R[a].A.size();
        out.clear();
        for (size_t i = 0; i < n; ++i)
        {
            float al = (R[a].A[i] + (R[b].A[i] - R[a].A[i]) * t) * scale;
            float h = R[a].H[i] + (R[b].H[i] - R[a].H[i]) * t;
            if (i + 1 == n)
            {
                al = D;
                h = 0.0f;
            }
            h += dz * (al / D);
            out.push_back({from[0] + ux * al, from[1] + uy * al, from[2] + h});
        }
        return true;
    }

    // One world trace (Visibility), from the shooter's world: blocking, and where.
    struct ArcTL
    {
        int32_t Frame = -1, World = -1, Start = -1, End = -1, Channel = -1, Complex = -1, Debug = -1, Ignore = -1, Hit = -1,
                HitBlock = -1, HitLoc = -1;
        bool Tried = false, Ok = false;
        API::UObject *Cdo = nullptr;
        API::UFunction *Fn = nullptr;
    } ArcT;

    bool ArcTrace(API::UObject *ctx, const float *a, const float *b, float *hit) // lancecontrol-48: timed wrapper
    {
        if (!IpOn)
            return ArcTraceRaw(ctx, a, b, hit);
        const double t0 = IpNow();
        const bool r = ArcTraceRaw(ctx, a, b, hit);
        IpArcMs += IpNow() - t0;
        ++IpArcN;
        return r;
    }
    bool ArcTraceRaw(API::UObject *ctx, const float *a, const float *b, float *hit)
    {
        if (!ArcT.Tried)
        {
            ArcT.Tried = true;
            auto *ksl = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.KismetSystemLibrary");
            ArcT.Cdo = ksl ? ksl->get_class_default_object() : nullptr;
            ArcT.Fn = ksl ? ksl->find_function(L"LineTraceSingle") : nullptr;
            if (ArcT.Fn)
            {
                ArcT.Frame = ArcT.Fn->get_properties_size();
                for (auto *f = ArcT.Fn->get_child_properties(); f; f = f->get_next())
                {
                    auto *p = reinterpret_cast<API::FProperty *>(f);
                    const std::wstring nm = p->get_fname()->to_string();
                    const int32_t off = p->get_offset();
                    if (nm == L"WorldContextObject") ArcT.World = off;
                    else if (nm == L"Start") ArcT.Start = off;
                    else if (nm == L"End") ArcT.End = off;
                    else if (nm == L"TraceChannel") ArcT.Channel = off;
                    else if (nm == L"bTraceComplex") ArcT.Complex = off;
                    else if (nm == L"DrawDebugType") ArcT.Debug = off;
                    else if (nm == L"bIgnoreSelf") ArcT.Ignore = off;
                    else if (nm == L"OutHit") ArcT.Hit = off;
                }
                if (auto *hr = API::get()->find_uobject<API::UStruct>(L"ScriptStruct /Script/Engine.HitResult"))
                    for (auto *f = hr->get_child_properties(); f; f = f->get_next())
                    {
                        auto *p = reinterpret_cast<API::FProperty *>(f);
                        const std::wstring nm = p->get_fname()->to_string();
                        if (nm == L"bBlockingHit") ArcT.HitBlock = p->get_offset();
                        else if (nm == L"ImpactPoint") ArcT.HitLoc = p->get_offset();
                        else if (nm == L"Location" && ArcT.HitLoc < 0) ArcT.HitLoc = p->get_offset();
                    }
            }
            ArcT.Ok = ArcT.Cdo && ArcT.Fn && ArcT.Frame > 64 && ArcT.Frame < 4096 && ArcT.World >= 0 && ArcT.Start >= 0 && ArcT.End == ArcT.Start + 12 &&
                      ArcT.Channel >= 0 && ArcT.Hit >= 0 && ArcT.HitBlock >= 0 && ArcT.HitLoc >= 0;
            TacLog("missile arc - world trace %s", ArcT.Ok ? "ready" : "NOT AVAILABLE: arcs are not checked");
        }
        if (!ArcT.Ok)
            return false;
        std::vector<uint8_t> f((size_t)ArcT.Frame + 64, 0);
        std::memcpy(f.data() + ArcT.World, &ctx, sizeof(ctx));
        std::memcpy(f.data() + ArcT.Start, a, 12);
        std::memcpy(f.data() + ArcT.End, b, 12);
        f[ArcT.Channel] = 0; // Visibility
        if (ArcT.Ignore >= 0)
            f[ArcT.Ignore] = 1;
        ArcT.Cdo->process_event(ArcT.Fn, f.data());
        if (!f[ArcT.Hit + ArcT.HitBlock])
            return false;
        std::memcpy(hit, f.data() + ArcT.Hit + ArcT.HitLoc, 12);
        return true;
    }

    // true = launching now would hit terrain / a structure / a friendly on the way. `why` says what.
    bool ArcBlocked(int s, API::UObject *pawn, API::UObject *w, API::UObject *tgt, std::wstring &why)
    {
        const ArcWpnInfo inf = ArcWeapon(w);
        if (inf.Kind == 2)
            return false;
        float from[3] = {}, to[3] = {};
        if (!CallRet(w, L"K2_GetComponentLocation", {}, from, sizeof(from)) || !CallRet(tgt, L"K2_GetActorLocation", {}, to, sizeof(to)))
            return false;
        const ULONGLONG now = GetTickCount64();
        for (auto &v : ArcVerdicts[s])
        {
            const float m1 = (v.From[0] - from[0]) * (v.From[0] - from[0]) + (v.From[1] - from[1]) * (v.From[1] - from[1]);
            const float m2 = (v.To[0] - to[0]) * (v.To[0] - to[0]) + (v.To[1] - to[1]) * (v.To[1] - to[1]);
            if (v.Tgt == tgt && v.Kind == inf.Kind && now - v.Ms < 1000 && m1 < 2000.0f * 2000.0f && m2 < 2000.0f * 2000.0f)
            {
                why = L"(as last checked)";
                return v.Blocked;
            }
        }
        // The path: its points, each gap split in two (Catmull-Rom) - or a straight line for homing paths.
        std::vector<std::array<float, 3>> pts;
        if (inf.Kind == 1)
        {
            for (int i = 0; i <= 4; ++i)
            {
                const float k = i / 4.0f;
                pts.push_back({from[0] + (to[0] - from[0]) * k, from[1] + (to[1] - from[1]) * k, from[2] + (to[2] - from[2]) * k});
            }
        }
        else
        {
            std::vector<std::array<float, 3>> P;
            if (!ArcPredict(inf.Path, from, to, P))
                return false;
            const int n = (int)P.size();
            for (int i = 0; i + 1 < n; ++i)
            {
                pts.push_back(P[i]);
                const float *p0 = P[i > 0 ? i - 1 : 0].data(), *p1 = P[i].data(), *p2 = P[i + 1].data(),
                            *p3 = P[i + 2 < n ? i + 2 : n - 1].data();
                std::array<float, 3> m{};
                for (int k = 0; k < 3; ++k)
                    m[k] = 0.5f * (2.0f * p1[k] + (-p0[k] + p2[k]) * 0.5f + (2.0f * p0[k] - 5.0f * p1[k] + 4.0f * p2[k] - p3[k]) * 0.25f +
                                   (-p0[k] + 3.0f * p1[k] - 3.0f * p2[k] + p3[k]) * 0.125f);
                pts.push_back(m);
            }
            pts.push_back(P[n - 1]);
        }
        bool blocked = false;
        // Terrain and structures: a hit more than 30 m (level) from the target, and past the first 15 m.
        for (size_t i = 0; i + 1 < pts.size() && !blocked; ++i)
        {
            float hit[3];
            if (!ArcTrace(pawn, pts[i].data(), pts[i + 1].data(), hit))
                continue;
            const float tx = hit[0] - to[0], ty = hit[1] - to[1], fx = hit[0] - from[0], fy = hit[1] - from[1];
            if (tx * tx + ty * ty > 3000.0f * 3000.0f && fx * fx + fy * fy > 1500.0f * 1500.0f)
            {
                blocked = true;
                wchar_t b[128];
                _snwprintf_s(b, _countof(b), _TRUNCATE, L"terrain / a structure at (%.0f, %.0f, %.0f) m, %.0f m out", hit[0] * 0.01f, hit[1] * 0.01f, hit[2] * 0.01f,
                           std::sqrt(fx * fx + fy * fy) * 0.01f);
                why = b;
            }
        }
        // Friendlies: within 12 m of the path, away from the launcher (20 m) and the target (30 m).
        if (!blocked)
        {
            const uint8_t ours = TeamOf(pawn);
            std::vector<API::UObject *> friends;
            if (API::UObject *me = API::get()->get_local_pawn(0))
                if (me != pawn)
                    friends.push_back(me);
            if (ours != 0xFF)
                for (auto *k : Live())
                    if (k != pawn && k != tgt && TeamOf(k) == ours && std::find(friends.begin(), friends.end(), k) == friends.end())
                        friends.push_back(k);
            for (auto *fr : friends)
            {
                if (blocked || IsDeadUnit(fr))
                    continue;
                float p[3] = {};
                if (!CallRet(fr, L"K2_GetActorLocation", {}, p, sizeof(p)))
                    continue;
                for (size_t i = 0; i + 1 < pts.size() && !blocked; ++i)
                {
                    const float *a = pts[i].data(), *b = pts[i + 1].data();
                    const float sx = b[0] - a[0], sy = b[1] - a[1], sz = b[2] - a[2], l2 = sx * sx + sy * sy + sz * sz;
                    if (l2 < 1.0f)
                        continue;
                    float k = ((p[0] - a[0]) * sx + (p[1] - a[1]) * sy + (p[2] - a[2]) * sz) / l2;
                    k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
                    const float c[3] = {a[0] + sx * k, a[1] + sy * k, a[2] + sz * k};
                    const float d2 = (c[0] - p[0]) * (c[0] - p[0]) + (c[1] - p[1]) * (c[1] - p[1]) + (c[2] - p[2]) * (c[2] - p[2]);
                    const float l0 = (c[0] - from[0]) * (c[0] - from[0]) + (c[1] - from[1]) * (c[1] - from[1]);
                    const float l1 = (c[0] - to[0]) * (c[0] - to[0]) + (c[1] - to[1]) * (c[1] - to[1]);
                    if (d2 < 1200.0f * 1200.0f && l0 > 2000.0f * 2000.0f && l1 > 3000.0f * 3000.0f)
                    {
                        blocked = true;
                        why = L"friendly " + ClassName(fr) + L" under the path";
                        LsEmit(LSE_FriendlyInLine, s, fr, 1.0f);
                    }
                }
            }
        }
        auto &vv = ArcVerdicts[s];
        vv.erase(std::remove_if(vv.begin(), vv.end(), [&](const ArcVerdict &v) { return now - v.Ms > 2000 || (v.Tgt == tgt && v.Kind == inf.Kind); }),
                 vv.end());
        ArcVerdict v{tgt, {from[0], from[1], from[2]}, {to[0], to[1], to[2]}, now, inf.Kind, blocked};
        vv.push_back(v);
        return blocked;
    }

    // Direct weapons that fired (fire result Success -> in firing sequence / on cooldown).
    void IcWatchDirect(int s, API::UObject *pawn)
    {
        if (!IcMaskSet[s] || !AiFrOk || !AiFrFn)
            return;
        const ULONGLONG now = GetTickCount64();
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        std::vector<API::UObject *> fresh;
        if (!acc || !ComponentsOn(pawn, acc, fresh))
            return;
        API::UObject *tgt = nullptr;
        if (API::UObject *ctl = ObjProp(pawn, L"Controller"))
            tgt = ObjProp(ctl, L"CombatTarget");
        // The fire check reads differently with no target / another target (vanillahud-83: "NOT
        // HELD" the moment a target arrived, nothing fired): a new target restarts the watch.
        const bool newTgt = tgt != IcWatchTgt[s];
        IcWatchTgt[s] = tgt;
        for (auto &x : IcWpns[s])
        {
            if (x.Indirect != 0 || std::find(fresh.begin(), fresh.end(), x.W) == fresh.end())
                continue;
            std::vector<uint8_t> f(AiFrBuf.size(), 0);
            for (int32_t off : AiFrObjParams)
                std::memcpy(f.data() + off, &tgt, sizeof(tgt));
            x.W->process_event(AiFrFn, f.data());
            const unsigned v = f[AiFrRet];
            if (newTgt)
            {
                x.Last = v;
                continue;
            }
            // Success -> InFiringSequence / OnCooldown: it fired. Not counted while exposed (the
            // plugin fires then) or within 2 s of a plugin start: those are our own shots.
            const bool ours = IcSpotter[s] || (x.OwnMs && now - x.OwnMs < 2000);
            if (x.Last == 0 && (v == 2 || v == 3) && !ours)
            {
                if (IcDirectShots[s]++ < 10)
                    TacLog("tacops: hold (indirect fire), slot %d - direct weapon %ls FIRED while held (NOT HELD)", s, x.Name.c_str());
            }
            x.Last = v;
        }
    }

    // ---- AIM TEST (vanillahud-77). What the game files suggest: the AI's weapon controller puts
    // its FiringAimLocationComponent on the aim point and each weapon fires toward its own
    // AIOverrideAimTarget. Two parts:
    //  WATCH - lancemates whose firing is the game's: each direct weapon's AIOverrideAimTarget is
    //    logged when it changes ("aim watch"), so the log shows what the game puts there.
    //  FIRE  - an EXPOSED lancemate under "hold position (indirect fire only)" no longer gets the
    //    game's firing back: the plugin fires its direct weapons too. Every 250 ms, while the game
    //    says it sees its target (the target is the spotter): the mech is turned to it
    //    (K2_FaceTarget, the game's attack jobs use it), each direct weapon's AIOverrideAimTarget is
    //    set to the target's aim hint component (else its root), and it is started if its own fire
    //    check says Success and heat is under 85 %. It is stopped after 1 s (beam length) and its
    //    old aim target is put back. The log gives the aim error: the mech's GetAimDirection
    //    against the direction to the aim point.
    struct IcDirShot
    {
        API::UObject *W;
        API::UObject *Old;
        ULONGLONG     StopMs;
        bool          Continuous = false; // held while the target stays fireable (rapid fire)
        API::UObject *Tgt = nullptr;      // what it was started at (a new target lets it go)
    };

    std::vector<IcDirShot> IcDirFiring[VanillaExportBlock::kTacMaxMembers + 1];

    int        IcDirFires[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG  IcAimLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    bool       IcAimSigLogged = false;

    static constexpr float kIcAimGateDeg = 3.0f;
    static constexpr float kIcTagAimGateDeg = 10.0f; // lancecontrol-81: a TAG keeps painting while the torso is within 10 deg

    int32_t AimOverrideOff(API::UObject *w)
    {
        static API::UClass *cls = nullptr;
        static int32_t off = -1;
        if (!w || !w->get_class())
            return -1;
        if (w->get_class() != cls)
        {
            cls = w->get_class();
            API::FProperty *p = FindProp(cls, L"AIOverrideAimTarget");
            off = (p && PropType(p) == L"ObjectProperty") ? p->get_offset() : -1;
        }
        return off;
    }

    API::UObject *AimPointOf(API::UObject *tgt)
    {
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        std::vector<API::UObject *> comps;
        if (acc && ComponentsOn(tgt, acc, comps))
        {
            // The target's aim hint (vanillahud-77: shots landed), else the game's own AI aim point
            // (which the AI may move to miss on purpose), else the root.
            for (auto *c : comps)
                if (c && ClassName(c).find(L"AimHint") != std::wstring::npos)
                    return c;
            for (auto *c : comps)
                if (c && ClassName(c).find(L"AIAimLocation") != std::wstring::npos)
                    return c;
        }
        return ObjProp(tgt, L"RootComponent");
    }

    void IcDirStop(int s, bool all, ULONGLONG now)
    {
        API::UObject *pawn = TacUnit[s];
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        std::vector<API::UObject *> fresh;
        const bool have = pawn && acc && IsLive(pawn) && ComponentsOn(pawn, acc, fresh);
        auto &v = IcDirFiring[s];
        for (size_t i = 0; i < v.size();)
        {
            if (!all && now < v[i].StopMs)
            {
                ++i;
                continue;
            }
            if (have && std::find(fresh.begin(), fresh.end(), v[i].W) != fresh.end())
            {
                IcCallNoArgs(v[i].W, L"K2_StopFire");
                const int32_t off = AimOverrideOff(v[i].W);
                if (off >= 0)
                    *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(v[i].W) + off) = v[i].Old;
            }
            v.erase(v.begin() + (ptrdiff_t)i);
        }
    }

    // ---- PER-WEAPON FIRING (vanillahud-89, fields since 90). Each weapon's data asset carries the
    // game's own firing data - the fields MechShaker's relay reads (via K2_GetTraceWeaponData /
    // K2_GetProjectileWeaponData), read here straight from the struct properties:
    //   TraceWeaponData:      Duration (s; 0 = continuous - MG with a RateOfFire, flamer without;
    //                         -1 = TAG, or AMS when it has DamageOverDuration), RateOfFire,
    //                         DamageOverDuration
    //   ProjectileWeaponData: Speed (m/s - converted to cm/s), NumberOfTimesToFire,
    //                         DelayBetweenFiring (s), NumberOfProjectiles
    // Once per weapon class, logged ("weapon stats"). What they drive (IcDirectFire):
    //   laser: hold Duration + 0.1 s, aim hint, no lead;
    //   MG / flamer / TAG: held while the target stays fireable;
    //   AMS: never fired by the plugin (it is the game's own automatic defence);
    //   projectile: held for its burst ((times - 1) x delay + 0.2 s; a single shot = 0.2 s), aimed
    //   at a LEAD point from Speed;
    //   nothing readable: the old 1 s press at the aim hint.
    enum WKind { WK_Unknown = 0, WK_Beam, WK_Continuous, WK_AMS, WK_Projectile };

    struct WStat
    {
        int   Kind = WK_Unknown;
        float Dur = 0.0f, Rof = 0.0f, Dod = 0.0f;     // trace
        float Speed = 0.0f, Delay = 0.0f;             // projectile: cm/s, s
        float Grav = 0.0f;                            // lancecontrol-52: projectile GravityScale (0 = flies straight)
        int   Times = 0, Pellets = 0;
        float Heat = -1.0f, Dmg = -1.0f;              // per shot / beam; -1 unknown (vanillahud-92)
        std::wstring How;
    };

    std::vector<std::pair<API::UClass *, WStat>> WStats;

    // A float / int field of a struct property of `obj` (e.g. TraceWeaponData.Duration).
    bool WsField(API::UObject *obj, API::FProperty *sp, const wchar_t *name, float &out)
    {
        auto *st = reinterpret_cast<API::FStructProperty *>(sp)->get_struct();
        auto *p = st ? StructProp(st, name) : nullptr;
        if (!p) // lancecontrol-52: the game's StatsData nests the fields (StatsData.Trace.Duration, StatsData.Projectile.Speed)
            return st && WsFindNum(st, reinterpret_cast<uint8_t *>(obj) + sp->get_offset(), 0, name, out);
        const uint8_t *at = reinterpret_cast<uint8_t *>(obj) + sp->get_offset() + p->get_offset();
        const std::wstring t = PropType(p);
        if (t == L"FloatProperty")
            out = *reinterpret_cast<const float *>(at);
        else if (t == L"IntProperty")
            out = (float)*reinterpret_cast<const int32_t *>(at);
        else if (t == L"ByteProperty")
            out = (float)*at;
        else
            return false;
        return true;
    }

    // lancecontrol-52: the first float / int / byte field named `name` anywhere in `st` at `base` (nested, 3 deep).
    bool WsFindNum(API::UStruct *st, uint8_t *base, int depth, const wchar_t *name, float &out)
    {
        for (auto *sst = st; sst; sst = sst->get_super_struct())
            for (auto *f = sst->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring n = p->get_fname()->to_string(), t = PropType(p);
                if (_wcsicmp(n.c_str(), name) == 0)
                {
                    const uint8_t *at = base + p->get_offset();
                    if (t == L"FloatProperty")
                        out = *reinterpret_cast<const float *>(at);
                    else if (t == L"IntProperty")
                        out = (float)*reinterpret_cast<const int32_t *>(at);
                    else if (t == L"ByteProperty")
                        out = (float)*at;
                    else
                        continue;
                    return true;
                }
                if (t == L"StructProperty" && depth < 3)
                    if (auto *ss = reinterpret_cast<API::FStructProperty *>(p)->get_struct())
                        if (WsFindNum(ss, base + p->get_offset(), depth + 1, name, out))
                            return true;
            }
        return false;
    }


    // RANGE (lancecontrol-69 / testpilot-plugin-7, user 2026-10-17): a weapon's rated range is the MaxRange float of a
    // WeaponRanges struct - StatsData.Range on projectile / missile weapons, StatsData.Ranges on trace weapons (MG, lasers,
    // flamer) - ALWAYS in cm (the mod kit's assets: MG 13000, LRM5 90000). The old "under 20000 = metres" rule read a
    // 195 m MG (19500 cm) as 19.5 km. Only WeaponRanges is accepted, never other range data (MWO_RangeData).
    bool WsFindRange(API::UStruct *st, uint8_t *base, int depth, bool inRanges, float &out, std::wstring &path,
                            const std::wstring &pre = L"")
    {
        for (auto *sst = st; sst; sst = sst->get_super_struct())
            for (auto *f = sst->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring n = p->get_fname()->to_string(), t = PropType(p);
                if (inRanges && t == L"FloatProperty" && _wcsicmp(n.c_str(), L"MaxRange") == 0)
                {
                    out = *reinterpret_cast<float *>(base + p->get_offset());
                    path = pre + n;
                    return true;
                }
                if (t == L"StructProperty" && depth < 3)
                    if (auto *ss = reinterpret_cast<API::FStructProperty *>(p)->get_struct())
                        if (WsFindRange(ss, base + p->get_offset(), depth + 1,
                                        _wcsicmp(ss->get_fname()->to_string().c_str(), L"WeaponRanges") == 0, out, path,
                                        pre + n + L"."))
                            return true;
            }
        return false;
    }

    // The first float field named `name` anywhere in `st` at `base` (nested structs, 3 deep).
    bool WsFind(API::UStruct *st, uint8_t *base, int depth, const wchar_t *name, float &out, std::wstring &path,
                const std::wstring &pre = L"")
    {
        for (auto *sst = st; sst; sst = sst->get_super_struct())
            for (auto *f = sst->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring n = p->get_fname()->to_string(), t = PropType(p);
                if (t == L"FloatProperty" && _wcsicmp(n.c_str(), name) == 0)
                {
                    out = *reinterpret_cast<float *>(base + p->get_offset());
                    path = pre + n;
                    return true;
                }
                if (t == L"StructProperty" && depth < 3)
                    if (auto *ss = reinterpret_cast<API::FStructProperty *>(p)->get_struct())
                        if (WsFind(ss, base + p->get_offset(), depth + 1, name, out, path, pre + n + L"."))
                            return true;
            }
        return false;
    }

    // By value (vanillahud-92): nothing holds a reference into the growing cache.
    WStat WeaponStat(API::UObject *w)
    {
        API::UClass *cls = w->get_class();
        for (auto &e : WStats)
            if (e.first == cls)
                return e.second;
        WStat st;
        API::UObject *da = nullptr;
        if (FindFn(w, L"GetWeaponDataAsset"))
            CallRet(w, L"GetWeaponDataAsset", {}, &da, sizeof(da));
        wchar_t b[320] = L"no data asset";
        if (da && da->get_class())
        {
            API::FProperty *tp = FindProp(da->get_class(), L"TraceWeaponData");
            API::FProperty *pp = FindProp(da->get_class(), L"ProjectileWeaponData");
            if (tp && PropType(tp) != L"StructProperty")
                tp = nullptr;
            if (pp && PropType(pp) != L"StructProperty")
                pp = nullptr;
            // lancecontrol-52 (user 2026-10-13 "just fix the drop and leading"): the game now keeps the stats in one
            // StatsData struct whose TYPE says the kind (TraceWeaponData / ProjectileWeaponData / MissileWeaponData,
            // fields nested one level down). Without this every weapon read "no usable ..." - no lead, every weapon a
            // 1 s press (MG / flamer stuttered, long beams cut short).
            if (!tp && !pp)
                if (API::FProperty *sd = FindProp(da->get_class(), L"StatsData"))
                    if (PropType(sd) == L"StructProperty")
                        if (auto *ss = reinterpret_cast<API::FStructProperty *>(sd)->get_struct())
                        {
                            const std::wstring sn = ss->get_fname()->to_string();
                            if (sn.find(L"Trace") != std::wstring::npos)
                                tp = sd;
                            else if (sn.find(L"Projectile") != std::wstring::npos || sn.find(L"Missile") != std::wstring::npos)
                                pp = sd;
                        }
            // A projectile speed wins (vanillahud-92): an asset carrying both structs is a projectile
            // weapon unless its trace data says something (a duration, a rate of fire, damage).
            float spd = 0.0f;
            const bool isProj = pp && WsField(da, pp, L"Speed", spd) && spd > 0.0f;
            float dur = 0.0f, rof = 0.0f, dod = 0.0f;
            bool isTrace = false;
            if (tp && WsField(da, tp, L"Duration", dur))
            {
                WsField(da, tp, L"RateOfFire", rof);
                WsField(da, tp, L"DamageOverDuration", dod);
                std::wstring lo = ClassName(w);
                for (auto &ch : lo)
                    ch = (wchar_t)towlower(ch);
                const bool named = lo.find(L"laser") != std::wstring::npos || lo.find(L"_mg") != std::wstring::npos ||
                                   lo.find(L"flamer") != std::wstring::npos || lo.find(L"_tag") != std::wstring::npos ||
                                   lo.find(L"ams") != std::wstring::npos;
                isTrace = (dur != 0.0f || rof > 0.0f || dod > 0.0f || named) && !(isProj && !named && dur == 0.0f && rof <= 0.0f);
            }
            if (isTrace)
            {
                st.Dur = dur;
                st.Rof = rof;
                st.Dod = dod;
                if (st.Dur > 0.0f)
                    st.Kind = WK_Beam;
                else if (st.Dur < -0.5f && st.Dod > 0.0f)
                    st.Kind = WK_AMS;
                else
                    st.Kind = WK_Continuous; // MG (RateOfFire), flamer (none), TAG (-1, no damage)
                _snwprintf_s(b, _countof(b), _TRUNCATE, L"TraceWeaponData Duration=%.2f RateOfFire=%.2f DamageOverDuration=%.2f -> %ls", st.Dur, st.Rof, st.Dod,
                           st.Kind == WK_Beam ? L"beam" : (st.Kind == WK_AMS ? L"AMS (not fired)" : L"continuous"));
            }
            else if (isProj)
            {
                float t = 0.0f, n = 0.0f;
                WsField(da, pp, L"NumberOfTimesToFire", t);
                WsField(da, pp, L"DelayBetweenFiring", st.Delay);
                WsField(da, pp, L"NumberOfProjectiles", n);
                st.Times = (int)t;
                st.Pellets = (int)n;
                if (!(st.Delay >= 0.0f && st.Delay < 5.0f))
                    st.Delay = 0.0f;
                st.Speed = spd > 5000.0f ? spd : spd * 100.0f; // lancecontrol-52: StatsData speeds are cm/s already (AC20 65000)
                float g = 0.0f;
                if (WsField(da, pp, L"GravityScale", g) && g > 0.0f && g < 50.0f)
                    st.Grav = g;
                st.Kind = WK_Projectile;
                _snwprintf_s(b, _countof(b), _TRUNCATE, L"ProjectileWeaponData Speed=%.0f m/s GravityScale=%.2f NumberOfTimesToFire=%d DelayBetweenFiring=%.2f NumberOfProjectiles=%d",
                           st.Speed * 0.01f, st.Grav, st.Times, st.Delay, st.Pellets);
            }
            else
                _snwprintf_s(b, _countof(b), _TRUNCATE, L"no usable TraceWeaponData / ProjectileWeaponData on %ls", ClassName(da).c_str());
            // Heat and damage for the heat budget (vanillahud-92): by name, anywhere in the asset.
            std::wstring hp, dp;
            float v = 0.0f;
            for (const wchar_t *hn : {L"HeatGenerated", L"HeatGeneratedOverDuration", L"Heat"})
                if (st.Heat < 0.0f && WsFind(da->get_class(), reinterpret_cast<uint8_t *>(da), 0, hn, v, hp) && v > 0.0f)
                    st.Heat = v;
            for (const wchar_t *dn : {L"Damage", L"DamageOverDuration"})
                if (st.Dmg < 0.0f && WsFind(da->get_class(), reinterpret_cast<uint8_t *>(da), 0, dn, v, dp) && v > 0.0f)
                    st.Dmg = v;
            if (st.Pellets > 1 && st.Dmg > 0.0f && dp.find(L"Projectile") != std::wstring::npos)
                st.Dmg *= (float)st.Pellets;
            const size_t k = wcslen(b);
            _snwprintf_s(b + k, 320 - k, _TRUNCATE, L" | heat %ls=%.2f damage %ls=%.2f", hp.empty() ? L"-" : hp.c_str(), st.Heat,
                         dp.empty() ? L"-" : dp.c_str(), st.Dmg);
        }
        st.How = b;
        TacLog("weapon stats - %ls: %ls", ClassName(w).c_str(), st.How.c_str());
        WStats.push_back({cls, st});
        return st;
    }

    // LEAD. A projectile weapon aims at where the target WILL be: the aim hint plus the target's
    // velocity x the flight time (from the weapon's muzzle, two passes). The point is marked with
    // the target's own MWAIAimLocationComponent - the component the game's AI aims its weapons at
    // (vanillahud-77 aim watch) - moved there just before that weapon fires, and put back 1.5 s
    // later. No such component, no speed, or a target barely moving: the aim hint, as before.
    struct IcMovedAim
    {
        API::UObject *Comp, *Owner;
        float         Rel[3];
        ULONGLONG     RestoreMs;
    };

    std::vector<IcMovedAim> IcMoved;

    API::UObject *AiAimCompOf(API::UObject *tgt)
    {
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        std::vector<API::UObject *> comps;
        if (acc && ComponentsOn(tgt, acc, comps))
            for (auto *c : comps)
                if (c && ClassName(c).find(L"AIAimLocation") != std::wstring::npos)
                    return c;
        return nullptr;
    }

    // AIM MARKS (lancecontrol-52, user 2026-10-13 "just fix the drop and leading"). A projectile weapon aims (through its
    // AIOverrideAimTarget) at one of our own invisible markers - Engine TargetPoint actors, spawned on demand (pool of up
    // to 24 per mission, cleared in ForgetWorldLance) - placed where the round must be aimed:
    //   the aim point + the target's velocity x flight time (lead), raised by 1/2 g GravityScale t^2 (drop);
    //   flight time from the weapon's muzzle at the weapon's speed, three passes, capped at 4 s.
    // Each mark is the weapon's alone until its trigger is let go (+0.3 s), so weapons of different speeds never share
    // one. Under 0.5 m of correction, or no mark available: the aim point itself, as before. World gravity from the
    // player's movement component (GetGravityZ / its GravityScale), per mission; -980 cm/s2 if unreadable.
    struct AimMark
    {
        API::UObject *Actor, *Root;
        ULONGLONG     BusyUntil;
    };
    std::vector<AimMark> AimMarks;
    API::UClass         *AmCls = nullptr;
    bool                 AmFailed = false;
    float                AmGravZ = 0.0f;
    int                  AmSpawned = 0;

    float AmGravity(API::UObject *pawn)
    {
        if (AmGravZ < 0.0f)
            return AmGravZ;
        // The WORLD's gravity is what a projectile falls by (its movement component: world gravity x GravityScale) -
        // read as the engine does (AWorldSettings::GetGravityZ): WorldGravityZ once set, else GlobalGravityZ when
        // bGlobalGravitySet, else PhysicsSettings.DefaultGravityZ. The mech's movement component is logged beside it
        // (a mech may fall by its own gravity); -980 cm/s2 when nothing is readable.
        auto fl = [&](API::UObject *o, const wchar_t *n, float &v) {
            const int32_t off = o ? LsOff(o, n, L"FloatProperty") : -1;
            if (off < 0)
                return false;
            v = *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(o) + off);
            return true;
        };
        auto bo = [&](API::UObject *o, const wchar_t *n) {
            API::FProperty *bp = o ? LsBool(o->get_class(), n) : nullptr;
            return bp && reinterpret_cast<API::FBoolProperty *>(bp)->get_value_from_object(o);
        };
        float wWorld = 0.0f, wGlobal = 0.0f, wDefault = 0.0f, wMech = 0.0f;
        bool  wWorldSet = false, wGlobalSet = false, haveWs = false, haveDef = false, haveMech = false;
        if (auto *wsc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.WorldSettings"))
        {
            std::vector<API::UObject *> v;
            if (ActorsOf(pawn, wsc, v) && !v.empty())
            {
                haveWs = true;
                wWorldSet = bo(v[0], L"bWorldGravitySet") && fl(v[0], L"WorldGravityZ", wWorld);
                wGlobalSet = bo(v[0], L"bGlobalGravitySet") && fl(v[0], L"GlobalGravityZ", wGlobal);
            }
        }
        if (auto *psc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.PhysicsSettings"))
            haveDef = fl(psc->get_class_default_object(), L"DefaultGravityZ", wDefault);
        if (API::UObject *me = API::get()->get_local_pawn(0))
            if (API::UObject *cm = LsObj(me, L"CharacterMovement"))
            {
                float gz = 0.0f, sc = 1.0f;
                fl(cm, L"GravityScale", sc);
                if (CallRet(cm, L"GetGravityZ", {}, &gz, sizeof(gz)) && sc > 0.01f)
                {
                    wMech = gz / sc;
                    haveMech = true;
                }
            }
        const char *how = "default -980";
        AmGravZ = -980.0f;
        if (wWorldSet && wWorld < 0.0f)
            AmGravZ = wWorld, how = "WorldSettings.WorldGravityZ";
        else if (wGlobalSet && wGlobal < 0.0f)
            AmGravZ = wGlobal, how = "WorldSettings.GlobalGravityZ";
        else if (haveDef && wDefault < 0.0f)
            AmGravZ = wDefault, how = "PhysicsSettings.DefaultGravityZ";
        LogInfo("aim marks: world gravity %.0f cm/s2 (%s) - world settings %s (WorldGravityZ %s%.0f, GlobalGravityZ %s%.0f), "
                "physics default %s%.0f, the player's mech movement %s%.0f",
                AmGravZ, how, haveWs ? "found" : "NOT found", wWorldSet ? "" : "unset ", wWorld, wGlobalSet ? "" : "unset ", wGlobal,
                haveDef ? "" : "unread ", wDefault, haveMech ? "" : "unread ", wMech);
        return AmGravZ;
    }

    AimMark *AmTake(API::UObject *pawn, ULONGLONG now)
    {
        for (auto &m : AimMarks)
            if (now >= m.BusyUntil)
                return &m;
        if (AmFailed || AimMarks.size() >= 24)
            return nullptr;
        if (!AmCls)
            AmCls = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.TargetPoint");
        if (!GameplayStatics)
            if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameplayStatics"))
                GameplayStatics = c->get_class_default_object();
        if (!AmCls || !GameplayStatics)
        {
            AmFailed = true;
            LogWarn("aim marks: %s not found - projectile weapons aim at the aim point without lead or drop",
                    AmCls ? "GameplayStatics" : "Engine.TargetPoint");
            return nullptr;
        }
        float xf[12] = {0, 0, 0, 1, 0, 0, -100000.0f, 0, 1, 1, 1, 0}; // FTransform: rotation, translation, scale
        uint8_t coll = 1; // AlwaysSpawn
        API::UObject *a = nullptr, *b = nullptr;
        if (!CallRet(GameplayStatics, L"BeginDeferredActorSpawnFromClass",
                     {{L"WorldContextObject", &pawn, sizeof(pawn)}, {L"ActorClass", &AmCls, sizeof(AmCls)}, {L"SpawnTransform", xf, sizeof(xf)},
                      {L"CollisionHandlingOverride", &coll, 1}},
                     &a, sizeof(a)) ||
            !a ||
            !CallRet(GameplayStatics, L"FinishSpawningActor", {{L"Actor", &a, sizeof(a)}, {L"SpawnTransform", xf, sizeof(xf)}}, &b, sizeof(b)))
        {
            AmFailed = true;
            LogWarn("aim marks: spawning a TargetPoint failed - projectile weapons aim at the aim point without lead or drop");
            return nullptr;
        }
        API::UObject *root = LsObj(a, L"RootComponent");
        if (!root)
        {
            AmFailed = true;
            LogWarn("aim marks: the TargetPoint has no root component - no lead or drop");
            return nullptr;
        }
        AimMarks.push_back({a, root, 0});
        ++AmSpawned;
        if (AmSpawned <= 3 || AmSpawned % 8 == 0)
            LogInfo("aim marks: %zu in this mission", AimMarks.size());
        return &AimMarks.back();
    }

    API::UObject *IcAimMark(API::UObject *pawn, API::UObject *w, API::UObject *tgt, const float *hint, const WStat &ws, ULONGLONG now,
                            ULONGLONG hold, float &leadM, float &dropM)
    {
        leadM = dropM = 0.0f;
        if (ws.Speed <= 0.0f)
            return nullptr;
        float v[3] = {};
        if (!CallRet(tgt, L"GetVelocity", {}, v, sizeof(v)))
            v[0] = v[1] = v[2] = 0.0f;
        float m[3] = {};
        if (!CallRet(w, L"K2_GetComponentLocation", {}, m, sizeof(m)))
            return nullptr;
        const float g = ws.Grav > 0.0f ? -AmGravity(pawn) * ws.Grav : 0.0f; // cm/s2, downward
        float p[3] = {hint[0], hint[1], hint[2]};
        float drop = 0.0f;
        for (int it = 0; it < 3; ++it)
        {
            const float dx = p[0] - m[0], dy = p[1] - m[1], dz = p[2] - drop - m[2];
            const float t = (std::min)(std::sqrt(dx * dx + dy * dy + dz * dz) / ws.Speed, 4.0f);
            drop = 0.5f * g * t * t;
            for (int k = 0; k < 3; ++k)
                p[k] = hint[k] + v[k] * t;
            p[2] += drop;
        }
        const float lx = p[0] - hint[0], ly = p[1] - hint[1], lz = p[2] - drop - hint[2];
        leadM = std::sqrt(lx * lx + ly * ly + lz * lz) * 0.01f;
        dropM = drop * 0.01f;
        if (leadM + dropM < 0.5f)
            return nullptr; // nothing worth correcting: the aim point itself
        AimMark *mk = AmTake(pawn, now);
        if (!mk)
            return nullptr;
        uint8_t tele = 1;
        if (!CallRet(mk->Root, L"K2_SetWorldLocation", {{L"NewLocation", p, sizeof(p)}, {L"bTeleport", &tele, 1}}, nullptr, 0))
            return nullptr;
        mk->BusyUntil = now + hold + 300;
        return mk->Root;
    }

    // Returns the component to aim at (moved to the lead point), or nullptr: use the aim hint.
    API::UObject *IcLeadAim(API::UObject *w, API::UObject *tgt, const float *hint, float speed, ULONGLONG now, ULONGLONG hold, float &leadM)
    {
        leadM = 0.0f;
        if (speed <= 0.0f)
            return nullptr;
        float v[3] = {};
        if (!CallRet(tgt, L"GetVelocity", {}, v, sizeof(v)) || v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 100.0f * 100.0f)
            return nullptr; // under 1 m/s: no lead needed
        API::UObject *comp = AiAimCompOf(tgt);
        if (!comp)
            return nullptr;
        float m[3] = {};
        if (!CallRet(w, L"K2_GetComponentLocation", {}, m, sizeof(m)))
            return nullptr;
        float p[3] = {hint[0], hint[1], hint[2]};
        for (int it = 0; it < 2; ++it)
        {
            const float dx = p[0] - m[0], dy = p[1] - m[1], dz = p[2] - m[2];
            const float t = (std::min)(std::sqrt(dx * dx + dy * dy + dz * dz) / speed, 3.0f); // capped: a bad speed
            for (int k = 0; k < 3; ++k)
                p[k] = hint[k] + v[k] * t;
        }
        // Remember where it was (once per component until put back).
        const ULONGLONG back = now + (std::max)((ULONGLONG)1500, hold + 200); // after the whole burst
        bool known = false;
        for (auto &e : IcMoved)
            if (e.Comp == comp)
            {
                known = true;
                e.RestoreMs = (std::max)(e.RestoreMs, back);
            }
        if (!known)
        {
            IcMovedAim e{comp, tgt, {0, 0, 0}, back};
            auto *rp = FindProp(comp->get_class(), L"RelativeLocation");
            if (!rp || PropType(rp) != L"StructProperty")
                return nullptr; // could not put it back: leave it alone
            std::memcpy(e.Rel, reinterpret_cast<uint8_t *>(comp) + rp->get_offset(), sizeof(e.Rel));
            IcMoved.push_back(e);
        }
        uint8_t tele = 1;
        if (!CallRet(comp, L"K2_SetWorldLocation", {{L"NewLocation", p, sizeof(p)}, {L"bTeleport", &tele, 1}}, nullptr, 0))
            return nullptr;
        const float lx = p[0] - hint[0], ly = p[1] - hint[1], lz = p[2] - hint[2];
        leadM = std::sqrt(lx * lx + ly * ly + lz * lz) * 0.01f;
        return comp;
    }

    // AIM SPREAD: lancecontrol-28 put each volley on a point (scattered hit / near miss) by moving
    // the target's MWAIAimLocationComponent - enemy mechs mostly carry none, so it never ran. Removed
    // in lancecontrol-31f; the spread is now by aim-hint component (IcAimComp).
    std::mt19937 IcRng{(uint32_t)GetTickCount64()};

    bool IcBounds(API::UObject *tgt, float *origin, float *extent)
    {
        uint8_t yes = 1;
        if (!CallRet(tgt, L"GetActorBounds", {{L"bOnlyCollidingComponents", &yes, 1}}, nullptr, 0))
            return false;
        API::UFunction *fn = FindFn(tgt, L"GetActorBounds");
        const int32_t oO = fn ? ParamOffset(fn, L"Origin") : -1, oE = fn ? ParamOffset(fn, L"BoxExtent") : -1;
        if (oO < 0 || oE < 0)
            return false;
        std::memcpy(origin, CallBuf + oO, 12);
        std::memcpy(extent, CallBuf + oE, 12);
        return extent[0] > 10.0f && extent[1] > 10.0f && extent[2] > 10.0f && extent[0] < 5000.0f && extent[2] < 5000.0f;
    }

    // =======================================================================================
    // CALLED SHOT PROBE (lancecontrol-31). Can the lance aim at a body part ("go for the legs")?
    //  - On every new lock of yours on a mech (once per chassis class): which of LidarExport's joint
    //    names (Thigh_Left, Calf_Right, Torso_Centre, Upperarm_Left ... - verified there for the
    //    shelved silhouettes) the mesh resolves through GetSocketLocation, and where they sit.
    //  - [lance] called_shot_probe = legs | arms | ct | head | sides (GlobalSettings.ini, read live):
    //    insane / impossibly strong allies put their hit volleys on that part (IaTick, the AIM SPREAD
    //    point is swapped); vanilla allies: the game's AI aim component of YOUR locked target is moved
    //    onto the part every 250 ms (the game's own aim + miss logic stays).
    //  - While on: every 5 s the locked target's armour / structure per part, so the log shows
    //    whether the called part is the one going down. Log lines: "called shot".
    // =======================================================================================
    int                  CsPart = -1;
    API::UFunction      *CsFn = nullptr;
    int                  CsFrame = -1, CsName = -1, CsRet = -1;
    bool                 CsTried = false, CsOk = false;
    std::vector<uint8_t> CsBuf;
    std::set<std::wstring> CsSeen;
    API::UObject        *CsLast = nullptr;
    ULONGLONG            CsTickMs = 0, CsCondMs = 0;
    int                  CsAimed = 0, CsFlip = 0;

    bool CsReady()
    {
        if (CsTried)
            return CsOk;
        CsTried = true;
        auto *skc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");
        for (API::UStruct *st = skc; st && !CsFn; st = st->get_super_struct())
            CsFn = static_cast<API::UClass *>(st)->find_function(L"GetSocketLocation");
        if (!CsFn)
        {
            LogInfo("called shot: GetSocketLocation not found - probe off");
            return false;
        }
        CsFrame = CsFn->get_properties_size();
        for (auto *f = CsFn->get_child_properties(); f; f = f->get_next())
        {
            auto *pr = reinterpret_cast<API::FProperty *>(f);
            const std::wstring nm = f->get_fname()->to_string();
            if (nm == L"InSocketName")
                CsName = pr->get_offset();
            else if (nm == L"ReturnValue")
                CsRet = pr->get_offset();
        }
        CsOk = CsFrame > 0 && CsName >= 0 && CsRet >= 0;
        if (CsOk)
            CsBuf.assign((size_t)CsFrame + 64, 0);
        LogInfo("called shot: GetSocketLocation %s (frame %d, name@%d, ret@%d)", CsOk ? "ready" : "layout incomplete", CsFrame, CsName,
                CsRet);
        return CsOk;
    }
    static API::UObject *CsMesh(API::UObject *pawn)
    {
        auto *pp = pawn ? pawn->get_property_data<void *>(L"MechMeshComponent") : nullptr;
        return (pp && *pp) ? reinterpret_cast<API::UObject *>(*pp) : nullptr;
    }
    // One joint's world position; false if the name is unknown to this mesh (UE returns the component's own location).
    bool CsJoint(API::UObject *mesh, const wchar_t *bone, float out[3])
    {
        if (!mesh || !CsReady())
            return false;
        std::memset(CsBuf.data(), 0, CsBuf.size());
        API::FName name(bone, API::FName::EFindName::Find);
        std::memcpy(CsBuf.data() + CsName, &name, sizeof(name));
        mesh->process_event(CsFn, CsBuf.data());
        std::memcpy(out, CsBuf.data() + CsRet, sizeof(float) * 3);
        float base[3];
        if (CallRet(mesh, L"K2_GetComponentLocation", {}, base, sizeof(base)))
        {
            const float dx = out[0] - base[0], dy = out[1] - base[1], dz = out[2] - base[2];
            if (dx * dx + dy * dy + dz * dz < 1.0f)
                return false; // unknown name: the component's own location
        }
        return out[0] != 0.0f || out[1] != 0.0f || out[2] != 0.0f;
    }
    // The called part's point on tgt (legs / arms / sides alternate left and right; a little scatter for insane).
    // lancecontrol-31b: the game's OWN per-part aim points. ObjectDump: MWMechMeshComponent carries eight
    // MWAimHintComponent subobjects (AimHintComponent_1..8 - eight = the eight paperdoll parts; the health
    // containers HealthContainer_LeftLeg ... are plain health objects with no position). Which hint is which part
    // is not known yet: matched by the hint's attach socket / name words, logged per chassis.
    std::map<API::UClass *, std::array<std::vector<int>, 5>> CsHintMap; // chassis class -> part (legs, arms, ct, head, sides) -> hint indices
    API::UClass *CsHintCls = nullptr;
    bool CsHints(API::UObject *tgt, std::vector<API::UObject *> &hints)
    {
        if (!CsHintCls)
            CsHintCls = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWAimHintComponent");
        return CsHintCls && ComponentsOn(tgt, CsHintCls, hints) && !hints.empty();
    }
    static std::wstring CsHintKey(API::UObject *h)
    {
        std::wstring k = h->get_fname() ? h->get_fname()->to_string() : L"";
        if (auto *sn = h->get_property_data<API::FName>(L"AttachSocketName"))
            k += L" " + sn->to_string();
        for (auto &c : k)
            c = (wchar_t)towlower(c);
        return k;
    }
    static int CsPartOfKey(const std::wstring &k)
    {
        auto has = [&](const wchar_t *x) { return k.find(x) != std::wstring::npos; };
        if (has(L"leg") || has(L"thigh") || has(L"calf") || has(L"knee") || has(L"foot") || has(L"pelvis"))
            return 0;
        if (has(L"clavicle")) // lancecontrol-31c: the game puts the side-torso aim hints on the clavicles
            return 4;
        if (has(L"arm") || has(L"hand") || has(L"shoulder"))
            return 1;
        if (has(L"head") || has(L"cockpit"))
            return 3;
        if (has(L"centre") || has(L"center") || has(L"_ct") || has(L"spine"))
            return 2;
        if (has(L"torso_l") || has(L"torso_r") || has(L"lefttorso") || has(L"righttorso") || has(L"side"))
            return 4;
        return -1;
    }
    bool CsHintPoint(API::UObject *tgt, float p[3])
    {
        std::vector<API::UObject *> hints;
        if (!tgt || !tgt->get_class() || !CsHints(tgt, hints))
            return false;
        // lancecontrol-31c: worked out per target, every time (8 hints, cheap). It was cached per
        // class, but every mech is DerivedMech_C, so the first chassis' order was used for all.
        std::vector<int> idx;
        for (int i = 0; i < (int)hints.size(); ++i)
            if (CsPartOfKey(CsHintKey(hints[i])) == CsPart)
                idx.push_back(i);
        if (idx.empty())
            return false;
        const int i = idx[(size_t)(CsFlip++) % idx.size()];
        return i < (int)hints.size() && CallRet(hints[i], L"K2_GetComponentLocation", {}, p, sizeof(float) * 3);
    }
    int CsFromHint = 0, CsFromBone = 0;
    // lancecontrol-31d: the aim hint COMPONENT of the called part - used as the aim target itself
    // (direct fire aims at a component; enemies carry no MWAIAimLocationComponent to move, so the
    // point-based path never ran). Left/right alternate every 2 s, not every tick (the torso gate).
    // The aim hint of a part (0 legs, 1 arms, 2 ct, 3 head, 4 sides); side = which of a pair.
    API::UObject *CsPartComp(API::UObject *tgt, int part, size_t side)
    {
        std::vector<API::UObject *> hints;
        if (part < 0 || !tgt || !CsHints(tgt, hints))
            return nullptr;
        std::vector<API::UObject *> pick;
        for (auto *h : hints)
            if (h && CsPartOfKey(CsHintKey(h)) == part)
                pick.push_back(h);
        return pick.empty() ? nullptr : pick[side % pick.size()];
    }
    // lancecontrol-59: is a cached aim hint still one of the target's components? Pointer comparison only (a freed
    // hint is never dereferenced) - the target's own component list is read fresh.
    bool CsHintStill(API::UObject *tgt, API::UObject *h)
    {
        std::vector<API::UObject *> hints;
        return h && tgt && CsHints(tgt, hints) && std::find(hints.begin(), hints.end(), h) != hints.end();
    }
    // lancecontrol-32: CALLED SHOT ORDERS (TCO_CalledShot) - per lancemate: the part and the one unit it is for.
    uint8_t       CsOrdPart[VanillaExportBlock::kTacMaxMembers + 1] = {};
    API::UObject *CsOrdTgt[VanillaExportBlock::kTacMaxMembers + 1] = {};
    static const wchar_t *CsPartName(int code)
    {
        static const wchar_t *k[] = {L"cancel", L"legs", L"left leg", L"right leg", L"arms", L"left arm", L"right arm",
                                     L"side torsos", L"left torso", L"right torso", L"centre torso", L"head"};
        return code >= 0 && code <= 11 ? k[code] : L"?";
    }
    // The aim hint for an order's part code: the group (CsPartOfKey) and, for a left / right one, the hint
    // whose name or socket says that side (the mech's own); a pair alternates every 2 s.
    API::UObject *CsOrderComp(API::UObject *tgt, int code)
    {
        static const int kGroup[] = {-1, 0, 0, 0, 1, 1, 1, 4, 4, 4, 2, 3};
        static const int kSide[] = {-1, -1, 0, 1, -1, 0, 1, -1, 0, 1, -1, -1}; // 0 left, 1 right
        if (code < 1 || code > 11)
            return nullptr;
        std::vector<API::UObject *> hints;
        if (!tgt || !CsHints(tgt, hints))
            return nullptr;
        std::vector<API::UObject *> pick, side;
        for (auto *h : hints)
        {
            if (!h)
                continue;
            const std::wstring k = CsHintKey(h);
            if (CsPartOfKey(k) != kGroup[code])
                continue;
            pick.push_back(h);
            if (kSide[code] >= 0 && k.find(kSide[code] == 0 ? L"left" : L"right") != std::wstring::npos)
                side.push_back(h);
        }
        const std::vector<API::UObject *> &use = !side.empty() ? side : pick;
        return use.empty() ? nullptr : use[(size_t)(GetTickCount64() / 2000) % use.size()];
    }
    bool CsOrder(API::UObject *pc, const VanillaExportBlock::TacCmd &c)
    {
        const int s = c.Slot, part = c.Pad[0];
        if (part < 1 || part > 11)
        {
            TacLog("called shot: slot %d - cancelled", s);
            CsOrdPart[s] = 0;
            CsOrdTgt[s] = nullptr;
            return true;
        }
        API::UObject *tgt = c.Pad[1] == 1 ? AiPlayerLock() : TacTargetOf(pc, c.Loc); // lancecontrol-61: a named one exactly
        if (!tgt || !IsLive(tgt) || IsDeadUnit(tgt))
        {
            TacLog("called shot: slot %d - %ls, but no target (%ls)", s, CsPartName(part),
                   c.Pad[1] == 1 ? L"you have no lock"
                                 : (c.Loc[2] == VanillaExportBlock::kTacLocActor ? L"the named unit is gone" : L"nothing within 40 m of the point"));
            return false;
        }
        if (!IsMechUnit(tgt))
        {
            // lancecontrol-42: a vehicle / turret / VTOL has no legs, arms or torsos to call (VoiceCommand says so too)
            TacLog("called shot: slot %d - %ls on %ls: not a mech - refused", s, CsPartName(part), ClassName(tgt).c_str());
            return false;
        }
        CsOrdPart[s] = (uint8_t)part;
        CsOrdTgt[s] = tgt;
        TacLog("called shot: slot %d - %ls on %ls (%ls)", s, CsPartName(part), CsChassis(tgt).c_str(),
               c.Pad[1] == 1 ? L"your lock" : L"named");
        // and the attack on that unit - the exact unit (lancecontrol-61; was the unit nearest its position)
        VanillaExportBlock::TacCmd a = c;
        a.Order = VanillaExportBlock::TCO_Attack;
        {
            uint32_t lo = 0, hi = 0;
            LsSplit(tgt, lo, hi);
            std::memcpy(&a.Loc[0], &lo, 4);
            std::memcpy(&a.Loc[1], &hi, 4);
            a.Loc[2] = VanillaExportBlock::kTacLocActor;
        }
        const bool ok = TacPost(pc, a);
        if (ok && AtkTgt[s] && AtkTgt[s] != tgt)
            AtkTgt[s] = tgt; // a unit standing right beside it was picked: keep the called one
        AtkByLock[s] = c.Pad[1] == 1; // lancecontrol-34: "get his legs" follows your lock, "the Atlas's legs" its sight
        return ok;
    }
    // ATTACK ORDERS END WITH THEIR TARGET (lancecontrol-34, user 2026-10-11). The game's own attack job only ends
    // on the target's death (Ally_TargetAttackJob::OnTargetLivingActorDeath) and insane allies pin the target - so a
    // lancemate kept chasing a mech it could not see. Now (user, 2026-10-11): any attack order ends when its unit
    // dies, or when no lancemate has seen it for 10 s (lost, or never seen at all; 1 s until lancecontrol-60). Your lock does not matter. The
    // lancemate then ENGAGES OTHER THREATS: an attack on the nearest enemy it sees itself (that order follows the
    // same rule); only when it sees none, a Follow. Then: its called shot on that unit cleared, a Follow (back to you, free targeting), and
    // LSE_AttackDropped for the pilot's line. A dead target ends it quietly (the kill callout says it).
    bool      AtkByLock[VanillaExportBlock::kTacMaxMembers + 1] = {};
    // lancecontrol-60 (user, 2026-10-16): 10 s, was 1 s - a called shot / attack on a unit only your sensors had was
    // dropped as "never seen" before a lancemate could get eyes on it.
    static constexpr ULONGLONG kAtkUnseenMs = 10000;
    ULONGLONG AtkSinceMs[VanillaExportBlock::kTacMaxMembers + 1] = {}, AtkSeenMs[VanillaExportBlock::kTacMaxMembers + 1] = {},
              AtkLockLostMs[VanillaExportBlock::kTacMaxMembers + 1] = {}, AtkWatchMs = 0;
    void AtkDrop(API::UObject *pc, int s, int why)
    {
        API::UObject *t = AtkTgt[s];
        static const char *kWhy[] = {"", "", "the lance lost sight of it", "never seen"};
        TacLog("attack order, slot %d - %ls: %s - dropped, back to you (free targeting)", s, ClassName(t).c_str(), kWhy[why]);
        if (CsOrdTgt[s] == t)
        {
            CsOrdPart[s] = 0;
            CsOrdTgt[s] = nullptr;
        }
        AtkTgt[s] = nullptr;
        LsEmit(LSE_AttackDropped, s, t, (float)why);
        // engaging other threats: the nearest enemy this lancemate sees itself
        API::UObject *pawn = TacUnit[s], *pick = nullptr;
        float here[3] = {}, best = 1e30f;
        API::UObject *ctl = pawn && IsLive(pawn) && !IsDeadUnit(pawn) ? ObjProp(pawn, L"Controller") : nullptr;
        if (ctl && CallRet(pawn, L"K2_GetActorLocation", {}, here, sizeof(here)))
        {
            const uint8_t ours = TeamOf(pawn);
            for (auto *e : OwnVisible(ctl))
            {
                if (!e || e == t || e == pawn || TeamOf(e) == ours || TeamOf(e) == 0xFF || IsDeadUnit(e) || !IsCombatUnit(e))
                    continue;
                float p[3] = {};
                if (!CallRet(e, L"K2_GetActorLocation", {}, p, sizeof(p)))
                    continue;
                const float d = (p[0] - here[0]) * (p[0] - here[0]) + (p[1] - here[1]) * (p[1] - here[1]) + (p[2] - here[2]) * (p[2] - here[2]);
                if (d < best)
                {
                    best = d;
                    pick = e;
                    std::memcpy(f_at, p, sizeof(f_at));
                }
            }
        }
        VanillaExportBlock::TacCmd f = {};
        f.Slot = (uint8_t)s;
        if (pick)
        {
            f.Order = VanillaExportBlock::TCO_Attack;
            std::memcpy(f.Loc, f_at, sizeof(f.Loc));
            const bool ok = TacPost(pc, f);
            if (ok && AtkTgt[s] != pick)
                AtkTgt[s] = pick; // the unit it sees, not a neighbour of it
            TacLog("attack order, slot %d - engaging other threats: the nearest it sees, %ls, %.0f m (%s)", s, ClassName(pick).c_str(),
                   std::sqrt(best) / 100.0f, ok ? "sent" : "FAILED");
            if (ok)
                return;
        }
        f.Order = VanillaExportBlock::TCO_Follow; // nothing in sight: back to you
        TacPost(pc, f);
        TacLog("attack order, slot %d - no other enemy in sight: back to you", s);
    }
    float f_at[3] = {};
    void AtkWatchTick(API::UObject *pc)
    {
        const ULONGLONG now = GetTickCount64();
        if (!pc || now - AtkWatchMs < 250)
            return;
        AtkWatchMs = now;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            API::UObject *t = AtkTgt[s];
            if (!t || !AtkSinceMs[s])
                continue;
            if (!IsLive(t) || IsDeadUnit(t))
                continue; // dead: IaTargetCheck lets go of it, the kill callout says it
            bool seen = false;
            for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers && !seen; ++k)
                if (TacUnit[k] && IsLive(TacUnit[k]) && !IsDeadUnit(TacUnit[k]))
                    if (API::UObject *ctl = ObjProp(TacUnit[k], L"Controller"))
                        seen = AiGameVisible(ctl, t) == 1;
            if (seen)
                AtkSeenMs[s] = now;
            else if (AtkSeenMs[s] && now - AtkSeenMs[s] > kAtkUnseenMs)
                AtkDrop(pc, s, 2);
            else if (!AtkSeenMs[s] && now - AtkSinceMs[s] > kAtkUnseenMs)
                AtkDrop(pc, s, 3); // never seen: the same time (user, 2026-10-11)
        }
    }
    API::UObject *CsHintComp(API::UObject *tgt)
    {
        return CsPart < 0 ? nullptr : CsPartComp(tgt, CsPart, (size_t)(GetTickCount64() / 2000));
    }
    // lancecontrol-31e: WHERE DIRECT FIRE AIMS. Called shot on: that part. Otherwise insane allies
    // (not impossibly strong): AIM SPREAD by component - each slot picks a random part, weighted
    // CT 30 / side torsos 30 / arms 20 / legs 18 / head 2, held 2 s (the torso aim gate needs a
    // steady point). Otherwise (vanilla direct fire, impossibly strong): the centre torso. Was the
    // first aim hint, which is the cockpit - every shot went at the head.
    // lancecontrol-59: the held pick is the PART and SIDE, never the hint pointer - a hint on a part that gets shot
    // off is freed by the game, and the cached pointer crashed the next K2_GetComponentLocation (2026-10-16 crash).
    int           IcSpreadPart[VanillaExportBlock::kTacMaxMembers + 1] = {};
    size_t        IcSpreadSide[VanillaExportBlock::kTacMaxMembers + 1] = {};
    API::UObject *IcSpreadTgt[VanillaExportBlock::kTacMaxMembers + 1] = {}; // compared only, never dereferenced
    ULONGLONG     IcSpreadMs[VanillaExportBlock::kTacMaxMembers + 1] = {};
    API::UObject *IcAimComp(int s, API::UObject *tgt, ULONGLONG now, const wchar_t *&how)
    {
        // lancecontrol-32: a called shot ORDER - this slot, this target only (the ini probe no longer aims)
        if (s >= 1 && s <= VanillaExportBlock::kTacMaxMembers && CsOrdPart[s] && CsOrdTgt[s])
        {
            if (!IsLive(CsOrdTgt[s]) || IsDeadUnit(CsOrdTgt[s]))
            {
                TacLog("called shot: slot %d - %ls is down, called shot over", s, CsPartName(CsOrdPart[s]));
                CsOrdPart[s] = 0;
                CsOrdTgt[s] = nullptr;
            }
            else if (CsOrdTgt[s] == tgt)
                if (API::UObject *c = CsOrderComp(tgt, CsOrdPart[s]))
                {
                    ++CsAimed;
                    ++CsFromHint;
                    how = CsPartName(CsOrdPart[s]);
                    return c;
                }
        }
        if (GsInsane && !GsExact && s >= 0 && s <= VanillaExportBlock::kTacMaxMembers)
        {
            if (IcSpreadTgt[s] == tgt && now - IcSpreadMs[s] < 2000)
                if (API::UObject *c = CsPartComp(tgt, IcSpreadPart[s], IcSpreadSide[s])) // looked up again: gone = new pick
                {
                    how = L"spread";
                    return c;
                }
            std::uniform_int_distribution<int> d100(0, 99), d2(0, 1);
            const int r = d100(IcRng);
            const int part = r < 30 ? 2 : r < 60 ? 4 : r < 80 ? 1 : r < 98 ? 0 : 3;
            const size_t side = (size_t)d2(IcRng);
            if (API::UObject *c = CsPartComp(tgt, part, side))
            {
                IcSpreadPart[s] = part;
                IcSpreadSide[s] = side;
                IcSpreadTgt[s] = tgt;
                IcSpreadMs[s] = now;
                how = L"spread";
                return c;
            }
        }
        how = L"ct";
        return CsPartComp(tgt, 2, 0);
    }
    bool CsPoint(API::UObject *tgt, float p[3], bool scatter)
    {
        if (CsPart >= 0 && CsHintPoint(tgt, p))
        {
            ++CsFromHint;
            if (scatter)
            {
                std::normal_distribution<float> n01(0.0f, 1.0f);
                for (int i = 0; i < 3; ++i)
                    p[i] += (std::max)(-80.0f, (std::min)(80.0f, n01(IcRng) * 35.0f));
            }
            return true;
        }
        ++CsFromBone;
        static const wchar_t *kLegs[] = {L"Calf_Left", L"Calf_Right", L"Thigh_Left", L"Thigh_Right"};
        static const wchar_t *kArms[] = {L"Upperarm_Left", L"Upperarm_Right"};
        static const wchar_t *kSides[] = {L"Torso_Left", L"Torso_Right"};
        API::UObject *mesh = CsMesh(tgt);
        if (!mesh || CsPart < 0)
            return false;
        const int f = CsFlip++;
        const wchar_t *bone = CsPart == 0 ? kLegs[f % 4] : CsPart == 1 ? kArms[f % 2] : CsPart == 2 ? L"Torso_Centre"
                              : CsPart == 3 ? L"Torso_Head" : kSides[f % 2];
        if (!CsJoint(mesh, bone, p))
            return false;
        if (scatter)
        {
            std::normal_distribution<float> n01(0.0f, 1.0f);
            for (int i = 0; i < 3; ++i)
                p[i] += (std::max)(-80.0f, (std::min)(80.0f, n01(IcRng) * 35.0f));
        }
        return true;
    }
    std::wstring CsChassis(API::UObject *tgt)
    {
        std::wstring c = ClassName(tgt);
        if (API::UObject *m = CsMesh(tgt))
            if (API::UObject *sk = ObjProp(m, L"SkeletalMesh"))
                if (sk->get_fname())
                    c += L" / " + sk->get_fname()->to_string();
        return c;
    }
    // VANILLA CALLED SHOTS (lancecontrol-36, user 2026-10-11). A vanilla ally the GAME fires for (insane allies off, no
    // fire settings of its own - those lancemates are fired by the plugin and aim through IcAimComp) aims at the called
    // part like this: EVERY FRAME, while the game's own combat target for it IS the called unit, each of its direct
    // weapons' AIOverrideAimTarget (the slot the game's AI aims through - vanillahud-77 aim watch) is set to that part's
    // aim hint. The game keeps its own firing, weapon choice and misses. Any other target, the order over, or the
    // plugin taking its firing: the game's own value goes back into each weapon and it is plain vanilla again.
    // Lock-on missiles and melee are left alone. The part's hint is looked up every 2 s and the weapon list every 5 s,
    // so a frame costs a few pointer reads / writes per weapon. Every 5 s the log says how often the game had put its
    // own aim back between frames ("reset") - the measure of whether this holds.
    struct CsVWpn
    {
        API::UObject *W;
        API::UObject *Old; // the game's own value, put back at the end
        bool          Ours;
        ULONGLONG     WroteMs = 0, ResetMs = 0; // lancecontrol-41 (aim-writer probe)
    };
    std::vector<CsVWpn>         CsVW[VanillaExportBlock::kTacMaxMembers + 1];
    std::vector<API::UObject *> CsVMine[VanillaExportBlock::kTacMaxMembers + 1]; // every hint we wrote (a group alternates)
    API::UObject *CsVComp[VanillaExportBlock::kTacMaxMembers + 1] = {}, *CsVFor[VanillaExportBlock::kTacMaxMembers + 1] = {};
    uint8_t       CsVPart[VanillaExportBlock::kTacMaxMembers + 1] = {};
    ULONGLONG     CsVCompMs[VanillaExportBlock::kTacMaxMembers + 1] = {}, CsVListMs[VanillaExportBlock::kTacMaxMembers + 1] = {},
              CsVLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};
    unsigned CsVFrames[VanillaExportBlock::kTacMaxMembers + 1] = {}, CsVResets[VanillaExportBlock::kTacMaxMembers + 1] = {};
    API::UObject *CsVAim[VanillaExportBlock::kTacMaxMembers + 1] = {}, *CsVAimFor[VanillaExportBlock::kTacMaxMembers + 1] = {}; // lc42
    bool          CsVAimTried[VanillaExportBlock::kTacMaxMembers + 1] = {}, CsVHintLogged[VanillaExportBlock::kTacMaxMembers + 1] = {};

    API::UObject **CsVSlot(API::UObject *w)
    {
        const int32_t off = w ? AimOverrideOff(w) : -1;
        return off < 0 ? nullptr : reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(w) + off);
    }

    void CsVRestore(int s, bool pawnAlive)
    {
        for (auto &x : CsVW[s])
        {
            if (!x.Ours)
                continue;
            x.Ours = false;
            if (!pawnAlive)
                continue;
            if (API::UObject **p = CsVSlot(x.W))
                if (std::find(CsVMine[s].begin(), CsVMine[s].end(), *p) != CsVMine[s].end())
                    *p = x.Old; // still ours: the game's own value back (a newer one of the game's stays)
        }
    }

    void CsVEnd(int s, bool pawnAlive, const char *why)
    {
        if (CsVW[s].empty() && !CsVComp[s])
            return;
        CsVRestore(s, pawnAlive);
        if (CsVFrames[s])
            TacLog("called shot (vanilla), slot %d - %s: game aim back (%u frames aimed, the game reset the aim %u times)", s, why,
                    CsVFrames[s], CsVResets[s]);
        CsVW[s].clear();
        CsVMine[s].clear();
        CsVComp[s] = CsVFor[s] = nullptr;
        CsVPart[s] = 0;
        CsVFrames[s] = CsVResets[s] = 0;
        CsVAim[s] = CsVAimFor[s] = nullptr; // lancecontrol-42: the moved AI aim point goes back via IcRestoreAims (<= 0.5 s)
        CsVAimTried[s] = CsVHintLogged[s] = false;
    }

    // AIM-WRITER PROBE (lancecontrol-41, debug setting [debug] aim_probe, off by default). What puts the game's own aim
    // back into AIOverrideAimTarget during a vanilla called shot. Two parts, both only while a vanilla called shot runs:
    //  - each reset: which weapon, what the game wrote (its class and name), how long after our write and after the
    //    previous reset on that weapon - the rhythm says whether it is per shot, per behaviour-tree tick or per target pick;
    //  - once per mission: every function with "Aim" in its name on the lancemate's mech, controller, weapons and its
    //    weapon / AI / aim / target components, marked native or blueprint; the blueprint ones are hooked (the lance
    //    probe's hook) and log their calls - only calls on our lancemates' objects, 12 per function per mission.
    //    A native writer cannot be hooked this way: then the list says so and the reset rhythm is all we get.
    bool                        AimProbeOn = false, AimProbeInvDone = false;
    int                         AimProbeLines = 0;
    std::vector<API::UObject *> AimProbeMine; // our lancemates' pawns, controllers and weapons (for the hook filter)
    static constexpr int        kAimProbeMaxLines = 80; // reset lines per mission

    bool AimProbeOurs(API::UObject *o) const
    {
        return AimProbeOn && o && std::find(AimProbeMine.begin(), AimProbeMine.end(), o) != AimProbeMine.end();
    }

    void AimProbeReset(int s, CsVWpn &x, API::UObject *gameVal, ULONGLONG now)
    {
        const ULONGLONG sinceWrite = x.WroteMs ? now - x.WroteMs : 0, sincePrev = x.ResetMs ? now - x.ResetMs : 0;
        x.ResetMs = now;
        if (AimProbeLines >= kAimProbeMaxLines)
            return;
        if (++AimProbeLines == kAimProbeMaxLines)
            LogInfo("aim probe: %d reset lines - quiet for the rest of this mission", kAimProbeMaxLines);
        std::wstring what = L"none";
        if (gameVal)
        {
            what = ClassName(gameVal);
            if (gameVal->get_fname())
                what += L" '" + gameVal->get_fname()->to_string() + L"'";
            if (API::UObject *o = gameVal->get_outer())
                what += L" of " + ClassName(o);
        }
        LogInfo("aim probe: slot %d %ls - the game wrote %ls, %llu ms after our write, %llu ms after the last reset on it",
                s, ClassName(x.W).c_str(), what.c_str(), sinceWrite, sincePrev);
    }

    void AimProbeInventory(int s, API::UObject *pawn)
    {
        if (AimProbeInvDone || !pawn)
            return;
        AimProbeInvDone = true;
        std::vector<API::UClass *> classes;
        auto add = [&](API::UObject *o) {
            if (o && o->get_class() && classes.size() < 32 &&
                std::find(classes.begin(), classes.end(), o->get_class()) == classes.end())
                classes.push_back(o->get_class());
        };
        API::UObject *ctl = ObjProp(pawn, L"Controller");
        add(pawn);
        add(ctl);
        for (auto &x : CsVW[s])
            add(x.W);
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        for (API::UObject *owner : {pawn, ctl})
        {
            std::vector<API::UObject *> comps;
            if (owner && acc && ComponentsOn(owner, acc, comps))
                for (auto *c : comps)
                {
                    const std::wstring cn = ClassName(c);
                    if (cn.find(L"Weapon") != std::wstring::npos || cn.find(L"Aim") != std::wstring::npos ||
                        cn.find(L"AI") != std::wstring::npos || cn.find(L"Target") != std::wstring::npos)
                        add(c);
                }
        }
        auto *fnClass = API::UFunction::static_class();
        std::vector<API::UStruct *> seen;
        int listed = 0, hooked = 0;
        for (auto *cls : classes)
            for (API::UStruct *st = cls; st; st = st->get_super_struct())
            {
                if (std::find(seen.begin(), seen.end(), st) != seen.end())
                    break; // this one and its supers are listed already
                seen.push_back(st);
                const std::wstring stName = st->get_fname() ? st->get_fname()->to_string() : L"?";
                for (auto *f = st->get_children(); f; f = f->get_next())
                {
                    if (f->get_class() != fnClass)
                        continue;
                    auto *fn = reinterpret_cast<API::UFunction *>(f);
                    const std::wstring name = fn->get_fname()->to_string();
                    if (name.find(L"Aim") == std::wstring::npos && name.find(L"aim") == std::wstring::npos)
                        continue;
                    const bool native = (fn->get_function_flags() & kFuncNative) != 0;
                    ++listed;
                    LogInfo("aim probe: fn %ls::%ls%s", stName.c_str(), Signature(fn).c_str(),
                            native ? " [native - cannot be hooked]" : " [blueprint - hooked]");
                    if (!native)
                    {
                        ProbeHookFn(reinterpret_cast<API::UClass *>(st), name.c_str(), L"aim probe: " + stName + L"::" + name);
                        ++hooked;
                    }
                }
            }
        LogInfo("aim probe: %zu classes, %zu structs, %d aim functions, %d blueprint ones hooked", classes.size(), seen.size(),
                listed, hooked);
    }

    void CsVanillaTick()
    {
        const ULONGLONG now = GetTickCount64();
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            if (!CsOrdPart[s] && CsVW[s].empty() && !CsVComp[s])
                continue; // nothing called, nothing to undo: the usual case, no cost
            API::UObject *pawn = TacUnit[s];
            const bool alive = pawn && IsLive(pawn) && !IsDeadUnit(pawn);
            const char *why = nullptr;
            API::UObject *tgt = nullptr;
            if (!alive)
                why = "unit gone";
            else if (GsInsane || IaSet[s])
                why = "the plugin fires for it";
            else if (!CsOrdPart[s] || !CsOrdTgt[s])
                why = "called shot over";
            else
            {
                API::UObject *ctl = ObjProp(pawn, L"Controller");
                tgt = ctl ? ObjProp(ctl, L"CombatTarget") : nullptr;
                if (tgt != CsOrdTgt[s])
                    why = "another target";
                else if (!IsLive(tgt) || IsDeadUnit(tgt))
                    why = "target down";
            }
            if (why)
            {
                CsVEnd(s, alive, why);
                continue;
            }
            // its direct weapons (per pawn, refreshed every 5 s)
            if (CsVFor[s] != pawn || now - CsVListMs[s] > 5000)
            {
                CsVRestore(s, true);
                CsVW[s].clear();
                CsVFor[s] = pawn;
                CsVListMs[s] = now;
                std::vector<API::UObject *> comps;
                if (acc && ComponentsOn(pawn, acc, comps))
                    for (auto *w : comps)
                    {
                        const wchar_t *how = nullptr;
                        if (AiIsWeaponComp(w) && !IsMeleeWeapon(w) && CsVSlot(w) && IcLockWeapon(w, how) != 1)
                            CsVW[s].push_back({w, nullptr, false});
                    }
                if (AimProbeOn)
                {
                    if (AimProbeMine.size() > 256)
                        AimProbeMine.clear();
                    AimProbeMine.push_back(pawn);
                    if (API::UObject *c = ObjProp(pawn, L"Controller"))
                        AimProbeMine.push_back(c);
                    for (auto &x : CsVW[s])
                        AimProbeMine.push_back(x.W);
                    AimProbeInventory(s, pawn);
                }
            }
            // the part's aim hint (every 2 s - a group such as "legs" alternates between its hints)
            // lancecontrol-42: looked up every 2 s even when there is no hint (it was every frame), logged once
            if (CsVPart[s] != CsOrdPart[s] || now - CsVCompMs[s] > 2000 ||
                (CsVComp[s] && !CsHintStill(tgt, CsVComp[s]))) // lancecontrol-59: the part shot off = its hint freed
            {
                CsVComp[s] = CsOrderComp(tgt, CsOrdPart[s]);
                CsVPart[s] = CsOrdPart[s];
                CsVCompMs[s] = now;
                if (CsVComp[s] && std::find(CsVMine[s].begin(), CsVMine[s].end(), CsVComp[s]) == CsVMine[s].end())
                    CsVMine[s].push_back(CsVComp[s]);
                if (!CsVFrames[s] && !CsVHintLogged[s])
                {
                    CsVHintLogged[s] = true;
                    TacLog("called shot (vanilla), slot %d - %ls on %ls: %zu direct weapon(s) aimed by the frame%s", s,
                            CsPartName(CsOrdPart[s]), CsChassis(tgt).c_str(), CsVW[s].size(), CsVComp[s] ? "" : " - NO aim hint for that part");
                }
            }
            if (!CsVComp[s])
                continue;
            // lancecontrol-42: THE TARGET'S OWN AI AIM POINT ON THE PART. The aim-writer probe (Test 7) showed the game
            // writing the target's MWAIAimLocationComponent into each weapon around every shot (and clearing it ~0.2 s
            // later) - native code, not hookable. So that component is moved onto the called part's aim hint while the
            // called shot runs (as insane allies' lead does - IcPlaceAimOn, remembered once, put back by IcRestoreAims
            // within 0.5 s of the end): the game's own write then aims at the part too. Every AI shooter of that unit -
            // in practice our lance - aims there meanwhile; two slots calling different parts on one unit: the last wins.
            if (CsVAimFor[s] != tgt || now - CsVCompMs[s] < 1) // lancecontrol-59: looked up again with the hint
            {
                CsVAimFor[s] = tgt;
                CsVAim[s] = AiAimCompOf(tgt);
                if (!CsVAimTried[s])
                {
                    CsVAimTried[s] = true;
                    TacLog("called shot (vanilla), slot %d - %ls: %s", s, CsChassis(tgt).c_str(),
                            CsVAim[s] ? "its AI aim point follows the called part" : "no AI aim point on that unit - per-frame aim only");
                }
            }
            if (CsVAim[s])
            {
                float hp[3] = {};
                if (CallRet(CsVComp[s], L"K2_GetComponentLocation", {}, hp, sizeof(hp)))
                    IcPlaceAimOn(CsVAim[s], tgt, hp, now + 500);
            }
            ++CsVFrames[s];
            for (auto &x : CsVW[s])
            {
                API::UObject **p = CsVSlot(x.W);
                if (!p || *p == CsVComp[s])
                    continue;
                const bool mine = std::find(CsVMine[s].begin(), CsVMine[s].end(), *p) != CsVMine[s].end();
                if (!mine)
                {
                    if (x.Ours)
                    {
                        ++CsVResets[s]; // the game wrote its own aim since the last frame
                        if (AimProbeOn)
                            AimProbeReset(s, x, *p, now);
                    }
                    x.Old = *p;
                }
                *p = CsVComp[s];
                x.Ours = true;
                x.WroteMs = now;
            }
            if (now - CsVLogMs[s] >= 5000)
            {
                CsVLogMs[s] = now;
                TacLog("called shot (vanilla), slot %d - %ls on %ls: %u frames, the game reset the aim %u times", s,
                        CsPartName(CsOrdPart[s]), CsChassis(tgt).c_str(), CsVFrames[s], CsVResets[s]);
            }
        }
    }

    void CsTick()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - CsTickMs < 250)
            return;
        CsTickMs = now;
        API::UObject *tgt = AiPlayerLock();
        if (tgt && (!IsLive(tgt) || IsDeadUnit(tgt)))
            tgt = nullptr;
        API::UObject *mesh = tgt ? CsMesh(tgt) : nullptr;
        // 1) the joint check, once per chassis class
        if (mesh && tgt != CsLast && CsReady())
        {
            // lancecontrol-31c: once per CHASSIS - the skeletal mesh asset (all mechs are DerivedMech_C)
            std::wstring cls = ClassName(tgt);
            if (API::UObject *sk = ObjProp(mesh, L"SkeletalMesh"))
                if (sk->get_fname())
                    cls += L" / " + sk->get_fname()->to_string();
            if (CsSeen.insert(cls).second)
            {
                static const wchar_t *kJ[] = {L"Pelvis", L"Thigh_Left", L"Knee_Left", L"Calf_Left", L"Foot_Left", L"Thigh_Right",
                                              L"Knee_Right", L"Calf_Right", L"Foot_Right", L"Torso_Centre", L"Torso_Head",
                                              L"Torso_Left", L"Torso_Right", L"Upperarm_Left", L"Upperarm_Right"};
                float a[3] = {};
                CallRet(tgt, L"K2_GetActorLocation", {}, a, sizeof(a));
                std::wstring line;
                int ok = 0;
                for (auto *j : kJ)
                {
                    float q[3];
                    wchar_t b[96];
                    if (CsJoint(mesh, j, q))
                    {
                        ++ok;
                        _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls(%.0f,%.0f,%.0f)", j, q[0] - a[0], q[1] - a[1], q[2] - a[2]);
                    }
                    else
                        _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls(-)", j);
                    line += b;
                }
                LogInfo("called shot: %ls - %d of 15 joints resolve, offsets from the actor in cm:%ls", cls.c_str(), ok, line.c_str());
                // lancecontrol-31b: the game's own aim hints - name, attach socket, offset, and their own properties
                std::vector<API::UObject *> hints;
                CsHints(tgt, hints);
                LogInfo("called shot: %ls - %zu MWAimHintComponents", cls.c_str(), hints.size());
                for (size_t i = 0; i < hints.size() && i < 16; ++i)
                {
                    API::UObject *h = hints[i];
                    float q[3] = {};
                    CallRet(h, L"K2_GetComponentLocation", {}, q, sizeof(q));
                    auto *sn = h->get_property_data<API::FName>(L"AttachSocketName");
                    LogInfo("called shot:   hint %zu '%ls' socket '%ls' at (%.0f,%.0f,%.0f) from the actor -> part guess %d", i,
                            h->get_fname() ? h->get_fname()->to_string().c_str() : L"?", sn ? sn->to_string().c_str() : L"?",
                            q[0] - a[0], q[1] - a[1], q[2] - a[2], CsPartOfKey(CsHintKey(h)));
                    if (i == 0 && h->get_class())
                    {
                        ProbeLines = 0; // its own (MWAimHintComponent) members, once
                        for (auto *f = h->get_class()->get_child_properties(); f; f = f->get_next())
                        {
                            auto *pr = reinterpret_cast<API::FProperty *>(f);
                            ProbeDump(L"hint0." + f->get_fname()->to_string(), pr, reinterpret_cast<uint8_t *>(h) + pr->get_offset(), 1);
                        }
                    }
                }
            }
        }
        CsLast = tgt;
        if (CsPart < 0)
            return;
        // 2) vanilla allies: the game's aim component of your target onto the part
        float cp[3];
        if (!GsInsane && tgt && AiAimCompOf(tgt) && CsPoint(tgt, cp, false))
        {
            IcPlaceAim(tgt, cp, now + 600);
            ++CsAimed;
        }
        if (!GsInsane)
            IcRestoreAims(now);
        // 3) the target's damage by part, every 5 s
        if (tgt && now - CsCondMs >= 5000)
        {
            CsCondMs = now;
            LsCondition c;
            LsAmmoList am;
            int uav = -1;
            if (LsReadCondition(tgt, c, am, uav) && c.Have)
            {
                static const wchar_t *kP[] = {L"head", L"CT", L"LT", L"RT", L"LA", L"RA", L"LL", L"RL"};
                std::wstring line;
                for (int k = 0; k < kLsParts; ++k)
                {
                    wchar_t b[48];
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls %.0f/%.0f", kP[k], c.Armour[k] * 100.0f, c.Structure[k] * 100.0f);
                    line += b;
                }
                LogInfo("called shot: %ls on %ls, %d aimed volleys/placements so far (%d on aim hints, %d on bones) - armour/structure %%:%ls",
                        CsPart == 0 ? L"legs" : CsPart == 1 ? L"arms" : CsPart == 2 ? L"ct" : CsPart == 3 ? L"head" : L"sides",
                        CsChassis(tgt).c_str(), CsAimed, CsFromHint, CsFromBone, line.c_str());
            }
        }
    }

    // The target's AI aim component on `p` until `back` (remembered once, put back by IcRestoreAims).
    API::UObject *IcPlaceAim(API::UObject *tgt, const float *p, ULONGLONG back)
    {
        return IcPlaceAimOn(AiAimCompOf(tgt), tgt, p, back);
    }

    // lancecontrol-42: the same with the component already found (vanilla called shots keep it per target).
    API::UObject *IcPlaceAimOn(API::UObject *comp, API::UObject *tgt, const float *p, ULONGLONG back)
    {
        if (!comp)
            return nullptr;
        bool known = false;
        for (auto &e : IcMoved)
            if (e.Comp == comp)
            {
                known = true;
                e.RestoreMs = (std::max)(e.RestoreMs, back);
            }
        if (!known)
        {
            IcMovedAim e{comp, tgt, {0, 0, 0}, back};
            auto *rp = FindProp(comp->get_class(), L"RelativeLocation");
            if (!rp || PropType(rp) != L"StructProperty")
                return nullptr;
            std::memcpy(e.Rel, reinterpret_cast<uint8_t *>(comp) + rp->get_offset(), sizeof(e.Rel));
            IcMoved.push_back(e);
        }
        uint8_t tele = 1;
        float q[3] = {p[0], p[1], p[2]};
        if (!CallRet(comp, L"K2_SetWorldLocation", {{L"NewLocation", q, sizeof(q)}, {L"bTeleport", &tele, 1}}, nullptr, 0))
            return nullptr;
        return comp;
    }

    // Put moved aim points back once nobody of ours is using them.
    void IcRestoreAims(ULONGLONG now, bool all = false)
    {
        for (size_t i = 0; i < IcMoved.size();)
        {
            auto &e = IcMoved[i];
            if (!all && now < e.RestoreMs)
            {
                ++i;
                continue;
            }
            if (IsLive(e.Owner) && !IsDeadUnit(e.Owner))
            {
                uint8_t tele = 1;
                CallRet(e.Comp, L"K2_SetRelativeLocation", {{L"NewLocation", e.Rel, sizeof(e.Rel)}, {L"bTeleport", &tele, 1}}, nullptr, 0);
            }
            IcMoved.erase(IcMoved.begin() + (ptrdiff_t)i);
        }
    }

    // MELEE (lancecontrol-83, user 2026-10-20): THE SWING ONLY. The game's fire check says a punch is ready whenever
    // the mech is not mid-sequence ("CannotMeleeAttack" only while its other weapons fire - range never enters it), so
    // the plugin judges the reach itself and uses the check only for busy / cooldown. A swing needs:
    //   - in reach: from this mech's centre to the NEAREST POINT of the target's collision box, within the melee
    //     weapon's own rated range (StatsData.Ranges.MaxRange: fists 17.5 m, claws 18.75, swords / axes 20, lance 22,
    //     katana 22.5; 17.5 m if unreadable) - an Atlas's centre sits metres inside its hull;
    //   - in front: the target within kIcMeleeConeDeg of the torso's aim (horizontal);
    //   - the game's fire check Success (not cooling down / not mid-sequence), no friendly in the line;
    //   - the arm on the target's side first (both near dead ahead: they take turns); one swing per kIcMeleeGapMs.
    // No fire-settings toggle (user): hold fire / return fire still apply (IcDirectFire returns before this). No rush
    // in: only what is already in reach. Heat ignored (melee makes none). Every swing logged with the numbers; a target
    // within 30 m that is not swung at logs why every 2 s.
    static constexpr float kIcMeleeConeDeg = 30.0f;
    static constexpr float kIcMeleeDefaultReachCm = 1750.0f;
    static constexpr ULONGLONG kIcMeleeGapMs = 500;
    ULONGLONG IcMeleeMs[VanillaExportBlock::kTacMaxMembers + 1] = {};
    ULONGLONG IcMeleeLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};
    int       IcMeleeAlt[VanillaExportBlock::kTacMaxMembers + 1] = {};
    int       IcMeleeSwings[VanillaExportBlock::kTacMaxMembers + 1] = {};

    void IcMeleeFire(int s, API::UObject *pawn, API::UObject *tgt, const float *here, ULONGLONG now)
    {
        if (IcMelee[s].empty() || !tgt || now - IcMeleeMs[s] < kIcMeleeGapMs)
            return;
        float o[3] = {}, ext[3] = {};
        if (!IcBounds(tgt, o, ext))
            return;
        float d2 = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            const float c = (std::max)(o[i] - ext[i], (std::min)(here[i], o[i] + ext[i]));
            d2 += (here[i] - c) * (here[i] - c);
        }
        const float edge = std::sqrt(d2);
        if (edge > 3000.0f)
            return; // nothing near: no work, no log
        const float cdx = o[0] - here[0], cdy = o[1] - here[1], cdz = o[2] - here[2];
        const float centre = std::sqrt(cdx * cdx + cdy * cdy + cdz * cdz);
        float ad[3] = {};
        if (!CallRet(pawn, L"GetAimDirection", {}, ad, sizeof(ad)))
            return;
        const float al = std::sqrt(ad[0] * ad[0] + ad[1] * ad[1]), tl = std::sqrt(cdx * cdx + cdy * cdy);
        float yaw = 0.0f; // signed: + = the target is to the right (UE: X forward, Y right)
        if (al > 0.0f && tl > 0.0f)
        {
            float c = (ad[0] * cdx + ad[1] * cdy) / (al * tl);
            c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
            yaw = std::acos(c) * 57.2958f * ((ad[0] * cdy - ad[1] * cdx) >= 0.0f ? 1.0f : -1.0f);
        }
        const bool front = std::fabs(yaw) <= kIcMeleeConeDeg;
        // Arm order: the target's side first; near dead ahead (under 10 deg) the arms take turns.
        bool wantRight = yaw > 0.0f;
        if (std::fabs(yaw) < 10.0f)
            wantRight = (IcMeleeAlt[s] & 1) != 0;
        std::vector<API::UObject *> order;
        for (int pass = 0; pass < 2; ++pass)
            for (API::UObject *w : IcMelee[s])
            {
                if (!w || !OnMech(pawn, w))
                    continue;
                std::wstring n = ClassName(w);
                for (auto &ch : n)
                    ch = (wchar_t)towlower(ch);
                const bool isRight = n.find(L"right") != std::wstring::npos;
                const bool isLeft = n.find(L"left") != std::wstring::npos;
                const bool pref = (wantRight && isRight) || (!wantRight && isLeft) || (!isRight && !isLeft);
                if ((pass == 0) == pref)
                    order.push_back(w);
            }
        const char *why = nullptr;
        float reachLog = -1.0f;
        for (API::UObject *w : order)
        {
            float reach = IcWeaponRangeCm(w);
            if (!(reach > 0.0f && reach < 10000.0f))
                reach = kIcMeleeDefaultReachCm;
            reachLog = (std::max)(reachLog, reach);
            if (edge > reach)
            {
                why = why ? why : "out of reach";
                continue;
            }
            if (!front)
            {
                why = "not in front of the torso";
                continue;
            }
            bool busy = false;
            for (auto &d : IcDirFiring[s])
                busy |= d.W == w;
            if (busy)
            {
                why = "still swinging";
                continue;
            }
            std::vector<uint8_t> f(AiFrBuf.size(), 0);
            for (int32_t po : AiFrObjParams)
                std::memcpy(f.data() + po, &tgt, sizeof(tgt));
            IaCanFire(w, f.data());
            if (f[AiFrRet] != 0)
            {
                why = "the game's check: busy / cooling down";
                continue;
            }
            if (API::UObject *fr = FriendInLine(s, pawn, here, o))
            {
                (void)fr;
                why = "a friendly in the way";
                break;
            }
            const int32_t off = AimOverrideOff(w);
            API::UObject *old = nullptr;
            if (off >= 0)
            {
                auto **slot = reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(w) + off);
                old = *slot;
                if (API::UObject *ap = AimPointOf(tgt))
                    *slot = ap;
            }
            if (!IcCallNoArgs(w, L"K2_StartFire"))
            {
                if (off >= 0)
                    *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(w) + off) = old;
                why = "K2_StartFire failed";
                continue;
            }
            IcDirFiring[s].push_back({w, old, now + 250, false, tgt}); // one press: let go in 0.25 s (IcDirStop)
            IcMeleeMs[s] = now;
            ++IcMeleeAlt[s];
            if (++IcMeleeSwings[s] <= 60)
                TacLog("melee, slot %d - SWING %ls at %ls: centre %.1f m, edge %.1f m, reach %.1f m, %+.0f deg off the torso", s,
                        ClassName(w).c_str(), ClassName(tgt).c_str(), centre * 0.01f, edge * 0.01f, reach * 0.01f, yaw);
            return;
        }
        if (why && now - IcMeleeLogMs[s] > 2000)
        {
            IcMeleeLogMs[s] = now;
            TacLog("melee, slot %d - no swing at %ls (%s): centre %.1f m, edge %.1f m, reach %.1f m, %+.0f deg off the torso", s,
                    ClassName(tgt).c_str(), why, centre * 0.01f, edge * 0.01f, reachLog * 0.01f, yaw);
        }
    }

    void IcDirectFire(int s, API::UObject *pawn, API::UObject *ctl, API::UObject *tgt, const float *here, ULONGLONG now)
    {
        IcDirStop(s, false, now);
        if (!IcMaskSet[s] || !tgt || IaDead(tgt) || !AiFrOk || !AiFrFn)
            return;
        if (IcHoldNow(s) || !FsTargetOk(s, tgt)) // hold fire / return fire: not this one
            return;
        if (!IcAimSigLogged)
        {
            IcAimSigLogged = true;
            API::UFunction *b = FindFn(pawn, L"GetAimDirection");
            TacLog("tacops: aim test - %ls; AIOverrideAimTarget %ls", b ? Signature(b).c_str() : L"GetAimDirection NOT FOUND",
                    (!IcWpns[s].empty() && OnMech(pawn, IcWpns[s][0].W) && AimOverrideOff(IcWpns[s][0].W) >= 0) ? L"found" : L"NOT FOUND");
        }
        const bool vis = AiGameVisible(ctl, tgt) == 1;
        if (!vis)
            return;
        IcMeleeFire(s, pawn, tgt, here, now); // lancecontrol-83: the swing, in reach only (no fire-settings toggle)
        API::UObject *aim = AimPointOf(tgt);
        const wchar_t *aimHow = L"first hint";
        if (API::UObject *ch = IcAimComp(s, tgt, now, aimHow)) // lancecontrol-31e: called shot / spread / CT
            aim = ch;
        if (!aim)
            return;
        float ap[3] = {};
        CallRet(aim, L"K2_GetComponentLocation", {}, ap, sizeof(ap));
        const float dx = ap[0] - here[0], dy = ap[1] - here[1], dz = ap[2] - here[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        // Aim error: the mech's aim direction against the direction to the aim point.
        float err = -1.0f;
        float ad[3] = {};
        if (dist > 1.0f && CallRet(pawn, L"GetAimDirection", {}, ad, sizeof(ad)))
        {
            const float al = std::sqrt(ad[0] * ad[0] + ad[1] * ad[1] + ad[2] * ad[2]);
            if (al > 0.0f)
            {
                float c = (ad[0] * dx + ad[1] * dy + ad[2] * dz) / (al * dist);
                c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
                err = std::acos(c) * 57.2958f;
            }
        }
        // lancecontrol-81 (user, 2026-10-20): a TAG keeps PAINTING - it is not held by the heat ceiling (no heat), the
        // friendly-in-line check (no damage) or the 3 deg aim gate (it allows 10 deg); only by sight, its own fire check
        // (range ...), a new target, hold fire, or its category being off. Every other weapon: the gates as before.
        const float heat = IcHeatFrac(pawn);
        const bool heatOk = heat <= FsHeatLimit(s);
        // Aim gate: the torso must be on the target (vanillahud-77: shots started 10-41 deg off
        // right after a spot or a target change, before the torso had turned).
        const bool aimOk = !(err < 0.0f || err > kIcAimGateDeg);
        const bool tagAimOk = !(err < 0.0f || err > kIcTagAimGateDeg);
        if (!aimOk && heatOk && now - IcAimLogMs[s] > 2000)
        {
            IcAimLogMs[s] = now;
            TacLog("tacops: direct fire, slot %d - sees %ls, %.0f m, aim error %.1f deg - waiting for the torso%s", s,
                    ClassName(tgt).c_str(), dist * 0.01f, err, tagAimOk ? " (a TAG may paint)" : "");
        }
        // Friendly fire: no direct shot while a friendly (the player or a lancemate) is near the
        // line from this mech to the aim point.
        bool ffBlock = false;
        if (heatOk && aimOk)
            if (API::UObject *fr = FriendInLine(s, pawn, here, ap))
            {
                ffBlock = true;
                if (now - IcFfLogMs[s] > 2000)
                {
                    IcFfLogMs[s] = now;
                    LsEmit(LSE_FriendlyInLine, s, fr, 0.0f);
                    TacLog("tacops: direct fire, slot %d - HELD: friendly %ls in the line of fire to %ls", s, ClassName(fr).c_str(),
                            ClassName(tgt).c_str());
                }
            }
        const bool othersOk = heatOk && aimOk && !ffBlock;
        if (!othersOk && !tagAimOk)
            return;
        std::wstring started;
        // HEAT BUDGET (vanillahud-92): weapons start in order of damage per heat (from their data),
        // each adding its heat to the projection - no start that would pass the ceiling. A weapon of
        // unknown heat only needs the current heat under the ceiling, as before.
        float hcur = 0.0f, hmax = 0.0f;
        const bool hAbs = IcHeatAbs(pawn, hcur, hmax);
        if (IcHeatProjSet[s] && now - IcHeatProjMs[s] < 150)
            hcur = IcHeatProj[s]; // what the missiles started this tick already committed
        IcHeatProjSet[s] = false;
        const float hlim = FsHeatLimit(s);
        std::vector<std::pair<float, size_t>> order;
        for (size_t i = 0; i < IcWpns[s].size(); ++i)
        {
            if (!OnMech(pawn, IcWpns[s][i].W))
                continue; // shot off: never touched
            const WStat ws = WeaponStat(IcWpns[s][i].W);
            const float eff = (ws.Heat > 0.0f && ws.Dmg > 0.0f) ? ws.Dmg / ws.Heat : (ws.Heat > 0.0f ? 0.0f : -1.0f);
            order.push_back({eff, i});
        }
        std::stable_sort(order.begin(), order.end(), [](auto &a, auto &b) { return a.first > b.first; });
        for (auto &oi : order)
        {
            auto &x = IcWpns[s][oi.second];
            if (x.Indirect != 0)
                continue;
            if (!(FsMask[s] & x.Cat))
                continue; // not a category it may fire
            const bool isTag = IsTagWeapon(x.W); // lancecontrol-81: TAG paints past the other gates (see above)
            if (isTag ? !tagAimOk : !othersOk)
                continue;
            IcDirShot *busy = nullptr;
            for (auto &d : IcDirFiring[s])
                if (d.W == x.W)
                    busy = &d;
            if (busy)
            {
                // A continuous weapon keeps firing while the target stays fireable (this point is
                // only reached when it is: seen, torso on it, no friendly in the line, heat ok) - the
                // same target, and its own fire check still not ruling it out (vanillahud-92).
                if (busy->Continuous && busy->Tgt == tgt && OnMech(pawn, x.W))
                {
                    std::vector<uint8_t> f(AiFrBuf.size(), 0);
                    for (int32_t o : AiFrObjParams)
                        std::memcpy(f.data() + o, &tgt, sizeof(tgt));
                    IaCanFire(x.W, f.data()); // lancecontrol-50: timed
                    const unsigned v = f[AiFrRet];
                    if (v == 0 || v == 2 || v == 3)
                        busy->StopMs = now + 350;
                }
                else if (busy->Continuous && busy->Tgt != tgt)
                    busy->StopMs = now; // another target now: let go on the next stop pass
                continue;
            }
            if (!OnMech(pawn, x.W))
                continue;
            const int32_t off = AimOverrideOff(x.W);
            if (off < 0)
                continue;
            std::vector<uint8_t> f(AiFrBuf.size(), 0);
            for (int32_t o : AiFrObjParams)
                std::memcpy(f.data() + o, &tgt, sizeof(tgt));
            IaCanFire(x.W, f.data()); // lancecontrol-50: timed
            if (f[AiFrRet] == 5)
            {
                IcLearnLock(s, x, 5); // a lock-on weapon after all: IcOwnFire's from now on
                continue;
            }
            if (f[AiFrRet] != 0) // not Success: cooldown, range, ammo ...
                continue;
            const WStat ws = WeaponStat(x.W);
            if (ws.Kind == WK_AMS)
                continue; // the game's automatic defence
            if (!isTag && hAbs && hmax > 0.0f && ws.Heat > 0.0f && hcur + ws.Heat > hlim * hmax)
                continue; // would pass the heat ceiling
            const bool cont = ws.Kind == WK_Continuous;
            const bool proj = ws.Kind == WK_Projectile;
            const bool beam = ws.Kind == WK_Beam;
            ULONGLONG hold = 1000; // nothing known: as before
            if (cont)
                hold = 350;
            else if (beam)
                hold = (ULONGLONG)(std::max)(0.0f, (std::min)(ws.Dur * 1000.0f + 100.0f, 3000.0f));
            else if (proj)
                hold = (ULONGLONG)(std::max)(0.0f, (std::min)((ws.Times > 1 ? (ws.Times - 1) * ws.Delay * 1000.0f : 0.0f) + 200.0f, 3000.0f));
            float leadM = 0.0f;
            API::UObject *use = aim;
            // Lead only when the aim point is not the game's own aim component itself (vanillahud-92:
            // that is the one moved - leading from it would stack the lead shot on shot).
            float dropM = 0.0f;
            if (proj) // lancecontrol-52: lead + drop on our own aim mark (the old lead moved an AI aim component most mechs lack)
                if (API::UObject *la = IcAimMark(pawn, x.W, tgt, ap, ws, now, hold, leadM, dropM))
                    use = la;
            auto **slot = reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(x.W) + off);
            API::UObject *old = *slot;
            *slot = use;
            ++IpFireN; // lancecontrol-48
            if (!IcCallNoArgs(x.W, L"K2_StartFire"))
            {
                *slot = old;
                continue;
            }
            IcDirFiring[s].push_back({x.W, nullptr, now + hold, cont, tgt});
            if (ws.Heat > 0.0f)
                hcur += ws.Heat;
            x.OwnMs = now;
            ++IcDirFires[s];
            wchar_t b[160];
            if (proj)
                _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls[lead %.1f m, drop %.1f m]", x.Name.c_str(), leadM, dropM);
            else
                _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls[%ls %.1f s]", x.Name.c_str(), cont ? L"held" : (beam ? L"beam" : L"press"),
                             hold / 1000.0f);
            started += b;
        }
        if (!started.empty() && IcDirFires[s] <= 60)
            TacLog("tacops: direct fire, slot %d - STARTED at %ls (aim %ls, %ls), %.0f m, aim error %.1f deg, heat %.0f %%:%ls", s,
                    ClassName(tgt).c_str(), CsHintKey(aim).c_str(), aimHow,
                    dist * 0.01f, err, heat * 100.0f, started.c_str());
        else if (now - IcAimLogMs[s] > 2000)
        {
            IcAimLogMs[s] = now;
            TacLog("tacops: direct fire, slot %d - sees %ls, %.0f m, aim error %.1f deg, nothing ready", s, ClassName(tgt).c_str(),
                    dist * 0.01f, err);
        }
    }

    // FRIENDLY FIRE. A friendly - the player, or another lancemate - whose origin is within
    // kFfRadiusCm of the segment from the shooter to the aim point, and between them (not behind the
    // shooter, not beyond the target), blocks the shot. Mechs are roughly 10 m wide at most; the
    // game's own AI pads friendly capsules x2 (MW.AI.FriendlyCapsuleMultiplier), hence 12 m.
    // Plain geometry on actor origins - no trace - so a friend just above or below the line counts too.
    static constexpr float kFfRadiusCm = 1200.0f;

    ULONGLONG IcFfLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    // Every live pawn in the level (the game's own actor list), refreshed at most every 250 ms: the
    // only safe way to tell whether a pointer we kept is still a unit.
    std::vector<API::UObject *> LivePawns;

    ULONGLONG LivePawnsMs = 0;

    const std::vector<API::UObject *> &Live()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - LivePawnsMs < 250)
        {
            // lancecontrol-65 (user, 2026-10-17): a pawn freed since the list was taken (up to 250 ms ago) is dropped here,
            // so no caller reads through it (FriendInLinePass read its class for the team offset - the lc62 crash's twin)
            LivePawns.erase(std::remove_if(LivePawns.begin(), LivePawns.end(), [this](API::UObject *k) { return !ObjStillThere(k); }),
                            LivePawns.end());
            return LivePawns;
        }
        LivePawnsMs = now;
        LivePawns.clear();
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        if (API::UObject *pc = API::get()->get_player_controller(0))
            if (PawnClass)
                ActorsOf(pc, PawnClass, LivePawns);
        return LivePawns;
    }

    bool IsLive(API::UObject *a)
    {
        const auto &l = Live();
        return a && std::find(l.begin(), l.end(), a) != l.end();
    }

    // vanillahud-87: every live unit on this lancemate's team (the player, the lance, and allied
    // units a mission brings - convoys, allied mechs, turrets), not only the lance.
    API::UObject *FriendInLine(int s, API::UObject *pawn, const float *from, const float *to) // lancecontrol-48: timed
    {
        if (!IpOn)
            return IaInPass ? FriendInLinePass(s, pawn, from, to) : FriendInLineRaw(s, pawn, from, to);
        const double t0 = IpNow();
        API::UObject *r = IaInPass ? FriendInLinePass(s, pawn, from, to) : FriendInLineRaw(s, pawn, from, to);
        IpFilMs += IpNow() - t0;
        ++IpFilN;
        return r;
    }

    // FRIENDLY LIST PER PASS (lancecontrol-49, user 2026-10-13 stage 2 after the lc48 profile: ~1 ms per friendly-in-line
    // check, 1-2 per pass). FriendInLineRaw walked every pawn (team looked up by name), then asked the game whether each
    // friend is dead and where it is - again for every check. Now, inside an IaTick pass, the friends of a team are
    // collected once (team by cached offset, dead by LsDead, position by LsPos - the lc46 reads, checked against the
    // game's) and every check in that pass uses the list. Same geometry, same 12 m radius, same "player always counts".
    // Check: every 32nd check runs the old one too; a different answer is logged ("iatick friendly check").
    struct IaFriend
    {
        API::UObject *A;
        float         P[3];
    };
    std::vector<IaFriend> IaFr;
    uint8_t               IaFrTeam = 0xFE;
    bool                  IaInPass = false, IaFrBuilt = false;
    unsigned              IaFrChecks = 0;
    int                   IaFrLogs = 0;

    API::UObject *FriendInLinePass(int s, API::UObject *pawn, const float *from, const float *to)
    {
        const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
        const float len2 = dx * dx + dy * dy + dz * dz;
        if (len2 < 1.0f)
            return nullptr;
        const int32_t to_ = TeamOff(pawn);
        const uint8_t ours = to_ < 0 ? (uint8_t)0xFF : *(reinterpret_cast<uint8_t *>(pawn) + to_);
        if (!IaFrBuilt || IaFrTeam != ours)
        {
            IaFrBuilt = true;
            IaFrTeam = ours;
            IaFr.clear();
            std::vector<API::UObject *> friends;
            if (API::UObject *me = API::get()->get_local_pawn(0))
                friends.push_back(me);
            if (ours != 0xFF)
                for (auto *k : Live())
                {
                    if (!k || std::find(friends.begin(), friends.end(), k) != friends.end())
                        continue;
                    const int32_t o = TeamOff(k);
                    if (o >= 0 && *(reinterpret_cast<uint8_t *>(k) + o) == ours)
                        friends.push_back(k);
                }
            for (auto *f : friends)
            {
                if (LsDead(f))
                    continue;
                IaFriend e{f, {}};
                if (LsPos(f, e.P))
                    IaFr.push_back(e);
            }
        }
        API::UObject *hit = nullptr;
        for (auto &e : IaFr)
        {
            if (e.A == pawn) // the shooter itself (the old list skipped it while walking the pawns)
                continue;
            const float px = e.P[0] - from[0], py = e.P[1] - from[1], pz = e.P[2] - from[2];
            const float t = (px * dx + py * dy + pz * dz) / len2;
            if (t <= 0.0f || t >= 1.0f)
                continue;
            const float cx = px - t * dx, cy = py - t * dy, cz = pz - t * dz;
            if (cx * cx + cy * cy + cz * cz < kFfRadiusCm * kFfRadiusCm)
            {
                hit = e.A;
                break;
            }
        }
        if ((++IaFrChecks & 31) == 0)
        {
            API::UObject *old = FriendInLineRaw(s, pawn, from, to);
            if ((old != nullptr) != (hit != nullptr) && IaFrLogs < 20)
            {
                ++IaFrLogs;
                LogInfo("iatick friendly check: slot %d - per-pass list says %ls, the old check says %ls", s,
                        hit ? ClassName(hit).c_str() : L"clear", old ? ClassName(old).c_str() : L"clear");
            }
        }
        return hit;
    }
    API::UObject *FriendInLineRaw(int s, API::UObject *pawn, const float *from, const float *to)
    {
        (void)s;
        const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
        const float len2 = dx * dx + dy * dy + dz * dz;
        if (len2 < 1.0f)
            return nullptr;
        const uint8_t ours = TeamOf(pawn);
        std::vector<API::UObject *> friends;
        if (API::UObject *me = API::get()->get_local_pawn(0))
            friends.push_back(me);
        if (ours != 0xFF)
            for (auto *k : Live())
                if (k != pawn && TeamOf(k) == ours && std::find(friends.begin(), friends.end(), k) == friends.end())
                    friends.push_back(k);
        for (auto *f : friends)
        {
            if (IsDeadUnit(f))
                continue;
            float p[3] = {};
            if (!CallRet(f, L"K2_GetActorLocation", {}, p, sizeof(p)))
                continue;
            const float px = p[0] - from[0], py = p[1] - from[1], pz = p[2] - from[2];
            const float t = (px * dx + py * dy + pz * dz) / len2;
            if (t <= 0.0f || t >= 1.0f)
                continue;
            const float cx = px - t * dx, cy = py - t * dy, cz = pz - t * dz;
            if (cx * cx + cy * cy + cz * cz < kFfRadiusCm * kFfRadiusCm)
                return f;
        }
        return nullptr;
    }

    // INSANE ALLIES (vanillahud-79). On while GlobalSettings.ini [lance] insane_allies = 1 (GsTick,
    // lancecontrol-19; it was HUDDisplay's time-stamped OptFlags bit before). While set, every lancemate
    // that is not under "hold position (indirect fire only)" (that order keeps its own rules) has
    // the game's firing switched off, and every 250 ms the plugin fires, at the lancemate's own
    // combat target (the game still picks it):
    //   - lock-on weapons: locked, within 900 m, heat under 85 % (IcOwnFire);
    //   - direct weapons: the game says it sees the target, the torso is within 3 deg of it, no
    //     friendly in the line, heat under 85 % (IcDirectFire).
    // Every weapon whose own fire check says Success fires - not one group at a time.
    // Cleared: the game's firing comes back on.
    bool IaSet[VanillaExportBlock::kTacMaxMembers + 1] = {};

    // ---- FIRE SETTINGS (vanillahud-91, TCO_FireSettings). Per lance slot - kept across missions,
    // standing orders for that slot:
    //   discipline: 0 weapons free, 1 return fire (only enemies engaging the lance or you: their
    //     combat target is one of us, or a round / volley of theirs went at one of us in the last
    //     10 s), 2 hold fire (the plugin fires nothing; the game's firing stays off);
    //   category mask (kFc*): the weapon categories it may fire;
    //   heat: 0 normal 85 %, 1 cool 60 %, 2 hot 95 %, 3 alpha (no limit).
    // Anything but all-defaults makes the plugin do that lancemate's firing (IaTick), with or without
    // insane allies; back to defaults hands it to the game. The indirect hold honours them too.
    uint8_t FsDisc[VanillaExportBlock::kTacMaxMembers + 1] = {};

    uint8_t FsMask[VanillaExportBlock::kTacMaxMembers + 1] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    uint8_t FsHeat[VanillaExportBlock::kTacMaxMembers + 1] = {};

    std::vector<std::pair<API::UObject *, ULONGLONG>> FsShooters; // enemy -> last time it fired at one of us

    bool FsCustom(int s) const { return FsDisc[s] != 0 || FsMask[s] != kFcAll || FsHeat[s] != 0; }

    float FsHeatLimit(int s) const
    {
        static const float k[4] = {0.85f, 0.60f, 0.95f, 100.0f};
        return k[FsHeat[s] & 3];
    }

    static const wchar_t *FcName(uint8_t c)
    {
        switch (c)
        {
        case kFcLaser: return L"lasers";
        case kFcPPC: return L"PPCs";
        case kFcBallistic: return L"ballistics";
        case kFcLockMissile: return L"lock-on missiles";
        case kFcDumbMissile: return L"dumbfire missiles";
        case kFcMGFlamer: return L"MG / flamers";
        case kFcTAG: return L"TAG";
        case kFcNARC: return L"NARC";
        }
        return L"?";
    }

    bool FsIsOurs(API::UObject *a)
    {
        if (!a)
            return false;
        API::UObject *me = API::get()->get_local_pawn(0);
        if (a == me)
            return true;
        for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers; ++k)
            if (TacUnit[k] == a)
                return true;
        // Allied mission units too (vanillahud-92): anything on the player's team.
        return me && TeamOf(me) != 0xFF && TeamOf(a) == TeamOf(me);
    }

    // BATTLE ARMOUR ON A FRIEND (lancecontrol-64, user 2026-10-17). Elementals climb onto mechs to attack the cockpit, and
    // the friendly-fire check ignores a friend beside the TARGET - a suit on your cockpit would have the lance shoot your
    // mech. So no plugin shot at a suit within 15 m (flat) and 25 m (height: a cockpit sits high above the mech's origin)
    // of a friendly unit - the player or anything on the player's team. Answered at most every 250 ms per suit.
    static constexpr float kBaFriendCm = 1500.0f, kBaFriendUpCm = 2500.0f;
    std::unordered_map<API::UObject *, std::pair<ULONGLONG, bool>> BaNearCache;
    ULONGLONG BaNearLogMs = 0;

    bool BaOnFriend(API::UObject *e)
    {
        if (!e || !ObjStillThere(e) || !IsBattleArmor(e))
            return false;
        const ULONGLONG now = GetTickCount64();
        auto it = BaNearCache.find(e);
        if (it != BaNearCache.end() && now - it->second.first < 250)
            return it->second.second;
        bool near_ = false;
        API::UObject *who = nullptr;
        float p[3] = {};
        if (CallRet(e, L"K2_GetActorLocation", {}, p, sizeof(p)))
        {
            API::UObject *me = API::get()->get_local_pawn(0);
            const uint8_t ours = me ? TeamOf(me) : (uint8_t)0xFF;
            auto close = [&](API::UObject *f) {
                float q[3] = {};
                if (!f || f == e || !ObjStillThere(f) || IsDeadUnit(f) || !CallRet(f, L"K2_GetActorLocation", {}, q, sizeof(q)))
                    return false;
                const float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
                return dx * dx + dy * dy < kBaFriendCm * kBaFriendCm && std::fabs(dz) < kBaFriendUpCm;
            };
            if (me && close(me))
                near_ = true, who = me;
            if (!near_ && ours != 0xFF)
                for (auto *k : Live())
                    if (k != me && k != e && ObjStillThere(k) && TeamOf(k) == ours && IsCombatUnit(k) && close(k))
                    {
                        near_ = true;
                        who = k;
                        break;
                    }
        }
        if (BaNearCache.size() > 256)
            BaNearCache.clear();
        BaNearCache[e] = {now, near_};
        if (near_ && now - BaNearLogMs > 5000)
        {
            BaNearLogMs = now;
            TacLog("battle armour: %ls is on / beside a friendly (%ls) - no plugin shots at it", ClassName(e).c_str(),
                   ClassName(who).c_str());
        }
        return near_;
    }

    // Return fire: is `e` engaging us?
    // lancecontrol-67 (user 2026-10-17): the battle armour 15 m rule is OFF - suits are nearly always beside a friend, so the
    // lance never fired on them. BaOnFriend() above is kept but no longer called.
    bool FsTargetOk(int s, API::UObject *e)
    {
        if (FsDisc[s] != 1)
            return true;
        if (!e)
            return false;
        const ULONGLONG now = GetTickCount64();
        for (auto &r : FsShooters)
            if (r.first == e && now - r.second < 10000)
                return true;
        API::UObject *ectl = ObjProp(e, L"Controller");
        return FsIsOurs(ectl ? ObjProp(ectl, L"CombatTarget") : nullptr);
    }

    // lancecontrol-81 (user, 2026-10-20): TAG BY ITS WEAPONTYPE. The weapon data asset's WeaponType GameplayTag - every
    // weapon in the game and in the user's mods has one (checked in the paks: 4,911 mod weapons, none without), and only
    // TAGs are under Weapon.Energy.TAG (TAG, .Clan, YAW's .Improved / .Light / .Clan.Light, Garrisons' searchlight TAG).
    // The class name lies (the Raven's TAG is Weapon_Mech_RVN_Torso_Right_EH1_Laser_C) and the firing data cannot tell a
    // TAG from a modded continuous laser (YAWE's Weapon.Energy.Laser.Standard.*.Continuous). No readable tag: the old
    // "_tag" in the class / asset name. Per class, logged once.
    // lancecontrol-82: the WeaponType tag itself, cached per class ("(none)" when it cannot be read), logged once per
    // class with the data asset's name - WeaponCat sorts every weapon by it (see there).
    std::vector<std::pair<API::UClass *, std::wstring>> WpnTypeCache;
    std::wstring WeaponTypeOf(API::UObject *w)
    {
        if (!w || !w->get_class())
            return L"(none)";
        for (auto &e : WpnTypeCache)
            if (e.first == w->get_class())
                return e.second;
        API::UObject *da = nullptr;
        if (FindFn(w, L"GetWeaponDataAsset"))
            CallRet(w, L"GetWeaponDataAsset", {}, &da, sizeof(da));
        std::wstring wt = da ? AmmoTag(da, L"WeaponType") : L"(none)";
        if (wt.empty())
            wt = L"(none)";
        WpnTypeCache.push_back({w->get_class(), wt});
        TacLog("weapon type: %ls (%ls) -> %ls", ClassName(w).c_str(), da ? da->get_fname()->to_string().c_str() : L"no data asset", wt.c_str());
        return wt;
    }

    // True when the WeaponType tag is `fam` or under it (`fam` + "."), case-insensitive.
    static bool WtIs(const std::wstring &wt, const wchar_t *fam)
    {
        const size_t n = wcslen(fam);
        return wt.size() >= n && _wcsnicmp(wt.c_str(), fam, n) == 0 && (wt.size() == n || wt[n] == L'.');
    }

    bool IsTagWeapon(API::UObject *w)
    {
        const std::wstring wt = WeaponTypeOf(w);
        if (wt != L"(none)")
            return WtIs(wt, L"Weapon.Energy.TAG");
        API::UObject *da = nullptr; // no readable tag: the old "_tag" in the class / asset name
        if (w && FindFn(w, L"GetWeaponDataAsset"))
            CallRet(w, L"GetWeaponDataAsset", {}, &da, sizeof(da));
        std::wstring lo = (w ? ClassName(w) : L"") + L" " + (da ? ClassName(da) + L" " + da->get_fname()->to_string() : L"");
        for (auto &ch : lo)
            ch = (wchar_t)towlower(ch);
        return lo.find(L"_tag") != std::wstring::npos;
    }

    // The weapon's category: its class name (Weapon_..._EH1_PPC_C, ..._MG_C), its firing data
    // (TraceWeaponData: TAG / MG / flamer / laser) and its data asset (missile data = a missile).
    uint8_t WeaponCat(API::UObject *w, int lockw)
    {
        const std::wstring n = ClassName(w);
        API::UObject *da = nullptr;
        if (FindFn(w, L"GetWeaponDataAsset"))
            CallRet(w, L"GetWeaponDataAsset", {}, &da, sizeof(da));
        const std::wstring dn = da ? ClassName(da) + L" " + da->get_fname()->to_string() : L"";
        std::wstring lo = n + L" " + dn;
        for (auto &ch : lo)
            ch = (wchar_t)towlower(ch);
        // Case-insensitive (vanillahud-92). "_mh" is the class names' missile hardpoint code
        // (..._MH100_Missile15_C), "_mg" / "_tag" the machine gun / TAG tokens.
        auto has = [&](const wchar_t *k) {
            std::wstring kl(k);
            for (auto &ch : kl)
                ch = (wchar_t)towlower(ch);
            return lo.find(kl) != std::wstring::npos;
        };
        // lancecontrol-82 (user, 2026-10-20): BY THE WEAPONTYPE TAG FIRST. The class name is the mech's HARDPOINT slot,
        // not the weapon in it (EH#_Laser holds flamers / TAGs / PPCs, BH#_MG holds autocannons), so the name rules below
        // filed flamers as lasers, ACs in MG slots as MGs, SRMediumRifle as a missile. Lock or dumb-fire still comes from
        // IcLockWeapon (lockw). Plasma rifles / cannons and the TSEMP count as PPCs (user). Artillery / mortars / Arrow IV
        // keep the lock answer (arcing path = lock missile), else ballistic. AMS, melee, an unknown family or no tag:
        // the old name rules.
        {
            const std::wstring wt = WeaponTypeOf(w);
            if (wt != L"(none)")
            {
                if (WtIs(wt, L"Weapon.Energy.TAG"))
                    return kFcTAG;
                if (WtIs(wt, L"Weapon.Missile.NARC"))
                    return kFcNARC;
                if (WtIs(wt, L"Weapon.Energy.ParticleCannon") || WtIs(wt, L"Weapon.Energy.PlasmaRifle") ||
                    WtIs(wt, L"Weapon.Energy.BC-Plasma") || WtIs(wt, L"Weapon.Energy.TSEMP"))
                    return kFcPPC;
                if (WtIs(wt, L"Weapon.Energy.Laser"))
                    return kFcLaser;
                if (WtIs(wt, L"Weapon.Energy.Flamer") || WtIs(wt, L"Weapon.Ballistic.MG"))
                    return kFcMGFlamer;
                if (WtIs(wt, L"Weapon.Missile") || WtIs(wt, L"Weapon.ArtemisMissile"))
                    return lockw == 1 ? kFcLockMissile : kFcDumbMissile;
                if (WtIs(wt, L"Weapon.Artillery") || WtIs(wt, L"Weapon.Ballistic.Artillery") || WtIs(wt, L"Weapon.Ballistic.MechMortar"))
                    return lockw == 1 ? kFcLockMissile : kFcBallistic;
                if (WtIs(wt, L"Weapon.Ballistic"))
                    return kFcBallistic;
            }
        }
        if (lo.find(L"narc") != std::wstring::npos)
            return kFcNARC;
        const WStat ws = WeaponStat(w);
        if (IsTagWeapon(w)) // no / unknown WeaponType: the "_tag" name rule
            return kFcTAG;
        if (has(L"PPC"))
            return kFcPPC;
        if (lockw == 1)
            return kFcLockMissile;
        if (has(L"Missile") || has(L"SRM") || has(L"MRM") || has(L"Rocket") || has(L"_MH"))
            return kFcDumbMissile;
        if (ws.Kind == WK_Beam || has(L"Laser"))
            return kFcLaser;
        if (ws.Kind == WK_Continuous || has(L"_MG") || lo.find(L"flamer") != std::wstring::npos)
            return kFcMGFlamer;
        return kFcBallistic;
    }

    bool FsOrder(const VanillaExportBlock::TacCmd &c)
    {
        const int s = c.Slot;
        if (s < 1 || s > VanillaExportBlock::kTacMaxMembers)
            return false;
        const int disc = (int)c.Loc[0], mask = (int)c.Loc[1], heat = (int)c.Loc[2];
        FsDisc[s] = (uint8_t)(disc >= 0 && disc <= 2 ? disc : 0);
        FsMask[s] = (uint8_t)(mask & 0xFF);
        FsHeat[s] = (uint8_t)(heat >= 0 && heat <= 3 ? heat : 0);
        static const char *kD[] = {"weapons free", "return fire", "hold fire"};
        static const char *kH[] = {"normal 85 %", "cool 60 %", "hot 95 %", "alpha (no heat limit)"};
        std::wstring cats;
        for (int b = 0; b < 8; ++b)
            if (FsMask[s] & (1 << b))
                cats += std::wstring(cats.empty() ? L"" : L", ") + FcName((uint8_t)(1 << b));
        TacLog("fire settings, slot %d - %s, heat %s, weapons: %ls%s", s, kD[FsDisc[s]], kH[FsHeat[s]],
                cats.empty() ? L"none" : cats.c_str(), FsCustom(s) ? " - the plugin fires for it" : " (defaults)");
        return true;
    }

    API::UObject *AtkTgt[VanillaExportBlock::kTacMaxMembers + 1] = {}; // the unit of the slot's attack order, if any

    // TARGET SWITCH (vanillahud-80). A target is FIREABLE while the game says this lancemate sees
    // it with no friendly in the line (direct weapons), or its missile lock on it is complete
    // (lock-on weapons). Cooldown, heat and the aim gate do not count against it. Unfireable for
    // more than kIaUnfireMs -> the nearest enemy the lancemate itself sees (the game's visible
    // list), preferring one with a clear line; none seen -> it keeps its target. Any new target -
    // ours or the game's - is not judged for kIaGraceMs (torso turn, lock forming).
    static constexpr ULONGLONG kIaUnfireMs = 3000, kIaGraceMs = 5000;

    API::UObject *IaTgt[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG     IaOkMs[VanillaExportBlock::kTacMaxMembers + 1] = {};   // last fireable (or grace end - kIaUnfireMs)

    ULONGLONG     IaNoneLogMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int           IaSwitches[VanillaExportBlock::kTacMaxMembers + 1] = {};

    bool LockedOn(API::UObject *pawn, API::UObject *tgt)
    {
        API::UObject *ws = ObjProp(pawn, L"WeaponSystemComponent");
        API::UObject *lc = ws ? ObjProp(ws, L"LockOnComponent") : nullptr;
        if (!lc)
            return false;
        auto *p = FindProp(lc->get_class(), L"LockOnState");
        if (!p || *(reinterpret_cast<uint8_t *>(lc) + p->get_offset()) != 3)
            return false;
        API::UObject *pending = ObjProp(lc, L"PendingLockTarget");
        return !pending || pending == tgt;
    }

    // The game's own list of what this lancemate sees right now.
    std::vector<API::UObject *> OwnVisible(API::UObject *ctl)
    {
        std::vector<API::UObject *> out;
        API::UObject *tvc = ctl ? ObjProp(ctl, L"TargetVisibilityComponent") : nullptr;
        API::FProperty *p = tvc ? FindProp(tvc->get_class(), L"CurrentVisibleActors") : nullptr;
        if (!p || PropType(p) != L"ArrayProperty")
            return out;
        const auto *arr = reinterpret_cast<const TArr *>(reinterpret_cast<const uint8_t *>(tvc) + p->get_offset());
        for (int32_t i = 0; arr->Data && i < arr->Num && i < 64; ++i)
            if (auto *o = static_cast<API::UObject *const *>(arr->Data)[i])
                out.push_back(o);
        return out;
    }

    float AimLoc(API::UObject *a, float *out)
    {
        API::UObject *aim = AimPointOf(a);
        if (!(aim && CallRet(aim, L"K2_GetComponentLocation", {}, out, 12)) && !CallRet(a, L"K2_GetActorLocation", {}, out, 12))
            return -1.0f;
        return 1.0f;
    }

    // Any weapon of this kind (1 lock-on, 0 direct) whose fire check on `tgt` is Success,
    // InFiringSequence or OnCooldown.
    bool IaReach(int s, API::UObject *tgt, int kind) // lancecontrol-48: timed wrapper
    {
        if (!IpOn)
            return IaReachRaw(s, tgt, kind);
        const double t0 = IpNow();
        const bool r = IaReachRaw(s, tgt, kind);
        IpReachMs += IpNow() - t0;
        ++IpReachN;
        return r;
    }
    bool IaReachRaw(int s, API::UObject *tgt, int kind)
    {
        if (!AiFrOk || !AiFrFn)
            return true; // cannot tell: do not count it against the target
        API::UObject *pawn = TacUnit[s];
        float d = -1.0f; // lock-on weapons: the target within rated range too (vanillahud-100)
        if (kind == 1 && pawn)
        {
            float a[3] = {}, b[3] = {};
            if (CallRet(pawn, L"K2_GetActorLocation", {}, a, sizeof(a)) && CallRet(tgt, L"K2_GetActorLocation", {}, b, sizeof(b)))
                d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        }
        for (auto &x : IcWpns[s])
        {
            if (x.Indirect != kind || !(FsMask[s] & x.Cat) || !OnMech(pawn, x.W))
                continue;
            if (kind == 1 && d > 0.0f)
            {
                const float rr = IcWeaponRangeCm(x.W);
                if (rr > 0.0f && d > rr)
                    continue;
            }
            std::vector<uint8_t> f(AiFrBuf.size(), 0);
            for (int32_t o : AiFrObjParams)
                std::memcpy(f.data() + o, &tgt, sizeof(tgt));
            x.W->process_event(AiFrFn, f.data());
            const unsigned v = f[AiFrRet];
            IcLearnLock(s, x, v);
            if (v == 0 || v == 2 || v == 3)
                return true;
        }
        return false;
    }

    // Returns the lancemate's target after the check.
    API::UObject *IaTargetCheck(int s, API::UObject *pawn, API::UObject *ctl, API::UObject *tgt, const float *here, ULONGLONG now)
    {
        // An attack / focus-fire / engage order: the ordered unit is the game's to pursue - no
        // switching while it lives (vanillahud-87).
        if (AtkTgt[s])
        {
            if (IsLive(AtkTgt[s]) && !IaDead(AtkTgt[s]))
            {
                IaTgt[s] = tgt;
                IaOkMs[s] = now;
                return tgt;
            }
            TacLog("insane allies, slot %d - attack order's target gone: target switching resumes", s);
            AtkTgt[s] = nullptr;
        }
        if (tgt != IaTgt[s])
        {
            IaTgt[s] = tgt;
            IaOkMs[s] = now + kIaGraceMs - kIaUnfireMs; // grace for the new target
        }
        if (IcHoldNow(s))
            return tgt; // hold fire: nothing to switch for
        if (tgt && FsTargetOk(s, tgt))
        {
            // A weapon of the kind that could shoot counts only if its own fire check does not rule
            // the target out (range, minimum range, ammo, destroyed): cooldown / firing still count.
            bool ok = LockedOn(pawn, tgt) && IaReach(s, tgt, 1);
            if (!ok && AiGameVisible(ctl, tgt) == 1 && IaReach(s, tgt, 0))
            {
                float ap[3] = {};
                ok = AimLoc(tgt, ap) > 0.0f && !FriendInLine(s, pawn, here, ap);
            }
            if (ok)
            {
                if (IaOkMs[s] < now)
                    IaOkMs[s] = now;
                return tgt;
            }
        }
        if (tgt && now < IaOkMs[s] + kIaUnfireMs)
            return tgt;
        // Unfireable too long (or no target): the nearest enemy it sees, a clear line first.
        const uint8_t ours = IaTeam(pawn);
        API::UObject *clear = nullptr, *any = nullptr;
        float dClear = 1e30f, dAny = 1e30f;
        for (auto *e : OwnVisible(ctl))
        {
            if (e == tgt || e == pawn || IaTeam(e) == ours || IaTeam(e) == 0xFF || IaDead(e) || !IsCombatUnit(e) || !FsTargetOk(s, e))
                continue;
            float p[3] = {};
            if (AimLoc(e, p) < 0.0f)
                continue;
            const float dx = p[0] - here[0], dy = p[1] - here[1], dz = p[2] - here[2], d = dx * dx + dy * dy + dz * dz;
            if (d < dAny)
            {
                dAny = d;
                any = e;
            }
            if (d < dClear && !FriendInLine(s, pawn, here, p))
            {
                dClear = d;
                clear = e;
            }
        }
        API::UObject *pick = clear ? clear : any;
        if (!pick)
        {
            if (now - IaNoneLogMs[s] > 5000)
            {
                IaNoneLogMs[s] = now;
                if (gsp::VerboseLog()) TacLog("insane allies, slot %d - target %ls unfireable, no other enemy in sight: kept", s,
                        tgt ? ClassName(tgt).c_str() : L"none");
            }
            return tgt;
        }
        bool set = false;
        if (ResolveSetTarget(ctl))
        {
            API::UObject *aware = nullptr;
            bool known = false;
            set = GiveTarget(ctl, pawn, pick, aware, known);
        }
        API::UObject *now_t = LsObj(ctl, L"CombatTarget");
        ++IaSwitches[s];
        wchar_t why[64];
        if (tgt && now >= IaOkMs[s])
            _snwprintf_s(why, _countof(why), _TRUNCATE, L"%ls unfireable for %.1f s", ClassName(tgt).c_str(), (now - IaOkMs[s]) / 1000.0f);
        else
            _snwprintf_s(why, _countof(why), _TRUNCATE, L"none");
        TacLog("insane allies, slot %d - target %ls: switched to %ls, %.0f m, in sight%s (set %s, target now %ls)", s,
                why, ClassName(pick).c_str(),
                std::sqrt(clear ? dClear : dAny) * 0.01f, clear ? ", clear line" : ", friendly in the line",
                set ? "ok" : "FAILED", ClassName(now_t).c_str());
        IaTgt[s] = now_t;
        IaOkMs[s] = now + kIaGraceMs - kIaUnfireMs;
        return (now_t && !IaDead(now_t)) ? now_t : nullptr;
    }

    ULONGLONG IaTickMs = 0;

    bool IaWasOn = false;

    ULONGLONG IcRestoreMs = 0;

    ULONGLONG IaRetryMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    // IATICK CACHED READS (lancecontrol-50, user 2026-10-13 "build the fixes"): the insane-allies pass (IaTick,
    // IaTargetCheck, IcOwnFire, IcDirectFire, IcSuppressedNow, the heat reads) used the by-name lookups -
    // IsDeadUnit (for a live unit: two component lookups, the IsDead call and four name searches that walk the
    // whole class chain and miss), ObjProp / FindProp / TeamOf (a name search per read) and K2_GetActorLocation.
    // Now the lc46 cached readers: LsDead, LsObj / LsOff / LsBool, LsPos - the same tests, lookups cached per
    // class. Every 32nd dead read is compared with IsDeadUnit ("iatick dead check", only on disagreement, max 20).
    unsigned IaDeadN = 0;
    int      IaDeadLogs = 0;
    bool IaDead(API::UObject *a)
    {
        const bool r = LsDead(a);
        if ((++IaDeadN & 31u) == 0 && IaDeadLogs < 20)
        {
            const bool o = IsDeadUnit(a);
            if (o != r)
            {
                ++IaDeadLogs;
                LogInfo("iatick dead check: %ls - cached read says %s, the old check says %s", ClassName(a).c_str(),
                        r ? "dead" : "alive", o ? "dead" : "alive");
            }
        }
        return r;
    }
    uint8_t IaTeam(API::UObject *a) // TeamOf with the offset cached per class
    {
        const int32_t o = TeamOff(a);
        return o >= 0 ? *(reinterpret_cast<uint8_t *>(a) + o) : (uint8_t)0xFF;
    }
    double   IpCanMs = 0;
    unsigned IpCanN = 0;
    void IaCanFire(API::UObject *w, uint8_t *f) // the game's per-weapon fire check, timed
    {
        if (!IpOn)
        {
            w->process_event(AiFrFn, f);
            return;
        }
        const double t0 = IpNow();
        w->process_event(AiFrFn, f);
        IpCanMs += IpNow() - t0;
        ++IpCanN;
    }

    // IATICK PROFILE (lancecontrol-48, user 2026-10-13 "run stage 1"): measuring only. IaTick (insane allies / own fire
    // settings: the plugin fires the lancemates' weapons, 4 Hz) timed by part, totals every 30 s as "iatick profile" /
    // "iatick content". Arc traces, friendly-in-line checks and weapon reach checks are timed inside the parts that
    // call them (a subset, not added on top). Nothing decided or fired changes.
    enum { IP_Setup, IP_Target, IP_Own, IP_Direct, IP_Map, IP_Read, IP_N }; // lc49: map / read split out of setup
    LARGE_INTEGER IpT0{}, IpFreq{};
    double        IpLast = 0, IpRun[IP_N] = {}, IpSum[IP_N] = {}, IpMax[IP_N] = {}, IpTotSum = 0, IpTotMax = 0;
    double        IpArcMs = 0, IpFilMs = 0, IpReachMs = 0, IpRestoreMs = 0, IpRestoreMax = 0;
    unsigned      IpArcN = 0, IpFilN = 0, IpReachN = 0, IpFireN = 0, IpSlotN = 0, IpSwN = 0, IpRuns = 0, IpRestoreN = 0;
    ULONGLONG     IpLogMs = 0;
    bool          IpOn = false;

    double IpNow()
    {
        if (!IpFreq.QuadPart)
            QueryPerformanceFrequency(&IpFreq);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return double(t.QuadPart) * 1000.0 / double(IpFreq.QuadPart);
    }
    void IpBegin()
    {
        std::memset(IpRun, 0, sizeof(IpRun));
        IpLast = IpNow();
        IpOn = true;
    }
    void IpMark(int k)
    {
        if (!IpOn)
            return;
        const double t = IpNow();
        IpRun[k] += t - IpLast;
        IpLast = t;
    }
    void IpEnd()
    {
        if (!IpOn)
            return;
        IpOn = false;
        double tot = 0;
        for (int k = 0; k < IP_N; ++k)
        {
            tot += IpRun[k];
            IpSum[k] += IpRun[k];
            IpMax[k] = (std::max)(IpMax[k], IpRun[k]);
        }
        IpTotSum += tot;
        IpTotMax = (std::max)(IpTotMax, tot);
        ++IpRuns;
        const ULONGLONG now = GetTickCount64();
        if (!IpLogMs)
            IpLogMs = now;
        if (now - IpLogMs < 30000)
            return;
        const double r = IpRuns ? double(IpRuns) : 1.0;
        if (gsp::VerboseLog()) LogInfo("iatick profile, last 30 s: %u runs, total avg %.2f / worst %.2f ms - map %.2f/%.2f setup %.2f/%.2f read %.2f/%.2f "
                "target %.2f/%.2f own %.2f/%.2f direct %.2f/%.2f; inside them: arc traces %.2f ms, friendly-in-line %.2f ms, weapon reach %.2f ms, weapon fire checks %.2f ms (per run); "
                "aim restore %.2f ms avg / %.2f worst over %u runs",
                IpRuns, IpTotSum / r, IpTotMax, IpSum[IP_Map] / r, IpMax[IP_Map], IpSum[0] / r, IpMax[0], IpSum[IP_Read] / r,
                IpMax[IP_Read], IpSum[1] / r, IpMax[1], IpSum[2] / r, IpMax[2],
                IpSum[3] / r, IpMax[3], IpArcMs / r, IpFilMs / r, IpReachMs / r, IpCanMs / r,
                IpRestoreN ? IpRestoreMs / IpRestoreN : 0.0, IpRestoreMax, IpRestoreN);
        if (gsp::VerboseLog()) LogInfo("iatick content, last 30 s (per run): lancemates handled %.2f, arc traces %.1f, friendly-in-line checks %.1f, "
                "reach checks %.1f, weapon fire checks %.1f, fire calls %.2f, target switches %u in total",
                IpSlotN / r, IpArcN / r, IpFilN / r, IpReachN / r, IpCanN / r, IpFireN / r, IpSwN);
        std::memset(IpSum, 0, sizeof(IpSum));
        std::memset(IpMax, 0, sizeof(IpMax));
        IpTotSum = IpTotMax = IpArcMs = IpFilMs = IpReachMs = IpRestoreMs = IpRestoreMax = IpCanMs = 0;
        IpArcN = IpFilN = IpReachN = IpCanN = IpFireN = IpSlotN = IpSwN = IpRuns = IpRestoreN = 0;
        IpLogMs = now;
    }

    void IaTick(API::UObject *pc)
    {
        const ULONGLONG now = GetTickCount64();
        const bool on = GsInsane; // lancecontrol-19: GlobalSettings.ini [lance] insane_allies
        if (on != IaWasOn)
        {
            IaWasOn = on;
            TacLog("insane allies: %s", on ? "ON - the plugin fires every ready lancemate weapon" : "OFF - the game fires again");
        }
        bool any = on;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            any |= IaSet[s] || FsCustom(s);
        bool anyIc = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            anyIc |= IcOn[s];
        // Moved aim points go back whatever else runs (vanillahud-92) - all of them at once when
        // nothing of ours fires any more.
        if (!IcMoved.empty() && now - IcRestoreMs >= 100)
        {
            IcRestoreMs = now;
            bool anyCsV = false; // lancecontrol-42: a vanilla called shot holds the target's AI aim point too
            for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
                anyCsV |= CsVAim[s] != nullptr;
            const double rt0 = IpNow(); // lancecontrol-48
            IcRestoreAims(now, !any && !anyIc && !anyCsV);
            const double rt = IpNow() - rt0;
            IpRestoreMs += rt;
            IpRestoreMax = (std::max)(IpRestoreMax, rt);
            ++IpRestoreN;
        }
        if (!any || now - IaTickMs < 250)
            return;
        IaTickMs = now;
        IpBegin(); // lancecontrol-48
        IaInPass = true; // lancecontrol-49: the friendly list is built (once) for this pass
        ++IaPassNo;      // lancecontrol-50: the heat component is found once per mech per pass
        IaFrBuilt = false;
        // No player mech (destroyed, or the level going): the game fires for the lancemates again,
        // or they would stand silent. Only units still in the level are touched (vanillahud-87).
        if (!API::get()->get_local_pawn(0))
        {
            for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            {
                if (!IaSet[s])
                    continue;
                IaSet[s] = false;
                IaTgt[s] = nullptr;
                API::UObject *pawn = TacUnit[s];
                const bool alive = pawn && IsLive(pawn) && !IsDeadUnit(pawn);
                if (alive)
                {
                    IcDirStop(s, true, 0); // stops them and puts their aim targets back
                    for (auto *w : IcFiring[s])
                        if (OnMech(pawn, w))
                            IcCallNoArgs(w, L"K2_StopFire");
                    IcMaskOff(s, pawn, true);
                }
                else
                    IcMaskSet[s] = false;
                IcDirFiring[s].clear();
                IcFiring[s].clear();
                TacLog("insane allies, slot %d - no player mech: %s", s, alive ? "game firing restored" : "unit gone");
            }
            IpOn = false; // lancecontrol-48: not counted
            IaInPass = false;
            return;
        }
        IpMark(IP_Setup);
        TacRefreshMap(pc, now);
        IpMark(IP_Map); // lancecontrol-49
        API::UObject *me = API::get()->get_local_pawn(0);
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            API::UObject *pawn = TacUnit[s];
            const bool alive = pawn && pawn != me && !IaDead(pawn); // lancecontrol-50
            if (IcOn[s]) // the hold order runs its own rules (and its own suppression)
            {
                if (IaSet[s] && alive)
                {
                    // Stop what we started for it (vanillahud-92: a held MG kept firing).
                    IcDirStop(s, true, 0);
                    for (auto *w : IcFiring[s])
                        if (OnMech(pawn, w))
                            IcCallNoArgs(w, L"K2_StopFire");
                    IcFiring[s].clear();
                }
                IaSet[s] = false;
                // lancecontrol-71 (bug sweep): a takeover hands the firing back (IcMaskSet off) but keeps the hold order;
                // its suppression was only ever set when the order came, so the lancemate then fired every direct
                // weapon under game control. Put back here, every 2 s until it takes.
                if (alive && !IcMaskSet[s] && now - IaRetryMs[s] > 2000)
                {
                    IaRetryMs[s] = now;
                    IcMaskOn(s, pawn, true);
                    if (IcMaskSet[s])
                        TacLog("hold (indirect fire), slot %d - game firing suppressed again (after a takeover)", s);
                }
                continue;
            }
            const bool want = on || FsCustom(s); // insane allies, or this lancemate's own fire settings
            if (!want || !alive)
            {
                if (IaSet[s])
                {
                    IaSet[s] = false;
                    IcDirStop(s, true, 0);
                    for (auto *w : IcFiring[s])
                        if (alive && OnMech(pawn, w))
                            IcCallNoArgs(w, L"K2_StopFire");
                    IcFiring[s].clear();
                    if (alive)
                        IcMaskOff(s, pawn, true); // the summary below says it
                    else
                        IcMaskSet[s] = false;
                    TacLog("insane allies, slot %d - %s: direct shots %d, lock-on volleys %d, target switches %d", s,
                            alive ? "game firing restored" : "unit gone", IcDirFires[s], IcOwnFires[s], IaSwitches[s]);
                    IaTgt[s] = nullptr;
                }
                continue;
            }
            // Not taken over last time: try again every 2 s (vanillahud-92 - the unit or its weapon
            // list may not have been ready at mission start).
            if (IaSet[s] && !IcMaskSet[s] && now - IaRetryMs[s] > 2000)
                IaSet[s] = false;
            if (!IaSet[s])
            {
                IaSet[s] = true;
                IaRetryMs[s] = now;
                IcSpotter[s] = false;
                IcMaskOn(s, pawn, true);
                if (IcMaskSet[s])
                    TacLog("insane allies, slot %d - game firing SUPPRESSED (%s); weapons fired by the plugin:%ls", s,
                            on ? "insane allies" : "its fire settings", IcWpnList[s].c_str());
                else
                    TacLog("insane allies, slot %d - NOT taken over (%s): the game keeps firing. Weapons:%ls", s,
                            IcNoFire[s] ? IcNoFire[s] : "?", IcWpnList[s].c_str());
            }
            IcWatchHold(s, pawn);
            // Taken over only to hold fire, but the plugin cannot fire for it: once firing is
            // allowed again the game takes it back (it is retried in 2 s, and stays with the game).
            if (IcMaskSet[s] && IcCantFire[s] && !IcHoldNow(s) && FsMask[s] != 0)
            {
                IcMaskOff(s, pawn, true);
                IaRetryMs[s] = now;
                TacLog("slot %d - firing allowed again, but the plugin cannot fire for it (%s): the game fires", s, IcCantFire[s]);
                continue;
            }
            IpMark(IP_Setup); // lancecontrol-49: bookkeeping above, reads below
            float here[3] = {};
            API::UObject *ctl = LsObj(pawn, L"Controller"); // lancecontrol-50: cached offset / LsPos
            if (!ctl || !LsPos(pawn, here))
            {
                IcDirStop(s, false, now); // held weapons still stop on time (vanillahud-92)
                continue;
            }
            API::UObject *tgt = LsObj(ctl, L"CombatTarget");
            if (tgt && IaDead(tgt))
                tgt = nullptr;
            ++IpSlotN; // lancecontrol-48
            const int sw0 = IaSwitches[s];
            IpMark(IP_Read);
            tgt = IaTargetCheck(s, pawn, ctl, tgt, here, now);
            IpMark(IP_Target);
            IpSwN += (unsigned)(IaSwitches[s] - sw0);
            IcOwnFire(s, pawn, ctl, tgt, here);
            IpMark(IP_Own);
            IcDirectFire(s, pawn, ctl, tgt, here, now);
            IpMark(IP_Direct);
        }
        IpMark(IP_Setup);
        IpEnd(); // lancecontrol-48
        IaInPass = false; // lancecontrol-49
    }

    // WATCH: what the game puts in AIOverrideAimTarget while its own firing runs.
    ULONGLONG AimWatchMs = 0;

    int       AimWatchLines = 0;

    std::vector<std::pair<API::UObject *, API::UObject *>> AimWatchLast;

    void AimWatch(API::UObject *pc)
    {
        const ULONGLONG now = GetTickCount64();
        if (now - AimWatchMs < 250 || AimWatchLines >= 120 || !API::get()->get_local_pawn(0))
            return;
        AimWatchMs = now;
        TacRefreshMap(pc, now);
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        if (!acc)
            return;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            API::UObject *pawn = TacUnit[s];
            if (!pawn || IcMaskSet[s] || IsDeadUnit(pawn))
                continue;
            std::vector<API::UObject *> comps;
            if (!ComponentsOn(pawn, acc, comps))
                continue;
            API::UObject *ctl = ObjProp(pawn, L"Controller");
            API::UObject *tgt = ctl ? ObjProp(ctl, L"CombatTarget") : nullptr;
            for (auto *w : comps)
            {
                if (!AiIsWeaponComp(w))
                    continue;
                const int32_t off = AimOverrideOff(w);
                if (off < 0)
                    continue;
                API::UObject *v = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(w) + off);
                auto it = std::find_if(AimWatchLast.begin(), AimWatchLast.end(), [&](auto &e) { return e.first == w; });
                if (it == AimWatchLast.end())
                {
                    AimWatchLast.push_back({w, v});
                    continue;
                }
                if (it->second == v)
                    continue;
                it->second = v;
                std::wstring owner = L"-";
                if (v)
                    if (auto *o = v->get_outer())
                        owner = ObjTag(o);
                ++AimWatchLines;
                TacLog("aim watch, slot %d - %ls AIOverrideAimTarget -> %ls (owner %ls); target %ls; controller aim location %ls", s,
                        w->get_fname()->to_string().c_str(), ObjTag(v).c_str(), owner.c_str(), ObjTag(tgt).c_str(),
                        ObjTag(ctl ? ObjProp(ctl, L"FiringAimLocationComponent") : nullptr).c_str());
            }
        }
    }

    void IcEnd(int s, const char *why)
    {
        if (!IcOn[s])
            return;
        IcOn[s] = false;
        if (API::UObject *u = TacUnit[s])
            for (auto *w : IcFiring[s])
                if (OnMech(u, w))
                    IcCallNoArgs(w, L"K2_StopFire"); // started on the last tick
        IcFiring[s].clear();
        IcDirStop(s, true, 0);
        IcSpotter[s] = false;
        // lancecontrol-58 (2026-10-14, user): while insane allies or this lancemate's own fire settings (IaTick) hold its
        // firing, ending the indirect hold must not hand firing back to the game - on HOLD FIRE the next order (hold
        // position, move ...) would let it open up until IaTick took it over again (~2 s).
        if (IaSet[s] && IcMaskSet[s])
            TacLog("tacops: hold (indirect fire), slot %d - firing stays with the plugin (%s)", s,
                    IcHoldNow(s) ? "hold fire kept" : "insane allies / fire settings");
        else
            IcMaskOff(s, TacUnit[s]);
        HoldIndirect[s] = false;
        TacLog("tacops: hold (indirect fire), slot %d - ended (%s) after %.0f s: exposed %d time(s), target switches %d", s, why,
                (GetTickCount64() - IcStartMs[s]) / 1000.0f, IcSeenCount[s], IcTTSwitches[s]);
    }

    void IcTick(API::UObject *pc)
    {
        bool any = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            any |= IcOn[s];
        const ULONGLONG now = GetTickCount64();
        if (!any || now - IcTickMs < 250)
            return;
        IcTickMs = now;
        TacRefreshMap(pc, now);
        bool sent = false; // one order per tick, all slots together
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            if (!IcOn[s])
                continue;
            API::UObject *pawn = TacUnit[s];
            float here[3] = {};
            if (!pawn || IsDeadUnit(pawn) || !CallRet(pawn, L"K2_GetActorLocation", {}, here, sizeof(here)))
            {
                IcOn[s] = false;
                HoldIndirect[s] = false;
                IcMaskSet[s] = false; // the mech is gone with its weapon system
                IcFiring[s].clear();
                IcDirFiring[s].clear();
                IcSpotter[s] = false;
                TacLog("tacops: hold (indirect fire), slot %d - ended (unit gone)", s);
                continue;
            }
            IcWatchHold(s, pawn);
            IcWatchDirect(s, pawn);
            std::vector<API::UObject *> seers, enemies;
            const int seen = IcSeenBy(pc, pawn, here, seers, enemies);
            API::UObject *ctl = ObjProp(pawn, L"Controller");
            API::UObject *tgt = ctl ? ObjProp(ctl, L"CombatTarget") : nullptr;
            if (tgt && IsDeadUnit(tgt))
                tgt = nullptr;
            const float el = (now - IcStartMs[s]) / 1000.0f;
            if (seen)
                IcUnseenMs[s] = now;

            // SEEN = EXPOSED: all weapons, spotter first. The moment any (non-VTOL) enemy sees it -
            // by that enemy's own visible list - the game's firing goes back on (its direct
            // weapons as well as the LRMs) and its combat target becomes the nearest enemy seeing
            // it. It keeps its position (or its walk to the point). Back to LRMs only once nobody
            // has seen it for 2 s (below).
            if (seen && ctl)
            {
                const bool first = !IcSpotter[s];
                if (first)
                {
                    IcSpotter[s] = true;
                    ++IcSeenCount[s];
                    // aim test: the game's firing stays off, the plugin fires the direct weapons
                }
                if (first || now - IcSpotMs[s] > 1000)
                {
                    IcSpotMs[s] = now;
                    bool set = false;
                    API::UObject *sp = IcTargetSpotter(s, pawn, ctl, here, seers, tgt, set);
                    if (first || set)
                        TacLog("tacops: hold (indirect fire), slot %d +%.1fs - EXPOSED (seen by %d): all weapons (plugin, aim test), target the spotter %ls%s", s,
                                el, seen, sp ? ClassName(sp).c_str() : L"?", sp == tgt ? " (already its target)" : (set ? "" : " - NOT SET"));
                    tgt = ObjProp(ctl, L"CombatTarget");
                }
            }
            // Exposure ends once nobody (non-VTOL) has seen it for 2 s: direct fire held again.
            if (IcSpotter[s] && now - IcUnseenMs[s] > 2000)
            {
                IcSpotter[s] = false;
                IcDirStop(s, true, 0);
                TacLog("tacops: hold (indirect fire), slot %d +%.1fs - unseen for 2 s: direct fire held again, LRMs only (plugin direct shots so far %d)", s, el, IcDirFires[s]);
            }
            // TARGET TIMER (vanillahud-86, replaces the 900 m swap): hidden, a target its lock-on
            // weapons cannot fire on for 5 s is swapped for the nearest known enemy. Not while
            // fighting its spotters (their target is chosen above); a new target after that gets
            // the grace again.
            if (IcSpotter[s])
                IcTT[s] = nullptr;
            else if (ctl)
                tgt = IcTargetTimer(pc, s, pawn, ctl, here, tgt, now);
            // The game's firing is suppressed for the whole order (the walk to the point included):
            // the plugin fires the lock-on weapons, on the way as well as at the point.
            if (ctl)
                IcOwnFire(s, pawn, ctl, tgt, here);
            if (ctl && IcSpotter[s])
                IcDirectFire(s, pawn, ctl, tgt, here, now);

            // Walking to the point: at it, hold there (Wait), LRMs on what the team knows.
            if (IcPhase[s] == IC_Move)
            {
                const float dx = TacLastMove[s][0] - here[0], dy = TacLastMove[s][1] - here[1];
                if (dx * dx + dy * dy < 2000.0f * 2000.0f && !TacPendMs[s] && !sent)
                {
                    const bool ok = IcSend(pc, s, VanillaExportBlock::TCO_HoldHere, here, true);
                    IcPhase[s] = IC_Engage;
                    sent = true;
                    TacLog("tacops: hold (indirect fire), slot %d +%.1fs - at the point: holding, LRMs on what the team knows%s", s,
                            el, ok ? "" : " - HOLD FAILED");
                }
            }
        }
    }

    // =====================================================================================
    // AI PROBE (vanillahud-62) - DISCOVERY ONLY: reads, never writes to the game. Everything a
    // lancemate does that reflection can reach, logged as CHANGES to its own file, ai_probe.log,
    // next to the game log (UEVR's MechWarrior folder):
    //   - its blackboard: every key the behaviour tree reads (Guard / Attack / Wait / TravelLocation
    //     / CombatTarget / FleeLocation / MissionOrders ...), by key type;
    //   - the controller: combat target, tactic (Ranged / Indirect / Tank / Mobile / HitAndRun),
    //     engagement, hold-fire, idle, speed limit; "took damage" when LastTimeDamageTaken moves;
    //   - movement: move status, the point it is moving to, speed;
    //   - its weapon control (Idle / Plan / Track / Wait / Firing), per-weapon records (can fire,
    //     ammo, destroyed; "fired" when a weapon's TimeUntilFire jumps up), its loadout once;
    //   - its target records (in line of sight, in sensor range, relayed by the team, selected);
    //   - what its perception component currently perceives;
    //   - heat, in 10 % steps;
    //   - the BEHAVIOUR TREE: every blueprint task starting / aborting, every blueprint service
    //     switching on / off, every blueprint decorator's answer changing (hooked on the node's
    //     event functions). Built-in nodes (MoveTo, Wait, EQS) are not blueprints: they show up
    //     as the blackboard and movement changes they cause.
    // The layout of each object it reads (every property and its type) is logged once.
    // Volume: lines are collected and written once a second; past 300 in one second the rest are
    // dropped and counted. lancecontrol-41: switched by GlobalSettings.ini [debug] ai_probe (GsTick), OFF by default -
    // off, nothing is collected or written (AiLog returns at once) and ai_probe.log is not created.
    // =====================================================================================
    bool AiProbeOn = false;

    static constexpr int  kAiMaxLinesPerSec = 1000; // property lists and marks do not count against it

    bool        AiExempt = false;                  // set while writing a property list or a mark

    const wchar_t *AiCurTag = L"[event]";          // where the next AiSet line comes from (see AiProbeTick)

    int         AiMarks = 0;

    bool        AiMarkDown = false;

    // Every "tacops:" line - our own orders and what the plugin did about them (hold, fall back,
    // advance, target handing, autopilot, torso) - goes to the main log AND to ai_probe.log, so
    // our interference is never mistaken for the AI's own decision.
    template <typename... Args> void TacLog(const char *format, Args... args)
    {
        LogInfo(format, args...);
        if (!AiProbeOn)
            return;
        char b[1024];
        _snprintf_s(b, _TRUNCATE, format, args...);
        AiLog(L"[order] %hs", b);
    }

    std::mutex  AiMx;

    FILE       *AiFile = nullptr;

    bool        AiFileTried = false;

    std::string AiBuf;

    int         AiLinesThisSec = 0, AiDropped = 0;

    ULONGLONG   AiSecMs = 0, AiTickMs = 0, AiBbMs = 0, AiReportMs = 0;

    int         AiBbNext = 1;

    std::vector<void *> FireProbeDumped; // classes / structs whose layout has been logged

    void AiLog(const wchar_t *fmt, ...)
    {
        if (!AiProbeOn)
            return; // lancecontrol-41: the AI probe is a debug setting - off, no buffer grows
        // Long enough for a weapon controller state snapshot or a behaviour config dump.
        static thread_local wchar_t w[8192];
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf_s(w, _TRUNCATE, fmt, ap);
        va_end(ap);
        static thread_local char n[16384];
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, n, sizeof(n), nullptr, nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char ts[32];
        _snprintf_s(ts, _countof(ts), _TRUNCATE, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        std::lock_guard<std::mutex> lk(AiMx);
        if (!AiExempt)
        {
            if (AiLinesThisSec >= kAiMaxLinesPerSec)
            {
                ++AiDropped;
                return;
            }
            ++AiLinesThisSec;
        }
        AiBuf += ts;
        if (len > 0)
            AiBuf.append(n, (size_t)len - 1);
        AiBuf += '\n';
    }

    // Once a second: the collected lines in one write. The file is started fresh each launch.
    void AiFlush()
    {
        std::lock_guard<std::mutex> lk(AiMx);
        if (!AiFileTried)
        {
            AiFileTried = true;
            wchar_t path[MAX_PATH] = L"";
            HMODULE me = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&LanceControl::AiStaticAnchor), &me) &&
                GetModuleFileNameW(me, path, MAX_PATH))
            {
                // ...\MechWarrior-Win64-Shipping\plugins\VanillaHud.dll -> ...\MechWarrior-Win64-Shipping\ai_probe.log
                std::wstring p(path);
                for (int up = 0; up < 2; ++up)
                {
                    const size_t k = p.find_last_of(L"\\/");
                    if (k != std::wstring::npos)
                        p.resize(k);
                }
                p += L"\\ai_probe.log";
                // Shared for reading, so the log can be read while the game runs (fopen_s denies it).
                AiFile = _wfsopen(p.c_str(), L"wb", _SH_DENYNO);
            }
            LogInfo("ai probe: %s", AiFile ? "writing ai_probe.log next to the game log" : "could not open ai_probe.log");
        }
        if (AiDropped)
        {
            char b[64];
            _snprintf_s(b, _countof(b), _TRUNCATE, "(%d line(s) dropped: over %d in one second)\n", AiDropped, kAiMaxLinesPerSec);
            AiBuf += b;
            AiDropped = 0;
        }
        AiLinesThisSec = 0;
        if (AiFile && !AiBuf.empty())
        {
            fwrite(AiBuf.data(), 1, AiBuf.size(), AiFile);
            fflush(AiFile);
        }
        AiBuf.clear();
    }

    static void AiStaticAnchor() {}

    static API::FProperty *StructProp(API::UStruct *st, const wchar_t *name)
    {
        for (; st; st = st->get_super_struct())
            if (auto *p = st->find_property(name))
                return p;
        return nullptr;
    }

    // A short, stable name for an object: its class and the low bits of its address.
    static std::wstring ObjTag(API::UObject *o)
    {
        if (!o)
            return L"none";
        wchar_t b[24];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"#%04x", (unsigned)(reinterpret_cast<uintptr_t>(o) >> 4) & 0xFFFF);
        return ClassName(o) + b;
    }

    // One value, readable. `base` is the object / struct element the property's offset is from.
    std::wstring FpVal(API::FProperty *p, uint8_t *base)
    {
        const std::wstring t = PropType(p);
        uint8_t *at = base + p->get_offset();
        wchar_t b[160];
        if (t == L"FloatProperty")
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%.2f", *reinterpret_cast<float *>(at));
        else if (t == L"IntProperty")
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d", *reinterpret_cast<int32_t *>(at));
        else if (t == L"BoolProperty")
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d", reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(base) ? 1 : 0);
        else if (t == L"ByteProperty" || t == L"EnumProperty")
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%u", (unsigned)*at);
        else if (t == L"ObjectProperty")
            return ObjTag(*reinterpret_cast<API::UObject **>(at));
        else if (t == L"ArrayProperty")
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"[%d]", *reinterpret_cast<int32_t *>(at + sizeof(void *)));
        else if (t == L"NameProperty")
            return reinterpret_cast<API::FName *>(at)->to_string();
        else
            return L"(" + t + L")";
        return b;
    }

    // Every property of a class or struct, once: name and type (and the struct / inner type).
    void FpDumpLayout(API::UStruct *st, const char *label)
    {
        if (!st)
            return;
        for (void *d : FireProbeDumped)
            if (d == st)
                return;
        FireProbeDumped.push_back(st);
        AiExempt = true;
        std::wstring list;
        int n = 0;
        for (auto *s2 = st; s2; s2 = s2->get_super_struct())
        {
            const std::wstring sn = s2->get_fname()->to_string();
            if (sn == L"Object" || sn == L"ActorComponent" || sn == L"Actor" || sn == L"Controller")
                break;
            for (auto *f = s2->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                std::wstring t = PropType(p);
                if (t == L"StructProperty")
                    if (auto *ss = reinterpret_cast<API::FStructProperty *>(p)->get_struct())
                        t += L"<" + ss->get_fname()->to_string() + L">";
                if (t == L"ArrayProperty")
                    if (auto *in = reinterpret_cast<API::FArrayProperty *>(p)->get_inner())
                    {
                        std::wstring it = PropType(in);
                        if (it == L"StructProperty")
                            if (auto *ss = reinterpret_cast<API::FStructProperty *>(in)->get_struct())
                                it = ss->get_fname()->to_string();
                        t += L"<" + it + L">";
                    }
                list += L" " + f->get_fname()->to_string() + L":" + t;
                if (++n % 12 == 0)
                {
                    AiLog(L"layout %hs%ls", label, list.c_str());
                    list.clear();
                }
            }
        }
        AiLog(L"layout %hs (%ls, %d properties)%ls", label, st->get_fname()->to_string().c_str(), n, list.c_str());
        AiExempt = false;
    }

    // An array of structs on `base` (of type `st`) whose element struct is named like `want`, or
    // has a member named `member`. Out: the array, its element struct and stride.
    bool FpFindArray(API::UStruct *st, uint8_t *base, const wchar_t *want, const wchar_t *member, uint8_t *&data,
                     int32_t &num, API::UScriptStruct *&es, int32_t &stride, std::wstring &name)
    {
        for (auto *s2 = st; s2; s2 = s2->get_super_struct())
            for (auto *f = s2->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                if (PropType(p) != L"ArrayProperty")
                    continue;
                auto *in = reinterpret_cast<API::FArrayProperty *>(p)->get_inner();
                if (!in || PropType(in) != L"StructProperty")
                    continue;
                auto *ss = reinterpret_cast<API::FStructProperty *>(in)->get_struct();
                if (!ss)
                    continue;
                const std::wstring sn = ss->get_fname()->to_string();
                const bool match = (want && sn.find(want) != std::wstring::npos) || (member && StructProp(ss, member));
                if (!match)
                    continue;
                uint8_t *at = base + p->get_offset();
                data = *reinterpret_cast<uint8_t **>(at);
                num = *reinterpret_cast<int32_t *>(at + sizeof(void *));
                es = ss;
                stride = ss->get_struct_size();
                name = f->get_fname()->to_string();
                return stride > 0;
            }
        return false;
    }

    // The values of the named members of one struct element, as "name=value ...".
    std::wstring FpFields(API::UStruct *st, uint8_t *el, std::initializer_list<const wchar_t *> names)
    {
        std::wstring out;
        for (const wchar_t *n : names)
            if (auto *p = StructProp(st, n))
                out += std::wstring(L" ") + n + L"=" + FpVal(p, el);
        return out;
    }

    struct AiRel // one unit's relationship record for a target (the game's own flags); -1 = unknown
    {
        int Known = 0, Los = -1, Sensor = -1, Relay = -1, Sel = -1;
    };

    // ---- per lancemate: the last value of everything, so only changes are logged ----------
    struct AiSlot
    {
        API::UObject *Pawn = nullptr, *Ctl = nullptr;
        std::wstring  PawnName; // its class, read while it was alive: reports never touch Pawn (vanillahud-99)
        std::vector<std::pair<std::wstring, std::wstring>> Vals;
        bool          DmgInit = false;
        float         Dmg = 0.0f;
        std::vector<float> WpnTime;
        bool          LoadoutLogged = false;
        std::vector<std::wstring> Targets, Perceived, Sees, Reports;
        AiRel                     GRel, YRel; // its / your flags for its combat target, this tick
        API::UObject *Heat = nullptr;
        bool          HeatSearched = false;
        int           WcsState = -1;        // the weapon controller's last EAIWeaponControlState
        bool          WcsCfgLogged = false;
        // One-time notes when something the probe expects is not there, so an empty section is
        // never read as "nothing happened".
        bool          NoteWcs = false, NoteWpn = false, NoteTgt = false, NoteHeat = false, NoteBb = false, NoteWpnComp = false;
        // Weapon components (the game's own), their firing state.
        std::vector<API::UObject *> Wpns;
        std::vector<std::wstring>   WpnNames;
        std::vector<uint8_t>        WpnFiring;
        bool                        WpnSearched = false;
        int                         GameLos = -1; // the target record's bInLineOfSight for the current target
        int                         LosEngine = -1, LosVisible = -1; // this tick, for the current target
        bool                        LockNow = false;
        // The firing test.
        struct Cond
        {
            double Secs = 0.0;
            int    Volleys = 0, Rounds = 0, FireStarts = 0;
        };
        Cond          Conds[4];
        int           CurCond = -1;
        ULONGLONG     CondMs = 0;
        int           RoundsTotal = 0;
        ULONGLONG     EpStart = 0;
        API::UObject *EpTarget = nullptr;
        std::wstring  EpTargetTag, EpResults, EpGameLos;
        int           EpVolleys = 0, EpRounds = 0, EpFires = 0;
        bool          EpLockAll = true, EpLockAny = false;
    };

    void AiNote(int s, bool &flag, const wchar_t *what)
    {
        if (flag)
            return;
        flag = true;
        AiLog(L"[note] slot %d  %ls", s, what);
    }

    AiSlot AiSt[VanillaExportBlock::kTacMaxMembers + 1];

    void AiSet(int s, const std::wstring &key, const std::wstring &val)
    {
        for (auto &kv : AiSt[s].Vals)
            if (kv.first == key)
            {
                if (kv.second != val)
                {
                    AiLog(L"%ls slot %d  %ls: %ls -> %ls", AiCurTag, s, key.c_str(), kv.second.c_str(), val.c_str());
                    kv.second = val;
                }
                return;
            }
        AiSt[s].Vals.push_back({key, val});
        AiLog(L"%ls slot %d  %ls = %ls", AiCurTag, s, key.c_str(), val.c_str());
    }

    // A set of names that comes and goes (targets, perceived actors): arrivals and departures.
    void AiDiffSet(int s, const wchar_t *what, std::vector<std::wstring> &was, const std::vector<std::wstring> &now)
    {
        for (const auto &n : now)
            if (std::find(was.begin(), was.end(), n) == was.end())
                AiLog(L"%ls slot %d  %ls + %ls", AiCurTag, s, what, n.c_str());
        for (const auto &o : was)
            if (std::find(now.begin(), now.end(), o) == now.end())
                AiLog(L"%ls slot %d  %ls - %ls", AiCurTag, s, what, o.c_str());
        was = now;
    }

    static std::wstring AiVec(const float *v)
    {
        if (std::fabs(v[0]) > 1e30f)
            return L"unset";
        wchar_t b[64];
        // Metres, in 5 m steps: a point wandering by a metre is not news.
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"(%d, %d, %d)", (int)std::round(v[0] / 500.0f) * 5, (int)std::round(v[1] / 500.0f) * 5,
                   (int)std::round(v[2] / 500.0f) * 5);
        return b;
    }

    // The blackboard's keys, from its asset (and the assets it inherits), with each key's type.
    struct AiBbKey
    {
        std::wstring Name, Type;
    };

    API::UObject *AiBbAsset = nullptr;

    std::vector<AiBbKey> AiBbKeys;

    void AiBbLoadKeys(API::UObject *asset)
    {
        if (asset == AiBbAsset)
            return;
        AiBbAsset = asset;
        AiBbKeys.clear();
        for (API::UObject *a = asset; a && a->get_class(); a = ObjProp(a, L"Parent"))
        {
            uint8_t *data = nullptr;
            int32_t num = 0, stride = 0;
            API::UScriptStruct *es = nullptr;
            std::wstring an;
            if (!FpFindArray(a->get_class(), reinterpret_cast<uint8_t *>(a), L"BlackboardEntry", nullptr, data, num, es, stride, an))
                break;
            auto *pn = StructProp(es, L"EntryName");
            auto *pt = StructProp(es, L"KeyType");
            for (int32_t i = 0; data && pn && i < num && i < 128; ++i)
            {
                uint8_t *el = data + (size_t)i * stride;
                AiBbKey k;
                k.Name = reinterpret_cast<API::FName *>(el + pn->get_offset())->to_string();
                API::UObject *kt = pt ? *reinterpret_cast<API::UObject **>(el + pt->get_offset()) : nullptr;
                k.Type = kt ? ClassName(kt) : L"?";
                AiBbKeys.push_back(k);
            }
        }
        std::wstring list;
        for (const auto &k : AiBbKeys)
            list += L" " + k.Name + L":" + k.Type;
        AiExempt = true;
        AiLog(L"blackboard %ls: %d key(s)%ls", ClassName(asset).c_str(), (int)AiBbKeys.size(), list.c_str());
        AiExempt = false;
    }

    void AiReadBlackboard(int s, API::UObject *ctl)
    {
        API::UObject *bb = ObjProp(ctl, L"Blackboard");
        API::UObject *asset = bb ? ObjProp(bb, L"BlackboardAsset") : nullptr;
        if (!asset)
        {
            AiNote(s, AiSt[s].NoteBb, bb ? L"blackboard has no readable BlackboardAsset - no blackboard lines"
                                         : L"controller has no Blackboard - no blackboard lines");
            return;
        }
        AiBbLoadKeys(asset);
        for (const auto &k : AiBbKeys)
        {
            API::FName key(k.Name.c_str(), API::FName::EFindName::Find);
            const std::wstring &t = k.Type;
            std::wstring v;
            wchar_t b[64];
            if (t.find(L"Object") != std::wstring::npos)
            {
                API::UObject *o = nullptr;
                if (CallRet(bb, L"GetValueAsObject", {{L"KeyName", &key, sizeof(key)}}, &o, sizeof(o)))
                    v = ObjTag(o);
            }
            else if (t.find(L"Vector") != std::wstring::npos)
            {
                float vec[3] = {};
                if (CallRet(bb, L"GetValueAsVector", {{L"KeyName", &key, sizeof(key)}}, vec, sizeof(vec)))
                    v = AiVec(vec);
            }
            else if (t.find(L"Bool") != std::wstring::npos)
            {
                uint8_t x = 0;
                if (CallRet(bb, L"GetValueAsBool", {{L"KeyName", &key, sizeof(key)}}, &x, 1))
                    v = x ? L"1" : L"0";
            }
            else if (t.find(L"Enum") != std::wstring::npos)
            {
                uint8_t x = 0;
                if (CallRet(bb, L"GetValueAsEnum", {{L"KeyName", &key, sizeof(key)}}, &x, 1))
                {
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%u", (unsigned)x);
                    v = b;
                }
            }
            else if (t.find(L"Int") != std::wstring::npos)
            {
                int32_t x = 0;
                if (CallRet(bb, L"GetValueAsInt", {{L"KeyName", &key, sizeof(key)}}, &x, 4))
                {
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d", x);
                    v = b;
                }
            }
            else if (t.find(L"Float") != std::wstring::npos)
            {
                float x = 0.0f;
                if (CallRet(bb, L"GetValueAsFloat", {{L"KeyName", &key, sizeof(key)}}, &x, 4))
                {
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%.1f", x);
                    v = b;
                }
            }
            else if (t.find(L"Name") != std::wstring::npos)
            {
                API::FName x(L"None", API::FName::EFindName::Find);
                if (CallRet(bb, L"GetValueAsName", {{L"KeyName", &key, sizeof(key)}}, &x, sizeof(x)))
                    v = x.to_string();
            }
            else if (t.find(L"Rotator") != std::wstring::npos)
            {
                float r[3] = {};
                if (CallRet(bb, L"GetValueAsRotator", {{L"KeyName", &key, sizeof(key)}}, r, sizeof(r)))
                {
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L"yaw %d", (int)std::round(r[1] / 5.0f) * 5);
                    v = b;
                }
            }
            else if (t.find(L"Class") != std::wstring::npos)
            {
                API::UObject *o = nullptr;
                if (CallRet(bb, L"GetValueAsClass", {{L"KeyName", &key, sizeof(key)}}, &o, sizeof(o)))
                    v = o && o->get_fname() ? o->get_fname()->to_string() : L"none";
            }
            else
                continue; // strings: an FString return would need its memory handed back
            if (!v.empty())
                AiSet(s, L"bb." + k.Name, v);
        }
    }

    // What the perception component perceives now. The out-array lives in a buffer kept for this
    // call; its pointer is never zeroed (the engine owns that allocation), only its count.
    API::UFunction      *AiPercFn = nullptr;

    std::vector<uint8_t> AiPercBuf;

    int32_t              AiPercOut = -1;

    bool                 AiPercTried = false;

    void AiReadPerception(int s, API::UObject *ctl)
    {
        API::UObject *pc = ObjProp(ctl, L"PerceptionComponent");
        if (!pc)
            return;
        if (!AiPercTried)
        {
            AiPercTried = true;
            FpDumpLayout(pc->get_class(), "perception");
            AiPercFn = FindFn(pc, L"GetCurrentlyPerceivedActors");
            if (AiPercFn)
            {
                AiPercOut = ParamOffset(AiPercFn, L"OutActors");
                const int32_t frame = AiPercFn->get_properties_size();
                if (AiPercOut < 0 || frame <= 0)
                    AiPercFn = nullptr;
                else
                    AiPercBuf.assign((size_t)frame + 16, 0);
            }
            AiLog(L"perception: GetCurrentlyPerceivedActors %ls", AiPercFn ? L"found" : L"NOT FOUND");
        }
        if (!AiPercFn)
            return;
        const int32_t sense = ParamOffset(AiPercFn, L"SenseToUse");
        if (sense >= 0)
            std::memset(AiPercBuf.data() + sense, 0, sizeof(void *)); // every sense
        // The engine ADDS to OutActors without clearing it first: empty it (Num = 0), keeping the
        // allocation it owns - otherwise it grows every call and keeps pointers to destroyed actors
        // (the vanillahud-64 crash).
        const int32_t zero = 0;
        std::memcpy(AiPercBuf.data() + AiPercOut + sizeof(void *), &zero, sizeof(zero));
        pc->process_event(AiPercFn, AiPercBuf.data());
        struct Arr
        {
            API::UObject **Data;
            int32_t        Num, Max;
        } arr{};
        std::memcpy(&arr, AiPercBuf.data() + AiPercOut, sizeof(arr));
        std::vector<std::wstring> now;
        for (int32_t i = 0; arr.Data && i < arr.Num && i < 64; ++i)
            now.push_back(ObjTag(arr.Data[i]));
        AiDiffSet(s, L"perceives", AiSt[s].Perceived, now);
    }

    // ---- enum names, read from the engine's own UEnum -------------------------------------------
    // UEnum keeps TArray<TPair<FName, int64>> Names right after UField (UObject 0x28, Next 0x30,
    // CppType FString 0x30-0x40): at +0x40 in 4.26. Checked before use - every name must carry the
    // enum's own "Name::" prefix or be sensible, else the numbers are logged bare.
    struct AiEnum
    {
        const void *En;
        std::vector<std::wstring> Names;
    };

    std::vector<AiEnum> AiEnums;

    static bool AiEnumRaw(const void *en, uint8_t **data, int32_t *num)
    {
        __try
        {
            *data = *reinterpret_cast<uint8_t *const *>(reinterpret_cast<const uint8_t *>(en) + 0x40);
            *num = *reinterpret_cast<const int32_t *>(reinterpret_cast<const uint8_t *>(en) + 0x48);
            if (!*data || *num <= 0 || *num > 64)
                return false;
            // Layout check before any name is touched: each entry's value (int64 after the FName)
            // must be small and increasing - except the last, the engine's _MAX entry.
            int64_t prev = -1;
            for (int32_t i = 0; i < *num; ++i)
            {
                const int64_t v = *reinterpret_cast<const int64_t *>(*data + (size_t)i * 16 + 8);
                if (i < *num - 1 && (v <= prev || v > 4096))
                    return false;
                prev = v;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const std::vector<std::wstring> *AiEnumNames(API::UObject *en)
    {
        if (!en)
            return nullptr;
        for (const auto &e : AiEnums)
            if (e.En == en)
                return e.Names.empty() ? nullptr : &e.Names;
        AiEnum e{en, {}};
        uint8_t *data = nullptr;
        int32_t num = 0;
        if (AiEnumRaw(en, &data, &num) && data && num > 0 && num <= 64)
        {
            const std::wstring own = en->get_fname() ? en->get_fname()->to_string() : L"";
            bool ok = true;
            for (int32_t i = 0; i < num && ok; ++i)
            {
                std::wstring n = reinterpret_cast<API::FName *>(data + (size_t)i * 16)->to_string();
                const size_t k = n.find(L"::");
                if (k != std::wstring::npos)
                {
                    ok = n.compare(0, k, own) == 0;
                    n = n.substr(k + 2);
                }
                ok = ok && !n.empty() && n.size() < 64;
                e.Names.push_back(n);
            }
            if (!ok)
                e.Names.clear();
        }
        AiEnums.push_back(e);
        AiExempt = true;
        std::wstring list;
        for (size_t i = 0; i < AiEnums.back().Names.size(); ++i)
            list += L" " + std::to_wstring(i) + L"=" + AiEnums.back().Names[i];
        AiLog(L"enum %ls:%ls", ClassName(en) == L"?" ? L"?" : en->get_fname()->to_string().c_str(),
              list.empty() ? L" (names not readable - numbers only)" : list.c_str());
        AiExempt = false;
        return AiEnums.back().Names.empty() ? nullptr : &AiEnums.back().Names;
    }

    // "3 (Indirect)" when the enum's names are known, "3" otherwise.
    std::wstring AiEnumVal(API::FProperty *p, unsigned v)
    {
        std::wstring out = std::to_wstring(v);
        if (p && PropType(p) == L"EnumProperty")
            if (auto *names = AiEnumNames(reinterpret_cast<API::UObject *>(reinterpret_cast<API::FEnumProperty *>(p)->get_enum())))
                if (v < names->size())
                    out += L" (" + (*names)[v] + L")";
        return out;
    }

    // ---- the lancemate's weapons, as the game's own components ---------------------------------
    std::vector<std::pair<API::UClass *, bool>> AiWpnClassIs; // class -> is a weapon component

    bool AiIsWeaponComp(API::UObject *c)
    {
        API::UClass *cls = c ? c->get_class() : nullptr;
        if (!cls)
            return false;
        for (const auto &v : AiWpnClassIs)
            if (v.first == cls)
                return v.second;
        bool is = false;
        for (auto *st = static_cast<API::UStruct *>(cls); st && !is; st = st->get_super_struct())
            is = st->get_fname()->to_string() == L"MWWeaponComponent";
        if (AiWpnClassIs.size() > 512) // lancecontrol-72: bounded
            AiWpnClassIs.clear();
        AiWpnClassIs.push_back({cls, is});
        return is;
    }

    // GetWeaponFireResults (the game's own "can this weapon fire, and if not why"): its caller,
    // built from its signature - every object parameter gets the lancemate's combat target, every
    // plain one zero. Anything else in the signature and it is not called (the log says so).
    API::UFunction      *AiFrFn = nullptr;

    bool                 AiFrTried = false, AiFrOk = false;

    int32_t              AiFrRet = -1;

    API::FProperty      *AiFrRetProp = nullptr;

    std::vector<int32_t> AiFrObjParams;

    std::vector<uint8_t> AiFrBuf;

    API::UFunction      *AiIsFiringFn = nullptr;

    bool                 AiIsFiringTried = false;

    void AiPrepWeaponFns(API::UObject *wc)
    {
        if (!AiFrTried)
        {
            AiFrTried = true;
            AiFrFn = FindFn(wc, L"GetWeaponFireResults");
            if (!AiFrFn)
                AiLog(L"[note] weapons: GetWeaponFireResults not on %ls - no fire-result lines", ClassName(wc).c_str());
            else
            {
                AiLog(L"weapons: %ls on %ls", Signature(AiFrFn).c_str(), ClassName(wc).c_str());
                AiFrOk = true;
                for (auto *f = AiFrFn->get_child_properties(); f; f = f->get_next())
                {
                    auto *p = reinterpret_cast<API::FProperty *>(f);
                    if (!p->is_param())
                        continue;
                    const std::wstring t = PropType(p);
                    if (p->is_return_param())
                    {
                        AiFrRet = p->get_offset();
                        AiFrRetProp = p;
                        continue;
                    }
                    if (t == L"ObjectProperty")
                        AiFrObjParams.push_back(p->get_offset());
                    else if (p->is_out_param() || !(t == L"BoolProperty" || t == L"FloatProperty" || t == L"IntProperty" ||
                                                    t == L"ByteProperty" || t == L"EnumProperty"))
                        AiFrOk = false;
                }
                if (AiFrRet < 0)
                    AiFrOk = false;
                if (AiFrOk)
                    AiFrBuf.assign((size_t)AiFrFn->get_properties_size() + 16, 0);
                AiLog(AiFrOk ? L"weapons: GetWeaponFireResults will be asked every 250 ms per weapon"
                             : L"[note] weapons: GetWeaponFireResults has parameters the probe cannot fill - not called");
            }
        }
        if (!AiIsFiringTried)
        {
            AiIsFiringTried = true;
            for (const wchar_t *n : {L"IsFiring", L"IsWeaponCurrentlyFiring"})
                if (auto *fn = FindFn(wc, n))
                {
                    bool noParams = true;
                    for (auto *f = fn->get_child_properties(); f; f = f->get_next())
                    {
                        auto *p = reinterpret_cast<API::FProperty *>(f);
                        if (p->is_param() && !p->is_return_param())
                            noParams = false;
                    }
                    AiLog(L"weapons: %ls on %ls%ls", Signature(fn).c_str(), ClassName(wc).c_str(),
                          noParams ? L" - used for firing start / stop" : L" - has parameters, not used");
                    if (noParams)
                    {
                        AiIsFiringFn = fn;
                        break;
                    }
                }
            if (!AiIsFiringFn)
                AiLog(L"[note] weapons: no parameterless IsFiring on %ls - firing start / stop from projectiles only",
                      ClassName(wc).c_str());
        }
    }

    // ---- projectiles and missile volleys, counted by who fired them ----------------------------
    API::UClass *AiProjCls = nullptr, *AiPathCls = nullptr;

    struct AiPendingVolley
    {
        void     *Path;
        int       Slot;
        ULONGLONG SpawnMs;
    };

    std::vector<AiPendingVolley> AiPendingLock; // dumb-fire at spawn: re-checked for 2 s

    bool         AiProjTried = false;

    std::vector<void *> AiProjSeen, AiPathSeen;

    void AiResolveProjectiles(API::UObject *ctl)
    {
        if (AiProjTried || !ctl)
            return;
        AiProjTried = true;
        // The game's own module, from the native AI controller class's package ("/Script/<Module>").
        std::wstring pkg;
        for (auto *st = static_cast<API::UStruct *>(ctl->get_class()); st && pkg.empty(); st = st->get_super_struct())
        {
            const std::wstring n = st->get_fname()->to_string();
            if (n == L"MWMechAIController" || n == L"MWAIController")
                if (API::UObject *o = st->get_outer())
                    pkg = o->get_fname()->to_string();
        }
        if (!pkg.empty())
        {
            AiProjCls = API::get()->find_uobject<API::UClass>((L"Class " + pkg + L".MWProjectile").c_str());
            AiPathCls = API::get()->find_uobject<API::UClass>((L"Class " + pkg + L".MWMissilePath").c_str());
        }
        AiLog(L"projectiles: module %ls, MWProjectile %ls, MWMissilePath %ls", pkg.empty() ? L"?" : pkg.c_str(),
              AiProjCls ? L"found" : L"NOT FOUND", AiPathCls ? L"found" : L"NOT FOUND");
    }

    int AiSlotOfCtl(API::UObject *c) const
    {
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers && c; ++s)
            if (AiSt[s].Ctl == c)
                return s;
        return 0;
    }

    int AiSlotOfPawn(API::UObject *p) const
    {
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers && p; ++s)
            if (AiSt[s].Pawn == p)
                return s;
        return 0;
    }

    // ENEMY FIRE, for comparison with our lancemates: every round and missile volley whose
    // instigator is an enemy unit, one line per enemy per scan (at most 20 lines a second), with
    // what it is firing at and ITS OWN relationship flags for that target - does the enemy side
    // fire blind too?
    struct AiEnemyAgg
    {
        API::UObject *Who;
        std::wstring  Rounds;
        int           N;
        bool          Volley;
        API::UObject *Locked;
    };

    std::vector<AiEnemyAgg> AiEnemyPend;

    ULONGLONG AiEnemySecMs = 0;

    int       AiEnemyLines = 0;

    void AiEnemyShot(API::UObject *who, const std::wstring &cls, bool volley, API::UObject *locked)
    {
        API::UObject *me = API::get()->get_local_pawn(0);
        if (!me || TeamOf(who) == TeamOf(me) || TeamOf(who) == 0xFF)
            return;
        {
            // Return fire (vanillahud-91): an enemy whose round / volley went at one of us.
            API::UObject *ectl = ObjProp(who, L"Controller");
            API::UObject *at = locked ? locked : (ectl ? ObjProp(ectl, L"CombatTarget") : nullptr);
            if (FsIsOurs(at))
            {
                bool found = false;
                for (auto &r : FsShooters)
                    if (r.first == who)
                    {
                        r.second = GetTickCount64();
                        found = true;
                    }
                if (!found)
                    FsShooters.push_back({who, GetTickCount64()});
            }
        }
        for (auto &e : AiEnemyPend)
            if (e.Who == who)
            {
                ++e.N;
                e.Volley |= volley;
                if (locked)
                    e.Locked = locked;
                if (e.Rounds.find(cls) == std::wstring::npos)
                    e.Rounds += L" " + cls;
                return;
            }
        AiEnemyPend.push_back({who, L" " + cls, 1, volley, locked});
    }

    void AiEnemyFlush()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - AiEnemySecMs >= 1000)
        {
            AiEnemySecMs = now;
            AiEnemyLines = 0;
        }
        for (auto &e : AiEnemyPend)
        {
            if (AiEnemyLines >= 20 || !e.Who || IsDeadUnit(e.Who))
                continue;
            ++AiEnemyLines;
            API::UObject *ctl = ObjProp(e.Who, L"Controller");
            API::UObject *tgt = ctl ? ObjProp(ctl, L"CombatTarget") : nullptr;
            API::UObject *at = e.Locked ? e.Locked : tgt;
            float a[3] = {}, b[3] = {};
            wchar_t dist[32] = L"?";
            if (at && CallRet(e.Who, L"K2_GetActorLocation", {}, a, sizeof(a)) && CallRet(at, L"K2_GetActorLocation", {}, b, sizeof(b)))
                _snwprintf_s(dist, _countof(dist), _TRUNCATE, L"%.0f m", std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                                                      (a[2] - b[2]) * (a[2] - b[2])) * 0.01f);
            std::wstring st;
            if (ctl)
                if (auto *wp = FindProp(ctl->get_class(), L"WeaponControlSystem"))
                    if (PropType(wp) == L"ObjectProperty")
                        if (API::UObject *w = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(ctl) + wp->get_offset()))
                            if (auto *cp = FindProp(w->get_class(), L"ControlState"))
                                st = AiEnumVal(cp, *(reinterpret_cast<uint8_t *>(w) + cp->get_offset()));
            AiLog(L"[4 Hz] ENEMY %ls (%ls) fired %d%ls%ls at %ls, %ls away | its flags: %ls | tactic %ls, weapon control %ls",
                  ObjTag(e.Who).c_str(), ClassName(e.Who).c_str(), e.N, e.Rounds.c_str(), e.Volley ? L" (MISSILE VOLLEY)" : L"",
                  at ? ObjTag(at).c_str() : L"?", dist, AiRelText(AiRelOf(e.Who, at)).c_str(),
                  [&]() -> std::wstring {
                      auto *p = ctl ? FindProp(ctl->get_class(), L"CombatTactic") : nullptr;
                      return p && PropType(p) == L"EnumProperty" ? AiEnumVal(p, *(reinterpret_cast<uint8_t *>(ctl) + p->get_offset()))
                                                                 : std::wstring(L"?");
                  }().c_str(),
                  st.empty() ? L"?" : st.c_str());
        }
        AiEnemyPend.clear();
    }

    void AiScanProjectiles(API::UObject *pc)
    {
        std::vector<API::UObject *> list;
        // Rounds (missiles, ballistics, PPC bolts ...): new ones, by their Instigator.
        if (AiProjCls && ActorsOf(pc, AiProjCls, list))
        {
            std::vector<void *> now;
            std::vector<std::pair<int, std::wstring>> fresh;
            for (auto *p : list)
            {
                if (!p)
                    continue;
                now.push_back(p);
                if (std::find(AiProjSeen.begin(), AiProjSeen.end(), (void *)p) != AiProjSeen.end())
                    continue;
                API::UObject *ins = ObjProp(p, L"Instigator");
                if (const int s = AiSlotOfPawn(ins))
                    fresh.push_back({s, ClassName(p)});
                else if (ins)
                    AiEnemyShot(ins, ClassName(p), false, nullptr);
            }
            AiProjSeen.swap(now);
            AiEnemyFlush();
            // One line per lancemate and round type: "12 x LRM_Missile_C".
            for (size_t i = 0; i < fresh.size(); ++i)
            {
                if (fresh[i].first == 0)
                    continue;
                int n = 0;
                for (size_t j = i; j < fresh.size(); ++j)
                    if (fresh[j].first == fresh[i].first && fresh[j].second == fresh[i].second)
                    {
                        ++n;
                        if (j != i)
                            fresh[j].first = 0;
                    }
                AiSlot &A = AiSt[fresh[i].first];
                A.RoundsTotal += n;
                A.EpRounds += n;
                if (A.CurCond >= 0)
                    A.Conds[A.CurCond].Rounds += n;
                AiLog(L"[4 Hz] slot %d  ROUNDS SPAWNED %d x %ls", fresh[i].first, n, fresh[i].second.c_str());
            }
        }
        // Missile volleys (MWMissilePath): new ones, with what they are locked on.
        if (AiPathCls && ActorsOf(pc, AiPathCls, list))
        {
            std::vector<void *> now;
            const ULONGLONG tnow = GetTickCount64();
            // Volleys that had no lock when first seen: the lock may be set a moment later. Only
            // paths in THIS scan's list are touched (a finished volley's pointer is never used).
            for (size_t i = 0; i < AiPendingLock.size();)
            {
                AiPendingVolley &pv = AiPendingLock[i];
                API::UObject *mp = nullptr;
                for (auto *c : list)
                    if (c == pv.Path)
                        mp = c;
                API::UObject *lk = nullptr, *owner = nullptr;
                if (mp)
                    CallRet(mp, L"GetLockedTarget", {}, &lk, sizeof(lk));
                if (lk)
                {
                    CallRet(lk, L"GetOwner", {}, &owner, sizeof(owner));
                    AiLog(L"[4 Hz] slot %d  MISSILE VOLLEY lock set %llu ms after launch: locked on %ls", pv.Slot,
                          tnow - pv.SpawnMs, ObjTag(owner ? owner : lk).c_str());
                }
                else if (mp && tnow - pv.SpawnMs < 2000)
                {
                    ++i;
                    continue;
                }
                else
                    AiLog(L"[4 Hz] slot %d  MISSILE VOLLEY confirmed dumb-fire: no lock %ls", pv.Slot,
                          mp ? L"within 2 s" : L"before the volley ended");
                AiPendingLock.erase(AiPendingLock.begin() + i);
            }
            for (auto *mp : list)
            {
                if (!mp)
                    continue;
                now.push_back(mp);
                if (std::find(AiPathSeen.begin(), AiPathSeen.end(), (void *)mp) != AiPathSeen.end())
                    continue;
                API::UObject *ins = ObjProp(mp, L"Instigator");
                const int s = AiSlotOfPawn(ins);
                if (!s)
                {
                    if (ins)
                    {
                        API::UObject *lk = nullptr, *owner = nullptr;
                        CallRet(mp, L"GetLockedTarget", {}, &lk, sizeof(lk));
                        if (lk)
                            CallRet(lk, L"GetOwner", {}, &owner, sizeof(owner));
                        AiEnemyShot(ins, ClassName(mp), true, owner ? owner : lk);
                    }
                    continue;
                }
                API::UObject *lk = nullptr, *owner = nullptr;
                CallRet(mp, L"GetLockedTarget", {}, &lk, sizeof(lk));
                if (lk)
                    CallRet(lk, L"GetOwner", {}, &owner, sizeof(owner));
                AiSlot &A = AiSt[s];
                ++A.EpVolleys;
                if (A.CurCond >= 0)
                    ++A.Conds[A.CurCond].Volleys;
                if (!lk)
                    AiPendingLock.push_back({mp, s, tnow});
                API::UObject *aim = owner ? owner : nullptr;
                wchar_t dist[40] = L"?";
                float a[3] = {}, b[3] = {};
                if (aim && CallRet(A.Pawn, L"K2_GetActorLocation", {}, a, sizeof(a)) &&
                    CallRet(aim, L"K2_GetActorLocation", {}, b, sizeof(b)))
                    _snwprintf_s(dist, _countof(dist), _TRUNCATE, L"%.0f m",
                               std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])) * 0.01f);
                auto yn = [](int v) { return v < 0 ? L"?" : (v ? L"yes" : L"NO"); };
                const bool onCur = aim && aim == ObjProp(A.Ctl, L"CombatTarget");
                AiLog(L"[4 Hz] slot %d  MISSILE VOLLEY %ls, locked on %ls, %ls away | %ls: line of sight game %ls / engine %ls, "
                      L"your lock on it %ls | its flags: %ls | YOUR flags: %ls",
                      s, ClassName(mp).c_str(), lk ? ObjTag(owner ? owner : lk).c_str() : L"nothing (dumb-fire)", dist,
                      onCur ? L"its combat target" : L"NOT its combat target (sight values are for that one)", yn(A.LosVisible),
                      yn(A.LosEngine), aim && AiPlayerLock() == aim ? L"yes" : L"no", AiRelText(A.GRel).c_str(),
                      AiRelText(A.YRel).c_str());
            }
            AiPathSeen.swap(now);
        }
    }

    // The player's own lock (TargetTracking.CurrentTarget), as ReadLock reads it.
    API::UObject *AiPlayerLock()
    {
        API::UObject *me = API::get()->get_local_pawn(0);
        API::UObject *tt = me ? ObjProp(me, L"TargetTracking") : nullptr;
        return tt ? ObjProp(tt, L"CurrentTarget") : nullptr;
    }

    // ---- the firing test: time and shots by condition, and no-line-of-sight episodes ----------
    // Every 250 ms each lancemate WITH a combat target is put in one of four conditions - line of
    // sight to it (the engine's LineOfSightTo) yes / no, x the pilot's lock on that same target yes
    // / no - and the time, rounds, volleys and weapon firing starts are added to that condition.
    // Every 60 s: a table per lancemate. And each stretch of 10 s or more with a target but NO line
    // of sight ends in one "TEST lrm-no-los" verdict line with what it fired and the fire results.
    // The game's own visibility check: the controller's IsActorVisible(Actor, TimeThreshold). Called
    // if it takes one object (the target), otherwise only plain numbers (passed as 0), and returns
    // a bool.
    API::UFunction *AiVisFn = nullptr;

    std::wstring    AiVisParam;

    bool            AiVisTried = false;

    int AiGameVisible(API::UObject *ctl, API::UObject *tgt)
    {
        if (!AiVisTried)
        {
            AiVisTried = true;
            if (API::UObject *tvc = ObjProp(ctl, L"TargetVisibilityComponent"))
                FpDumpLayout(tvc->get_class(), "target visibility");
            else
                AiLog(L"[note] visibility: no TargetVisibilityComponent on the controller");
            AiVisFn = FindFn(ctl, L"IsActorVisible");
            if (!AiVisFn)
                AiLog(L"[note] visibility: IsActorVisible not on %ls - engine line-of-sight check only", ClassName(ctl).c_str());
            else
            {
                int objs = 0, others = 0;
                bool boolRet = false;
                for (auto *f = AiVisFn->get_child_properties(); f; f = f->get_next())
                {
                    auto *p = reinterpret_cast<API::FProperty *>(f);
                    if (!p->is_param())
                        continue;
                    if (p->is_return_param())
                        boolRet = PropType(p) == L"BoolProperty";
                    else if (PropType(p) == L"ObjectProperty" && !objs)
                    {
                        ++objs;
                        AiVisParam = p->get_fname()->to_string();
                    }
                    else if (p->is_out_param() || !(PropType(p) == L"FloatProperty" || PropType(p) == L"BoolProperty" ||
                                                     PropType(p) == L"IntProperty"))
                        ++others; // plain numbers are passed as 0 (CallRet zeroes the frame): TimeThreshold 0 = visible now
                }
                const bool ok = objs == 1 && others == 0 && boolRet;
                AiLog(L"visibility: %ls on %ls%ls", Signature(AiVisFn).c_str(), ClassName(ctl).c_str(),
                      ok ? L" - used as the game's line of sight" : L" - signature not usable, not called");
                if (!ok)
                    AiVisFn = nullptr;
            }
        }
        // First choice: the game's own list of what this AI sees right now
        // (MWAITargetVisibilityComponent.CurrentVisibleActors) - a plain read, no time allowance.
        if (API::UObject *tvc = ObjProp(ctl, L"TargetVisibilityComponent"))
        {
            static API::UClass *tvcCls = nullptr;
            static int32_t      cvaOff = -1;
            if (tvc->get_class() != tvcCls)
            {
                tvcCls = tvc->get_class();
                API::FProperty *p = FindProp(tvcCls, L"CurrentVisibleActors");
                cvaOff = (p && PropType(p) == L"ArrayProperty") ? p->get_offset() : -1;
                AiLog(cvaOff >= 0 ? L"visibility: CurrentVisibleActors read directly (the game's own visible list)"
                                  : L"[note] visibility: no CurrentVisibleActors array - IsActorVisible only");
            }
            if (cvaOff >= 0)
            {
                const auto *arr = reinterpret_cast<const TArr *>(reinterpret_cast<const uint8_t *>(tvc) + cvaOff);
                const int s = AiSlotOfCtl(ctl);
                std::vector<std::wstring> now;
                bool in = false;
                for (int32_t i = 0; arr->Data && i < arr->Num && i < 64; ++i)
                    if (auto *o = static_cast<API::UObject *const *>(arr->Data)[i])
                    {
                        in = in || o == tgt;
                        now.push_back(ObjTag(o));
                    }
                if (s)
                    AiDiffSet(s, L"sees", AiSt[s].Sees, now);
                if (tgt)
                    return in ? 1 : 0;
            }
        }
        if (!AiVisFn || !tgt)
            return -1;
        uint8_t v = 0;
        if (!CallRet(ctl, L"IsActorVisible", {{AiVisParam.c_str(), &tgt, sizeof(tgt)}}, &v, 1))
            return -1;
        return v ? 1 : 0;
    }

    static const wchar_t *AiCondName(int c)
    {
        static const wchar_t *k[] = {L"no LOS, no lock from you", L"no LOS, your lock on it", L"LOS, no lock from you",
                                     L"LOS, your lock on it"};
        return (c >= 0 && c < 4) ? k[c] : L"?";
    }

    void AiEpisodeEnd(int s, const char *why)
    {
        AiSlot &A = AiSt[s];
        if (!A.EpStart)
            return;
        const double secs = (GetTickCount64() - A.EpStart) / 1000.0;
        if (secs >= 10.0)
            AiLog(L"[test] TEST lrm-no-los: slot %d (%ls) vs %ls for %.0f s with NO line of sight (%ls, every 250 ms; "
                  L"%ls), your lock on it %ls: %d missile volley(s), %d round(s), %d weapon firing start(s); "
                  L"fire results seen:%ls; ended: %hs",
                  s, A.PawnName.c_str(), A.EpTargetTag.c_str(), secs,
                  L"the game's visible list / IsActorVisible", A.EpGameLos.c_str(),
                  A.EpLockAll ? L"the whole time" : (A.EpLockAny ? L"part of the time" : L"never"), A.EpVolleys, A.EpRounds,
                  A.EpFires, A.EpResults.empty() ? L" (none read)" : A.EpResults.c_str(), why);
        A.EpStart = 0;
    }

    void AiCondTick(int s, API::UObject *ctl, API::UObject *tgt, int gameLos, ULONGLONG now)
    {
        AiSlot &A = AiSt[s];
        const double dt = A.CondMs ? (now - A.CondMs) / 1000.0 : 0.0;
        A.CondMs = now;
        if (!tgt)
        {
            AiGameVisible(ctl, nullptr); // keeps the "sees" list current
            A.CurCond = -1;
            A.LosEngine = A.LosVisible = -1;
            A.LockNow = false;
            AiEpisodeEnd(s, "no combat target");
            return;
        }
        uint8_t eng = 0;
        const bool engOk = CallRet(ctl, L"LineOfSightTo", {{L"Other", &tgt, sizeof(tgt)}}, &eng, 1);
        A.LosEngine = engOk ? (eng ? 1 : 0) : -1;
        A.LosVisible = AiGameVisible(ctl, tgt);
        // Buckets and episodes use the game's own answer when there is one, the engine's otherwise.
        const bool losOk = A.LosVisible >= 0 || engOk;
        const uint8_t los = A.LosVisible >= 0 ? (uint8_t)A.LosVisible : eng;
        const bool lock = AiPlayerLock() == tgt;
        A.LockNow = lock;
        const int c = (los ? 2 : 0) + (lock ? 1 : 0);
        if (A.CurCond >= 0 && dt < 1.0)
            A.Conds[A.CurCond].Secs += dt;
        A.CurCond = losOk ? c : -1;
        // The no-line-of-sight episode.
        if (losOk && !los)
        {
            if (A.EpStart && A.EpTarget != tgt)
                AiEpisodeEnd(s, "target changed");
            if (!A.EpStart)
            {
                A.EpStart = now;
                A.EpTarget = tgt;
                A.EpTargetTag = ObjTag(tgt);
                A.EpVolleys = A.EpRounds = A.EpFires = 0;
                A.EpLockAll = true;
                A.EpLockAny = false;
                A.EpResults.clear();
                A.EpGameLos.clear();
            }
            A.EpLockAll = A.EpLockAll && lock;
            A.EpLockAny = A.EpLockAny || lock;
            const wchar_t *g = A.LosEngine < 0 ? L"engine unknown" : (A.LosEngine ? L"engine said YES" : L"engine agreed: no");
            if (A.EpGameLos.find(g) == std::wstring::npos)
                A.EpGameLos += (A.EpGameLos.empty() ? L"" : L" / ") + std::wstring(g);
            if (now - A.EpStart >= 30000)
                AiEpisodeEnd(s, "30 s window");
        }
        else if (A.EpStart)
            AiEpisodeEnd(s, losOk ? "line of sight gained" : "line of sight not readable");
    }

    void AiCondReport(int s)
    {
        AiSlot &A = AiSt[s];
        std::wstring t;
        for (int c = 0; c < 4; ++c)
        {
            const auto &k = A.Conds[c];
            if (k.Secs < 0.5)
                continue;
            wchar_t b[200];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L" | %ls: %.0f s, %d volley(s), %d round(s), %d firing start(s)", AiCondName(c), k.Secs, k.Volleys,
                       k.Rounds, k.FireStarts);
            t += b;
        }
        if (!t.empty())
            AiLog(L"[test] TEST firing-by-condition: slot %d (%ls) since the mission start (line of sight from %ls)%ls", s,
                  A.PawnName.c_str(), L"the game's visible list / IsActorVisible", t.c_str());
    }

    // Positions, once a second (with the blackboard round): the lancemate, its target, the pilot.
    void AiPositions(int s, API::UObject *pawn, API::UObject *tgt)
    {
        float p[3] = {}, r[3] = {}, me[3] = {}, t[3] = {};
        if (!CallRet(pawn, L"K2_GetActorLocation", {}, p, sizeof(p)))
            return;
        CallRet(pawn, L"K2_GetActorRotation", {}, r, sizeof(r));
        API::UObject *mePawn = API::get()->get_local_pawn(0);
        const bool haveMe = mePawn && CallRet(mePawn, L"K2_GetActorLocation", {}, me, sizeof(me));
        const bool haveT = tgt && CallRet(tgt, L"K2_GetActorLocation", {}, t, sizeof(t));
        auto d = [](const float *a, const float *b) {
            return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])) * 0.01f;
        };
        wchar_t tb[160] = L"no target";
        if (haveT)
            _snwprintf_s(tb, _countof(tb), _TRUNCATE, L"target %ls at (%.0f, %.0f, %.0f) m, %.0f m away, %+.0f m up/down", ObjTag(tgt).c_str(), t[0] * 0.01f,
                       t[1] * 0.01f, t[2] * 0.01f, d(p, t), (t[2] - p[2]) * 0.01f);
        wchar_t mb[96] = L"";
        if (haveMe)
            _snwprintf_s(mb, _countof(mb), _TRUNCATE, L" | you at (%.0f, %.0f, %.0f) m, %.0f m away", me[0] * 0.01f, me[1] * 0.01f, me[2] * 0.01f, d(p, me));
        AiLog(L"[1 Hz] slot %d  pos (%.0f, %.0f, %.0f) m facing %.0f | %ls%ls", s, p[0] * 0.01f, p[1] * 0.01f, p[2] * 0.01f,
              r[1], tb, mb);
    }

    // Every weapon component of the lancemate: firing start / stop and the game's fire result
    // against its combat target.
    void AiReadWeaponComps(int s, API::UObject *pawn, API::UObject *tgt)
    {
        AiSlot &A = AiSt[s];
        if (!A.WpnSearched)
        {
            A.WpnSearched = true;
            static API::UClass *acc = nullptr;
            if (!acc)
                acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
            std::vector<API::UObject *> comps;
            if (acc && ComponentsOn(pawn, acc, comps))
                for (auto *c : comps)
                    if (AiIsWeaponComp(c))
                    {
                        A.Wpns.push_back(c);
                        A.WpnNames.push_back(c->get_fname() ? c->get_fname()->to_string() : L"?");
                        A.WpnFiring.push_back(0xFF);
                    }
            std::wstring list;
            for (size_t i = 0; i < A.Wpns.size(); ++i)
                list += L" " + A.WpnNames[i] + L"(" + ClassName(A.Wpns[i]) + L")";
            AiLog(L"slot %d  weapon components: %d%ls", s, (int)A.Wpns.size(), list.c_str());
            if (A.Wpns.empty())
                AiNote(s, A.NoteWpnComp, L"no MWWeaponComponent found on the mech - no per-weapon firing / fire-result lines");
            else
            {
                FpDumpLayout(A.Wpns[0]->get_class(), "weapon component");
                AiPrepWeaponFns(A.Wpns[0]);
            }
        }
        // Only weapons still on the mech's fresh component list are touched (a destroyed arm's
        // weapon may be freed; a saved pointer to it must never be used).
        else
        {
            static API::UClass *acc2 = nullptr;
            if (!acc2)
                acc2 = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
            std::vector<API::UObject *> fresh;
            if (!acc2 || !ComponentsOn(pawn, acc2, fresh))
                return;
            for (size_t i = 0; i < A.Wpns.size(); ++i)
                if (A.Wpns[i] && std::find(fresh.begin(), fresh.end(), A.Wpns[i]) == fresh.end())
                {
                    AiLog(L"[4 Hz] slot %d  weapon component gone: %ls", s, A.WpnNames[i].c_str());
                    A.Wpns[i] = nullptr;
                }
        }
        for (size_t i = 0; i < A.Wpns.size(); ++i)
        {
            API::UObject *w = A.Wpns[i];
            if (!w)
                continue;
            if (AiIsFiringFn)
            {
                uint8_t f = 0;
                if (CallRet(w, AiIsFiringFn->get_fname()->to_string().c_str(), {}, &f, 1))
                {
                    if (A.WpnFiring[i] != 0xFF && f && !A.WpnFiring[i])
                    {
                        AiLog(L"[4 Hz] slot %d  %ls STARTED FIRING", s, A.WpnNames[i].c_str());
                        ++A.EpFires;
                        if (A.CurCond >= 0)
                            ++A.Conds[A.CurCond].FireStarts;
                    }
                    else if (A.WpnFiring[i] != 0xFF && !f && A.WpnFiring[i])
                        AiLog(L"[4 Hz] slot %d  %ls stopped firing", s, A.WpnNames[i].c_str());
                    A.WpnFiring[i] = f ? 1 : 0;
                }
            }
            if (AiFrOk && AiFrFn && tgt)
            {
                std::memset(AiFrBuf.data(), 0, AiFrBuf.size());
                for (int32_t off : AiFrObjParams)
                    std::memcpy(AiFrBuf.data() + off, &tgt, sizeof(tgt));
                w->process_event(AiFrFn, AiFrBuf.data());
                const unsigned v = AiFrBuf[AiFrRet];
                const std::wstring val = AiEnumVal(AiFrRetProp, v);
                AiSet(s, L"fire result " + A.WpnNames[i], val);
                if (A.EpStart && A.EpResults.find(A.WpnNames[i] + L"=" + val) == std::wstring::npos)
                    A.EpResults += L" " + A.WpnNames[i] + L"=" + val;
            }
        }
    }

    // ---- detection: the game's OWN answers (target tracking, team sharing, lock-on) ---------
    // From the executable's reflection names: every mech's MWTargetTrackingComponent keeps
    // ActorRelations - one ActorRelationInfo per unit it knows, with bInLineOfSight,
    // bInSensorRange, bRelayedFromTeam, bIsSelectedTarget, bInPlayerLance - plus
    // LastSubmittedSharedTargets (what it reported to the team) and a TargetSharingActor (the
    // team's shared pool). The weapon system's LockOnComponent holds the missile lock
    // (ELockOnState None / Acquiring / Losing / Locked). All read by name, layouts logged once.

    // Every member of one struct element as "name=value ...", skipping delegates and floats.
    std::wstring FpAll(API::UStruct *st, uint8_t *el)
    {
        std::wstring out;
        for (auto *s2 = st; s2; s2 = s2->get_super_struct())
            for (auto *f = s2->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring t = PropType(p);
                // Delegates say nothing; floats are mostly timestamps that would make every
                // element "change" every second.
                if (t.find(L"Delegate") != std::wstring::npos || t == L"FloatProperty" || t == L"DoubleProperty")
                    continue;
                out += (out.empty() ? L"" : L" ") + f->get_fname()->to_string() + L"=" +
                       (t == L"EnumProperty" ? AiEnumVal(p, *(el + p->get_offset())) : FpVal(p, el));
            }
        return out;
    }

    static int AiBoolField(API::UStruct *st, uint8_t *el, const wchar_t *name)
    {
        auto *p = StructProp(st, name);
        if (!p || PropType(p) != L"BoolProperty")
            return -1;
        return reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(el) ? 1 : 0;
    }

    // `pawn`'s own relationship record for `tgt`. With `summary`, also counts over all records.
    AiRel AiRelOf(API::UObject *pawn, API::UObject *tgt, std::wstring *summary = nullptr)
    {
        AiRel r;
        API::UObject *tt = pawn ? ObjProp(pawn, L"TargetTracking") : nullptr;
        if (!tt)
            return r;
        uint8_t *data = nullptr;
        int32_t num = 0, stride = 0;
        API::UScriptStruct *es = nullptr;
        std::wstring an;
        if (!FpFindArray(tt->get_class(), reinterpret_cast<uint8_t *>(tt), L"ActorRelationInfo", nullptr, data, num, es,
                         stride, an))
        {
            static bool noted = false;
            if (!noted)
            {
                noted = true;
                AiLog(L"[note] detection: no ActorRelationInfo array on %ls - no game line-of-sight / sensor flags",
                      ClassName(tt).c_str());
            }
            return r;
        }
        FpDumpLayout(es, "actor relation");
        auto *ta = StructProp(es, L"TargetActor");
        if (!ta || PropType(ta) != L"ObjectProperty")
            return r;
        int n = 0, nLos = 0, nSen = 0, nRel = 0;
        for (int32_t i = 0; data && i < num && i < 256; ++i)
        {
            uint8_t *el = data + (size_t)i * stride;
            API::UObject *a = *reinterpret_cast<API::UObject **>(el + ta->get_offset());
            if (!a)
                continue;
            ++n;
            const int los = AiBoolField(es, el, L"bInLineOfSight"), sen = AiBoolField(es, el, L"bInSensorRange"),
                      rel = AiBoolField(es, el, L"bRelayedFromTeam");
            nLos += los == 1;
            nSen += sen == 1;
            nRel += rel == 1;
            if (a == tgt)
            {
                r.Known = 1;
                r.Los = los;
                r.Sensor = sen;
                r.Relay = rel;
                r.Sel = AiBoolField(es, el, L"bIsSelectedTarget");
            }
        }
        if (summary)
        {
            wchar_t b[128];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d known: %d in line of sight, %d in sensor range, %d relayed from team", n, nLos, nSen, nRel);
            *summary = b;
        }
        return r;
    }

    static std::wstring AiRelText(const AiRel &r)
    {
        if (!r.Known)
            return L"not in its relations";
        auto yn = [](int v) { return v < 0 ? L"?" : (v ? L"yes" : L"no"); };
        wchar_t b[160];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"line of sight %ls, in sensor range %ls, relayed from team %ls, selected %ls", yn(r.Los), yn(r.Sensor),
                   yn(r.Relay), yn(r.Sel));
        return b;
    }

    // Every struct-array on an object, element by element (change-only, keyed by array name).
    void AiStructArrays(int s, API::UObject *o, const wchar_t *label, std::vector<std::wstring> &was)
    {
        std::vector<std::wstring> now;
        if (o && o->get_class())
            for (auto *s2 = static_cast<API::UStruct *>(o->get_class()); s2; s2 = s2->get_super_struct())
                for (auto *f = s2->get_child_properties(); f; f = f->get_next())
                {
                    auto *p = reinterpret_cast<API::FProperty *>(f);
                    if (PropType(p) != L"ArrayProperty")
                        continue;
                    auto *in = reinterpret_cast<API::FArrayProperty *>(p)->get_inner();
                    if (!in)
                        continue;
                    const std::wstring an = f->get_fname()->to_string();
                    if (s > 0 && an.find(L"Submitted") == std::wstring::npos && an.find(L"Tagged") == std::wstring::npos)
                        continue; // on a lancemate's tracker: only what it reports
                    uint8_t *at = reinterpret_cast<uint8_t *>(o) + p->get_offset();
                    uint8_t *data = *reinterpret_cast<uint8_t **>(at);
                    const int32_t num = *reinterpret_cast<int32_t *>(at + sizeof(void *));
                    if (PropType(in) == L"StructProperty")
                    {
                        auto *ss = reinterpret_cast<API::FStructProperty *>(in)->get_struct();
                        const int32_t stride = ss ? ss->get_struct_size() : 0;
                        if (!ss || stride <= 0)
                            continue;
                        FpDumpLayout(ss, "shared-target element");
                        for (int32_t i = 0; data && i < num && i < 32; ++i)
                            now.push_back(an + L": " + FpAll(ss, data + (size_t)i * stride));
                    }
                    else if (PropType(in) == L"ObjectProperty")
                        for (int32_t i = 0; data && i < num && i < 32; ++i)
                            now.push_back(an + L": " + ObjTag(reinterpret_cast<API::UObject **>(data)[i]));
                }
        for (const auto &n : now)
            if (std::find(was.begin(), was.end(), n) == was.end())
                AiLog(L"%ls %ls + %ls", AiCurTag, label, n.c_str());
        for (const auto &o2 : was)
            if (std::find(now.begin(), now.end(), o2) == now.end())
                AiLog(L"%ls %ls - %ls", AiCurTag, label, o2.c_str());
        was = now;
    }

    std::vector<std::wstring> AiTeamShared;

    void                     *AiTeamSharer = nullptr;

    // Per lancemate, 4 Hz: its flags for its combat target, yours for the same target, its
    // missile lock, and its (and your) detection status.
    void AiReadDetection(int s, API::UObject *pawn, API::UObject *tgt, bool slow)
    {
        AiSlot &A = AiSt[s];
        std::wstring sum;
        const AiRel r = AiRelOf(pawn, tgt, slow ? &sum : nullptr);
        A.GRel = r;
        if (tgt)
            AiSet(s, L"game flags (its combat target)", AiRelText(r));
        API::UObject *me = API::get()->get_local_pawn(0);
        const AiRel y = AiRelOf(me, tgt);
        A.YRel = y;
        if (tgt)
            AiSet(s, L"YOUR flags (its combat target)", AiRelText(y));
        if (slow && !sum.empty())
            AiSet(s, L"relations", sum);

        // Missile lock: the weapon system's lock-on component, every object / enum / bool member.
        if (API::UObject *ws = ObjProp(pawn, L"WeaponSystemComponent"))
            if (API::UObject *lc = ObjProp(ws, L"LockOnComponent"))
            {
                FpDumpLayout(lc->get_class(), "lock-on component");
                for (auto *s2 = static_cast<API::UStruct *>(lc->get_class()); s2; s2 = s2->get_super_struct())
                {
                    if (s2->get_fname()->to_string() == L"ActorComponent")
                        break;
                    for (auto *f = s2->get_child_properties(); f; f = f->get_next())
                    {
                        auto *p = reinterpret_cast<API::FProperty *>(f);
                        const std::wstring t = PropType(p);
                        uint8_t *b = reinterpret_cast<uint8_t *>(lc);
                        if (t == L"ObjectProperty" || t == L"BoolProperty")
                            AiSet(s, L"lock." + f->get_fname()->to_string(), FpVal(p, b));
                        else if (t == L"EnumProperty" || t == L"ByteProperty")
                            AiSet(s, L"lock." + f->get_fname()->to_string(), AiEnumVal(p, *(b + p->get_offset())));
                    }
                }
            }

        if (auto *p = FindProp(pawn->get_class(), L"DetectionStatus"))
            AiSet(s, L"detection status", AiEnumVal(p, *(reinterpret_cast<uint8_t *>(pawn) + p->get_offset())));

        // What it reports to the team, and (once a second, from one tracker) the team's pool.
        if (slow)
            if (API::UObject *tt = ObjProp(pawn, L"TargetTracking"))
            {
                AiStructArrays(s, tt, (L"slot " + std::to_wstring(s) + L"  reports").c_str(), A.Reports);
                if (API::UObject *sa = ObjProp(tt, L"TargetSharingActor"))
                {
                    if (sa != AiTeamSharer)
                    {
                        AiTeamSharer = sa;
                        FpDumpLayout(sa->get_class(), "target sharing actor");
                        AiLog(L"detection: slot %d shares through %ls", s, ObjTag(sa).c_str());
                    }
                    AiStructArrays(0, sa, L"TEAM SHARED", AiTeamShared);
                }
            }
    }

    // ---- a full snapshot of a struct / object, nested (the weapon controller's states) ------
    // Floats included (these are read at a transition, not diffed), nested structs one level
    // deep, arrays of structs up to 6 elements, weak object pointers resolved, maps counted.
    static API::UObject *WeakGet(const uint8_t *at)
    {
        const int32_t idx = *reinterpret_cast<const int32_t *>(at);
        if (idx <= 0)
            return nullptr;
        auto *arr = API::FUObjectArray::get();
        return arr ? arr->get_object(idx) : nullptr;
    }

    std::wstring FpDeep(API::UStruct *st, uint8_t *el, int depth, API::UStruct *stopAt = nullptr)
    {
        std::wstring out;
        for (auto *s2 = st; s2 && s2 != stopAt; s2 = s2->get_super_struct())
        {
            if (s2->get_fname()->to_string() == L"ActorComponent" || s2->get_fname()->to_string() == L"Object")
                break;
            for (auto *f = s2->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring t = PropType(p);
                if (t.find(L"Delegate") != std::wstring::npos)
                    continue;
                uint8_t *at = el + p->get_offset();
                std::wstring v;
                wchar_t b[64];
                if (t == L"FloatProperty")
                {
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%.2f", *reinterpret_cast<float *>(at));
                    v = b;
                }
                else if (t == L"EnumProperty")
                    v = AiEnumVal(p, *at);
                else if (t == L"WeakObjectProperty")
                    v = ObjTag(WeakGet(at));
                else if (t == L"MapProperty" || t == L"SetProperty")
                {
                    // FScriptMap / FScriptSet: TSparseArray { TArray Data; TBitArray (32 bytes);
                    // int FirstFreeIndex; int NumFreeIndices } - live elements = Data.Num - NumFree.
                    const int32_t num = *reinterpret_cast<int32_t *>(at + 8) - *reinterpret_cast<int32_t *>(at + 16 + 32 + 4);
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L"(%d entr%ls)", num, num == 1 ? L"y" : L"ies");
                    v = b;
                }
                else if (t == L"StructProperty" && depth > 0)
                {
                    auto *ss = reinterpret_cast<API::FStructProperty *>(p)->get_struct();
                    const std::wstring sn = ss ? ss->get_fname()->to_string() : L"";
                    if (sn == L"Vector" || sn == L"Rotator")
                    {
                        const float *x = reinterpret_cast<float *>(at);
                        _snwprintf_s(b, _countof(b), _TRUNCATE, L"(%.0f, %.0f, %.0f)", x[0], x[1], x[2]);
                        v = b;
                    }
                    else
                        v = ss ? L"{" + FpDeep(ss, at, depth - 1) + L"}" : L"{?}";
                }
                else if (t == L"ArrayProperty")
                {
                    auto *in = reinterpret_cast<API::FArrayProperty *>(p)->get_inner();
                    uint8_t *data = *reinterpret_cast<uint8_t **>(at);
                    const int32_t num = *reinterpret_cast<int32_t *>(at + sizeof(void *));
                    v = L"[" + std::to_wstring(num);
                    const std::wstring it = in ? PropType(in) : L"";
                    if (data && num > 0 && it == L"StructProperty" && depth > 0)
                    {
                        auto *ss = reinterpret_cast<API::FStructProperty *>(in)->get_struct();
                        const int32_t stride = ss ? ss->get_struct_size() : 0;
                        for (int32_t i = 0; ss && stride > 0 && i < num && i < 6; ++i)
                            v += L" {" + FpDeep(ss, data + (size_t)i * stride, depth - 1) + L"}";
                    }
                    else if (data && num > 0 && it == L"ObjectProperty")
                        for (int32_t i = 0; i < num && i < 6; ++i)
                            v += L" " + ObjTag(reinterpret_cast<API::UObject **>(data)[i]);
                    else if (data && num > 0 && it == L"WeakObjectProperty")
                        for (int32_t i = 0; i < num && i < 6; ++i)
                            v += L" " + ObjTag(WeakGet(data + (size_t)i * 8));
                    v += L"]";
                }
                else
                    v = FpVal(p, el);
                out += (out.empty() ? L"" : L" ") + f->get_fname()->to_string() + L"=" + v;
            }
        }
        return out;
    }

    // The weapon controller's state machine (EAIWeaponControlState Idle / Wait / Plan / Track /
    // Firing): at every change, the contents of the state it LEFT (their final values say why it
    // left) and of the one it entered. BehaviourConfig and its asset once per lancemate.
    // The weapon controller's console settings (names and descriptions found in the game's
    // executable), read once through UEVR's console manager - read only. Their descriptions:
    // AbortTrackingTime "how long to go without successfully tracking a target before aborting and
    // re-planning", MaxHeatUsagePercent "how close to maximum heat the AI is willing to play",
    // FriendlyCapsuleMultiplier "how much to expand a friendly unit's capsule by when performing a
    // possible friendly fire hit test", and so on.
    bool AiCvarsLogged = false;

    void AiLogCvars()
    {
        if (AiCvarsLogged)
            return;
        AiCvarsLogged = true;
        auto *cm = API::get()->get_console_manager();
        if (!cm)
        {
            AiLog(L"[note] console settings: no console manager - weapon controller settings not read");
            return;
        }
        static const wchar_t *kNames[] = {
            L"MW.AI.FireWeapons",
            L"MW.AI.WeaponControlSystem.AimHintRadius",
            L"MW.AI.WeaponControlSystem.AbortTrackingTime",
            L"MW.AI.WeaponControlSystem.AbortTrackingTimeInAir",
            L"MW.AI.WeaponControlSystem.AbortTrackingCooldownTime",
            L"MW.AI.WeaponControlSystem.DelayBetweenShots",
            L"MW.AI.WeaponControlSystem.MaxHeatUsagePercent",
            L"MW.AI.WeaponControlSystem.AimMissRadiusExpansion",
            L"MW.AI.WeaponControlSystem.AimMissHeightExpansion",
            L"MW.AI.WeaponControlSystem.AimMissRandomConeAngleDegrees",
            L"MW.AI.WeaponControlSystem.ContinuousFireMinTime",
            L"MW.AI.WeaponControlSystem.ContinuousFireMaxTime",
            L"MW.AI.WeaponControlSystem.FireThroughDestructibleCheckTime",
            L"MW.AI.WeaponControlSystem.FiringDelay",
            L"MW.AI.WeaponControlSystem.ExtremeWeatherMissModifier",
            L"MW.AI.WeaponControlSystem.ForceExtremeWeather",
            L"MW.AI.WeaponControlSystem.PostFiringDelay",
            L"MW.AI.WeaponControlSystem.FriendlyCapsuleMultiplier",
            L"MW.AI.WeaponControlSystem.EnemyAccuracyMultipler",
            L"MW.AI.WeaponControlSystem.IgnoreTargetHealthWhenFiring",
            L"MW.AI.WeaponControlSystem.MaxRapidFireTime",
            L"MW.AI.WeaponControlSystem.RapidFireTimeDelay",
            L"MW.AI.WeaponControlSystem.RapidFireCooldownThreshold",
            L"MW.AI.WeaponControlSystem.MaxInterruptibleCooldownFireAttempts",
            L"MW.AI.WeaponControlSystem.MaxRapidFireAttempts",
            L"MW.AI.Context.CombatTarget.LeadTime",
            L"MW.OptimalRange.DefaultMetres",
            L"MW.OptimalRange.IntervalMetres",
            L"MW.OptimalRange.MaxSampleRangeMetres",
            L"MW.OptimalRange.RangeBoundaryPadding",
            L"MW.PerceptionAimTarget.PingRangeMetres",
            L"MW.AISense.CallForHelpRange",
            L"MW.AISense.CallForHelpCooldown",
        };
        AiExempt = true;
        for (const wchar_t *n : kNames)
        {
            auto *v = cm->find_variable(n);
            if (!v)
                AiLog(L"console setting %ls: NOT FOUND", n);
            else
                AiLog(L"console setting %ls = %.3f (as int %d)", n, v->get_float(), v->get_int());
        }
        AiExempt = false;
    }

    void AiWcsTransition(int s, API::UStruct *wst, uint8_t *wbase)
    {
        AiLogCvars();
        AiSlot &A = AiSt[s];
        API::FProperty *cp = StructProp(wst, L"ControlState");
        if (!cp)
            return;
        const int now = *(wbase + cp->get_offset());
        if (!A.WcsCfgLogged)
        {
            A.WcsCfgLogged = true;
            AiExempt = true;
            if (auto *bp = StructProp(wst, L"BehaviourConfig"))
                if (PropType(bp) == L"StructProperty")
                    AiLog(L"slot %d  weapon control BehaviourConfig: %ls", s,
                          FpDeep(reinterpret_cast<API::FStructProperty *>(bp)->get_struct(), wbase + bp->get_offset(), 2).c_str());
            if (auto *ap = StructProp(wst, L"BehaviourConfigAsset"))
                if (PropType(ap) == L"ObjectProperty")
                    if (API::UObject *asset = *reinterpret_cast<API::UObject **>(wbase + ap->get_offset()))
                        AiLog(L"slot %d  weapon control BehaviourConfigAsset %ls: %ls", s, ObjTag(asset).c_str(),
                              FpDeep(asset->get_class(), reinterpret_cast<uint8_t *>(asset), 2).c_str());
            for (const wchar_t *n : {L"DelayBetweenShots", L"bEnableWeaponControlDebugging", L"CoopAccuracyAdjustment",
                                     L"CoopLethalityAdjustment", L"TotalWeaponFireCount", L"bHasTAG"})
                if (auto *p = StructProp(wst, n))
                    AiLog(L"slot %d  weapon control %ls = %ls", s, n, FpVal(p, wbase).c_str());
            AiExempt = false;
        }
        if (now == A.WcsState)
            return;
        static const wchar_t *kStateMember[5] = {nullptr, L"WaitState", L"PlanState", L"TrackState", L"FiringState"};
        auto snap = [&](int st) -> std::wstring {
            if (st < 1 || st > 4)
                return L"";
            auto *p = StructProp(wst, kStateMember[st]);
            if (!p || PropType(p) != L"StructProperty")
                return L"";
            return FpDeep(reinterpret_cast<API::FStructProperty *>(p)->get_struct(), wbase + p->get_offset(), 2);
        };
        const std::wstring from = A.WcsState >= 0 ? AiEnumVal(cp, (unsigned)A.WcsState) : L"-";
        const std::wstring to = AiEnumVal(cp, (unsigned)now);
        const std::wstring left = snap(A.WcsState), entered = snap(now);
        AiLog(L"[4 Hz] slot %d  WEAPON CONTROL %ls -> %ls | left: {%ls} | entered: {%ls}", s, from.c_str(), to.c_str(),
              left.c_str(), entered.c_str());
        A.WcsState = now;
    }

    // Everything cheap (no blackboard, no perception): controller, movement, weapons, targets, heat.
    void AiReadFast(int s, API::UObject *pawn, API::UObject *ctl)
    {
        AiSlot &A = AiSt[s];
        auto *cb = reinterpret_cast<uint8_t *>(ctl);
        auto *cc = ctl->get_class();
        FpDumpLayout(cc, "controller");
        wchar_t b[96];

        // The controller.
        for (const wchar_t *n : {L"CombatTarget", L"PrimaryCombatTactic", L"CombatTactic", L"CombatTargetInteraction", L"bSuppressFiring",
                                 L"bIsIdle", L"SpeedLimitKph", L"ActiveLookFocus"})
            if (auto *p = FindProp(cc, n))
                AiSet(s, std::wstring(L"ctl.") + n,
                      PropType(p) == L"EnumProperty" ? AiEnumVal(p, *(cb + p->get_offset())) : FpVal(p, cb));
        uint8_t tac = 0xFF;
        if (API::UFunction *tf = FindFn(ctl, L"K2_GetCombatTactic"))
            if (CallRet(ctl, L"K2_GetCombatTactic", {}, &tac, 1))
                AiSet(s, L"ctl.tactic", AiEnumVal(tf->find_property(L"ReturnValue"), tac));
        if (auto *p = FindProp(cc, L"LastTimeDamageTaken"))
            if (PropType(p) == L"FloatProperty")
            {
                const float t = *reinterpret_cast<float *>(cb + p->get_offset());
                if (A.DmgInit && t > A.Dmg + 0.001f)
                    AiLog(L"%ls slot %d  TOOK DAMAGE (seen since the last sample)", AiCurTag, s);
                A.Dmg = t;
                A.DmgInit = true;
            }

        // Movement.
        uint8_t ms = 0xFF;
        if (CallRet(ctl, L"GetMoveStatus", {}, &ms, 1))
        {
            static const wchar_t *kMs[] = {L"Idle", L"Waiting", L"Paused", L"Moving"};
            AiSet(s, L"move.status", ms < 4 ? kMs[ms] : L"?");
        }
        float dst[3] = {};
        if (CallRet(ctl, L"GetImmediateMoveDestination", {}, dst, sizeof(dst)))
            AiSet(s, L"move.to", AiVec(dst));
        float vel[3] = {};
        if (CallRet(pawn, L"GetVelocity", {}, vel, sizeof(vel)))
        {
            const float kph = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1]) * 0.036f;
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d kph", (int)std::round(kph / 5.0f) * 5);
            AiSet(s, L"move.speed", b);
        }

        // Weapon control.
        API::UStruct *wst = nullptr;
        uint8_t *wbase = nullptr;
        if (auto *wp = FindProp(cc, L"WeaponControlSystem"))
        {
            const std::wstring wt = PropType(wp);
            if (wt == L"ObjectProperty")
            {
                auto *wo = *reinterpret_cast<API::UObject **>(cb + wp->get_offset());
                if (wo && wo->get_class())
                {
                    wst = wo->get_class();
                    wbase = reinterpret_cast<uint8_t *>(wo);
                }
            }
            else if (wt == L"StructProperty")
            {
                wst = reinterpret_cast<API::FStructProperty *>(wp)->get_struct();
                wbase = cb + wp->get_offset();
            }
        }
        if (!wst)
            AiNote(s, A.NoteWcs, L"no readable WeaponControlSystem on the controller - no firing-state (wcs.) lines");
        if (wst)
        {
            FpDumpLayout(wst, "weapon control");
            for (const wchar_t *n : {L"FireTarget", L"bSuppressFiring"})
                if (auto *p = StructProp(wst, n))
                    AiSet(s, std::wstring(L"wcs.") + n, FpVal(p, wbase));
            AiWcsTransition(s, wst, wbase); // the state machine, with its states' contents
        }
        uint8_t *data = nullptr;
        int32_t num = 0, stride = 0;
        API::UScriptStruct *es = nullptr;
        std::wstring an;
        const bool found = (wst && FpFindArray(wst, wbase, L"AIWeaponDescriptor", nullptr, data, num, es, stride, an)) ||
                           FpFindArray(cc, cb, L"AIWeaponDescriptor", nullptr, data, num, es, stride, an) ||
                           FpFindArray(pawn->get_class(), reinterpret_cast<uint8_t *>(pawn), L"AIWeaponDescriptor", nullptr,
                                       data, num, es, stride, an);
        if (!found || !data)
            AiNote(s, A.NoteWpn, L"no AIWeaponDescriptor array found (weapon control, controller, mech) - no loadout / wpn / FIRED lines");
        if (found && data)
        {
            FpDumpLayout(es, "weapon record");
            if (!A.LoadoutLogged || (int)A.WpnTime.size() != num)
            {
                A.LoadoutLogged = true;
                A.WpnTime.assign((size_t)(num > 0 ? num : 0), -1.0f);
                for (int32_t i = 0; i < num && i < 32; ++i)
                    AiLog(L"slot %d  loadout %ls[%d]%ls", s, an.c_str(), i,
                          FpFields(es, data + (size_t)i * stride,
                                   {L"bIsLRM", L"bIsHoming", L"bIsTAG", L"bIsNARC", L"bIsMelee", L"bIsAMS", L"bContinuous",
                                    L"MinimumRange", L"OptimalRange", L"MaximumRange", L"Cooldown", L"Damage"})
                              .c_str());
            }
            auto *pCan = StructProp(es, L"bCanFire");
            auto *pAmmo = StructProp(es, L"bIsAmmoDepleted");
            auto *pDest = StructProp(es, L"bDestroyed");
            auto *pT = StructProp(es, L"TimeUntilFire");
            for (int32_t i = 0; i < num && i < 32; ++i)
            {
                uint8_t *el = data + (size_t)i * stride;
                _snwprintf_s(b, _countof(b), _TRUNCATE, L"wpn[%d]", i);
                std::wstring st;
                if (pCan) st += L"canFire=" + FpVal(pCan, el);
                if (pAmmo) st += L" noAmmo=" + FpVal(pAmmo, el);
                if (pDest) st += L" destroyed=" + FpVal(pDest, el);
                AiSet(s, b, st);
                if (pT && PropType(pT) == L"FloatProperty")
                {
                    const float t = *reinterpret_cast<float *>(el + pT->get_offset());
                    if ((size_t)i < A.WpnTime.size())
                    {
                        if (A.WpnTime[i] >= 0.0f && t > A.WpnTime[i] + 0.05f)
                            AiLog(L"%ls slot %d  wpn[%d] FIRED (seen since the last sample; time until ready %.2f)", AiCurTag, s, i, t);
                        A.WpnTime[i] = t;
                    }
                }
            }
        }

        // Target records.
        if (API::UObject *tsc = ObjProp(ctl, L"TargetSelectionComponent"))
        {
            FpDumpLayout(tsc->get_class(), "target selection");
            uint8_t *td = nullptr;
            int32_t tn = 0, ts = 0;
            API::UScriptStruct *tes = nullptr;
            std::wstring tan;
            if (FpFindArray(tsc->get_class(), reinterpret_cast<uint8_t *>(tsc), nullptr, L"bInLineOfSight", td, tn, tes, ts, tan))
            {
                FpDumpLayout(tes, "target record");
                auto *ta = StructProp(tes, L"TargetActor");
                auto *tl = StructProp(tes, L"bInLineOfSight");
                API::UObject *cur = ObjProp(ctl, L"CombatTarget");
                A.GameLos = -1;
                std::vector<std::wstring> now;
                for (int32_t i = 0; td && i < tn && i < 48; ++i)
                {
                    uint8_t *el = td + (size_t)i * ts;
                    const std::wstring who = ta ? FpVal(ta, el) : L"?";
                    if (cur && ta && tl && PropType(ta) == L"ObjectProperty" &&
                        *reinterpret_cast<API::UObject **>(el + ta->get_offset()) == cur && PropType(tl) == L"BoolProperty")
                        A.GameLos = reinterpret_cast<API::FBoolProperty *>(tl)->get_value_from_object(el) ? 1 : 0;
                    now.push_back(who);
                    AiSet(s, L"target " + who,
                          FpFields(tes, el, {L"bInLineOfSight", L"bInSensorRange", L"bIsSelectedTarget", L"bRelayedFromTeam"}));
                }
                AiDiffSet(s, L"target record", A.Targets, now);
            }
            else
                AiNote(s, A.NoteTgt, L"no target-record array with bInLineOfSight on the target selection component - no target lines");
            // RECENT DAMAGE (lancecontrol-7 probe, read only): the AI's own record of who hurt it
            // last - RecentDamageTarget (MWTimeSensitiveTarget). Layout logged once, members logged
            // as they change. If it holds the attacker, it is the exact source for callouts like
            // "that Atlas is ripping me apart" (lasers included - it is not projectile based).
            if (auto *rp = FindProp(tsc->get_class(), L"RecentDamageTarget"))
                if (PropType(rp) == L"StructProperty")
                    if (auto *rs = reinterpret_cast<API::FStructProperty *>(rp)->get_struct())
                    {
                        FpDumpLayout(rs, "recent damage target");
                        uint8_t *rb = reinterpret_cast<uint8_t *>(tsc) + rp->get_offset();
                        std::wstring v;
                        for (auto *s2 = static_cast<API::UStruct *>(rs); s2; s2 = s2->get_super_struct())
                            for (auto *f = s2->get_child_properties(); f; f = f->get_next())
                            {
                                auto *q = reinterpret_cast<API::FProperty *>(f);
                                std::wstring val;
                                if (PropType(q) == L"WeakObjectProperty")
                                {
                                    const int32_t idx = *reinterpret_cast<int32_t *>(rb + q->get_offset());
                                    auto *arr = API::get()->get_uobject_array();
                                    val = ObjTag((arr && idx > 0) ? arr->get_object(idx) : nullptr);
                                }
                                else
                                    val = FpVal(q, rb);
                                v += L" " + f->get_fname()->to_string() + L"=" + val;
                            }
                        AiSet(s, L"recent damage", v);
                    }
        }
        else
            AiNote(s, A.NoteTgt, L"no TargetSelectionComponent on the controller - no target lines");

        // Heat, found once per mech among its components.
        if (!A.HeatSearched)
        {
            A.HeatSearched = true;
            static API::UClass *acc = nullptr;
            if (!acc)
                acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
            std::vector<API::UObject *> comps;
            if (acc && ComponentsOn(pawn, acc, comps))
                for (auto *c : comps)
                    if (c && ClassName(c).find(L"Heat") != std::wstring::npos)
                    {
                        A.Heat = c;
                        FpDumpLayout(c->get_class(), "heat");
                        break;
                    }
            if (!A.Heat)
                AiNote(s, A.NoteHeat, L"no heat component found - no heat lines");
        }
        if (A.Heat && A.Heat->get_class() && !FindProp(A.Heat->get_class(), L"HeatPercentage"))
        {
            // MWHeatComponent (the layout the probe logged): CurrentHeat / MaxHeatCapacity.
            auto *c = FindProp(A.Heat->get_class(), L"CurrentHeat");
            auto *m = FindProp(A.Heat->get_class(), L"MaxHeatCapacity");
            if (c && m && PropType(c) == L"FloatProperty" && PropType(m) == L"FloatProperty")
            {
                const uint8_t *hb = reinterpret_cast<uint8_t *>(A.Heat);
                const float cur = *reinterpret_cast<const float *>(hb + c->get_offset());
                const float mx = *reinterpret_cast<const float *>(hb + m->get_offset());
                if (mx > 0.0f)
                {
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d %%", (int)std::round(cur / mx * 10.0f) * 10);
                    AiSet(s, L"heat", b);
                }
            }
        }
        if (A.Heat && A.Heat->get_class())
            if (auto *p = FindProp(A.Heat->get_class(), L"HeatPercentage"))
                if (PropType(p) == L"FloatProperty")
                {
                    float h = *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(A.Heat) + p->get_offset());
                    if (h <= 1.5f)
                        h *= 100.0f; // a fraction or a percentage, whichever it is
                    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d %%", (int)std::round(h / 10.0f) * 10);
                    AiSet(s, L"heat", b);
                }
    }

    // =====================================================================================
    // LANCE STATUS FEED + EVENT LIST (vanillahud-94, include/LanceStatusShared.h, MW5_LanceStatus).
    // Read-only towards the game: everything here is values the plugin reads anyway. 4 Hz.
    // =====================================================================================
    HANDLE            LsMap = nullptr;

    LanceStatusBlock *Ls = nullptr;

    ULONGLONG         LsMs = 0;

    ULONGLONG         LsRetryMs = 0;    // lancecontrol-5: next try at opening the block (0 = now)
    bool              LsFailLogged = false;

    // per slot, for the events
    bool          LsPrevAlive[VanillaExportBlock::kTacMaxMembers + 1] = {};

    API::UObject *LsPrevUnit[VanillaExportBlock::kTacMaxMembers + 1] = {};

    API::UObject *LsPrevTgt[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int           LsPrevSeen[VanillaExportBlock::kTacMaxMembers + 1] = {};

    bool          LsHot[VanillaExportBlock::kTacMaxMembers + 1] = {};

    float         LsDmgT[VanillaExportBlock::kTacMaxMembers + 1] = {};

    bool          LsDmgInit[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG     LsDmgMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    API::UObject *LsFireTgt[VanillaExportBlock::kTacMaxMembers + 1] = {};

    ULONGLONG     LsFireTgtMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    struct LsWpnPrev
    {
        API::UObject *W;
        uint8_t       State;
        ULONGLONG     FiredMs;
        bool          Low = false;
    };

    std::vector<LsWpnPrev> LsWpns[VanillaExportBlock::kTacMaxMembers + 1];

    API::UObject *LsPrevLock = nullptr;

    // WHO HIT WHOM (lancecontrol-8). Every AI unit's target selection keeps RecentDamageTarget
    // {Target, LastUpdateTime}: the unit that last damaged it (the probe, lancecontrol-7b: it
    // switches to the latest attacker, credits the player, clears when the attacker dies or after
    // ~40-60 s; mechs, vehicles, turrets and VTOLs all have it - the player's mech has no AI).
    API::UObject *RdtTarget(API::UObject *ctl)
    {
        API::UObject *tsc = ctl ? ObjProp(ctl, L"TargetSelectionComponent") : nullptr;
        API::FProperty *rp = tsc ? FindProp(tsc->get_class(), L"RecentDamageTarget") : nullptr;
        if (!rp || PropType(rp) != L"StructProperty")
            return nullptr;
        auto *rs = reinterpret_cast<API::FStructProperty *>(rp)->get_struct();
        API::FProperty *tp = rs ? StructProp(rs, L"Target") : nullptr;
        if (!tp || PropType(tp) != L"ObjectProperty")
            return nullptr;
        API::UObject *t = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(tsc) + rp->get_offset() + tp->get_offset());
        return (t && IsLive(t)) ? t : nullptr;
    }

    // Enemies, twice a second: the player hitting one (LSE_PlayerHit) and one turning on the
    // player (LSE_EnemyOnPlayer) - VoiceCommand's "good hits, commander" / "that Atlas is coming
    // for you". Edges only: an enemy already on the player says nothing more.
    struct LsEnemySeen
    {
        API::UObject *Rdt, *Tgt;
    };
    std::map<API::UObject *, LsEnemySeen> LsEnemyPrev;
    ULONGLONG LsEnemyMs = 0;
    // FLANKED (lancecontrol-21): an enemy close behind a lancemate - the candidate seen last tick (it must be
    // there two ticks running, a second) and when each lancemate last said so.
    API::UObject *LsFlankCand[9] = {};
    ULONGLONG     LsFlankMs[9] = {};

    ULONGLONG LsSumMs = 0;

    // =====================================================================================
    // WHO IS FIRING AT YOU (lancecontrol-14, lance status v5). Ten times a second every MWProjectile
    // round's position is read - two plain memory reads (RootComponent -> RelativeLocation, the way
    // LidarExport does it: no ProcessEvent per round, which is what once froze the game under LRM
    // salvos). A round also seen on the last look has a velocity; one whose straight line passes within
    // 40 m of the player in the next 3 s is coming at the player, and is traced ONCE to the unit that
    // fired it - the round's Instigator (LidarExport: "resolves to the firing mech"). Per shooter: rounds
    // by kind in its current spell of fire (reset after 10 s quiet). A pooled round that jumps (reused)
    // starts afresh. Lasers are beams, not rounds: they are never seen here.
    // =====================================================================================
    struct ShotRound
    {
        float     X, Y, Z;
        ULONGLONG Ms;
        bool      Counted;
    };
    struct ShotRec
    {
        uint32_t  Lo, Hi;
        ULONGLONG Last;
        uint8_t   N[LST_Count];
    };
    std::unordered_map<API::UObject *, ShotRound> ShotPrev; // compared only
    std::map<API::UObject *, ShotRec>             ShotBy;   // compared only - IsLive before any read
    std::unordered_map<const void *, uint8_t>     ShotTypeCache;
    API::UClass *ShotProjClass = nullptr;
    bool         ShotNoClass = false;
    int32_t      ShotRootOff = -1, ShotLocOff = -1, ShotInstOff = -1;
    ULONGLONG    ShotMs = 0, ShotLogMs = 0;
    int          ShotTraced = 0, ShotUntraced = 0;

    uint8_t ShotTypeOf(API::UObject *r)
    {
        const void *ck = r->get_class();
        auto it = ShotTypeCache.find(ck);
        if (it != ShotTypeCache.end())
            return it->second;
        std::wstring cn = r->get_class()->get_fname()->to_string();
        for (auto &ch : cn)
            ch = (wchar_t)towlower(ch);
        auto has = [&](const wchar_t *k) { return cn.find(k) != std::wstring::npos; };
        uint8_t t = LST_Ballistic;
        if (has(L"ppc") || has(L"particle"))
            t = LST_PPC;
        else if (has(L"arrow"))
            t = LST_Arrow;
        else if (has(L"missile") || has(L"srm") || has(L"lrm") || has(L"rocket") || has(L"narc") || has(L"streak") || has(L"atm") ||
                 has(L"mrm"))
            t = LST_Missile;
        ShotTypeCache[ck] = t;
        LogInfo("shots: round class %ls -> %s", r->get_class()->get_fname()->to_string().c_str(),
                t == LST_PPC ? "PPC" : t == LST_Arrow ? "Arrow IV" : t == LST_Missile ? "missile" : "ballistic");
        return t;
    }

    void ShotTick(API::UObject *pc, ULONGLONG now)
    {
        if (!Ls || !pc || now - ShotMs < 100)
            return;
        ShotMs = now;
        API::UObject *me = API::get()->get_local_pawn(0);
        if (!me)
            return;
        if (!ShotProjClass)
        {
            if (ShotNoClass)
                return;
            ShotProjClass = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWProjectile");
            if (!ShotProjClass)
            {
                ShotNoClass = true;
                LogInfo("shots: MWProjectile class not found - no shooter tracing");
                return;
            }
        }
        float mp[3] = {};
        if (!CallRet(me, L"K2_GetActorLocation", {}, mp, sizeof(mp)))
            return;
        std::vector<API::UObject *> rounds;
        if (!ActorsOf(pc, ShotProjClass, rounds))
            return;
        std::unordered_map<API::UObject *, ShotRound> cur;
        const uint8_t myTeam = TeamOf(me);
        for (auto *r : rounds)
        {
            if (!r || !r->get_class())
                continue;
            if (ShotRootOff == -1)
            {
                auto *rp = FindProp(r->get_class(), L"RootComponent");
                auto *ip = FindProp(r->get_class(), L"Instigator");
                ShotRootOff = rp ? rp->get_offset() : -2;
                ShotInstOff = ip ? ip->get_offset() : -2;
                LogInfo("shots: RootComponent @%d, Instigator @%d", ShotRootOff, ShotInstOff);
            }
            if (ShotRootOff < 0 || ShotInstOff < 0)
                return;
            auto *root = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(r) + ShotRootOff);
            if (!root || !root->get_class())
                continue;
            if (ShotLocOff == -1)
            {
                auto *lp = FindProp(root->get_class(), L"RelativeLocation");
                ShotLocOff = lp ? lp->get_offset() : -2;
                LogInfo("shots: RelativeLocation @%d", ShotLocOff);
            }
            if (ShotLocOff < 0)
                return;
            const float *L = reinterpret_cast<const float *>(reinterpret_cast<uint8_t *>(root) + ShotLocOff);
            const float x = L[0], y = L[1], z = L[2];
            const float dx = x - mp[0], dy = y - mp[1], dz = z - mp[2];
            if (dx * dx + dy * dy + dz * dz > 150000.0f * 150000.0f)
                continue; // past 1.5 km: not yet a threat worth a look
            ShotRound nr{x, y, z, now, false};
            auto it = ShotPrev.find(r);
            if (it != ShotPrev.end() && now > it->second.Ms)
            {
                const ShotRound &p = it->second;
                const float dt = (now - p.Ms) / 1000.0f;
                const float vx = (x - p.X) / dt, vy = (y - p.Y) / dt, vz = (z - p.Z) / dt;
                const float v2 = vx * vx + vy * vy + vz * vz;
                nr.Counted = p.Counted;
                if (v2 > 400000.0f * 400000.0f)
                    nr.Counted = false; // jumped: a pooled round fired again
                else if (!nr.Counted && v2 > 2000.0f * 2000.0f)
                {
                    // closest approach of its straight line to the player, within 3 s
                    const float rx = mp[0] - x, ry = mp[1] - y, rz = mp[2] - z;
                    const float t = (rx * vx + ry * vy + rz * vz) / v2;
                    if (t > 0.0f && t < 3.0f)
                    {
                        const float cx = x + vx * t - mp[0], cy = y + vy * t - mp[1], cz = z + vz * t - mp[2];
                        if (cx * cx + cy * cy + cz * cz < 4000.0f * 4000.0f)
                        {
                            nr.Counted = true;
                            auto *who = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(r) + ShotInstOff);
                            if (who && who != me && IsLive(who) && TeamOf(who) != myTeam)
                            {
                                ShotRec &rec = ShotBy[who];
                                if (!rec.Last || now - rec.Last > 10000)
                                {
                                    std::memset(rec.N, 0, sizeof(rec.N));
                                    LsSplit(who, rec.Lo, rec.Hi);
                                }
                                const uint8_t ty = ShotTypeOf(r);
                                if (rec.N[ty] < 255)
                                    ++rec.N[ty];
                                rec.Last = now;
                                ++ShotTraced;
                            }
                            else
                                ++ShotUntraced;
                        }
                    }
                }
            }
            cur[r] = nr;
        }
        ShotPrev.swap(cur);
        for (auto it = ShotBy.begin(); it != ShotBy.end();)
            it = now - it->second.Last > 30000 ? ShotBy.erase(it) : std::next(it);
        // publish, most recent first
        std::vector<const ShotRec *> v;
        for (auto &kv : ShotBy)
            v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const ShotRec *a, const ShotRec *b) { return a->Last > b->Last; });
        LsShooter out[kLsMaxShooters] = {};
        uint32_t n = 0;
        for (auto *s : v)
        {
            if (n >= (uint32_t)kLsMaxShooters)
                break;
            LsShooter &o = out[n++];
            o.ActorLo = s->Lo;
            o.ActorHi = s->Hi;
            o.LastMs = (uint32_t)s->Last;
            std::memcpy(o.Rounds, s->N, sizeof(o.Rounds));
        }
        Ls->ShotSeq = Ls->ShotSeq + 1; // odd: writing
        MemoryBarrier();
        std::memcpy((void *)Ls->Shooters, out, sizeof(out));
        Ls->ShooterCount = n;
        MemoryBarrier();
        Ls->ShotSeq = Ls->ShotSeq + 1;
        if (now - ShotLogMs >= 10000 && (ShotTraced || ShotUntraced))
        {
            ShotLogMs = now;
            LogInfo("shots: last 10 s - %d round(s) at you traced to a shooter, %d not (no / friendly / gone instigator); "
                    "%u shooter(s) on record, %zu round(s) tracked", ShotTraced, ShotUntraced, n, ShotPrev.size());
            ShotTraced = ShotUntraced = 0;
        }
    }

    // One mech's own tracker records (ActorRelations): per unit, the game's line-of-sight and sensor flags.
    // Read once per call - not per unit. false = no records.
    // RELATION TABLE, LAYOUT CACHED (lancecontrol-51): the TargetTracking component's ActorRelationInfo array was found
    // by walking every property of its class (a type string per property) and its three fields by name, for every
    // lancemate on every enemy pass. Now found once per class; only the array and its flags are read each time.
    struct RelLay
    {
        API::UClass    *Cls;
        int32_t         ArrOff, Stride;
        API::FProperty *Ta, *Lp, *Sp;
        bool            Ok;
    };
    std::vector<RelLay> RelLays;
    template <class Row> bool RelTable(API::UObject *pawn, int slot, std::vector<Row> &out)
    {
        API::UObject *tt = pawn ? LsObj(pawn, L"TargetTracking") : nullptr;
        API::UClass  *c = tt ? tt->get_class() : nullptr;
        if (!c)
            return false;
        int li = -1;
        for (size_t i = 0; i < RelLays.size(); ++i)
            if (RelLays[i].Cls == c)
                li = (int)i;
        if (li < 0)
        {
            RelLay n{c, -1, 0, nullptr, nullptr, nullptr, false};
            uint8_t *data = nullptr;
            int32_t num = 0, stride = 0;
            API::UScriptStruct *es = nullptr;
            std::wstring an;
            if (FpFindArray(c, reinterpret_cast<uint8_t *>(tt), L"ActorRelationInfo", nullptr, data, num, es, stride, an))
            {
                API::FProperty *ap = FindProp(c, an.c_str());
                API::FProperty *ta = StructProp(es, L"TargetActor");
                API::FProperty *lp = StructProp(es, L"bInLineOfSight");
                API::FProperty *sp = StructProp(es, L"bInSensorRange");
                if (ap && ta && PropType(ta) == L"ObjectProperty" && lp && PropType(lp) == L"BoolProperty" && sp &&
                    PropType(sp) == L"BoolProperty")
                {
                    n.ArrOff = ap->get_offset();
                    n.Stride = stride;
                    n.Ta = ta;
                    n.Lp = lp;
                    n.Sp = sp;
                    n.Ok = true;
                }
            }
            if (RelLays.size() > 64)
                RelLays.clear();
            RelLays.push_back(n);
            li = (int)RelLays.size() - 1;
        }
        const RelLay &L = RelLays[li];
        if (!L.Ok)
            return false;
        uint8_t *at = reinterpret_cast<uint8_t *>(tt) + L.ArrOff;
        uint8_t *data = *reinterpret_cast<uint8_t **>(at);
        const int32_t num = *reinterpret_cast<int32_t *>(at + sizeof(void *));
        for (int32_t i = 0; data && i < num && i < 256; ++i)
        {
            uint8_t *el = data + (size_t)i * L.Stride;
            API::UObject *a = *reinterpret_cast<API::UObject **>(el + L.Ta->get_offset()); // compared only
            if (!a)
                continue;
            out.push_back({a, slot, reinterpret_cast<API::FBoolProperty *>(L.Lp)->get_value_from_object(el),
                           reinterpret_cast<API::FBoolProperty *>(L.Sp)->get_value_from_object(el)});
        }
        return true;
    }

    // LSTICK SHARED SCAN (lancecontrol-44, user 2026-10-13 after the lancecontrol-43 profile: enemies 3.4-4.8 ms +
    // seenby 3.1-5.4 ms per run). Both walked every pawn on the map with reflected calls - "seen by" once PER LANCEMATE,
    // the flanked check once per lancemate per hostile. Now one snapshot per LsTick pass: every pawn once - position
    // (one K2_GetActorLocation), team, dead, VTOL, controller, its visible list - and both read from it. Property
    // offsets are remembered per class (LsOff), not searched by name each time; cleared per mission. Same results:
    // the same tests on the same values, taken once per pass instead of a few hundred microseconds apart.
    struct LsOffC
    {
        API::UClass   *Cls;
        const wchar_t *Name; // a literal
        int32_t        Off;  // -1: not there / another type
    };
    std::vector<LsOffC> LsOffs;

    // The team byte's offset - "TeamID" on every unit, battle armour too (see TeamOf).
    int32_t TeamOff(API::UObject *o)
    {
        return LsOff(o, L"TeamID", nullptr);
    }

    int32_t LsOff(API::UObject *o, const wchar_t *name, const wchar_t *type)
    {
        API::UClass *c = o ? o->get_class() : nullptr;
        if (!c)
            return -1;
        for (auto &e : LsOffs)
            if (e.Cls == c && (e.Name == name || std::wcscmp(e.Name, name) == 0))
                return e.Off;
        API::FProperty *p = FindProp(c, name);
        const int32_t off = (p && (!type || PropType(p) == type)) ? p->get_offset() : -1;
        if (LsOffs.size() > 512)
            LsOffs.clear();
        LsOffs.push_back({c, name, off});
        return off;
    }

    API::UObject *LsObj(API::UObject *o, const wchar_t *name)
    {
        const int32_t off = LsOff(o, name, L"ObjectProperty");
        return off < 0 ? nullptr : *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(o) + off);
    }

    struct LsPawn
    {
        API::UObject *A, *Ctl;
        float         P[3];
        bool          HasP, Dead, Vtol;
        uint8_t       Team;
        const TArr   *Vis; // its controller's TargetVisibilityComponent.CurrentVisibleActors, or null
        bool          Full; // lancecontrol-47: the costly reads were taken (hostile or our lance), not team-only
    };
    std::vector<LsPawn> LsSnap;
    bool                LsSnapOk = false;

    void LsSnapBuild(API::UObject *pc)
    {
        LsSnap.clear();
        LsSnapOk = false;
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> all;
        if (!PawnClass || !ActorsOf(pc, PawnClass, all))
            return;
        LsSnap.reserve(all.size());
        // lancecontrol-45: only HOSTILES and our own lance get the costly reads (dead, VTOL, position, visible list) -
        // a friendly or teamless pawn (base turrets, allied units...) is skipped by every reader, as the old code did
        // before it read anything (lc44 read them all: 3+ ms with no hostile at all).
        API::UObject *me = API::get()->get_local_pawn(0);
        const int32_t mto = me ? TeamOff(me) : -1;
        const uint8_t myTeam = mto < 0 ? (uint8_t)0xFF : *(reinterpret_cast<uint8_t *>(me) + mto);
        for (auto *a : all)
        {
            LsPawn e{};
            e.A = a;
            if (a)
            {
                const int32_t to = TeamOff(a); // as TeamOf: the byte, whatever its type
                e.Team = to < 0 ? (uint8_t)0xFF : *(reinterpret_cast<uint8_t *>(a) + to);
                bool ours = a == me;
                for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers && !ours; ++k)
                    ours = TacUnit[k] == a;
                if (e.Team == 0xFF || (e.Team == myTeam && !ours))
                {
                    LsSnap.push_back(e); // listed (team only) - every reader skips it
                    continue;
                }
                e.Dead = LsDead(a);              // lancecontrol-46: IsDeadUnit's tests, lookups cached per class
                e.Vtol = LsVtol(a);              // lancecontrol-46: per class
                e.HasP = LsPos(a, e.P);          // lancecontrol-46: root component's location, checked per class
                e.Ctl = LsObj(a, L"Controller");
                API::UObject *tvc = e.Ctl ? LsObj(e.Ctl, L"TargetVisibilityComponent") : nullptr;
                const int32_t vo = tvc ? LsOff(tvc, L"CurrentVisibleActors", L"ArrayProperty") : -1;
                e.Vis = vo < 0 ? nullptr : reinterpret_cast<const TArr *>(reinterpret_cast<const uint8_t *>(tvc) + vo);
                e.Full = true;
            }
            LsSnap.push_back(e);
        }
        LsSnapOk = true;
    }

    // lancecontrol-46 (user 2026-10-13: "the map displays must work the same"): the scan's three per-unit costs.
    //  - LsPos: an actor's location IS its root component's location; an UNATTACHED root component's RelativeLocation
    //    is that world location (what K2_GetActorLocation returns), read as a field instead of a reflected call.
    //    Attached root (a turret on a building, a unit carried by a dropship), no AttachParent / RelativeLocation
    //    field: the game's call, as before. And each CLASS is checked against the game's call first - its first 5
    //    reads both ways, then every 64th: any difference over 0.5 m and that class uses the call for the rest of
    //    the mission (logged). Until a class has passed, the call's value is used.
    //  - LsVtol: IsVtol's class-name test, remembered per class.
    //  - LsDead: IsDeadUnit's tests in its order (latch, living component bIsDead, IsDead(), the old flags) with the
    //    property lookups remembered per class; the value is still read every pass.
    struct LsPosTrust
    {
        API::UClass *Cls;
        int          State; // 0 being checked, 1 trusted, -1 the game's call only
        int          Checks, Reads;
    };
    std::vector<LsPosTrust> LsPosT;
    int                     LsPosLogs = 0;

    bool LsPos(API::UObject *a, float *out)
    {
        API::UClass *c = a ? a->get_class() : nullptr;
        if (!c)
            return false;
        LsPosTrust *t = nullptr;
        for (auto &e : LsPosT)
            if (e.Cls == c)
                t = &e;
        if (!t)
        {
            if (LsPosT.size() > 256)
                LsPosT.clear();
            LsPosT.push_back({c, 0, 0, 0});
            t = &LsPosT.back();
        }
        bool direct = false;
        float d[3] = {};
        if (t->State != -1)
        {
            API::UObject *root = LsObj(a, L"RootComponent");
            const int32_t po = root ? LsOff(root, L"AttachParent", L"ObjectProperty") : -1;
            const int32_t ro = root ? LsOff(root, L"RelativeLocation", L"StructProperty") : -1;
            if (root && po >= 0 && ro >= 0 && !*reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(root) + po))
            {
                std::memcpy(d, reinterpret_cast<uint8_t *>(root) + ro, sizeof(d));
                direct = true;
            }
        }
        if (!direct)
            return CallRet(a, L"K2_GetActorLocation", {}, out, sizeof(float) * 3);
        if (t->State == 1 && (++t->Reads % 64) != 0)
        {
            std::memcpy(out, d, sizeof(d));
            return true;
        }
        // checking: both ways
        float g[3] = {};
        if (!CallRet(a, L"K2_GetActorLocation", {}, g, sizeof(g)))
        {
            std::memcpy(out, d, sizeof(d));
            return true;
        }
        const float dx = d[0] - g[0], dy = d[1] - g[1], dz = d[2] - g[2];
        const float off = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (off > 50.0f)
        {
            t->State = -1;
            if (LsPosLogs++ < 20)
                LogInfo("lstick position: %ls - its root component's location is %.1f m off the game's - the game's call "
                        "from now on (this mission)", ClassName(a).c_str(), off * 0.01f);
        }
        else if (t->State == 0 && ++t->Checks >= 5)
        {
            t->State = 1;
            if (LsPosLogs++ < 20)
                LogInfo("lstick position: %ls - root component location matches the game's (5 checks) - read directly",
                        ClassName(a).c_str());
        }
        std::memcpy(out, g, sizeof(g)); // while checking: the game's value
        return true;
    }

    struct LsClsFlag
    {
        API::UClass *Cls;
        bool         V;
    };
    std::vector<LsClsFlag> LsVtolC;

    bool LsVtol(API::UObject *a)
    {
        API::UClass *c = a ? a->get_class() : nullptr;
        for (auto &e : LsVtolC)
            if (e.Cls == c)
                return e.V;
        const bool v = IsVtol(a);
        if (LsVtolC.size() > 256)
            LsVtolC.clear();
        LsVtolC.push_back({c, v});
        return v;
    }

    struct LsPropC
    {
        API::UClass    *Cls;
        const wchar_t  *Name; // a literal
        API::FProperty *P;    // null: not there / not a bool
    };
    std::vector<LsPropC> LsBoolP;

    API::FProperty *LsBool(API::UClass *c, const wchar_t *name)
    {
        if (!c)
            return nullptr;
        for (auto &e : LsBoolP)
            if (e.Cls == c && (e.Name == name || std::wcscmp(e.Name, name) == 0))
                return e.P;
        API::FProperty *p = FindProp(c, name);
        if (p && PropType(p) != L"BoolProperty")
            p = nullptr;
        if (LsBoolP.size() > 1024)
            LsBoolP.clear();
        LsBoolP.push_back({c, name, p});
        return p;
    }

    bool LsDead(API::UObject *a)
    {
        if (!a || !ObjStillThere(a) || !a->get_class()) // lancecontrol-71: as IsDeadUnit
            return true;
        if (DeadLatchHit(a)) // lancecontrol-72
            return true;
        bool known = false;
        for (const wchar_t *cn : {L"MechLivingActorComponent", L"SimpleLivingActorComponent"}) // as LivingDead
            if (API::UObject *lc = LsObj(a, cn))
                // lancecontrol-71 (bug sweep): the lc63 check LivingDead has - a component freed during the unit's death
                // gave a garbage class here (this cached path is the one run 4 times a second on every hostile).
                if (!ObjStillThere(lc))
                    continue;
                else if (API::FProperty *p = LsBool(lc->get_class(), L"bIsDead"))
                {
                    known = true;
                    if (reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(lc))
                    {
                        DeadLatchSet(a); // lancecontrol-72
                        return true;
                    }
                    break;
                }
        if (!known)
            return IsDeadUnit(a); // no living component: the full old test (its warning included)
        if (FindFn(a, L"IsDead")) // FindFn caches per class
        {
            uint8_t dead = 0;
            if (CallRet(a, L"IsDead", {}, &dead, 1) && dead)
                return true;
        }
        for (const wchar_t *n : {L"bIsPilotDead", L"bCenterTorsoDestroyed", L"bIsDead", L"bDead"})
            if (API::FProperty *p = LsBool(a->get_class(), n))
                if (reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(a))
                    return true;
        return false;
    }

    const LsPawn *LsSnapOf(API::UObject *a) const
    {
        for (auto &e : LsSnap)
            if (e.A == a)
                return &e;
        return nullptr;
    }

    // LSTICK SLOT FROM THE SCAN (lancecontrol-47, user 2026-10-13 "go ahead" on fix 3): the per-lancemate lines
    // (the "slot" section, 3-3.8 ms in a fight) asked the game again for what this pass's scan already holds - dead,
    // position, controller, visible list - for every lancemate AND its target. Now taken from the scan when the unit
    // is in it with its costly reads done (Full); anything else (a non-pawn lock, a friendly) gets the old call.
    // Heat: the heat component is found once per unit in a slot (was: every component's class name, every run);
    // property lookups by cached offset. Checked: every 64th run per slot the old IsDeadUnit / IcHeatFrac are run
    // too and a difference is logged ("lstick slot check").
    const LsPawn *LsFull(API::UObject *a) const
    {
        if (!LsSnapOk || !a)
            return nullptr;
        const LsPawn *e = LsSnapOf(a);
        return e && e->Full ? e : nullptr;
    }

    bool LsDeadQ(API::UObject *a)
    {
        const LsPawn *e = LsFull(a);
        return e ? e->Dead : IsDeadUnit(a);
    }

    bool LsLocQ(API::UObject *a, float *out)
    {
        const LsPawn *e = LsFull(a);
        if (e && e->HasP)
        {
            std::memcpy(out, e->P, sizeof(float) * 3);
            return true;
        }
        return CallRet(a, L"K2_GetActorLocation", {}, out, sizeof(float) * 3);
    }

    API::UObject *LsHeatC[VanillaExportBlock::kTacMaxMembers + 1] = {};
    int32_t       LsHeatCur[VanillaExportBlock::kTacMaxMembers + 1] = {}, LsHeatMax[VanillaExportBlock::kTacMaxMembers + 1] = {};
    bool          LsHeatTried[VanillaExportBlock::kTacMaxMembers + 1] = {};
    unsigned      LsSlotChk[VanillaExportBlock::kTacMaxMembers + 1] = {};
    int           LsSlotChkLogs = 0;

    void LsHeatForget(int s)
    {
        LsHeatC[s] = nullptr;
        LsHeatTried[s] = false;
    }

    float LsHeatQ(int s, API::UObject *pawn, API::UClass *acc)
    {
        if (!LsHeatTried[s])
        {
            LsHeatTried[s] = true;
            LsHeatC[s] = nullptr;
            std::vector<API::UObject *> comps;
            if (acc && ComponentsOn(pawn, acc, comps))
                for (auto *c : comps) // the same pick as IcHeatFrac: the first "Heat" component with both floats
                    if (c && ClassName(c).find(L"Heat") != std::wstring::npos)
                    {
                        const int32_t co = LsOff(c, L"CurrentHeat", L"FloatProperty");
                        const int32_t mo = LsOff(c, L"MaxHeatCapacity", L"FloatProperty");
                        if (co >= 0 && mo >= 0)
                        {
                            LsHeatC[s] = c;
                            LsHeatCur[s] = co;
                            LsHeatMax[s] = mo;
                            break;
                        }
                    }
        }
        if (!LsHeatC[s])
            return -1.0f;
        const uint8_t *b = reinterpret_cast<const uint8_t *>(LsHeatC[s]);
        const float cur = *reinterpret_cast<const float *>(b + LsHeatCur[s]);
        const float mx = *reinterpret_cast<const float *>(b + LsHeatMax[s]);
        return mx > 0.0f ? cur / mx : -1.0f;
    }

    void LsSlotCheck(int s, API::UObject *u, bool dead, float heat)
    {
        if ((++LsSlotChk[s] & 63) != 0)
            return;
        const bool d0 = IsDeadUnit(u);
        const float h0 = IcHeatFrac(u);
        if (d0 == dead && std::fabs(h0 - heat) < 0.001f)
            return;
        LsHeatForget(s); // find the heat component again next run
        if (LsSlotChkLogs < 20)
        {
            ++LsSlotChkLogs;
            LogInfo("lstick slot check: slot %d %ls - dead %d / game %d, heat %.3f / game %.3f", s, ClassName(u).c_str(),
                    dead ? 1 : 0, d0 ? 1 : 0, heat, h0);
        }
    }

    // IcSeenBy from the snapshot (same tests: other team, not dead, not a VTOL, within 1.5 km, its visible list has `pawn`).
    int LsSeenBy(API::UObject *pawn, const float *here, std::vector<const LsPawn *> &seers)
    {
        seers.clear();
        if (!LsSnapOk)
            return 0;
        const LsPawn *me = LsSnapOf(pawn);
        const uint8_t ours = me ? me->Team : TeamOf(pawn);
        int withList = 0, without = 0;
        for (auto &e : LsSnap)
        {
            if (!e.A || e.A == pawn || e.Team == ours || e.Team == 0xFF || e.Dead || e.Vtol || !e.HasP)
                continue;
            const float dx = e.P[0] - here[0], dy = e.P[1] - here[1];
            if (dx * dx + dy * dy > 150000.0f * 150000.0f) // 1.5 km
                continue;
            if (!e.Vis)
            {
                ++without;
                continue;
            }
            ++withList;
            for (int32_t i = 0; e.Vis->Data && i < e.Vis->Num && i < 64; ++i)
                if (static_cast<API::UObject *const *>(e.Vis->Data)[i] == pawn)
                {
                    seers.push_back(&e);
                    break;
                }
        }
        if (!IcVisChecked && withList + without > 0)
        {
            IcVisChecked = true;
            TacLog("tacops: hold (indirect fire) - enemy visible lists: %d enemy unit(s) readable, %d without one%s", withList, without,
                    withList ? "" : " - NO ENEMY LIST READABLE: being seen cannot be detected");
        }
        return (int)seers.size();
    }

    void LsEnemyTick(API::UObject *pc, API::UObject *me, ULONGLONG now)
    {
        if (now - LsEnemyMs < 500)
            return;
        LsEnemyMs = now;
        int ps = 0; // the player's slot
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
            if (TacUnit[s] == me)
                ps = s;
        if (!LsSnapOk) // lancecontrol-44: the pass's shared scan (LsSnapBuild)
            return;
        const uint8_t ours = IaTeam(me); // lancecontrol-51: cached offset; LsDead below
        if (LsEnemyPrev.size() > 512)
            LsEnemyPrev.clear();
        // v4: every hostile's position, any range (lancecontrol-9) - "where is it?" past 1000 m - and
        // whether the lance can SEE it: in a lancemate's own visible list, or within 500 m of you.
        // lancecontrol-12: what each lancemate's own tracker says (the game's flags) - line of sight and
        // sensor range per unit, and the player's own sensor range. The visible list stays as the fallback.
        struct RelRow
        {
            API::UObject *A;
            int           Slot; // 0 = the player
            bool          Los, Sensor;
        };
        std::vector<RelRow> rels;
        bool haveRel = false;
        for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers; ++k)
        {
            API::UObject *u = TacUnit[k];
            if (!u || LsDead(u))
                continue;
            haveRel |= RelTable(u, u == me ? 0 : k, rels);
        }
        std::vector<std::pair<API::UObject *, int>> sight; // (unit, the lancemate that sees it) - lancecontrol-11
        for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers; ++k)
        {
            API::UObject *u = TacUnit[k];
            if (!u || u == me || LsDead(u))
                continue;
            const LsPawn *lp = LsSnapOf(u);
            const TArr *arr = lp ? lp->Vis : nullptr;
            if (!arr)
                continue;
            for (int32_t i = 0; arr->Data && i < arr->Num && i < 64; ++i)
                sight.push_back(std::make_pair(static_cast<API::UObject *const *>(arr->Data)[i], k)); // compared only, never read
        }
        float mp[3] = {};
        const LsPawn *meP = LsSnapOf(me);
        const bool haveMe = meP ? (meP->HasP && (std::memcpy(mp, meP->P, sizeof(mp)), true))
                                : CallRet(me, L"K2_GetActorLocation", {}, mp, sizeof(mp));
        static LsUnit units[kLsMaxUnits];
        uint32_t un = 0;
        std::vector<API::UObject *> seenNow; // lancecontrol-37: for the lock check
        for (auto &sp : LsSnap)
        {
            API::UObject *a = sp.A;
            if (!a || a == me || sp.Team == ours || sp.Team == 0xFF || un >= (uint32_t)kLsMaxUnits)
                continue;
            if (!sp.HasP)
                continue;
            const float *p = sp.P;
            LsUnit &u = units[un++];
            std::memset(&u, 0, sizeof(u));
            LsSplit(a, u.ActorLo, u.ActorHi);
            u.Loc[0] = p[0];
            u.Loc[1] = p[1];
            u.Loc[2] = p[2];
            u.Dead = sp.Dead ? 1 : 0;
            const float dx = p[0] - mp[0], dy = p[1] - mp[1];
            // lancecontrol-11: WHICH lancemates see it (bit slot - 1), and whether it has you as its target
            for (auto &rr : rels)
                if (rr.A == a)
                {
                    if (rr.Slot >= 1 && rr.Slot <= 8 && rr.Los)
                        u.SeenBy |= (uint8_t)(1u << (rr.Slot - 1));
                    if (rr.Slot >= 1 && rr.Sensor)
                        u.Flags |= LSU_OnLanceRadar;
                    if (rr.Slot == 0 && rr.Sensor)
                        u.Flags |= LSU_OnPlayerRadar;
                }
            if (!haveRel) // no relation records: the lancemates' visible lists
                for (auto &sv : sight)
                    if (sv.first == a && sv.second >= 1 && sv.second <= 8)
                        u.SeenBy |= (uint8_t)(1u << (sv.second - 1));
            u.Seen = (u.SeenBy || (haveMe && dx * dx + dy * dy < 50000.0f * 50000.0f)) ? 1 : 0;
            if (!u.Dead && (u.SeenBy || (u.Flags & (LSU_OnLanceRadar | LSU_OnPlayerRadar))))
                seenNow.push_back(a);
            if (!u.Dead)
                if (API::UObject *uc = sp.Ctl)
                    if (LsObj(uc, L"CombatTarget") == me)
                        u.Flags |= LSU_TargetsPlayer;
        }
        LockSeenNow.swap(seenNow);
        LockSeenMs = GetTickCount64();
        // FLANKED (lancecontrol-21): a live enemy within 100 m of a lancemate and in its rear arc (more than 120
        // degrees off the way its legs face) for two ticks running -> LSE_Flanked, at most every 25 s per lancemate.
        for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers && k <= 8; ++k)
        {
            API::UObject *lm = TacUnit[k];
            if (!lm || lm == me || LsDead(lm))
            {
                LsFlankCand[k] = nullptr;
                continue;
            }
            float lp[3] = {}, lr[3] = {};
            const LsPawn *lmP = LsSnapOf(lm);
            if (!(lmP ? (lmP->HasP && (std::memcpy(lp, lmP->P, sizeof(lp)), true)) : CallRet(lm, L"K2_GetActorLocation", {}, lp, sizeof(lp))) ||
                !CallRet(lm, L"K2_GetActorRotation", {}, lr, sizeof(lr)))
                continue;
            const float fx = std::cos(lr[1] * 0.0174533f), fy = std::sin(lr[1] * 0.0174533f);
            API::UObject *best = nullptr;
            float bestD = 10000.0f; // 100 m
            for (auto &sp : LsSnap)
            {
                API::UObject *a = sp.A;
                if (!a || a == me || sp.Team == ours || sp.Team == 0xFF || sp.Dead || !sp.HasP)
                    continue;
                const float *p = sp.P;
                const float dx = p[0] - lp[0], dy = p[1] - lp[1], d = std::sqrt(dx * dx + dy * dy);
                if (d < 1.0f || d >= bestD || std::fabs(p[2] - lp[2]) > 5000.0f)
                    continue;
                if ((dx * fx + dy * fy) / d < -0.5f) // behind
                {
                    best = a;
                    bestD = d;
                }
            }
            if (best && best == LsFlankCand[k] && now - LsFlankMs[k] >= 25000)
            {
                LsFlankMs[k] = now;
                LsEmit(LSE_Flanked, k, best, bestD / 100.0f);
                AiLog(L"[flank] %d: %ls %.0f m behind", k, ObjLabel(best).c_str(), bestD / 100.0f);
            }
            LsFlankCand[k] = best;
        }
        if (now - LsSumMs >= 10000) // lancecontrol-12: a summary to check the flags by
        {
            LsSumMs = now;
            int nl = 0, nt = 0, ns = 0, nr = 0, np = 0;
            for (uint32_t i = 0; i < un; ++i)
            {
                if (units[i].Dead)
                    continue;
                ++nl;
                nt += (units[i].Flags & LSU_TargetsPlayer) != 0;
                ns += units[i].SeenBy != 0;
                nr += (units[i].Flags & LSU_OnLanceRadar) != 0;
                np += (units[i].Flags & LSU_OnPlayerRadar) != 0;
            }
            if (gsp::VerboseLog()) LogInfo("units feed: %d hostile alive - %d targeting you, %d in a lancemate's line of sight, %d on the lance's "
                    "radar, %d on yours (relation records %s)", nl, nt, ns, nr, np, haveRel ? "read" : "MISSING");
        }
        if (Ls)
        {
            Ls->UnitSeq = Ls->UnitSeq + 1; // odd: writing
            MemoryBarrier();
            std::memcpy((void *)Ls->Units, units, sizeof(LsUnit) * un);
            Ls->UnitCount = un;
            MemoryBarrier();
            Ls->UnitSeq = Ls->UnitSeq + 1;
        }
        for (auto &sp : LsSnap)
        {
            API::UObject *a = sp.A;
            if (!a || a == me || sp.Team == ours || sp.Team == 0xFF || sp.Dead)
                continue;
            API::UObject *ctl = sp.Ctl;
            if (!ctl)
                continue;
            API::UObject *rdt = RdtTarget(ctl);
            API::UObject *tgt = LsObj(ctl, L"CombatTarget");
            auto it = LsEnemyPrev.find(a);
            if (it != LsEnemyPrev.end())
            {
                if (rdt == me && it->second.Rdt != me)
                    LsEmit(LSE_PlayerHit, ps, a, 0.0f);
                if (tgt == me && it->second.Tgt != me)
                    LsEmit(LSE_EnemyOnPlayer, ps, a, 0.0f);
            }
            LsEnemyPrev[a] = {rdt, tgt};
        }
    }

    struct LsClassInfo
    {
        API::UClass *Cls;
        uint8_t      Cat, Kind;
    };

    std::vector<LsClassInfo> LsClasses;

    // lancecontrol-5: a block left over from an OLDER build (smaller) is kept alive by any reader
    // still holding it - VoiceCommand left running across the update. CreateFileMapping then hands
    // back that old, smaller block and the view of the new size fails. That used to leave the feed
    // dead for the whole session without a word; now it is logged, our handle is let go (so the old
    // block can disappear once the reader closes) and it is tried again every 5 s.
    void LsOpen()
    {
        if (Ls)
            return;
        const ULONGLONG now = GetTickCount64();
        if (now < LsRetryMs)
            return;
        LsRetryMs = now + 5000;
        LsMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(LanceStatusBlock)),
                                   LANCE_MMF_NAME);
        const DWORD created = GetLastError();
        if (!LsMap)
        {
            if (!LsFailLogged)
                LogError("lance status block could not be created (error %lu) - no status feed (voice callouts / status) "
                         "until it can; trying again every 5 s", (unsigned long)created);
            LsFailLogged = true;
            return;
        }
        Ls = static_cast<LanceStatusBlock *>(MapViewOfFile(LsMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LanceStatusBlock)));
        if (!Ls)
        {
            const DWORD err = GetLastError();
            CloseHandle(LsMap);
            LsMap = nullptr;
            if (!LsFailLogged)
                LogError("lance status block: an older, smaller one is still open (error %lu%s) - most likely VoiceCommand "
                         "from before the update is still running. CLOSE VOICECOMMAND; the feed starts by itself within 5 s",
                         (unsigned long)err, created == ERROR_ALREADY_EXISTS ? ", block already existed" : "");
            LsFailLogged = true;
            return;
        }
        if (LsFailLogged)
            LogInfo("lance status block: opened at last - the feed is running");
        LsFailLogged = false;
        std::memset(Ls, 0, sizeof(LanceStatusBlock));
        Ls->Version = LANCE_VERSION;
        Ls->Enabled = 1;
        LogInfo("lance status block ready (%zu bytes, v%u)", sizeof(LanceStatusBlock), LANCE_VERSION);
    }

    static void LsSplit(API::UObject *o, uint32_t &lo, uint32_t &hi)
    {
        lo = (uint32_t)(reinterpret_cast<uintptr_t>(o) & 0xFFFFFFFFu);
        hi = (uint32_t)((uint64_t)reinterpret_cast<uintptr_t>(o) >> 32);
    }

    // LSTICK PROFILE (lancecontrol-43, user 2026-10-12: measure before changing it). The lance status feed timed by
    // section - setup (lance map), lock, enemies (LsEnemyTick: every hostile + who sees it), slot (each lancemate's
    // record), seenby (IcSeenBy), weapons (+ ammo), cond (condition, 1 Hz), pilots, publish - summed over all slots.
    // Every 30 s one "lstick profile" line (average and worst ms per run, per section) and one "lstick content" line
    // (what was published: slots alive, seen-by total, weapons, units in the hostile list, events by type) - the A/B
    // baseline for any rework: same mission, same content, less time. Cost: one QueryPerformanceCounter per section.
    static constexpr int kLsPfN = 9;
    LARGE_INTEGER        LsPfT{}, LsPfFreq{};
    int                  LsPfCur = -1, LsPfRuns = 0;
    double               LsPfRunSec[kLsPfN] = {}, LsPfSum[kLsPfN] = {}, LsPfMax[kLsPfN] = {}, LsPfTotSum = 0, LsPfTotMax = 0;
    ULONGLONG            LsPfLogMs = 0;
    unsigned             LsPfSeen = 0, LsPfWpn = 0, LsPfAlive = 0, LsPfUnits = 0, LsPfEvt[64] = {};

    void LsPfBegin()
    {
        if (!LsPfFreq.QuadPart)
            QueryPerformanceFrequency(&LsPfFreq);
        QueryPerformanceCounter(&LsPfT);
        std::memset(LsPfRunSec, 0, sizeof(LsPfRunSec));
        LsPfCur = 0;
    }

    void LsPf(int k)
    {
        if (LsPfCur < 0)
            return;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        LsPfRunSec[LsPfCur] += (double)(t.QuadPart - LsPfT.QuadPart) * 1000.0 / (double)LsPfFreq.QuadPart;
        LsPfT = t;
        LsPfCur = k;
    }

    void LsPfEnd()
    {
        if (LsPfCur < 0)
            return; // LsTick did not run this frame
        LsPf(LsPfCur);
        LsPfCur = -1;
        double tot = 0;
        for (int k = 0; k < kLsPfN; ++k)
        {
            tot += LsPfRunSec[k];
            LsPfSum[k] += LsPfRunSec[k];
            LsPfMax[k] = (std::max)(LsPfMax[k], LsPfRunSec[k]);
        }
        LsPfTotSum += tot;
        LsPfTotMax = (std::max)(LsPfTotMax, tot);
        ++LsPfRuns;
        const ULONGLONG now = GetTickCount64();
        if (!LsPfLogMs)
            LsPfLogMs = now;
        if (now - LsPfLogMs < 30000)
            return;
        LsPfLogMs = now;
        static const char *kName[kLsPfN] = {"setup", "lock", "enemies", "slot", "seenby", "weapons", "cond", "pilots", "publish"};
        char line[900];
        int n = std::snprintf(line, sizeof(line), "lstick profile, last 30 s: %d runs, total avg %.2f / worst %.2f ms -", LsPfRuns,
                              LsPfTotSum / LsPfRuns, LsPfTotMax);
        for (int k = 0; k < kLsPfN && n > 0 && n < (int)sizeof(line); ++k)
            n += std::snprintf(line + n, sizeof(line) - n, " %s %.2f/%.2f", kName[k], LsPfSum[k] / LsPfRuns, LsPfMax[k]);
        if (gsp::VerboseLog()) LogInfo("%s", line);
        n = std::snprintf(line, sizeof(line), "lstick content, last 30 s (per run): alive %.2f, seen-by %.2f, weapons %.2f, hostiles listed %.2f; events:",
                          (double)LsPfAlive / LsPfRuns, (double)LsPfSeen / LsPfRuns, (double)LsPfWpn / LsPfRuns, (double)LsPfUnits / LsPfRuns);
        for (int k = 0; k < 64 && n > 0 && n < (int)sizeof(line); ++k)
            if (LsPfEvt[k])
                n += std::snprintf(line + n, sizeof(line) - n, " %d:%u", k, LsPfEvt[k]);
        if (gsp::VerboseLog()) LogInfo("%s", line);
        LsPfRuns = 0;
        LsPfTotSum = LsPfTotMax = 0;
        std::memset(LsPfSum, 0, sizeof(LsPfSum));
        std::memset(LsPfMax, 0, sizeof(LsPfMax));
        LsPfSeen = LsPfWpn = LsPfAlive = LsPfUnits = 0;
        std::memset(LsPfEvt, 0, sizeof(LsPfEvt));
    }

    void LsEmit(uint16_t type, int slot, API::UObject *a, float value)
    {
        if (!Ls)
            return;
        if (type < 64)
            ++LsPfEvt[type]; // lancecontrol-43 (A/B content check)
        const uint32_t n = Ls->EventHead;
        LsEvent &e = Ls->Events[n % kLsEventRing];
        e.Seq = 0;
        MemoryBarrier();
        e.TimeMs = (uint32_t)GetTickCount64();
        e.Type = type;
        e.Slot = (uint8_t)slot;
        e.Pad = 0;
        LsSplit(a, e.ALo, e.AHi);
        e.Value = value;
        MemoryBarrier();
        e.Seq = n + 1;
        MemoryBarrier();
        Ls->EventHead = n + 1;
    }

    LsClassInfo LsClassOf(API::UObject *w)
    {
        for (auto &c : LsClasses)
            if (c.Cls == w->get_class())
                return c;
        const wchar_t *how = L"";
        const int lockw = IcLockWeapon(w, how);
        const WStat ws = WeaponStat(w);
        uint8_t kind = LSK_Unknown;
        if (lockw == 1)
            kind = LSK_LockOn;
        else if (ws.Kind == WK_Beam)
            kind = LSK_Beam;
        else if (ws.Kind == WK_Continuous)
            kind = LSK_Continuous;
        else if (ws.Kind == WK_AMS)
            kind = LSK_AMS;
        else if (ws.Kind == WK_Projectile)
            kind = LSK_Projectile;
        LsClassInfo c{w->get_class(), WeaponCat(w, lockw), kind};
        LsClasses.push_back(c);
        return c;
    }

    // ---- CONDITION (lance status v2, lancecontrol-4): armour / structure per part -------------
    // (and, v3 / lancecontrol-5, the ammo bins by type - read with it, once a second)
    // Read off the unit's own living-actor component the way LidarExport does for the paperdoll:
    // its per-part container objects (Head, CenterTorso, ...) each carry ArmorPoints /
    // MaxArmorPoints and HitPoints / MaxHitPoints (MWArmoredHealthComponent : MWHealthComponent).
    // Offsets resolved once per class. Once a second per lancemate, at any distance - this is
    // what "status" reports from, and it is not limited to LidarExport's 1000 m scan.
    struct LsLivingLayout
    {
        API::UClass *Cls;
        int32_t      Part[kLsParts]; // ObjectProperty offsets of the part containers, -1 none
    };
    struct LsContLayout
    {
        API::UClass *Cls;
        int32_t      ArmCur, ArmMax, HpCur, HpMax; // float offsets, -1 none
    };
    std::vector<LsLivingLayout> LsLivingLayouts;
    std::vector<LsContLayout>   LsContLayouts;
    LsCondition   LsCond[VanillaExportBlock::kTacMaxMembers + 1] = {};
    LsAmmoList    LsAmmo[VanillaExportBlock::kTacMaxMembers + 1] = {}; // v3 (lancecontrol-5)
    int           LsUav[VanillaExportBlock::kTacMaxMembers + 1] = {};  // UAVs carried at the last read, -1 unknown (lancecontrol-6)
    API::UObject *LsCondUnit[VanillaExportBlock::kTacMaxMembers + 1] = {};
    ULONGLONG     LsCondMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int32_t LsFloatOff(API::UClass *cls, const wchar_t *const *names, int n)
    {
        for (int i = 0; i < n; ++i)
            if (auto *p = FindProp(cls, names[i]))
                if (PropType(p) == L"FloatProperty")
                    return p->get_offset();
        return -1;
    }

    static float LsFrac(const uint8_t *b, int32_t cur, int32_t mx)
    {
        if (cur < 0)
            return -1.0f;
        const float c = *reinterpret_cast<const float *>(b + cur);
        const float m = mx >= 0 ? *reinterpret_cast<const float *>(b + mx) : 0.0f;
        if (m <= 0.0f)
            return c > 0.0f ? 1.0f : 0.0f;
        const float f = c / m;
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    // v3 (lancecontrol-5): its ammo bins by type, off the same component list. One entry per
    // AmmoType tag (bins of a type summed); consumables (the UAV) left out.
    // uav: the UAV consumables carried (Ammo.Consumable.UAV bins), -1 none found.
    void LsReadAmmoBins(const std::vector<API::UObject *> &comps, LsAmmoList &out, int &uav)
    {
        std::memset(&out, 0, sizeof(out));
        uav = -1;
        for (auto *c : comps)
        {
            if (!c || ClassName(c).find(L"AmmoComponent") == std::wstring::npos)
                continue;
            API::FName bt{};
            if (!AmmoTagRaw(c, L"AmmoType", bt))
                continue;
            const std::wstring tag = bt.to_string();
            if (tag.compare(0, 19, L"Ammo.Consumable.UAV") == 0)
            {
                const int u = AmmoIntOf(c, L"GetAmmoCount", L"ServerAmmoCount");
                if (u >= 0)
                    uav = (uav < 0 ? 0 : uav) + u;
                continue;
            }
            if (tag.empty() || tag.compare(0, 16, L"Ammo.Consumable.") == 0 || tag == L"Ammo.Consumable")
                continue;
            const int n = AmmoIntOf(c, L"GetAmmoCount", L"ServerAmmoCount");
            const int m = AmmoIntOf(c, L"GetMaxAmmoCount", L"ServerCurrentMaxAmmoCount");
            if (m <= 0)
                continue;
            char narrow[sizeof(out.Bins[0].Tag)] = {};
            for (size_t k = 0; k < tag.size() && k + 1 < sizeof(narrow); ++k)
                narrow[k] = (char)(tag[k] < 128 ? tag[k] : '?');
            LsAmmoBin *b = nullptr;
            for (int i = 0; i < out.Count; ++i)
                if (std::strcmp(out.Bins[i].Tag, narrow) == 0)
                    b = &out.Bins[i];
            if (!b)
            {
                if (out.Count >= kLsAmmoTypes)
                    continue;
                b = &out.Bins[out.Count++];
                std::memcpy(b->Tag, narrow, sizeof(narrow));
            }
            b->Count = (int16_t)std::min(32767, b->Count + std::max(0, n));
            b->Max = (int16_t)std::min(32767, b->Max + m);
        }
    }

    bool LsReadCondition(API::UObject *pawn, LsCondition &out, LsAmmoList &ammo, int &uav)
    {
        std::memset(&out, 0, sizeof(out));
        std::memset(&ammo, 0, sizeof(ammo));
        uav = -1;
        for (int k = 0; k < kLsParts; ++k)
            out.Armour[k] = out.Structure[k] = -1.0f;
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        std::vector<API::UObject *> comps;
        if (!pawn || !acc || !ComponentsOn(pawn, acc, comps))
            return false;
        LsReadAmmoBins(comps, ammo, uav);
        API::UObject *living = nullptr;
        for (auto *c : comps)
            if (c && ClassName(c).find(L"LivingActor") != std::wstring::npos)
            {
                living = c;
                break;
            }
        if (!living || !living->get_class())
            return false;
        API::UClass *lcls = living->get_class();
        const LsLivingLayout *ll = nullptr;
        for (auto &l : LsLivingLayouts)
            if (l.Cls == lcls)
                ll = &l;
        if (!ll)
        {
            // The part names LidarExport uses (PartNames), same order as TerrainMechPart.
            static const wchar_t *kPart[kLsParts][3] = {
                {L"Head", L"HeadArmor", nullptr},       {L"CenterTorso", L"CentreTorso", L"Torso"},
                {L"LeftTorso", L"LeftSideTorso", nullptr}, {L"RightTorso", L"RightSideTorso", nullptr},
                {L"LeftArm", nullptr, nullptr},         {L"RightArm", nullptr, nullptr},
                {L"LeftLeg", nullptr, nullptr},         {L"RightLeg", nullptr, nullptr}};
            LsLivingLayout l{lcls, {}};
            int found = 0;
            for (int k = 0; k < kLsParts; ++k)
            {
                l.Part[k] = -1;
                for (int a = 0; a < 3 && l.Part[k] < 0 && kPart[k][a]; ++a)
                    if (auto *p = FindProp(lcls, kPart[k][a]))
                        if (PropType(p) == L"ObjectProperty")
                            l.Part[k] = p->get_offset();
                found += l.Part[k] >= 0;
            }
            LsLivingLayouts.push_back(l);
            ll = &LsLivingLayouts.back();
            LogInfo("lance condition: %ls - %d of %d part containers found", ClassName(living).c_str(), found, kLsParts);
        }
        int got = 0;
        for (int k = 0; k < kLsParts; ++k)
        {
            if (ll->Part[k] < 0)
                continue;
            API::UObject *cont = *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(living) + ll->Part[k]);
            // lancecontrol-31c: no IsLive here - that is the list of live UNITS, and a container is a
            // component, so it never passed and the condition was always empty. As LidarExport.
            if (!cont || !cont->get_class())
                continue;
            API::UClass *ccls = cont->get_class();
            const LsContLayout *cl = nullptr;
            for (auto &c : LsContLayouts)
                if (c.Cls == ccls)
                    cl = &c;
            if (!cl)
            {
                // Names from LidarExport (confirmed there by the component's member dump).
                static const wchar_t *kAC[] = {L"ArmorPoints", L"CurrentArmor"};
                static const wchar_t *kAM[] = {L"MaxArmorPoints", L"InstalledArmor"};
                static const wchar_t *kSC[] = {L"HitPoints", L"CurrentStructure", L"CurrentHealth"};
                static const wchar_t *kSM[] = {L"MaxHitPoints", L"InstalledStructure", L"MaxStructure", L"MaxHealth"};
                LsContLayout c{ccls, LsFloatOff(ccls, kAC, 2), LsFloatOff(ccls, kAM, 2), LsFloatOff(ccls, kSC, 3), LsFloatOff(ccls, kSM, 4)};
                LsContLayouts.push_back(c);
                cl = &LsContLayouts.back();
                LogInfo("lance condition: container %ls - armour %d/%d, structure %d/%d", ClassName(cont).c_str(), c.ArmCur,
                        c.ArmMax, c.HpCur, c.HpMax);
            }
            const uint8_t *b = reinterpret_cast<const uint8_t *>(cont);
            out.Armour[k] = LsFrac(b, cl->ArmCur, cl->ArmMax);
            out.Structure[k] = LsFrac(b, cl->HpCur, cl->HpMax);
            got += out.Armour[k] >= 0.0f || out.Structure[k] >= 0.0f;
        }
        out.Have = got > 0 ? 1 : 0;
        return out.Have != 0;
    }

    void LsTick(API::UObject *pc)
    {
        LsOpen();
        if (!Ls)
            return;
        const ULONGLONG now = GetTickCount64();
        if (now - LsMs < 250)
            return;
        LsMs = now;
        LsPfBegin(); // lancecontrol-43: LSTICK PROFILE (A/B baseline)
        LsSnapOk = false;
        API::UObject *me = API::get()->get_local_pawn(0);
        if (!me || !pc || !TacResolve(pc))
            return;
        TacRefreshMap(pc, now);
        LsSnapBuild(pc); // lancecontrol-44: one scan of every pawn for this pass (counted in "setup")
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        static LsSlot out[kLsMaxSlots];
        std::memset(out, 0, sizeof(out));
        uint32_t count = 0;

        // The player's lock.
        LsPf(1);
        {
            API::UObject *tt = ObjProp(me, L"TargetTracking");
            API::UObject *lk = tt ? ObjProp(tt, L"CurrentTarget") : nullptr;
            if (lk && !IsLive(lk))
                lk = nullptr;
            LockProbeCheck(lk, now);
            if (lk != LsPrevLock)
            {
                LockProbeChanged(me, lk); // lancecontrol-11
                LsPrevLock = lk;
                int ps = 0;
                for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
                    if (TacUnit[s] == me)
                        ps = s;
                LsEmit(LSE_PlayerLock, ps, lk, 0.0f);
            }
            LsPf(2);
            LsEnemyTick(pc, me, now);
            LsPf(1);
        }

        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers && count < (uint32_t)kLsMaxSlots; ++s)
        {
            LsPf(3);
            API::UObject *u = TacUnit[s];
            if (u != LsPrevUnit[s])
            {
                // Another unit in this slot (or none): start its history afresh.
                LsPrevUnit[s] = u;
                LsPrevAlive[s] = false;
                LsPrevTgt[s] = nullptr;
                LsPrevSeen[s] = 0;
                LsHot[s] = false;
                LsDmgInit[s] = false;
                LsFireTgt[s] = nullptr;
                LsWpns[s].clear();
                LsHeatForget(s); // lancecontrol-47
            }
            if (!u)
                continue;
            LsSlot &o = out[count++];
            o.Valid = 1;
            o.Slot = (uint8_t)s;
            o.IsPlayer = u == me ? 1 : 0;
            LsSplit(u, o.ActorLo, o.ActorHi);
            o.FireDisc = FsDisc[s];
            o.FireMask = FsMask[s];
            o.FireHeat = FsHeat[s];
            o.TargetDistM = -1.0f;
            o.NearestSpotterM = -1.0f;
            o.Heat = -1.0f;
            const bool live = IsLive(u);
            const bool alive = live && !LsDeadQ(u); // lancecontrol-47: from the scan
            o.Alive = alive ? 1 : 0;
            if (LsPrevAlive[s] && !alive)
                LsEmit(LSE_UnitDestroyed, s, u, 0.0f);
            LsPrevAlive[s] = alive;
            // Kill: the unit it last fired at died within 5 s of the shot.
            if (LsFireTgt[s])
            {
                if (now - LsFireTgtMs[s] > 5000)
                    LsFireTgt[s] = nullptr;
                else if (!IsLive(LsFireTgt[s]) || LsDeadQ(LsFireTgt[s]))
                {
                    LsEmit(LSE_Kill, s, LsFireTgt[s], 0.0f);
                    LsFireTgt[s] = nullptr;
                }
            }
            if (!alive)
                continue;
            LsLocQ(u, o.Loc);
            uint16_t mode = 0;
            if (IcMaskSet[s]) mode |= LSM_PluginFire;
            if (IaSet[s] && IaWasOn) mode |= LSM_InsaneAllies;
            if (IcOn[s]) mode |= LSM_IndirectHold;
            if (HoldOn[s] && !IcOn[s]) mode |= LSM_Hold;
            if (FomOn[s] && FomFallback[s]) mode |= LSM_FallBack;
            if (FomOn[s] && !FomFallback[s]) mode |= LSM_MoveFire;
            if (FlOn[s]) mode |= LSM_Flank | (FlPincer[s] ? LSM_Pincer : 0); // lancecontrol-75
            if (AtkTgt[s] && IsLive(AtkTgt[s]) && !LsDeadQ(AtkTgt[s])) mode |= LSM_AttackOrder;
            if (IcMaskSet[s] && IcMaskOld[s] == 1) mode |= LSM_GameHoldFire;
            o.Mode = mode;
            const float hf = LsHeatQ(s, u, acc); // lancecontrol-47: component found once per unit
            LsSlotCheck(s, u, !alive, hf);
            o.Heat = hf;
            if (hf > 0.95f && !LsHot[s])
            {
                LsHot[s] = true;
                LsEmit(LSE_Overheat, s, nullptr, hf);
            }
            else if (hf >= 0.0f && hf < 0.85f)
                LsHot[s] = false;
            // Damage (the AI controller's LastTimeDamageTaken moving on; lancemates only).
            const LsPawn *su = LsFull(u); // lancecontrol-47
            API::UObject *ctl = su ? su->Ctl : ObjProp(u, L"Controller");
            if (ctl && u != me)
                if (const int32_t dmo = LsOff(ctl, L"LastTimeDamageTaken", L"FloatProperty"); dmo >= 0)
                    {
                        const float t = *reinterpret_cast<const float *>(reinterpret_cast<uint8_t *>(ctl) + dmo);
                        if (LsDmgInit[s] && t != LsDmgT[s] && now - LsDmgMs[s] > 3000)
                        {
                            LsDmgMs[s] = now;
                            LsEmit(LSE_TookDamage, s, RdtTarget(ctl), 0.0f); // A = who hit it (lancecontrol-8)
                        }
                        LsDmgT[s] = t;
                        LsDmgInit[s] = true;
                    }
            // Target.
            API::UObject *tgt = nullptr;
            if (u == me)
            {
                tgt = LsPrevLock;
            }
            else if (ctl)
            {
                tgt = LsObj(ctl, L"CombatTarget");
                if (tgt && (!IsLive(tgt) || LsDeadQ(tgt)))
                    tgt = nullptr;
            }
            if (tgt)
            {
                LsSplit(tgt, o.TargetLo, o.TargetHi);
                float tp[3] = {};
                if (LsLocQ(tgt, tp))
                {
                    const float dx = tp[0] - o.Loc[0], dy = tp[1] - o.Loc[1], dz = tp[2] - o.Loc[2];
                    o.TargetDistM = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01f;
                }
                if (ctl && u != me)
                {
                    if (su && su->Ctl == ctl) // lancecontrol-47: the scan's visible list (the same array OwnVisible reads)
                    {
                        o.TargetVisible = 0;
                        const TArr *arr = su->Vis;
                        for (int32_t i = 0; arr && arr->Data && i < arr->Num && i < 64; ++i)
                            if (static_cast<API::UObject *const *>(arr->Data)[i] == tgt)
                                o.TargetVisible = 1;
                    }
                    else
                    {
                        const auto vis = OwnVisible(ctl);
                        o.TargetVisible = std::find(vis.begin(), vis.end(), tgt) != vis.end() ? 1 : 0;
                    }
                }
            }
            if (u != me && tgt != LsPrevTgt[s])
                LsEmit(LSE_TargetChanged, s, tgt, 0.0f);
            LsPrevTgt[s] = tgt;
            LsPf(4);
            // Seen by.
            std::vector<const LsPawn *> seers; // lancecontrol-44: from the pass's snapshot (was IcSeenBy per lancemate)
            const int seen = LsSeenBy(u, o.Loc, seers);
            o.SeenBy = (uint8_t)(seen > 255 ? 255 : seen);
            float nd = 1e30f;
            for (auto *e : seers)
            {
                const float dx = e->P[0] - o.Loc[0], dy = e->P[1] - o.Loc[1], dz = e->P[2] - o.Loc[2];
                nd = (std::min)(nd, dx * dx + dy * dy + dz * dz);
            }
            if (nd < 1e29f)
                o.NearestSpotterM = std::sqrt(nd) * 0.01f;
            if (seen > 0 && LsPrevSeen[s] == 0)
                LsEmit(LSE_Spotted, s, seers.empty() ? nullptr : seers[0]->A, (float)seen);
            else if (seen == 0 && LsPrevSeen[s] > 0)
                LsEmit(LSE_Hidden, s, nullptr, 0.0f);
            LsPrevSeen[s] = seen;
            LsPf(5);
            // Weapons.
            std::vector<API::UObject *> comps;
            if (!acc || !ComponentsOn(u, acc, comps))
                continue;
            std::vector<LsBin> bins; // lancecontrol-51: the mech's ammo bins, once per lancemate
            LsAmmoBins(comps, bins);
            if (!AiFrTried)
                for (auto *w : comps)
                    if (w && AiIsWeaponComp(w) && !IsMeleeWeapon(w))
                    {
                        AiPrepWeaponFns(w);
                        break;
                    }
            int wn = 0;
            for (auto *w : comps)
            {
                if (wn >= kLsMaxWeapons)
                    break;
                if (!w || !AiIsWeaponComp(w) || LsMelee(w))
                    continue;
                LsWeapon &x = o.Weapons[wn];
                const LsClassInfo ci = LsClassOf(w);
                x.Category = ci.Cat;
                x.Kind = ci.Kind;
                {
                    int an = -1, am = -1;
                    LsAmmoFor(u, w, bins, an, am); // lancecontrol-51 (AmmoForWeapon re-run every 64th, differences logged)
                    x.Ammo = (int16_t)(an > 32767 ? 32767 : an);
                    x.AmmoMax = (int16_t)(am > 32767 ? 32767 : am);
                }
                x.State = LSW_Unknown;
                if (AiFrOk && AiFrFn)
                {
                    std::vector<uint8_t> f(AiFrBuf.size(), 0);
                    for (int32_t off : AiFrObjParams)
                        std::memcpy(f.data() + off, &tgt, sizeof(tgt));
                    w->process_event(AiFrFn, f.data());
                    x.State = f[AiFrRet];
                }
                LsWeaponName(w, x.Name, sizeof(x.Name)); // lancecontrol-51: per class
                // History: fired (ready -> firing / cooldown), destroyed, out of ammo.
                LsWpnPrev *pv = nullptr;
                for (auto &e : LsWpns[s])
                    if (e.W == w)
                        pv = &e;
                if (!pv)
                {
                    LsWpns[s].push_back({w, x.State, 0, false});
                    pv = &LsWpns[s].back();
                }
                else
                {
                    if (pv->State == LSW_Ready && (x.State == LSW_Firing || x.State == LSW_Cooldown))
                    {
                        pv->FiredMs = now;
                        if (tgt)
                        {
                            LsFireTgt[s] = tgt;
                            LsFireTgtMs[s] = now;
                        }
                    }
                    if (x.State == LSW_Destroyed && pv->State != LSW_Destroyed && pv->State != LSW_Unknown)
                        LsEmit(LSE_WeaponDestroyed, s, nullptr, (float)wn);
                    if (x.State == LSW_NoAmmo && pv->State != LSW_NoAmmo && pv->State != LSW_Unknown)
                        LsEmit(LSE_AmmoEmpty, s, nullptr, (float)wn);
                    const bool low = x.AmmoMax > 0 && x.Ammo >= 0 && x.Ammo * 4 <= x.AmmoMax;
                    if (low && !pv->Low && x.Ammo > 0)
                        LsEmit(LSE_AmmoLow, s, nullptr, (float)wn);
                    pv->Low = low;
                    pv->State = x.State;
                }
                x.LastFiredMs = (uint32_t)pv->FiredMs;
                for (auto &iw : IcWpns[s])
                    if (iw.W == w && iw.OwnMs && now - iw.OwnMs < 1000)
                        x.Flags |= 1;
                ++wn;
            }
            o.WeaponCount = (uint8_t)wn;
        }
        LsPf(6);
        // Condition (v2), once a second per lancemate; Cond[i] goes with out[i].
        static LsCondition cond[kLsMaxSlots];
        static LsAmmoList  ammo[kLsMaxSlots];
        std::memset(cond, 0, sizeof(cond));
        std::memset(ammo, 0, sizeof(ammo));
        for (uint32_t i = 0; i < count; ++i)
        {
            const int s = out[i].Slot;
            if (s < 1 || s > VanillaExportBlock::kTacMaxMembers || !out[i].Alive || !TacUnit[s])
                continue;
            if (LsCondUnit[s] != TacUnit[s] || now - LsCondMs[s] >= 1000)
            {
                const bool sameUnit = LsCondUnit[s] == TacUnit[s];
                LsCondUnit[s] = TacUnit[s];
                LsCondMs[s] = now;
                int uav = -1;
                LsReadCondition(TacUnit[s], LsCond[s], LsAmmo[s], uav);
                // UAV LAUNCHED (lancecontrol-6): the player's UAV count going down by one or more.
                if (out[i].IsPlayer && sameUnit && LsUav[s] >= 0 && uav >= 0 && uav < LsUav[s])
                    LsEmit(LSE_UavLaunched, s, nullptr, (float)uav);
                LsUav[s] = uav;
            }
            cond[i] = LsCond[s];
            ammo[i] = LsAmmo[s];
        }
        LsPf(7);
        // v6 (lancecontrol-17): the pilot in each lance number, from the game's lance panel.
        static LsPilot pil[kLsMaxSlots];
        std::memset(pil, 0, sizeof(pil));
        for (int s = 1; s <= kLsMaxSlots && s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            API::UObject *u = TacUnit[s];
            if (!u)
                continue;
            for (auto &d : Di)
                if (d.Pawn == u || (TacIsPlayer[s] && d.Id == 1 && d.Ctl == pc))
                {
                    LsPilot &o = pil[s - 1];
                    o.Valid = 1;
                    o.Slot = (uint8_t)s;
                    o.MemberId = (uint8_t)d.Id;
                    o.ActorLo = (uint32_t)(reinterpret_cast<uintptr_t>(u) & 0xFFFFFFFFu);
                    o.ActorHi = (uint32_t)((uint64_t)reinterpret_cast<uintptr_t>(u) >> 32);
                    Utf8Fit(o.Name, sizeof(o.Name), d.Name);
                    break;
                }
        }
        LsPf(8);
        for (uint32_t i = 0; i < count; ++i) // lancecontrol-43: what was published (A/B content check)
        {
            LsPfSeen += out[i].SeenBy;
            LsPfWpn += out[i].WeaponCount;
            LsPfAlive += out[i].Alive;
        }
        LsPfUnits += Ls->UnitCount;
        Ls->StatusSeq++; // odd: writing
        MemoryBarrier();
        std::memcpy((void *)Ls->Pilots, pil, sizeof(pil));
        std::memcpy(Ls->Cond, cond, sizeof(cond));
        std::memcpy(Ls->Ammo, ammo, sizeof(ammo));
        std::memcpy(Ls->Slots, out, sizeof(out));
        Ls->SlotCount = count;
        MemoryBarrier();
        Ls->StatusSeq++;
        Ls->TickMs = now;
    }

    // =====================================================================================
    // ARC PROBE (vanillahud-95) - DISCOVERY ONLY: reads, never writes. For the predict-and-trace
    // check on missile fire: what a volley's flight path actually is. The game builds the path when
    // the volley is fired: an MWMissilePath actor - LRMs / ATMs / Arrow IV are MWLongRangeMissilePath
    // blueprints (LongRangeMissilePathV2, LongRangeHotloadMissilePathV2, ArrowIVMissilePath) carrying
    // SplineComponents ("MissilePathSpline", blended by range, m_Range); Streaks are
    // MWHomingMissilePath and SRMs MWSpreadFireMissilePath, with no spline. For the first volleys of
    // each path class in a mission (anyone's - enemies' too), ai_probe.log gets ("arc" lines):
    //   - the path's class, IsIndirectFire, OriginPosition / FiringOrientation / m_Range, the
    //     shooter's and the locked target's positions;
    //   - every spline: its points in WORLD space and 8 samples along it;
    //   - the shooter's missiles (its projectiles fired after the path appeared), sampled every
    //     250 ms for up to 8 s, and where they were last seen (short of the target = hit something).
    struct ArcTrack
    {
        API::UObject *Path, *Shooter;
        std::wstring  Cls;
        float         Tgt[3];
        bool          HaveTgt;
        ULONGLONG     StartMs;
        int           Lines;
        std::vector<std::pair<API::UObject *, std::array<float, 3>>> Last; // missile -> last position seen
        std::vector<API::UObject *> Missiles;
    };

    std::vector<ArcTrack> ArcTracks;

    std::vector<void *> ArcSeen;

    std::vector<std::pair<std::wstring, int>> ArcPerClass;

    std::wstring ArcVec(const float *v)
    {
        wchar_t b[64];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"(%.0f, %.0f, %.0f)", v[0] * 0.01f, v[1] * 0.01f, v[2] * 0.01f);
        return b;
    }

    void ArcProbe(API::UObject *pc)
    {
        if (!AiPathCls || !AiProjCls)
            return;
        const ULONGLONG now = GetTickCount64();
        std::vector<API::UObject *> paths, rounds;
        if (!ActorsOf(pc, AiPathCls, paths))
            return;
        ActorsOf(pc, AiProjCls, rounds);
        static API::UClass *splineCls = nullptr;
        if (!splineCls)
            splineCls = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SplineComponent");
        // New volleys.
        std::vector<void *> seenNow;
        for (auto *mp : paths)
        {
            if (!mp)
                continue;
            seenNow.push_back(mp);
            if (std::find(ArcSeen.begin(), ArcSeen.end(), (void *)mp) != ArcSeen.end())
                continue;
            const std::wstring cls = ClassName(mp);
            int *n = nullptr;
            for (auto &e : ArcPerClass)
                if (e.first == cls)
                    n = &e.second;
            if (!n)
            {
                ArcPerClass.push_back({cls, 0});
                n = &ArcPerClass.back().second;
            }
            if (*n >= 6 || ArcTracks.size() >= 6)
                continue;
            ++*n;
            ArcTrack t{};
            t.Path = mp;
            t.Shooter = ObjProp(mp, L"Instigator");
            t.Cls = cls;
            t.StartMs = now;
            API::UObject *lk = nullptr, *owner = nullptr;
            CallRet(mp, L"GetLockedTarget", {}, &lk, sizeof(lk));
            if (lk)
                CallRet(lk, L"GetOwner", {}, &owner, sizeof(owner));
            if (owner && CallRet(owner, L"K2_GetActorLocation", {}, t.Tgt, sizeof(t.Tgt)))
                t.HaveTgt = true;
            float sp[3] = {};
            const bool haveShooter = t.Shooter && CallRet(t.Shooter, L"K2_GetActorLocation", {}, sp, sizeof(sp));
            uint8_t ind = 0xFF;
            if (FindFn(mp, L"IsIndirectFire"))
                CallRet(mp, L"IsIndirectFire", {}, &ind, 1);
            const int slot = AiSlotOfPawn(t.Shooter);
            float dist = -1.0f, dz = 0.0f;
            if (haveShooter && t.HaveTgt)
            {
                const float dx = t.Tgt[0] - sp[0], dy = t.Tgt[1] - sp[1];
                dz = (t.Tgt[2] - sp[2]) * 0.01f;
                dist = std::sqrt(dx * dx + dy * dy) * 0.01f;
            }
            AiLog(L"arc NEW %ls by %ls (%ls) | indirect %d | OriginPosition %ls | FiringOrientation %ls | m_Range %ls | shooter %ls | target %ls %ls | %.0f m level, %+.0f m up",
                  cls.c_str(), ObjTag(t.Shooter).c_str(), slot ? (L"slot " + std::to_wstring(slot)).c_str() : L"not ours", (int)ind,
                  AmmoField(mp, L"OriginPosition").c_str(), AmmoField(mp, L"FiringOrientation").c_str(), AmmoField(mp, L"m_Range").c_str(),
                  haveShooter ? ArcVec(sp).c_str() : L"?", ObjTag(owner).c_str(), t.HaveTgt ? ArcVec(t.Tgt).c_str() : L"(no lock)", dist, dz);
            // Its splines, in world space.
            std::vector<API::UObject *> sc;
            if (splineCls && ComponentsOn(mp, splineCls, sc))
                for (auto *c : sc)
                {
                    int32_t np = 0;
                    float len = 0.0f;
                    CallRet(c, L"GetNumberOfSplinePoints", {}, &np, sizeof(np));
                    CallRet(c, L"GetSplineLength", {}, &len, sizeof(len));
                    std::wstring pts;
                    uint8_t world = 1;
                    for (int32_t i = 0; i < np && i < 16; ++i)
                    {
                        float v[3] = {};
                        if (CallRet(c, L"GetLocationAtSplinePoint", {{L"PointIndex", &i, 4}, {L"CoordinateSpace", &world, 1}}, v, sizeof(v)))
                            pts += L" " + ArcVec(v);
                    }
                    std::wstring smp;
                    for (int k = 0; k <= 8 && len > 0.0f; ++k)
                    {
                        float d = len * (float)k / 8.0f, v[3] = {};
                        if (CallRet(c, L"GetLocationAtDistanceAlongSpline", {{L"Distance", &d, 4}, {L"CoordinateSpace", &world, 1}}, v,
                                    sizeof(v)))
                            smp += L" " + ArcVec(v);
                    }
                    AiLog(L"arc   spline %ls: %d point(s), %.0f m long | points%ls | along%ls", c->get_fname()->to_string().c_str(), np,
                          len * 0.01f, pts.c_str(), smp.c_str());
                }
            else
                AiLog(L"arc   (no spline components on %ls)", cls.c_str());
            // Its missiles: the shooter's rounds not seen before this volley.
            for (auto *r : rounds)
                if (r && ObjProp(r, L"Instigator") == t.Shooter)
                    t.Missiles.push_back(r); // older rounds: excluded below by being already known
            std::vector<API::UObject *> older;
            older.swap(t.Missiles);
            t.Last.clear();
            for (auto *r : older)
                t.Last.push_back({r, {0, 0, 0}}); // marked as "before": never sampled (Missiles stays empty)
            ArcTracks.push_back(t);
        }
        ArcSeen.swap(seenNow);
        // Follow the volleys' missiles.
        for (size_t i = 0; i < ArcTracks.size();)
        {
            ArcTrack &t = ArcTracks[i];
            // New rounds from the shooter since the volley: its missiles.
            for (auto *r : rounds)
            {
                if (!r || ObjProp(r, L"Instigator") != t.Shooter)
                    continue;
                bool known = false;
                for (auto &e : t.Last)
                    known |= e.first == r;
                if (!known && now - t.StartMs < 1500)
                {
                    t.Missiles.push_back(r);
                    t.Last.push_back({r, {0, 0, 0}});
                }
            }
            // Positions of those still flying (only pointers in this scan's list are touched).
            int alive = 0;
            std::wstring pos;
            for (auto *m : t.Missiles)
            {
                if (std::find(rounds.begin(), rounds.end(), m) == rounds.end())
                    continue;
                float p[3] = {};
                if (!CallRet(m, L"K2_GetActorLocation", {}, p, sizeof(p)))
                    continue;
                ++alive;
                for (auto &e : t.Last)
                    if (e.first == m)
                        e.second = {p[0], p[1], p[2]};
                if (alive <= 3)
                    pos += L" " + ArcVec(p);
            }
            const bool pathGone = std::find(paths.begin(), paths.end(), t.Path) == paths.end();
            if (alive && t.Lines < 32)
            {
                ++t.Lines;
                AiLog(L"arc   %ls +%.2fs: %d missile(s) flying%ls", t.Cls.c_str(), (now - t.StartMs) / 1000.0f, alive, pos.c_str());
            }
            if ((!alive && now - t.StartMs > 1500) || now - t.StartMs > 8000)
            {
                // Where they ended: the last positions seen, against the target.
                std::wstring ends;
                int shortN = 0, n = 0;
                for (auto *m : t.Missiles)
                    for (auto &e : t.Last)
                        if (e.first == m && (e.second[0] != 0.0f || e.second[1] != 0.0f))
                        {
                            ++n;
                            if (t.HaveTgt)
                            {
                                const float dx = e.second[0] - t.Tgt[0], dy = e.second[1] - t.Tgt[1], dz = e.second[2] - t.Tgt[2];
                                const float d = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01f;
                                if (d > 40.0f)
                                    ++shortN;
                                if (n <= 4)
                                {
                                    wchar_t b[96];
                                    _snwprintf_s(b, _countof(b), _TRUNCATE, L" %ls %.0f m from the target", ArcVec(e.second.data()).c_str(), d);
                                    ends += b;
                                }
                            }
                        }
                AiLog(L"arc   %ls END after %.1f s (path %ls): %d missile(s) tracked, %d ended over 40 m from the target%ls", t.Cls.c_str(),
                      (now - t.StartMs) / 1000.0f, pathGone ? L"gone" : L"still there", n, shortN, ends.c_str());
                ArcTracks.erase(ArcTracks.begin() + (ptrdiff_t)i);
                continue;
            }
            ++i;
        }
    }

    // =====================================================================================
    // AMMO PROBE (vanillahud-93) - DISCOVERY ONLY: reads, never writes. For the voice module's
    // status feed: where a unit's ammo lives. Every 2 s, for every lance slot (you included):
    //   - every component whose class name has "Ammo" in it: its layout once per class, then its
    //     count (GetAmmoCount / GetMaxAmmoCount, else the AmmoCount / ServerAmmoCount fields) and
    //     AmmoType / AmmoDataAssetId - logged when they change;
    //   - every weapon: UsesAmmo / RequiresAmmo / IsAmmoDepleted / IsAmmoLow and its own AmmoCount
    //     field, if it has one - once, then on change;
    //   - the mech's GetAllAmmoComponents / AmmoComponents / AmmoPercentage, once.
    // Lines start "ammo" in ai_probe.log.
    std::vector<API::UClass *> AmmoSeen;                        // classes whose layout was dumped

    std::vector<std::pair<API::UObject *, std::wstring>> AmmoLast; // object -> last line

    ULONGLONG AmmoMs = 0;

    bool      AmmoMechLogged[VanillaExportBlock::kTacMaxMembers + 1] = {};

    // A no-parameter function returning a number or bool: its value as text ("-" if not callable).
    std::wstring AmmoCall(API::UObject *o, const wchar_t *name)
    {
        API::UFunction *fn = FindFn(o, name);
        if (!fn)
            return L"-";
        API::FProperty *ret = nullptr;
        for (auto *f = fn->get_child_properties(); f; f = f->get_next())
        {
            auto *p = reinterpret_cast<API::FProperty *>(f);
            if (!p->is_param())
                continue;
            if (!p->is_return_param())
                return L"(params)";
            ret = p;
        }
        if (!ret)
            return L"(void)";
        const std::wstring t = PropType(ret);
        uint8_t buf[16] = {};
        const size_t rs = (t == L"IntProperty" || t == L"FloatProperty") ? 4 : 1; // vanillahud-95: was 8 - every call failed
        if (!CallRet(o, name, {}, buf, rs))
            return L"(failed)";
        wchar_t b[48];
        if (t == L"IntProperty")
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d", *reinterpret_cast<int32_t *>(buf));
        else if (t == L"FloatProperty")
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%.2f", *reinterpret_cast<float *>(buf));
        else if (t == L"BoolProperty" || t == L"ByteProperty")
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%u", (unsigned)buf[0]);
        else
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"(%ls)", t.c_str());
        return b;
    }

    // A GameplayTag property's tag (its first member is the FName TagName).
    bool AmmoTagRaw(API::UObject *o, const wchar_t *name, API::FName &out)
    {
        auto *p = o ? FindProp(o->get_class(), name) : nullptr;
        if (!p || PropType(p) != L"StructProperty")
            return false;
        std::memcpy(&out, reinterpret_cast<uint8_t *>(o) + p->get_offset(), 8);
        return out.comparison_index != 0 || out.number != 0;
    }

    std::wstring AmmoTag(API::UObject *o, const wchar_t *name)
    {
        API::FName f{};
        return AmmoTagRaw(o, name, f) ? f.to_string() : L"(none)";
    }

    // AMMO BY TAG (vanillahud-95). A mech's ammo sits in MWWeaponAmmoComponent bins, one per ammo
    // type (AmmoType, a GameplayTag - "Ammo.Missile.LRM", "Ammo.Ballistic.AutoCannon.Ultra.AC5.Clan"
    // ... MW5AmmoTags.ini and the weapon mods' own tag files). A weapon feeds from the bins whose tag
    // is its SelectedAmmoType (a weapon may take several types - YAW's MML: LRM or SRM - the selected
    // one counts); failing an exact match, a parent / child tag. Count: GetAmmoCount, else the
    // ServerAmmoCount field; max: GetMaxAmmoCount, else ServerCurrentMaxAmmoCount. -1: no ammo (energy).
    int AmmoIntOf(API::UObject *c, const wchar_t *fn, const wchar_t *field)
    {
        if (API::UFunction *f = FindFn(c, fn))
        {
            int32_t v = 0;
            if (f->get_properties_size() <= 8 && CallRet(c, fn, {}, &v, 4))
                return v;
        }
        auto *p = FindProp(c->get_class(), field);
        if (p && PropType(p) == L"IntProperty")
            return *reinterpret_cast<int32_t *>(reinterpret_cast<uint8_t *>(c) + p->get_offset());
        return -1;
    }

    // LSTICK WEAPONS, PER-CLASS DECISIONS (lancecontrol-51, user 2026-10-13 "build both and test"). For every lancemate
    // weapon, every pass: AmmoForWeapon fetched ALL the mech's components again and built a class-name string for each
    // (~22 weapons x ~60 components), IsMeleeWeapon built a string per ancestor class, and the weapon's display name
    // was rebuilt from its class name. Now: the ammo bins are listed once per lancemate (LsAmmoBins, from the
    // component list the caller already has), "is an ammo component" / "is melee" / the display name are decided once
    // per class, and tags are read through cached offsets. Same matching rules as AmmoForWeapon, which is re-run on
    // every 64th weapon and any difference logged ("lstick ammo check", max 20).
    struct LsBin
    {
        API::UObject *C;
        API::FName    T;
    };
    std::vector<std::pair<API::UClass *, bool>> LsAmmoCls, LsMeleeCls;
    struct LsWName
    {
        API::UClass *Cls;
        char         N[64]; // wider than LsWeapon::Name (36)
    };
    std::vector<LsWName> LsWNames;
    unsigned LsAmmoN = 0;
    int      LsAmmoLogs = 0;

    static int LsClsMemo(std::vector<std::pair<API::UClass *, bool>> &v, API::UClass *c)
    {
        for (auto &e : v)
            if (e.first == c)
                return e.second ? 1 : 0;
        return -1;
    }
    bool LsMelee(API::UObject *w)
    {
        API::UClass *c = w ? w->get_class() : nullptr;
        if (!c)
            return false;
        int m = LsClsMemo(LsMeleeCls, c);
        if (m < 0)
        {
            m = IsMeleeWeapon(w) ? 1 : 0;
            if (LsMeleeCls.size() > 256)
                LsMeleeCls.clear();
            LsMeleeCls.push_back({c, m == 1});
        }
        return m == 1;
    }
    void LsWeaponName(API::UObject *w, char *out, size_t outSize)
    {
        API::UClass *c = w->get_class();
        const LsWName *hit = nullptr;
        for (auto &e : LsWNames)
            if (e.Cls == c)
                hit = &e;
        if (!hit)
        {
            LsWName n{c, {}};
            std::wstring nm = ClassName(w);
            for (const wchar_t *pre : {L"LBMV_", L"Weapon_Mech_", L"Weapon_"})
                if (nm.rfind(pre, 0) == 0)
                    nm = nm.substr(wcslen(pre));
            if (nm.size() > 2 && nm.compare(nm.size() - 2, 2, L"_C") == 0)
                nm.resize(nm.size() - 2);
            for (size_t k = 0; k < nm.size() && k < sizeof(n.N) - 1; ++k)
                n.N[k] = (char)(nm[k] < 128 ? nm[k] : '?');
            if (LsWNames.size() > 256)
                LsWNames.clear();
            LsWNames.push_back(n);
            hit = &LsWNames.back();
        }
        for (size_t k = 0; k + 1 < outSize && hit->N[k]; ++k) // as before: up to outSize-1 chars (the slot was zeroed)
            out[k] = hit->N[k];
    }
    bool LsTagAt(API::UObject *o, const wchar_t *name, API::FName &out) // AmmoTagRaw with a cached offset
    {
        const int32_t off = LsOff(o, name, L"StructProperty");
        if (off < 0)
            return false;
        std::memcpy(&out, reinterpret_cast<uint8_t *>(o) + off, 8);
        return out.comparison_index != 0 || out.number != 0;
    }
    void LsAmmoBins(const std::vector<API::UObject *> &comps, std::vector<LsBin> &bins)
    {
        bins.clear();
        for (auto *c : comps)
        {
            API::UClass *cls = c ? c->get_class() : nullptr;
            if (!cls)
                continue;
            int a = LsClsMemo(LsAmmoCls, cls);
            if (a < 0)
            {
                a = ClassName(c).find(L"AmmoComponent") != std::wstring::npos ? 1 : 0;
                if (LsAmmoCls.size() > 256)
                    LsAmmoCls.clear();
                LsAmmoCls.push_back({cls, a == 1});
            }
            if (a != 1)
                continue;
            LsBin b{c, {}};
            if (LsTagAt(c, L"AmmoType", b.T))
                bins.push_back(b);
        }
    }
    bool LsAmmoFor(API::UObject *unit, API::UObject *w, const std::vector<LsBin> &bins, int &count, int &max)
    {
        count = max = -1;
        API::FName wt{};
        bool ok = LsTagAt(w, L"SelectedAmmoType", wt);
        if (ok)
        {
            std::wstring wn;
            for (int pass = 0; pass < 2 && count < 0; ++pass)
                for (auto &b : bins)
                {
                    const API::FName &bt = b.T;
                    bool match = bt.comparison_index == wt.comparison_index && bt.number == wt.number;
                    if (!match && pass == 1)
                    {
                        if (wn.empty())
                            wn = wt.to_string();
                        const std::wstring bn = bt.to_string();
                        match = (bn.size() > wn.size() && bn.compare(0, wn.size(), wn) == 0 && bn[wn.size()] == L'.') ||
                                (wn.size() > bn.size() && wn.compare(0, bn.size(), bn) == 0 && wn[bn.size()] == L'.');
                    }
                    if (!match)
                        continue;
                    const int n = AmmoIntOf(b.C, L"GetAmmoCount", L"ServerAmmoCount");
                    const int m = AmmoIntOf(b.C, L"GetMaxAmmoCount", L"ServerCurrentMaxAmmoCount");
                    count = (count < 0 ? 0 : count) + (n > 0 ? n : 0);
                    max = (max < 0 ? 0 : max) + (m > 0 ? m : 0);
                }
            ok = count >= 0;
        }
        if ((++LsAmmoN & 63u) == 0 && LsAmmoLogs < 20)
        {
            int n2 = -1, m2 = -1;
            AmmoForWeapon(unit, w, n2, m2);
            if (n2 != count || m2 != max)
            {
                ++LsAmmoLogs;
                LogInfo("lstick ammo check: %ls - new read %d / %d, the old read %d / %d", ClassName(w).c_str(), count, max, n2, m2);
            }
        }
        return ok;
    }

    bool AmmoForWeapon(API::UObject *unit, API::UObject *w, int &count, int &max, std::wstring *which = nullptr)
    {
        count = max = -1;
        API::FName wt{};
        if (!AmmoTagRaw(w, L"SelectedAmmoType", wt))
            return false;
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        std::vector<API::UObject *> comps;
        if (!acc || !ComponentsOn(unit, acc, comps))
            return false;
        std::wstring wn;
        for (int pass = 0; pass < 2 && count < 0; ++pass)
            for (auto *c : comps)
            {
                if (!c || ClassName(c).find(L"AmmoComponent") == std::wstring::npos)
                    continue;
                API::FName bt{};
                if (!AmmoTagRaw(c, L"AmmoType", bt))
                    continue;
                bool match = bt.comparison_index == wt.comparison_index && bt.number == wt.number;
                if (!match && pass == 1)
                {
                    if (wn.empty())
                        wn = wt.to_string();
                    const std::wstring bn = bt.to_string();
                    match = (bn.size() > wn.size() && bn.compare(0, wn.size(), wn) == 0 && bn[wn.size()] == L'.') ||
                            (wn.size() > bn.size() && wn.compare(0, bn.size(), bn) == 0 && wn[bn.size()] == L'.');
                }
                if (!match)
                    continue;
                const int n = AmmoIntOf(c, L"GetAmmoCount", L"ServerAmmoCount");
                const int m = AmmoIntOf(c, L"GetMaxAmmoCount", L"ServerCurrentMaxAmmoCount");
                count = (count < 0 ? 0 : count) + (n > 0 ? n : 0);
                max = (max < 0 ? 0 : max) + (m > 0 ? m : 0);
                if (which)
                    *which += (which->empty() ? L"" : L" + ") + c->get_fname()->to_string() + (pass ? L" (parent/child tag)" : L"");
            }
        return count >= 0;
    }

    std::wstring AmmoBinText(API::UObject *unit, API::UObject *w)
    {
        int n = 0, m = 0;
        std::wstring which;
        if (!AmmoForWeapon(unit, w, n, m, &which))
            return L"none";
        wchar_t b[48];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d / %d from ", n, m);
        return b + which;
    }

    std::wstring AmmoField(API::UObject *o, const wchar_t *name)
    {
        auto *p = FindProp(o->get_class(), name);
        return p ? FpVal(p, reinterpret_cast<uint8_t *>(o)) : L"-";
    }

    void AmmoNote(int s, API::UObject *o, const std::wstring &line)
    {
        for (auto &e : AmmoLast)
            if (e.first == o)
            {
                if (e.second != line)
                {
                    e.second = line;
                    AiLog(L"ammo slot %d  %ls", s, line.c_str());
                }
                return;
            }
        AmmoLast.push_back({o, line});
        AiLog(L"ammo slot %d  %ls", s, line.c_str());
    }

    void AmmoProbeTick(API::UObject *pc)
    {
        if (!AiProbeOn)
            return;
        const ULONGLONG now = GetTickCount64();
        if (now - AmmoMs < 2000 || !API::get()->get_local_pawn(0))
            return;
        AmmoMs = now;
        TacRefreshMap(pc, now);
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        if (!acc)
            return;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            API::UObject *u = TacUnit[s];
            if (!u || !IsLive(u) || IsDeadUnit(u))
                continue;
            if (!AmmoMechLogged[s])
            {
                AmmoMechLogged[s] = true;
                API::UFunction *ga = FindFn(u, L"GetAllAmmoComponents");
                AiLog(L"ammo slot %d  mech %ls: GetAllAmmoComponents %ls | AmmoComponents %ls | AmmoPercentage %ls", s,
                      ClassName(u).c_str(), ga ? Signature(ga).c_str() : L"not found", AmmoField(u, L"AmmoComponents").c_str(),
                      AmmoField(u, L"AmmoPercentage").c_str());
            }
            std::vector<API::UObject *> comps;
            if (!ComponentsOn(u, acc, comps))
                continue;
            for (auto *c : comps)
            {
                if (!c || !c->get_class())
                    continue;
                const std::wstring cn = ClassName(c);
                const bool ammoComp = cn.find(L"Ammo") != std::wstring::npos;
                const bool weapon = !ammoComp && AiIsWeaponComp(c);
                if (!ammoComp && !weapon)
                    continue;
                if (std::find(AmmoSeen.begin(), AmmoSeen.end(), c->get_class()) == AmmoSeen.end())
                {
                    AmmoSeen.push_back(c->get_class());
                    if (ammoComp)
                        FpDumpLayout(c->get_class(), "ammo component");
                }
                std::wstring line;
                if (ammoComp)
                    line = c->get_fname()->to_string() + L" (" + cn + L"): count " + AmmoCall(c, L"GetAmmoCount") + L" / max " +
                           AmmoCall(c, L"GetMaxAmmoCount") + L" | fields AmmoCount " + AmmoField(c, L"AmmoCount") +
                           L" ServerAmmoCount " + AmmoField(c, L"ServerAmmoCount") + L" ServerCurrentMaxAmmoCount " +
                           AmmoField(c, L"ServerCurrentMaxAmmoCount") + L" | type " + AmmoTag(c, L"AmmoType") + L" | low " +
                           AmmoCall(c, L"IsAmmoLow") + L" depleted " +
                           AmmoCall(c, L"IsAmmoDepleted");
                else
                    line = L"weapon " + c->get_fname()->to_string() + L": UsesAmmo " + AmmoCall(c, L"UsesAmmo") + L" RequiresAmmo " +
                           AmmoCall(c, L"RequiresAmmo") + L" IsAmmoDepleted " + AmmoCall(c, L"IsAmmoDepleted") + L" IsAmmoLow " +
                           AmmoCall(c, L"IsAmmoLow") + L" | AmmoCount " + AmmoField(c, L"AmmoCount") + L" SelectedAmmoType " +
                           AmmoTag(c, L"SelectedAmmoType") + L" | GetAmmoCount " + AmmoCall(c, L"GetAmmoCount") +
                           L" | bin " + AmmoBinText(u, c);
                AmmoNote(s, c, line);
            }
        }
    }

    // RECENT DAMAGE, EVERY UNIT (lancecontrol-7 probe, read only): RecentDamageTarget on every
    // pawn's AI controller (lancemates, enemy mechs, vehicles, turrets - whoever has one), twice a
    // second. Logged: once per unit class whether it has one ("[rdt] class ..."), every refresh
    // ("hit by") and every change of attacker ("SWITCH"), each with both sides' teams - so the next
    // mission shows whether it follows the latest attacker or sticks, and which units carry it.
    struct AiRdtSeen
    {
        API::UObject *Target;
        float         Time;
    };
    std::map<API::UObject *, AiRdtSeen> AiRdt;
    std::map<std::wstring, int>         AiRdtClasses; // class -> 1 has it, 0 not
    API::UObject                       *AiRdtYou = nullptr;
    ULONGLONG                           AiRdtMs = 0;

    static constexpr bool kAiRdtProbe = false;

    // No C++ object lives in this frame, so SEH may guard it: a bad pointer costs a line, never the game.
    static void AiRdtSafe(API::UObject *pc, ULONGLONG now)
    {
        __try
        {
            Instance->AiRdtProbe(pc, now);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void AiRdtProbe(API::UObject *pc, ULONGLONG now)
    {
        if (now - AiRdtMs < 500)
            return;
        AiRdtMs = now;
        if (!PawnClass)
            PawnClass = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        std::vector<API::UObject *> all;
        if (!PawnClass || !ActorsOf(pc, PawnClass, all))
            return;
        API::UObject *me = API::get()->get_local_pawn(0);
        if (me && me != AiRdtYou)
        {
            AiRdtYou = me;
            AiLog(L"[rdt] you = %ls (team %u) - a player mech has no AI controller, so no RecentDamageTarget",
                  ObjTag(me).c_str(), (unsigned)TeamOf(me));
        }
        auto who = [&](API::UObject *o) -> std::wstring {
            if (!o)
                return L"none";
            // NEVER touch a unit that is not in the live list (lancecontrol-8): the attacker kept
            // from an earlier look can be destroyed by now - reading its class crashed the game on
            // exit (UE4Minidump 2026-10-05 20:58, in this lambda's TeamOf).
            if (o != me && !IsLive(o))
                return L"(gone)";
            wchar_t b[48];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L" (team %u%ls)", (unsigned)TeamOf(o), o == me ? L", YOU" : L"");
            std::wstring slot;
            for (int k = 1; k <= VanillaExportBlock::kTacMaxMembers; ++k)
                if (TacUnit[k] == o)
                    slot = L" slot " + std::to_wstring(k);
            return ObjTag(o) + b + slot;
        };
        for (auto *a : all)
        {
            if (!a || a == me || IsDeadUnit(a))
                continue;
            const std::wstring cls = ClassName(a);
            API::UObject *ctl = ObjProp(a, L"Controller");
            API::UObject *tsc = ctl ? ObjProp(ctl, L"TargetSelectionComponent") : nullptr;
            API::FProperty *rp = tsc ? FindProp(tsc->get_class(), L"RecentDamageTarget") : nullptr;
            API::UStruct *rs = (rp && PropType(rp) == L"StructProperty") ? reinterpret_cast<API::FStructProperty *>(rp)->get_struct() : nullptr;
            API::FProperty *tp = rs ? StructProp(rs, L"Target") : nullptr;
            API::FProperty *up = rs ? StructProp(rs, L"LastUpdateTime") : nullptr;
            const bool has = tp && up && PropType(tp) == L"ObjectProperty" && PropType(up) == L"FloatProperty";
            if (!AiRdtClasses.count(cls))
            {
                AiRdtClasses[cls] = has ? 1 : 0;
                AiLog(L"[rdt] class %ls (team %u): %ls", cls.c_str(), (unsigned)TeamOf(a),
                      has ? L"has RecentDamageTarget"
                          : !ctl ? L"no controller" : !tsc ? (L"no TargetSelectionComponent on " + ClassName(ctl)).c_str()
                                                         : L"no RecentDamageTarget");
            }
            if (!has)
                continue;
            uint8_t *rb = reinterpret_cast<uint8_t *>(tsc) + rp->get_offset();
            API::UObject *t = *reinterpret_cast<API::UObject **>(rb + tp->get_offset());
            const float tm = *reinterpret_cast<float *>(rb + up->get_offset());
            auto it = AiRdt.find(a);
            if (it == AiRdt.end())
            {
                AiRdt[a] = {t, tm};
                if (t)
                    AiLog(L"[rdt] %ls already hit by %ls t=%.2f", who(a).c_str(), who(t).c_str(), tm);
                continue;
            }
            if (t != it->second.Target)
                AiLog(L"[rdt] SWITCH %ls: %ls -> %ls t=%.2f", who(a).c_str(), who(it->second.Target).c_str(), who(t).c_str(), tm);
            else if (tm != it->second.Time)
                AiLog(L"[rdt] %ls hit by %ls t=%.2f", who(a).c_str(), who(t).c_str(), tm);
            it->second = {t, tm};
        }
    }

    void AiProbeTick(API::UObject *pc)
    {
        if (!AiProbeOn)
            return;
        const ULONGLONG now = GetTickCount64();
        // MARK: Scroll Lock puts a numbered marker in ai_probe.log and the game log, so a moment the
        // pilot saw ("mark 3: slot 2 stopped firing") can be found exactly.
        const SHORT ks = GetAsyncKeyState(VK_SCROLL);
        const bool down = (ks & 0x8000) != 0 || (ks & 1) != 0; // bit 0: a press shorter than the tick
        if (down && !AiMarkDown)
        {
            ++AiMarks;
            AiExempt = true;
            AiLog(L"========== MARK %d ==========", AiMarks);
            AiExempt = false;
            LogInfo("ai probe: MARK %d", AiMarks);
        }
        AiMarkDown = down;
        if (now - AiSecMs >= 1000)
        {
            AiSecMs = now;
            AiFlush();
        }
        if (now - AiTickMs < 250)
            return;
        AiTickMs = now;
        if (now - TacMapMs > 500) // the slot -> unit map, fresh enough (other ticks refresh it too)
            TacRefreshMap(pc, now);
        if (kAiRdtProbe)
            AiRdtSafe(pc, now); // answered its questions (lancecontrol-7b) - off
        // The blackboard and perception take a few dozen reflected calls: one lancemate per tick.
        int bbSlot = 0;
        if (now - AiBbMs >= 250)
        {
            AiBbMs = now;
            for (int k = 0; k < VanillaExportBlock::kTacMaxMembers && !bbSlot; ++k)
            {
                const int s = 1 + (AiBbNext - 1 + k) % VanillaExportBlock::kTacMaxMembers;
                if (TacUnit[s])
                    bbSlot = s;
            }
            if (bbSlot)
                AiBbNext = 1 + bbSlot % VanillaExportBlock::kTacMaxMembers;
        }
        API::UObject *me = API::get()->get_local_pawn(0);
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            API::UObject *pawn = TacUnit[s];
            if (!pawn || pawn == me || IsDeadUnit(pawn))
            {
                if (AiSt[s].Pawn)
                {
                    AiEpisodeEnd(s, "unit gone");
                    AiCondReport(s);
                    AiLog(L"slot %d  (unit gone)", s);
                }
                AiSt[s] = AiSlot{};
                continue;
            }
            API::UObject *ctl = ObjProp(pawn, L"Controller");
            if (!ctl || !ctl->get_class())
                continue;
            if (AiSt[s].Pawn != pawn || AiSt[s].Ctl != ctl)
            {
                if (AiSt[s].Pawn)
                {
                    AiEpisodeEnd(s, "unit changed");
                    AiCondReport(s);
                }
                AiSt[s] = AiSlot{};
                AiSt[s].Pawn = pawn;
                AiSt[s].PawnName = ClassName(pawn);
                AiSt[s].Ctl = ctl;
                AiLog(L"slot %d  unit %ls, controller %ls", s, ObjTag(pawn).c_str(), ObjTag(ctl).c_str());
            }
            AiCurTag = L"[4 Hz]";
            AiReadFast(s, pawn, ctl);
            API::UObject *tgt = ObjProp(ctl, L"CombatTarget");
            if (tgt && IsDeadUnit(tgt))
                tgt = nullptr;
            AiReadWeaponComps(s, pawn, tgt);
            AiCondTick(s, ctl, tgt, AiSt[s].GameLos, now);
            AiReadDetection(s, pawn, tgt, s == bbSlot);
            if (s == bbSlot)
            {
                AiCurTag = L"[1 Hz]";
                AiReadBlackboard(s, ctl);
                AiReadPerception(s, ctl);
                AiPositions(s, pawn, tgt);
            }
            AiCurTag = L"[event]";
            if (!AiProjTried)
                AiResolveProjectiles(ctl);
        }
        AiScanProjectiles(pc);
        ArcProbe(pc);
        if (now - AiReportMs >= 60000)
        {
            if (AiReportMs)
                for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
                    if (AiSt[s].Pawn)
                        AiCondReport(s);
            AiReportMs = now;
        }
        AiHookBtNodes(now);
    }

    // ---- the behaviour tree's blueprint nodes ------------------------------------------------
    // Every blueprint BT task / service / decorator class in memory, found by walking the object
    // table (once a world, and again 30 s in for trees loaded late), gets its event functions
    // hooked the way the lance probe hooks: the UFunction's native thunk swapped for ours.
    enum AiEv { AIEV_None = 0, AIEV_TaskStart, AIEV_TaskAbort, AIEV_SvcOn, AIEV_SvcOff, AIEV_Deco, AIEV_TaskDone, AIEV_AbortDone };

    struct AiHook
    {
        API::UFunction *Fn;
        int             Ev;
    };

    std::vector<AiHook> AiHooks;

    BP_FUNC   AiOrig = nullptr;

    int       AiScans = 0;

    ULONGLONG AiScanMs = 0;

    struct AiRate
    {
        int          Slot;
        API::UObject *Node;
        int          Ev;
        ULONGLONG    Ms;
        int          Quiet;
    };

    std::vector<AiRate> AiRates;

    void AiHookBtNodes(ULONGLONG now)
    {
        if (AiScans >= 2 || (AiScanMs && now < AiScanMs))
            return;
        if (!AiScanMs)
        {
            AiScanMs = now + 5000; // first scan 5 s after the probe starts
            return;
        }
        ++AiScans;
        AiScanMs = now + 30000;
        auto *bpgc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.BlueprintGeneratedClass");
        API::UClass *bases[3] = {API::get()->find_uobject<API::UClass>(L"Class /Script/AIModule.BTTask_BlueprintBase"),
                                 API::get()->find_uobject<API::UClass>(L"Class /Script/AIModule.BTService_BlueprintBase"),
                                 API::get()->find_uobject<API::UClass>(L"Class /Script/AIModule.BTDecorator_BlueprintBase")};
        const auto sdk = API::get()->sdk();
        const auto arr = sdk->functions->get_uobject_array();
        const int total = arr ? sdk->uobject_array->get_object_count(arr) : 0;
        AiHookFinish(bases[0]);
        int classes = 0, hooked = 0;
        for (int i = 0; i < total && bpgc; ++i)
        {
            auto *obj = reinterpret_cast<API::UObject *>(sdk->uobject_array->get_object(arr, i));
            if (!obj || obj->get_class() != bpgc)
                continue;
            auto *cls = reinterpret_cast<API::UClass *>(obj);
            int kind = -1;
            for (auto *st = static_cast<API::UStruct *>(cls); st && kind < 0; st = st->get_super_struct())
                for (int k = 0; k < 3; ++k)
                    if (bases[k] && st == static_cast<API::UStruct *>(bases[k]))
                        kind = k;
            if (kind < 0)
                continue;
            ++classes;
            struct N
            {
                const wchar_t *Name;
                int            Ev;
            };
            static const N kTask[] = {{L"ReceiveExecuteAI", AIEV_TaskStart}, {L"ReceiveExecute", AIEV_TaskStart},
                                      {L"ReceiveAbortAI", AIEV_TaskAbort},   {L"ReceiveAbort", AIEV_TaskAbort}};
            static const N kSvc[] = {{L"ReceiveActivationAI", AIEV_SvcOn}, {L"ReceiveActivation", AIEV_SvcOn},
                                     {L"ReceiveDeactivationAI", AIEV_SvcOff}, {L"ReceiveDeactivation", AIEV_SvcOff}};
            static const N kDeco[] = {{L"PerformConditionCheckAI", AIEV_Deco}, {L"PerformConditionCheck", AIEV_Deco}};
            const N *list = kind == 0 ? kTask : (kind == 1 ? kSvc : kDeco);
            const int count = kind == 2 ? 2 : 4;
            for (int j = 0; j < count; ++j)
            {
                API::UFunction *fn = cls->find_function(list[j].Name);
                if (!fn || fn->get_outer() != static_cast<API::UObject *>(cls) || (fn->get_function_flags() & kFuncNative))
                    continue; // not this blueprint's own override
                bool have = false;
                for (const auto &h : AiHooks)
                    have |= h.Fn == fn;
                if (have)
                    continue;
                auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
                if (*slot != &OnAiNodeFn)
                {
                    if (!AiOrig)
                        AiOrig = *slot;
                    if (*slot != AiOrig)
                        continue; // someone else's hook: leave it alone
                    *slot = &OnAiNodeFn;
                }
                AiHooks.push_back({fn, list[j].Ev});
                ++hooked;
            }
        }
        AiLog(L"behaviour tree: scan %d found %d blueprint node class(es), hooked %d new event function(s), %d in all",
              AiScans, classes, hooked, (int)AiHooks.size());
    }

    static void *OnAiNodeFn(API::UObject *self, FFrame *frame, void *const result)
    {
        int slot = 0, ev = 0;
        if (Instance)
            AiNodeSafe(self, frame, slot, ev, false);
        void *r = (Instance && Instance->AiOrig) ? Instance->AiOrig(self, frame, result) : nullptr;
        if (Instance && slot && ev == AIEV_Deco)
            AiNodeSafe(self, frame, slot, ev, true);
        return r;
    }

    // No C++ object lives in this frame, so SEH may guard it: a bad pointer costs a line, never the game.
    static void AiNodeSafe(API::UObject *self, FFrame *frame, int &slot, int &ev, bool after)
    {
        __try
        {
            Instance->AiNode(self, frame, slot, ev, after);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // One tree event, with repeats of the same node's same event folded: once per 5 s, then the count.
    void AiEventLine(int slot, API::UObject *self, int ev)
    {
        static const wchar_t *kWhat[] = {L"", L"task started", L"task aborted", L"service on", L"service off", L"",
                                         L"task finished", L"task finished aborting"};
        if (ev <= 0 || ev > AIEV_AbortDone)
            return;
        const ULONGLONG now = GetTickCount64();
        for (auto &r : AiRates)
            if (r.Slot == slot && r.Node == self && r.Ev == ev)
            {
                if (now - r.Ms < 5000)
                {
                    ++r.Quiet;
                    return;
                }
                AiLog(L"[event] slot %d  BT %ls %ls%ls", slot, kWhat[ev], ClassName(self).c_str(),
                      r.Quiet ? (L" (+" + std::to_wstring(r.Quiet) + L" in the last 5 s)").c_str() : L"");
                r.Ms = now;
                r.Quiet = 0;
                return;
            }
        if (AiRates.size() > 2000)
            AiRates.clear();
        AiRates.push_back({slot, self, ev, now, 0});
        AiLog(L"[event] slot %d  BT %ls %ls", slot, kWhat[ev], ClassName(self).c_str());
    }

    // A blueprint task calling the built-in FinishExecute / FinishAbort: which lancemate's node it
    // is comes from the node having been seen starting (AiRates).
    void AiFinish(API::UObject *self, int ev)
    {
        int slot = 0;
        for (const auto &r : AiRates)
            if (r.Node == self && r.Ev == AIEV_TaskStart)
                slot = r.Slot;
        if (slot)
            AiEventLine(slot, self, ev);
    }

    BP_FUNC AiFinishExecOrig = nullptr, AiFinishAbortOrig = nullptr;

    bool    AiFinishHooked = false;

    static void *OnAiFinishExec(API::UObject *self, FFrame *frame, void *const result)
    {
        if (Instance)
            AiFinishSafe(self, AIEV_TaskDone);
        return (Instance && Instance->AiFinishExecOrig) ? Instance->AiFinishExecOrig(self, frame, result) : nullptr;
    }

    static void *OnAiFinishAbort(API::UObject *self, FFrame *frame, void *const result)
    {
        if (Instance)
            AiFinishSafe(self, AIEV_AbortDone);
        return (Instance && Instance->AiFinishAbortOrig) ? Instance->AiFinishAbortOrig(self, frame, result) : nullptr;
    }

    static void AiFinishSafe(API::UObject *self, int ev)
    {
        __try
        {
            Instance->AiFinish(self, ev);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // The built-in FinishExecute / FinishAbort on BTTask_BlueprintBase, hooked once: every blueprint
    // task ends through one of them. The success flag is read by the engine from the calling
    // blueprint's code, not from a parameter block, so it is not visible here - only that it ended.
    void AiHookFinish(API::UClass *taskBase)
    {
        if (AiFinishHooked || !taskBase)
            return;
        AiFinishHooked = true;
        for (int k = 0; k < 2; ++k)
        {
            API::UFunction *fn = taskBase->find_function(k ? L"FinishAbort" : L"FinishExecute");
            if (!fn)
            {
                AiLog(L"behaviour tree: %ls not found - tasks will show starting only", k ? L"FinishAbort" : L"FinishExecute");
                continue;
            }
            auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
            BP_FUNC mine = k ? &OnAiFinishAbort : &OnAiFinishExec;
            if (*slot == mine)
                continue;
            (k ? AiFinishAbortOrig : AiFinishExecOrig) = *slot;
            *slot = mine;
            AiLog(L"behaviour tree: hooked %ls", k ? L"FinishAbort" : L"FinishExecute");
        }
    }

    void AiNode(API::UObject *self, FFrame *frame, int &slot, int &ev, bool after)
    {
        if (!frame || !frame->Node || !frame->Locals)
            return;
        if (!after)
        {
            for (const auto &h : AiHooks)
                if (h.Fn == frame->Node)
                    ev = h.Ev;
            if (!ev)
                return;
            // Whose tree: the OwnerController (AI events) or OwnerActor parameter.
            API::UObject *owner = nullptr;
            for (const wchar_t *pn : {L"OwnerController", L"OwnerActor"})
                if (auto *p = frame->Node->find_property(pn))
                {
                    owner = *reinterpret_cast<API::UObject **>(frame->Locals + p->get_offset());
                    break;
                }
            for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers && owner; ++s)
                if (AiSt[s].Ctl == owner || AiSt[s].Pawn == owner)
                    slot = s;
            if (!slot || ev == AIEV_Deco)
                return; // the decorator is logged after it has answered
            AiEventLine(slot, self, ev);
            return;
        }
        // After a decorator: its answer, logged when it changes.
        if (auto *p = frame->Node->find_property(L"ReturnValue"))
        {
            const bool v = *(frame->Locals + p->get_offset()) != 0;
            const wchar_t *was = AiCurTag;
            AiCurTag = L"[event]";
            AiSet(slot, L"BT if " + ClassName(self), v ? L"true" : L"false");
            AiCurTag = was;
        }
    }

    uint32_t  TacWaitSeq = 0;

    ULONGLONG TacWaitMs = 0;

    void TacRunCommands(API::UObject *pc)
    {
        const uint32_t head = Block->TacCmdHead;
        if (!TacDoneInit)
        {
            TacDoneInit = true;
            TacDone     = head; // anything queued before we were running is stale
        }
        if (head == TacDone)
            return;
        MemoryBarrier();
        if (head - TacDone > (uint32_t)VanillaExportBlock::kTacCmdRing)
            TacDone = head - VanillaExportBlock::kTacCmdRing; // fell behind: the oldest are gone
        const bool ready = pc && API::get()->get_local_pawn(0) && TacResolve(pc);
        if (ready)
            TacRefreshMap(pc, GetTickCount64());
        // ONE ORDER PER TICK (100 ms apart). The game never posts two squad orders in the same
        // frame - the F-keys cannot - and a burst of them is the most likely reason an order to
        // the second of two selected lancemates went missing.
        int ran = 0;
        while (TacDone != head)
        {
            const VanillaExportBlock::TacCmd c = Block->TacCmds[TacDone % VanillaExportBlock::kTacCmdRing];
            if (c.Seq != TacDone + 1)
            {
                // Claimed but not written yet (vanillahud-93: more than one writer) - wait for it,
                // up to 250 ms; older than that, or overwritten, it is skipped.
                const ULONGLONG t = GetTickCount64();
                if ((int32_t)(c.Seq - (TacDone + 1)) < 0)
                {
                    if (TacWaitSeq != TacDone + 1)
                    {
                        TacWaitSeq = TacDone + 1;
                        TacWaitMs = t;
                    }
                    if (t - TacWaitMs < 250)
                        break;
                }
                ++TacDone;
                continue; // torn or overwritten
            }
            // Fire settings are not squad orders: run at once, whatever else waits (vanillahud-92).
            if (c.Order == VanillaExportBlock::TCO_FireSettings)
            {
                ++TacDone;
                FsOrder(c);
                continue;
            }
            if (c.Order == VanillaExportBlock::TCO_NavMark || c.Order == VanillaExportBlock::TCO_NavUnmark)
            {
                ++TacDone; // NAV POINTS (lancecontrol-22): not squad orders either
                NavOrder(c);
                continue;
            }
            if (ran >= 1)
                break;
            ++TacDone;
            if (ready)
                TacPost(pc, c);
            ++ran;
        }
    }

    // =====================================================================================
    // MOVEMENT PROBE (build vanillahud-25) - DISCOVERY ONLY, for the TacOps autopilot. Writes
    // nothing to the game.
    //
    // The autopilot would drive your legs the way the stick does, and nothing names what the
    // stick drives: the cockpit only READS MWMech::GetMechThrottle. So, once per mech:
    //   - every movement-ish function on the mech, its movement components and the controller
    //     (class by class, up the chain), with its signature;
    //   - every movement-ish number on them, which becomes a watch list;
    // then, for three minutes, every change in the watch list is logged as you drive - throttle
    // up, turn, reverse, stop - so the value the input lands in shows itself.
    // =====================================================================================
    struct MoveWatch
    {
        API::UObject   *Obj;
        API::FProperty *Prop;
        std::wstring    Name;
        int             Kind; // 0 float, 1 int, 2 byte/enum, 3 bool, 4 vector/rotator
        float           Last[3];
    };

    std::vector<MoveWatch> MoveWatches;

    API::UObject *MovePawn     = nullptr;

    ULONGLONG     MoveStartMs  = 0, MoveNextMs = 0;

    int           MoveLines    = 0;

    static constexpr ULONGLONG kMoveProbeMs = 180000;

    static constexpr int       kMoveMaxLines = 900;

    static bool MoveKey(const std::wstring &n)
    {
        static const wchar_t *k[] = {L"Throttle", L"Speed", L"Move", L"Turn", L"Yaw", L"Steer", L"Input", L"Walk",
                                     L"Reverse", L"Velocity", L"Leg", L"Heading", L"Rotat", L"Drive", L"Accel",
                                     L"Decel", L"Stride", L"Gait", L"Axis", L"Forward", L"Brake", L"Stop"};
        for (auto *s : k)
            if (n.find(s) != std::wstring::npos)
                return true;
        return false;
    }

    // Every class of `o` up to (not including) the engine's own Actor/Object/Controller bases.
    void MoveDumpClasses(const wchar_t *who, API::UObject *o)
    {
        if (!o)
            return;
        for (auto *st = static_cast<API::UStruct *>(o->get_class()); st; st = st->get_super_struct())
        {
            const std::wstring cn = st->get_fname()->to_string();
            if (cn == L"Actor" || cn == L"Object" || cn == L"Pawn" || cn == L"Controller" || cn == L"ActorComponent" ||
                cn == L"SceneComponent")
                break;
            LogInfo("move probe: %ls class %ls", who, cn.c_str());
            auto *fnClass = API::UFunction::static_class();
            for (auto *f = st->get_children(); f; f = f->get_next())
            {
                if (f->get_class() != fnClass)
                    continue;
                auto *fn = reinterpret_cast<API::UFunction *>(f);
                if (!MoveKey(fn->get_fname()->to_string()))
                    continue;
                LogInfo("move probe:   fn %ls%s", Signature(fn).c_str(), (fn->get_function_flags() & kFuncNative) ? " [native]" : "");
            }
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring n = f->get_fname()->to_string();
                if (!MoveKey(n))
                    continue;
                const std::wstring ty = PropType(p);
                int kind = -1;
                if (ty == L"FloatProperty") kind = 0;
                else if (ty == L"IntProperty") kind = 1;
                else if (ty == L"ByteProperty" || ty == L"EnumProperty") kind = 2;
                else if (ty == L"BoolProperty") kind = 3;
                else if (ty == L"StructProperty")
                {
                    auto *s2 = reinterpret_cast<API::FStructProperty *>(p)->get_struct();
                    const std::wstring sn = s2 ? s2->get_fname()->to_string() : L"";
                    if (sn == L"Vector" || sn == L"Rotator")
                        kind = 4;
                }
                ProbeLines = 0;
                ProbeDump(std::wstring(who) + L"." + n, p, reinterpret_cast<uint8_t *>(o) + p->get_offset(), 2);
                if (kind >= 0 && MoveWatches.size() < 120)
                {
                    MoveWatch w{o, p, std::wstring(who) + L"." + n, kind, {}};
                    MoveRead(w, w.Last);
                    MoveWatches.push_back(w);
                }
            }
        }
    }

    static void MoveRead(const MoveWatch &w, float out[3])
    {
        const uint8_t *a = reinterpret_cast<const uint8_t *>(w.Obj) + w.Prop->get_offset();
        out[0] = out[1] = out[2] = 0.0f;
        switch (w.Kind)
        {
        case 0: out[0] = *reinterpret_cast<const float *>(a); break;
        case 1: out[0] = (float)*reinterpret_cast<const int32_t *>(a); break;
        case 2: out[0] = (float)*a; break;
        case 3: out[0] = reinterpret_cast<API::FBoolProperty *>(w.Prop)->get_value_from_propbase(const_cast<uint8_t *>(a)) ? 1.0f : 0.0f; break;
        case 4: std::memcpy(out, a, 12); break;
        }
    }

    void MoveProbe(API::UObject *pc, ULONGLONG now)
    {
        if (!kDiscoveryProbes)
            return;
        API::UObject *pawn = API::get()->get_local_pawn(0);
        if (!pawn || !pawn->get_property_data<void *>(L"MechMeshComponent"))
            return;
        if (pawn != MovePawn)
        {
            MovePawn = pawn;
            MoveWatches.clear();
            MoveLines = 0;
            LogInfo("move probe: ---- mech %ls ----", pawn->get_full_name().c_str());
            MoveDumpClasses(L"mech", pawn);
            MoveDumpClasses(L"pc", pc);
            // The mech's components: every class once, and the movement-ish ones in full.
            if (auto *acls = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent"))
            {
                std::vector<API::UObject *> comps;
                ComponentsOn(pawn, acls, comps);
                std::wstring all;
                for (auto *c : comps)
                {
                    const std::wstring cn = ClassName(c);
                    if (all.find(cn) == std::wstring::npos)
                        all += cn + L" ";
                    if (cn.find(L"Move") != std::wstring::npos || cn.find(L"Locomot") != std::wstring::npos ||
                        cn.find(L"Throttle") != std::wstring::npos || cn.find(L"Leg") != std::wstring::npos ||
                        cn.find(L"Input") != std::wstring::npos || cn.find(L"Engine") != std::wstring::npos)
                        MoveDumpClasses((L"comp " + cn).c_str(), c);
                }
                LogInfo("move probe: mech components: %ls", all.c_str());
            }
            LogInfo("move probe: watching %zu value(s) for 3 minutes - drive: throttle up, turn both ways, reverse, stop",
                    MoveWatches.size());
            MoveStartMs = now;
            MoveNextMs  = now;
        }
        if (now - MoveStartMs > kMoveProbeMs || now < MoveNextMs || MoveLines >= kMoveMaxLines)
            return;
        MoveNextMs = now + 200;
        float gm = 0.0f;
        CallRet(pawn, L"GetMechThrottle", {}, &gm, sizeof(gm));
        for (auto &w : MoveWatches)
        {
            float v[3];
            MoveRead(w, v);
            const float tol = (w.Kind == 4) ? 1.0f : 1.0e-3f;
            if (std::fabs(v[0] - w.Last[0]) <= tol && std::fabs(v[1] - w.Last[1]) <= tol && std::fabs(v[2] - w.Last[2]) <= tol)
                continue;
            if (w.Kind == 4)
                LogInfo("move probe: +%.1fs %ls (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)   [GetMechThrottle %.3f]",
                        (now - MoveStartMs) / 1000.0, w.Name.c_str(), w.Last[0], w.Last[1], w.Last[2], v[0], v[1], v[2], gm);
            else
                LogInfo("move probe: +%.1fs %ls %.4g -> %.4g   [GetMechThrottle %.3f]", (now - MoveStartMs) / 1000.0,
                        w.Name.c_str(), w.Last[0], v[0], gm);
            std::memcpy(w.Last, v, sizeof(v));
            if (++MoveLines >= kMoveMaxLines)
            {
                LogInfo("move probe: line cap reached");
                break;
            }
        }
    }

    // =====================================================================================
    // LANCEMATE MOVE PROBE (build vanillahud-31) - DISCOVERY ONLY. Why lancemates are slow to a
    // TacOps move. For 60 s after each move, per lancemate, every 0.5 s: distance left, speed
    // against top speed, the game's throttle reading - and every change in the movement-ish
    // state of its AI controller, its path following, its movement component and its group
    // movement component (the candidates: job start delay, combat interrupting, corner-to-corner
    // path following, a speed cap). What each of those holds is dumped once per lancemate.
    // =====================================================================================
    struct LanceTrace
    {
        int           Slot;
        API::UObject *Pawn;
        float         Goal[3];
        float         Start[3];
        ULONGLONG     StartMs, NextMs;
        bool          Moving;
        float         MaxCmS;
        std::vector<MoveWatch> W;
        API::UObject *HeatComp;
        int32_t       HeatOff;
    };

    std::vector<LanceTrace> LTraces;

    BeamSet   LBeams;

    ULONGLONG LBeamsMs = 0;

    std::vector<API::UObject *> LDumped; // pawns whose state was dumped this mission

    int LTraceLines = 0;

    static constexpr int kLTraceMaxLines = 3000;

    // Scalars of `o` whose name has one of `keys` (all when empty), up the class chain to the
    // engine's own bases: dumped once, and added to the watch list.
    void LWatch(std::vector<MoveWatch> &out, API::UObject *o, const std::wstring &who,
                std::initializer_list<const wchar_t *> keys, bool dump)
    {
        if (!o || !o->get_class())
            return;
        for (auto *st = static_cast<API::UStruct *>(o->get_class()); st; st = st->get_super_struct())
        {
            const std::wstring cn = st->get_fname()->to_string();
            if (cn == L"Actor" || cn == L"Object" || cn == L"ActorComponent")
                break;
            for (auto *f = st->get_child_properties(); f; f = f->get_next())
            {
                auto *p = reinterpret_cast<API::FProperty *>(f);
                const std::wstring n = f->get_fname()->to_string();
                bool want = keys.size() == 0;
                for (auto *k : keys)
                    want |= n.find(k) != std::wstring::npos;
                if (!want)
                    continue;
                const std::wstring ty = PropType(p);
                int kind = -1;
                if (ty == L"FloatProperty") kind = 0;
                else if (ty == L"IntProperty") kind = 1;
                else if (ty == L"ByteProperty" || ty == L"EnumProperty") kind = 2;
                else if (ty == L"BoolProperty") kind = 3;
                else if (ty == L"StructProperty")
                {
                    auto *s2 = reinterpret_cast<API::FStructProperty *>(p)->get_struct();
                    const std::wstring sn = s2 ? s2->get_fname()->to_string() : L"";
                    if (sn == L"Vector" || sn == L"Rotator")
                        kind = 4;
                }
                if (dump)
                {
                    ProbeLines = 0;
                    ProbeDump(who + L"." + n, p, reinterpret_cast<uint8_t *>(o) + p->get_offset(), 2);
                }
                if (kind >= 0 && out.size() < 80)
                {
                    MoveWatch w{o, p, who + L"." + n, kind, {}};
                    MoveRead(w, w.Last);
                    out.push_back(w);
                }
            }
        }
    }

    void LTraceStart(int slot, const float *goal)
    {
        if (!kDiscoveryProbes)
            return;
        API::UObject *pawn = (slot >= 1 && slot <= VanillaExportBlock::kTacMaxMembers) ? TacUnit[slot] : nullptr;
        if (!pawn || TacIsPlayer[slot])
            return;
        for (auto it = LTraces.begin(); it != LTraces.end(); ++it)
            if (it->Slot == slot)
            {
                LTraces.erase(it); // a new order replaces the trace of the old one
                break;
            }
        LanceTrace t{};
        t.Slot = slot;
        t.Pawn = pawn;
        std::memcpy(t.Goal, goal, sizeof(t.Goal));
        CallRet(pawn, L"K2_GetActorLocation", {}, t.Start, sizeof(t.Start));
        CallRet(pawn, L"GetMechMaxSpeed", {}, &t.MaxCmS, sizeof(t.MaxCmS));
        t.StartMs = t.NextMs = GetTickCount64();
        const bool dump = std::find(LDumped.begin(), LDumped.end(), pawn) == LDumped.end();
        if (dump)
        {
            LDumped.push_back(pawn);
            LogInfo("lance move probe: ---- slot %d %ls ----", slot, pawn->get_full_name().c_str());
        }
        API::UObject *ctl = ObjProp(pawn, L"Controller");
        const std::initializer_list<const wchar_t *> aiKeys = {L"Job", L"Command", L"Engag", L"Target", L"State", L"Focus",
                                                             L"Move", L"Order", L"Combat", L"Path", L"Speed", L"Status",
                                                             L"Wait", L"Guard", L"Travel", L"Aggro", L"Range"};
        LWatch(t.W, ctl, L"ai", aiKeys, dump);
        if (ctl)
            LWatch(t.W, ObjProp(ctl, L"WorkerComponent"), L"worker", {}, dump);
        if (ctl)
            LWatch(t.W, ObjProp(ctl, L"PathFollowingComponent"), L"path",
                   {L"Status", L"Speed", L"Accept", L"Segment", L"Block", L"Move", L"Path", L"Dest", L"Stop", L"Brak"}, dump);
        API::UObject *mc = nullptr;
        CallRet(pawn, L"GetMechMovementComponent", {}, &mc, sizeof(mc));
        LWatch(t.W, mc, L"move", {L"Speed", L"Limit", L"Throttle", L"Turn", L"Input", L"Suppress", L"Max"}, dump);
        if (auto *acls = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent"))
        {
            std::vector<API::UObject *> comps;
            ComponentsOn(pawn, acls, comps);
            t.HeatOff = -1;
            for (auto *c : comps)
            {
                const std::wstring cn = ClassName(c);
                if (cn == L"MWGroupMovementComponent")
                {
                    LWatch(t.W, c, L"group", {}, dump);
                    // The group itself (MWPlayerSquadGroup, led by the player): a leash distance
                    // would live here.
                    LWatch(t.W, ObjProp(c, L"Group"), L"squadgroup", {}, dump);
                }
                if (cn == L"MWHeatComponent")
                {
                    // The heat value itself: the first float named like it.
                    for (auto *st = static_cast<API::UStruct *>(c->get_class()); st && t.HeatOff < 0; st = st->get_super_struct())
                        for (auto *f = st->get_child_properties(); f && t.HeatOff < 0; f = f->get_next())
                        {
                            const std::wstring n = f->get_fname()->to_string();
                            if (PropType(reinterpret_cast<API::FProperty *>(f)) == L"FloatProperty" &&
                                (n == L"CurrentHeat" || n == L"Heat" || n == L"HeatLevel" || n == L"CurrentHeatLevel"))
                            {
                                t.HeatComp = c;
                                t.HeatOff  = reinterpret_cast<API::FProperty *>(f)->get_offset();
                            }
                        }
                    LWatch(t.W, c, L"heat", {L"Heat"}, dump);
                }
            }
        }
        LogInfo("lance move probe: slot %d ordered to (%.0f, %.0f, %.0f), %.0f m away, top speed %.0f cm/s, watching %zu value(s)",
                slot, goal[0], goal[1], goal[2],
                std::sqrt((goal[0] - t.Start[0]) * (goal[0] - t.Start[0]) + (goal[1] - t.Start[1]) * (goal[1] - t.Start[1])) * 0.01f,
                t.MaxCmS, t.W.size());
        LTraces.push_back(std::move(t));
    }

    void LTraceTick(API::UObject *pc)
    {
        if (LTraces.empty())
            return;
        TacRefreshMap(pc, GetTickCount64()); // which pawns are still alive and in the lance
        const ULONGLONG now = GetTickCount64();
        for (size_t i = 0; i < LTraces.size();)
        {
            LanceTrace &t = LTraces[i];
            const bool alive = t.Slot >= 1 && t.Slot <= VanillaExportBlock::kTacMaxMembers && TacUnit[t.Slot] == t.Pawn;
            if (!alive || now - t.StartMs > 60000 || LTraceLines >= kLTraceMaxLines)
            {
                LogInfo("lance move probe: slot %d trace ends (%s)", t.Slot,
                        !alive ? "unit gone" : (LTraceLines >= kLTraceMaxLines ? "line cap" : "60 s"));
                LTraces.erase(LTraces.begin() + (long)i);
                continue;
            }
            if (now < t.NextMs)
            {
                ++i;
                continue;
            }
            t.NextMs = now + 500;
            const float secs = (now - t.StartMs) / 1000.0f;
            float here[3] = {}, spd = 0.0f, thr = 0.0f;
            CallRet(t.Pawn, L"K2_GetActorLocation", {}, here, sizeof(here));
            CallRet(t.Pawn, L"GetMechSpeed", {}, &spd, sizeof(spd));
            CallRet(t.Pawn, L"GetMechThrottle", {}, &thr, sizeof(thr));
            const float dx = t.Goal[0] - here[0], dy = t.Goal[1] - here[1];
            const float dist = std::sqrt(dx * dx + dy * dy) * 0.01f;
            const float moved = std::sqrt((here[0] - t.Start[0]) * (here[0] - t.Start[0]) +
                                          (here[1] - t.Start[1]) * (here[1] - t.Start[1])) * 0.01f;
            if (!t.Moving && moved > 3.0f)
            {
                t.Moving = true;
                LogInfo("lance move probe: slot %d STARTED MOVING after %.1f s", t.Slot, secs);
            }
            // Is it fighting? The AI's combat and torso targets, and the mech's heat (it rises
            // when the mech fires).
            API::UObject *ctl = ObjProp(t.Pawn, L"Controller");
            API::UObject *ct = ctl ? ObjProp(ctl, L"CombatTarget") : nullptr;
            API::UObject *tt = ctl ? ObjProp(ctl, L"TorsoTarget") : nullptr;
            // Heat is NOT a firing measure - vanillahud-32 showed it tracking speed (mechs heat up
            // walking). Lasers are: MWRay beams starting within 15 m of this mech.
            int lasers = 0;
            if (LBeamsMs != now)
            {
                LBeamsMs = now;
                ReadBeams(pc, LBeams);
            }
            for (uint32_t b = 0; b < LBeams.Count; ++b)
            {
                const float *f = LBeams.B[b].From;
                if ((f[0] - here[0]) * (f[0] - here[0]) + (f[1] - here[1]) * (f[1] - here[1]) < 1500.0f * 1500.0f)
                    ++lasers;
            }
            // How far from the player - the stop came at the same distance for every lancemate.
            float me[3] = {};
            if (API::UObject *mine = API::get()->get_local_pawn(0))
                CallRet(mine, L"K2_GetActorLocation", {}, me, sizeof(me));
            const float toMe = std::sqrt((me[0] - here[0]) * (me[0] - here[0]) + (me[1] - here[1]) * (me[1] - here[1])) * 0.01f;
            // What job its AI is doing - the move order is one; when it stops, is that job gone?
            std::wstring job = L"-";
            if (ctl)
                if (API::UObject *wk = ObjProp(ctl, L"WorkerComponent"))
                {
                    API::UObject *j = nullptr;
                    if (CallRet(wk, L"GetJob", {}, &j, sizeof(j)) && j)
                        job = ClassName(j);
                }
            // Which way the legs face the enemy: 0 = straight at it, 180 = rear to it.
            int facing = -1;
            if (ct)
            {
                float rot[3] = {}, ep[3] = {};
                CallRet(t.Pawn, L"K2_GetActorRotation", {}, rot, sizeof(rot));
                if (CallRet(ct, L"K2_GetActorLocation", {}, ep, sizeof(ep)))
                {
                    float d = std::atan2(ep[1] - here[1], ep[0] - here[0]) * 57.2957795f - rot[1];
                    while (d > 180.0f) d -= 360.0f;
                    while (d < -180.0f) d += 360.0f;
                    facing = (int)std::fabs(d);
                }
            }
            wchar_t face[24] = L"-";
            if (facing >= 0)
                _snwprintf_s(face, _countof(face), _TRUNCATE, L"%d deg%s", facing, facing > 90 ? L" REAR" : L"");
            const char *mode = FomOn[t.Slot] ? (FomFallback[t.Slot] ? "  [fall-back]" : "  [fire-on-move]") : "";
            LogInfo("lance move probe: slot %d +%.1fs  left %.0f m  from you %.0f m  speed %.0f (top %.0f)  throttle %.2f  job %ls  combat %ls  legs-to-enemy %ls  torso %ls  lasers %d%s",
                    t.Slot, secs, dist, toMe, spd, t.MaxCmS, thr, job.c_str(), ct ? ClassName(ct).c_str() : L"-", face,
                    tt ? ClassName(tt).c_str() : L"-", lasers, mode);
            ++LTraceLines;
            for (auto &w : t.W)
            {
                float v[3];
                MoveRead(w, v);
                const float tol = (w.Kind == 4) ? 50.0f : 1.0e-3f;
                if (std::fabs(v[0] - w.Last[0]) <= tol && std::fabs(v[1] - w.Last[1]) <= tol && std::fabs(v[2] - w.Last[2]) <= tol)
                    continue;
                if (w.Kind == 4)
                    LogInfo("lance move probe: slot %d +%.1fs  %ls (%.0f, %.0f, %.0f) -> (%.0f, %.0f, %.0f)", t.Slot, secs,
                            w.Name.c_str(), w.Last[0], w.Last[1], w.Last[2], v[0], v[1], v[2]);
                else
                    LogInfo("lance move probe: slot %d +%.1fs  %ls %.4g -> %.4g", t.Slot, secs, w.Name.c_str(), w.Last[0], v[0]);
                std::memcpy(w.Last, v, sizeof(v));
                ++LTraceLines;
            }
            if (dist < 20.0f)
            {
                LogInfo("lance move probe: slot %d ARRIVED after %.1f s (%.0f m walked)", t.Slot, secs, moved);
                LTraces.erase(LTraces.begin() + (long)i);
                continue;
            }
            ++i;
        }
    }

    // =====================================================================================
    // THE MOVE POINT, ENFORCED (build vanillahud-34).
    //
    // In the game, a lancemate given a TacOps move walks to wherever the PLAYER'S RETICLE is
    // aiming, not to the map point - even though the order carries the map point
    // (OrderContext.TargetLocation, confirmed by the order probe). The swap happens after the
    // order: the lancemate's move job (Ally_GoToLocationJob) works out its own destination
    // when it starts. So that job's start (K2_BeginWorker - an event the game calls, the same
    // kind of hook as HUD_C::Tick) is hooked, and straight after it has run:
    //   - what the job chose is logged, beside the point we sent (the diagnosis);
    //   - the job's TargetLocation and the AI's blackboard travel key are set to our point;
    //   - the AI is sent there itself (AIController::MoveToLocation), replacing the move the job
    //     started - pathfinding on, partial paths allowed.
    // Only for a lancemate that TacOps ordered in the last 20 s; F-key orders are left alone.
    // =====================================================================================
    API::UFunction *GoToBeginFn = nullptr;

    BP_FUNC         GoToOrig    = nullptr; // the newest hook's original (fallback only - see GoToHooks)

    // lancecontrol-70 (user 2026-10-17, from the same-shape crash audit): every K2_BeginWorker we ever hooked, with ITS
    // OWN original. A function already in this list is never re-hooked and its slot never re-read, so a detour another
    // module might put on top of ours can never be saved as "the original" (the loop that crashed LidarExport). Hooks
    // are kept for the session (never unhooked), and the detour finds its original by the running function.
    struct GoToHook
    {
        API::UFunction *Fn;
        BP_FUNC         Orig;
    };
    std::vector<GoToHook> GoToHooks;

    bool            GoToTried   = false;

    float           TacPendGoal[VanillaExportBlock::kTacMaxMembers + 1][3] = {};

    ULONGLONG       TacPendMs[VanillaExportBlock::kTacMaxMembers + 1] = {};

    int             TacPendDirect[VanillaExportBlock::kTacMaxMembers + 1] = {}; // direct passes done

    int             PendLog = 0;

    // A MOVE TO A LANCEMATE THAT IS ALREADY MOVING (vanillahud-43). The point fix lives in the
    // Ally_GoToLocationJob's K2_BeginWorker - and the log showed that only runs when the job is
    // NEW: a second move order to a lancemate still on a move job produced no "move job started"
    // line at all, so nothing put the TacOps point back and it took the game's (the reticle's).
    // So a move still pending 0.4 s after it was posted is enforced directly - blackboard and
    // MoveToLocation, the same as GoToFix does - and once more at 1.5 s, in case the job applies
    // its own point a little later. If the hook does fire, it takes over as before.
    void GoToPendTick(API::UObject *pc)
    {
        const ULONGLONG now = GetTickCount64();
        bool refreshed = false;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers; ++s)
        {
            if (!TacPendMs[s])
                continue;
            const ULONGLONG age = now - TacPendMs[s];
            const int due = age >= 1500 ? 2 : (age >= 400 ? 1 : 0);
            if (due <= TacPendDirect[s])
                continue;
            TacPendDirect[s] = due;
            // The slot's unit from THIS tick's lance list: one that died since the order was
            // posted is gone from it rather than a pointer to a destroyed pawn.
            if (!refreshed)
            {
                TacRefreshMap(pc, now);
                refreshed = true;
            }
            API::UObject *pawn = TacUnit[s];
            API::UObject *ctl = pawn ? ObjProp(pawn, L"Controller") : nullptr;
            if (ctl)
            {
                uint8_t res = 0xFF;
                const bool bbOk = SendToPoint(ctl, TacPendGoal[s], &res);
                if (PendLog++ < 60)
                    TacLog("tacops: slot %d - no new move job %.1f s after the order (already on one?), point enforced directly: blackboard %s, MoveToLocation result %u",
                            s, age / 1000.0f, bbOk ? "set" : "not set", (unsigned)res);
            }
            if (due == 2)
                TacPendMs[s] = 0;
        }
    }

    ULONGLONG GoToNextTryMs = 0;

    void GoToInstallHook()
    {
        if (GoToBeginFn)
            return;
        const ULONGLONG now = GetTickCount64();
        if (now < GoToNextTryMs)
            return; // find_uobject walks the object table: not every tick
        GoToNextTryMs = now + 3000;
        auto *cls = API::get()->find_uobject<API::UClass>(
            L"BlueprintGeneratedClass /Game/AI/Jobs/Ally_GoToLocationJob.Ally_GoToLocationJob_C");
        if (!cls)
            return;
        auto *fn = cls->find_function(L"K2_BeginWorker");
        if (!fn || (fn->get_function_flags() & kFuncNative))
        {
            if (!GoToTried)
                TacLog("tacops: Ally_GoToLocationJob K2_BeginWorker %s - move point cannot be enforced",
                        fn ? "is native" : "not found");
            GoToTried = true;
            return;
        }
        for (auto &h : GoToHooks)
            if (h.Fn == fn)
            {
                GoToBeginFn = fn; // hooked earlier this session - kept, slot not touched
                return;
            }
        auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
        if (*slot == &OnGoToBegin)
        {
            GoToBeginFn = fn; // (never expected: ours, but not in the list)
            return;
        }
        GoToHooks.push_back({fn, *slot});
        GoToOrig    = *slot;
        *slot       = &OnGoToBegin;
        GoToBeginFn = fn;
        TacLog("tacops: hooked Ally_GoToLocationJob::K2_BeginWorker (%p) - kept for the session (%d hooked)", (void *)fn,
               (int)GoToHooks.size());
    }

    static void *OnGoToBegin(API::UObject *self, FFrame *frame, void *const result)
    {
        API::UObject *worker = nullptr;
        if (Instance && frame && frame->Locals && frame->Node)
            if (auto *wp = frame->Node->find_property(L"Worker"))
                worker = *reinterpret_cast<API::UObject **>(frame->Locals + wp->get_offset());
        // lancecontrol-70: this function's own original (by the running function), the newest one as a fallback
        BP_FUNC orig = nullptr;
        if (Instance)
        {
            if (frame)
                for (auto &h : Instance->GoToHooks)
                    if ((void *)h.Fn == (void *)frame->Node)
                        orig = h.Orig;
            if (!orig)
                orig = Instance->GoToOrig;
        }
        // LOOP GUARD (as the cockpit tick): a chain that leads back into this detour is cut at depth 8, one error line.
        static thread_local int loopDepth = 0;
        if (loopDepth >= 8)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                API::get()->log_error("[LanceControl] move job hook: the K2_BeginWorker chain loops back into LanceControl - cut");
            }
            return nullptr;
        }
        ++loopDepth;
        void *r = orig ? orig(self, frame, result) : nullptr;
        --loopDepth;
        if (Instance)
            GoToFixSafe(self, worker);
        return r;
    }

    static void GoToFixSafe(API::UObject *job, API::UObject *worker)
    {
        __try
        {
            Instance->GoToFix(job, worker);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void GoToFix(API::UObject *job, API::UObject *worker)
    {
        if (!job || !worker)
            return;
        // worker (component) -> its AI controller -> the pawn -> which TacOps slot.
        API::UObject *ctl = nullptr;
        CallRet(worker, L"GetOwner", {}, &ctl, sizeof(ctl));
        API::UObject *pawn = ctl ? ObjProp(ctl, L"Pawn") : nullptr;
        int slot = 0;
        for (int s = 1; s <= VanillaExportBlock::kTacMaxMembers && !slot; ++s)
            if (pawn && TacUnit[s] == pawn)
                slot = s;
        const ULONGLONG now = GetTickCount64();
        if (!slot || !TacPendMs[slot] || now - TacPendMs[slot] > 20000)
            return; // not a TacOps order (or too old): the game's own, left alone
        TacPendMs[slot] = 0;
        const float *goal = TacPendGoal[slot];

        // What the job chose.
        float chosen[3] = {};
        auto *tl = FindProp(job->get_class(), L"TargetLocation");
        if (tl && PropType(tl) == L"StructProperty")
            std::memcpy(chosen, reinterpret_cast<uint8_t *>(job) + tl->get_offset(), 12);
        const float off = std::sqrt((chosen[0] - goal[0]) * (chosen[0] - goal[0]) + (chosen[1] - goal[1]) * (chosen[1] - goal[1]));
        TacLog("tacops: slot %d move job started - job chose (%.0f, %.0f, %.0f), TacOps sent (%.0f, %.0f, %.0f): %.0f m apart",
                slot, chosen[0], chosen[1], chosen[2], goal[0], goal[1], goal[2], off * 0.01f);

        // Put ours back everywhere the job keeps it.
        if (tl && PropType(tl) == L"StructProperty")
            std::memcpy(reinterpret_cast<uint8_t *>(job) + tl->get_offset(), goal, 12);
        if (auto *kp = FindProp(job->get_class(), L"BB_TravelLocation"))
            if (PropType(kp) == L"NameProperty")
            {
                std::memcpy(&TravelKey, reinterpret_cast<uint8_t *>(job) + kp->get_offset(), sizeof(TravelKey));
                TravelKeyOk = true;
            }
        uint8_t res = 0xFF;
        const bool bbOk = SendToPoint(ctl, goal, &res);
        TacLog("tacops: slot %d move point enforced - job target %s, blackboard %s, MoveToLocation %s (result %u)", slot,
                tl ? "set" : "n/a", bbOk ? "set" : "not set", res != 0xFF ? "sent" : "not available", (unsigned)res);
    }

    // The AI's travel destination on its blackboard, and the AI sent there itself. Used when the
    // move job starts (GoToFix) and whenever a fall-back / fire-on-the-move leg has to be put
    // back on course (FomTick). Returns whether the blackboard took it; `res` is MoveToLocation's
    // result, 0xFF when it could not be called.
    API::FName TravelKey{};

    bool       TravelKeyOk = false;

    bool SendToPoint(API::UObject *ctl, const float *goal, uint8_t *res)
    {
        bool bbOk = false;
        if (API::UObject *bb = ObjProp(ctl, L"Blackboard"))
        {
            API::FName key = TravelKeyOk ? TravelKey : API::FName(L"TravelLocation", API::FName::EFindName::Find);
            float g[3] = {goal[0], goal[1], goal[2]};
            bbOk = Call(bb, L"SetValueAsVector", {{L"KeyName", &key, sizeof(key)}, {L"VectorValue", g, sizeof(g)}});
        }
        float dest[3] = {goal[0], goal[1], goal[2]};
        float accept = 500.0f;
        bool yes = true;
        uint8_t r = 0xFF;
        if (!CallRet(ctl, L"MoveToLocation",
                     {{L"Dest", dest, sizeof(dest)},
                      {L"AcceptanceRadius", &accept, 4},
                      {L"bUsePathfinding", &yes, 1},
                      {L"bProjectDestinationToNavigation", &yes, 1},
                      {L"bAllowPartialPath", &yes, 1}},
                     &r, 1))
            r = 0xFF;
        if (res)
            *res = r;
        return bbOk;
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
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
std::unique_ptr<LanceControl> g_plugin{new LanceControl()};
