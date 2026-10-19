// LidarExport.cpp
// UEVR plugin: each tick, read the player mech + all nearby mechs via UEVR
// reflection and publish their positions to a memory-mapped file for the external
// LIDAR display app. Reads only — modifies nothing in the game.
//
// Built against sicsix's PluginExtension framework (MIT). See BUILD.md.

#include <PluginExtension.h>
#include <windows.h>
#include <GsPointer.h>
#include <vector>
#include <algorithm> // rockdeco1: std::stable_sort
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "SlowTimer.h"
#include <deque>
#include <memory>
#include <cmath>
#include <initializer_list>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <mutex>
#include "LidarShared.h"
#include "TerrainShared.h"
#include "LanceStatusShared.h" // 2026-10-15: the 5 km radar mode reads who the lance detects (read only)

// UE4.26 FVector / FRotator are 32-bit floats. These mirror the return layout of
// K2_GetActorLocation / K2_GetActorRotation so call_function can fill them.
struct FVector3f { float X, Y, Z; };
struct FVector2f { float X, Y; };
struct FRotator3f { float Pitch, Yaw, Roll; };

// UE TArray/FScriptArray header layout (64-bit): { data ptr, count, capacity }.
// Reading a UArrayProperty's data as this lets us walk e.g. ULevel.Actors directly
// instead of the whole global UObject array.
struct FScriptArrayLayout { void* Data; int32_t Num; int32_t Max; };
// NOTE for anyone adding a property read here: TWeakObjectPtr is NOT a pointer. It is an
// {int32 ObjectIndex, int32 ObjectSerialNumber} pair into the global object array, and
// reading it as void* yields (Serial<<32 | Index) - a garbage address that LOOKS plausible.
// An EXCEPTION_ACCESS_VIOLATION reading 0x000000010000000f is literally serial=1, index=15.
// Equally, a property's NAME says nothing about its type: a float called TargetLockProgress
// holds 1.0f, whose bits are 0x3f800000, and dereferencing that faults at 0x3f800010. Both
// of those crashed this plugin. Confirm the property class before reading the slot.

class LidarExport final : public PluginExtension {
    HANDLE            MmfHandle    = nullptr;
    LidarExportBlock* Block        = nullptr;
    API::UClass*      MechClass     = nullptr;
    UEVR_UClassHandle BaseMechClass = nullptr; // native MWMech base; catches every mech subclass
    UEVR_UClassHandle PawnClass   = nullptr;   // native Pawn; used to cheaply gate vehicle resolution
    UEVR_UClassHandle ActorClass  = nullptr;   // native Actor; gates projectile-base resolution
    UEVR_UClassHandle TankClass   = nullptr;   // MWTank base   (resolved from a live instance)
    UEVR_UClassHandle TurretClass = nullptr;   // MWTurret base
    UEVR_UClassHandle VtolClass   = nullptr;   // MWVTOL base
    UEVR_UClassHandle BaClass     = nullptr;   // KelClanElemental base - one battle armour suit (ba1, 2026-10-17)
    UEVR_UClassHandle ProjClass   = nullptr;   // MWProjectile base (all traveling munitions)
    UEVR_UClassHandle MissileClass = nullptr;  // MWMissilePath base (enumerated for the lock-on check)

    // UGameplayStatics::GetAllActorsOfClass - the cheap enumeration (live world actors
    // of a class only, no CDOs). GaacBuf is the reused out-array (leaks once at exit).
    API::UObject*      GsCdo   = nullptr;
    API::UFunction*    GaacFn  = nullptr;
    FScriptArrayLayout GaacBuf{};
    API::UObject*     TorsoTwistC   = nullptr; // player's TorsoTwistComponent (for the aim line)
    API::UObject*     TargetTrackingC = nullptr; // player's TargetTracking component (locked target)
    std::vector<std::wstring> TargetFields;       // ObjectProperty names on it that hold a live target
    std::unordered_map<void*, uint16_t> IdMap;    // actor pointer -> stable contact id (across scans)
    uint16_t                  NextId = 1;          // 0 reserved for "none"

    // Stable id for a mech/vehicle/ally pointer. IdMap is otherwise never pruned, and
    // over a long mission actors churn (deaths + reinforcement waves), so it would grow
    // without bound. Cap it: at the ceiling, clear and restart numbering - costs at most a
    // one-frame id reshuffle (a trail/selection blink), which we'll essentially never hit
    // in normal play. Projectiles do NOT use this (they'd blow the cap in seconds).
    uint16_t AssignId(void* p) {
        auto it = IdMap.find(p);
        if (it != IdMap.end()) return it->second;
        if (IdMap.size() >= 8192) { IdMap.clear(); NextId = 1; }
        uint16_t id = NextId++;
        if (NextId == 0) NextId = 1;
        IdMap[p] = id;
        return id;
    }

    // Projectile diagnostics (regain log visibility into the hot path - stripped earlier).
    int      LastProjEnum    = 0;  // pointers GAAC returned last projectile tick
    uint32_t LastProjWritten = 0;  // projectiles actually published last tick
    int      PeakProjEnum    = 0;  // high-water mark since the last diagnostic log

    // Projectile hot-path caches (hundreds of rounds can be airborne). Classification is
    // per class, so `to_string` runs once per weapon type, not once per round. The two
    // spent-round filter bools live on AActor (shared offset/mask), resolved once.
    std::unordered_map<void*, uint8_t> ProjTypeCache; // projectile class ptr -> LidarProjType
    API::FBoolProperty* PropBeingDestroyed = nullptr;
    API::FBoolProperty* PropHidden         = nullptr;

    // Resolve the two spent-actor bools ONCE. They live on AActor, so one offset/mask pair
    // serves every actor class - no property-chain walk per object, which is the rule this
    // file lives by on the projectile hot path.
    void EnsureActorFlagProps() {
        if (PropBeingDestroyed || !ActorClass) return;
        auto* ac = reinterpret_cast<API::UClass*>(ActorClass);
        if (auto* pd = ac->find_property(L"bActorIsBeingDestroyed"))
            if (pd->get_class() && pd->get_class()->get_name() == L"BoolProperty")
                PropBeingDestroyed = reinterpret_cast<API::FBoolProperty*>(pd);
        if (auto* ph = ac->find_property(L"bHidden"))
            if (ph->get_class() && ph->get_class()->get_name() == L"BoolProperty")
                PropHidden = reinterpret_cast<API::FBoolProperty*>(ph);
    }

    // Remember where a launch happened, for the spatial match in the projectile scan.
    // Position is the cheap pose read - a plain property slot, no ProcessEvent - and runs only
    // for paths that are new this probe, which is a handful even in a heavy fight.
    void RememberLaunch(API::UObject* path, bool onMe) {
        FVector3f pos{}; float yaw = 0.0f;
        if (!ReadActorPose(path, pos, yaw)) {
            // NO POSITION, NO VOTE - and this is a SILENT way to lose a salvo entirely, so it
            // is counted. A path we cannot place contributes no match candidate, so every
            // round it fires is rejected for having no credible origin and the alarm never
            // sounds for it. MWMissilePath is a "fixed-point path descriptor" and there is no
            // guarantee every subclass has a RootComponent to read.
            //
            // Matches land at 15-25 m so this clearly succeeds for the common case; what is
            // unknown is whether some class fails. onMe is tracked separately because a
            // failure on a path aimed at us is the one that costs a warning.
            ++PoseFailAll;
            if (onMe) ++PoseFailOnMe;
            return;
        }
        PendingLaunches.push_back(PendingLaunch{pos, onMe});
    }

    // Id for a live projectile: its existing one, or the next free value.
    //
    // The probe walks forward from a rolling cursor rather than just incrementing, so a
    // wrapped counter cannot hand out an id another live round still holds. Bounded by the
    // id space and, with at most LIDAR_MAX_PROJECTILES rounds in a 65535-value space, it
    // finds a free slot almost immediately.
    // Return ids whose quarantine has expired to the free pool. The deque is in release
    // order, so this stops at the first entry still serving its time.
    void DrainProjIdQuarantine(ULONGLONG now) {
        while (!ProjIdQuarantine.empty() &&
               now - ProjIdQuarantine.front().FreedAtMs >= kIdQuarantineMs) {
            ProjIdUsed.erase(ProjIdQuarantine.front().Id);
            ProjIdQuarantine.pop_front();
        }
    }

    uint16_t AcquireProjId(void* actor) {
        auto it = ProjIdMap.find(actor);
        if (it != ProjIdMap.end()) return it->second;
        for (int guard = 0; guard < 0x10000; ++guard) {
            if (NextProjId == 0) NextProjId = 1;          // 0 is reserved
            const uint16_t cand = NextProjId++;
            if (ProjIdUsed.insert(cand).second) {
                ProjIdMap[actor] = cand;
                return cand;
            }
        }
        return 1; // id space exhausted - impossible at 512 live rounds, but never return 0
    }

    // Read an object reference ONLY when the property really is one.
    //
    // A property's NAME says nothing about its type, and inferring one from the other has
    // crashed this plugin twice: a float called something like TargetLockProgress holds
    // 1.0f, whose bits are 0x3f800000, and dereferencing that faults at 0x3f800010; a
    // TWeakObjectPtr read as void* yields (Serial<<32 | Index) and faults at 0x10000000f.
    // Resolve the property, confirm its class, and only then read the slot.
    API::UObject* SafeObjectProp(API::UObject* owner, const wchar_t* name) {
        if (!owner || !owner->get_class()) return nullptr;
        auto* pd = owner->get_class()->find_property(name);
        if (!pd || !pd->get_class()) return nullptr;
        if (pd->get_class()->get_name() != L"ObjectProperty") return nullptr;
        auto* slot = owner->get_property_data<void*>(name);
        return (slot && *slot) ? reinterpret_cast<API::UObject*>(*slot) : nullptr;
    }

    // A pooled projectile that has been used and returned is still ENUMERATED by
    // GetAllActorsOfClass, which is why every round is screened this way.
    //
    // Do NOT extend this to MWMissilePath. It was tried: 238 logged samples all reported
    // spent=0, so not one dormant path was ever screened out. The pathN volatility it was
    // meant to explain (2 -> 22 -> 2 between samples) is not pool noise at all - a
    // MWMissilePath is spawned per MISSILE, not per salvo, confirmed by a burst of new paths
    // all sharing one class (LongRangeMissilePathV2_C) and by pathN tracking lockMe almost
    // exactly at high counts. Those swings are real missiles arriving and impacting.
    bool IsSpentActor(API::UObject* a) const {
        if (PropBeingDestroyed && PropBeingDestroyed->get_value_from_object(static_cast<void*>(a))) return true;
        if (PropHidden        && PropHidden->get_value_from_object(static_cast<void*>(a)))        return true;
        return false;
    }
    // --- LPROJ_INCOMING hold ------------------------------------------------------------
    // This flag exists ONLY for the frozen standalone LidarDisplay.exe, which reads it to
    // colour incoming missiles. HUDDisplay ignores it entirely - its alarm reads the
    // block-level ThreatFlags instead.
    //
    // It used to be armed by Betty voice-line hooks and the AMS detection sphere. Both are
    // gone: Betty fires on a throttled voice queue (misses salvos while she is saying
    // something else) and the AMS sphere counts every missile in radius regardless of
    // guidance (false-triggers on dumbfire SRMs). The MWMissilePath lock probe is strictly
    // better than either - exact, homing-only, every salvo, no cooldown - so the hold is
    // now armed from pathLockMe alone. The standalone's display gets more accurate as a
    // side effect, and the per-tick cost of both legacy mechanisms disappears.
    // --- per-round threat attribution --------------------------------------------------
    // The plugin decides, per ROUND, whether it is part of a salvo guiding on us, and says
    // so in LidarProjectile.Flags. This has to happen HERE and cannot be reconstructed
    // downstream, because the published id is a 16-bit hash of the actor pointer: two live
    // rounds can collide, and a pooled actor re-fired after impact hands its id to an
    // unrelated round. Only this side holds the actual pointer, which is exact.
    //
    // The rule: a MWMissilePath actor we have never seen before IS a launch (a path that
    // winks out and returns is the same pointer, so it cannot fake one - which is what the
    // old salvo-COUNT edge did, opening a bind window six times in five seconds). If a
    // launch locked on us happens on the same scan a round first appears, that round belongs
    // to it. Everything is pointer-set work over a handful of paths and one already-running
    // projectile loop: no per-round property lookups, so it stays clear of the hot path.
    // Path actors and the probe index at which each was last seen ALIVE. Not a plain
    // present/absent set: enumeration flickers hard (pathN observed swinging 2 -> 22 -> 2
    // between samples), and a set rebuilt every probe turns each reappearance into a launch
    // - which is precisely the jitter the pointer scheme was supposed to remove, reappearing
    // one level down. A path has to be gone for kPathForgetProbes consecutive probes before
    // we forget it, so a flicker cannot re-arm while a genuinely recycled actor still can.
    struct PathRec {
        uint32_t LastSeen;     // probe index this path was last enumerated
        bool     CountedOnMe;  // already produced a launch while locked onto us
    };
    std::unordered_map<void*, PathRec> PathSeen;
    uint32_t ProbeSeq = 0;
    static constexpr uint32_t kPathForgetProbes = 8; // ~260 ms at the 30 Hz projectile tick
    std::unordered_set<void*> PrevProjPtrs; // projectile actors alive at the last scan
    std::unordered_set<void*> MinePtrs;     // rounds attributed to a salvo guiding on US
    // Landed detection for attributed rounds ONLY. A pooled projectile lingers after impact
    // still enumerated and with neither bActorIsBeingDestroyed nor bHidden set - which is
    // exactly why the renderer has carried its own DeadProj stationary test all along. The
    // engine flags alone therefore retire a round far too late (mine= was observed ratcheting
    // to 47 and holding), and a stale entry whose pooled actor is re-fired hands its
    // attribution straight to a round aimed at somebody else.
    //
    // Tracked only for rounds in MinePtrs - a handful, not the ~250 airborne - so this is one
    // small map, not a per-round position history.
    //
    // Thresholds deliberately match LidarView's DeadProj (kStillEpsCm / kDeadFrames) so the
    // two sides agree on what "landed" means instead of one compensating for the other.
    // --- projectile identity -------------------------------------------------------------
    // ASSIGNED ids, not a pointer hash. The id used to be (pointer>>4)&0xFFFF, which is not
    // an identity at all: it is a 16-bit hash, and expected collisions among n live rounds
    // are n^2/2m. At the ~250 rounds we had been logging that is ~0.5 pairs per frame; at the
    // ~500 seen in a large battle it is ~1.9, so two unrelated missiles almost always shared
    // an id. Everything the renderer tracks between publishes is keyed on it - trails,
    // streaks, and the ProjStill stationary counter - so a collision merged two missiles'
    // histories: trails zig-zagged between two flight paths, and a shared ProjStill counter
    // marked a LIVE round dead whenever its partner was stationary, hiding it for a publish.
    // That is the blinking and teleporting seen on the scope.
    //
    // Here an id is owned by an actor for as long as it is alive and is only reused after it
    // is gone, so collisions between live rounds are impossible rather than merely unlikely.
    //
    // CLAUDE.md warns off an IdMap for projectiles because it "would grow unboundedly with
    // projectile churn". That was about a map that only ever grows; this one is pruned to the
    // live set every scan, so it is bounded by rounds in the air - a set we already iterate.
    //
    // NOT FIXED by this, and worth knowing: pool HANDOVER. If a round impacts and the engine
    // immediately reissues its actor, the pointer never leaves the live set, so the new round
    // inherits the dead one's id. Closing that needs reliable end-of-round detection, which
    // is a separate problem (see the note on MinePtrs retirement).
    std::unordered_map<void*, uint16_t> ProjIdMap;  // live actor -> its id, stable for its life
    // Ids that are unavailable: those held by a live round, PLUS those in quarantine below.
    std::unordered_set<uint16_t>        ProjIdUsed;
    uint16_t NextProjId = 1;                        // rolling cursor; 0 is reserved as "none"

    // ID QUARANTINE - one half of a CROSS-BINARY INVARIANT.
    //
    // Releasing an id the instant its round dies is not enough, because the renderer keeps
    // keying on it after the round is gone: a smoke trail survives its missile by
    // LidarView::kTrailRetainMs (1000 ms) so that a round bumped out of a truncated array
    // does not lose its trail. Hand that id straight to a new round and the new round
    // inherits the dead one's trail - drawn with a smoke line leading back to where it never
    // flew. Recycling ids actively (rather than by pointer reuse, as before) made that
    // likely rather than rare.
    //
    // So a freed id waits here before it can be reassigned. THE RULE: this must stay LONGER
    // than every downstream memory of an id. The trail is the binding one at 1000 ms;
    // everything else the renderer keys on the id (ProjPrev, ProjStreakFrom, ProjStill) is
    // rebuilt or pruned every publish. If LidarView::kTrailRetainMs ever grows, this must
    // grow first.
    //
    // Budget is not a concern: 512 live rounds plus whatever dies during the quarantine,
    // against 65535 ids. Even at an implausible 500 deaths/second that is ~1250 held, under
    // 2% of the space.
    struct FreedId { uint16_t Id; ULONGLONG FreedAtMs; };
    std::deque<FreedId> ProjIdQuarantine;
    static constexpr ULONGLONG kIdQuarantineMs = 1500;

    std::unordered_map<void*, FVector3f> MineLastPos;
    std::unordered_map<void*, int>       MineStill;
    static constexpr float kMineStillCm    = 100.0f; // < 1 m moved since the last scan
    static constexpr int   kMineStillScans = 3;      // consecutive scans before it counts
    // Last projectile scan, for gap detection. Anything longer than this and every pointer
    // we hold is suspect - see the note at the top of the scan.
    double LastProjScanMs = 0.0;
    static constexpr double kProjGapMs = 250.0;
    // Set by the path probe, consumed by the projectile scan later in the same tick.
    //
    // A COUNTDOWN, not a bool. Requiring the launch and the round's first appearance on the
    // very same scan lost half of them: the field log showed `newOnMe=1 ... mine=0` on 50%
    // of the samples where something was locked onto us - the MWMissilePath actor exists
    // before its munition shows up in the projectile array, so the round arrives on a later
    // scan and no longer counts as new.
    //
    // ONE SCAN OF BUFFER, and no more: the scan the launch was seen on, plus the next one.
    // ~66 ms at 30 Hz.
    //
    // The window admits EVERY round that spawns while it is open, so its width is directly
    // how much unrelated missile traffic can be swept in. Widening it to cover a rare late
    // arrival costs false attribution on every launch, which is the failure this design
    // exists to prevent - so the buffer covers the ordinary one-scan lag between a path
    // actor being created and its munition reaching the projectile array, and nothing else.
    //
    // If a munition ever lags its path by two or more scans, that salvo is simply missed.
    // The signature in the log is `lockMe>0 ... mine=0` with a preceding `newOnMe>0`; if
    // that reappears at any rate, the lag is longer than one scan and this is the constant
    // to revisit - but revisit it with a measurement, not by widening it speculatively.
    // SPATIAL TIEBREAKER. A launch is remembered with the POSITION of the path that made it,
    // so a newly spawned round can be matched to the salvo it actually came out of instead of
    // to whatever launch happened to be open.
    //
    // The blunt version this replaces admitted EVERY new missile round while any launch on us
    // was live. That is fine when launches are rare and ruinous in a sustained missile fight,
    // where launches occur on nearly every scan and the window is effectively never shut - so
    // ally-targeted salvos were flagged as inbound on us. It also could not see the commonest
    // case at all: a salvo aimed at a lancemate whose path appeared a scan or two ago is not
    // "new", so the mixed-tick check never fired for it, yet its rounds were still swept in.
    //
    // Matching is nearest-path in 3D at the moment the round appears - a round is sitting on
    // its launcher then, so the discrimination is between launcher positions, not trajectories.
    // No trajectory inference, no thresholds to tune beyond one sanity distance.
    // A LIVE VIEW, rebuilt from scratch every probe - not a list of snapshots with expiry
    // timers. Every path currently enumerated is a candidate, carrying its position and
    // whether its lock resolves to us RIGHT NOW.
    //
    // This replaced a 2-scan countdown, which was too short: a rack that ripple-fires spawns
    // its rounds over far longer than 66 ms under a SINGLE path, so the tail of every such
    // salvo went unattributed and unmarked. (Chain-fire was never affected - each chain-fired
    // missile creates its own path, so it brings its own launch with it.)
    //
    // Rebuilding rather than widening a timer is what bounds the stale-launch risk by REALITY
    // instead of by a guessed constant: a launch stops being a match candidate the moment its
    // path stops being enumerated or stops reporting us, rather than when an arbitrary
    // countdown expires. See kAmbiguousMatch for the case it cannot resolve.
    struct PendingLaunch {
        FVector3f Pos;   // where the path actor is
        bool      OnMe;  // its GetLockedTarget resolves to our pawn, as of this probe
    };
    std::vector<PendingLaunch> PendingLaunches;

    // Sanity bound on a match. If the nearest launch is further than this from a spawning
    // round, the geometry is not telling us what we think it is and we do NOT trust it - see
    // the fallback at the match site.
    //
    // UNRESOLVED, and the reason for the fallback: CLAUDE.md says a MWMissilePath is a
    // "fixed-point path descriptor (impact / launch point)" in one sentence and "pathDist
    // reads the launcher, not the impact" in the next. If it is the launcher, spawning rounds
    // sit within metres of it and this threshold is generous. If it is the impact point,
    // matches will be hundreds of metres out, every match will fail this bound, and we fall
    // back to previous behaviour rather than attributing nonsense. launch-match logging says
    // which world we are in.
    // 60 m, and this is MEASURED rather than reasoned. Field logging of the match distance
    // (launch-match: near=) showed correct matches landing at 16-28 m, not the "few metres"
    // both earlier guesses assumed: attribution happens on a round's first PUBLISHED
    // appearance, up to two scans after it spawns, which is ~10 m of travel at LRM speed, on
    // top of the offset between a mech's origin and its launch tubes.
    //
    // 20 m sat inside that distribution and rejected about half of all CORRECT matches. 60 m
    // clears the observed range with room for faster munitions and a slower frame. Wrong
    // matches in the same logs were 1100-1350 m out, so the two populations are separated by
    // more than an order of magnitude - the bound does not need to be finely tuned, it needs
    // to be on the right side of a very large gap.
    static constexpr float kLaunchMatchCm = 6000.0f; // 60 m
    int LastMatchFallbacks = 0;
    int LastAmbiguous      = 0;
    unsigned PoseFailAll   = 0;  // paths with no readable position, cumulative
    unsigned PoseFailOnMe  = 0;  // of those, ones locked onto us - a lost warning

    // AMBIGUOUS MATCH. Holding launches for as long as their paths live means a single mech
    // can have two live paths at once - one still locked on us from a salvo already away, and
    // a new one locked on a lancemate. They sit metres apart, so "nearest" between them is a
    // coin flip, and half of those flips would flag an ally's salvo as inbound.
    //
    // So when the nearest launch ON us and the nearest launch on SOMEONE ELSE are both inside
    // the match radius, we do not guess: the round is not attributed. Silence for the case we
    // genuinely cannot separate, rather than a 50% chance of lying about it.
    static constexpr float kAmbiguousMatchCm = 6000.0f; // same radius as a valid match
    // NOTE there is no mixed-tick policy constant any more. It decided what to do when one
    // scan carried launches at us AND at others, because nothing could tell their rounds
    // apart. The spatial match now can, per round, so the question it answered no longer
    // exists - and it was the last path by which an ally-targeted salvo could be attributed.
    int LastNewPathsOnMe = 0;   // diagnostics for the threat: line
    int LastNewPathsOther = 0;
    int LastMineCount     = 0;
    int LastOwnSkipped    = 0;  // own outgoing rounds refused attribution, last scan
    int LastProjDropped   = 0;  // rounds the array cap cost us, last scan
    int LastProjEvicted   = 0;  // flagged rounds that displaced an unflagged one
    // CUMULATIVE session totals. newOnMe/hold are instantaneous values sampled by a 2 s log
    // throttle, so at 30 Hz they read 0 on ~97% of samples even while firing correctly -
    // which is exactly why a build that attributed NOTHING all mission looked the same in
    // the log as one that was working. Totals cannot hide that.
    unsigned TotalLaunchOnMe  = 0;  // launches detected against us this session
    unsigned TotalAttributed  = 0;  // rounds ever attributed this session

    // --- MWMissilePath lock probe state -------------------------------------------------
    // Runs on its own ~10 Hz cadence, independent of the projectile array scan, so the
    // renderer can have the threat signal on every MFD page without paying for the array.
    // Published verbatim: no hold, no latch. See LidarShared.h.
    static constexpr ULONGLONG kThreatIntervalMs = 100;
    uint8_t ThreatFlagsNow  = 0;  // LTHREAT_* as of the last probe
    uint8_t ThreatSalvosNow = 0;  // salvos locked on us as of the last probe
    // NO ARROW IV DETECTION LIVES HERE, and none should be added without new evidence.
    // Four independent routes were built and all four are dead ends, each confirmed from
    // field logs rather than inferred:
    //   1. MWMissilePath - Arrow IV creates none (arrow>0 observed with pathN=0), so the
    //      LRM lock probe below can never enumerate it.
    //   2. ProjectileMovementComponent - bIsHomingProjectile reads 0 and
    //      HomingTargetComponent is an unset TWeakObjectPtr (ObjectIndex -1) on 79 of 79
    //      sampled rounds, despite the round visibly re-aiming at a walking mech.
    //   3. TargetSharingActor on the round - null on every round sampled.
    //   4. The pawn's own aim-location component - every name-matched property on the pawn
    //      is the wrong type or null. Note this only walked the pawn class's OWN properties,
    //      so it is "not found at this level", not proven absent.
    // A fifth route was left unexplored: Instigator resolves to the firing mech, which does
    // expose TargetTracking. It says "that mech is targeting you", not "that shell is
    // guiding on you" - a mech that fired at an ally and then turned would read as a false
    // positive - which is why it was not taken.
    int     LastPathCount   = 0;  // diagnostics for the periodic proj: line
    int     LastPathLocked  = 0;
    int     LastPathLockMe  = 0;

    // Allies: re-enumerated (fresh GAAC) only every ~100 ms into this value buffer, then
    // republished every fast tick. Value data only - no actor pointers held across ticks.
    LidarContact AllyBuf[LIDAR_MAX_ALLIES];
    uint32_t     AllyBufCount = 0;
    bool              HookReady     = false;
    float             Accumulator  = 0.0f;

    // THREADING: Gather() is reached from BOTH on_present (RENDER thread) and
    // on_pre_engine_tick (GAME thread). Enumeration (GetAllActorsOfClass / ProcessEvent)
    // and the actor reads MUST run on the game thread only - doing them on the render
    // thread races the game thread spawning/streaming actors (worst during mission load),
    // which reads a resizing actor list mid-realloc → EXCEPTION_ACCESS_VIOLATION in a
    // memcpy at a page-aligned address, with a recursive ProcessEvent/ModLoader stack.
    // - GatherActive: hard guard so Gather can never run re-entrantly or on two threads
    //   at once (the throttle statics were non-atomic and raced).
    // - GameThreadTickAlive: once a game-thread driver is confirmed firing, on_present
    //   stops calling Gather entirely, so all reflection runs on the game thread.
    // In THIS UEVR setup the SDK on_pre_engine_tick callback is NOT delivered to us
    // (confirmed: its log line never appears), so we instead drive Gather from a Blueprint
    // UFunction detour - OnCalculateHeadAim on the mech cockpit - which the game calls
    // every frame ON THE GAME THREAD while piloting (same mechanism HeadAim.dll relies on).
    // AddEventHook overwrites the UFunction's native pointer; our detour runs on the game
    // thread, calls Gather, then chains the original (so HeadAim / the game still run).
    std::atomic<bool> GatherActive{false};
    std::atomic<bool> GameThreadTickAlive{false}; // set by whichever game-thread driver fires
    bool              BpTickInstalled = false;   // at least one game-thread hook installed?
    API::UObject*     DriverPawn      = nullptr; // last pawn we (re)armed the driver for
    const void*       HookedCockpitClass = nullptr; // cockpit class our ReceiveTick hook is bound to
    ULONGLONG         BpInstallTick   = 0;        // when we installed (grace before fallback)
    // We install BOTH candidate hooks and chain each one's own original, so game-thread
    // execution happens if EITHER fires - ReceiveTick (always, if the mech BP ticks) or
    // OnCalculateHeadAim (only when head aim is active). Whichever the game calls first
    // flips GameThreadTickAlive; the other is harmless (guard + throttle absorb it).
    BP_FUNC           ReceiveTickOrig = nullptr;  // (unused since tickhook1 - see TickHooks)
    // TICK HOOKS (tickhook1, user 2026-10-17) - as LanceControl's / CameraCapture's game-thread driver: each cockpit
    // ReceiveTick UFunction is hooked ONCE and never unhooked, its original kept beside it and looked up by the function
    // actually running (FFrame::Node). The old way (one PluginExtension hook keyed by NAME, removed and re-added on a
    // chassis change) re-hooked the SAME function when the new cockpit INHERITS its tick (Corsair -> Rifleman takeover):
    // the remove was refused (another plugin hooked on top), the re-add saved that plugin's detour as our "original"
    // while it chained back to us - an endless loop, a stack-overflow crash. Gather runs only for the player's cockpit.
    struct TickHook
    {
        API::UFunction* Fn;
        BP_FUNC         Orig;
    };
    std::vector<TickHook> TickHooks;
    std::mutex            TickMx;
    BP_FUNC           HeadAimOrig     = nullptr;  // chained original of the cockpit OnCalculateHeadAim

    // Refresh rate for the export (seconds). ~10 Hz is plenty for a tactical scan.
    static constexpr float kInterval = 0.1f;
    // Scan range in cm (1000 m).
    static constexpr float kRangeCm  = 100000.0f;

    // ---- THE 5 KM RADAR MODE (2026-10-15, user) -------------------------------------------------------------------
    // HUDDisplay's "Vanilla, 5 km" radar mode sets LIDAR_CTRL_FAR and bumps ControlPad every poll. While both hold,
    // contacts, allies, rounds and buildings are kept out to LIDAR_FAR_RANGE_CM (5 km) instead of 1 km / 1.5 km. A set
    // bit whose heartbeat has not moved for 5 s is ignored, so a renderer that died never leaves the far ranges on.
    // Rounds past 1 km are kept only when fired by an enemy the lance detects (MW5_LanceStatus Units[] flags, the same
    // rules as the Vanilla radar mode) or within 500 m of a lancemate (the 'in a lancemate's sight' stand-in: no traces).
    bool      FarOn     = false;
    uint16_t  FarBeat   = 0;
    ULONGLONG FarBeatMs = 0;
    float RangeNowCm() const { return FarOn ? LIDAR_FAR_RANGE_CM : kRangeCm; }
    void UpdateFar(ULONGLONG now) {
        const bool bit = Block && (Block->ControlLayers & LIDAR_CTRL_FAR) != 0;
        const uint16_t beat = Block ? Block->ControlPad : 0;
        if (beat != FarBeat) { FarBeat = beat; FarBeatMs = now; }
        const bool on = bit && FarBeatMs && now - FarBeatMs < 5000;
        if (on != FarOn) {
            FarOn = on;
            LogInfo("far ranges %s (the 5 km radar mode)", on ? "ON - 5 km" : "off - 1 km");
        }
    }
    HANDLE                         FarLsMmf   = nullptr;
    const LanceStatusBlock*        FarLs      = nullptr;
    ULONGLONG                      FarLsTryMs = 0, FarDetMs = 0;
    std::unordered_set<void*>      FarDetected;        // live enemies the lance detects now
    std::unordered_map<void*, void*> FarInst;          // round -> its Instigator (never changes)
    void FarRefreshDetected(ULONGLONG now) {
        if (now - FarDetMs < 500) return;
        FarDetMs = now;
        if (!FarLs && now - FarLsTryMs > 5000) {
            FarLsTryMs = now;
            if (!FarLsMmf) FarLsMmf = OpenFileMappingW(FILE_MAP_READ, FALSE, LANCE_MMF_NAME);
            if (FarLsMmf)
                FarLs = reinterpret_cast<const LanceStatusBlock*>(MapViewOfFile(FarLsMmf, FILE_MAP_READ, 0, 0, sizeof(LanceStatusBlock)));
        }
        if (!FarLs || FarLs->Version != LANCE_VERSION) return; // keep the last set
        for (int tries = 0; tries < 4; ++tries) {
            const uint32_t a = FarLs->UnitSeq;
            if (a & 1) continue;
            MemoryBarrier();
            std::unordered_set<void*> det;
            uint32_t n = FarLs->UnitCount;
            if (n > (uint32_t)kLsMaxUnits) n = kLsMaxUnits;
            for (uint32_t i = 0; i < n; ++i) {
                const LsUnit& u = FarLs->Units[i];
                if (u.Dead || !(u.SeenBy || (u.Flags & (LSU_OnLanceRadar | LSU_OnPlayerRadar)))) continue;
                det.insert(reinterpret_cast<void*>(((uint64_t)u.ActorHi << 32) | u.ActorLo));
            }
            MemoryBarrier();
            if (FarLs->UnitSeq == a) { FarDetected.swap(det); return; }
        }
    }
    // A round past 1 km in the 5 km mode: shown only if a detected enemy fired it, or it is within 500 m of a lancemate.
    bool FarRoundShown(API::UObject* p, const FVector3f& loc) {
        void* key = static_cast<void*>(p);
        auto it = FarInst.find(key);
        if (it == FarInst.end()) {
            void* inst = nullptr;
            if (const auto* ip = p->get_property_data<void*>(L"Instigator")) inst = *ip;
            if (FarInst.size() > 8192) FarInst.clear();
            it = FarInst.emplace(key, inst).first;
        }
        if (it->second && FarDetected.count(it->second)) return true;
        for (uint32_t k = 0; k < AllyBufCount; ++k) {
            const float ax = AllyBuf[k].X - loc.X, ay = AllyBuf[k].Y - loc.Y, az = AllyBuf[k].Z - loc.Z;
            if (ax * ax + ay * ay + az * az < 50000.0f * 50000.0f) return true;
        }
        return false;
    }
    // A contact more than this far BELOW the player is an underground phantom (300 m).
    static constexpr float kUndergroundCm = 30000.0f;

public:
    inline static LidarExport* Instance;

    LidarExport() {
        Instance                  = this;
        PluginExtension::Instance = this;
        Name                      = "LidarExport";
        Version                   = "0.1.0";
        VersionInt                = 10;
        // VersionCheckFnName / VersionPropertyName left unset on purpose: that
        // handshake is MWVR-specific. The base will log one harmless "not found"
        // and continue — we don't depend on MWVR's VR_Global.
    }

    void OnInitialize() override {
        // 1) Create the shared block.
        MmfHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       0, sizeof(LidarExportBlock), LIDAR_MMF_NAME);
        if (!MmfHandle) {
            LogError("CreateFileMapping failed (%lu)", GetLastError());
            return;
        }
        Block = static_cast<LidarExportBlock*>(
            MapViewOfFile(MmfHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LidarExportBlock)));
        if (!Block) {
            LogError("MapViewOfFile failed (%lu)", GetLastError());
            return;
        }
        ZeroMemory(Block, sizeof(LidarExportBlock));
        Block->Magic   = LIDAR_MAGIC;
        Block->Version = LIDAR_VERSION;
        LogInfo("MMF created, %zu bytes", sizeof(LidarExportBlock));
        // Build stamp: prints on startup regardless of which callbacks fire, so we can
        // confirm at a glance WHICH build is actually loaded (UEVR won't hot-reload).
        LogInfo("BUILD=lidarexport-wpngroup (GROUPED WEAPONS 2026-10-19: identical weapons - same name, part, flags - one entry with a Count, TERRAIN v15; read up to 48, publish 12) (lidarexport-quietlog2 (QUIET SLOW LINES 2026-10-18: one-by-one slow: lines only with [debug] verbose_log, hitches over 50 ms always; 60 s summary kept) (lidarexport-quietlog QUIET LOG 2026-10-18: periodic profiles / status lines only with GlobalSettings [debug] verbose_log = 1) (bugsweep2) (2026-10-18: the death latch is trusted only while the unit keeps being checked - a gap means the address may hold a new unit; terrain Generation seeded per game session) (bugsweep1 2026-10-17: a new world by controller / level / world clock resets the death latch, the terrain and every cache keyed by an address) exitfix1-gamethread-dualhook-relpose-proj512-threatblock-nobetty-noarrow-attrib14-terrain-skyread1-nosil1-identity1-deco1-short2-vehwpn10-art1-actor1-tidy1-cond1-rear1-eqguard1-turretpose1-deadflag1-dropship1-nostruct1-slowtimer1-dsclass1-unionfix1-compfix1-compfix2-projclass1-mechprof1-unitmemo1-unitmemo2-unitmemo3-rockdeco1-vwpnmemo1-nolayout1-canyondiag1-startfloor1-seamoffset1-fmttrunc1-nocanyondiag1-far5km1-objcheck1-ba4-tickhook1-loopguard1");

        // 1b) The terrain heightfield gets its own block: the LIDAR one above is frozen at
        //     15180 bytes because LidarDisplay.exe shares it, and a heightfield is ~100 KB.
        //     Non-fatal - a failure here costs the topo map, nothing else.
        SetupTerrainMmf();

        // 2) Activate the UEVR object hook so get_objects_by_class works.
        API::get()->param()->sdk->uobject_hook->activate();
        HookReady = true;
        LogInfo("UObject hook activated");
    }

    // Drive the gather from BOTH the engine tick AND the render-present callback.
    // In this MWVR/UEVR setup the engine-tick SDK callback may not reach us, but the
    // render-present one does (it's the callback the HUD-style plugins effectively
    // ride). Gather() self-throttles, so whichever callback UEVR delivers runs ~10 Hz.
    void on_present() override {
        static bool firstPresent = false;
        if (!firstPresent) {
            firstPresent = true;
            LogInfo("on_present fired (callbacks ARE reaching this plugin)");
        }
        // Re-arm the game-thread driver when the LOCAL PAWN changes (you died and possessed a
        // lancemate, entered a vehicle, etc.). The cockpit UFunction we hooked belongs to the OLD
        // mech; if the new mech's cockpit is a different class the old hook never fires again, and
        // because GameThreadTickAlive stayed latched true the render-thread fallback was also
        // disabled -> everything froze. Clearing both flags re-runs installation (and re-enables
        // the fallback in the gap); TryInstallGameThreadHook rebinds to the new cockpit class.
        {
            const auto curPawn = API::get()->get_local_pawn(0);
            if (curPawn != DriverPawn) {
                DriverPawn = curPawn;
                GameThreadTickAlive.store(false, std::memory_order_release);
                BpTickInstalled = false;
            }
        }
        // Render thread. Once a GAME-THREAD driver is live, do NOTHING here - all the
        // enumeration/ProcessEvent work belongs on the game thread (see threading note).
        if (GameThreadTickAlive.load(std::memory_order_acquire))
            return;
        if (!BpTickInstalled) {
            // Not hooked yet. Try to install (cheap; only succeeds once in a mech). If it
            // installs, return immediately so we DON'T enumerate on the render thread - the
            // game-thread hook fires next frame. Until then Gather() below only ever reads
            // the player pawn (out of a mech it early-returns), never the actor list.
            TryInstallGameThreadHook();
            if (BpTickInstalled) return;
        } else if (GetTickCount64() - BpInstallTick < 2000) {
            // Hooked but not fired yet - give the game-thread hook a moment before falling
            // back to the render thread (avoids a race window right after install).
            return;
        }
        // Fallback: either not in a mech yet, or the hook installed but never fired (e.g. a
        // hook-ordering conflict). No worse than before; the GatherActive guard prevents any
        // overlap with the hook, and the instant the hook fires we stop enumerating here.
        Gather();
    }

    void on_pre_engine_tick(API::UGameEngine* /*engine*/, const float delta) override {
        if (!GameThreadTickAlive.exchange(true))
            LogInfo("on_pre_engine_tick fired (delta=%.4f) - routing Gather to GAME thread", delta);
        Gather();
    }

    // Blueprint UFunction detour: runs ON THE GAME THREAD every frame the game calculates
    // head aim (while piloting). This is our real game-thread driver since the SDK engine
    // tick isn't delivered here. Do the whole scan here, then chain the original.
    // Shared body: run the scan on the GAME thread (this is why we hook a game-thread
    // UFunction at all). Guarded/throttled inside Gather, so being called from two hooks
    // per frame is harmless. Each detour below calls this then chains ITS OWN original.
    static void RunGatherOnGameThread() {
        if (!Instance) return;
        if (!Instance->GameThreadTickAlive.exchange(true))
            Instance->LogInfo("GAME-THREAD tick hook FIRED - Gather now runs on the GAME thread");
        Instance->Gather();
    }

    // The local player's cockpit actor (the one whose tick drives us), or nullptr - as LanceControl's LocalCockpit.
    static API::UObject* LocalCockpit() {
        auto* pawn = API::get()->get_local_pawn(0);
        if (!pawn) return nullptr;
        auto* meshPP = pawn->get_property_data<void*>(L"MechMeshComponent");
        if (!meshPP || !*meshPP) return nullptr;
        auto* cockPP = reinterpret_cast<API::UObject*>(*meshPP)->get_property_data<void*>(L"FirstPersonCockpitComponent");
        if (!cockPP || !*cockPP) return nullptr;
        auto* childPP = reinterpret_cast<API::UObject*>(*cockPP)->get_property_data<void*>(L"ChildActor");
        return (childPP && *childPP) ? reinterpret_cast<API::UObject*>(*childPP) : nullptr;
    }

    // tickhook1: the original of the function actually running, then (only for the player's cockpit) the gather.
    static void* OnReceiveTickDetour(API::UObject* obj, FFrame* frame, void* const result) {
        BP_FUNC orig = nullptr;
        if (Instance) {
            std::lock_guard<std::mutex> lk(Instance->TickMx);
            for (auto& h : Instance->TickHooks)
                if (frame && h.Fn == frame->Node)
                    orig = h.Orig;
            if (!orig && !Instance->TickHooks.empty())
                orig = Instance->TickHooks.back().Orig; // (never expected: Node is the running function)
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
                API::get()->log_error("[LidarExport] game-thread driver: the cockpit tick chain loops back into LidarExport - cut");
            }
            return nullptr;
        }
        ++loopDepth;
        void* r = orig ? orig(obj, frame, result) : nullptr; // the cockpit (and anyone hooked before us) first
        --loopDepth;
        if (Instance && obj && obj == LocalCockpit())
            RunGatherOnGameThread();
        return r;
    }

    static void* OnHeadAimDetour(API::UObject* obj, FFrame* frame, void* const result) {
        RunGatherOnGameThread();
        if (Instance && Instance->HeadAimOrig)       // chain HeadAim / the game's head-aim logic
            return Instance->HeadAimOrig(obj, frame, result);
        return nullptr;
    }

    // Dump a class's UFunctions (own + inherited) whose name looks like a per-frame driver,
    // so we can SEE what's actually hookable rather than guessing (reference-first). Called
    // once when we enter a mech. get_children() is UFunctions; walk the super chain since
    // ReceiveTick lives up on AActor, not on DerivedMech_C itself.
    void DumpHookCandidates(API::UStruct* cls, const char* label) {
        if (!cls) return;
        for (API::UStruct* s = cls; s; s = s->get_super_struct()) {
            for (API::UField* f = s->get_children(); f; f = f->get_next()) {
                std::wstring n = f->get_fname()->to_string();
                std::wstring l = n;
                for (auto& ch : l) ch = (wchar_t)towlower(ch);
                if (l.find(L"tick") != std::wstring::npos || l.find(L"update") != std::wstring::npos ||
                    l.find(L"aim")  != std::wstring::npos || l.find(L"receive") != std::wstring::npos)
                    LogInfo("hook-candidate [%s]: %s", label, WideToNarrow(n.c_str()).c_str());
            }
        }
    }


    // Install a GAME-THREAD driver by hooking a UFunction the game calls every frame while
    // piloting. Priority: the mech pawn's ReceiveTick (Actor Event Tick - fires regardless
    // of head-aim settings), then the cockpit's OnCalculateHeadAim (only fires when head aim
    // is active, so a weaker fallback). Our detour chains the original either way, so the
    // mech ticks / head aim runs normally. Cheap raw property reads, safe to poll each frame.
    void TryInstallGameThreadHook() {
        const auto pawn = API::get()->get_local_pawn(0);
        if (!pawn) return;
        auto* meshPP = pawn->get_property_data<void*>(L"MechMeshComponent");
        if (!meshPP || !*meshPP) return;                       // not a mech pawn yet

        // One-time visibility into what per-frame functions exist on the pawn + cockpit.
        static bool dumped = false;
        API::UObject* mechCockpit = nullptr;
        {
            auto* mesh   = reinterpret_cast<API::UObject*>(*meshPP);
            auto* cockPP = mesh->get_property_data<void*>(L"FirstPersonCockpitComponent");
            if (cockPP && *cockPP) {
                auto* cockpitC = reinterpret_cast<API::UObject*>(*cockPP);
                auto* childPP  = cockpitC->get_property_data<void*>(L"ChildActor");
                if (childPP && *childPP) mechCockpit = reinterpret_cast<API::UObject*>(*childPP);
            }
            if (!dumped) {
                dumped = true;
                DumpHookCandidates(pawn->get_class(), "pawn");
                if (mechCockpit) DumpHookCandidates(mechCockpit->get_class(), "cockpit");
            }
        }

        // Install BOTH drivers so game-thread execution happens if EITHER fires:
        //  - ReceiveTick on the pawn: fires every frame the mech ticks, regardless of head
        //    aim (the primary, head-aim-independent driver).
        //  - OnCalculateHeadAim on the cockpit: fires only when head aim is active (backup;
        //    also covers the case where the mech BP has no own Event Tick).
        // Whichever the game calls flips GameThreadTickAlive; the guard/throttle in Gather
        // makes a second call the same frame a cheap no-op. We only need one to fire.
        // NOTE: the pawn (DerivedMech_C) has NO own ReceiveTick - find_function would return
        // the inherited AActor stub, which the mech may never call. The candidate dump showed
        // the COCKPIT actor has its own ReceiveTick (it updates heat/damage/HUD every frame),
        // so hook ReceiveTick THERE - a true per-frame game-thread driver, head-aim-independent.
        auto* cockCls = mechCockpit ? mechCockpit->get_class() : nullptr;
        if (!cockCls) return;

        // Same cockpit class as we're already hooked on (e.g. re-entering an identical chassis):
        // the class-level UFunction hook is still live and fires for the new instance. Just mark
        // installed + refresh the grace window; do NOT re-hook — re-hooking the same UFunction
        // would capture our OWN detour as the "original" and recurse infinitely.
        if (static_cast<const void*>(cockCls) == HookedCockpitClass) {
            BpTickInstalled = true;
            BpInstallTick   = GetTickCount64();
            return;
        }

        // Cockpit class CHANGED (possessed a different chassis). Drop our old-class ReceiveTick
        // hook (restores that class's pointer, frees the map key) and bind ReceiveTick on the new
        // class. Leave OnCalculateHeadAim alone on a swap — ReceiveTick is the confirmed driver and
        // re-touching HeadAim's shared chain is riskier than it's worth.
        // tickhook1: the new cockpit's ReceiveTick hooked once and kept - or, when it is a function we already sit on
        // (an inherited tick, or a chassis driven before), simply reused. Nothing is ever unhooked or hooked twice.
        bool any = false;
        if (API::UFunction* fn = cockCls->find_function(L"ReceiveTick")) {
            std::lock_guard<std::mutex> lk(TickMx);
            bool have = false;
            for (auto& h : TickHooks)
                have |= (h.Fn == fn);
            const std::string cn = WideToNarrow(static_cast<API::UStruct*>(cockCls)->get_fname()->to_string().c_str());
            if (have) {
                LogInfo("GAME-THREAD hook: %s::ReceiveTick (%p) already hooked - reused (inherited or seen before)", cn.c_str(), (void*)fn);
            } else {
                auto** slot = reinterpret_cast<BP_FUNC*>(reinterpret_cast<uintptr_t>(fn) + NATIVE_FUNCTION_OFFSET);
                TickHooks.push_back({fn, *slot});
                *slot = &OnReceiveTickDetour;
                LogInfo("GAME-THREAD hook installed on %s::ReceiveTick (%p) - kept for the session (%u hooked)", cn.c_str(), (void*)fn,
                        (unsigned)TickHooks.size());
            }
            any = true;
        }
        if (!HookedCockpitClass && AddEventHook(cockCls, L"OnCalculateHeadAim",
                                                &OnHeadAimDetour, &HeadAimOrig)) {
            LogInfo("GAME-THREAD hook installed on OnCalculateHeadAim (cockpit)");
            any = true;
        }
        if (any) {
            BpTickInstalled    = true;
            BpInstallTick      = GetTickCount64();
            HookedCockpitClass = static_cast<const void*>(cockCls);
            LogInfo("game-thread driver(s) installed - enumeration will move off the render thread");
        }
    }

    // Enumerate the ObjectProperty fields on the TargetTracking component that hold a
    // *live* target (current enemy or ally). We keep names containing "target" but
    // drop history/pending/override slots so we don't highlight stale picks. Every
    // target-ish field is logged once so we can confirm the ally slot's real name.
    void CollectTargetFields(API::UObject* comp) {
        auto* cls = comp->get_class();
        if (!cls) return;
        auto lower = [](std::wstring s) {
            for (auto& ch : s) ch = (wchar_t)towlower(ch);
            return s;
        };
        for (API::UStruct* s = cls; s; s = s->get_super_struct()) {
            for (API::FField* f = s->get_child_properties(); f; f = f->get_next()) {
                const std::wstring name = f->get_fname()->to_string();
                const std::wstring ty   = f->get_class() ? f->get_class()->get_name() : L"";
                const std::wstring l    = lower(name);
                if (l.find(L"target") == std::wstring::npos) continue;
                const bool stale = l.find(L"previous") != std::wstring::npos
                                || l.find(L"pending")  != std::wstring::npos
                                || l.find(L"override") != std::wstring::npos
                                || l.find(L"last")     != std::wstring::npos;
                // Only raw ObjectProperty slots are safe to read as a live pointer.
                if (ty == L"ObjectProperty" && !stale) TargetFields.push_back(name);
            }
        }
        LogInfo("TargetTracking: %zu live target field(s)", TargetFields.size());
    }

    // Resolve the native MWTank / MWTurret / MWVTOL base classes from a live instance
    // (find_uobject on these returns null here, same as MWMech). Gated on is_a(Pawn)
    // so we only chain-walk the handful of pawns, not all ~400k objects. Retries every
    // 2 s until all three are found (they may spawn after the mission starts), then
    // never scans again.
    void ResolveVehicleClasses() {
        if (!PawnClass) return;
        if (TankClass && TurretClass && VtolClass && BaClass) return;
        static ULONGLONG lastTry = 0;
        const ULONGLONG now = GetTickCount64();
        // ba1: battle armour is in few missions - once the three vehicle bases are known, look for it every 10 s, not 2 s
        if (now - lastTry < ((TankClass && TurretClass && VtolClass) ? 10000u : 2000u)) return;
        lastTry = now;

        auto*      sdk   = API::get()->param()->sdk;
        const auto arr   = sdk->functions->get_uobject_array();
        const int  total = arr ? sdk->uobject_array->get_object_count(arr) : 0;
        for (int i = 0; i < total; ++i) {
            if (TankClass && TurretClass && VtolClass && BaClass) break;
            const auto obj = sdk->uobject_array->get_object(arr, i);
            if (!obj || !sdk->uobject->is_a(obj, PawnClass))
                continue;
            auto* o = reinterpret_cast<API::UObject*>(obj);
            for (API::UStruct* st = o->get_class(); st; st = st->get_super_struct()) {
                const std::wstring n = st->get_fname()->to_string();
                const auto h = reinterpret_cast<UEVR_UClassHandle>(st);
                if (!TankClass && n == L"MWTank")        { TankClass   = h; LogInfo("resolved MWTank base"); }
                else if (!TurretClass && n == L"MWTurret") { TurretClass = h; LogInfo("resolved MWTurret base"); }
                else if (!VtolClass && n == L"MWVTOL")     { VtolClass   = h; LogInfo("resolved MWVTOL base"); }
                else if (!BaClass && n == L"KelClanElemental") { BaClass = h; LogInfo("resolved KelClanElemental base (battle armour)"); }
                if (n == L"Pawn") break; // don't walk above Pawn
            }
        }
    }

    // Resolve MWProjectile + MWMissilePath from live instances (same trick; discovery
    // confirmed these bases). Gated on is_a(Actor) so we chain-walk only actors, and
    // only retried while the renderer has projectiles enabled and combat is producing
    // them. Retries every 1 s until both are found, then stops.
    void ResolveProjectileClasses() {
        if (!ActorClass) return;
        if (ProjClass && MissileClass) return;
        static ULONGLONG lastTry = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastTry < 1000) return;
        lastTry = now;

        auto*      sdk   = API::get()->param()->sdk;
        const auto arr   = sdk->functions->get_uobject_array();
        const int  total = arr ? sdk->uobject_array->get_object_count(arr) : 0;
        for (int i = 0; i < total; ++i) {
            if (ProjClass && MissileClass) break;
            const auto obj = sdk->uobject_array->get_object(arr, i);
            if (!obj || !sdk->uobject->is_a(obj, ActorClass))
                continue;
            auto* o = reinterpret_cast<API::UObject*>(obj);
            for (API::UStruct* st = o->get_class(); st; st = st->get_super_struct()) {
                const std::wstring n = st->get_fname()->to_string();
                const auto h = reinterpret_cast<UEVR_UClassHandle>(st);
                if (!ProjClass && n == L"MWProjectile")            { ProjClass = h; LogInfo("resolved MWProjectile base"); }
                else if (!MissileClass && n == L"MWMissilePath")   { MissileClass = h; LogInfo("resolved MWMissilePath base"); }
                if (n == L"Actor") break; // don't walk above Actor
            }
        }
    }

    // =======================================================================================
    // TERRAIN TRACING - the foundation of the topographic map. One-shot probes for now; the
    // layout resolution and RunTrace below are written to be what the real tile cache uses.
    //
    // WHY TRACES. MW5 terrain is static meshes in streamed chunk sublevels - the game's own
    // config has zero Landscape/HeightmapTexture references and 169 StaticMesh ones - so there
    // is no heightmap to read. ~10,500 meshes do carry bAllowCPUAccess, so vertex data is
    // probably readable, but render data is NOT reflected: reading it means hand-derived
    // offsets with no get_offset() safety net, which is how this project produced two access
    // violations. And a triangle soup would have to be resampled onto a grid anyway, which is
    // exactly what a trace returns directly. Mesh reading is the fallback if trace cost bites.
    //
    // WHAT THE GAME CONFIG FIXED FOR US (DefaultEngine.ini, inside the game pak):
    //   * All custom channels are bTraceType=False, so ETraceTypeQuery has only the built-ins:
    //     0 = Visibility, 1 = Camera. There is nothing else to try.
    //   * Profile "Pawn" sets Visibility to ECR_Ignore - MECHS DO NOT BLOCK VISIBILITY. A
    //     trace from the mech's own origin passes through its legs and reaches the ground.
    //
    // WHAT THE FIELD PROBES ESTABLISHED:
    //   * The parameter frame is MEASURED, never assumed: UFunction lists its own parameters
    //     and FProperty::get_offset() gives each byte offset. Same for FHitResult's members.
    //   * Simple and complex collision returned identical results to the centimetre, so
    //     SIMPLE traces are sufficient - and they are the cheap ones.
    //   * Starting 50 m above the mech hit a Visibility blocker ~17 m up. Start just above the
    //     origin instead and trace down; nothing overhead can interfere.
    //   * The pawn origin is NOT at the feet (measured 6.34 m above ground), so nothing may
    //     assume ground == origin Z. Each sample returns its own absolute world Z.
    // =======================================================================================
    static constexpr bool kTerrainProbe = true;
    // The benchmark has done its job: 3.4-4.7 us per trace, 100% hit rate, and a 1024-trace
    // burst costs a 4.8 ms frame hitch. Keeping it would spend that hitch at the start of every
    // mission to re-learn a number we already have. Switch it back on only if the shape of the
    // trace changes (channel, complex collision, reach) and the cost needs re-establishing.
    static constexpr bool kTerrainTiming = false;
    bool TerrainProbeDone = false;
    bool TerrainTimingDone = false;

    struct TraceLayout {
        int32_t Frame = -1;
        int32_t World = -1, Start = -1, End = -1, Channel = -1, Complex = -1;
        int32_t DebugType = -1, Hit = -1, IgnoreSelf = -1, Ret = -1;
        int32_t HitBlocking = -1, HitLocation = -1, HitImpactPoint = -1, HitDistance = -1;
        // Offset of FHitResult::Actor. NOTHING READS THIS YET - it is measured only so the
        // startup log can say whether per-hit actor filtering (rejecting dropships, mechs and
        // vehicles as "ground") is possible in this build at all. Learning that from one log
        // line is cheaper than learning it halfway through building the filter.
        //
        // If it is ever used: this is a TWeakObjectPtr, {int32 ObjectIndex, int32 Serial}, NOT
        // a pointer. It resolves through uobject_array->get_object(arr, index). Dereferencing
        // the slot faults at 0x10000000f - see the note at the top of this file.
        int32_t HitActor = -1;
        // rockdeco1: offset of FHitResult::Component (same two shapes as Actor) - which mesh of a
        // decoration system the ray hit, so rock and cliff meshes can be told from trees.
        int32_t HitComp = -1;
    };
    bool TraceCompIsWeak = false;
    struct TraceResult { bool Blocking = false; float X = 0, Y = 0, Z = 0; float Dist = -1.0f; };

    TraceLayout          TL;
    bool                 TraceReady   = false;
    bool                 TraceChecked = false;
    API::UObject*        TraceCdo     = nullptr;
    API::UFunction*      TraceFn      = nullptr;
    std::vector<uint8_t> TraceFrame;   // reused: a per-trace heap allocation is not acceptable
                                       // in anything that will run thousands of times

    // Resolve and validate the call frame once. Returns false if anything is unmeasured, in
    // which case nothing ever calls ProcessEvent.
    bool EnsureTraceLayout() {
        if (TraceChecked) return TraceReady;
        TraceChecked = true;

        auto* ksl = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.KismetSystemLibrary");
        if (!ksl) { LogError("trace: KismetSystemLibrary not found"); return false; }
        TraceCdo = ksl->get_class_default_object();
        TraceFn  = ksl->find_function(L"LineTraceSingle");
        if (!TraceCdo || !TraceFn) { LogError("trace: cdo=%p fn=%p", (void*)TraceCdo, (void*)TraceFn); return false; }

        TL.Frame = TraceFn->get_properties_size();
        for (auto* f = TraceFn->get_child_properties(); f; f = f->get_next()) {
            auto* p = reinterpret_cast<API::FProperty*>(f);
            const std::wstring nm = f->get_fname()->to_string();
            const int32_t off = p->get_offset();
            if      (nm == L"WorldContextObject") TL.World      = off;
            else if (nm == L"Start")              TL.Start      = off;
            else if (nm == L"End")                TL.End        = off;
            else if (nm == L"TraceChannel")       TL.Channel    = off;
            else if (nm == L"bTraceComplex")      TL.Complex    = off;
            else if (nm == L"DrawDebugType")      TL.DebugType  = off;
            else if (nm == L"OutHit")             TL.Hit        = off;
            else if (nm == L"bIgnoreSelf")        TL.IgnoreSelf = off;
            else if (nm == L"ReturnValue")        TL.Ret        = off;
        }
        if (auto* hr = API::get()->find_uobject<API::UStruct>(L"ScriptStruct /Script/Engine.HitResult")) {
            for (auto* f = hr->get_child_properties(); f; f = f->get_next()) {
                auto* p = reinterpret_cast<API::FProperty*>(f);
                const std::wstring nm = f->get_fname()->to_string();
                const int32_t off = p->get_offset();
                if      (nm == L"bBlockingHit") TL.HitBlocking    = off;
                else if (nm == L"Location")     TL.HitLocation    = off;
                else if (nm == L"ImpactPoint")  TL.HitImpactPoint = off;
                else if (nm == L"Distance")     TL.HitDistance    = off;
                else if (nm == L"Actor") {
                    // Type decided by the property's CLASS, never by its name. A WeakObjectProperty
                    // holds {index, serial} and must be resolved through the object array;
                    // reading it as a pointer faults at 0x10000000f. If it is neither of the two
                    // shapes we understand, the offset is left unset and filtering stays off -
                    // guessing here would corrupt every height in the map.
                    const std::wstring pc = p->get_class() ? p->get_class()->get_name() : L"";
                    if (pc == L"WeakObjectProperty") { TL.HitActor = off; TraceActorIsWeak = true; }
                    else if (pc == L"ObjectProperty") { TL.HitActor = off; TraceActorIsWeak = false; }
                    else LogError("trace: FHitResult::Actor is '%ls', not an object property - "
                                  "actor filtering disabled", pc.c_str());
                }
                else if (nm == L"Component") { // rockdeco1
                    const std::wstring pc = p->get_class() ? p->get_class()->get_name() : L"";
                    if (pc == L"WeakObjectProperty") { TL.HitComp = off; TraceCompIsWeak = true; }
                    else if (pc == L"ObjectProperty") { TL.HitComp = off; TraceCompIsWeak = false; }
                    else LogError("trace: FHitResult::Component is '%ls' - rock/cliff decoration test disabled", pc.c_str());
                }
            }
        }

        const int32_t vecSize = (TL.Start >= 0 && TL.End >= 0) ? (TL.End - TL.Start) : -1;
        if (TL.Frame <= 0 || TL.World < 0 || TL.Start < 0 || TL.End < 0 || TL.Channel < 0 ||
            TL.Hit < 0 || TL.HitBlocking < 0 || (TL.HitLocation < 0 && TL.HitImpactPoint < 0)) {
            LogError("trace: incomplete layout - tracing disabled"); return false;
        }
        // UE4 FVector is three floats; UE5 is three doubles. Writing floats into a double frame
        // would aim the trace somewhere arbitrary and still return a plausible-looking number.
        if (vecSize != 12) { LogError("trace: Start->End gap %d, expected 12 - tracing disabled", vecSize); return false; }
        if (TL.Frame < 64 || TL.Frame > 4096) { LogError("trace: frame %d implausible - tracing disabled", TL.Frame); return false; }

        TraceFrame.assign(static_cast<size_t>(TL.Frame) + 64, 0);
        TraceReady = true;
        LogInfo("trace: ready (frame=%d, OutHit@%d, Location@%d) - simple Visibility traces",
                TL.Frame, TL.Hit, TL.HitLocation);
        // Answers "can we reject non-world hits at all?" before anyone spends a day on it.
        if (TL.HitActor >= 0)
            LogInfo("trace: FHitResult::Actor@%d (%s) - per-hit actor filtering is available",
                    TL.HitActor, TraceActorIsWeak ? "TWeakObjectPtr" : "object");
        else
            LogInfo("trace: FHitResult::Actor NOT FOUND - per-hit actor filtering is not possible "
                    "in this build; object-type filtering or a drop-zone re-trace would be the routes");
        return true;
    }

    // One trace, filled BY MEASURED OFFSET. The frame is fully zeroed each call so a trace that
    // writes nothing cannot be read as the previous trace's result.
    TraceResult RunTrace(API::UObject* pawn, const FVector3f& from, const FVector3f& to,
                         uint8_t channel = 0, bool complex = false) {
        TraceResult r{};
        if (!TraceReady || !pawn) return r;
        std::memset(TraceFrame.data(), 0, TraceFrame.size());

        void* wc = static_cast<void*>(pawn);
        std::memcpy(TraceFrame.data() + TL.World, &wc, sizeof(void*));
        const float s[3] = {from.X, from.Y, from.Z};
        const float e[3] = {to.X, to.Y, to.Z};
        std::memcpy(TraceFrame.data() + TL.Start, s, sizeof(s));
        std::memcpy(TraceFrame.data() + TL.End,   e, sizeof(e));
        TraceFrame[TL.Channel] = channel;
        if (TL.Complex    >= 0) TraceFrame[TL.Complex]    = complex ? 1 : 0;
        if (TL.DebugType  >= 0) TraceFrame[TL.DebugType]  = 0;
        if (TL.IgnoreSelf >= 0) TraceFrame[TL.IgnoreSelf] = 1;

        TraceCdo->process_event(TraceFn, TraceFrame.data());

        r.Blocking = TraceFrame[TL.Hit + TL.HitBlocking] != 0;
        const int32_t locOff = (TL.HitLocation >= 0) ? TL.HitLocation : TL.HitImpactPoint;
        float loc[3] = {0, 0, 0};
        std::memcpy(loc, TraceFrame.data() + TL.Hit + locOff, sizeof(loc));
        r.X = loc[0]; r.Y = loc[1]; r.Z = loc[2];
        if (TL.HitDistance >= 0) std::memcpy(&r.Dist, TraceFrame.data() + TL.Hit + TL.HitDistance, sizeof(r.Dist));
        return r;
    }

    // Ground height at a world XY, as absolute world Z. Starts well above so remote samples on
    // higher terrain are still reachable, and returns the TOPMOST blocking surface - which
    // includes buildings and canopy, deliberately: on a wireframe those read as structures.
    // =======================================================================================
    // GROUND HEIGHT AT A WORLD XY. Cast UPWARD from below, not downward from above.
    //
    // Three earlier revisions all failed the same way, and it took three to see it:
    //     50 m above the mech  -> hit a blocker 17 m up
    //      2 m above the origin -> holes wherever terrain rose more than 200 m
    //      1 km above           -> holes closed, and tiles came back with 1 km of "relief"
    //                              because samples were hitting a ceiling near the ray start
    // Each fix moved the start altitude. None of them addressed the actual defect: a downward
    // ray returns the TOPMOST blocking surface, and "topmost" is never what "ground" means.
    // Ceilings, roofs and canopy are above the ground by definition, so they always win.
    //
    // A rising ray cannot make that mistake. The first surface it meets is the ground's
    // underside, and everything that ruined the downward version sits above that point, so the
    // ray stops before reaching any of it. No reference altitude to choose, no reach ladder, no
    // neighbour tracking - the failure mode is designed out rather than tuned around.
    //
    // It also settles the buildings question for free: structures sit above the ground, so they
    // are simply never hit. If we later want them, a second downward ray gives building height
    // as the difference between the two.
    //
    // THE ASSUMPTION, AND THE FALLBACK. This relies on MW5's terrain collision blocking from
    // underneath. UE4 triangle meshes are two-sided by default and have no interior to start
    // inside of, so it should - but that is reasoning, and this project has been repeatedly
    // wrong when it reasoned instead of measuring. So if the upward ray finds nothing we fall
    // back to a downward one with a TIGHT window, which is the old behaviour and no worse. The
    // per-tile counters below report which path actually fired, so the log says plainly whether
    // the assumption held instead of leaving us to infer it from the shape of the terrain.
    // =======================================================================================
    uint32_t TerrUpHits = 0, TerrDownHits = 0, TerrMisses = 0;

    // Samples published from the BOOTSTRAP RAY'S OWN READING because the downward re-read found
    // nothing - the `outZ = zu` branch at the bottom of GroundAt.
    //
    // This is the one path that can publish an UNDERSIDE as ground, which is exactly the "hill
    // reads as a pit" failure this file's header describes. It was previously folded into
    // TerrUpHits alongside every healthy bootstrap, so the log could not distinguish "the
    // bootstrap fired and then corrected itself" from "the correction failed and we published
    // the underside anyway". Those are a normal event and a data fault respectively, and they
    // had the same number.
    //
    // If the flat square canyons are what I think they are, the tiles containing them will
    // report a large figure here and every other tile will report zero.
    uint32_t TerrUnderFallback = 0;

    // =======================================================================================
    // PER-HIT ACTOR FILTERING
    //
    // The height cache is PERMANENT and the things it traces are not. A dropship at the drop
    // point, a lance standing where you start, a parked vehicle - each is traced once in the
    // mission's first thirty seconds and becomes ground for the rest of it, including long
    // after it has walked away. That is a defect on its own terms, independent of whatever is
    // producing the flat-square canyons.
    //
    // So: read FHitResult::Actor, and if the thing we hit is a pawn or a munition, reject the
    // hit and keep tracing downward from just below it. Buildings are deliberately ACCEPTED
    // for now - see the note on kRejectBuildings.
    //
    // Cost. The check is ~20-50 ns against a 4.7 us trace, under 1%, because the verdict is
    // memoised by UClass* and is_a walks the super chain only on the first sighting of each
    // class. What actually costs is the RETRY - a whole extra trace - which is why retries are
    // charged against the fill loop's trace budget rather than added on top of it. A tile under
    // a dropship fills slower; no frame gets a spike.
    // =======================================================================================
    static constexpr bool kTerrActorFilter = true;

    // ---- THE STRUCTURES LAYER IS OFF ------------------------------------------------------
    //
    // Scrapped by decision, not by defect. The enumeration works and the boxes are accurate, but
    // inclusion is gated on MWDestructibleObject_C, so only DESTRUCTIBLE buildings are
    // candidates at all. Anything modelled as a plain StaticMeshActor - and that class takes
    // eleven thousand terrain ray hits on a typical map - is invisible to it. Completing the
    // layer meant separating scenery buildings from mesh-built terrain by asset name, with a
    // real chance the asset names carry no usable structure, and a half-populated building layer
    // is worse than none: it tells the pilot a clear area is clear when it is not.
    //
    // The MACHINERY stays, because the terrain repairs depend on it and they are worth keeping
    // on their own: a departed dropship and a destroyed building both leave their shape baked
    // into the heightfield, and both are found by this scan. Nothing is published and nothing is
    // drawn - StructureCount stays 0, so a consumer sees "scanned, none" rather than stale data.
    static constexpr bool kPublishStructures = false;

    // Buildings stay in the terrain mesh until there is somewhere better to put them. Rejecting
    // them today would trace past them to true ground, which is more correct - and would also
    // make every structure silently vanish from both maps, which is less useful than a lump.
    // Flip this the day the structures layer exists, not before.
    static constexpr bool kRejectBuildings = false;

    // Volumes: see the long note in VerdictFor. Left switchable rather than deleted, because the
    // mechanism is sound and only the classification was wrong.
    static constexpr bool kSoftRejectVolumes = false;

    // Clamp the no-hint descent to start above the player's own altitude. This is the canyon fix
    // and it is on; the flag exists so a single mission can prove it by being turned off.
    static constexpr bool kNoHintStartsAbovePlayer = true;

    // How many times a downward ray may be restarted below a rejected surface before the sample
    // is given up as a hole.
    //
    // Four was chosen for a dropship - hull, interior deck, a mech standing inside it - and that
    // reasoning did not survive foliage rejection. MWDecorationSystem is INSTANCED foliage, so a
    // ray through dense scrub hits canopy after canopy and can spend all four retries without
    // ever reaching dirt. The field log showed it plainly: the tiles with the most rejections
    // had the most give-ups, 345 and 34 on the same tile, while quiet tiles had almost none.
    // Eight costs a few extra traces only where the ray is already failing.
    static constexpr int kHitRejectRetries = 8;

    // FHitResult::Actor is a TWeakObjectPtr in UE4 ({int32 ObjectIndex, int32 Serial}) and a
    // raw-ish handle in UE5. Decided by the property's CLASS at layout time, never by its name
    // - a property's name has lied about its type twice in this plugin's history, and reading a
    // weak pointer as a pointer faults at 0x10000000f.
    bool TraceActorIsWeak = false;

    // MWDestructibleObject_C - the common base of every building on the map, discovered by the
    // census rather than guessed. Captured here the first time a ray lands on one, ready for the
    // structures layer to enumerate; nothing reads it yet.
    UEVR_UClassHandle DestructibleClass = nullptr;

    uint32_t TerrHitsRejected = 0;

    // Samples whose published height came from a VOLUME because nothing solid lay beneath it.
    // The number that decides whether volumes are load-bearing on a given map: near zero means
    // they were pure noise and could be refused outright; large means they ARE the floor there
    // and refusing them would have holed the map.
    uint32_t TerrVolumeFallback = 0;
    uint32_t TerrDecoGround = 0;

    // DECORATION (MWDecorationSystem) is two different things depending on the map: grass, scrub
    // and tree canopy standing on the ground, or rock formations that ARE the ground (cliffs, and
    // floors laid over a base plane far below). A downward ray keeps going through it and looks
    // at what comes next: real ground within this distance below means it was vegetation, so
    // the ground wins; anything further down, or nothing at all, means the decoration was the
    // surface. 40 m clears the tallest trees and is a fraction of the 220 m fall-through seen.
    static constexpr float kDecoGroundCm = 4000.0f;

    // Samples whose descent start was raised off a too-low bootstrap reading to the player's own
    // altitude. Large on maps with deep geometry - which is exactly where the canyons were.
    uint32_t TerrBootstrapRaised = 0;

    // Verdict memo. A linear scan of at most 64 pointers beats a hash for this size and has no
    // allocation; the census below is the same table, so the lookup pays for both.
    struct HitClassInfo {
        const void *Cls = nullptr;
        uint8_t     Reject = 0;
        uint32_t    Accepted = 0, Rejected = 0;
        char        Name[64] = {};
        char        Chain[160] = {};
    };
    static constexpr int kMaxHitClasses = 64;
    HitClassInfo HitClasses[kMaxHitClasses];
    int          HitClassCount = 0;
    bool         HitCensusLogged = false;

    // Resolve the actor behind the hit currently sitting in TraceFrame. Null when unavailable,
    // which is treated as "accept" everywhere - a filter that cannot see must not start
    // deleting terrain.
    API::UObject* HitActorOf() {
        if (TL.HitActor < 0) return nullptr;
        const uint8_t* slot = TraceFrame.data() + TL.Hit + TL.HitActor;

        if (!TraceActorIsWeak) {
            void* p = nullptr;
            std::memcpy(&p, slot, sizeof(void*));
            return reinterpret_cast<API::UObject*>(p);
        }

        int32_t idx = 0;
        std::memcpy(&idx, slot, sizeof(idx));
        if (idx < 0) return nullptr;              // unset weak pointer
        auto*      sdk = API::get()->param()->sdk;
        const auto arr = sdk->functions->get_uobject_array();
        if (!arr || idx >= sdk->uobject_array->get_object_count(arr)) return nullptr;
        // The serial is NOT checked. It guards against a recycled slot, and the only window
        // here is between the trace returning and this read on the same line of the same
        // function - the object cannot have been destroyed and its slot reused inside it.
        return reinterpret_cast<API::UObject*>(sdk->uobject_array->get_object(arr, idx));
    }

    // ---------------------------------------------------------------------------------------
    // ROCK AND CLIFF DECORATION (rockdeco1, user 2026-10-13 "build the exception for rock or cliff
    // meshes"). MWDecorationSystem holds trees and scrub AND rock formations. The 40 m rule
    // (kDecoGroundCm) took any decoration with real ground within 40 m below it for vegetation,
    // so a rock outcrop or ridge under 40 m tall, standing on ground dug out beneath it, was
    // erased and its dug-out floor published - a false canyon (Tourmaline Desert (Black),
    // 2026-10-13). Now the hit component's static mesh name decides: a rock / cliff mesh is
    // ground at once (verdict 0); anything else keeps the 40 m rule. Decided once per component
    // (each instanced component holds one mesh); the memo goes with TerrainReset. The census
    // line at map complete lists the decoration meshes the rays hit and the verdict for each.
    // ---------------------------------------------------------------------------------------
    struct DecoComp { const void* Comp; const void* Cls; uint8_t Rock; uint32_t Hits; char Mesh[72]; };
    std::vector<DecoComp> DecoComps;
    struct DecoMeshOff { const void* Cls; int32_t Off; };
    std::vector<DecoMeshOff> DecoMeshOffs;
    uint32_t TerrDecoRock = 0;

    API::UObject* HitObjAt(int32_t off, bool weak) {
        if (off < 0) return nullptr;
        const uint8_t* slot = TraceFrame.data() + TL.Hit + off;
        if (!weak) {
            void* p = nullptr;
            std::memcpy(&p, slot, sizeof(void*));
            return reinterpret_cast<API::UObject*>(p);
        }
        int32_t idx = 0;
        std::memcpy(&idx, slot, sizeof(idx));
        if (idx < 0) return nullptr;
        auto*      sdk = API::get()->param()->sdk;
        const auto arr = sdk->functions->get_uobject_array();
        if (!arr || idx >= sdk->uobject_array->get_object_count(arr)) return nullptr;
        return reinterpret_cast<API::UObject*>(sdk->uobject_array->get_object(arr, idx));
    }

    static bool DecoNameIsRock(const char* lower) {
        // "ridge" is left out on purpose (bRIDGE); "stone" also covers cobblestone floors, which are ground anyway.
        for (const char* k : {"rock", "cliff", "boulder", "mesa", "butte", "crag", "spire", "outcrop", "mountain",
                              "canyon", "plateau", "stone", "pillar", "monolith", "hoodoo", "formation", "scree"})
            if (std::strstr(lower, k)) return true;
        return false;
    }

    // True when the hit sitting in TraceFrame is a rock or cliff mesh.
    bool DecoHitIsRock() {
        API::UObject* c = HitObjAt(TL.HitComp, TraceCompIsWeak);
        if (!c) return false;
        API::UStruct* cls = c->get_class();
        if (!cls) return false;
        for (auto& e : DecoComps)
            if (e.Comp == c && e.Cls == cls) { ++e.Hits; return e.Rock != 0; }

        int32_t off = -2;
        for (auto& m : DecoMeshOffs)
            if (m.Cls == cls) off = m.Off;
        if (off == -2) {
            off = -1;
            for (API::UStruct* st = cls; st && off < 0; st = st->get_super_struct())
                for (auto* f = st->get_child_properties(); f; f = f->get_next())
                    if (f->get_fname()->to_string() == L"StaticMesh") {
                        auto* p = reinterpret_cast<API::FProperty*>(f);
                        if (p->get_class() && p->get_class()->get_name() == L"ObjectProperty") off = p->get_offset();
                        break;
                    }
            if (DecoMeshOffs.size() > 64) DecoMeshOffs.clear();
            DecoMeshOffs.push_back({cls, off});
        }
        DecoComp d{c, cls, 0, 1, {}};
        API::UObject* mesh = nullptr;
        if (off >= 0) std::memcpy(&mesh, reinterpret_cast<uint8_t*>(c) + off, sizeof(void*));
        const std::wstring n = mesh ? mesh->get_fname()->to_string() : (L"(no mesh) " + cls->get_fname()->to_string());
        char lower[sizeof(d.Mesh)];
        size_t k = 0;
        for (; k + 1 < sizeof(lower) && k < n.size(); ++k) {
            const wchar_t ch = n[k];
            lower[k] = (ch < 128) ? static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch) : '?';
            d.Mesh[k] = (ch < 128) ? static_cast<char>(ch) : '?';
        }
        lower[k] = d.Mesh[k] = '\0';
        d.Rock = (mesh && DecoNameIsRock(lower)) ? 1 : 0;
        if (DecoComps.size() < 2048) DecoComps.push_back(d);
        return d.Rock != 0;
    }

    void LogDecoCensus() {
        if (DecoComps.empty()) return;
        // One line per mesh name (components sharing a mesh merged), most hits first, top 30.
        struct M { const char* Name; uint8_t Rock; uint32_t Hits, Comps; };
        std::vector<M> ms;
        for (auto& e : DecoComps) {
            bool found = false;
            for (auto& m : ms)
                if (std::strcmp(m.Name, e.Mesh) == 0) { m.Hits += e.Hits; ++m.Comps; found = true; break; }
            if (!found) ms.push_back({e.Mesh, e.Rock, e.Hits, 1});
        }
        std::stable_sort(ms.begin(), ms.end(), [](const M& a, const M& b) { return a.Hits > b.Hits; });
        LogInfo("terrain: decoration meshes hit (rockdeco1) - %zu meshes, %u samples took a rock/cliff mesh as the ground",
                ms.size(), TerrDecoRock);
        for (size_t i = 0; i < ms.size() && i < 30; ++i)
            LogInfo("terrain:   deco mesh %-56s %s  hits %u (%u components)", ms[i].Name,
                    ms[i].Rock ? "ROCK -> ground" : "40 m rule     ", ms[i].Hits, ms[i].Comps);
    }

    // 0 = accept as ground, 1 = reject and keep looking, 2 = volume (unused, see
    // kSoftRejectVolumes), 3 = DECORATION: keep looking, but it is the ground if nothing real
    // turns up close beneath it (see kDecoGroundCm).
    uint8_t VerdictFor(API::UObject* actor) {
        if (!actor) return 0;
        API::UStruct* cls = actor->get_class();
        if (!cls) return 0;

        for (int i = 0; i < HitClassCount; ++i) {
            if (HitClasses[i].Cls == static_cast<const void*>(cls)) {
                if (HitClasses[i].Reject) { ++HitClasses[i].Rejected; return HitClasses[i].Reject; }
                ++HitClasses[i].Accepted;
                return 0;
            }
        }

        // First sighting of this class: decide once, then never walk the chain again.
        auto* sdk = API::get()->param()->sdk;
        const auto obj = reinterpret_cast<UEVR_UObjectHandle>(actor);
        uint8_t reject = 0;
        if (PawnClass     && sdk->uobject->is_a(obj, PawnClass))     reject = 1;
        if (ProjClass     && sdk->uobject->is_a(obj, ProjClass))     reject = 1;
        if (MissileClass  && sdk->uobject->is_a(obj, MissileClass))  reject = 1;

        // Narrowed by hand rather than via a locale conversion: UE class names are ASCII, and a
        // wide format specifier through the log wrapper is one more thing to be wrong.
        char name[64];
        {
            const std::wstring n = cls->get_fname()->to_string();
            // unionfix1: the DLC3 Union dropship blueprints, captured as a ray finds them (see DropshipScan)
            if (!UnionDestrClass && n.rfind(L"UnionDropship_Destructible", 0) == 0) {
                UnionDestrClass = reinterpret_cast<UEVR_UClassHandle>(cls);
                LogInfo("dropships: Union (destructible) class found by the terrain scan");
            }
            if (!UnionTakeoffClass && n.rfind(L"UnionDropship_Takeoff", 0) == 0) {
                UnionTakeoffClass = reinterpret_cast<UEVR_UClassHandle>(cls);
                LogInfo("dropships: Union (takeoff) class found by the terrain scan");
            }
            if (!UnionBaseClass && n == L"UnionDropship_C") { // the base-game landing Union (2026-10-19)
                UnionBaseClass = reinterpret_cast<UEVR_UClassHandle>(cls);
                LogInfo("dropships: Union (base game) class found by the terrain scan");
            }
            size_t k = 0;
            for (; k + 1 < sizeof(name) && k < n.size(); ++k)
                name[k] = (n[k] < 128) ? static_cast<char>(n[k]) : '?';
            name[k] = '\0';
        }

        // ---- ANCESTRY, which is where the real rules live -----------------------------------
        //
        // The first census named thirty-one classes and explained none of them. The second one
        // walked the chain and the answers fell out immediately:
        //
        //   Leopard_C        < AbstractLeopard_C < MWDropship < Actor      1776 hits
        //   BombBayDoor_C    < BayDoor_C < MWBayDoor < Actor                508 hits
        //   DerivedMech_C    < AbstractMech_C < MWMech < Character < Pawn    35 hits
        //   Landscape        < LandscapeProxy < Actor                     44744 hits
        //   <every building> < MWDestructibleObject_C < ... < Actor
        //
        // So the dropship has a proper native base after all, and - the part a leaf-name rule
        // would never have caught - its BAY DOORS are a different class entirely. 508 samples
        // of "ground" were a bomb bay door. That is exactly why the name matching this replaces
        // was written under protest.
        //
        // Matching on ANCESTOR names rather than the leaf costs nothing: the chain is being
        // walked for the census anyway, so the test rides along on a loop that already exists.
        {
            size_t w = 0;
            char chain[sizeof(HitClassInfo::Chain)];
            chain[0] = '\0';

            for (API::UStruct* st = cls->get_super_struct(); st; st = st->get_super_struct()) {
                const std::wstring sn = st->get_fname()->to_string();

                // Not ground, by base class. Each of these is a whole family in one line.
                if (sn == L"MWDropship" ||        // Leopard, Union, Overlord and their kin
                    sn == L"MWBayDoor")           // their doors, which are separate actors
                    reject = 1;
                if (sn == L"MWDecorationSystem" && !reject) // foliage, scatter - or cliffs; see below
                    reject = 3;

                // ---- VOLUMES: the canyons, and why they are SOFT-rejected -------------------
                //
                // One map came back like this:
                //
                //   BlockingVolume  accept  212441 hits   [Volume < Brush < Actor]
                //   Landscape       accept   26666 hits
                //
                // 84% of that map's "terrain" was an invisible collision volume - no visual
                // mesh, nothing to see in game, which is precisely why the canyons appeared on
                // our map and nowhere else. Every tile floored at exactly -237.1 m and five of
                // them capped at exactly +62.9 m: two flat planes at fixed heights, map-wide.
                //
                // But these cannot simply be refused. MW5 uses blocking volumes as invisible
                // walls AND to smooth collision over rough ground, and at 8:1 against the
                // landscape some of them are certainly the surface you actually walk on.
                // Hard-rejecting would turn most of that map into holes, which is worse than
                // the canyon it fixes.
                //
                // So: verdict 2, "prefer anything below this". The ray keeps going, real
                // geometry underneath wins, and the volume's own height is used only when
                // there is genuinely nothing else down there. It costs one extra trace per
                // volume hit and it cannot punch a hole or invent a canyon.
                // ...AND IT WAS STILL WRONG, in the way the "reject them outright" option was
                // predicted to be. A map came back with ONE class in the census - BlockingVolume,
                // 1161 hits, nothing else at all. No landscape, no static meshes. Demoting
                // volumes there demoted the entire world: 0 tiles with ground, a blank page.
                //
                // The soft fallback was supposed to make that impossible and fired exactly once
                // in 1161 hits, which I cannot explain from the log and will not guess at.
                //
                // So volumes are ACCEPTED again. They are legitimately the floor on some maps -
                // that map proves it - and the canyons were never really about what class the
                // ray hit. See kNoHintStartsAbovePlayer below for what was actually wrong.
                if (kSoftRejectVolumes && sn == L"Volume" && !reject) reject = 2;

                // The building base, captured for the structures layer. Buildings are still
                // ACCEPTED as terrain until that layer exists - see kRejectBuildings.
                if (!DestructibleClass && sn == L"MWDestructibleObject_C")
                    DestructibleClass = reinterpret_cast<UEVR_UClassHandle>(st);

                if (w + 4 < sizeof(chain)) {
                    if (w) { chain[w++] = ' '; chain[w++] = '<'; chain[w++] = ' '; }
                    for (size_t k = 0; k < sn.size() && w + 1 < sizeof(chain); ++k)
                        chain[w++] = (sn[k] < 128) ? static_cast<char>(sn[k]) : '?';
                    chain[w] = '\0';
                }
                if (sn == L"Actor") break; // nothing above Actor is informative here
            }

            // Two classes derive straight from Actor and share a base with nothing, so they are
            // the only ones that must still be matched by their own name. The ancestor loop
            // above starts at the SUPER, so it never sees the leaf and cannot catch these.
            //
            //   Ultra_Dynamic_Sky_C   235 hits - the sky. A downward ray ending on the sky is
            //                         nonsense however it got there.
            //   MWDecorationSystem   1335 hits - foliage and scatter. Third-largest source of
            //                         "ground" on the map after the landscape and static meshes,
            //                         and the likeliest explanation for terrain reading subtly
            //                         lumpy everywhere: a tree canopy is not the floor. Note
            //                         this does NOT touch Decoration_C or MWDecorationBreakable,
            //                         which are the placed props - crates, generators, traffic
            //                         lights - and remain terrain.
            //
            //   ...AND ON SOME MAPS IT IS THE GROUND. A moon canyon map came back with 98,319
            //   decoration hits against 38,716 landscape: its cliffs and much of its floor are
            //   decoration meshes over a flat base plane 220 m down, and rejecting them sank the
            //   whole canyon into a pit. So decoration is verdict 3 - see kDecoGroundCm.
            if (std::strstr(name, "Ultra_Dynamic_Sky"))
                reject = 1;
            else if (std::strstr(name, "MWDecorationSystem") && !reject)
                reject = 3;

            if (HitClassCount < kMaxHitClasses) {
                HitClassInfo& e = HitClasses[HitClassCount++];
                e.Cls    = static_cast<const void*>(cls);
                e.Reject = reject;
                (reject ? e.Rejected : e.Accepted) = 1;
                std::memcpy(e.Name, name, sizeof(e.Name));
                std::memcpy(e.Chain, chain, sizeof(e.Chain) < sizeof(chain) ? sizeof(e.Chain) : sizeof(chain));
            }
        }
        return reject;
    }

    // The census. This is the highest-information line the plugin can print right now: it says
    // what the terrain rays are ACTUALLY hitting, by class, which is the only way to learn what
    // MW5 calls its landscape, whether dropships are pawn-derived at all, and whether some
    // unexpected volume class is turning up - the shape of thing that would produce a flat
    // plane at a constant Z.
    void LogHitCensus(const char* why) {
        if (HitClassCount <= 0) return;
        LogInfo("terrain: hit census (%s) - %d distinct classes, %u hits rejected, "
                "%u samples fell back to a volume surface, %u samples took decoration as the ground",
                why, HitClassCount, TerrHitsRejected, TerrVolumeFallback, TerrDecoGround);
        for (int i = 0; i < HitClassCount; ++i) {
            const HitClassInfo& e = HitClasses[i];
            LogInfo("terrain:   %-42s %s  acc %u, rej %u   [%s]",
                    e.Name, (e.Reject == 3) ? "deco? " : (e.Reject == 2) ? "below?" : (e.Reject ? "REJECT" : "accept"),
                    e.Accepted, e.Rejected, e.Chain);
        }
        LogDecoCensus(); // rockdeco1
    }

    // How far above a LOCAL height estimate the downward ray starts. 200 m clears any hill
    // shoulder between two 16 m samples while staying nowhere near the altitude where the first
    // attempt at a downward ray found a ceiling and reported 1047 m of relief.
    static constexpr float kSkyMarginCm = 20000.0f;

    // startfloor1 (user 2026-10-13 "the ray should always start at least 300 m above the player's altitude"). The start
    // came only from the previous sample (+200 m): one sample that fell to a floor far below (the 62.9 m BlockingVolume on
    // Tourmaline Black) dragged the next start UNDER the real ground, which a ray from below does not hit - the canyon
    // spread sample by sample (canyondiag1: 248 of 24,630 re-reads found ground 240-377 m higher without the hint).
    // Now no downward read starts lower than the player's altitude + 300 m.
    static constexpr float kMinStartAbovePlayerCm = 30000.0f;
    static constexpr float kSeamOffsetXCm = 3.7f, kSeamOffsetYCm = 5.3f; // seamoffset1: off any exact grid line

    // -------------------------------------------------------------------------------------
    // GROUND HEIGHT. Read from ABOVE, starting just above the local surface.
    //
    // The history here matters, because both obvious answers are wrong and for opposite reasons.
    //
    // A downward ray from high up returns the TOPMOST blocking surface, which is a rooftop, a
    // bridge deck or - at 1 km - some high-altitude volume. That is what produced the original
    // 1047 m "relief".
    //
    // Inverting it fixed that and introduced a subtler fault. An upward ray returns the
    // BOTTOM-MOST surface. For the landscape heightfield those are the same thing, which is why
    // the verification probe agreed to 0.66 m on open ground and the inversion looked sound. But
    // a hill built as a static mesh rather than sculpted into the landscape is a closed solid,
    // and it is sunk into the terrain: the ray from below enters through its underside, BELOW
    // the surrounding ground. The hill is then published as a pit. Hills reading as canyons,
    // some hills and not others - exactly the shape of the report from the field, and exactly
    // what you would expect from mesh outcrops mixed into a sculpted landscape.
    //
    // So: downward, but started only ~200 m above where the ground is already known to be
    // rather than from the sky. Terrain is continuous at 16 m spacing, so the PREVIOUS sample's
    // height is an excellent estimate and costs nothing. That keeps the start below any ceiling
    // while still being above the surface, which is the combination neither earlier version had.
    //
    // hintZ is that estimate, or TERR_NO_HEIGHT when there is none (first sample of a tile, or
    // after a miss). Without a hint we bootstrap with the upward ray - which is reliable for
    // finding SOMETHING - and then immediately re-read downward from above it.
    // -------------------------------------------------------------------------------------
    bool GroundAt(API::UObject* pawn, float x, float y, float refZ, float& outZ,
                  float hintZ = TERR_NO_HEIGHT, int* tracesOut = nullptr) {
        int traces = 0;
        // seamoffset1 (user 2026-10-13 "build the offset fix"): every read is traced a few cm off the exact grid point.
        // Sample centres sit on exact multiples of 8 m; on Tourmaline Black a whole row at Y = +-1512 m lay exactly on
        // a seam between landscape collision pieces, the vertical ray slipped between them and the row read as the
        // 62.9 m floor across the map (403 of 512 samples) - a straight 130 m "canyon" in the 3D map. The published
        // position is unchanged; 4-5 cm make no difference to a height.
        x += kSeamOffsetXCm;
        y += kSeamOffsetYCm;

        // A downward ray from just above the local estimate. This is the normal path and the
        // one that produces the published height.
        //
        // It RESTARTS below anything that is not world geometry. A dropship's hull would
        // otherwise be published as a 40 m plateau that outlives the dropship by the whole
        // mission; a mech standing at the drop point becomes a permanent bump in the ground.
        // Each restart is a full trace, so the count is reported back and charged to the
        // caller's budget.
        auto downFrom = [&](float startZ, float& z) -> bool {
            const float floorZ = startZ - 200000.0f; // 2 km of reach, total
            float from = startZ;

            // The topmost VOLUME surface seen on the way down, kept as a last resort. First one
            // wins: that is the highest, and so the closest thing to a floor you could stand on.
            bool  haveSoft = false;
            float softZ    = 0.0f;
            // The topmost decoration surface seen on the way down (verdict 3).
            bool  haveDeco = false;
            float decoZ    = 0.0f;

            // Grows on any attempt that fails to descend - see the note at the restart below.
            float step = 50.0f;

            for (int attempt = 0; attempt <= kHitRejectRetries; ++attempt) {
                const FVector3f a{x, y, from};
                const FVector3f b{x, y, floorZ};
                const TraceResult r = RunTrace(pawn, a, b);
                ++traces;
                if (!r.Blocking) break;
                if (!kTerrActorFilter || TL.HitActor < 0) { z = r.Z; return true; }

                uint8_t v = VerdictFor(HitActorOf());
                if (v == 3 && DecoHitIsRock()) { v = 0; ++TerrDecoRock; } // rockdeco1: a rock / cliff mesh is ground
                if (v == 0) {                               // real geometry: done
                    if (haveDeco && decoZ - r.Z > kDecoGroundCm) { z = decoZ; ++TerrDecoGround; return true; }
                    z = r.Z;
                    return true;
                }
                if (v == 3) {
                    if (!haveDeco) { haveDeco = true; decoZ = r.Z; }
                    else if (decoZ - r.Z > kDecoGroundCm) { z = decoZ; ++TerrDecoGround; return true; }
                    ++TerrHitsRejected;
                }
                else if (v == 2 && !haveSoft) { haveSoft = true; softZ = r.Z; }
                else ++TerrHitsRejected;                    // a hard reject, or a lower volume

                // ---- GUARANTEE PROGRESS ------------------------------------------------
                //
                // Restarting a fixed half-metre below the rejected surface assumes the ray
                // actually got somewhere. Against an ENCLOSING shell it does not: the sky
                // sphere wraps the entire map, so a ray starting inside it reports a hit at
                // its own start point, and half a metre per attempt means 4.5 m over nine
                // tries. Two tiles came back 60% holes for exactly this reason - 5,866
                // rejections on one tile, nine per sample, every one of them the sky.
                //
                // So the step GROWS whenever an attempt fails to descend. 0.5 m, then 5, then
                // 50, then 500 - four rejections escape any shell in this game. A ray making
                // real progress through stacked geometry never triggers it and still steps
                // half a metre, which is what you want between a roof and the floor below it.
                // The test is on the HIT against THIS ray's own start - not on how far the next
                // start moved. Getting that backwards keeps the crawl, because the start always
                // advances by the step and so always looks like progress, while the ray is in
                // fact pinned against the same shell. A hit within a metre of where the ray
                // began means the ray began INSIDE that geometry, and only a bigger step gets
                // out of it. (Written the wrong way round first; a simulation caught it still
                // crawling 0.5 m per attempt.)
                const bool noProgress = (r.Z >= from - 100.0f);
                if (noProgress) step *= 10.0f;
                from = r.Z - step;
                if (from <= floorZ) break;
            }

            // Nothing solid underneath. A volume surface is a far better answer than a hole -
            // it is where the mech's feet would actually stop - so prefer it, and only give up
            // when there was no acceptable surface of any kind.
            if (haveDeco) { z = decoZ; ++TerrDecoGround; return true; }
            if (haveSoft) { z = softZ; ++TerrVolumeFallback; return true; }

            // Still standing on something we refuse to call ground. Publishing a hole is the
            // honest answer: a hole is skipped by both renderers, a wrong height is DRAWN.
            return false;
        };

        // Every exit writes the trace count back, so no path can slip out unbilled.
        struct Bill { int* out; const int* n; ~Bill() { if (out) *out = *n; } } bill{tracesOut, &traces};

        if (hintZ > TERR_NO_HEIGHT * 0.5f) {
            float z;
            if (downFrom((std::max)(hintZ + kSkyMarginCm, refZ + kMinStartAbovePlayerCm), z)) { outZ = z; ++TerrDownHits; return true; } // startfloor1
            // Fall through: the hint was below the surface here, which means a near-vertical
            // rise between adjacent samples. Rare, and the bootstrap below handles it.
        }

        // Bootstrap. Upward from well below finds the terrain reliably even with no estimate;
        // it may land on the underside of a mesh, which is precisely why it is not the answer -
        // only a starting point for a proper reading from above.
        float zu = 0.0f;
        {
            const FVector3f from{x, y, refZ - 100000.0f};
            const FVector3f to  {x, y, refZ + 100000.0f};
            const TraceResult r = RunTrace(pawn, from, to);
            ++traces;
            // NOT filtered, deliberately. This ray only produces a starting altitude for the
            // real reading below; if it lands on a mech's foot the downward re-read still
            // starts 200 m above and still finds the true ground.
            if (!r.Blocking) { ++TerrMisses; return false; }
            zu = r.Z;
        }
        ++TerrUpHits;

        // ---- NEVER START THE DESCENT BELOW THE PLAYER'S OWN GROUND -------------------------
        //
        // This one line is the canyon. The arithmetic from the field log:
        //
        //     upward ray hit Z = -437.11 m      (a blocking volume 650 m below the terrain)
        //     kSkyMarginCm     =  200 m
        //     descent started at -237.11 m
        //
        // and the canyon map floored EVERY tile at exactly -237.1 m. The canyon floor was the
        // start altitude of the downward ray. The bootstrap anchors the real reading to whatever
        // the up-ray stops on, and where deep geometry exists the descent begins hundreds of
        // metres UNDERNEATH the ground and everything above it is invisible to the ray.
        //
        // refZ is the player's own Z, and the player is standing on the ground, so it is a floor
        // on where the surface can plausibly be. Taking the higher of the two costs nothing, is
        // unchanged wherever the up-ray was already sane, and makes "start below the terrain and
        // look down" arithmetically impossible.
        //
        // It does NOT reintroduce the ceiling problem that killed the original read-from-the-sky
        // version: that started 1 km up and caught roofs and canopy. This starts 200 m above the
        // player, which is above local terrain and well below anything overhead.
        const float zRef = (kNoHintStartsAbovePlayer && zu < refZ) ? refZ : zu;
        if (zRef != zu) ++TerrBootstrapRaised;

        float z;
        if (downFrom((std::max)(zRef + kSkyMarginCm, refZ + kMinStartAbovePlayerCm), z)) { outZ = z; return true; } // startfloor1

        // Nothing acceptable from above, twice. The upward ray's own reading is NOT published:
        // for a landscape heightfield the underside and the surface are the same point, but for
        // a static-mesh outcrop or anything hollow it is the bottom of the solid, and publishing
        // it draws a hill as a pit.
        //
        // This used to publish it anyway and the counter read zero on three maps, so it looked
        // harmless. Rejecting foliage changed that - a ray through dense scrub can spend every
        // retry on canopy - and the count went to 34 on a single tile, correlating almost
        // exactly with that tile's rejection count. So: hole, and the interpolation pass on
        // tile completion decides whether it is an interior gap worth filling from its
        // neighbours or a genuine edge of the world that must stay empty.
        ++TerrUnderFallback;
        return false;
    }

    // =======================================================================================
    // HOLE FILLING - and the one distinction that makes it safe
    //
    // There are two completely different reasons a sample has no height, and they must not be
    // treated alike:
    //
    //   INTERIOR gap  - the descent gave up (dense foliage ate every retry, or everything
    //                   under the ray was refused). The ground is really there; we failed to
    //                   measure it. Its neighbours are a good estimate and leaving a pit in
    //                   otherwise continuous terrain is the worse answer.
    //
    //   EDGE OF MAP   - there is genuinely nothing there. Boundary tiles come back 800/1024
    //                   empty and that is CORRECT. Interpolating across it would invent
    //                   terrain outside the playable area and, worse, would keep growing:
    //                   each filled sample becomes a neighbour for the next one, so a naive
    //                   pass would march the map outward into the void indefinitely.
    //
    // The two are told apart by NEIGHBOUR COUNT rather than by tracking why each sample failed.
    // An interior gap is surrounded by ground - typically all eight neighbours. A sample at the
    // edge of a void has three or fewer. Requiring a majority (5 of 8) fills the first and
    // cannot touch the second, and it needs no extra per-sample state to do it.
    //
    // Order independence matters: each pass reads a SNAPSHOT, so a filled value cannot seed
    // another fill within the same pass. Repeated passes let a two-sample gap close from both
    // sides without either pass smearing in one direction. Four passes close anything that is
    // genuinely an interior gap; whatever survives was never interior.
    // =======================================================================================
    static constexpr int kFillPasses      = 4;
    static constexpr int kFillMinNeighbours = 5;
    float TerrFillSnapshot[TERR_SAMPLES * TERR_SAMPLES];

    uint32_t FillInteriorHoles(TerrainTile& t) {
        uint32_t filled = 0;
        for (int pass = 0; pass < kFillPasses; ++pass) {
            std::memcpy(TerrFillSnapshot, t.Height, sizeof(TerrFillSnapshot));
            uint32_t thisPass = 0;

            for (int row = 0; row < TERR_SAMPLES; ++row) {
                for (int col = 0; col < TERR_SAMPLES; ++col) {
                    const int i = row * TERR_SAMPLES + col;
                    if (TerrFillSnapshot[i] != TERR_NO_HEIGHT) continue;

                    float sum = 0.0f;
                    int   n   = 0;
                    for (int dr = -1; dr <= 1; ++dr) {
                        for (int dc = -1; dc <= 1; ++dc) {
                            if (!dr && !dc) continue;
                            const int r2 = row + dr, c2 = col + dc;
                            // Samples outside the tile count as unknown rather than as ground.
                            // Conservative at tile borders - a genuine gap straddling a seam
                            // may not close - which is the right way to be wrong here.
                            if (r2 < 0 || c2 < 0 || r2 >= TERR_SAMPLES || c2 >= TERR_SAMPLES) continue;
                            const float h = TerrFillSnapshot[r2 * TERR_SAMPLES + c2];
                            if (h == TERR_NO_HEIGHT) continue;
                            sum += h;
                            ++n;
                        }
                    }
                    if (n >= kFillMinNeighbours) { t.Height[i] = sum / (float)n; ++thisPass; }
                }
            }
            filled += thisPass;
            if (!thisPass) break; // nothing closed this pass; nothing will close next
        }
        return filled;
    }

    // =======================================================================================
    // STRUCTURES. Buildings as enumerated ACTORS with real boxes, not as samples in the height
    // lattice. The reasoning is in TerrainShared.h; this is the machinery.
    //
    // GetActorBounds is a BlueprintCallable UFUNCTION on AActor, so it is reachable exactly the
    // way LineTraceSingle is - measure the frame by property name, fill it, process_event. Its
    // out-parameters are FVectors, three floats in UE4, and the same 12-byte check the trace
    // layout does applies here for the same reason: a UE5 build would hand back doubles and the
    // numbers would look plausible and be wrong.
    // =======================================================================================
    API::UObject*   BoundsCdoActor = nullptr;   // any live actor; the function is not static
    API::UFunction* BoundsFn       = nullptr;
    bool            BoundsChecked  = false, BoundsReady = false;
    int32_t         BoundsOnlyColliding = -1, BoundsOrigin = -1, BoundsExtent = -1, BoundsFrame = -1;
    std::vector<uint8_t> BoundsFrameBuf;

    // =======================================================================================
    // PER-COMPONENT BOUNDS
    //
    // GetActorBounds returns ONE axis-aligned box per ACTOR, and "actor" is not the unit anyone
    // sees. That single choice produced every problem this code has had:
    //
    //   * one actor owning many buildings - a wall run, a row of segments - became one box
    //     swallowing the map, because the union spans all of them;
    //   * one component left at the world origin stretched the union back to (0,0,0), giving a
    //     kilometre-wide box with nothing visible inside it;
    //   * and a crate and a barracks are the same kind of box, so telling them apart needed
    //     rules about size that are really guesses about identity.
    //
    // Size gates and the actor-coherence test were both compensation for that, and neither can
    // succeed: the information was destroyed before the box was computed.
    //
    // A MESH COMPONENT is the right unit. It is what is drawn, so its bounds are the thing you
    // can see. A wall parent becomes one box per segment. A stray scene component has no mesh
    // and is never enumerated, so the origin inflation cannot happen at all. Nothing here has
    // to know what a building IS.
    //
    // Reflection: K2_GetComponentsByClass, GetLocalBounds and K2_GetComponentLocation are all
    // BlueprintCallable, so all three are reachable the way LineTraceSingle is. USceneComponent
    // ::Bounds would have been simpler and is NOT a UPROPERTY in UE4 - it is a plain public
    // member - which is why this goes the long way round.
    //
    // Rotation is not read. Local bounds are turned into a world box using the component's world
    // location and the LARGER horizontal half-extent for both axes, so a rotated building is
    // slightly over-boxed rather than clipped. The display was already axis-aligned; this keeps
    // the error on the safe side of it.
    // =======================================================================================
    API::UFunction*   CompsByClassFn = nullptr;
    API::UFunction*   LocalBoundsFn  = nullptr;
    API::UFunction*   CompLocFn      = nullptr;
    UEVR_UClassHandle MeshCompClass  = nullptr;
    int32_t CbcFrame = -1, CbcClass = -1, CbcRet = -1;
    int32_t LbFrame = -1, LbMin = -1, LbMax = -1;
    int32_t ClFrame = -1, ClRet = -1;
    bool    CompChecked = false, CompReady = false;
    std::vector<uint8_t> CbcBuf, LbBuf, ClBuf;

    // At most this many mesh components per actor. A building is a handful; anything presenting
    // hundreds is a prefab container, and drawing all of it would bury the map in blue.
    static constexpr int kMaxCompsPerActor = 24;
    // Below this, a component is a handrail, a vent or a door handle - detail, not structure.
    static constexpr float kMinCompHalfCm = 150.0f; // 3 m across

    bool EnsureComponentLayout(API::UObject* anyActor) {
        if (CompChecked) return CompReady;
        if (!anyActor || !anyActor->get_class()) return false;
        CompChecked = true;

        auto findFn = [&](API::UObject* o, const wchar_t* nm) -> API::UFunction* {
            for (API::UStruct* st = o->get_class(); st; st = st->get_super_struct())
                if (auto* f = static_cast<API::UClass*>(st)->find_function(nm)) return f;
            return nullptr;
        };

        CompsByClassFn = findFn(anyActor, L"K2_GetComponentsByClass");
        if (!CompsByClassFn) { LogError("components: K2_GetComponentsByClass not found"); return false; }
        CbcFrame = CompsByClassFn->get_properties_size();
        for (auto* f = CompsByClassFn->get_child_properties(); f; f = f->get_next()) {
            auto* p = reinterpret_cast<API::FProperty*>(f);
            const std::wstring nm = f->get_fname()->to_string();
            if      (nm == L"ComponentClass") CbcClass = p->get_offset();
            else if (nm == L"ReturnValue")    CbcRet   = p->get_offset();
        }

        auto* smc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.StaticMeshComponent");
        MeshCompClass = reinterpret_cast<UEVR_UClassHandle>(smc);

        if (smc) {
            if (auto* cdo = smc->get_class_default_object()) {
                LocalBoundsFn = findFn(cdo, L"GetLocalBounds");
                CompLocFn     = findFn(cdo, L"K2_GetComponentLocation");
            }
        }
        if (LocalBoundsFn) {
            LbFrame = LocalBoundsFn->get_properties_size();
            for (auto* f = LocalBoundsFn->get_child_properties(); f; f = f->get_next()) {
                auto* p = reinterpret_cast<API::FProperty*>(f);
                const std::wstring nm = f->get_fname()->to_string();
                if      (nm == L"Min") LbMin = p->get_offset();
                else if (nm == L"Max") LbMax = p->get_offset();
            }
        }
        if (CompLocFn) {
            ClFrame = CompLocFn->get_properties_size();
            for (auto* f = CompLocFn->get_child_properties(); f; f = f->get_next()) {
                auto* p = reinterpret_cast<API::FProperty*>(f);
                if (f->get_fname()->to_string() == L"ReturnValue") ClRet = p->get_offset();
            }
        }

        if (!MeshCompClass || !LocalBoundsFn || !CompLocFn || CbcFrame <= 0 || CbcClass < 0 ||
            CbcRet < 0 || LbMin < 0 || LbMax < 0 || ClRet < 0) {
            LogError("components: layout incomplete (mesh=%p lb=%p cl=%p cbc %d/%d/%d "
                     "min %d max %d ret %d) - falling back to actor bounds",
                     (void*)MeshCompClass, (void*)LocalBoundsFn, (void*)CompLocFn,
                     CbcFrame, CbcClass, CbcRet, LbMin, LbMax, ClRet);
            return false;
        }
        // Same 12-byte FVector guard as everywhere else: UE5 would hand back doubles and every
        // number would look plausible and be wrong.
        if (LbMax - LbMin != 12) {
            LogError("components: GetLocalBounds Min->Max gap %d, expected 12 - falling back",
                     LbMax - LbMin);
            return false;
        }

        CbcBuf.assign(static_cast<size_t>(CbcFrame) + 64, 0);
        LbBuf.assign(static_cast<size_t>(LbFrame) + 64, 0);
        ClBuf.assign(static_cast<size_t>(ClFrame) + 64, 0);
        CompReady = true;
        LogInfo("components: ready (ComponentsByClass frame %d, GetLocalBounds Min@%d Max@%d, "
                "Location ret@%d) - publishing a box per mesh component",
                CbcFrame, LbMin, LbMax, ClRet);
        return true;
    }

    // ==========================================================================================
    // THE ACTOR'S LIVE COMPONENTS - ASK THE ENGINE, DO NOT READ THE PROPERTY TABLE
    //
    // This replaces every "walk the class's properties and dereference each object pointer to see
    // what it is" search in this file. Those searches caused every crash this plugin has had, and
    // the reason is now plain from MWVR's own source: VR_HUD_TargetObject gets its components
    // with GetComponentByClass. It asks the engine. It never reads a property table.
    //
    // The difference is not style. K2_GetComponentsByClass returns the actor's REGISTERED
    // component set, a list the engine builds and maintains, so everything in it is alive and is
    // really a component. The property table is a different thing entirely: it is every
    // object-typed field the class declares, which also holds uninitialised blueprint variables,
    // references to actors that have since been destroyed, and transient editor fields. Most of
    // those pointers are fine. The ones that are not are EXCEPTION_ACCESS_VIOLATION, and a mech
    // has hundreds of them, walked four times a second, for every contact on the field.
    //
    // Matching on the class NAME is kept, because that is what already works and it tolerates the
    // per-chassis blueprint classes (viper_Target_C). What changes is the list being matched
    // against: the engine's, instead of raw memory.
    UEVR_UClassHandle ActorCompClass = nullptr;
    std::vector<uint8_t> LcBuf;

    // WALKED IN PLACE, NOT COPIED INTO A FIXED ARRAY.
    //
    // The first version copied into a 96-entry buffer, and a mech turned out to have MORE than 96
    // registered components - the log said "96 live component(s)", exactly the cap, and the
    // per-chassis target component was past it. That is why the art key stayed at 0/3 after the
    // sweep: the component was there, the engine returned it, and my own buffer threw it away.
    // An arbitrary cap on someone else's list is a bug waiting for a big enough actor.
    template <typename F>
    void ForEachLiveComponent(API::UObject* actor, F&& fn) {
        if (!CompReady || !actor) return;
        if (!ActorCompClass) {
            auto* ac = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
            if (!ac) return;
            ActorCompClass = reinterpret_cast<UEVR_UClassHandle>(ac);
            LogInfo("components: ActorComponent resolved - component lookups now ask the engine");
        }
        if (LcBuf.size() < (size_t)CbcFrame + 64) LcBuf.assign((size_t)CbcFrame + 64, 0);
        // NOT zeroed between calls. The return slot still holds the previous call's result, and
        // the engine ASSIGNS the new one over it - which frees the old allocation. Zeroing it
        // orphaned every returned array/string (the GaacBuf rule, above). Inputs are rewritten
        // every call; this buffer serves this one function only.
        void* cls = reinterpret_cast<void*>(ActorCompClass);
        std::memcpy(LcBuf.data() + CbcClass, &cls, sizeof(void*));
        actor->process_event(CompsByClassFn, LcBuf.data());

        FScriptArrayLayout arr{};
        std::memcpy(&arr, LcBuf.data() + CbcRet, sizeof(arr));
        if (!arr.Data || arr.Num <= 0) return;
        auto** comps = reinterpret_cast<API::UObject**>(arr.Data);
        for (int32_t i = 0; i < arr.Num; ++i)
            if (comps[i] && !fn(comps[i])) return;
    }

    // ---- per-CLASS name verdicts ------------------------------------------------------------
    // A class's super chain never changes, so whether it matches a name is decided once per
    // class, not once per component per lookup. Before this every lookup converted the class
    // name of all ~273 components (and each of their parents) to a string - tens of thousands
    // of allocations per scan, on the game thread. Same answers; the chain is walked the first
    // time a class is met and never again. Cleared when the player pawn changes (a new mission
    // can unload blueprint classes and reuse their addresses) and capped as a backstop.
    std::vector<std::wstring> VerdictNames;                          // index = bit
    std::unordered_map<void*, std::pair<uint32_t, uint32_t>> ClassVerdicts; // known, matched
    std::unordered_map<void*, uint8_t> TargetVerdicts;               // 1 target comp, 0 not

    // Also every other per-CLASS cache: a new mission can load a different class at an address an
    // old one used, and a cached function or offset from the old class would then be applied to
    // the new one.
    void ResetClassVerdicts() {
        LastArt.clear(); ArtLapses = 0;
        UnitMemos.clear(); // unitmemo3: per-unit memos go with the pawn change too (new mission, mech swap)
        ClassVerdicts.clear(); TargetVerdicts.clear();
        PodVerdicts.clear(); PodDataFns.clear(); PodIdNames.clear(); PodIdMelee.clear(); RegLayouts.clear(); EqOwnerVerdicts.clear();
        // bugsweep1 (2026-10-17): the other caches keyed by a class or actor ADDRESS - a freed class / unit's address is
        // reused by the next one (a wrong chassis label, a freed UFunction called, a freed class handed to
        // GetAllActorsOfClass). The Union dropship classes are captured again as the terrain scan meets them.
        IdentCache.clear(); NoArgCache.clear(); ArgCache.clear(); ProjTypeCache.clear(); FarInst.clear();
        UnionDestrClass = UnionTakeoffClass = UnionBaseClass = nullptr;
    }

    // bugsweep1 (2026-10-17): A NEW WORLD, told the way LanceControl tells it - a different controller or level, or the
    // world clock going backwards. The pawn address alone missed a next-mission mech that landed at the old one's
    // address (the old map, death latch and labels then lived on), and "left the mission" could never run (Gather
    // returns before TerrainStep without a mech).
    API::UObject* WorldPc = nullptr;
    API::UObject* WorldLevel = nullptr;
    float         WorldT = -1.0f;
    bool NewWorld() {
        auto* pc = API::get()->get_player_controller(0);
        if (!pc) return false;
        API::UObject* lvl = pc->get_outer();
        float t = -1.0f;
        static API::UObject* gs = nullptr;
        if (!gs)
            if (auto* c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameplayStatics"))
                gs = c->get_class_default_object();
        if (gs) {
            struct { API::UObject* Ctx; float Ret; } p{ pc, -1.0f };
            gs->call_function(L"GetTimeSeconds", &p);
            t = p.Ret;
        }
        const bool fresh = WorldPc && (pc != WorldPc || lvl != WorldLevel || (t >= 0.0f && WorldT >= 0.0f && t + 1.0f < WorldT));
        WorldPc = pc;
        WorldLevel = lvl;
        if (t >= 0.0f) WorldT = t;
        return fresh;
    }

    int VerdictBit(const wchar_t* want) {
        for (size_t i = 0; i < VerdictNames.size(); ++i)
            if (VerdictNames[i] == want) return (int)i;
        if (VerdictNames.size() >= 32) return -1; // more distinct names than bits: walk uncached
        VerdictNames.emplace_back(want);
        return (int)VerdictNames.size() - 1;
    }

    static bool ChainHas(API::UObject* c, const wchar_t* want) {
        for (API::UStruct* st = c->get_class(); st; st = st->get_super_struct()) {
            if (!st->get_fname()) break;
            if (NameHas(st->get_fname()->to_string(), want)) return true;
        }
        return false;
    }

    bool ClassHas(API::UObject* c, const wchar_t* want, int bit) {
        void* cls = c->get_class();
        if (!cls || bit < 0) return ChainHas(c, want);
        if (ClassVerdicts.size() >= 4096) ClassVerdicts.clear();
        auto& v = ClassVerdicts[cls];
        const uint32_t m = 1u << bit;
        if (!(v.first & m)) {
            v.first |= m;
            if (ChainHas(c, want)) v.second |= m;
        }
        return (v.second & m) != 0;
    }

    API::UObject* CompByClassName(API::UObject* actor, const wchar_t* want) {
        API::UObject* hit = nullptr;
        const int bit = VerdictBit(want);
        ForEachLiveComponent(actor, [&](API::UObject* c) {
            if (ClassHas(c, want, bit)) { hit = c; return false; }
            return true;
        });
        return hit;
    }

    // Boxes for one actor's mesh components. Returns how many were written.
    int ComponentBoxes(API::UObject* actor, TerrainStructure* out, int maxOut) {
        if (!CompReady || !actor || maxOut <= 0) return 0;

        // CbcBuf is not zeroed per call - see ForEachLiveComponent.
        void* cls = reinterpret_cast<void*>(MeshCompClass);
        std::memcpy(CbcBuf.data() + CbcClass, &cls, sizeof(void*));
        actor->process_event(CompsByClassFn, CbcBuf.data());

        FScriptArrayLayout arr{};
        std::memcpy(&arr, CbcBuf.data() + CbcRet, sizeof(arr));
        if (!arr.Data || arr.Num <= 0) return 0;
        auto** comps = reinterpret_cast<API::UObject**>(arr.Data);

        int n = arr.Num;
        if (n > kMaxCompsPerActor) n = kMaxCompsPerActor;

        int written = 0;
        for (int i = 0; i < n && written < maxOut; ++i) {
            API::UObject* c = comps[i];
            if (!c) continue;

            // VISIBILITY is what restores the "destroyed buildings disappear" behaviour.
            //
            // It used to work by accident: GetActorBounds was called with
            // bOnlyCollidingComponents, so a wrecked building - whose collision is gone - came
            // back with degenerate bounds and got dropped by the size gate. Enumerating mesh
            // components directly removed that accident, because the intact mesh is still a
            // component of the actor; MW5 does not destroy the actor, it HIDES the intact mesh
            // and shows a rubble mesh in its place.
            //
            // So ask the component whether it is actually being drawn. Via ReadBoolProp,
            // because bVisible and bHiddenInGame are packed bitfields and a raw byte test trips
            // on unrelated bits - a lesson this file already paid for once.
            if (!ReadBoolProp(c, L"bVisible")) continue;
            if (ReadBoolProp(c, L"bHiddenInGame")) continue;

            std::memset(LbBuf.data(), 0, LbBuf.size());
            c->process_event(LocalBoundsFn, LbBuf.data());
            float mn[3], mx[3];
            std::memcpy(mn, LbBuf.data() + LbMin, sizeof(mn));
            std::memcpy(mx, LbBuf.data() + LbMax, sizeof(mx));

            std::memset(ClBuf.data(), 0, ClBuf.size());
            c->process_event(CompLocFn, ClBuf.data());
            float loc[3];
            std::memcpy(loc, ClBuf.data() + ClRet, sizeof(loc));

            // Local bounds are around the mesh's own pivot, which is not always its centre, so
            // the box is the local centre offset from the component's world location. Scale is
            // not read: a mesh component scaled in the level is rare enough, and reading the
            // transform for it would cost a third call per component.
            float ex = (mx[0] - mn[0]) * 0.5f;
            float ey = (mx[1] - mn[1]) * 0.5f;
            const float ez = (mx[2] - mn[2]) * 0.5f;
            if (!(ex > 0.0f && ey > 0.0f && ez > 0.0f)) continue;

            // Rotation is not read, so widen to the larger horizontal half-extent: a rotated
            // building is over-boxed rather than clipped.
            const float h = (ex > ey) ? ex : ey;
            ex = ey = h;
            if (h < kMinCompHalfCm) continue;

            TerrainStructure &s = out[written++];
            s.X = loc[0] + (mx[0] + mn[0]) * 0.5f;
            s.Y = loc[1] + (mx[1] + mn[1]) * 0.5f;
            s.Z = loc[2] + (mx[2] + mn[2]) * 0.5f;
            s.EX = ex; s.EY = ey; s.EZ = ez;
        }
        return written;
    }

    ULONGLONG StructScanTick   = 0;
    uint32_t  StructScanCount  = 0;
    // Last scan's boxes, for spotting the ones that disappear.
    TerrainStructure ScanBoxes[TERR_MAX_STRUCTURES];
    TerrainStructure PrevStruct[TERR_MAX_STRUCTURES];
    uint32_t         PrevStructCount = 0;
    // Re-scan interval. Buildings are static until something shoots them, so this only has to be
    // fast enough that a flattened structure stops being drawn while the pilot still cares.
    static constexpr ULONGLONG kStructScanMs = 5000;

    // Only structures this close to the mech are published. The 3D page draws a 1000 m horizon,
    // so half as much again covers it with margin for the pilot walking between scans.
    static constexpr float kStructRangeCm = 150000.0f; // 1.5 km

    bool EnsureBoundsLayout(API::UObject* anyActor) {
        if (BoundsChecked) return BoundsReady;
        if (!anyActor || !anyActor->get_class()) return false;
        BoundsChecked = true;

        for (API::UStruct* st = anyActor->get_class(); st && !BoundsFn; st = st->get_super_struct())
            BoundsFn = static_cast<API::UClass*>(st)->find_function(L"GetActorBounds");
        if (!BoundsFn) { LogError("structures: GetActorBounds not found - layer disabled"); return false; }

        BoundsFrame = BoundsFn->get_properties_size();
        for (auto* f = BoundsFn->get_child_properties(); f; f = f->get_next()) {
            auto* p = reinterpret_cast<API::FProperty*>(f);
            const std::wstring nm = f->get_fname()->to_string();
            const int32_t off = p->get_offset();
            if      (nm == L"bOnlyCollidingComponents") BoundsOnlyColliding = off;
            else if (nm == L"Origin")                   BoundsOrigin        = off;
            else if (nm == L"BoxExtent")                BoundsExtent        = off;
        }
        if (BoundsFrame <= 0 || BoundsOrigin < 0 || BoundsExtent < 0) {
            LogError("structures: GetActorBounds layout incomplete (frame %d, origin %d, extent %d)"
                     " - layer disabled", BoundsFrame, BoundsOrigin, BoundsExtent);
            return false;
        }
        // Same guard as the trace frame, for the same reason: UE4 FVector is three floats and
        // UE5 is three doubles, and reading the wrong one gives plausible nonsense.
        const int32_t gap = BoundsExtent - BoundsOrigin;
        if (gap != 12) {
            LogError("structures: Origin->BoxExtent gap %d, expected 12 - layer disabled", gap);
            return false;
        }

        BoundsFrameBuf.assign(static_cast<size_t>(BoundsFrame) + 64, 0);
        BoundsReady = true;
        LogInfo("structures: GetActorBounds ready (frame %d, Origin@%d, BoxExtent@%d)",
                BoundsFrame, BoundsOrigin, BoundsExtent);
        return true;
    }

    bool ActorBounds(API::UObject* actor, TerrainStructure& out) {
        if (!BoundsReady || !actor) return false;
        std::memset(BoundsFrameBuf.data(), 0, BoundsFrameBuf.size());
        // Colliding components only: a building's visual bounds include banners and aerials that
        // are not part of the shape a mech has to walk around.
        if (BoundsOnlyColliding >= 0) BoundsFrameBuf[BoundsOnlyColliding] = 1;
        actor->process_event(BoundsFn, BoundsFrameBuf.data());

        float o[3], e[3];
        std::memcpy(o, BoundsFrameBuf.data() + BoundsOrigin, sizeof(o));
        std::memcpy(e, BoundsFrameBuf.data() + BoundsExtent, sizeof(e));
        // A zero or absurd extent means the call told us nothing; publishing it would put a
        // degenerate box on the map that the pilot cannot interpret.
        if (!(e[0] > 50.0f && e[1] > 50.0f && e[2] > 50.0f)) return false;
        if (e[0] > 100000.0f || e[1] > 100000.0f || e[2] > 100000.0f) return false;

        out.X = o[0]; out.Y = o[1]; out.Z = o[2];
        out.EX = e[0]; out.EY = e[1]; out.EZ = e[2];
        return true;
    }

    // ---- per-class structure census -------------------------------------------------------
    //
    // The hit census answered "what are the terrain rays touching"; this answers the different
    // question "what is actually in the structures list, and how big is each kind". Without it
    // the only available description of the problem is the one from the screen - "large
    // rectangles and small cubes" - which names no class and supports no rule.
    //
    // Records the largest footprint seen per class, because that is what identifies an actor
    // that owns a whole row of things rather than one thing.
    struct StructClassInfo {
        const void *Cls = nullptr;
        char        Name[64] = {};
        uint32_t    Kept = 0, Dropped = 0;
        float       MaxSpanM = 0.0f, MinSpanM = 1.0e9f;
    };
    static constexpr int kMaxStructClasses = 48;
    StructClassInfo StructClasses[kMaxStructClasses];
    int  StructClassCount = 0;
    bool StructCensusLogged = false;
    float NoteSpanM = 0.0f;   // set by ActorBounds' caller just before NoteStructClass

    void NoteStructClass(API::UObject* a, bool kept) {
        if (!a) return;
        API::UStruct* cls = a->get_class();
        if (!cls) return;
        StructClassInfo* e = nullptr;
        for (int i = 0; i < StructClassCount; ++i)
            if (StructClasses[i].Cls == static_cast<const void*>(cls)) { e = &StructClasses[i]; break; }
        if (!e) {
            if (StructClassCount >= kMaxStructClasses) return;
            e = &StructClasses[StructClassCount++];
            e->Cls = static_cast<const void*>(cls);
            const std::wstring nm = cls->get_fname()->to_string();
            size_t k = 0;
            for (; k + 1 < sizeof(e->Name) && k < nm.size(); ++k)
                e->Name[k] = (nm[k] < 128) ? static_cast<char>(nm[k]) : '?';
            e->Name[k] = '\0';
        }
        (kept ? e->Kept : e->Dropped)++;
        if (NoteSpanM > e->MaxSpanM) e->MaxSpanM = NoteSpanM;
        if (NoteSpanM < e->MinSpanM) e->MinSpanM = NoteSpanM;
    }

    void LogStructCensus() {
        if (StructCensusLogged || StructClassCount <= 0) return;
        StructCensusLogged = true;
        LogInfo("structures: class census - %d distinct classes", StructClassCount);
        for (int i = 0; i < StructClassCount; ++i) {
            const StructClassInfo& e = StructClasses[i];
            LogInfo("structures:   %-44s kept %u, dropped %u, footprint %.0f..%.0f m",
                    e.Name, e.Kept, e.Dropped,
                    (e.MinSpanM > 1.0e8f) ? 0.0f : e.MinSpanM, e.MaxSpanM);
        }
    }

    void ScanStructures(API::UObject* pawn, const FVector3f& ploc) {
        if (!Terr || !DestructibleClass || !pawn) return;
        const ULONGLONG now = GetTickCount64();
        if (StructScanTick && now - StructScanTick < kStructScanMs) return;
        StructScanTick = now;

        ResolveGameplayStatics();
        int n = 0;
        API::UObject** list = GetActorsOfClass(pawn, DestructibleClass, n);
        if (!list) return;
        if (!EnsureBoundsLayout(n > 0 ? list[0] : nullptr)) return;
        // NOTE the ordering: GaacBuf is reused by every GetActorsOfClass call, so this list must
        // be fully consumed before TrackDropships issues its own enumeration below.

        // QueryPerformanceCounter directly: this file has no NowMs helper, and the one place
        // that already times anything (the terrain timing probe) does the same thing inline.
        LARGE_INTEGER qfreq{}, qt0{}, qt1{};
        QueryPerformanceFrequency(&qfreq);
        QueryPerformanceCounter(&qt0);

        const bool byComponent = EnsureComponentLayout(n > 0 ? list[0] : nullptr);

        uint32_t written = 0, tooSmall = 0, dropped = 0, outOfRange = 0;
        // 5 km radar mode (2026-10-15): buildings out to 5 km, NEAREST FIRST so the 512-box cap drops the farthest ones
        // (in enumeration order it would drop whatever came last, near ones included). 1.5 km otherwise, order as before.
        const float structRangeCm = FarOn ? LIDAR_FAR_RANGE_CM : kStructRangeCm;
        std::vector<std::pair<float, int>> order;
        order.reserve(n > 0 ? (size_t)n : 0);
        for (int i = 0; i < n; ++i) {
            float d2 = 0.0f;
            if (FarOn && list[i]) {
                FVector3f ol{}; float oy = 0.0f;
                if (ReadActorPose(list[i], ol, oy)) d2 = (ol.X - ploc.X) * (ol.X - ploc.X) + (ol.Y - ploc.Y) * (ol.Y - ploc.Y);
            }
            order.push_back({d2, i});
        }
        if (FarOn) std::stable_sort(order.begin(), order.end(), [](const std::pair<float, int>& x, const std::pair<float, int>& y) { return x.first < y.first; });
        for (size_t oi = 0; oi < order.size() && written < (uint32_t)TERR_MAX_STRUCTURES; ++oi) {
            const int i = order[oi].second;
            API::UObject* a = list[i];
            if (!a || IsSpentActor(a)) continue;   // destroyed or hidden: stop drawing it

            // ---- RANGE, and why the list was arbitrary without it ------------------------
            //
            // The field log read "512 boxes from 651 actors ... (CAPPED)" with only 6 distinct
            // classes in the census. That is the whole explanation for "some buildings are here
            // and some are not": there was no selection rule at all. The scan walked the actor
            // list in enumeration order and stopped dead when the array filled, so inclusion was
            // decided by whatever order the engine happened to return - which is to say, by
            // nothing.
            //
            // The display only ever shows a 1000 m horizon, so publishing a whole 651-actor map
            // was never useful. Filtering by distance makes the cap almost unreachable AND makes
            // what survives the right things: the ones near the pilot.
            FVector3f aloc{};
            float ayaw = 0.0f;
            if (ReadActorPose(a, aloc, ayaw)) {
                const float ddx = aloc.X - ploc.X, ddy = aloc.Y - ploc.Y;
                if (ddx * ddx + ddy * ddy > structRangeCm * structRangeCm) { ++outOfRange; continue; }
            }

            if (byComponent) {
                // The good path. One box per mesh component, so the unit published is the unit
                // drawn - no size gates, no coherence test, no class rules. Everything those
                // were compensating for is gone by construction.
                TerrainStructure boxes[kMaxCompsPerActor];
                const int room = (int)((uint32_t)TERR_MAX_STRUCTURES - written);
                const int got  = ComponentBoxes(a, boxes, (room < kMaxCompsPerActor) ? room : kMaxCompsPerActor);
                if (got <= 0) { ++dropped; NoteStructClass(a, false); continue; }
                NoteSpanM = 2.0f * boxes[0].EX * 0.01f;
                NoteStructClass(a, true);
                for (int k = 0; k < got; ++k) ScanBoxes[written++] = boxes[k];
                continue;
            }

            // FALLBACK, used only when the component reflection did not resolve. One box per
            // actor, with the blunt size gate that was the best available before components -
            // kept so a layout change degrades the layer instead of deleting it.
            TerrainStructure s{};
            if (!ActorBounds(a, s)) continue;
            const float spanX = s.EX, spanY = s.EY;
            NoteSpanM = 2.0f * ((spanX > spanY) ? spanX : spanY) * 0.01f;
            if (spanX < 250.0f || spanY < 250.0f) { ++tooSmall; NoteStructClass(a, false); continue; }
            NoteStructClass(a, true);
            ScanBoxes[written++] = s;
        }
        QueryPerformanceCounter(&qt1);
        const double ms = (qfreq.QuadPart > 0)
                        ? (double)(qt1.QuadPart - qt0.QuadPart) * 1000.0 / (double)qfreq.QuadPart
                        : 0.0;

        // A building that has gone left its pre-collapse height baked into the terrain, exactly
        // as the dropship does. Same repair: find the boxes that vanished since the last scan
        // and re-trace the ground under them. Matched by centre within half a tile - identity by
        // position, because holding actor pointers across five seconds is how this plugin has
        // crashed before.
        if (PrevStructCount > 0) {
            for (uint32_t i = 0; i < PrevStructCount; ++i) {
                bool stillThere = false;
                for (uint32_t j = 0; j < written && !stillThere; ++j) {
                    const float dx = ScanBoxes[j].X - PrevStruct[i].X;
                    const float dy = ScanBoxes[j].Y - PrevStruct[i].Y;
                    if (dx * dx + dy * dy < (TERR_TILE_CM * 0.5f) * (TERR_TILE_CM * 0.5f))
                        stillThere = true;
                }
                if (stillThere) continue;
                const uint32_t m = InvalidateTilesInBox(
                    WorldBox{PrevStruct[i].X, PrevStruct[i].Y, PrevStruct[i].Z, PrevStruct[i].EX, PrevStruct[i].EY});
                if (m) LogInfo("terrain: structure gone - re-tracing %u tile(s) under it", m);
            }
        }
        PrevStructCount = (written < (uint32_t)TERR_MAX_STRUCTURES) ? written : TERR_MAX_STRUCTURES;
        std::memcpy(PrevStruct, ScanBoxes, sizeof(TerrainStructure) * PrevStructCount);

        ++Terr->H.FrameCounter;
        // Nothing is published while the layer is off; the scan exists only to drive the terrain
        // re-traces. Scanned is still set, so a consumer can tell "looked, found none" from
        // "never looked".
        Terr->H.StructureCount   = kPublishStructures ? written : 0;
        Terr->H.StructuresScanned = 1;
        if (kPublishStructures && written)
            std::memcpy(Terr->H.Structures, ScanBoxes, sizeof(TerrainStructure) * written);
        Terr->H.FrameCounterEnd  = Terr->H.FrameCounter;

        // Logged only when the count moves or the scan is slow, so a static map says this once.
        if (written != StructScanCount || ms > 2.0) {
            LogInfo("structures: %u boxes from %d actors in %.2f ms (%s; %u out of range, "
                    "%u no visible mesh, %u tiny)%s",
                    written, n, ms, byComponent ? "per component" : "ACTOR BOUNDS FALLBACK",
                    outOfRange, dropped, tooSmall,
                    (written >= (uint32_t)TERR_MAX_STRUCTURES) ? " (CAPPED)" : "");
            StructScanCount = written;
        }
        LogStructCensus();   // once per map, after the first full scan

        // LAST, because it runs its own GetActorsOfClass and that overwrites GaacBuf.
        // DROPSHIP PAD RE-TRACING IS OFF.
        //
        // The idea was sound - a dropship sits on the ground during tracing, so its hull gets
        // sampled as terrain and stays there as a mound after it leaves. The cure turned out to
        // cost far more than the disease. Each departure marks its tiles incomplete and they
        // vanish from the 3D mesh until the re-trace finishes, taking the seam row of every
        // neighbouring tile with them. One mission logged SIXTEEN departures re-tracing up to
        // five tiles each - about forty tile rebuilds - which is where the rows of holes came
        // from, on a map where the pilot never rode a dropship at all. Allied and enemy drops
        // trigger it just as readily as the player's.
        //
        // A stale mound is a small cosmetic wart in one spot. Holes are the ground disappearing
        // out from under whole rows, repeatedly, for the whole mission. The per-hit actor filter
        // still rejects MWDropship while tracing, so newly traced tiles stay clean either way.
        if (kRetraceDropshipPads)
            TrackDropships(pawn);
    }

    // =======================================================================================
    // RE-TRACING WHAT A TRANSIENT LEFT BEHIND
    //
    // The height cache is permanent; the dropship is not. It stands at the drop point for the
    // first few seconds - exactly when the tiles beneath it are traced - and then flies away,
    // leaving its shape baked into the ground for the rest of the mission. The pilot sees a
    // mound where the ship used to be.
    //
    // Class rejection cannot fix this. The hull and the bay doors have classes we can match, but
    // a Leopard's ramp, skids, landing pad and interior decking are plain StaticMeshActors,
    // indistinguishable from the mesh-built terrain that makes up a third of every map. Reject
    // those and you would punch holes in real hillsides.
    //
    // So: remember where the ship stood, and when it is gone, MEASURE the ground again. Marking
    // a tile TTS_Filling with a zeroed cursor is all it takes - the existing fill loop picks it
    // up on the next tick and re-traces it against a now-empty world. Measured at 0.20 s per
    // tile, and a dropship covers one or two, so the whole repair is under half a second.
    //
    // Interpolating the footprint instead was the alternative and it is strictly worse: it
    // guesses at ground we are perfectly capable of measuring once the obstruction leaves.
    // =======================================================================================
    struct WorldBox { float X, Y, Z, EX, EY; };
    // How far a dropship may sit from a recorded footprint and still count as standing on it.
    static constexpr float kTransientMoveCm = 10000.0f; // 100 m
    static constexpr float kTransientLiftCm = 15000.0f; // 150 m of altitude change
    static constexpr int kMaxTransientBoxes = 8;
    WorldBox TransientBox[kMaxTransientBoxes];
    int      TransientBoxCount = 0;
    bool     TransientSeen     = false;   // a dropship has been present at some point
    uint32_t TerrRetraceTiles  = 0;

    UEVR_UClassHandle DropshipClass = nullptr;
    SlowTimer SlowT{"LidarExport"}; // SLOW-STEP TIMER (slowtimer1): hitch hunting

    // Mark every tile the box touches for re-tracing. Complete tiles go back to Filling; tiles
    // still filling are left alone because they will finish against the current world anyway.
    uint32_t InvalidateTilesInBox(const WorldBox& b) {
        if (!Terr) return 0;
        // One sample of padding: the ship's influence does not stop cleanly at its bounds, and
        // re-tracing one extra ring of samples is far cheaper than leaving a rim of the mound.
        const float pad = TERR_SPACING_CM;
        const int32_t tx0 = (int32_t)std::floor((b.X - b.EX - pad) / TERR_TILE_CM);
        const int32_t tx1 = (int32_t)std::floor((b.X + b.EX + pad) / TERR_TILE_CM);
        const int32_t ty0 = (int32_t)std::floor((b.Y - b.EY - pad) / TERR_TILE_CM);
        const int32_t ty1 = (int32_t)std::floor((b.Y + b.EY + pad) / TERR_TILE_CM);

        uint32_t marked = 0;
        for (int32_t ty = ty0; ty <= ty1; ++ty) {
            for (int32_t tx = tx0; tx <= tx1; ++tx) {
                const int cell = TerrCellIndex(tx, ty);
                if (cell < 0 || TerrSlotOf[cell] < 0) continue;
                const int slot = TerrSlotOf[cell];
                TerrainSlotInfo& s = Terr->H.Slots[slot];
                if (s.State != TTS_Complete) continue;

                ++Terr->H.FrameCounter;
                s.State = TTS_Filling;          // consumers drop it; the fill loop reclaims it
                if (s.Hits > 0 && Terr->H.Complete > 0) --Terr->H.Complete;
                s.Hits = 0;
                Terr->H.FrameCounterEnd = Terr->H.FrameCounter;
                TerrFillCursor[slot] = 0;
                ++marked;
            }
        }
        TerrRetraceTiles += marked;
        return marked;
    }

    // Called from the structure scan, which already has the world context and the bounds frame.
    void TrackDropships(API::UObject* pawn) {
        if (!Terr || !DropshipClass || !pawn || !BoundsReady) return;

        int n = 0;
        API::UObject** list = GetActorsOfClass(pawn, DropshipClass, n);

        // Where every live dropship is RIGHT NOW.
        int live = 0;
        WorldBox seen[kMaxTransientBoxes];
        for (int i = 0; i < n && live < kMaxTransientBoxes; ++i) {
            if (!list[i] || IsSpentActor(list[i])) continue;
            TerrainStructure s{};
            if (!ActorBounds(list[i], s)) continue;
            seen[live++] = WorldBox{s.X, s.Y, s.Z, s.EX, s.EY};
        }

        // ---- DEPARTURE IS MOVEMENT, NOT DISAPPEARANCE --------------------------------------
        //
        // The first version waited for the dropship to stop being enumerated, on the assumption
        // that leaving meant being destroyed. It does not: the Leopard flies off and the actor
        // persists, so GetActorsOfClass kept returning it, `live` never reached zero, and the
        // repair branch was simply unreachable. The field log proved it - MWDropship resolved,
        // "dropship departed" never once logged, and the mound stayed.
        //
        // So a recorded footprint is VACATED when no live dropship is still standing on it.
        // That covers both endings - flown away, or destroyed - because neither leaves a ship
        // near the spot it was occupying.
        uint32_t marked = 0;
        for (int i = 0; i < TransientBoxCount; ++i) {
            const WorldBox& b = TransientBox[i];
            bool occupied = false;
            for (int j = 0; j < live && !occupied; ++j) {
                const float dx = seen[j].X - b.X, dy = seen[j].Y - b.Y, dz = seen[j].Z - b.Z;
                // Generous horizontally, because a ship shuffles while unloading; tighter
                // vertically, because lift-off is the earliest unambiguous sign it has gone and
                // the ground underneath is free the moment it rises.
                if (dx * dx + dy * dy < kTransientMoveCm * kTransientMoveCm &&
                    (dz < kTransientLiftCm && dz > -kTransientLiftCm))
                    occupied = true;
            }
            if (!occupied) marked += InvalidateTilesInBox(b);
        }
        if (marked)
            LogInfo("terrain: dropship left its pad - re-tracing %u tile(s) it was standing on", marked);

        // The current positions become the new record. A box only survives while a ship is still
        // on it, so a vacated one is not carried forward and cannot fire twice.
        for (int i = 0; i < live; ++i) TransientBox[i] = seen[i];
        TransientBoxCount = live;
        if (live > 0) TransientSeen = true;
    }

    // MWDestructibleObject_C by object scan, the same trick the vehicle and projectile bases
    // use. find_uobject on these returns null here, and waiting for a terrain ray to land on a
    // building is not a resolution strategy - a map can have buildings nowhere near the rays.
    // THE DROPSHIP CLASS (dsclass1, 2026-10-08). Used for one thing: DropshipScan's GetActorsOfClass, which puts the
    // dropship hulls on the maps (TrackDropships also takes it, but dropship-pad re-tracing is off). MWDropship is a
    // NATIVE class, so one lookup by name finds it - the whole-object-array walk that used to live here was for the
    // building class (MWDestructibleObject_C, a blueprint the lookup could not find) and the dropship rode along;
    // with the structure scan gone, nothing needs the walk. Not found: said once, and no hulls are drawn.
    void ResolveDestructibleClass() {
        static bool tried = false;
        if (DropshipClass || tried) return;
        tried = true;
        if (auto* c = API::get()->find_uobject<API::UClass>(L"Class /Script/MechWarrior.MWDropship")) {
            DropshipClass = reinterpret_cast<UEVR_UClassHandle>(c);
            LogInfo("dropships: MWDropship class found");
        } else
            LogWarn("dropships: MWDropship class not found - dropship hulls will not be drawn");
    }

    // =======================================================================================
    // MECH SILHOUETTES
    //
    // Joint positions for the nearest few mechs, so a contact can be drawn as the mech it is
    // rather than as a triangle. TerrainShared.h carries the why; this is the how.
    //
    // GetSocketLocation, not GetBoneTransform. Both are BlueprintCallable, but a socket location
    // returns a bare FVector where a transform returns an FTransform - a quat, a translation and
    // a scale with alignment padding, three times the frame to measure for no information we
    // need. On a skeletal mesh every BONE name is also a valid socket name, so this reads bones
    // through the simpler door.
    //
    // The joint names come from the game's own asset naming: the mod paks ship damage meshes
    // called GRN_Thigh_Left, GRN_Knee_Left, GRN_Calf_Left, GRN_Foot_Left, GRN_Pelvis_* and
    // weapon mounts on Torso_Centre / Torso_Left / Torso_Right / Torso_Head / Upperarm_*. UE
    // rigs conventionally name a damage mesh after the bone it hangs on, so these are very
    // likely the bone names with the chassis prefix dropped - "very likely" being exactly why
    // the collapse check below exists.
    // =======================================================================================
    static const wchar_t* JointName(int j) {
        switch (j) {
            case TJ_Pelvis:      return L"Pelvis";
            case TJ_ThighL:      return L"Thigh_Left";
            case TJ_KneeL:       return L"Knee_Left";
            case TJ_CalfL:       return L"Calf_Left";
            case TJ_FootL:       return L"Foot_Left";
            case TJ_ThighR:      return L"Thigh_Right";
            case TJ_KneeR:       return L"Knee_Right";
            case TJ_CalfR:       return L"Calf_Right";
            case TJ_FootR:       return L"Foot_Right";
            case TJ_TorsoCentre: return L"Torso_Centre";
            case TJ_TorsoHead:   return L"Torso_Head";
            case TJ_TorsoL:      return L"Torso_Left";
            case TJ_TorsoR:      return L"Torso_Right";
            case TJ_UpperarmL:   return L"Upperarm_Left";
            case TJ_UpperarmR:   return L"Upperarm_Right";
        }
        return L"Pelvis";
    }

    API::UFunction*   SocketLocFn   = nullptr;
    UEVR_UClassHandle SkelCompClass = nullptr;
    int32_t SlFrame = -1, SlName = -1, SlRet = -1;
    bool    SilChecked = false, SilReady = false, SilNamesWarned = false;
    std::vector<uint8_t> SlBuf;
    // SHELVED. The pipeline works - the bone names inferred from the mod paks resolved, the
    // spread check passed, three mechs posed correctly - but fifteen joints joined by thin lines
    // reads as a stick insect, not a mech. Recognising a chassis needs BULK (slab torso, boxy
    // legs) and the joint data describes a pose, not a shape. Left intact and switched off
    // rather than deleted: if the renderer ever grows tapered limbs and a torso quad, this is
    // the half that already works.
    static constexpr bool kSilhouettesEnabled = false;
    ULONGLONG SilScanTick = 0;
    static constexpr ULONGLONG kSilScanMs  = 200;      // 5 Hz; the eye interpolates the rest
    static constexpr float     kSilRangeCm = 60000.0f; // 600 m; beyond that it is a triangle anyway

    bool EnsureSilhouetteLayout() {
        if (SilChecked) return SilReady;
        SilChecked = true;

        auto* skc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");
        SkelCompClass = reinterpret_cast<UEVR_UClassHandle>(skc);
        if (!skc) { LogError("silhouette: SkeletalMeshComponent class not found"); return false; }

        for (API::UStruct* st = skc; st && !SocketLocFn; st = st->get_super_struct())
            SocketLocFn = static_cast<API::UClass*>(st)->find_function(L"GetSocketLocation");
        if (!SocketLocFn) { LogError("silhouette: GetSocketLocation not found"); return false; }

        SlFrame = SocketLocFn->get_properties_size();
        for (auto* f = SocketLocFn->get_child_properties(); f; f = f->get_next()) {
            auto* p = reinterpret_cast<API::FProperty*>(f);
            const std::wstring nm = f->get_fname()->to_string();
            if      (nm == L"InSocketName") SlName = p->get_offset();
            else if (nm == L"ReturnValue")  SlRet  = p->get_offset();
        }
        if (SlFrame <= 0 || SlName < 0 || SlRet < 0) {
            LogError("silhouette: GetSocketLocation layout incomplete (frame %d, name %d, ret %d)",
                     SlFrame, SlName, SlRet);
            return false;
        }
        SlBuf.assign(static_cast<size_t>(SlFrame) + 64, 0);
        SilReady = true;
        LogInfo("silhouette: ready (GetSocketLocation frame %d, InSocketName@%d, ret@%d)",
                SlFrame, SlName, SlRet);
        return true;
    }

    // One joint. The FName is CONSTRUCTED through the engine's own name table - a raw string
    // cannot be poked into an FName slot, and EFindName::Find is used rather than Add so a
    // misspelt joint cannot pollute the global table with junk entries.
    void JointLocation(API::UObject* comp, const wchar_t* bone, float out[3]) {
        std::memset(SlBuf.data(), 0, SlBuf.size());
        API::FName name(bone, API::FName::EFindName::Find);
        std::memcpy(SlBuf.data() + SlName, &name, sizeof(name));
        comp->process_event(SocketLocFn, SlBuf.data());
        std::memcpy(out, SlBuf.data() + SlRet, sizeof(float) * 3);
    }

    void ScanSilhouettes(API::UObject* pawn, const FVector3f& ploc) {
        if (!kSilhouettesEnabled) { if (Terr) { Terr->H.SilhouetteCount = 0; Terr->H.SilhouettesOk = 0; } return; }
        if (!Terr || !pawn || !BaseMechClass) return;
        const ULONGLONG now = GetTickCount64();
        if (SilScanTick && now - SilScanTick < kSilScanMs) return;
        SilScanTick = now;
        if (!EnsureSilhouetteLayout() || !CompReady) { Terr->H.SilhouettesOk = 0; return; }

        ResolveGameplayStatics();
        Breadcrumb("scan: enumerate mechs");
        int n = 0;
        API::UObject** list = GetActorsOfClass(pawn, BaseMechClass, n);
        if (!list) return;

        // Pointers copied out first: GaacBuf is reused by every enumeration, and the component
        // lookup below issues its own.
        API::UObject* mechs[64];
        int mechCount = 0;
        for (int i = 0; i < n && mechCount < 64; ++i)
            if (list[i] && list[i] != pawn && !IsSpentActor(list[i])) mechs[mechCount++] = list[i];

        uint32_t written = 0;
        bool collapsed = true;
        for (int i = 0; i < mechCount && written < (uint32_t)TERR_MAX_SILHOUETTES; ++i) {
            FVector3f aloc{}; float ayaw = 0.0f;
            if (!ReadActorPose(mechs[i], aloc, ayaw)) continue;
            const float dx = aloc.X - ploc.X, dy = aloc.Y - ploc.Y;
            if (dx * dx + dy * dy > kSilRangeCm * kSilRangeCm) continue;

            // CbcBuf is not zeroed per call - see ForEachLiveComponent.
            void* cls = reinterpret_cast<void*>(SkelCompClass);
            std::memcpy(CbcBuf.data() + CbcClass, &cls, sizeof(void*));
            mechs[i]->process_event(CompsByClassFn, CbcBuf.data());
            FScriptArrayLayout arr{};
            std::memcpy(&arr, CbcBuf.data() + CbcRet, sizeof(arr));
            if (!arr.Data || arr.Num <= 0) continue;
            API::UObject* comp = reinterpret_cast<API::UObject**>(arr.Data)[0];
            if (!comp) continue;

            TerrainSilhouette sil{};
            for (int j = 0; j < TJ_COUNT; ++j) {
                float p[3] = {0, 0, 0};
                JointLocation(comp, JointName(j), p);
                sil.X[j] = p[0]; sil.Y[j] = p[1]; sil.Z[j] = p[2];
            }

            // An unknown socket name does NOT fail loudly - UE returns the component's own
            // location instead - so a wrong name list produces fifteen coincident points and a
            // mech drawn as a dot. Measuring the spread is the only honest way to tell that
            // apart from a mech standing still.
            const float spread = std::fabs(sil.Z[TJ_TorsoHead] - sil.Z[TJ_FootL]) +
                                 std::fabs(sil.X[TJ_FootL] - sil.X[TJ_FootR]) +
                                 std::fabs(sil.Y[TJ_FootL] - sil.Y[TJ_FootR]);
            if (spread > 100.0f) collapsed = false;

            sil.Valid   = 1;
            sil.Hostile = 1;
            Terr->H.Silhouettes[written++] = sil;
        }

        ++Terr->H.FrameCounter;
        Terr->H.SilhouetteCount = written;
        Terr->H.SilhouettesOk   = (written > 0 && !collapsed) ? 1 : 0;
        Terr->H.FrameCounterEnd = Terr->H.FrameCounter;

        if (written > 0 && collapsed && !SilNamesWarned) {
            SilNamesWarned = true;
            LogError("silhouette: every joint resolved to the same point - the bone names are "
                     "wrong for this skeleton (tried Pelvis / Thigh_Left / Knee_Left / Calf_Left "
                     "/ Foot_Left / Torso_Centre / Torso_Head / Upperarm_Left ...). Consumers "
                     "fall back to triangles.");
        } else if (written > 0 && !collapsed && !SilNamesWarned) {
            SilNamesWarned = true;
            LogInfo("silhouette: joint names resolve - %u mech(s) posed", written);
        }
    }


    // =======================================================================================
    // MECH IDENTITY - chassis and variant
    //
    // Two reads, both cheap, both cached per actor for the life of the mission because neither
    // can change while a mech is alive.
    //
    //   CHASSIS  - every mech carries a MechTargetComponent whose CLASS is chassis-specific:
    //              viper_Target_C on a Viper, catapult_Target_C on a Catapult. Matches the
    //              per-chassis /Objects/Mechs/<Chassis>/Target asset in the paks, and is
    //              corroborated by the body mesh name (Viper_SKM).
    //
    //   VARIANT  - GetMechDataAsset() returns the mech's MWMechDataAsset, and that asset is
    //              named after the variant: VPR-A_MDA, GRN-A_MDA, DSY-001H_MDA, matching the
    //              MDA asset paths in the paks and the codes in the YAML packs' mechs.json.
    //              Strip the suffix and the variant is left.
    //
    // The route came from MWVR's own VR_HUD_TargetObject widget, which ships uncooked in the
    // modder source files and does exactly this. Worth recording why: four missions were spent
    // walking the ACTOR's property graph looking for the variant, and it was never there - a
    // FUNCTION returns the asset that holds it, and no property walk can find a function. When
    // something in the game already displays the data you want, read that first.
    // =======================================================================================
    API::UFunction* GetMdaFn = nullptr;
    int32_t MdaRet = -1;
    bool    MdaChecked = false, MdaReady = false, IdentityLogged = false;
    std::vector<uint8_t> MdaBuf;

    void Narrow(const std::wstring& in, char* out, size_t cap) {
        size_t k = 0;
        for (; k + 1 < cap && k < in.size(); ++k) out[k] = (in[k] < 128) ? (char)in[k] : '?';
        out[k] = '\0';
    }

    bool EnsureMdaFn(API::UObject* mech) {
        if (MdaChecked) return MdaReady;
        MdaChecked = true;
        for (API::UStruct* st = mech->get_class(); st && !GetMdaFn; st = st->get_super_struct())
            GetMdaFn = static_cast<API::UClass*>(st)->find_function(L"GetMechDataAsset");
        if (!GetMdaFn) { LogError("identity: GetMechDataAsset not found"); return false; }

        const int32_t frame = GetMdaFn->get_properties_size();
        for (auto* f = GetMdaFn->get_child_properties(); f; f = f->get_next())
            if (f->get_fname()->to_string() == L"ReturnValue")
                MdaRet = reinterpret_cast<API::FProperty*>(f)->get_offset();
        if (frame <= 0 || MdaRet < 0) {
            LogError("identity: GetMechDataAsset layout incomplete (frame %d, ret %d)", frame, MdaRet);
            return false;
        }
        MdaBuf.assign(static_cast<size_t>(frame) + 64, 0);
        MdaReady = true;
        return true;
    }

    // "viper", "Catapult" - empty on failure.
    bool ChassisOf(API::UObject* mech, char out[32]) {
        out[0] = '\0';
        auto* slot = mech->get_property_data<void*>(L"MechTargetComponent");
        if (!slot || !*slot) return false;
        auto* comp = reinterpret_cast<API::UObject*>(*slot);
        if (!comp || !comp->get_class() || !comp->get_class()->get_fname()) return false;
        std::wstring cn = comp->get_class()->get_fname()->to_string();
        const size_t cut = cn.find(L"_Target");
        if (cut != std::wstring::npos) cn = cn.substr(0, cut);
        Narrow(cn, out, 32);
        return out[0] != '\0';
    }

    // "VPR-A", "DSY-001H" - empty on failure.
    bool VariantOf(API::UObject* mech, char out[32]) {
        out[0] = '\0';
        if (!EnsureMdaFn(mech)) return false;
        std::memset(MdaBuf.data(), 0, MdaBuf.size());
        mech->process_event(GetMdaFn, MdaBuf.data());
        auto* mda = *reinterpret_cast<API::UObject**>(MdaBuf.data() + MdaRet);
        if (!mda || !mda->get_fname()) return false;
        std::wstring an = mda->get_fname()->to_string();
        const size_t cut = an.rfind(L"_MDA");
        if (cut != std::wstring::npos) an = an.substr(0, cut);
        Narrow(an, out, 32);
        return out[0] != '\0';
    }

    // One line, once, so a silent failure is still visible without a probe pass.
    void LogIdentityOnce(API::UObject* mech) {
        if (IdentityLogged || !mech) return;
        IdentityLogged = true;
        char c[32], v[32];
        const bool haveC = ChassisOf(mech, c);
        const bool haveV = VariantOf(mech, v);
        LogInfo("identity: chassis '%s', variant '%s'", haveC ? c : "?", haveV ? v : "?");
    }

    // ---- mech identity and condition --------------------------------------------------------
    // 250 ms: the doll and the label are read at a glance, not tracked frame to frame, and this
    // is by far the heaviest per-mech read in the plugin. 1.2 km matches what the 3D page can
    // usefully draw a doll at.
    static constexpr bool      kMechInfoEnabled  = true;
    // ---- the three new reads, each with its own switch ---------------------------------------
    //
    // Two builds in a row crashed inside code added here, and the buffered log could not say
    // which part. These exist so the answer is one rebuild away instead of one mission away:
    // flip one to false, and that feature reverts to exactly what shipped before it.
    //
    // kEquipmentWeapons is OFF by default and that is deliberate. It has never once completed -
    // the first build could not find GetEquipment on the mech, and the second crashed before
    // reaching the data asset - so it is the least proven thing here, and it is also the only one
    // whose absence costs nothing but longer weapon names. Turn it on once the build is stable.
    // ON. The route is now built from the game's own HUD_TargetWeaponLoadout / HUD_WeaponPanel
    // chain rather than from my guesses at it, and it degrades to the registry with a logged
    // reason at every step rather than failing silently.
    static constexpr bool      kEquipmentWeapons = true;
    static constexpr bool      kTargetComponent  = true;  // kind, TargetName, art key
    static constexpr bool      kPersonaCallsign  = true;  // proven working in the first build
    // See the note at the TrackDropships call site: the re-trace punched more holes than the
    // mound it removed was worth.
    static constexpr bool      kRetraceDropshipPads = false;
    static constexpr ULONGLONG kMechInfoMs       = 250;
    static constexpr float     kMechInfoRangeCm  = 120000.0f;
    bool MechInfoLogged = false;

    // =======================================================================================
    // MECH CONDITION - the rest of what a contact IS
    //
    // WHERE THE ROUTE CAME FROM. MWVR's VR_HUD_TargetObject widget ships uncooked in the modder
    // source files. It casts the target actor to MWMech and calls GetMechData(), and the struct
    // that comes back carries the lot: VariantName, ChassisFullName, MechType, MechDataStats,
    // HealthDataStats, EquipmentAllocation, SelectedWeaponHardPoints, EngineRating. Every one of
    // those names is read straight off the asset's pin list, so none of them is a guess. This is
    // the same lesson as the variant read: when the game already displays what you want, read
    // whatever displays it before writing a probe.
    //
    // WHY THE INNER STRUCTS ARE HANDLED DIFFERENTLY. MechStats, MechHealthStats and the hardpoint
    // entries are native structs in /Script/MechWarrior. No asset breaks them open, so their
    // MEMBER names are not recoverable from game files at any effort - they exist only in the
    // shipped binary's reflection data. They are UScriptStructs though, which means the engine
    // will describe them to us at runtime. So instead of guessing member names and burning a
    // mission per guess, this code ENUMERATES each struct's members and matches by substring: a
    // float whose name contains "Ton" is the tonnage, one containing "Heat" is heat capacity. It
    // then logs the complete member list ONCE, so the next build can replace a substring with an
    // exact name. One mission produces the whole answer instead of a yes/no.
    //
    // CACHING. Every offset resolved here is cached after the first success. The per-mech read
    // count is in the hundreds and a reflected lookup by name is a string compare against a
    // property list, so doing this per mech per tick would cost about a millisecond on the game
    // thread. Identity (chassis, variant, tonnage, weapon mounts) is cached PER ACTOR for the
    // life of the mission because none of it can change while a mech is alive; only the live
    // condition - armour, structure, heat - is re-read on the scan cadence.
    // =======================================================================================

    // ---- generic reflection: walk a struct's own properties AND everything it inherits ------
    // get_child_properties() returns only the properties a struct declares itself, so a walk
    // that forgets the super chain silently omits the entire native layer under a blueprint.
    // That exact mistake cost a mission earlier in this project; it is not repeated here.
    template <class F>
    static void ForEachProp(API::UStruct* st, F&& fn) {
        for (API::UStruct* s = st; s; s = s->get_super_struct())
            for (API::FField* f = s->get_child_properties(); f; f = f->get_next())
                fn(reinterpret_cast<API::FProperty*>(f));
    }

    static std::wstring PropTypeOf(API::FProperty* p) {
        auto* fc = p ? p->get_class() : nullptr;
        return fc ? fc->get_name() : std::wstring();
    }

    static bool NameHas(const std::wstring& hay, const wchar_t* needle) {
        return hay.find(needle) != std::wstring::npos;
    }

    // Exact-name lookup, super chain included. Returns offset or -1.
    static int32_t PropOffset(API::UStruct* st, const wchar_t* name, API::FProperty** out = nullptr) {
        int32_t off = -1;
        ForEachProp(st, [&](API::FProperty* p) {
            if (off >= 0 || !p->get_fname()) return;
            if (p->get_fname()->to_string() == name) {
                off = p->get_offset();
                if (out) *out = p;
            }
        });
        return off;
    }

    // First name in the list that exists, EXACTLY. Preferred over the substring matcher below
    // wherever the real names are known, because substring matching quietly finds the wrong
    // neighbour: matching "Armor" on the health container picked up CurrentArmor and
    // CurrentRearArmor - two adjacent floats - and divided front armour by rear armour. It
    // produced a plausible number, which is the worst kind of wrong.
    static int32_t PropOffsetFirst(API::UStruct* st, const wchar_t* const* names, int n,
                                   const wchar_t** picked = nullptr) {
        for (int i = 0; i < n; ++i) {
            const int32_t off = PropOffset(st, names[i]);
            if (off >= 0) { if (picked) *picked = names[i]; return off; }
        }
        return -1;
    }

    // Substring lookup restricted to a property type, so "Heat" cannot match a delegate called
    // OnHeatChanged. `avoid` rejects a near-miss - matching "Armor" must not pick up "ArmorMax"
    // when the current value is wanted.
    // Find a float by name ANYWHERE inside a struct, descending through nested structs and
    // returning the cumulative offset. MechDataStats does not hold tonnage directly - it holds
    // BaseStats, MoveStats, TorsoStats, PowerStats and MechImportData, and the number is one
    // level further down. A flat search over the top level finds nothing and reports it as
    // "no tonnage on this mech", which is a wrong answer rather than a missing one.
    static int32_t FindFloatDeep(API::UStruct* st, const wchar_t* const* names, int n,
                                 int depth, bool* isInt = nullptr) {
        if (!st || depth < 0) return -1;
        int32_t found = -1;
        // Exact names at this level first, so a nested near-match cannot beat a direct hit.
        ForEachProp(st, [&](API::FProperty* p) {
            if (found >= 0 || !p->get_fname()) return;
            const std::wstring pn = p->get_fname()->to_string();
            const std::wstring ty = PropTypeOf(p);
            if (ty != L"FloatProperty" && ty != L"IntProperty") return;
            for (int i = 0; i < n; ++i)
                if (pn == names[i]) {
                    found = p->get_offset();
                    if (isInt) *isInt = (ty == L"IntProperty");
                    return;
                }
        });
        if (found >= 0) return found;

        ForEachProp(st, [&](API::FProperty* p) {
            if (found >= 0) return;
            API::UStruct* inner = StructOfProp(p);
            if (!inner) return;
            const int32_t sub = FindFloatDeep(inner, names, n, depth - 1, isInt);
            if (sub >= 0) found = p->get_offset() + sub;
        });
        return found;
    }

    static int32_t PropOffsetLike(API::UStruct* st, const wchar_t* want, const wchar_t* type,
                                  const wchar_t* avoid = nullptr) {
        int32_t off = -1;
        ForEachProp(st, [&](API::FProperty* p) {
            if (off >= 0 || !p->get_fname()) return;
            const std::wstring n = p->get_fname()->to_string();
            if (!NameHas(n, want)) return;
            if (avoid && NameHas(n, avoid)) return;
            if (type && PropTypeOf(p) != type) return;
            off = p->get_offset();
        });
        return off;
    }

    // One line per member: name, type, offset. This is the whole point of the self-describing
    // approach - it turns "what is in MechStats?" from a mission into a log line.
    // ---- a breadcrumb that survives the crash -------------------------------------------------
    //
    // UEVR's log goes through a BUFFERED logger. When the game dies on an access violation the
    // last lines never reach disk, so the tail of log.txt is not where the fault was - it is
    // wherever the buffer last flushed. I read that tail as if it were the crash site and drew
    // the wrong conclusion from it, which cost a build.
    //
    // This writes one short phase string to its own file and CLOSES it every time, so the content
    // is on disk before the next instruction runs. After a crash that file names the exact step.
    // It costs an open/write/close a few times per scan, at 4 Hz, against a crash that otherwise
    // takes a full mission to localise.
    // OFF. It did its job (it named the PersonaComponent scan as the crash), and at ~30 file
    // create/truncate/close operations per scan on the game thread it was a steady hitch -
    // worse with an antivirus scanning each close. Flip back on only to chase a crash.
    static constexpr bool kBreadcrumbs = false;
    char BreadcrumbPath[MAX_PATH] = "";

    void Breadcrumb(const char* what) {
        if (!kBreadcrumbs) return;
        if (!BreadcrumbPath[0]) {
            const char* appdata = std::getenv("APPDATA");
            if (!appdata) return;
            snprintf(BreadcrumbPath, sizeof(BreadcrumbPath),
                     "%s\\UnrealVRMod\\MechWarrior-Win64-Shipping\\lidar_phase.txt", appdata);
        }
        if (FILE* f = fopen(BreadcrumbPath, "w")) {
            fputs(what, f);
            fclose(f);   // the close is the point - an unflushed breadcrumb is no breadcrumb
        }
    }

    // Log a line the FIRST time its exact text occurs, and never again.
    //
    // The diagnostics that have actually earned their keep in this file are the ones that fire
    // once per distinct VALUE - one line per paperdoll class, per weapon name, per vehicle - not
    // once per session and not once per frame. A once-per-session flag hides the second and third
    // thing, which is exactly how the turret paperdoll class would have stayed invisible.
    std::unordered_set<std::string> LoggedOnce;
    void LogDistinctOnce(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (LoggedOnce.size() < 256 && LoggedOnce.insert(buf).second) LogInfo("%s", buf);
    }

    void LogStructMembers(const char* label, API::UStruct* st) {
        if (!st) { LogInfo("layout %s: <null>", label); return; }
        int n = 0;
        ForEachProp(st, [&](API::FProperty* p) {
            if (!p->get_fname()) return;
            ++n;
            const std::wstring ty = PropTypeOf(p);

            // An "ArrayProperty" on its own is useless - it says a list is here and nothing
            // about what is in it. SelectedWeaponHardPoints dumped as an array and the element
            // struct came back null, which left stride 0 and no weapons, with no way to tell
            // from the log whether the elements were structs, objects or names. So arrays print
            // their inner type, and struct elements print the stride the iteration needs.
            char extra[128] = "";
            if (ty == L"ArrayProperty") {
                auto* inner = reinterpret_cast<API::FArrayProperty*>(p)->get_inner();
                const std::wstring it = inner ? PropTypeOf(inner) : std::wstring(L"?");
                API::UStruct* es = StructOfProp(inner);
                if (es) {
                    const int32_t sz = es->get_properties_size();
                    const int32_t al = es->get_min_alignment();
                    const int32_t stride = (al > 0) ? ((sz + al - 1) / al) * al : sz;
                    std::snprintf(extra, sizeof(extra), "  <%s %s, stride %d>",
                                  WideToNarrow(it.c_str()).c_str(),
                                  WideToNarrow(es->get_fname()->to_string().c_str()).c_str(),
                                  stride);
                } else {
                    std::snprintf(extra, sizeof(extra), "  <%s>", WideToNarrow(it.c_str()).c_str());
                }
            } else if (ty == L"StructProperty") {
                if (API::UStruct* es = StructOfProp(p))
                    std::snprintf(extra, sizeof(extra), "  <%s>",
                                  WideToNarrow(es->get_fname()->to_string().c_str()).c_str());
            }

            LogInfo("layout %s: %-40s %-18s +%d%s", label,
                    WideToNarrow(p->get_fname()->to_string().c_str()).c_str(),
                    WideToNarrow(ty.c_str()).c_str(), p->get_offset(), extra);
        });
        LogInfo("layout %s: %d member(s)", label, n);
    }

    // ---- reading the awkward value types ----------------------------------------------------
    // An FString is a TArray<wchar_t>; an FName is a table index whose address can be handed to
    // API::FName directly (to_handle() is just the pointer).
    static void ReadFString(const void* at, char* out, size_t cap) {
        out[0] = '\0';
        if (!at) return;
        FScriptArrayLayout a{};
        std::memcpy(&a, at, sizeof(a));
        if (!a.Data || a.Num <= 0) return;
        const wchar_t* w = reinterpret_cast<const wchar_t*>(a.Data);
        size_t k = 0;
        for (; k + 1 < cap && (int32_t)k < a.Num && w[k]; ++k) out[k] = (w[k] < 128) ? (char)w[k] : '?';
        out[k] = '\0';
    }

    static void ReadFNameAt(const void* at, char* out, size_t cap) {
        out[0] = '\0';
        if (!at) return;
        auto* n = reinterpret_cast<API::FName*>(const_cast<void*>(at));
        const std::wstring s = n->to_string();
        if (s.empty() || s == L"None") return;
        size_t k = 0;
        for (; k + 1 < cap && k < s.size(); ++k) out[k] = (s[k] < 128) ? (char)s[k] : '?';
        out[k] = '\0';
    }

    // ---- FText -> FString, through the engine's own converter -------------------------------
    //
    // ChassisFullName and VariantName are TextProperty. An FText is a ref-counted handle to a
    // text object, not a string, and reading it as one produced '???' - three bytes of unrelated
    // memory. The response then was to abandon them and parse the DATA ASSET's name instead.
    //
    // That works on stock mechs and is wrong on modded ones: a mod prefixes its assets, so
    // zatCHP-ZAT_MDA parses to "zatCHP-ZAT" where the game itself displays "CHP-ZAT". The asset
    // name was never the variant - it just resembled it closely enough to pass.
    //
    // Rather than decode FText's internals (a layout that shifts between engine builds), call
    // KismetTextLibrary::Conv_TextToString - the same conversion the game's own UI uses. No
    // guessing, and it cannot drift.
    API::UFunction* TextToStrFn = nullptr;
    API::UObject*   TextLibCdo  = nullptr;
    int32_t TtsIn = -1, TtsRet = -1;
    bool    TtsChecked = false;
    std::vector<uint8_t> TtsBuf;

    bool EnsureTextToString() {
        if (TtsChecked) return TextToStrFn != nullptr;
        TtsChecked = true;
        auto* cls = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.KismetTextLibrary");
        if (!cls) { LogError("identity: KismetTextLibrary not found"); return false; }
        TextLibCdo  = cls->get_class_default_object();
        TextToStrFn = cls->find_function(L"Conv_TextToString");
        if (!TextLibCdo || !TextToStrFn) { LogError("identity: Conv_TextToString not found"); return false; }
        for (auto* f = TextToStrFn->get_child_properties(); f; f = f->get_next()) {
            auto* pr = reinterpret_cast<API::FProperty*>(f);
            if (!pr->get_fname()) continue;
            const std::wstring n = pr->get_fname()->to_string();
            if (n == L"InText")      TtsIn  = pr->get_offset();
            if (n == L"ReturnValue") TtsRet = pr->get_offset();
        }
        const int32_t frame = TextToStrFn->get_properties_size();
        if (frame <= 0 || TtsIn < 0 || TtsRet < 0) {
            LogError("identity: Conv_TextToString layout incomplete (frame %d, in %d, ret %d)",
                     frame, TtsIn, TtsRet);
            TextToStrFn = nullptr;
            return false;
        }
        TtsBuf.assign(static_cast<size_t>(frame) + 64, 0);
        LogInfo("identity: Conv_TextToString ready (in +%d, ret +%d)", TtsIn, TtsRet);
        return true;
    }

    // `at` points at the FText inside a struct. FText is 24 bytes here (ChassisFullName +792 and
    // VariantName +816 sit exactly that far apart), copied wholesale into the call frame.
    bool TextToString(const void* at, char* out, size_t cap) {
        out[0] = '\0';
        if (!at || !EnsureTextToString()) return false;
        // An FText is a shared reference; its first word is the pointer to the text data. If
        // that is null the FText was never initialised, and handing it to Conv_TextToString
        // would have the engine dereference it for us. Cheap guard, and it costs nothing.
        const void* inner = nullptr;
        std::memcpy(&inner, at, sizeof(inner));
        if (!inner) return false;
        // Not zeroed: the previous FString in the return slot is freed by the engine's
        // assignment of the new one. (It used to be zeroed on the theory this ran once per actor;
        // weapon and target names now call it every scan, and each call leaked its string.)
        // The input FText is a borrowed copy, overwritten in full every call.
        std::memcpy(TtsBuf.data() + TtsIn, at, 24);
        TextLibCdo->process_event(TextToStrFn, TtsBuf.data());
        ReadFString(TtsBuf.data() + TtsRet, out, cap);
        return out[0] != '\0';
    }

    // ---- GetMechData: one reflected call, everything static about the mech ------------------
    API::UFunction* GetMechDataFn = nullptr;
    int32_t         MechDataRet   = -1;
    API::UStruct*   MechDataStruct = nullptr;
    std::vector<uint8_t> MechDataBuf;
    bool MechDataChecked = false, MechDataReady = false;

    // Offsets INSIDE the MechData struct. Names from the VR_HUD_TargetObject pin list.
    int32_t MdVariantName = -1, MdChassisFull = -1, MdMechType = -1;
    int32_t MdStats = -1, MdHealthStats = -1, MdHardPoints = -1;
    API::UStruct* MdStatsStruct = nullptr;
    API::UStruct* MdHealthStruct = nullptr;
    API::UStruct* MdHpElemStruct = nullptr;
    int32_t MdHpStride = 0;

    // Members matched inside those inner structs (substring, then logged for tightening).
    int32_t StTons = -1, StHeatCap = -1;
    bool    StTonsIsInt = false;
    int32_t HpPart = -1, HpWeapon = -1;

    bool LayoutLogged = false;

    static API::UStruct* StructOfProp(API::FProperty* p) {
        if (!p || PropTypeOf(p) != L"StructProperty") return nullptr;
        return reinterpret_cast<API::FStructProperty*>(p)->get_struct();
    }

    // The mech's own MWMechDataAsset. The variant read already goes through this call, so the
    // route is proven; this just keeps the asset rather than only its name.
    API::UObject* MdaOf(API::UObject* mech) {
        if (!EnsureMdaFn(mech)) return nullptr;
        std::memset(MdaBuf.data(), 0, MdaBuf.size());
        mech->process_event(GetMdaFn, MdaBuf.data());
        return *reinterpret_cast<API::UObject**>(MdaBuf.data() + MdaRet);
    }

    // WHERE GetMechData ACTUALLY LIVES. The first attempt looked for it on the MECH and it was
    // not there - the log said so plainly. Re-reading the widget graph explains why: it casts to
    // MWMech, calls GetMechDataAsset() to get the MWMechDataAsset, and only THEN reaches
    // MechData. GetMechData is a member of the ASSET, not of the mech.
    //
    // Better still, MechData is a plain struct member on that asset, so it can be read in place
    // with no call at all. That is the preferred path here and the function is only a fallback:
    // a memory read cannot fail halfway or run script, which a process_event on the game thread
    // can. Both are tried, because a pin list proves a member exists, not how to reach it.
    int32_t MdaDataOff = -1;

    bool EnsureMechDataFn(API::UObject* mech) {
        if (MechDataChecked) return MechDataReady;
        MechDataChecked = true;

        API::UObject* mda = MdaOf(mech);
        if (!mda || !mda->get_class()) {
            LogError("condition: no MechDataAsset on this mech");
            return false;
        }
        API::UClass* mdaCls = mda->get_class();

        // Preferred: the struct member, read directly.
        API::FProperty* dataProp = nullptr;
        MdaDataOff = PropOffset(mdaCls, L"MechData", &dataProp);
        MechDataStruct = StructOfProp(dataProp);

        // Fallback: the accessor, if the member is not exposed.
        if (!MechDataStruct) {
            for (API::UStruct* st = mdaCls; st && !GetMechDataFn; st = st->get_super_struct())
                GetMechDataFn = static_cast<API::UClass*>(st)->find_function(L"GetMechData");
            if (!GetMechDataFn) {
                LogError("condition: neither MechData member nor GetMechData on %s",
                         WideToNarrow(mdaCls->get_fname()->to_string().c_str()).c_str());
                return false;
            }
            const int32_t frame = GetMechDataFn->get_properties_size();
            API::FProperty* retProp = nullptr;
            for (auto* f = GetMechDataFn->get_child_properties(); f; f = f->get_next()) {
                auto* p = reinterpret_cast<API::FProperty*>(f);
                if (p->get_fname() && p->get_fname()->to_string() == L"ReturnValue") {
                    MechDataRet = p->get_offset();
                    retProp = p;
                }
            }
            MechDataStruct = StructOfProp(retProp);
            if (frame <= 0 || MechDataRet < 0 || !MechDataStruct) {
                LogError("condition: GetMechData layout incomplete (frame %d, ret %d, struct %p)",
                         frame, MechDataRet, (void*)MechDataStruct);
                return false;
            }
            MechDataBuf.assign(static_cast<size_t>(frame) + 128, 0);
        }

        // Exact names - straight off the uncooked pin list, so a miss here means the game
        // changed, not that we guessed wrong.
        API::FProperty* pStats = nullptr; API::FProperty* pHealth = nullptr; API::FProperty* pHp = nullptr;
        MdVariantName = PropOffset(MechDataStruct, L"VariantName");
        MdChassisFull = PropOffset(MechDataStruct, L"ChassisFullName");
        MdMechType    = PropOffset(MechDataStruct, L"MechType");
        MdStats       = PropOffset(MechDataStruct, L"MechDataStats",           &pStats);
        MdHealthStats = PropOffset(MechDataStruct, L"HealthDataStats",         &pHealth);
        MdHardPoints  = PropOffset(MechDataStruct, L"SelectedWeaponHardPoints", &pHp);

        MdStatsStruct  = StructOfProp(pStats);
        MdHealthStruct = StructOfProp(pHealth);

        // The hardpoint list is a TArray; its element stride comes from the inner struct.
        if (pHp && PropTypeOf(pHp) == L"ArrayProperty") {
            auto* inner = reinterpret_cast<API::FArrayProperty*>(pHp)->get_inner();
            MdHpElemStruct = StructOfProp(inner);
            if (MdHpElemStruct) {
                const int32_t sz = MdHpElemStruct->get_properties_size();
                const int32_t al = MdHpElemStruct->get_min_alignment();
                MdHpStride = (al > 0) ? ((sz + al - 1) / al) * al : sz;
            }
        }

        // Substring matches inside the native inner structs.
        if (MdStatsStruct) {
            // Tonnage / Tons are real property names out of the HarJel pak. The TYPE is not
            // known, so the offset is taken by name and the width decided by the property's own
            // class - a float read off an int32 field is not a small error, it is a garbage
            // number with a plausible magnitude.
            static const wchar_t* kTons[] = { L"Tonnage", L"Tons", L"Mass" };
            StTons = FindFloatDeep(MdStatsStruct, kTons, 3, 3, &StTonsIsInt);
            StHeatCap = PropOffsetLike(MdStatsStruct, L"Heat", L"FloatProperty");
        }
        if (MdHpElemStruct) {
            // The mount is an enum or a name; the weapon is a name, a string or an object.
            HpPart = PropOffsetLike(MdHpElemStruct, L"Component", L"EnumProperty");
            if (HpPart < 0) HpPart = PropOffsetLike(MdHpElemStruct, L"Location", L"EnumProperty");
            if (HpPart < 0) HpPart = PropOffsetLike(MdHpElemStruct, L"Part",     L"EnumProperty");
            HpWeapon = PropOffsetLike(MdHpElemStruct, L"Weapon", L"NameProperty");
            if (HpWeapon < 0) HpWeapon = PropOffsetLike(MdHpElemStruct, L"Weapon", L"StrProperty");
        }

        MechDataReady = true;
        return true;
    }

    // One dump, once per mission, of every layout this reader depends on. Cheap (it runs once)
    // and it is the difference between "heat did not resolve" and knowing what heat is called.
    // nolayout1 (user 2026-10-13): the component layout dump (~880 lines in one frame on the first mission of each
    // session = a 36-47 ms hitch) is off; the layouts it found are in use. Set true to print it again.
    static constexpr bool kLogComponentLayout = false;
    void LogConditionLayoutOnce(API::UObject* mech) {
        if (!kLogComponentLayout) return;
        if (LayoutLogged || !mech) return;
        LayoutLogged = true;

        // ORDER MATTERS, AND IT IS NOT COSMETIC. The first build put EnsureMechDataFn ahead of
        // everything here and returned on failure, so when GetMechData was not found the log got
        // one error line and NOTHING else - the dump that existed purely to explain the failure
        // was gated behind it. A diagnostic may never depend on the thing it is diagnosing. The
        // component dump therefore runs first and unconditionally.
        LogInfo("layout: components on %s",
                WideToNarrow(mech->get_class()->get_fname()->to_string().c_str()).c_str());
        // THE DIAGNOSTIC WAS THE WORST OFFENDER. This used to walk every object property on the
        // mech and dereference each one just to print what it was - the single largest number of
        // blind dereferences in the plugin, running on the player's mech at mission start. The
        // engine's component list gives the same census and prints only things that exist.
        {
            int cn = 0;
            ForEachLiveComponent(mech, [&](API::UObject* c) {
                ++cn;
                if (!c->get_class() || !c->get_class()->get_fname()) return true;
                const std::wstring cls = c->get_class()->get_fname()->to_string();
                LogInfo("layout: component %s", WideToNarrow(cls.c_str()).c_str());
                if (NameHas(cls, L"LivingActor") || NameHas(cls, L"Heat") ||
                    NameHas(cls, L"CriticalSlot") || NameHas(cls, L"WeaponSystem") ||
                    NameHas(cls, L"Persona") || NameHas(cls, L"Target"))
                    LogStructMembers(WideToNarrow(cls.c_str()).c_str(), c->get_class());
                return true;
            });
            LogInfo("layout: %d live component(s)", cn);
        }

        // The PER-PART CONTAINER. This is the one layout nothing in the game's files can give us:
        // MWVR's paperdoll reads already-computed percentages out of the base game's
        // CalculateHealthArmorPctForPart and never touches the raw values, so the container's own
        // member names appear in no asset. Armour resolved on the first build and structure's
        // maximum did not, and this dump is what names it.
        if (LivingComp) {
            for (int i = 0; i < TMP_COUNT; ++i) {
                if (PartSlotOff[i] < 0) continue;
                auto** slot = reinterpret_cast<API::UObject**>(
                    reinterpret_cast<uint8_t*>(LivingComp) + PartSlotOff[i]);
                API::UObject* cont = slot ? *slot : nullptr;
                if (!cont || !cont->get_class()) continue;
                LogStructMembers("HealthContainer", cont->get_class());
                break; // one container describes all eight
            }
        }

        // AN INDEPENDENT SECOND SOURCE FOR WEAPON <-> COMPONENT.
        //
        // The pairing currently comes from MechWeaponSystemEntry.MechPart. That is one source,
        // and the byte values it uses are the one thing in this chain that no game FILE states -
        // cooked name tables are alphabetised, so the enum's order is not recoverable from them.
        // The order used here was read off MWMechLivingActorComponent's property declarations
        // instead, which is solid but still a single source.
        //
        // The game's own target weapon panel (HUD_TargetWeaponLoadout) does not use the registry
        // at all - it casts to MWMech and goes through MWMechCriticalSlotComponent, whose
        // MechEquipment is a CriticalSlotEquipment struct. Dumping that gives a SECOND, entirely
        // independent statement of which weapon sits in which component, so the two can be
        // compared rather than trusted. One mission's log settles it.
        if (API::UObject* comp = CompByClassName(mech, L"CriticalSlot")) {
            API::FProperty* eq = nullptr;
            if (PropOffset(comp->get_class(), L"MechEquipment", &eq) >= 0 && eq) {
                if (API::UStruct* es = StructOfProp(eq)) {
                    LogStructMembers("CriticalSlotEquipment", es);
                    // One level further: the per-part lists live inside it.
                    ForEachProp(es, [&](API::FProperty* q) {
                        if (!q->get_fname() || PropTypeOf(q) != L"ArrayProperty") return;
                        auto* inner = reinterpret_cast<API::FArrayProperty*>(q)->get_inner();
                        if (API::UStruct* is2 = StructOfProp(inner))
                            LogStructMembers(WideToNarrow(q->get_fname()->to_string().c_str()).c_str(), is2);
                    });
                }
            }
        }

        // WeaponRegistry is the LIVE weapon list on MWMechWeaponSystemComponent, and it is a
        // better source than the data asset's hardpoints: it is what the mech is actually
        // carrying now, including battlefield losses. Its element layout is dumped here so the
        // next build can iterate it instead of the asset.
        if (API::UObject* comp = CompByClassName(mech, L"WeaponSystem")) {
            API::FProperty* reg = nullptr;
            if (PropOffset(comp->get_class(), L"WeaponRegistry", &reg) >= 0 && reg &&
                PropTypeOf(reg) == L"ArrayProperty") {
                auto* inner = reinterpret_cast<API::FArrayProperty*>(reg)->get_inner();
                LogInfo("layout: WeaponRegistry inner is %s",
                        inner ? WideToNarrow(PropTypeOf(inner).c_str()).c_str() : "?");
                if (API::UStruct* es = StructOfProp(inner))
                    LogStructMembers("WeaponRegistryEntry", es);
            }
        }

        // MechData last, because it is the part that can fail.
        if (!EnsureMechDataFn(mech)) return;

        LogInfo("layout: MechData member +%d / ret +%d, variant +%d chassis +%d type +%d stats +%d health +%d hp +%d (stride %d)",
                MdaDataOff, MechDataRet, MdVariantName, MdChassisFull, MdMechType, MdStats,
                MdHealthStats, MdHardPoints, MdHpStride);
        LogStructMembers("MechData",        MechDataStruct);
        LogStructMembers("MechStats",       MdStatsStruct);
        LogStructMembers("MechHealthStats", MdHealthStruct);
        LogStructMembers("HardPoint",       MdHpElemStruct);

    }

    // ---- live condition: armour and structure per part, and heat ---------------------------
    // The eight parts in contract order. Several spellings per part because the engine is not
    // consistent about Center/Centre and about whether a torso is "Torso" or "SideTorso".
    static const wchar_t* PartNames(int part, int alt) {
        static const wchar_t* t[TMP_COUNT][3] = {
            { L"Head",        L"HeadArmor",        nullptr },
            { L"CenterTorso", L"CentreTorso",      L"Torso" },
            { L"LeftTorso",   L"LeftSideTorso",    nullptr },
            { L"RightTorso",  L"RightSideTorso",   nullptr },
            { L"LeftArm",     nullptr,             nullptr },
            { L"RightArm",    nullptr,             nullptr },
            { L"LeftLeg",     nullptr,             nullptr },
            { L"RightLeg",    nullptr,             nullptr },
        };
        return (part >= 0 && part < TMP_COUNT && alt >= 0 && alt < 3) ? t[part][alt] : nullptr;
    }

    // ---- weapons: the LIVE registry, not the variant's paper loadout --------------------
    // MWMechWeaponSystemComponent.WeaponRegistry is an array of MechWeaponSystemEntry:
    //     Weapon           WeakObjectProperty +0
    //     AimTarget        EnumProperty       +8
    //     MechPart         EnumProperty       +9    <- the mount, which is the whole point
    //     WeaponGroupFlags EnumProperty       +10
    //     bIsAMS           BoolProperty       +11
    //     bIsMelee         BoolProperty       +12
    //     SortName         StrProperty        +16
    // Offsets are resolved by NAME anyway - the numbers above are what this build observed, not
    // constants to rely on. This beats MechData.SelectedWeaponHardPoints (which never resolved
    // an element struct at all) because it is what the mech is carrying NOW.
    API::UObject* WeaponComp = nullptr;
    int32_t WsRegOff = -1, WsStride = 0;
    int32_t WeSortName = -1, WeMechPart = -1, WeIsAMS = -1, WeIsMelee = -1, WeWeapon = -1;
    // Hud list entries kept per mech for pairing (the registry can hold more than
    // TERR_MAX_WEAPONS entries once AMS and melee are counted).
    static constexpr int kHudMax = 32;
    // 2026-10-19 GROUPED WEAPONS (TERRAIN v15): the routes read up to kRawWeapons, then GroupWeapons folds identical
    // weapons - same short name, same part, same flags - into one entry with a Count, so a 21-weapon Adder S
    // (16 MG, 4 ER ML, 1 flamer over 5 parts) fits the 12 published slots. Parts are never merged here: the
    // displays merge across parts only while the state matches, so a destroyed part's weapons still split off.
    static constexpr int kRawWeapons = 48;
    static uint8_t GroupWeapons(const TerrainWeapon* raw, int n, TerrainWeapon* out) {
        uint8_t m = 0;
        for (int i = 0; i < n; ++i) {
            int j = 0;
            for (; j < m; ++j)
                if (out[j].Part == raw[i].Part && out[j].Flags == raw[i].Flags &&
                    std::strncmp(out[j].Name, raw[i].Name, sizeof(raw[i].Name)) == 0)
                    break;
            if (j < m) {
                if (out[j].Count < 255) ++out[j].Count;
            } else if (m < (uint8_t)TERR_MAX_WEAPONS) {
                out[m] = raw[i];
                out[m].Count = 1;
                ++m;
            }
        }
        return m;
    }
    // Full registry name -> the game's short name, learned from any mech this mission.
    std::unordered_map<std::string, std::string> ShortByLong;

    // A TWeakObjectPtr is {int32 ObjectIndex, int32 Serial} into the global object array - NOT a
    // pointer. Reading it as one yields (Serial<<32 | Index) and faults, which this plugin has
    // already done once.
    API::UObject* ResolveWeak(const void* at) {
        if (!at) return nullptr;
        int32_t idx = 0;
        std::memcpy(&idx, at, sizeof(idx));
        if (idx < 0) return nullptr;
        const auto sdk = API::get()->sdk();
        const auto arr = sdk->functions->get_uobject_array();
        if (!arr) return nullptr;
        if (idx >= sdk->uobject_array->get_object_count(arr)) return nullptr;
        return reinterpret_cast<API::UObject*>(sdk->uobject_array->get_object(arr, idx));
    }

    bool    WsChecked = false;

    API::UObject* WeaponCompOf(API::UObject* mech) {
        API::UObject* found = CompByClassName(mech, L"WeaponSystem");
        if (found) return found;
        // Tanks, missile carriers and VTOLs carry no weapon system of their own (the log listed
        // every component: none). Each has a ChildActorComponent - its turret - so look there,
        // and one level further down.
        for (API::UObject* child : ChildActorsOf(mech)) {
            if ((found = CompByClassName(child, L"WeaponSystem"))) return found;
            for (API::UObject* grand : ChildActorsOf(child))
                if ((found = CompByClassName(grand, L"WeaponSystem"))) return found;
        }
        return nullptr;
    }

    // The actors spawned by an actor's ChildActorComponents (live component list; the
    // component's ChildActor is a strong reference, so each is alive while its component is).
    std::vector<API::UObject*> ChildActorsOf(API::UObject* actor) {
        std::vector<API::UObject*> out;
        if (!actor) return out;
        ForEachLiveComponent(actor, [&](API::UObject* c) {
            if (c->get_class() && c->get_class()->get_fname() &&
                c->get_class()->get_fname()->to_string() == L"ChildActorComponent")
                if (auto* pp = c->get_property_data<API::UObject*>(L"ChildActor"))
                    if (*pp) out.push_back(*pp);
            return true;
        });
        return out;
    }

    void ResolveWeaponLayout(API::UObject* mech) {
        if (WsChecked) return;
        WsChecked = true;
        WeaponComp = WeaponCompOf(mech);
        if (!WeaponComp || !WeaponComp->get_class()) { LogError("weapons: no weapon system"); return; }

        API::FProperty* reg = nullptr;
        WsRegOff = PropOffset(WeaponComp->get_class(), L"WeaponRegistry", &reg);
        if (WsRegOff < 0 || !reg || PropTypeOf(reg) != L"ArrayProperty") {
            LogError("weapons: WeaponRegistry not an array (off %d)", WsRegOff);
            WsRegOff = -1;
            return;
        }
        auto* inner = reinterpret_cast<API::FArrayProperty*>(reg)->get_inner();
        API::UStruct* es = StructOfProp(inner);
        if (!es) { LogError("weapons: registry element is not a struct"); WsRegOff = -1; return; }

        const int32_t sz = es->get_properties_size();
        const int32_t al = es->get_min_alignment();
        WsStride   = (al > 0) ? ((sz + al - 1) / al) * al : sz;
        WeSortName = PropOffset(es, L"SortName");
        WeMechPart = PropOffset(es, L"MechPart");
        WeIsAMS    = PropOffset(es, L"bIsAMS");
        WeIsMelee  = PropOffset(es, L"bIsMelee");
        WeWeapon   = PropOffset(es, L"Weapon");

        LogInfo("weapons: registry +%d stride %d name +%d part +%d ams +%d melee +%d",
                WsRegOff, WsStride, WeSortName, WeMechPart, WeIsAMS, WeIsMelee);
    }

    // EMechParts -> contract order. The byte values are NOT documented anywhere I can read, so
    // this mapping is the one link in the chain that is inferred rather than proven. It is
    // logged per weapon on the first scan precisely so it can be checked against the mech on
    // screen: if a Catapult's LRMs show up in the wrong torso, this table is why.
    // EMechParts -> contract order. NOT inferred any more: MWMechLivingActorComponent declares
    // its per-part containers in the enum's own order, and the layout dump printed them at
    // consecutive offsets -
    //     Head, CenterTorso, LeftTorso, LeftArm, LeftLeg, RightTorso, RightArm, RightLeg,
    //     RearCenterTorso, RearLeftTorso, RearRightTorso
    // - so the whole left side is grouped before the whole right side, NOT paired left/right as
    // the first version of this table assumed. That guess put a Catapult's LRM pods in its left
    // LEG. The giveaway was in the log: the two missile mounts came back as bytes 3 and 6, which
    // are six apart in a paired layout and a clean left/right mirror in this one.
    static uint8_t PartFromEnum(uint8_t v) {
        static const uint8_t map[11] = {
            TMP_Head, TMP_CentreTorso,
            TMP_LeftTorso,  TMP_LeftArm,  TMP_LeftLeg,
            TMP_RightTorso, TMP_RightArm, TMP_RightLeg,
            // Rear faces fold onto their front part: the doll has no back view, and a weapon
            // mounted rear-centre is still lost when the centre torso goes.
            TMP_CentreTorso, TMP_LeftTorso, TMP_RightTorso
        };
        return (v < 11) ? map[v] : TMP_UNKNOWN;
    }

    // Reads the registry straight off this mech's own component. Live, every scan: a weapon can
    // be blown off mid-fight, and a cached list would keep claiming it is there.
    // Weapon registry layout per weapon-system CLASS (mech, tank, VTOL and turret systems differ).
    struct RegLayout {
        int32_t RegOff = -1, Stride = 0, SortName = -1, MechPart = -1, IsAMS = -1, IsMelee = -1, Weapon = -1;
    };
    std::unordered_map<const void*, RegLayout> RegLayouts;

    const RegLayout* RegLayoutFor(API::UObject* wc) {
        API::UClass* cls = wc ? wc->get_class() : nullptr;
        if (!cls) return nullptr;
        auto it = RegLayouts.find(cls);
        if (it == RegLayouts.end()) {
            RegLayout L{};
            API::FProperty* reg = nullptr;
            L.RegOff = PropOffset(cls, L"WeaponRegistry", &reg);
            API::UStruct* es = nullptr;
            if (L.RegOff >= 0 && reg && PropTypeOf(reg) == L"ArrayProperty")
                es = StructOfProp(reinterpret_cast<API::FArrayProperty*>(reg)->get_inner());
            if (es) {
                const int32_t sz = es->get_properties_size(), al = es->get_min_alignment();
                L.Stride   = (al > 0) ? ((sz + al - 1) / al) * al : sz;
                L.SortName = PropOffset(es, L"SortName");
                L.MechPart = PropOffset(es, L"MechPart");
                L.IsAMS    = PropOffset(es, L"bIsAMS");
                L.IsMelee  = PropOffset(es, L"bIsMelee");
                L.Weapon   = PropOffset(es, L"Weapon");
            } else {
                L.RegOff = -1;
            }
            LogInfo("weapons: registry layout for %s: +%d stride %d name +%d part +%d weapon +%d",
                    cls->get_fname() ? WideToNarrow(cls->get_fname()->to_string().c_str()).c_str() : "?",
                    L.RegOff, L.Stride, L.SortName, L.MechPart, L.Weapon);
            if (L.RegOff < 0 && es == nullptr) LogStructMembers("weapon system (no registry)", cls);
            it = RegLayouts.emplace(cls, L).first;
        }
        const RegLayout& L = it->second;
        return (L.RegOff >= 0 && L.Stride > 0 && L.SortName >= 0) ? &L : nullptr;
    }

    // A unit with no WeaponSystem component: say once per class which weapon-ish components it
    // does have, so the next build can read those instead.
    std::unordered_set<const void*> WeaponlessLogged;
    void LogWeaponlessOnce(API::UObject* actor) {
        API::UClass* cls = actor ? actor->get_class() : nullptr;
        if (!cls || !WeaponlessLogged.insert(cls).second) return;
        std::string list;
        ForEachLiveComponent(actor, [&](API::UObject* c) {
            if (c->get_class() && c->get_class()->get_fname()) {
                const std::wstring n = c->get_class()->get_fname()->to_string();
                // Every component class, once each: the first build filtered on weapon-ish names
                // and found nothing on tanks and VTOLs, so their weapons are named otherwise.
                const std::string nn = WideToNarrow(n.c_str());
                if (list.find(nn) == std::string::npos) {
                    if (!list.empty()) list += ", ";
                    list += nn;
                }
            }
            return true;
        });
        LogInfo("weapons: %s has no WeaponSystem component; its components: %s",
                cls->get_fname() ? WideToNarrow(cls->get_fname()->to_string().c_str()).c_str() : "?",
                list.empty() ? "(none)" : list.c_str());
        // And what its attached child actors are made of, since the weapons are not on the unit.
        for (API::UObject* child : ChildActorsOf(actor)) {
            std::string cl;
            ForEachLiveComponent(child, [&](API::UObject* c) {
                if (c->get_class() && c->get_class()->get_fname()) {
                    const std::string nn = WideToNarrow(c->get_class()->get_fname()->to_string().c_str());
                    if (cl.find(nn) == std::string::npos) { if (!cl.empty()) cl += ", "; cl += nn; }
                }
                return true;
            });
            LogInfo("weapons:   child actor %s: %s",
                    child->get_class() && child->get_class()->get_fname()
                        ? WideToNarrow(child->get_class()->get_fname()->to_string().c_str()).c_str() : "?",
                    cl.empty() ? "(no components)" : cl.c_str());
        }
    }

    // ---- vehicle weapons: components on attached turret / pod actors ----------------------
    std::unordered_map<const void*, uint8_t> PodVerdicts;       // class -> is a weapon component

    std::unordered_set<const void*> PodUnitsLogged;

    bool IsWeaponComp(API::UObject* c) {
        const void* cls = c ? c->get_class() : nullptr;
        if (!cls) return false;
        auto it = PodVerdicts.find(cls);
        if (it != PodVerdicts.end()) return it->second != 0;
        uint8_t v = 0;
        std::string chain;
        for (API::UStruct* st = c->get_class(); st && st->get_fname(); st = st->get_super_struct()) {
            const std::wstring n = st->get_fname()->to_string();
            if (!chain.empty()) chain += " < ";
            chain += WideToNarrow(n.c_str());
            if (n == L"ActorComponent") break;
            if (NameHas(n, L"WeaponComponent") && !NameHas(n, L"WeaponSystem") && !NameHas(n, L"Emitter") &&
                !NameHas(n, L"Cooldown") && !NameHas(n, L"Jamming"))
                v = 1;
        }
        PodVerdicts.emplace(cls, v);
        if (v) LogInfo("weapons: vehicle weapon class %s", chain.c_str());
        return v != 0;
    }

    // A vehicle weapon's name comes from ITS OWN DATA, per weapon, never from its class.
    //
    // The class name lies. The log (vehwpn6): Scorpion_C's main gun sits on
    // PointDefense_Scorpion_MainGun_AC5_C but its component class is WeaponComponent_PPC_Manticore_C,
    // so the class token said "PPC"; the VTOL Warrior's AC5 is a WeaponComponent_AC2_SarLon_C; and
    // every Sunburn / Inferno / Pleiades / Cheesegrater weapon is the generic WeaponComponent_C,
    // which has no token at all. Designers reuse one component class and give each instance its
    // weapon through data. Caching the name per CLASS then spread the first answer to all of them.
    //
    // The data: GetWeaponData() returns { WeaponId <WeaponDataAssetId>, HealthState } (the log's
    // layout dump), and WeaponId is exactly the 16-byte argument GetWeaponDataAsset takes (the
    // "call(arg)" dump) - the same id -> asset -> GetShortDisplayName route the game's weapon panel
    // uses. So: GetWeaponData on this component every time (cheap, and a component pointer can be
    // reused by a later spawn), and the id -> name answer cached, since ids never change meaning.
    // The return struct holds FNames and an enum - nothing that owns heap memory - so one reused,
    // zeroed frame is safe.
    struct PodDataFn { API::UFunction* Fn = nullptr; int32_t Ret = -1, IdOff = -1, IdSize = 0, Frame = 0; };
    std::unordered_map<const void*, PodDataFn> PodDataFns;          // component class -> GetWeaponData
    std::unordered_map<std::string, std::string> PodIdNames;        // id bytes -> short name
    std::vector<uint8_t> PodFrame;
    bool PodStructLogged = false;

    const PodDataFn* PodDataFnFor(API::UObject* comp) {
        const void* cls = comp->get_class();
        auto it = PodDataFns.find(cls);
        if (it != PodDataFns.end()) return it->second.Fn ? &it->second : nullptr;
        PodDataFn d;
        for (API::UStruct* st = comp->get_class(); st && !d.Fn; st = st->get_super_struct())
            d.Fn = static_cast<API::UClass*>(st)->find_function(L"GetWeaponData");
        API::FProperty* ret = nullptr;
        if (d.Fn)
            for (auto* f = d.Fn->get_child_properties(); f; f = f->get_next()) {
                auto* pr = reinterpret_cast<API::FProperty*>(f);
                if (pr->get_fname() && pr->get_fname()->to_string() == L"ReturnValue") ret = pr;
            }
        d.Frame = d.Fn ? d.Fn->get_properties_size() : 0;
        API::UStruct* st = (ret && PropTypeOf(ret) == L"StructProperty")
                               ? reinterpret_cast<API::FStructProperty*>(ret)->get_struct() : nullptr;
        if (st && !PodStructLogged) { PodStructLogged = true; LogStructMembers("vehicle GetWeaponData", st); }
        API::FProperty* idp = nullptr;
        if (st) d.IdOff = PropOffset(st, L"WeaponId", &idp);
        if (idp) if (API::UStruct* ids = StructOfProp(idp)) d.IdSize = ids->get_properties_size();
        if (!d.Fn || !ret || d.IdOff < 0 || d.IdSize <= 0 || d.Frame <= 0 || d.Frame > 4096) d.Fn = nullptr;
        else d.Ret = ret->get_offset();
        if (PodDataFns.size() >= 256) PodDataFns.clear();
        it = PodDataFns.emplace(cls, d).first;
        return it->second.Fn ? &it->second : nullptr;
    }

    // "" = could not be named (the weapon is then left out rather than shown as something else).
    std::string ResolvePodName(API::UObject* comp) {
        if (const PodDataFn* d = PodDataFnFor(comp)) {
            if (PodFrame.size() < (size_t)d->Frame + 64) PodFrame.assign((size_t)d->Frame + 64, 0);
            else std::memset(PodFrame.data(), 0, (size_t)d->Frame + 64);
            comp->process_event(d->Fn, PodFrame.data());
            const uint8_t* id = PodFrame.data() + d->Ret + d->IdOff;
            const std::string key(reinterpret_cast<const char*>(id), (size_t)d->IdSize);
            auto it = PodIdNames.find(key);
            if (it != PodIdNames.end()) return it->second;

            char buf[48] = "";
            API::UObject* host = WeaponDataAssetCdo();
            API::UObject* wda = host ? CallArgObject(host, L"GetWeaponDataAsset", id, d->IdSize) : nullptr;
            if (wda && GetNoArgFn(wda, L"GetShortDisplayName"))
                if (uint8_t* sn = CallNoArg(wda, L"GetShortDisplayName")) TextToString(sn, buf, sizeof(buf));
            uint64_t raw[2] = {0, 0};
            std::memcpy(raw, id, d->IdSize < 16 ? (size_t)d->IdSize : 16);
            LogInfo("weapons: vehicle weapon %s id %016llx%016llx -> %s '%s'",
                    comp->get_class()->get_fname() ? WideToNarrow(comp->get_class()->get_fname()->to_string().c_str()).c_str() : "?",
                    (unsigned long long)raw[0], (unsigned long long)raw[1],
                    wda ? WideToNarrow(wda->get_fname()->to_string().c_str()).c_str() : "(no data asset)", buf);
            if (PodIdNames.size() >= 512) PodIdNames.clear();
            PodIdNames.emplace(key, buf);
            return buf;
        }
        char buf[48] = "";
        if (buf[0]) return buf;

        // The class name. The weapon is the token that looks like one: SRM6, LRM10, AC2, PPC...
        static const wchar_t* kWeap[] = {L"ERPPC", L"PPC", L"UAC", L"RAC", L"LBX", L"LB", L"AC", L"SRM", L"LRM", L"MRM",
                                         L"ATM", L"NARC", L"TAG", L"AMS", L"MG", L"ERLL", L"ERML", L"ERSL", L"LPL",
                                         L"MPL", L"SPL", L"LL", L"ML", L"SL", L"GAUSS", L"FLAMER", L"LASER", L"HAG"};
        std::wstring n = comp->get_class()->get_fname() ? comp->get_class()->get_fname()->to_string() : L"";
        std::vector<std::wstring> toks;
        for (size_t i = 0, j; i <= n.size(); i = j + 1) {
            j = n.find(L'_', i);
            if (j == std::wstring::npos) j = n.size();
            if (j > i) toks.push_back(n.substr(i, j - i));
        }
        std::wstring pick;
        for (const auto& t : toks) {
            std::wstring up;
            for (wchar_t c : t) up += (wchar_t)towupper(c);
            for (const wchar_t* w : kWeap) {
                const size_t wl = wcslen(w);
                if (up.compare(0, wl, w) != 0) continue;
                bool digitsOnly = true;
                for (size_t k = wl; k < up.size(); ++k) digitsOnly &= iswdigit(up[k]) != 0;
                if (digitsOnly) { pick = up; break; }
            }
            if (!pick.empty()) break;
        }
        if (pick.empty()) return "";
        std::string o;
        for (size_t i = 0; i < pick.size(); ++i) {
            if (i > 0 && iswdigit(pick[i]) && !iswdigit(pick[i - 1])) o += ' ';
            o += (pick[i] < 128) ? static_cast<char>(pick[i]) : '?';
        }
        return o;
    }

    // Resolved every call, per component - see ResolvePodName. Anti-missile systems are left out,
    // as they are for mechs: they are not something the pilot can be shot by.
    bool PodWeaponName(API::UObject* comp, char* out, size_t cap) {
        out[0] = '\0';
        const std::string nm = ResolvePodName(comp);
        if (IsAmsName(nm.c_str())) return false;
        strncpy_s(out, cap, nm.c_str(), _TRUNCATE);
        return out[0] != '\0';
    }

    uint8_t ReadPodWeapons(API::UObject* unit, TerrainWeapon* out) {
        struct Hit { API::UObject* Comp; API::UObject* Owner; };
        std::vector<Hit> hits;
        std::vector<API::UObject*> frontier{unit};
        for (int depth = 0; depth < 4 && !frontier.empty() && hits.size() < 32; ++depth) {
            std::vector<API::UObject*> next;
            for (API::UObject* a : frontier) {
                ForEachLiveComponent(a, [&](API::UObject* c) {
                    if (IsWeaponComp(c)) hits.push_back({c, a});
                    return true;
                });
                for (API::UObject* ch : ChildActorsOf(a)) next.push_back(ch);
            }
            frontier.swap(next);
        }
        uint8_t n = 0;
        for (const Hit& h : hits) {
            if (n >= (uint8_t)kRawWeapons) break;
            TerrainWeapon w{};
            w.Part = TMP_UNKNOWN;
            if (!PodWeaponName(h.Comp, w.Name, sizeof(w.Name))) continue;
            // A pod that has been shot off takes its weapon with it.
            if (h.Owner != unit)
                if (API::UObject* hc = FindCompByClassName(h.Owner, L"Health")) {
                    if (VehHpCur < 0 && hc->get_class()) {
                        VehHpCur = PropOffset(hc->get_class(), L"HitPoints");
                        VehHpMax = PropOffset(hc->get_class(), L"MaxHitPoints");
                    }
                    if (FracAt(hc, VehHpCur, VehHpMax) == 0.0f) w.Flags |= TWF_Destroyed;
                }
            out[n++] = w;
        }
        // Once per unit class: how many weapon components the walk found, and on what - so a
        // vehicle showing fewer weapons than it carries can be traced to where the rest live.
        if (PodUnitsLogged.insert(unit->get_class()).second) {
            std::string list;
            for (const Hit& h : hits) {
                char nm[48];
                PodWeaponName(h.Comp, nm, sizeof(nm));
                if (!list.empty()) list += ", ";
                list += nm[0] ? nm : "?";
                list += " on ";
                list += h.Owner && h.Owner->get_class() && h.Owner->get_class()->get_fname()
                            ? WideToNarrow(h.Owner->get_class()->get_fname()->to_string().c_str()) : "?";
            }
            LogInfo("weapons: %s carries %zu weapon component(s): %s",
                    unit->get_class()->get_fname() ? WideToNarrow(unit->get_class()->get_fname()->to_string().c_str()).c_str() : "?",
                    hits.size(), list.empty() ? "(none)" : list.c_str());
        }
        return n;
    }

    // out: TERR_MAX_WEAPONS grouped entries (see GroupWeapons).
    uint8_t ReadWeapons(API::UObject* mech, TerrainWeapon* out, bool logOnce,
                        const char names[kHudMax][20] = nullptr, uint8_t nameCount = 0,
                        void* const* hudWeapons = nullptr) {
        TerrainWeapon raw[kRawWeapons] = {};
        const uint8_t n = ReadWeaponsRaw(mech, raw, logOnce, names, nameCount, hudWeapons);
        return GroupWeapons(raw, n, out);
    }
    // out: up to kRawWeapons entries, one per weapon.
    uint8_t ReadWeaponsRaw(API::UObject* mech, TerrainWeapon* out, bool logOnce,
                           const char names[kHudMax][20] = nullptr, uint8_t nameCount = 0,
                           void* const* hudWeapons = nullptr) {
        // The game's own route first; the registry below is the fallback.
        Breadcrumb("weapons: equipment route");
        if (const uint8_t viaEq = ReadWeaponsViaEquipmentSafe(mech, out)) return viaEq;
        if (EqFaulted && !EqFaultLogged) {
            EqFaultLogged = true;
            LogError("weapons: the equipment route faulted reading GetEquipment's result - off for this session, "
                     "weapons come from the registry (long names where no short one is known)");
        }
        Breadcrumb("weapons: registry route");

        API::UObject* wc = WeaponCompOf(mech);
        if (!wc) {
            // Vehicles: no weapon system - their weapons are components on the turret and pod
            // actors attached to them (the log: WeaponComponent_VTOL_Warrior_Wing_Right_MH1_SRM2_C
            // on a PointDefense_..._Wing_Right_SRM2_C child actor).
            const uint8_t pods = ReadPodWeapons(mech, out);
            if (!pods) LogWeaponlessOnce(mech);
            return pods;
        }
        // The registry's layout is read off THIS component's own class. It used to be taken once
        // from the first mech and applied to everything - and a tank's or VTOL's weapon system
        // is a different class, so vehicles read the wrong offsets and came back with no weapons.
        const RegLayout* L = RegLayoutFor(wc);
        if (!L) return 0;

        FScriptArrayLayout arr{};
        std::memcpy(&arr, reinterpret_cast<const uint8_t*>(wc) + L->RegOff, sizeof(arr));
        if (!arr.Data || arr.Num <= 0) return 0;

        uint8_t n = 0;
        for (int32_t i = 0; i < arr.Num && n < (uint8_t)kRawWeapons; ++i) {
            const uint8_t* e = reinterpret_cast<const uint8_t*>(arr.Data) + (size_t)i * L->Stride;

            // Anti-missile systems and melee are not a threat readout - they clutter the label
            // with things the pilot cannot be shot by.
            if (L->IsAMS   >= 0 && e[L->IsAMS])   continue;
            if (L->IsMelee >= 0 && e[L->IsMelee]) continue;

            TerrainWeapon w{};
            w.Part = TMP_UNKNOWN;

            // OFF, AND NOT BECAUSE IT DID NOT WORK - BECAUSE IT CRASHED THE GAME.
            //
            // MechWeaponSystemEntry::Weapon is a TWeakObjectPtr, and resolving one by INDEX is
            // only half the job: a weak pointer also carries a serial number, and the pair is
            // what says the target is still alive. Index alone will happily hand back a
            // destroyed object, or whatever has since been allocated into that slot. Calling
            // get_class() on that - let alone process_event - is an access violation, which is
            // what EXCEPTION_ACCESS_VIOLATION at 0x179c was: a near-null object plus a member
            // offset.
            //
            // UEVR does expose FUObjectItem::serial_number, so the check is possible in
            // principle, but only by indexing the object array as if it were flat. UE4 commonly
            // uses a CHUNKED array, and guessing wrong there is another crash of exactly this
            // kind. Reaching into engine internals to repair a cosmetic abbreviation is a bad
            // trade, so this stays off until the short name can be reached through an object
            // whose lifetime is not in question - the weapon SYSTEM component, which is held
            // directly and never weakly.
            // Positional pairing against the RAW registry index, not the filtered one: the hud
            // list carries AMS and melee too (MWVR's panel shows AMS separately), so the two
            // sequences line up only before filtering. Guarded on the lengths matching exactly -
            // a mismatch means one list has lost an entry, and a weapon labelled with its
            // neighbour's name is worse than one labelled verbosely.
            // FALLBACK PATH. If this is running at all, the equipment route did not resolve.
            // The length guard below is the one that used to fail in silence; it now says so.
            // PAIRED BY OBJECT, NOT BY POSITION. The hud list and the registry disagree in
            // length (the log: "hud 9 vs registry 17"), so positional pairing was declined on
            // most mechs and their labels stayed verbose. Each hud entry names its weapon
            // component with a strong pointer the engine filled; the registry entry names the same
            // component with a weak pointer. The weak pointer's slot in the object array is
            // compared against the hud pointers - a pointer COMPARISON, nothing is dereferenced -
            // so the pair is exact whatever order or length either list has.
            char longName[20] = "";
            ReadFString(e + L->SortName, longName, sizeof(longName));
            for (size_t z = std::strlen(longName); z > 0 && longName[z - 1] == ' '; --z)
                longName[z - 1] = '\0';
            if (names && hudWeapons && nameCount && L->Weapon >= 0) {
                API::UObject* wobj = ResolveWeak(e + L->Weapon);
                if (wobj)
                    for (uint8_t k = 0; k < nameCount; ++k)
                        if (hudWeapons[k] == static_cast<void*>(wobj) && names[k][0]) {
                            std::memcpy(w.Name, names[k], sizeof(w.Name));
                            if (longName[0]) ShortByLong[longName] = names[k];
                            break;
                        }
            }
            // Not paired on this mech (its hud list lacks the weapon): the same weapon on any
            // other mech this mission already taught us its short name.
            if (!w.Name[0] && longName[0]) {
                auto sb = ShortByLong.find(longName);
                if (sb != ShortByLong.end())
                    strncpy_s(w.Name, sizeof(w.Name), sb->second.c_str(), _TRUNCATE);
            }
            if (!w.Name[0]) {
                std::memcpy(w.Name, longName, sizeof(w.Name));
                if (longName[0])
                    LogDistinctOnce("weapons: no short name for '%s' - shown in full", longName);
            }
            if (!w.Name[0]) continue;
            uint8_t raw = 0xFF;
            if (L->MechPart >= 0) { raw = e[L->MechPart]; w.Part = PartFromEnum(raw); }
            if (logOnce)
                LogInfo("weapons:   '%s' MechPart byte %u -> part %u", w.Name, raw, w.Part);
            out[n++] = w;
        }
        return n;
    }

    // Rear faces, in the contract's order. Exact property names, straight off the layout dump.
    static const wchar_t* RearPartName(int part) {
        switch (part) {
        case TRP_CentreTorso: return L"RearCenterTorso";
        case TRP_LeftTorso:   return L"RearLeftTorso";
        default:              return L"RearRightTorso";
        }
    }
    int32_t RearSlotOff[TRP_COUNT];

    API::UObject* LivingComp = nullptr;
    API::UObject* HeatComp   = nullptr;
    bool CondCompsChecked = false;

    // Per-part container object property offsets on the living-actor component, plus the
    // armour/health member offsets on a container. Resolved once.
    int32_t PartSlotOff[TMP_COUNT];
    int32_t CArmCur = -1, CArmMax = -1, CHpCur = -1, CHpMax = -1;
    int32_t HeatCurOff = -1, HeatMaxOff = -1, HeatPctOff = -1;

    void ResolveConditionComps(API::UObject* mech) {
        if (CondCompsChecked) return;
        CondCompsChecked = true;
        for (int i = 0; i < TMP_COUNT; ++i) PartSlotOff[i] = -1;
        for (int i = 0; i < TRP_COUNT; ++i) RearSlotOff[i] = -1;

        // By class name, off the engine's live component list. Matching the name rather than a
        // property name is still right - a renamed property must not hide a component that is
        // plainly there - but the list being searched is now the actor's registered components
        // instead of its property table.
        if (!LivingComp) LivingComp = CompByClassName(mech, L"LivingActor");
        if (!HeatComp)   HeatComp   = CompByClassName(mech, L"Heat");

        if (LivingComp && LivingComp->get_class()) {
            auto* lc = LivingComp->get_class();
            for (int i = 0; i < TMP_COUNT; ++i)
                for (int a = 0; a < 3 && PartSlotOff[i] < 0; ++a)
                    if (const wchar_t* nm = PartNames(i, a))
                        PartSlotOff[i] = PropOffset(lc, nm);
            for (int i = 0; i < TRP_COUNT; ++i)
                RearSlotOff[i] = PropOffset(lc, RearPartName(i));

            // Any resolved container describes the rest of them.
            //
            // THESE NAMES ARE NOT GUESSES. They came out of the HarJel mod's pak (an armour
            // repair mod, so it reads and writes exactly these fields): MWArmoredHealthComponent
            // carries CurrentArmor / InstalledArmor for the front face, CurrentRearArmor /
            // InstalledRearArmor for the back, and CurrentStructure for what is underneath. The
            // structure MAXIMUM is the one still-open name, so it keeps an ordered candidate
            // list, and the layout dump below prints the container's members either way.
            for (int i = 0; i < TMP_COUNT && CArmCur < 0; ++i) {
                if (PartSlotOff[i] < 0) continue;
                auto** slot = reinterpret_cast<API::UObject**>(
                    reinterpret_cast<uint8_t*>(LivingComp) + PartSlotOff[i]);
                auto* cont = slot ? *slot : nullptr;
                if (!cont || !cont->get_class()) continue;
                auto* cc = cont->get_class();

                // ArmorPoints / MaxArmorPoints, confirmed by the component's own member dump.
                // CurrentArmor / InstalledArmor are the LOADOUT ASSET's names for the same idea
                // (persistent between-mission state) and do not exist on the live component -
                // the log said "armour ?/MaxArmorPoints", which is what sent me to the dump.
                static const wchar_t* kArmCur[] = { L"ArmorPoints", L"CurrentArmor" };
                static const wchar_t* kArmMax[] = { L"MaxArmorPoints", L"InstalledArmor" };
                // STRUCTURE IS HitPoints / MaxHitPoints, and the ordering here matters.
                // MWArmoredHealthComponent extends MWHealthComponent, and it is the BASE that
                // carries HitPoints, MaxHitPoints and SetHitPoints - HarJel's repair handler
                // calls exactly those. CurrentStructure is a different thing: it lives on
                // MWMechLoadoutAsset's MechPartsHealth struct, which is the mech's PERSISTENT
                // between-mission state, not its live condition. Reading that would have shown a
                // mech's damage as it was when it launched and never updated during the fight.
                static const wchar_t* kStrCur[] = { L"HitPoints", L"CurrentStructure",
                                                    L"CurrentHealth" };
                static const wchar_t* kStrMax[] = { L"MaxHitPoints", L"InstalledStructure",
                                                    L"MaxStructure", L"MaxHealth" };

                const wchar_t* pAC = nullptr; const wchar_t* pAM = nullptr;
                const wchar_t* pSC = nullptr; const wchar_t* pSM = nullptr;
                CArmCur = PropOffsetFirst(cc, kArmCur, 2, &pAC);
                CArmMax = PropOffsetFirst(cc, kArmMax, 2, &pAM);
                CHpCur  = PropOffsetFirst(cc, kStrCur, 3, &pSC);
                CHpMax  = PropOffsetFirst(cc, kStrMax, 4, &pSM);

                LogInfo("condition: container %s armour %s/%s structure %s/%s",
                        WideToNarrow(cc->get_fname()->to_string().c_str()).c_str(),
                        pAC ? WideToNarrow(pAC).c_str() : "?", pAM ? WideToNarrow(pAM).c_str() : "?",
                        pSC ? WideToNarrow(pSC).c_str() : "?", pSM ? WideToNarrow(pSM).c_str() : "?");
            }
        }

        if (HeatComp && HeatComp->get_class()) {
            auto* hc = HeatComp->get_class();
            // HeatPercentage is published by MWHeatComponent ready-made, which removes the
            // divide and the question of what the capacity is called. CurrentHeat/MaxHeat is the
            // fallback. Both names are from the HarJel pak, not guessed.
            HeatPctOff = PropOffset(hc, L"HeatPercentage");
            HeatCurOff = PropOffset(hc, L"CurrentHeat");
            static const wchar_t* kHeatMax[] = { L"MaxHeat", L"HeatCapacity", L"MaxHeatCapacity" };
            HeatMaxOff = PropOffsetFirst(hc, kHeatMax, 3);
        }

        int partsFound = 0;
        for (int i = 0; i < TMP_COUNT; ++i) partsFound += (PartSlotOff[i] >= 0) ? 1 : 0;
        int rearFound = 0;
        for (int i = 0; i < TRP_COUNT; ++i) rearFound += (RearSlotOff[i] >= 0) ? 1 : 0;
        LogInfo("condition: living=%p heat=%p parts=%d/%d rear=%d/%d armour(%d,%d) struct(%d,%d) heat(pct %d, %d/%d)",
                (void*)LivingComp, (void*)HeatComp, partsFound, (int)TMP_COUNT,
                rearFound, (int)TRP_COUNT,
                CArmCur, CArmMax, CHpCur, CHpMax, HeatPctOff, HeatCurOff, HeatMaxOff);
    }

    static float FracAt(API::UObject* o, int32_t curOff, int32_t maxOff) {
        if (!o || curOff < 0) return -1.0f;
        auto* base = reinterpret_cast<const uint8_t*>(o);
        float cur = 0.0f, mx = 0.0f;
        std::memcpy(&cur, base + curOff, sizeof(float));
        if (maxOff >= 0) std::memcpy(&mx, base + maxOff, sizeof(float));
        if (mx <= 0.0f) return (cur > 0.0f) ? 1.0f : 0.0f;
        const float f = cur / mx;
        return (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    // ---- calling a no-argument reflected function ---------------------------------------------
    //
    // RESOLVED PER CLASS, AND THAT IS THE WHOLE POINT OF THIS HELPER.
    //
    // The first version cached one UFunction per call site, resolved from whatever object it
    // happened to see first, and then used it on every object afterwards. That crashed the game:
    // EquipOwnerOf resolved GetMechDataAsset off a mech and then ran it against a TANK, which is
    // an access violation the moment the callee touches a member that a tank does not have.
    //
    // This is the same mistake as the TWeakObjectPtr crash wearing different clothes. Both times
    // I held a handle that was only valid in one context and used it in another. process_event is
    // not checked - handing it a function that does not belong to the object's class is undefined
    // and lands inside the game's own code, where the stack trace says nothing useful.
    //
    // So the cache is keyed on the CLASS, and an object whose class does not have the function
    // gets a null back rather than somebody else's.
    struct NoArgFn {
        API::UFunction* Fn = nullptr;
        int32_t Ret = -1;
        API::FProperty* RetProp = nullptr;
        std::vector<uint8_t> Buf;
        bool Ok = false;
    };
    std::unordered_map<std::wstring, NoArgFn> NoArgCache;

    // `fallback`: the function to use when the class chain declares none - an INTERFACE's
    // function, which the caller must first have checked the class implements.
    NoArgFn* GetNoArgFn(API::UObject* obj, const wchar_t* name, API::UFunction* fallback = nullptr) {
        if (!obj || !obj->get_class()) return nullptr;
        // fmttrunc1: the cache key is a growable string - never truncated, so two long names can't share a key.
        wchar_t ptr[32];
        _snwprintf_s(ptr, _countof(ptr), _TRUNCATE, L"%p|", (void*)obj->get_class());
        std::wstring key = std::wstring(ptr) + name;
        auto it = NoArgCache.find(key);
        if (it != NoArgCache.end()) return it->second.Ok ? &it->second : nullptr;
        if (NoArgCache.size() >= 256) NoArgCache.clear();

        NoArgFn e;
        for (API::UStruct* st = obj->get_class(); st && !e.Fn; st = st->get_super_struct())
            e.Fn = static_cast<API::UClass*>(st)->find_function(name);
        if (!e.Fn) e.Fn = fallback;
        if (e.Fn) {
            for (auto* f = e.Fn->get_child_properties(); f; f = f->get_next()) {
                auto* pr = reinterpret_cast<API::FProperty*>(f);
                if (pr->get_fname() && pr->get_fname()->to_string() == L"ReturnValue") {
                    e.Ret = pr->get_offset(); e.RetProp = pr;
                }
            }
            const int32_t frame = e.Fn->get_properties_size();
            if (frame > 0 && e.Ret >= 0) { e.Buf.assign((size_t)frame + 64, 0); e.Ok = true; }
        }
        LogDistinctOnce("call: %s on %s -> %s", WideToNarrow(name).c_str(),
                        WideToNarrow(obj->get_class()->get_fname()->to_string().c_str()).c_str(),
                        e.Ok ? "ok" : "not available");
        auto ins = NoArgCache.emplace(key, std::move(e));
        return ins.first->second.Ok ? &ins.first->second : nullptr;
    }

    // Call it and hand back the raw return slot, or null. The object is always the one the
    // function was resolved against, which is what makes this safe.
    uint8_t* CallNoArg(API::UObject* obj, const wchar_t* name) {
        NoArgFn* e = GetNoArgFn(obj, name);
        if (!e) return nullptr;
        // No inputs, and the buffer is this entry's alone: not zeroed, so the engine frees the
        // previous result (GetEquipment's arrays, GetShortDisplayName's text) as it assigns.
        obj->process_event(e->Fn, e->Buf.data());
        return e->Buf.data() + e->Ret;
    }

    // ---- the pilot, for the player's own lance ------------------------------------------------
    // MWVR's readout shows a lancemate's callsign rather than repeating the chassis, which is the
    // one thing on that label you cannot get from the silhouette. Read from the unit's persona
    // component. The squad test is deliberately the team flag rather than IsUnitInPlayerSquad:
    // the team is already resolved for every contact, and for labelling purposes "on my side" and
    // "in my lance" want the same treatment.
    int32_t PersonaCallsign = -1;
    bool    PersonaProbed = false;

    // A component held in a NAMED property, read the way ChassisOf reads MechTargetComponent.
    // No search, no recursion, one dereference of one pointer the class says is there.
    static API::UObject* CompByName(API::UObject* owner, const wchar_t* prop) {
        if (!owner) return nullptr;
        auto* slot = owner->get_property_data<void*>(prop);
        return (slot && *slot) ? reinterpret_cast<API::UObject*>(*slot) : nullptr;
    }

    void CallsignOf(API::UObject* unit, char out[24]) {
        out[0] = '\0';
        if (!kPersonaCallsign) return;

        // WHY THIS IS NOT A SEARCH ANY MORE. It used to call FindCompByClassName(unit, "Persona"),
        // which defaults to depth 1: it walks every object property on the mech, and for each one
        // that does not match it walks every object property on THAT component too, calling
        // get_class() on each pointer it finds. A mech has hundreds, and some of them are
        // uninitialised blueprint variables or references to actors that are already gone.
        // Dereferencing one of those is the access violation at 0x4c60000b686 - found by the
        // breadcrumb file, which named this step.
        //
        // The search was never necessary. The log's own component dump says
        // "component PersonaComponent -> MWPersonaComponent": it is a named property, so ask for
        // it by name and touch exactly one pointer.
        API::UObject* pc = CompByName(unit, L"PersonaComponent");
        if (!pc || !pc->get_class()) return;

        // Callsign may be a plain member; only reach for the accessor if it is not. (A property
        // walk can never find a function - that has cost three separate rounds in this file.)
        if (!PersonaProbed) {
            PersonaProbed = true;
            PersonaCallsign = PropOffset(pc->get_class(), L"Callsign");
            LogInfo("persona: %s Callsign +%d",
                    WideToNarrow(pc->get_class()->get_fname()->to_string().c_str()).c_str(),
                    PersonaCallsign);
        }
        if (PersonaCallsign >= 0) {
            auto* at = reinterpret_cast<uint8_t*>(pc) + PersonaCallsign;
            if (TextToString(at, out, 24) && out[0]) return;
            ReadFNameAt(at, out, 24);
            if (out[0]) return;
        }

        NoArgFn* e = GetNoArgFn(pc, L"GetPersonaData");
        if (!e) return;
        uint8_t* ret = CallNoArg(pc, L"GetPersonaData");
        if (!ret) return;
        API::UStruct* ps = StructOfProp(e->RetProp);
        if (!ps) return;
        // READ BY ITS DECLARED TYPE, NOT BY ASSUMING FText.
        //
        // The first version called TextToString unconditionally. Conv_TextToString on 24 bytes
        // that are NOT an FText does not fail - it returns a string of garbage - and because the
        // renderer showed the callsign in place of the variant, that garbage replaced a perfectly
        // good "CPLT-C2" on screen every time a target was acquired. A read that cannot fail
        // safely must be dispatched on the property's actual type.
        API::FProperty* cp = nullptr;
        const int32_t off = PropOffset(ps, L"Callsign", &cp);
        if (off < 0 || !cp) { LogStructMembers("PersonaData", ps); return; }
        const std::wstring ty = PropTypeOf(cp);
        if      (ty == L"TextProperty") TextToString(ret + off, out, 24);
        else if (ty == L"StrProperty")  ReadFString(ret + off, out, 24);
        else if (ty == L"NameProperty") ReadFNameAt(ret + off, out, 24);
        else {
            LogDistinctOnce("persona: Callsign is %s - not a string, ignored",
                            WideToNarrow(ty.c_str()).c_str());
            out[0] = '\0';
            return;
        }
        LogDistinctOnce("persona: Callsign %s +%d -> '%s'",
                        WideToNarrow(ty.c_str()).c_str(), off, out);

        // And a last sanity gate: a callsign is short printable text. Anything else is a
        // misread, and showing it is worse than showing nothing.
        for (int i = 0; out[i]; ++i) {
            if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7E) { out[0] = '\0'; return; }
        }
    }

    // ---- armour and structure, via the game's own per-part calculation ------------------------
    //
    // WITHDRAWN, AND WHY. MechPaperDoll exposes CalculateHealthArmorPctForPart, which returns
    // ArmorPct and HealthPct together and would replace offsets I have already had to correct
    // twice. To call it I need a live widget, and MWTargetComponent holds one at PaperDollWidget.
    //
    // My first attempt CACHED that widget pointer and dereferenced it on later frames. A paperdoll
    // widget is created and destroyed as the player's target changes, so that is a dangling
    // pointer - the third time in this file I have held something past its lifetime, after the
    // weak object pointer and the cross-class UFunction. It also re-armed itself every scan, so it
    // would keep retrying forever once the widget died.
    //
    // The probe is removed rather than repaired. It produced a diagnostic line and nothing the
    // display uses, so carrying any lifetime risk for it is a bad trade. The proven offset path is
    // what has been drawing the dolls all along and it stays.

    // ==========================================================================================
    // WEAPONS, THE WAY THE GAME DOES IT
    //
    // Walked out of the game's own two widgets rather than guessed, after three wrong guesses:
    //
    //   HUD_TargetWeaponLoadout:  cast MWMech -> CriticalSlotComponent
    //                             -> cast THAT to MWEquipmentOwnerInterface
    //                             -> GetEquipment() -> { Equipment, Weapons }
    //                             -> per entry: SetupWeaponInfo(WeaponData)
    //   HUD_WeaponPanel:          WeaponData -> WeaponId
    //                             -> GetWeaponDataAsset -> MWWeaponDataAsset
    //                             -> GetShortDisplayName -> TextToUpper -> SetText
    //
    // THE OWNER IS THE CRITICAL SLOT COMPONENT. Not the mech - which is exactly what the log
    // meant by "GetEquipment not found on DerivedMech_C" - and not the mech data asset, which was
    // my second guess. MWMechCriticalSlotComponent is the thing that implements the interface.
    //
    // That also makes this LIVE data. I previously argued against this whole route on the grounds
    // that it reads a variant's stock loadout and so cannot show battlefield losses. That was
    // true of the data asset I had wrongly picked as the owner. The critical slot component is
    // the one that fires OnEquipmentDamaged, OnWeaponsDamaged and OnAnyMechEquipmentChanged, so
    // it is the live state, and the objection does not apply.
    // ==========================================================================================

    bool    EqChecked = false, EqUsable = false;
    int32_t EqWeaponsAt = -1;     // the Weapons member inside GetEquipment's return struct
    int32_t EqEntryStride = 0;    // one weapon-data entry
    int32_t EqIdAt = -1;          // WeaponId / WeaponDataAssetId inside an entry
    int32_t EqIdSize = 0;
    int32_t EqHealthAt = -1;      // WeaponData.HealthState inside an entry
    std::unordered_map<void*, int32_t> EqPartOffByClass;

    // Resolve the shape of GetEquipment's return once. Everything is by NAME - the names come
    // from the widgets above - and a failure logs the struct so the next run names the field
    // rather than leaving us to guess at it again.
    bool EnsureEquipmentShape(API::UObject* owner) {
        if (EqChecked) return EqUsable;
        NoArgFn* e = GetNoArgFn(owner, L"GetEquipment");
        if (!e) return false;
        EqChecked = true;

        API::UStruct* ret = StructOfProp(e->RetProp);
        if (!ret) { LogError("weapons: GetEquipment return is not a struct"); return false; }

        API::FProperty* weapons = nullptr;
        EqWeaponsAt = PropOffset(ret, L"Weapons", &weapons);
        if (EqWeaponsAt < 0 || !weapons || PropTypeOf(weapons) != L"ArrayProperty") {
            LogError("weapons: no Weapons array on the GetEquipment return");
            LogStructMembers("EquipmentReturn", ret);
            return false;
        }
        auto* inner = reinterpret_cast<API::FArrayProperty*>(weapons)->get_inner();
        API::UStruct* es = StructOfProp(inner);
        if (!es) { LogError("weapons: Weapons inner is not a struct"); return false; }
        const int32_t sz = es->get_properties_size(), al = es->get_min_alignment();
        EqEntryStride = (al > 0) ? ((sz + al - 1) / al) * al : sz;

        // THE ID IS entry.WeaponData.WeaponId, NOT entry.WeaponId. The entry (log):
        //     MechPart EnumProperty +0, WeaponId NameProperty +4, WeaponData StructProperty +12
        // and HUD_TargetWeaponLoadout hands WeaponData to HUD_WeaponPanel.SetupWeaponInfo, which
        // breaks it into { WeaponId, HealthState } and passes THAT WeaponId (a 16-byte
        // WeaponDataAssetId) to GetWeaponDataAsset. The entry's own WeaponId is an 8-byte FName -
        // half an argument - which is why every call so far came back null.
        API::FProperty* idp = nullptr;
        {
            API::FProperty* wdp = nullptr;
            const int32_t wdo = PropOffset(es, L"WeaponData", &wdp);
            API::UStruct* wds = wdp ? StructOfProp(wdp) : nullptr;
            API::FProperty* wip = nullptr;
            const int32_t wio = wds ? PropOffset(wds, L"WeaponId", &wip) : -1;
            if (wdo >= 0 && wio >= 0 && wip && StructOfProp(wip)) {
                EqIdAt = wdo + wio; idp = wip;
                const int32_t ho = PropOffset(wds, L"HealthState");
                EqHealthAt = ho >= 0 ? wdo + ho : -1;
                LogInfo("weapons: entry id 'WeaponData.WeaponId' +%d, HealthState +%d", EqIdAt, EqHealthAt);
                LogStructMembers("WeaponData", wds);
            }
        }
        if (EqIdAt < 0)
        for (const wchar_t* nm : { L"WeaponId", L"WeaponDataAssetId", L"WeaponDataAsset" }) {
            EqIdAt = PropOffset(es, nm, &idp);
            if (EqIdAt >= 0) { LogInfo("weapons: entry id '%s' +%d", WideToNarrow(nm).c_str(), EqIdAt); break; }
        }
        if (EqIdAt < 0 || !idp) {
            LogError("weapons: no weapon id on the entry struct");
            LogStructMembers("WeaponDataEntry", es);
            return false;
        }
        if (API::UStruct* ids = StructOfProp(idp)) EqIdSize = ids->get_properties_size();
        if (EqIdSize <= 0) EqIdSize = (int32_t)sizeof(void*);

        LogStructMembers("WeaponDataEntry", es);   // once - it also carries the mount
        EqUsable = (EqEntryStride > 0);
        LogInfo("weapons: GetEquipment ok - Weapons +%d stride %d id +%d size %d",
                EqWeaponsAt, EqEntryStride, EqIdAt, EqIdSize);
        return EqUsable;
    }

    // ---- calling a reflected function that TAKES an argument ---------------------------------
    // Every other call in this plugin is no-arg. GetWeaponDataAsset takes the weapon id, so the
    // frame needs the argument written into the input parameter's slot before the call.
    struct ArgFn {
        API::UFunction* Fn = nullptr;
        int32_t In = -1, Ret = -1, InSize = 0;
        std::vector<uint8_t> Buf;
        bool Ok = false;
    };
    std::unordered_map<std::wstring, ArgFn> ArgCache;

    ArgFn* GetArgFn(API::UObject* obj, const wchar_t* name) {
        if (!obj || !obj->get_class()) return nullptr;
        // fmttrunc1: the cache key is a growable string - never truncated, so two long names can't share a key.
        wchar_t ptr[32];
        _snwprintf_s(ptr, _countof(ptr), _TRUNCATE, L"%p|", (void*)obj->get_class());
        std::wstring key = std::wstring(ptr) + name;
        auto it = ArgCache.find(key);
        if (it != ArgCache.end()) return it->second.Ok ? &it->second : nullptr;
        if (ArgCache.size() >= 128) ArgCache.clear();

        ArgFn e;
        for (API::UStruct* st = obj->get_class(); st && !e.Fn; st = st->get_super_struct())
            e.Fn = static_cast<API::UClass*>(st)->find_function(name);
        if (e.Fn) {
            for (auto* f = e.Fn->get_child_properties(); f; f = f->get_next()) {
                auto* pr = reinterpret_cast<API::FProperty*>(f);
                if (!pr->get_fname()) continue;
                if (pr->is_return_param()) e.Ret = pr->get_offset();
                else if (pr->is_param() && e.In < 0) e.In = pr->get_offset();
            }
            // THE ARGUMENT'S SIZE IS THE GAP TO THE RETURN VALUE, and that is not a heuristic -
            // the parameters are laid out in order in the frame, so ReturnValue's offset is
            // exactly where the input ends.
            //
            // This is what was wrong. The log dumped the frame:
            //     param WeaponDataAssetId  StructProperty  +0
            //     param ReturnValue        ObjectProperty  +16
            // so the id is SIXTEEN bytes. I was copying eight, because StructOfProp gave nothing
            // for the entry member and I fell back to sizeof(void*) - a guess dressed as a
            // default. Half an id is a valid call with a meaningless argument, which is why it
            // returned null every time instead of failing.
            const int32_t frame = e.Fn->get_properties_size();
            if (e.Ret > e.In && e.In >= 0) e.InSize = e.Ret - e.In;
            if (frame > 0 && e.Ret >= 0 && e.In >= 0 && e.InSize > 0) {
                e.Buf.assign((size_t)frame + 64, 0);
                e.Ok = true;
                LogInfo("call(arg): %s frame %d, in +%d size %d, ret +%d",
                        WideToNarrow(name).c_str(), frame, e.In, e.InSize, e.Ret);
            }

            // THE CALL SUCCEEDING IS NOT THE SAME AS THE CALL BEING RIGHT. The log said
            // "GetWeaponDataAsset on MWWeaponDataAsset -> ok" and then "did not resolve": the
            // function was found and invoked, and handed back null, which means what went INTO
            // the frame was not what it wanted. So the parameter list is printed once - name,
            // type and offset - because the argument I am passing is the only thing left to be
            // wrong, and its declared type names the mistake.
            for (auto* f = e.Fn->get_child_properties(); f; f = f->get_next()) {
                auto* pr = reinterpret_cast<API::FProperty*>(f);
                if (!pr->get_fname()) continue;
                LogInfo("call(arg):   %s param %-22s %-16s +%d",
                        WideToNarrow(name).c_str(),
                        WideToNarrow(pr->get_fname()->to_string().c_str()).c_str(),
                        WideToNarrow(PropTypeOf(pr).c_str()).c_str(), pr->get_offset());
            }
        }
        LogDistinctOnce("call(arg): %s on %s -> %s", WideToNarrow(name).c_str(),
                        WideToNarrow(obj->get_class()->get_fname()->to_string().c_str()).c_str(),
                        e.Ok ? "ok" : "not available");
        auto ins = ArgCache.emplace(key, std::move(e));
        return ins.first->second.Ok ? &ins.first->second : nullptr;
    }

    API::UObject* CallArgObject(API::UObject* obj, const wchar_t* name,
                                const void* arg, int32_t avail) {
        ArgFn* e = GetArgFn(obj, name);
        if (!e) return nullptr;
        // Copy what the FUNCTION asks for, never more than the source actually holds.
        const int32_t n = (avail > 0 && avail < e->InSize) ? avail : e->InSize;
        if (n <= 0 || (size_t)e->In + (size_t)n > e->Buf.size()) return nullptr;
        // Only the INPUT region is cleared; the return slot keeps its previous value so the
        // engine's assignment frees it, rather than orphaning it.
        std::memset(e->Buf.data() + e->In, 0, (size_t)e->InSize);
        std::memcpy(e->Buf.data() + e->In, arg, (size_t)n);
        obj->process_event(e->Fn, e->Buf.data());
        return *reinterpret_cast<API::UObject**>(e->Buf.data() + e->Ret);
    }

    // The mount, per entry class. Names taken from the entry dump above on the first run.
    uint8_t EquipmentPart(API::UStruct* es, const uint8_t* entry) {
        if (!es || !entry) return TMP_UNKNOWN;
        void* k = (void*)es;
        auto it = EqPartOffByClass.find(k);
        if (it == EqPartOffByClass.end()) {
            int32_t off = -1;
            for (const wchar_t* nm : { L"MechPart", L"Location", L"MountLocation", L"Part",
                                       L"EquipmentLocation" }) {
                off = PropOffset(es, nm);
                if (off >= 0) { LogInfo("weapons: entry mount '%s' +%d",
                                        WideToNarrow(nm).c_str(), off); break; }
            }
            if (EqPartOffByClass.size() >= 64) EqPartOffByClass.clear();
            it = EqPartOffByClass.emplace(k, off).first;
        }
        return (it->second < 0) ? TMP_UNKNOWN : PartFromEnum(entry[it->second]);
    }

    // ---- where GetWeaponDataAsset lives ------------------------------------------------------
    //
    // The log settled where it is NOT: "GetWeaponDataAsset unavailable on
    // MWMechCriticalSlotComponent". The component owns GetEquipment, not this.
    //
    // Located from the paks instead of by trial: seven assets in the base game call
    // GetWeaponDataAsset, and every one of them - including HUD_WeaponPanel, the widget that
    // actually draws the short name - imports MWWeaponDataAsset. That is the UE pattern for a
    // static accessor that turns an id into its asset, and it is called on the CLASS, not on an
    // instance. We already drive two functions exactly this way: Conv_TextToString on
    // KismetTextLibrary's default object and GetAllActorsOfClass on GameplayStatics'.
    //
    // The class is resolved the same way MWTank and MWVTOL are, by walking the object array once
    // for the name, because find_uobject does not return /Script/MechWarrior classes here.
    API::UObject* WdaCdo = nullptr;
    bool WdaCdoTried = false;

    API::UObject* WeaponDataAssetCdo() {
        if (WdaCdoTried) return WdaCdo;
        WdaCdoTried = true;

        // The cheap route first, in case this one IS reachable by path.
        for (const wchar_t* path : { L"Class /Script/MechWarrior.MWWeaponDataAsset",
                                     L"Class /Script/MechWarrior.MWWeaponData" }) {
            if (auto* c = API::get()->find_uobject<API::UClass>(path)) {
                WdaCdo = c->get_class_default_object();
                if (WdaCdo) { LogInfo("weapons: MWWeaponDataAsset via find_uobject"); return WdaCdo; }
            }
        }

        auto*      sdk   = API::get()->param()->sdk;
        const auto arr   = sdk->functions->get_uobject_array();
        const int  total = arr ? sdk->uobject_array->get_object_count(arr) : 0;
        for (int i = 0; i < total; ++i) {
            const auto obj = sdk->uobject_array->get_object(arr, i);
            if (!obj) continue;
            auto* o = reinterpret_cast<API::UObject*>(obj);
            auto* cls = o->get_class();
            if (!cls || !cls->get_fname()) continue;
            if (cls->get_fname()->to_string() != L"MWWeaponDataAsset") continue;
            WdaCdo = cls->get_class_default_object();
            if (WdaCdo) {
                LogInfo("weapons: MWWeaponDataAsset resolved from a live instance");
                return WdaCdo;
            }
        }
        LogError("weapons: MWWeaponDataAsset not found - short names unavailable");
        return nullptr;
    }

    API::UStruct* EqEntryStruct = nullptr;

    // "AMS", "AMS (C)", "AMS (V)", "LAMS"... - the game's short names carry a tech suffix.
    static bool IsAmsName(const char* n) {
        return !std::strncmp(n, "AMS", 3) || !std::strncmp(n, "LAMS", 4);
    }

    // MELEE, hidden like it is on the registry route (which reads IsMelee off the entry). The
    // data asset is asked first: any BoolProperty with "Melee" in its name, or an enum/name
    // property whose value names melee. Its class layout is logged once so the flag can be
    // pinned down by name. Failing that, the short name ("Light Hand", "Hatchet"...).
    std::unordered_set<std::string> PodIdMelee;
    bool WdaLayoutLogged = false;
    bool IsMeleeWeapon(API::UObject* wda, const char* shortName) {
        if (wda && wda->get_class()) {
            if (!WdaLayoutLogged) { WdaLayoutLogged = true; LogStructMembers("MWWeaponDataAsset", wda->get_class()); }
            bool found = false, melee = false;
            ForEachProp(wda->get_class(), [&](API::FProperty* p) {
                if (found || !p->get_fname()) return;
                const std::wstring n = p->get_fname()->to_string();
                if (NameHas(n, L"Melee") && PropTypeOf(p) == L"BoolProperty") {
                    found = true;
                    melee = reinterpret_cast<API::FBoolProperty*>(p)->get_value_from_object(wda);
                }
            });
            if (found) return melee;
        }
        static const char* kMelee[] = {"Hand", "Hatchet", "Sword", "Club", "Fist", "Claw", "Talon",
                                       "Mace", "Kick", "Melee", "Punch", "Axe", "Blade"};
        for (const char* k : kMelee)
            if (std::strstr(shortName, k)) return true;
        return false;
    }

    // A /Script/MechWarrior class by name: find_uobject first, then the object array (the way
    // WeaponDataAssetCdo finds its class, since find_uobject has not always returned these).
    API::UClass* ScriptClass(const wchar_t* name) {
        std::wstring path = std::wstring(L"Class /Script/MechWarrior.") + name;
        if (auto* c = API::get()->find_uobject<API::UClass>(path)) return c;
        auto*      sdk   = API::get()->param()->sdk;
        const auto arr   = sdk->functions->get_uobject_array();
        const int  total = arr ? sdk->uobject_array->get_object_count(arr) : 0;
        for (int i = 0; i < total; ++i) {
            auto* o = reinterpret_cast<API::UObject*>(sdk->uobject_array->get_object(arr, i));
            if (!o || !o->get_fname() || o->get_fname()->to_string() != name) continue;
            auto* oc = o->get_class();
            if (oc && oc->get_fname() && oc->get_fname()->to_string() == L"Class")
                return reinterpret_cast<API::UClass*>(o);
        }
        return nullptr;
    }

    API::UClass*    EqIface = nullptr;
    API::UFunction* EqIfaceFn = nullptr;
    bool            EqIfaceTried = false;
    std::unordered_map<const void*, bool> EqOwnerVerdicts; // unit class -> implements it

    // Does this unit implement MWEquipmentOwnerInterface? Asked of the ENGINE
    // (KismetSystemLibrary::DoesImplementInterface), never assumed: calling an interface
    // function on an object that does not implement it is undefined behaviour inside the game.
    bool IsEquipmentOwner(API::UObject* unit) {
        const void* cls = unit ? unit->get_class() : nullptr;
        if (!cls) return false;
        auto it = EqOwnerVerdicts.find(cls);
        if (it != EqOwnerVerdicts.end()) return it->second;

        if (!EqIfaceTried) {
            EqIfaceTried = true;
            EqIface = ScriptClass(L"MWEquipmentOwnerInterface");
            EqIfaceFn = EqIface ? EqIface->find_function(L"GetEquipment") : nullptr;
            LogInfo("weapons: MWEquipmentOwnerInterface %s, GetEquipment %s",
                    EqIface ? "found" : "NOT found", EqIfaceFn ? "found" : "NOT found");
        }
        bool yes = false;
        auto* ksl = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.KismetSystemLibrary");
        API::UObject* cdo = ksl ? ksl->get_class_default_object() : nullptr;
        API::UFunction* fn = ksl ? ksl->find_function(L"DoesImplementInterface") : nullptr;
        if (EqIface && cdo && fn) {
            int32_t to = -1, io = -1, ro = -1;
            for (auto* f = fn->get_child_properties(); f; f = f->get_next()) {
                auto* pr = reinterpret_cast<API::FProperty*>(f);
                if (!pr->get_fname()) continue;
                const std::wstring n = pr->get_fname()->to_string();
                if (n == L"TestObject") to = pr->get_offset();
                else if (n == L"Interface") io = pr->get_offset();
                else if (n == L"ReturnValue") ro = pr->get_offset();
            }
            const int32_t frame = fn->get_properties_size();
            if (to >= 0 && io >= 0 && ro >= 0 && frame > 0 && frame <= 256) {
                uint8_t buf[256 + 16] = {};
                std::memcpy(buf + to, &unit, sizeof(void*));
                std::memcpy(buf + io, &EqIface, sizeof(void*));
                cdo->process_event(fn, buf);
                yes = buf[ro] != 0;
            }
        }
        // The function: the class's own if it declares one, the interface's otherwise.
        if (yes && !GetNoArgFn(unit, L"GetEquipment", EqIfaceFn)) yes = false;
        if (EqOwnerVerdicts.size() >= 256) EqOwnerVerdicts.clear();
        EqOwnerVerdicts.emplace(cls, yes);
        LogInfo("weapons: %s %s an equipment owner",
                unit->get_class()->get_fname() ? WideToNarrow(unit->get_class()->get_fname()->to_string().c_str()).c_str() : "?",
                yes ? "IS" : "is not");
        return yes;
    }

    // EQUIPMENT ROUTE GUARD (eqguard1, 2026-10-05). UE4Minidump 21:10: an access violation in the
    // std::string key built from a GetEquipment entry - its Weapons array pointed into freed memory
    // (the log's last minute: dozens of "GetWeaponDataAsset returned null" with ids that were plainly
    // pointers and UTF-16 text). The route had crashed builds before (see kEquipmentWeapons). Now:
    // a sane-size check on the array, and SEH around the whole read - a fault turns the route off
    // for the session and the registry route answers instead. The game keeps running.
    bool EqFaulted = false, EqFaultLogged = false;

    // No C++ object lives in this frame, so SEH may guard it.
    static uint8_t ReadWeaponsViaEquipmentSafe(API::UObject* mech, TerrainWeapon* out) {
        __try {
            return Instance->ReadWeaponsViaEquipment(mech, out);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Instance->EqFaulted = true;
            return 0;
        }
    }

    uint8_t ReadWeaponsViaEquipment(API::UObject* mech, TerrainWeapon* out) {
        if (!kEquipmentWeapons || EqFaulted) return 0;

        // The owner, exactly as HUD_TargetWeaponLoadout.SetupLoadoutData picks it: a mech's
        // critical slot component, and for anything else - tank, VTOL, turret - the unit itself,
        // cast to MWEquipmentOwnerInterface. The game's target panel reads every kind of unit
        // this one way, and so do we now.
        API::UObject* owner = nullptr;
        UnitMemo* um = UmPeek(mech); // unitmemo1: the owner found once per unit (a component search every pass before)
        if (um && um->EqDone && !UmCheckRun(um))
            owner = um->EqOwner;
        else {
            owner = CompByClassName(mech, L"CriticalSlot");
            if (!owner && IsEquipmentOwner(mech)) owner = mech;
            if (um) {
                if (um->EqDone && um->EqOwner != owner)
                    UmLog(mech, "equipment owner", um->EqOwner ? "set" : "none", owner ? "set (other)" : "none");
                um->EqOwner = owner;
                um->EqDone = true;
            }
        }
        if (!owner || !EnsureEquipmentShape(owner)) return 0;

        NoArgFn* e = GetNoArgFn(owner, L"GetEquipment");
        if (!e) return 0;
        uint8_t* ret = CallNoArg(owner, L"GetEquipment");
        if (!ret) return 0;

        if (!EqEntryStruct) {
            API::UStruct* rs = StructOfProp(e->RetProp);
            API::FProperty* wp = nullptr;
            if (rs && PropOffset(rs, L"Weapons", &wp) >= 0 && wp)
                EqEntryStruct = StructOfProp(reinterpret_cast<API::FArrayProperty*>(wp)->get_inner());
        }

        FScriptArrayLayout arr{};
        std::memcpy(&arr, ret + EqWeaponsAt, sizeof(arr));
        if (!arr.Data || arr.Num <= 0) return 0;
        // A mech carries a few dozen entries at most; more (or a stride past any real entry) is not
        // a weapon list - leave it to the registry route (eqguard1).
        if (arr.Num > 96 || arr.Max < arr.Num || EqEntryStride <= 0 || EqEntryStride > 1024 ||
            EqIdAt < 0 || EqIdAt + EqIdSize > EqEntryStride) {
            LogDistinctOnce("weapons: GetEquipment returned an implausible Weapons array (num %d max %d) - registry route used",
                            arr.Num, arr.Max);
            return 0;
        }

        uint8_t n = 0;
        for (int32_t i = 0; i < arr.Num && n < (uint8_t)kRawWeapons; ++i) {
            auto* entry = reinterpret_cast<uint8_t*>(arr.Data) + (size_t)i * EqEntryStride;

            TerrainWeapon w{};
            w.Part = TMP_UNKNOWN;
            // id -> name is fixed for the whole game: resolve each id once, then read the cache.
            const std::string key(reinterpret_cast<const char*>(entry + EqIdAt), (size_t)EqIdSize);
            auto known = PodIdNames.find(key);
            if (known != PodIdNames.end()) {
                strncpy_s(w.Name, known->second.c_str(), _TRUNCATE);
            } else {
                // On the data asset CLASS, not on the equipment owner - see WeaponDataAssetCdo().
                API::UObject* host = WeaponDataAssetCdo();
                const int32_t availInEntry = EqEntryStride - EqIdAt;
                API::UObject* wda = host ? CallArgObject(host, L"GetWeaponDataAsset",
                                                         entry + EqIdAt, availInEntry) : nullptr;
                if (!wda) {
                    uint64_t raw = 0;
                    std::memcpy(&raw, entry + EqIdAt, sizeof(raw) < (size_t)EqIdSize
                                                      ? sizeof(raw) : (size_t)EqIdSize);
                    LogDistinctOnce("weapons: GetWeaponDataAsset returned null for id +%d size %d "
                                    "(first 8 bytes %016llx) - entry skipped",
                                    EqIdAt, EqIdSize, (unsigned long long)raw);
                    continue; // one empty entry (a vehicle's list is all empty) is not the whole unit
                }
                if (!GetNoArgFn(wda, L"GetShortDisplayName")) return 0;
                uint8_t* sn = CallNoArg(wda, L"GetShortDisplayName");
                if (!sn) continue;
                TextToString(sn, w.Name, sizeof(w.Name));
                if (PodIdNames.size() >= 512) { PodIdNames.clear(); PodIdMelee.clear(); }
                PodIdNames.emplace(key, w.Name);
                if (w.Name[0] && IsMeleeWeapon(wda, w.Name)) {
                    PodIdMelee.insert(key);
                    LogDistinctOnce("weapons: '%s' is melee - hidden", w.Name);
                }
            }
            if (!w.Name[0] || PodIdMelee.count(key)) continue;
            if (IsAmsName(w.Name)) continue; // not a threat
            if (EqHealthAt >= 0 && EqHealthAt < EqEntryStride)
                LogDistinctOnce("weapons: '%s' HealthState %u", w.Name, (unsigned)entry[EqHealthAt]);
            w.Part = EquipmentPart(EqEntryStruct, entry);
            LogDistinctOnce("weapons: equipment '%s' part %u", w.Name, w.Part);
            out[n++] = w;
        }
        return n;
    }

    // ---- short weapon names, the way MWVR gets them -----------------------------------------
    //
    // Read off MWVR's own graph rather than guessed: VR_HUD_WeaponLoadout calls
    // GetHudWeaponList on MWMech - the MECH, not the weapon system component - and the entries
    // that come back carry Weapon_ShortName as FText. MWVR never resolves a TWeakObjectPtr
    // itself; the engine fills the entries and hands over components whose lifetime is already
    // settled. That is precisely why its version does not crash and my first attempt did.
    //
    // Called ONCE PER MECH PER MISSION, behind the identity cache. Weapon names cannot change
    // during a fight - only which of them are still attached - so the live mount and destroyed
    // state keep coming from WeaponRegistry each scan. The call also allocates an array the
    // engine would normally reclaim, so doing it once rather than four times a second is the
    // difference between a rounding error and a leak.
    API::UFunction* HudListFn = nullptr;
    int32_t HwlRet = -1, HwlStride = 0, HweShort = -1, HweName = -1, HweWeapon = -1;
    bool    HudListChecked = false;
    std::vector<uint8_t> HwlBuf;

    bool EnsureHudList(API::UObject* mech) {
        if (HudListChecked) return HudListFn != nullptr;
        HudListChecked = true;
        for (API::UStruct* st = mech->get_class(); st && !HudListFn; st = st->get_super_struct())
            HudListFn = static_cast<API::UClass*>(st)->find_function(L"GetHudWeaponList");
        if (!HudListFn) { LogError("weapons: GetHudWeaponList not on this mech"); return false; }

        API::FProperty* ret = nullptr;
        for (auto* f = HudListFn->get_child_properties(); f; f = f->get_next()) {
            auto* pr = reinterpret_cast<API::FProperty*>(f);
            if (pr->get_fname() && pr->get_fname()->to_string() == L"ReturnValue") {
                HwlRet = pr->get_offset(); ret = pr;
            }
        }
        const int32_t frame = HudListFn->get_properties_size();
        if (frame <= 0 || HwlRet < 0 || !ret || PropTypeOf(ret) != L"ArrayProperty") {
            LogError("weapons: GetHudWeaponList layout incomplete (frame %d, ret %d)", frame, HwlRet);
            HudListFn = nullptr; return false;
        }
        auto* inner = reinterpret_cast<API::FArrayProperty*>(ret)->get_inner();
        API::UStruct* es = StructOfProp(inner);
        if (!es) { LogError("weapons: hud entry is not a struct"); HudListFn = nullptr; return false; }
        const int32_t sz = es->get_properties_size(), al = es->get_min_alignment();
        HwlStride = (al > 0) ? ((sz + al - 1) / al) * al : sz;
        // Name and ShortName, NOT Weapon_Name / Weapon_ShortName.
        //
        // The "Weapon_" prefixes came from MWVR's asset, where a break-struct node names its
        // pins after the struct VARIABLE - so a pin reads Weapon_ShortName while the member is
        // just ShortName. I took pin names for member names, and the log said so at once:
        // "short +-1 name +-1" against a struct that plainly has both.
        HweShort  = PropOffset(es, L"ShortName");
        HweName   = PropOffset(es, L"Name");
        HweWeapon = PropOffset(es, L"Weapon");
        if (HweShort < 0) HweShort = PropOffset(es, L"Weapon_ShortName");
        if (HweName  < 0) HweName  = PropOffset(es, L"Weapon_Name");
        HwlBuf.assign(static_cast<size_t>(frame) + 64, 0);
        LogStructMembers("MechHudWeaponEntry", es);
        LogInfo("weapons: hud list ret +%d stride %d short +%d name +%d weapon +%d",
                HwlRet, HwlStride, HweShort, HweName, HweWeapon);
        return true;
    }

    // Fills `out` with up to kHudMax short names, and `weapons` with each entry's weapon
    // component (for pairing by object - see ReadWeapons); returns how many.
    uint8_t HudWeaponNames(API::UObject* mech, char out[kHudMax][20], void* weapons[kHudMax]) {
        if (!EnsureHudList(mech) || HweShort < 0) return 0;
        // Not zeroed: the previous returned array is freed by the engine's assignment.
        mech->process_event(HudListFn, HwlBuf.data());
        FScriptArrayLayout arr{};
        std::memcpy(&arr, HwlBuf.data() + HwlRet, sizeof(arr));
        if (!arr.Data || arr.Num <= 0 || HwlStride <= 0) return 0;
        uint8_t n = 0;
        for (int32_t i = 0; i < arr.Num && n < (uint8_t)kHudMax; ++i) {
            const uint8_t* e = reinterpret_cast<const uint8_t*>(arr.Data) + (size_t)i * HwlStride;
            char buf[20] = "";
            if (!TextToString(e + HweShort, buf, sizeof(buf)) && HweName >= 0)
                TextToString(e + HweName, buf, sizeof(buf));
            void* wp = nullptr;
            if (HweWeapon >= 0)
                std::memcpy(&wp, e + HweWeapon, sizeof(wp));
            weapons[n] = wp;
            std::memcpy(out[n++], buf, sizeof(buf));
        }
        return n;
    }

    // ---- per-actor identity cache -----------------------------------------------------------
    // Chassis, variant, tonnage, weight class and weapon mounts cannot change while a mech is
    // alive, so they are read once per actor and kept. Only armour, structure and heat are
    // re-read, which is what keeps this affordable at two dozen mechs.
    struct MechIdent {
        char    Chassis[32];
        char    Variant[32];
        char    WeaponNames[kHudMax][20];
        void*   HudWeapons[kHudMax]; // compared only, never dereferenced
        uint8_t NameCount;
        float   Tons;
        uint8_t WeightClass;
        uint8_t WeaponCount;
        TerrainWeapon Weapons[TERR_MAX_WEAPONS];
    };
    std::unordered_map<void*, MechIdent> IdentCache;

    // Map the game's hardpoint-component enum onto contract part order. The enum's own byte
    // values are NOT assumed: the name is read through the enum property and matched, because a
    // byte value is exactly the kind of thing a patch renumbers silently.
    static uint8_t PartFromName(const char* n) {
        if (!n || !n[0]) return TMP_UNKNOWN;
        const bool L = (std::strstr(n, "Left") != nullptr) || (std::strstr(n, "_L") != nullptr);
        const bool R = (std::strstr(n, "Right") != nullptr) || (std::strstr(n, "_R") != nullptr);
        if (std::strstr(n, "Head"))  return TMP_Head;
        if (std::strstr(n, "Arm"))   return L ? (uint8_t)TMP_LeftArm : (R ? (uint8_t)TMP_RightArm : TMP_UNKNOWN);
        if (std::strstr(n, "Leg"))   return L ? (uint8_t)TMP_LeftLeg : (R ? (uint8_t)TMP_RightLeg : TMP_UNKNOWN);
        if (std::strstr(n, "Torso")) {
            if (L) return TMP_LeftTorso;
            if (R) return TMP_RightTorso;
            return TMP_CentreTorso;
        }
        return TMP_UNKNOWN;
    }

    // Read the static half of a mech once: one GetMechData call, then pure memory reads.
    const MechIdent* IdentOf(API::UObject* mech) {
        auto it = IdentCache.find(static_cast<void*>(mech));
        if (it != IdentCache.end()) return &it->second;
        if (IdentCache.size() >= 256) IdentCache.clear();

        // DELIBERATELY NOT GATED on MechData resolving. The first build returned early here when
        // GetMechData was not found, and threw away the chassis, the variant and the weight class
        // - three reads that were already working and have nothing to do with MechData. Every
        // field degrades on its own or the contract's Have* flags are a lie.
        const bool haveMd = EnsureMechDataFn(mech);

        MechIdent id{};
        id.Tons = 0.0f;
        id.WeightClass = 0xFF;

        // Reach MechData whichever way resolved: the struct member on the asset (no call), or
        // the accessor on it.
        const uint8_t* md = nullptr;
        if (haveMd) {
            if (MdaDataOff >= 0) {
                if (API::UObject* mda = MdaOf(mech))
                    md = reinterpret_cast<const uint8_t*>(mda) + MdaDataOff;
            } else if (GetMechDataFn) {
                API::UObject* mda = MdaOf(mech);
                if (mda) {
                    // Not zeroed: the previous struct's strings/arrays are freed on assignment.
                    mda->process_event(GetMechDataFn, MechDataBuf.data());
                    md = MechDataBuf.data() + MechDataRet;
                }
            }
        }

        // CHASSIS AND VARIANT COME FROM THE PROVEN ROUTE, NOT FROM MechData.
        //
        // ChassisFullName and VariantName are TextProperty - FText, which is a ref-counted
        // handle to a text object, NOT an FString or an FName. Reading one as either gives you
        // whatever the pointer happens to address, and that is exactly what shipped: the label
        // showed '???', three bytes of unrelated memory narrowed into question marks. It looked
        // like a failed lookup when it was actually a successful read of the wrong thing.
        //
        // Decoding FText by hand means walking an internal layout that varies between engine
        // builds, for no gain: ChassisOf and VariantOf already produce "viper" and "VPR-M" from
        // the target component's class and the data asset's name, and have done since before
        // this work started. So those are used, full stop, and the FText members are left alone.
        // The game's own display strings first - they are what MWVR shows and what the pilot
        // expects. The asset-name parse stays as the fallback for anything that does not
        // resolve, since it is proven on stock chassis.
        id.NameCount = HudWeaponNames(mech, id.WeaponNames, id.HudWeapons);

        if (md && MdVariantName >= 0) TextToString(md + MdVariantName, id.Variant, sizeof(id.Variant));
        if (md && MdChassisFull >= 0) TextToString(md + MdChassisFull, id.Chassis, sizeof(id.Chassis));
        if (!id.Chassis[0]) ChassisOf(mech, id.Chassis);
        if (!id.Variant[0]) VariantOf(mech, id.Variant);

        if (md && MdStats >= 0 && StTons >= 0) {
            if (StTonsIsInt) {
                int32_t t = 0;
                std::memcpy(&t, md + MdStats + StTons, sizeof(t));
                id.Tons = (float)t;
            } else {
                std::memcpy(&id.Tons, md + MdStats + StTons, sizeof(float));
            }
        }

        if (const auto* p = mech->get_property_data<uint8_t>(L"ComputedWeightClass"))
            if (*p <= 3) id.WeightClass = *p;

        // ComputedWeightClass reported Light for a 65-tonne Catapult, so it is not trustworthy
        // on its own - it is presumably only filled in for the player's own mech. Tonnage IS
        // reliable now, and the BattleTech class boundaries are fixed, so derive from it and
        // keep the read value only when there is no tonnage to work from.
        if (id.Tons >= 20.0f)
            id.WeightClass = (id.Tons <= 35.0f) ? 0 : (id.Tons <= 55.0f) ? 1
                           : (id.Tons <= 75.0f) ? 2 : 3;

        // Weapons, with the body part each is mounted in - the thing that makes the colour rule
        // on the label mean anything.
        if (md && MdHardPoints >= 0 && MdHpStride > 0 && HpWeapon >= 0) {
            FScriptArrayLayout arr{};
            std::memcpy(&arr, md + MdHardPoints, sizeof(arr));
            const int32_t n = (arr.Data && arr.Num > 0) ? arr.Num : 0;
            for (int32_t i = 0; i < n && id.WeaponCount < TERR_MAX_WEAPONS; ++i) {
                const uint8_t* e = reinterpret_cast<const uint8_t*>(arr.Data) + (size_t)i * MdHpStride;
                TerrainWeapon w{};
                w.Part = TMP_UNKNOWN;
                ReadFNameAt(e + HpWeapon, w.Name, sizeof(w.Name));
                if (!w.Name[0]) ReadFString(e + HpWeapon, w.Name, sizeof(w.Name));
                if (!w.Name[0]) continue; // a blank hardpoint, which every variant has some of
                if (HpPart >= 0) {
                    char pn[32];
                    ReadFNameAt(e + HpPart, pn, sizeof(pn));
                    w.Part = PartFromName(pn);
                }
                id.Weapons[id.WeaponCount++] = w;
            }
        }

        auto ins = IdentCache.emplace(static_cast<void*>(mech), id);
        return &ins.first->second;
    }


    // ==========================================================================================
    // THE TARGET COMPONENT - what the game itself knows about a contact
    //
    // Every targetable unit owns an MWTargetComponent, and three of the things this exporter was
    // deriving by hand are simply properties on it:
    //
    //   PaperDollClass  the widget class the game would draw for this unit. MWVR's target
    //                   readout class-casts this against nine known paperdoll classes and never
    //                   looks at the actor's class at all. It is the game's own answer to "what
    //                   kind of thing is this", so it is right for modded units for free.
    //   TargetName      the display name, as FText. This is what shows "Manticore Heavy Tank"
    //                   for a tank whose blueprint a mod author called "Manticoreconvoyfast".
    //   SetupTexture    the paperdoll Texture2D. Following this pointer is how the game draws
    //                   the correct art without matching any names.
    //
    // I built name-parsing for all three before reading this. Every one of those parses was a
    // guess at something the component states outright.
    // ==========================================================================================

    API::UClass* TargetCompClass = nullptr;
    int32_t TcPaperDollClass = -1, TcTargetName = -1;
    bool    TcResolved = false, TcLogged = false;

    // The unit's MWTargetComponent.
    //
    // MATCHED ON THE PROPERTY NAME, NOT THE CLASS NAME. A vehicle's is TankTargetComponent_C and
    // a turret's is TurretTargetComponent_C, so matching the class on "TargetComponent" looked
    // right - and found nothing at all on a mech, because a MECH's target component is a
    // PER-CHASSIS blueprint called "viper_Target_C". That is why the first run came back with an
    // art key on 0 of 3 units while the tank resolved fine.
    //
    // The property is called MechTargetComponent on a mech and TargetComponent elsewhere, so the
    // property name is the stable part. Two near-misses have to be excluded by hand:
    // TargetTracking (MWTargetTrackingComponent) and AIUnitTargetingDataComponent both contain
    // "Target" and are not this.
    API::UObject* TargetCompOf(API::UObject* actor) {
        // "TargetComponent" matches MechTargetComponent, TankTargetComponent and the per-chassis
        // viper_Target_C through its parent chain. MWTargetTrackingComponent and
        // AIUnitTargetingDataComponent are the two near-misses and are excluded by name.
        API::UObject* hit = nullptr;
        ForEachLiveComponent(actor, [&](API::UObject* c) {
            // Verdict per class (see ClassHas): the same chain walk, done once per class.
            void* cls = c->get_class();
            if (!cls) return true;
            if (TargetVerdicts.size() >= 4096) TargetVerdicts.clear();
            auto it = TargetVerdicts.find(cls);
            if (it == TargetVerdicts.end()) {
                uint8_t v = 0;
                for (API::UStruct* st = c->get_class(); st; st = st->get_super_struct()) {
                    if (!st->get_fname()) break;
                    const std::wstring cn = st->get_fname()->to_string();
                    if (NameHas(cn, L"Tracking") || NameHas(cn, L"Targeting")) break;
                    if (NameHas(cn, L"TargetComponent")) { v = 1; break; }
                }
                it = TargetVerdicts.emplace(cls, v).first;
            }
            if (it->second) { hit = c; return false; }
            return true;
        });
        return hit;
    }

    // The paperdoll texture a target component points at, found by TYPE rather than by name.
    //
    // There is no single "SetupTexture" member to read: the blueprint keeps its textures in
    // whatever variables its author made - the Manticore's component has "Body" and "Side". So
    // this walks the component's object properties and takes the first one that is a Texture2D
    // named like a paperdoll, which is what every one of them is called.
    //
    // THE ONE SCAN THAT SURVIVES, AND WHY IT IS BOUNDED. A texture is not a component, so
    // GetComponentsByClass cannot answer this one - it is the only place left that reads object
    // properties directly. Two things keep it safe where the others were not. It walks ONLY the
    // component's own class, not the inherited chain, so it sees the handful of variables the
    // blueprint author declared (the Manticore's are "Body" and "Side") rather than hundreds of
    // engine fields. And those variables are asset references fixed at author time, so they are
    // either null or point at a loaded asset - unlike an actor's property table, which holds
    // runtime pointers to things that can already be dead.
    static API::UObject* PaperdollTextureOn(API::UObject* tc) {
        if (!tc || !tc->get_class()) return nullptr;
        API::UObject* hit = nullptr;
        for (API::FField* f = tc->get_class()->get_child_properties(); f && !hit; f = f->get_next()) {
            auto* p = reinterpret_cast<API::FProperty*>(f);
            if (!p->get_fname() || PropTypeOf(p) != L"ObjectProperty") continue;
            auto** slot = reinterpret_cast<API::UObject**>(
                reinterpret_cast<uint8_t*>(tc) + p->get_offset());
            auto* o = slot ? *slot : nullptr;
            if (!o || !o->get_fname() || !o->get_class() || !o->get_class()->get_fname()) continue;
            if (!NameHas(o->get_class()->get_fname()->to_string(), L"Texture")) continue;
            if (NameHas(o->get_fname()->to_string(), L"aperdoll")) hit = o;
        }
        return hit;
    }

    // Offsets are resolved once off the first component seen. Every target component derives from
    // the same native MWTargetComponent, so the three offsets transfer - the same reasoning that
    // lets the condition offsets be resolved on the player's mech and used on everyone else's.
    void EnsureTargetComp(API::UObject* tc) {
        if (TcResolved || !tc || !tc->get_class()) return;
        TcResolved = true;
        API::UStruct* st = tc->get_class();
        // Named PaperDoll, a ClassProperty - "PaperDollClass" was my guess and does not exist.
        TcPaperDollClass = PropOffset(st, L"PaperDoll");
        TcTargetName     = PropOffset(st, L"TargetName");
        LogInfo("target: %s PaperDoll +%d TargetName +%d",
                WideToNarrow(tc->get_class()->get_fname()->to_string().c_str()).c_str(),
                TcPaperDollClass, TcTargetName);
        if (TcPaperDollClass < 0 || TcTargetName < 0)
            LogStructMembers("MWTargetComponent", st);
    }

    // The nine classes, in contract order. Matched on NAME rather than on a resolved UClass
    // pointer because these are blueprint classes: resolving nine handles up front means nine
    // more things that can fail to resolve, and the names are stable - they are the asset names
    // the paks ship and that MWVR itself casts against.
    static uint8_t KindFromDollClassName(const std::wstring& n) {
        struct Row { const wchar_t* key; uint8_t kind; };
        // ORDER MATTERS: VTOLDualEngine must be tested before VTOL, because the shorter name is
        // a substring of the longer one and would swallow it.
        static const Row rows[] = {
            { L"VTOLDualEngine", TUK_VtolDualEngine },
            { L"SatelliteDish",  TUK_SatelliteDish  },
            { L"RadarTower",     TUK_RadarTower     },
            { L"Artillery",      TUK_Artillery      },
            { L"Generator",      TUK_Generator      },
            { L"Turret",         TUK_Turret         },
            { L"Tank",           TUK_Tank           },
            { L"VTOL",           TUK_Vtol           },
            { L"Mech",           TUK_Mech           },
        };
        for (const Row& r : rows)
            if (NameHas(n, r.key)) return r.kind;
        return 0xFF;
    }

    // "Paperdoll_Manticore_Body_Side" -> "manticore": the extractor's folder key, which is named
    // from the same asset path. Strips the Paperdoll_ prefix and the view suffix, and drops the
    // characters a folder name cannot carry.
    static void ArtKeyFromTextureName(const std::wstring& tex, char out[32]) {
        out[0] = '\0';
        std::wstring n = tex;
        for (auto& c : n) c = (wchar_t)towlower(c);
        if (n.rfind(L"paperdoll_", 0) == 0) n = n.substr(10);
        static const wchar_t* suf[] = { L"_body_side", L"_body_top", L"_side_uix", L"_top_uix",
                                        L"_front_uix", L"_back_uix", L"_side", L"_top",
                                        L"_front", L"_back" };
        for (const wchar_t* sx : suf) {
            const size_t L = wcslen(sx);
            if (n.size() > L && n.compare(n.size() - L, L, sx) == 0) { n.resize(n.size() - L); break; }
        }
        size_t k = 0;
        for (size_t i = 0; i < n.size() && k + 1 < 32; ++i) {
            const wchar_t c = n[i];
            if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_')
                out[k++] = (char)c;
        }
        out[k] = '\0';
    }

    // THE CHASSIS KEY, TAKEN FROM THE TEXTURE THE GAME IS ACTUALLY USING.
    //
    // Read out of the game's own files rather than inferred. A mech's per-chassis target
    // component - Catapult_Target - holds all 22 paperdoll masks as NAMED Texture2D properties:
    //
    //     CenterTorsoArmor     -> catapult_ct_armor
    //     CenterTorsoInternal  -> catapult_ct_internal
    //     HeadArmor            -> catapult_head_armor
    //     RearCenterTorsoArmor -> catapult_rct_armor
    //     RearRIghtTorsoArmor  -> catapult_rrt_armor     (the capital I is the GAME'S typo)
    //
    // So the chassis name is sitting inside the texture's own object name, and the extractor's
    // folders are named from the same asset paths - which is why "catapult/ct_armor.png" lines
    // up exactly.
    //
    // This beats deriving the key from the component's CLASS name, which is what I did first. A
    // mod whose blueprint is called YAGDM_Clint_Target_C but which points at the stock
    // clint_ct_armor textures resolves correctly here and would have missed there. The class name
    // describes the blueprint; the texture describes the art, and the art is what we are keying.
    static const wchar_t* MechMaskProp(int i) {
        static const wchar_t* p[] = { L"CenterTorsoArmor", L"HeadArmor", L"LeftTorsoArmor",
                                      L"RightTorsoArmor", L"LeftArmArmor", L"RightArmArmor",
                                      L"CenterTorsoInternal", L"HeadInternal" };
        return (i >= 0 && i < 8) ? p[i] : nullptr;
    }

    // The texture's PACKAGE PATH names the chassis - not the texture's own name.
    //
    // Measured, not assumed. Deriving the chassis by stripping the part and layer off the asset
    // name ("catapult_ct_armor" -> "catapult") matched only 97 of the extracted folders and
    // missed 150, because the part is not always one token: Devastator_Arm_Left_Armor reduces to
    // "devastator_arm", and Anubis_F_CT_Internals to "anubis_f". Taking the FOLDER the texture
    // lives in instead matched 167 and missed 1.
    //
    // It matches because it is the same rule the extractor uses to name its folders: the nearest
    // enclosing directory that is not a Target or Textures wrapper. Both sides now derive the key
    // from the same thing, so they cannot drift apart.
    //
    // (The one miss is YAGM's shared _Common/Targets/Assault_Target - generic per-weight-class
    // art rather than per-chassis, so there is correctly no chassis folder for it.)
    static void ChassisFromTexture(API::UObject* tex, char out[32]) {
        out[0] = '\0';
        if (!tex) return;
        API::UObject* pkg = tex->get_outer();
        while (pkg && pkg->get_outer() && pkg->get_outer() != pkg) pkg = pkg->get_outer();
        if (!pkg || !pkg->get_fname()) return;

        std::wstring path = pkg->get_fname()->to_string();   // /Game/.../Catapult/Target/Textures/catapult_ct_armor
        std::vector<std::wstring> seg;
        size_t i = 0;
        while (i < path.size()) {
            size_t j = path.find(L'/', i);
            if (j == std::wstring::npos) j = path.size();
            if (j > i) seg.push_back(path.substr(i, j - i));
            i = j + 1;
        }
        if (seg.size() < 2) return;
        seg.pop_back();                                      // the asset name itself
        static const wchar_t* kWrap[] = { L"textures", L"texture", L"hdtextures", L"hdtexture",
                                          L"target", L"targets", L"materials" };
        auto isWrap = [&](const std::wstring& w) {
            std::wstring l = w;
            for (auto& c : l) c = (wchar_t)towlower(c);
            for (const wchar_t* k : kWrap) if (l == k) return true;
            return false;
        };
        while (!seg.empty() && isWrap(seg.back())) seg.pop_back();
        if (seg.empty()) return;

        // A SHARED FOLDER NAMES NOTHING. The generic turret art lives in Objects/Turrets/_common,
        // so the path rule yields "_common" - which is not a unit and matches no extracted folder.
        // The log showed exactly that: art key '_common' from TurretTargetComponent_C. Leaving
        // the key empty sends the consumer to the kind's generic silhouette, which for a turret
        // is that same art arriving by the correct route.
        if (!seg.back().empty() && seg.back()[0] == L'_') return;

        std::wstring cn = seg.back();
        for (auto& c : cn) c = (wchar_t)towlower(c);
        size_t k = 0;
        for (size_t x = 0; x < cn.size() && k + 1 < 32; ++x) {
            const wchar_t c = cn[x];
            if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_')
                out[k++] = (char)c;
        }
        out[k] = '\0';
    }

    // The first of the known mask properties that resolves to a texture.
    void MechArtKeyFrom(API::UObject* tc, char artKey[32]) {
        if (!tc || !tc->get_class()) return;
        for (int i = 0; MechMaskProp(i); ++i) {
            const int32_t off = PropOffset(tc->get_class(), MechMaskProp(i));
            if (off < 0) continue;
            auto* tex = *reinterpret_cast<API::UObject**>(reinterpret_cast<uint8_t*>(tc) + off);
            if (!tex || !tex->get_fname()) continue;
            ChassisFromTexture(tex, artKey);
            if (artKey[0]) return;
        }
    }

    // THE LAST GOOD ART KEY, PER UNIT. The art key and kind are re-read from the target component
    // every scan, and one read can come back empty - the log caught "target: NONE no target
    // component on DerivedMech_C" once, and a Stalker's resolved doll reverted to the generic
    // rectangles in front of the player. A unit's art cannot change while it lives, so an empty
    // read keeps what the unit last resolved. Keyed on the actor AND its class, so an actor
    // address reused by a different unit never inherits someone else's art. Each lapse is
    // logged (capped) with what the lookup saw, so the cause can be pinned down.
    struct ArtMemo { void* Cls; char Key[32]; uint8_t Kind; };
    std::unordered_map<void*, ArtMemo> LastArt;
    int ArtLapses = 0;

    void KeepArt(API::UObject* actor, uint8_t& kind, char artKey[32]) {
        if (!actor) return;
        void* cls = actor->get_class();
        auto it = LastArt.find(actor);
        if (artKey[0]) {
            if (LastArt.size() >= 512) LastArt.clear();
            ArtMemo& m = LastArt[actor];
            m.Cls = cls;
            std::memcpy(m.Key, artKey, 32);
            m.Kind = kind;
            return;
        }
        if (it == LastArt.end() || it->second.Cls != cls || !it->second.Key[0]) return;
        std::memcpy(artKey, it->second.Key, 32);
        kind = it->second.Kind;
        if (ArtLapses++ < 20) {
            API::UObject* tc = TargetCompOf(actor);
            int comps = 0;
            ForEachLiveComponent(actor, [&](API::UObject*) { ++comps; return true; });
            LogInfo("target: art key lapsed on %s (kept '%s') - target component %s, %d component(s) listed",
                    actor->get_class() && actor->get_class()->get_fname()
                        ? WideToNarrow(actor->get_class()->get_fname()->to_string().c_str()).c_str() : "?",
                    artKey, tc ? (tc->get_class() && tc->get_class()->get_fname()
                                      ? WideToNarrow(tc->get_class()->get_fname()->to_string().c_str()).c_str() : "?")
                               : "NOT FOUND",
                    comps);
        }
    }

    // Fills kind, display name and art key from the unit's target component. Each of the three is
    // independent: a component that yields only a texture still improves the art.
    void ReadTargetComp(API::UObject* actor, uint8_t& kind, char name[32], char artKey[32]) {
        if (!kTargetComponent) return;
        API::UObject* tc = TargetCompOf(actor);
        // THE SILENT CASE, NOW LOGGED. The OK/MISS line below only runs once a component is in
        // hand, so a unit whose target component was never FOUND left no trace at all - its art
        // key just stayed empty. That is the one failure the Black Knight can still be, having
        // ruled out a bad key, missing art and a stale ghost.
        if (!tc) {
            if (actor && actor->get_class() && actor->get_class()->get_fname())
                LogDistinctOnce("target: NONE no target component on %s",
                    WideToNarrow(actor->get_class()->get_fname()->to_string().c_str()).c_str());
            return;
        }
        EnsureTargetComp(tc);
        auto* base = reinterpret_cast<uint8_t*>(tc);

        if (TcPaperDollClass >= 0) {
            auto* dc = *reinterpret_cast<API::UObject**>(base + TcPaperDollClass);
            if (dc && dc->get_fname()) {
                const std::wstring dn = dc->get_fname()->to_string();
                const uint8_t k = KindFromDollClassName(dn);
                if (k != 0xFF) kind = k;
                LogDistinctOnce("target: paperdoll class '%s' -> kind %u",
                                WideToNarrow(dn.c_str()).c_str(), (unsigned)kind);
            }
        }
        if (TcTargetName >= 0 && name) {
            char buf[32] = "";
            if (TextToString(base + TcTargetName, buf, sizeof(buf)) && buf[0])
                std::memcpy(name, buf, sizeof(buf));
        }
        // WHICH SOURCE NAMES THE ART, AND IN WHICH ORDER. The two cases are genuinely different
        // and the order matters more than it looks.
        //
        // A MECH's target component is PER CHASSIS - "viper_Target_C", "mistlynx_Target_C" - so
        // the class name IS the art key, and it comes from the asset path, which is what the
        // extractor's folders are named from. That is the one source that solves multi-word
        // chassis: the folder is "mistlynx" while ChassisFullName is the display string
        // "Mist Lynx", with a space, and no amount of normalising one reliably yields the other.
        //
        // A VEHICLE's is SHARED - every tank has TankTargetComponent_C - so its class name names
        // nothing and only the texture reference identifies the unit.
        //
        // Taking the texture first would break the mech case: a per-part mask called
        // Paperdoll_Viper_CT_Armor reduces to "viper_ct_armor", which is not a folder, and the
        // mech silently loses its doll - the exact symptom that started this. So a per-chassis
        // component wins, and the texture is for the shared ones.
        // 1. the mask textures themselves - the game's own answer
        if (artKey && !artKey[0]) MechArtKeyFrom(tc, artKey);

        // 2. the per-chassis component class name, when the masks are not named properties
        if (artKey && !artKey[0] && tc->get_class() && tc->get_class()->get_fname()) {
            const std::wstring raw = tc->get_class()->get_fname()->to_string();
            const size_t cut = raw.find(L"_Target");
            const bool perChassis = (cut != std::wstring::npos) && !NameHas(raw, L"TargetComponent");
            if (perChassis) {
                std::wstring cn = raw.substr(0, cut);
                for (auto& c : cn) c = (wchar_t)towlower(c);
                size_t k = 0;
                for (size_t i = 0; i < cn.size() && k + 1 < 32; ++i) {
                    const wchar_t c = cn[i];
                    if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_')
                        artKey[k++] = (char)c;
                }
                artKey[k] = '\0';
            }
        }
        // 3. a shared component (TankTargetComponent_C) names nothing, so the texture it points
        //    at is the only identification - same path rule, same reason.
        if (artKey && !artKey[0]) {
            if (API::UObject* tex = PaperdollTextureOn(tc)) {
                ChassisFromTexture(tex, artKey);
                if (!artKey[0]) ArtKeyFromTextureName(tex->get_fname()->to_string(), artKey);
            }
        }
        // Reported WITH the kind, and reported on FAILURE too. An art key that resolves is only
        // half the story: the consumer picks the mech doll or a one-body silhouette by Kind, so a
        // mech misclassified as a vehicle draws a generic tank no matter how good its key is.
        // The Black Knight came back with a correct key and the wrong picture, and this log could
        // not tell the two apart - it only printed successes.
        LogDistinctOnce("target: %s art key '%s' kind %u from %s",
                        artKey[0] ? "OK  " : "MISS",
                        artKey[0] ? artKey : "-", (unsigned)kind,
                        WideToNarrow(tc->get_class()->get_fname()->to_string().c_str()).c_str());
    }

    // ---- vehicles: tanks, turrets and VTOLs -------------------------------------------------
    // MWVR's own paperdolls describe these completely, and they are far simpler than a mech.
    // VR_TankPaperdoll / VR_TurretPaperDoll / VR_VTOLPaperDoll each cast the target to MWTank /
    // MWTurret / MWVTOL, find one MWSimpleLivingActorComponent carrying a single
    // MWHealthComponent, and drive ONE widget called "Body" from GetHealthAsPercentage().
    //
    // So there is no armour layer, no per-part breakdown and no eight-part doll: a vehicle has a
    // body and an amount of health left. Publishing anything more detailed would be inventing it.
    //
    // The health numbers are read as HitPoints / MaxHitPoints rather than by calling
    // GetHealthAsPercentage, because those are the same two fields already proven on the mech
    // containers (MWArmoredHealthComponent extends MWHealthComponent) and a memory read cannot
    // run script on the game thread the way a process_event can.

    // A component on `actor` whose CLASS name contains `want`, searched one level deep so a
    // health component owned by MWSimpleLivingActorComponent is still found.
    // Kept as a thin alias so the vehicle code reads the same, but it no longer searches the
    // property table and no longer recurses: a health component owned by a living-actor component
    // is registered to the ACTOR, so the engine's flat list already contains it.
    API::UObject* FindCompByClassName(API::UObject* actor, const wchar_t* want, int = 1) {
        return CompByClassName(actor, want);
    }

    // "BP_Demolisher_C" -> "demolisher". The paperdoll folders are named after the vehicle, and
    // the consumer lowercases before looking one up, so only the decoration is stripped here.
    static void VehicleName(API::UObject* actor, char out[32]) {
        out[0] = '\0';
        if (!actor || !actor->get_class() || !actor->get_class()->get_fname()) return;
        std::wstring n = actor->get_class()->get_fname()->to_string();
        if (n.size() > 2 && n.compare(n.size() - 2, 2, L"_C") == 0) n.resize(n.size() - 2);
        if (n.rfind(L"BP_", 0) == 0) n = n.substr(3);
        while (!n.empty() && (n.back() == L'_' || (n.back() >= L'0' && n.back() <= L'9')))
            n.pop_back();
        size_t k = 0;
        for (; k + 1 < 32 && k < n.size(); ++k) out[k] = (n[k] < 128) ? (char)n[k] : '?';
        out[k] = '\0';
    }

    int32_t VehHpCur = -1, VehHpMax = -1;
    bool    VehChecked = false;

    // Fills one record for a tank / turret / VTOL. Returns false if it is not worth publishing.
    // The record's actor address - what the TacOps plates join lance members on (TerrainShared v13).
    static void SetActor(TerrainMechInfo& r, API::UObject* a) {
        const uint64_t p = (uint64_t)reinterpret_cast<uintptr_t>(a);
        r.ActorLo = (uint32_t)(p & 0xFFFFFFFFu);
        r.ActorHi = (uint32_t)(p >> 32);
    }

    bool ReadVehicle(API::UObject* v, uint8_t kind, uint8_t playerTeam, TerrainMechInfo& r) {
        std::memset(&r, 0, sizeof(r));
        r.Id      = AssignId(static_cast<void*>(v));
        SetActor(r, v);
        r.Valid   = 1;
        r.Kind    = kind;   // a seed - ReadTargetComp overrides it from PaperDollClass below
        r.HeatPct = -1.0f;
        r.WeightClass = 0xFF;
        for (int k = 0; k < TMP_COUNT; ++k) { r.Armour[k] = -1.0f; r.Structure[k] = -1.0f; }
        for (int k = 0; k < TRP_COUNT; ++k) { r.ArmourRear[k] = -1.0f; r.StructureRear[k] = -1.0f; }

        uint8_t team = 0xFF;
        if (const auto* p = TeamByte(v)) team = *p;
        r.Hostile = (team == playerTeam) ? 0 : 1;

        // The blueprint's class name is the LAST resort, not the first. It is the mod author's
        // asset name - "Manticoreconvoyfast" - and reading it is what put that on screen where
        // the game shows "Manticore Heavy Tank".
        Breadcrumb("vehicle: target component");
        Mp(MP_VRead);
        UnitMemo* um = UmTouch(v); // unitmemo1
        UmIdent(v, um, r.Kind, r.Chassis, r.ArtKey, true);
        if (kind == TUK_BattleArmor) {
            // ba4: a suit stays a suit whatever its doll class reads as; the game's own Elemental doll
            // (paperdolls/clanelemental) when its texture did not resolve, and a plain name.
            r.Kind = TUK_BattleArmor;
            if (!r.ArtKey[0]) strcpy_s(r.ArtKey, "clanelemental");
            if (!r.Chassis[0] || std::strstr(r.Chassis, "KelClan")) strcpy_s(r.Chassis, "Elemental");
        }
        Mp(MP_VName);

        // WHAT ACTUALLY RESOLVED, per distinct unit. The renderer finds art at
        // paperdolls/<name>/body.png, lowercased, so the name derived here IS the lookup key
        // and a mismatch shows up as a generic silhouette rather than as an error. Log each
        // distinct one once, so a mission tells us which names actually occur instead of the
        // blueprint spellings I would otherwise be guessing at.
        {
            static char seen[24][32] = {};
            static int  nseen = 0;
            bool known = false;
            for (int i = 0; i < nseen; ++i)
                if (std::strcmp(seen[i], r.Chassis) == 0) { known = true; break; }
            if (!known && nseen < 24 && r.Chassis[0]) {
                strcpy_s(seen[nseen++], r.Chassis);
                static const char* kindName[10] = { "mech", "tank", "turret", "vtol",
                                                   "vtoldual", "artillery", "generator",
                                                   "radartower", "satellite", "battlearmor" };
                LogInfo("vehicles: kind %s name '%s' art '%s'%s",
                        (r.Kind < 10) ? kindName[r.Kind] : "?", r.Chassis,
                        r.ArtKey[0] ? r.ArtKey : "-",
                        r.ArtKey[0] ? "" : "  (falls back to the kind's generic)");
            }
        }

        API::UObject* hc = nullptr;
        if (um && um->HealthDone && !UmCheckRun(um))
            hc = um->Health;
        else {
            hc = FindCompByClassName(v, L"Health");
            if (um) {
                if (um->HealthDone && um->Health != hc) UmLog(v, "health component", um->Health ? "set" : "none", hc ? "set (other)" : "none");
                um->Health = hc;
                um->HealthDone = true;
            }
        }
        if (hc && hc->get_class()) {
            if (!VehChecked) {
                VehChecked = true;
                VehHpCur = PropOffset(hc->get_class(), L"HitPoints");
                VehHpMax = PropOffset(hc->get_class(), L"MaxHitPoints");
                LogInfo("vehicles: health %s cur +%d max +%d",
                        WideToNarrow(hc->get_class()->get_fname()->to_string().c_str()).c_str(),
                        VehHpCur, VehHpMax);
            }
            const float f = FracAt(hc, VehHpCur, VehHpMax);
            if (f >= 0.0f) {
                // One body, published in the centre-torso slot by contract.
                r.Structure[TMP_CentreTorso] = f;
                r.HaveCondition = 1;
            }
        }

        // Turrets and tanks do carry weapons, and the registry read is class-agnostic - it looks
        // for a component whose class name mentions WeaponSystem and walks the same struct. If a
        // given vehicle has none, the list simply comes back empty.
        Mp(MP_VHealth);
        // VEHICLE WEAPONS FROM THE UNIT MEMO (vwpnmemo1, user 2026-10-13 "build both and test"): GetEquipment and a
        // per-entry walk (with two once-only log lines formatted every time) ran for every vehicle every pass, 1.3-1.5 ms.
        // A vehicle's loadout does not change and only names + parts are published, so the list is kept in the unit's
        // memo and re-read every 16th pass (~4 s); the unitmemo check pass (every 64th) re-reads and logs a difference.
        // An empty list in the unit's first 8 passes is not kept (data may not be ready at spawn).
        if (um && um->WpnDone && UmPass - um->WpnPass < 16 && !UmCheckRun(um)) {
            r.WeaponCount = um->WpnN;
            std::memcpy(r.Weapons, um->Wpn, sizeof(TerrainWeapon) * (size_t)um->WpnN);
        } else {
            r.WeaponCount = ReadWeapons(v, r.Weapons, false);
            if (um) {
                const uint8_t n = r.WeaponCount;
                if (um->WpnDone && UmCheckRun(um) &&
                    (n != um->WpnN || std::memcmp(um->Wpn, r.Weapons, sizeof(TerrainWeapon) * (size_t)n) != 0))
                    UmLog(v, "vehicle weapons", "kept list", "a different list");
                if (n > 0 || um->Uses > 8) {
                    um->WpnN = n;
                    std::memcpy(um->Wpn, r.Weapons, sizeof(TerrainWeapon) * (size_t)n);
                    um->WpnDone = true;
                    um->WpnPass = UmPass;
                }
            }
        }
        Mp(MP_VWpn);
        return true;
    }

    // ---- condition log (cond1): why an ally's doll can disagree with the game's HUD -------------
    // Once per mech when first seen, and once more the first time any of its parts reads below
    // 100%: every armour / structure value on each part container, read by NAME on that
    // container's own class, next to what the doll used (offsets resolved on the player's).
    // Logging only - nothing here feeds the display.
    struct CondFields { int32_t Off[10]; };
    std::unordered_map<void*, CondFields> CondFieldCache; // container class -> offsets by name
    std::unordered_set<void*> CondLoggedFirst, CondLoggedDrop;

    const CondFields& CondFieldsOf(API::UObject* c) {
        void* cls = c->get_class();
        auto it = CondFieldCache.find(cls);
        if (it != CondFieldCache.end()) return it->second;
        static const wchar_t* kNames[10] = {
            L"ArmorPoints", L"MaxArmorPoints", L"InitialArmorPoints", L"InitialMaxArmorPoints",
            L"ArmorTraitMultiplierPercent", L"HitPoints", L"MaxHitPoints", L"InitialHitPoints",
            L"InitialMaxHitPoints", L"HealthTraitMultiplierPercent" };
        CondFields f{};
        for (int i = 0; i < 10; ++i) f.Off[i] = PropOffset(c->get_class(), kNames[i]);
        return CondFieldCache.emplace(cls, f).first->second;
    }

    static std::string CondVal(API::UObject* c, int32_t off) {
        if (!c || off < 0) return "-";
        float v = 0.0f;
        std::memcpy(&v, reinterpret_cast<const uint8_t*>(c) + off, sizeof(float));
        char b[32];
        _snprintf_s(b, _countof(b), _TRUNCATE, "%.1f", v);
        return b;
    }

    void CondLogPart(const char* tag, const char* part, API::UObject* cont, float armFrac, float strFrac,
                     bool rear = false) {
        if (!cont || !cont->get_class()) {
            LogInfo("cond1: %s   %-8s no container", tag, part);
            return;
        }
        const CondFields& f = CondFieldsOf(cont);
        // A rear container is read only for HitPoints (its rear armour); a front one for both.
        const bool same = (rear || (f.Off[0] == CArmCur && f.Off[1] == CArmMax)) && f.Off[5] == CHpCur && f.Off[6] == CHpMax;
        LogInfo("cond1: %s   %-8s %s | armour %s / max %s (initial %s / %s, trait %s%%) | structure %s / max %s "
                "(initial %s / %s, trait %s%%) | doll showed armour %.2f structure %.2f | offsets %s",
                tag, part, WideToNarrow(cont->get_class()->get_fname()->to_string().c_str()).c_str(),
                CondVal(cont, f.Off[0]).c_str(), CondVal(cont, f.Off[1]).c_str(), CondVal(cont, f.Off[2]).c_str(),
                CondVal(cont, f.Off[3]).c_str(), CondVal(cont, f.Off[4]).c_str(), CondVal(cont, f.Off[5]).c_str(),
                CondVal(cont, f.Off[6]).c_str(), CondVal(cont, f.Off[7]).c_str(), CondVal(cont, f.Off[8]).c_str(),
                CondVal(cont, f.Off[9]).c_str(), armFrac, strFrac,
                same ? "same as the player's" : "DIFFER from the player's (the doll read the wrong fields)");
    }

    void CondLogMech(API::UObject* m, API::UObject* lc, const TerrainMechInfo& r) {
        bool below = false;
        for (int k = 0; k < TMP_COUNT; ++k)
            below |= (r.Armour[k] >= 0.0f && r.Armour[k] < 0.999f) || (r.Structure[k] >= 0.0f && r.Structure[k] < 0.999f);
        for (int k = 0; k < TRP_COUNT; ++k)
            below |= (r.ArmourRear[k] >= 0.0f && r.ArmourRear[k] < 0.999f);
        const char* why = nullptr;
        if (CondLoggedFirst.insert(m).second) why = "first seen";
        else if (below && CondLoggedDrop.insert(m).second) why = "first reading below 100%";
        if (!why) return;
        char tag[96];
        _snprintf_s(tag, _countof(tag), _TRUNCATE, "%s %s #%u", r.Chassis[0] ? r.Chassis : "?", r.Variant[0] ? r.Variant : "", (unsigned)r.Id);
        LogInfo("cond1: %s (%s, %s) - %s; living component %s", tag, r.Hostile ? "enemy" : "ally",
                WideToNarrow(m->get_class()->get_fname()->to_string().c_str()).c_str(), why,
                lc && lc->get_class() ? WideToNarrow(lc->get_class()->get_fname()->to_string().c_str()).c_str() : "NONE");
        if (!lc) return;
        static const char* kPart[TMP_COUNT] = { "head", "CT", "LT", "RT", "LA", "RA", "LL", "RL" };
        for (int k = 0; k < TMP_COUNT; ++k) {
            API::UObject* cont = nullptr;
            if (PartSlotOff[k] >= 0) cont = *reinterpret_cast<API::UObject**>(reinterpret_cast<uint8_t*>(lc) + PartSlotOff[k]);
            CondLogPart(tag, kPart[k], cont, r.Armour[k], r.Structure[k]);
        }
        static const char* kRear[TRP_COUNT] = { "rear CT", "rear LT", "rear RT" };
        for (int k = 0; k < TRP_COUNT; ++k) {
            API::UObject* cont = nullptr;
            if (RearSlotOff[k] >= 0) cont = *reinterpret_cast<API::UObject**>(reinterpret_cast<uint8_t*>(lc) + RearSlotOff[k]);
            CondLogPart(tag, kRear[k], cont, r.ArmourRear[k], r.StructureRear[k], true);
        }
    }

    // ---- the scan ---------------------------------------------------------------------------
    ULONGLONG MechInfoTick = 0;
    // 5 km radar mode (2026-10-15, user): mech / vehicle records 1.2-5 km are read fully only every 2 s (FarInfoMs);
    // between those passes the last full record is republished (ids match the contacts, so the dolls stay put).
    static constexpr ULONGLONG kFarInfoMs = 2000;
    ULONGLONG FarInfoMs = 0;
    std::unordered_map<void*, TerrainMechInfo> FarInfoCache;
    // Actor -> first scan that saw it dead. Erased the moment the same pointer is seen alive
    // (address reuse), and pruned once well past the hold so it cannot grow over a mission.
    std::unordered_map<void*, ULONGLONG> DeathSeenMs;
    uint16_t LastLockId = 0xFFFF; // for the lock-change log line only

    // UNIT MEMO (unitmemo1, 2026-10-13, user: stage 2 after mechprof1 showed vehicles at ~0.35 ms each, 8-9.7 ms per
    // run): what never changes for a unit - kind / name / art key (target component), its health component, its
    // equipment owner (critical slot component, or the unit itself) - is found once per unit and remembered, instead
    // of searching the unit's components again four times a second. Keyed by actor + class; a unit not listed in a
    // pass is forgotten at the end of it (spent, destroyed, gone, new mission), so a reused address starts afresh.
    // CHECK: every 64th pass per unit everything is looked up the old way again; a difference is logged
    // ("unitmemo check") and the fresh values are kept.
    struct UnitMemo {
        void*         Cls = nullptr;
        uint32_t      Uses = 0, Seen = 0;
        bool          IdentDone = false, HealthDone = false, EqDone = false;
        uint8_t       Kind = 0;
        char          Name[32] = {}, Art[32] = {};
        API::UObject* Health = nullptr;
        API::UObject* EqOwner = nullptr;
        // vwpnmemo1: a vehicle's weapon list (names + parts only - a loadout does not change), re-read every 16th pass
        bool          WpnDone = false;
        uint32_t      WpnPass = 0;
        uint8_t       WpnN = 0;
        TerrainWeapon Wpn[TERR_MAX_WEAPONS] = {};
    };
    std::unordered_map<void*, UnitMemo> UnitMemos;
    uint32_t UmPass = 0;
    int      UmCheckLogs = 0;

    UnitMemo* UmTouch(API::UObject* a) { // in a scan pass: the unit's memo, made or reset as needed
        if (!a) return nullptr;
        void* cls = a->get_class();
        UnitMemo& m = UnitMemos[static_cast<void*>(a)];
        // unitmemo3: trusted only if this unit was also listed in the pass before (or already in this one). A gap -
        // skipped passes, a mission load - means the address may now belong to a different unit: start afresh.
        if (m.Cls != cls || (m.Seen != UmPass && m.Seen + 1 != UmPass)) { m = UnitMemo{}; m.Cls = cls; }
        if (m.Seen != UmPass) { m.Seen = UmPass; ++m.Uses; }
        return &m;
    }
    UnitMemo* UmPeek(API::UObject* a) {
        auto it = UnitMemos.find(static_cast<void*>(a));
        return (it != UnitMemos.end() && a && it->second.Cls == a->get_class() && it->second.Seen == UmPass) ? &it->second : nullptr;
    }
    static bool UmCheckRun(const UnitMemo* m) { return (m->Uses & 63) == 0; }
    void UmLog(API::UObject* a, const char* what, const char* was, const char* now) {
        if (UmCheckLogs >= 30) return;
        ++UmCheckLogs;
        LogInfo("unitmemo check: %s on %s changed: '%s' -> '%s' (fresh value kept)", what,
                a && a->get_class() && a->get_class()->get_fname()
                    ? WideToNarrow(a->get_class()->get_fname()->to_string().c_str()).c_str() : "?", was, now);
    }
    void UmPrune() {
        for (auto it = UnitMemos.begin(); it != UnitMemos.end();)
            it = (it->second.Seen != UmPass) ? UnitMemos.erase(it) : std::next(it);
    }
    // Kind / name / art from the target component (+ the art memo, + the vehicle name fallback), once per unit.
    void UmIdent(API::UObject* a, UnitMemo* m, uint8_t& kind, char name[32], char art[32], bool vehicle) {
        if (m && m->IdentDone && !UmCheckRun(m)) {
            kind = m->Kind;
            if (name) std::memcpy(name, m->Name, 32);
            std::memcpy(art, m->Art, 32);
            return;
        }
        char ignore[32] = "";
        char* nm = name ? name : ignore;
        ReadTargetComp(a, kind, nm, art);
        KeepArt(a, kind, art);
        if (vehicle && !nm[0]) VehicleName(a, nm);
        if (!m) return;
        if (m->IdentDone) {
            if (m->Kind != kind) { char x[8], y[8]; _snprintf_s(x, _countof(x), _TRUNCATE, "%u", m->Kind); _snprintf_s(y, _countof(y), _TRUNCATE, "%u", kind); UmLog(a, "kind", x, y); }
            if (std::strncmp(m->Art, art, 32) != 0) UmLog(a, "art", m->Art, art);
            if (name && std::strncmp(m->Name, name, 32) != 0) UmLog(a, "name", m->Name, name);
        }
        m->Kind = kind;
        if (name) std::memcpy(m->Name, name, 32);
        std::memcpy(m->Art, art, 32);
        // unitmemo2: remembered only once the art key is known. A unit just spawned can come back without one (the
        // unitmemo1 test: two mechs had '' first and their art a few passes later) - keep looking every pass, as the
        // old code did, until it resolves; a unit that never has art keeps the old per-pass lookup.
        m->IdentDone = art[0] != 0;
    }

    // MECHINFO PROFILE (mechprof1, 2026-10-13, user: "do stage 1"): measuring only - ScanMechInfo ("slow: LidarExport
    // mech records + dropships", 5-10 ms at 4 Hz with ~30 units) timed by part, totals every 30 s as
    // "mechinfo profile" / "mechinfo content". Nothing read or published changes.
    enum { MP_Setup, MP_List, MP_Pose, MP_Ident, MP_Dead, MP_Call, MP_Art, MP_Cond, MP_Wpn, MP_Heat,
           MP_VList, MP_VRead, MP_Ships, MP_Pub, MP_VName, MP_VHealth, MP_VWpn, MP_N };
    LARGE_INTEGER MpT{}, MpFreq{};
    double        MpRun[MP_N] = {}, MpSum[MP_N] = {}, MpMax[MP_N] = {}, MpTotSum = 0, MpTotMax = 0;
    unsigned      MpRuns = 0, MpMechs = 0, MpVehs = 0, MpSkipped = 0;
    ULONGLONG     MpLogMs = 0;
    bool          MpOn = false;

    void MpBegin() {
        if (!MpFreq.QuadPart) QueryPerformanceFrequency(&MpFreq);
        QueryPerformanceCounter(&MpT);
        std::memset(MpRun, 0, sizeof(MpRun));
        MpOn = true;
    }
    void Mp(int k) { // the time since the last mark goes to part k
        if (!MpOn) return;
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        MpRun[k] += double(t.QuadPart - MpT.QuadPart) * 1000.0 / double(MpFreq.QuadPart);
        MpT = t;
    }
    void MpEnd(unsigned mechs, unsigned vehs) {
        if (!MpOn) return;
        MpOn = false;
        double tot = 0;
        for (int k = 0; k < MP_N; ++k) {
            tot += MpRun[k];
            MpSum[k] += MpRun[k];
            if (MpRun[k] > MpMax[k]) MpMax[k] = MpRun[k];
        }
        MpTotSum += tot;
        if (tot > MpTotMax) MpTotMax = tot;
        ++MpRuns; MpMechs += mechs; MpVehs += vehs;
        const ULONGLONG now = GetTickCount64();
        if (!MpLogMs) MpLogMs = now;
        if (now - MpLogMs < 30000) return;
        static const char* kN[MP_N] = { "setup", "list", "pose", "ident", "dead", "callsign", "art", "cond", "weapons",
                                        "heat", "vlist", "vread", "dropships", "publish", "vname", "vhealth", "vweapons" };
        char b[900]; int o = _snprintf_s(b, _countof(b), _TRUNCATE, "mechinfo profile, last 30 s: %u runs, total avg %.2f / worst %.2f ms -",
                                       MpRuns, MpRuns ? MpTotSum / MpRuns : 0.0, MpTotMax);
        for (int k = 0; k < MP_N && o > 0 && o < (int)sizeof(b) - 40; ++k)
        {
            const int n = _snprintf_s(b + o, sizeof(b) - o, _TRUNCATE, " %s %.2f/%.2f", kN[k], MpRuns ? MpSum[k] / MpRuns : 0.0, MpMax[k]);
            if (n < 0) break;   // fmttrunc1: truncated - stop appending
            o += n;
        }
        if (gsp::VerboseLog()) LogInfo("%s", b);
        if (gsp::VerboseLog()) LogInfo("mechinfo content, last 30 s (per run): mechs %.1f, vehicles %.1f; skipped (spent / out of range) %.1f",
                MpRuns ? double(MpMechs) / MpRuns : 0.0, MpRuns ? double(MpVehs) / MpRuns : 0.0,
                MpRuns ? double(MpSkipped) / MpRuns : 0.0);
        std::memset(MpSum, 0, sizeof(MpSum)); std::memset(MpMax, 0, sizeof(MpMax));
        MpTotSum = MpTotMax = 0; MpRuns = MpMechs = MpVehs = MpSkipped = 0;
        MpLogMs = now;
    }

    void ScanMechInfo(API::UObject* pawn, const FVector3f& ploc) {
        uint8_t playerTeam = 0xFF;
        if (const auto* tp = TeamByte(pawn)) playerTeam = *tp;
        if (!kMechInfoEnabled) { if (Terr) Terr->H.MechInfoCount = 0; return; }
        if (!Terr || !pawn || !BaseMechClass) return;
        const ULONGLONG now = GetTickCount64();
        if (MechInfoTick && now - MechInfoTick < kMechInfoMs) return;
        MechInfoTick = now;
        // 5 km radar mode: is this one of the 2 s passes that also reads the far units in full?
        if (!FarOn && !FarInfoCache.empty()) FarInfoCache.clear();
        if (FarInfoCache.size() > 512) FarInfoCache.clear();
        const bool farPass = FarOn && now - FarInfoMs >= kFarInfoMs;
        if (farPass) FarInfoMs = now;
        MpBegin(); // mechprof1
        ++UmPass;  // unitmemo1

        // compfix2: the component layout FIRST. compfix1 put it after ResolveConditionComps, which
        // runs once - it found no living component (no layout yet), gave up for good, and every
        // record went out with structure 0, which the dolls draw as "part destroyed": blank plates.
        EnsureComponentLayout(pawn);
        // Components first: the layout dump prints the per-part container's members, and it can
        // only reach the container through LivingComp.
        ResolveConditionComps(pawn);
        ResolveWeaponLayout(pawn);
        LogConditionLayoutOnce(pawn);
        ResolveGameplayStatics();
        // compfix1: the component lookups (target component -> art key, silhouettes) need the
        // layout, and its only caller was the structure scan (removed, nostruct1). It is made
        // above now (compfix2), before the condition components are looked up.
        Mp(MP_Setup);

        int n = 0;
        API::UObject** list = GetActorsOfClass(pawn, BaseMechClass, n);
        if (!list) { MpOn = false; return; }

        // Copy the pointers out before doing anything else: GaacBuf is reused by every
        // enumeration, and IdentOf issues its own reflected call.
        API::UObject* mechs[TERR_MAX_MECHS * 2];
        int count = 0;
        for (int i = 0; i < n && count < (int)(sizeof(mechs) / sizeof(mechs[0])); ++i)
            if (list[i] && list[i] != pawn && !IsSpentActor(list[i])) mechs[count++] = list[i];

        TerrainMechInfo out[TERR_MAX_MECHS];
        uint32_t written = 0;

        // The game's lock: TargetTracking.CurrentTarget, and only that. The other target-named
        // slots on the component are not the lock - one of them is non-null from mission start
        // with nothing targeted - which is why following all of them never let the lock clear.
        // Same field MWVR's HeadAim reads and writes as the current target.
        void* curTarget = nullptr;
        if (TargetTrackingC)
            if (auto* pp = TargetTrackingC->get_property_data<void*>(L"CurrentTarget"))
                curTarget = *pp;
        uint16_t lockId = 0;
        auto republishFar = [&](API::UObject* u) {
            auto fc = FarInfoCache.find(static_cast<void*>(u));
            if (fc == FarInfoCache.end() || written >= (uint32_t)TERR_MAX_MECHS) return;
            out[written] = fc->second;
            out[written].Locked = (curTarget && static_cast<void*>(u) == curTarget) ? 1 : 0;
            if (out[written].Locked) lockId = out[written].Id;
            ++written;
        };
        unsigned mpMechs = 0, mpVehs = 0;

        for (auto it = DeathSeenMs.begin(); it != DeathSeenMs.end();)
            it = (now - it->second > 600000) ? DeathSeenMs.erase(it) : std::next(it); // deadflag1: 10 min (60 s re-showed latched wrecks)
        Mp(MP_List);

        for (int i = 0; i < count && written < (uint32_t)TERR_MAX_MECHS; ++i) {
            API::UObject* m = mechs[i];

            FVector3f mloc{}; float myaw = 0.0f;
            if (!ReadActorPose(m, mloc, myaw)) { Mp(MP_Pose); ++MpSkipped; continue; }
            const float dx = mloc.X - ploc.X, dy = mloc.Y - ploc.Y;
            bool farMech = false; // 5 km radar mode: 1.2-5 km, read in full every 2 s
            if (dx * dx + dy * dy > kMechInfoRangeCm * kMechInfoRangeCm) {
                if (!FarOn || dx * dx + dy * dy > LIDAR_FAR_RANGE_CM * LIDAR_FAR_RANGE_CM) { Mp(MP_Pose); ++MpSkipped; continue; }
                farMech = true;
                if (!farPass) { republishFar(m); Mp(MP_Pose); continue; }
            }
            Mp(MP_Pose);
            ++mpMechs;

            const MechIdent* id = IdentOf(m);
            Mp(MP_Ident);

            TerrainMechInfo& r = out[written];
            std::memset(&r, 0, sizeof(r));
            r.Id      = AssignId(static_cast<void*>(m)); // same id space as the LIDAR contacts
            SetActor(r, m);
            r.Valid   = 1;
            r.Kind    = TUK_Mech;
            r.HeatPct = -1.0f;

            uint8_t team = 0xFF;
            if (const auto* p = TeamByte(m)) team = *p;
            r.Hostile = (team == playerTeam) ? 0 : 1;

            // Dead: the same test the scope scan uses to drop a mech. The wreck actor stays
            // enumerable until it sinks, so without this its record - and the 3D page's label -
            // outlived the kill by however long that took. Held TERR_DEAD_HOLD_MS, flagged, with
            // nothing else read: a wreck has no loadout worth showing.
            if (UnitDead(m, true)) { // deadflag1: the game's own flag
                Mp(MP_Dead);
                auto ins = DeathSeenMs.emplace(static_cast<void*>(m), now).first;
                if (now - ins->second > TERR_DEAD_HOLD_MS) continue; // hold over: gone
                r.Dead = 1;
                ++written;
                continue;
            }
            Mp(MP_Dead);
            DeathSeenMs.erase(static_cast<void*>(m));
            if (curTarget && static_cast<void*>(m) == curTarget) { r.Locked = 1; lockId = r.Id; }

            // The pilot, for our own side only.
            Breadcrumb("mech: callsign");
            if (!r.Hostile) CallsignOf(m, r.Callsign);
            Mp(MP_Call);

            // A mech's NAME is already right (ChassisFullName / VariantName off the data asset),
            // but its ART key was being derived from that display name - and "Mist Lynx" with a
            // space never matched the mistlynx folder. The texture reference has no such problem.
            {
                uint8_t mk = TUK_Mech;
                Breadcrumb("mech: target component");
                UmIdent(m, UmTouch(m), mk, nullptr, r.ArtKey, false); // unitmemo1: once per unit (was every pass)
                r.Kind = mk;
            }
            Mp(MP_Art);

            if (id) {
                std::memcpy(r.Chassis, id->Chassis, sizeof(r.Chassis));
                std::memcpy(r.Variant, id->Variant, sizeof(r.Variant));
                r.Tons        = id->Tons;
                r.WeightClass = id->WeightClass;
                // Weapons are NOT taken from the identity cache: they change during a fight.
            } else {
                r.WeightClass = 0xFF;
            }

            // Live condition. Read off THIS mech's own living-actor component - the offsets were
            // resolved on the player's, which is the same class, so they transfer.
            if (LivingComp && (CArmCur >= 0 || CHpCur >= 0)) {
                API::UObject* lc = CompByClassName(m, L"LivingActor");
                int got = 0;
                for (int k = 0; k < TMP_COUNT; ++k) {
                    r.Armour[k] = r.Structure[k] = -1.0f;
                    if (!lc || PartSlotOff[k] < 0) continue;
                    auto** slot = reinterpret_cast<API::UObject**>(
                        reinterpret_cast<uint8_t*>(lc) + PartSlotOff[k]);
                    API::UObject* cont = slot ? *slot : nullptr;
                    if (!cont) continue;
                    r.Armour[k]    = FracAt(cont, CArmCur, CArmMax);
                    r.Structure[k] = FracAt(cont, CHpCur,  CHpMax);
                    if (r.Armour[k] >= 0.0f || r.Structure[k] >= 0.0f) ++got;
                }
                r.HaveCondition = (got > 0) ? 1 : 0;

                // REAR FACES (cond1 log). A rear torso's container is a plain MWHealthComponent -
                // no armour fields at all - and its HitPoints / MaxHitPoints IS that face's rear
                // armour (Catapult: rear CT 18, rear LT/RT 12). Reading the front's armour offsets
                // on it read past the end of the object. Front and rear armour are separate, but
                // they share ONE structure: the front torso container's HitPoints.
                static const int kFrontOf[TRP_COUNT] = { TMP_CentreTorso, TMP_LeftTorso, TMP_RightTorso };
                int rgot = 0;
                for (int k = 0; k < TRP_COUNT; ++k) {
                    r.ArmourRear[k] = r.StructureRear[k] = -1.0f;
                    if (!lc || RearSlotOff[k] < 0) continue;
                    auto** rslot = reinterpret_cast<API::UObject**>(
                        reinterpret_cast<uint8_t*>(lc) + RearSlotOff[k]);
                    API::UObject* rc = rslot ? *rslot : nullptr;
                    if (!rc) continue;
                    r.ArmourRear[k]    = FracAt(rc, CHpCur, CHpMax);
                    r.StructureRear[k] = r.Structure[kFrontOf[k]];
                    if (r.ArmourRear[k] >= 0.0f) ++rgot;
                }
                r.HaveRear = (rgot > 0) ? 1 : 0;
                CondLogMech(m, lc, r);
            }
            Mp(MP_Cond);

            r.WeaponCount = ReadWeapons(m, r.Weapons, !MechInfoLogged && written == 0,
                                        id ? id->WeaponNames : nullptr, id ? id->NameCount : 0,
                                        id ? id->HudWeapons : nullptr);
            Mp(MP_Wpn);

            if (HeatComp && (HeatPctOff >= 0 || HeatCurOff >= 0)) {
                API::UObject* hc = CompByClassName(m, L"Heat");
                float f = -1.0f;
                if (hc && HeatPctOff >= 0) {
                    std::memcpy(&f, reinterpret_cast<const uint8_t*>(hc) + HeatPctOff, sizeof(float));
                    if (f > 1.5f) f *= 0.01f; // published as 0..100 rather than 0..1
                    f = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
                } else {
                    f = FracAt(hc, HeatCurOff, HeatMaxOff);
                }
                if (f >= 0.0f) { r.HeatPct = f; r.HaveHeat = 1; }
            }
            Mp(MP_Heat);

            if (farMech) FarInfoCache[static_cast<void*>(m)] = r; // republished between the 2 s far passes
            ++written;
        }

        // Vehicles fill whatever slots the mechs left. Mechs first is deliberate: if a defence
        // mission ever overruns the array, losing a turret record costs less than losing a mech.
        // ba4 (user 2026-10-17): battle armour suits get a record like every other enemy (name, HP, weapons, lock).
        const struct { UEVR_UClassHandle cls; uint8_t kind; } vbases[4] = {
            { TankClass, TUK_Tank }, { TurretClass, TUK_Turret }, { VtolClass, TUK_Vtol }, { BaClass, TUK_BattleArmor }
        };
        for (int b = 0; b < 4 && written < (uint32_t)TERR_MAX_MECHS; ++b) {
            if (!vbases[b].cls) continue;
            Mp(MP_VRead);
            int vn = 0;
            API::UObject** vlist = GetActorsOfClass(pawn, vbases[b].cls, vn);
            if (!vlist) { Mp(MP_VList); continue; }

            // Copied out before any further reflected call - GaacBuf is reused by every
            // enumeration, and ReadVehicle issues its own.
            API::UObject* vs[TERR_MAX_MECHS];
            int vc = 0;
            for (int i = 0; i < vn && vc < TERR_MAX_MECHS; ++i)
                if (vlist[i] && !IsSpentActor(vlist[i])) vs[vc++] = vlist[i];
            Mp(MP_VList);

            for (int i = 0; i < vc && written < (uint32_t)TERR_MAX_MECHS; ++i) {
                FVector3f vloc{}; float vyaw = 0.0f;
                if (!ReadActorPose(vs[i], vloc, vyaw)) continue;
                const float vdx = vloc.X - ploc.X, vdy = vloc.Y - ploc.Y;
                bool farVeh = false; // 5 km radar mode, as the mechs
                if (vdx * vdx + vdy * vdy > kMechInfoRangeCm * kMechInfoRangeCm) {
                    if (!FarOn || vdx * vdx + vdy * vdy > LIDAR_FAR_RANGE_CM * LIDAR_FAR_RANGE_CM) continue;
                    farVeh = true;
                    if (!farPass) { republishFar(vs[i]); continue; }
                }
                ++mpVehs;
                if (ReadVehicle(vs[i], vbases[b].kind, playerTeam, out[written])) {
                    if (UnitDead(vs[i], false)) { // deadflag1: a vehicle wreck, held like a mech's then gone
                        auto ins = DeathSeenMs.emplace(static_cast<void*>(vs[i]), now).first;
                        if (now - ins->second > TERR_DEAD_HOLD_MS) continue;
                        out[written].Dead = 1;
                    } else
                        DeathSeenMs.erase(static_cast<void*>(vs[i]));
                    if (curTarget && static_cast<void*>(vs[i]) == curTarget) {
                        out[written].Locked = 1;
                        lockId = out[written].Id;
                    }
                    if (farVeh) FarInfoCache[static_cast<void*>(vs[i])] = out[written];
                    ++written;
                }
            }
        }

        if (lockId != LastLockId) {
            LastLockId = lockId;
            LogInfo("lock: CurrentTarget %s -> id %u", curTarget ? "set" : "null", (unsigned)lockId);
        }

        Mp(MP_VRead);
        TerrainDropship ships[TERR_MAX_DROPSHIPS];
        const uint32_t shipN = DropshipScan(pawn, playerTeam, ships, TERR_MAX_DROPSHIPS, now); // dropship1
        Mp(MP_Ships);

        Breadcrumb("scan: complete");
        ++Terr->H.FrameCounter;
        Terr->H.MechInfoCount = written;
        if (written > 0) std::memcpy(Terr->H.Mechs, out, written * sizeof(TerrainMechInfo));
        Terr->H.DropshipCount = shipN;
        if (shipN > 0) std::memcpy(Terr->H.Dropships, ships, shipN * sizeof(TerrainDropship));
        Terr->H.MechInfoScanned = 1;
        Terr->H.FrameCounterEnd = Terr->H.FrameCounter;
        UmPrune(); // unitmemo1: units not listed this pass are forgotten
        Mp(MP_Pub);
        MpEnd(mpMechs, mpVehs); // mechprof1 (the first-scan log lines below are not counted)

        if (!MechInfoLogged && written > 0) {
            MechInfoLogged = true;
            const TerrainMechInfo& r = Terr->H.Mechs[0];
            uint32_t nveh = 0;
            for (uint32_t k = 0; k < written; ++k) nveh += (Terr->H.Mechs[k].Kind != TUK_Mech);
            LogInfo("mechinfo: %u vehicle(s) of the above", nveh);
            LogInfo("mechinfo: %u unit(s); first '%s' '%s' %.0ft class %u weapons %u "
                    "cond %u rear %u heat %u (%.2f)",
                    written, r.Chassis[0] ? r.Chassis : "?", r.Variant[0] ? r.Variant : "?",
                    r.Tons, r.WeightClass, r.WeaponCount, r.HaveCondition, r.HaveRear,
                    r.HaveHeat, r.HeatPct);
            for (uint8_t w = 0; w < r.WeaponCount; ++w)
                LogInfo("mechinfo:   weapon '%s' part %u x%u", r.Weapons[w].Name, r.Weapons[w].Part,
                        r.Weapons[w].Count ? r.Weapons[w].Count : 1u);

            // WHETHER THE NEW ROUTES ACTUALLY FIRED. Each of these is a separate resolve that can
            // fail on its own and fall back silently to the old behaviour, so each is reported by
            // name rather than inferred from the output looking plausible - which is precisely how
            // the weapon-name pairing stayed broken through several builds.
            uint32_t withArt = 0, withCall = 0;
            for (uint32_t k = 0; k < written; ++k) {
                withArt  += (Terr->H.Mechs[k].ArtKey[0]   != '\0');
                withCall += (Terr->H.Mechs[k].Callsign[0] != '\0');
            }
            LogInfo("mechinfo: art key on %u/%u, callsign on %u, weapons via %s",
                    withArt, written, withCall,
                    EqUsable ? "GetEquipment" : "weapon registry (fallback)");
        }
    }

    // ---- one-shot: confirm ground is reachable and report the origin offset -----------------
    void TerrainProbe(API::UObject* pawn, const FVector3f& ploc) {
        if (!kTerrainProbe || TerrainProbeDone || !pawn) return;
        TerrainProbeDone = true;
        if (!EnsureTraceLayout()) return;

        const FVector3f from{ploc.X, ploc.Y, ploc.Z + 200.0f};
        const FVector3f to  {ploc.X, ploc.Y, ploc.Z - 20000.0f};
        const TraceResult g = RunTrace(pawn, from, to);

        if (!g.Blocking) { LogError("terrain: no ground under the mech - unexpected"); return; }
        const float dxy = std::fabs(g.X - ploc.X) + std::fabs(g.Y - ploc.Y);
        if (dxy > 1.0f) { LogError("terrain: hit %.0f cm off the trace line - frame suspect", dxy); return; }
        if (g.Z >= from.Z) { LogError("terrain: hit at or above the start - not ground"); return; }

        // Reported, not stored: TerrainStep re-measures this every probe.
        LogInfo("terrain: ground at Z=%.0f, pawn origin %.2f m above it at this instant", g.Z,
                (ploc.Z - g.Z) / 100.0f);

        // Head to head, at a point whose answer we already know from the line above. If the
        // upward ray agrees, the inversion is sound and every sample can use it; if it finds
        // nothing, terrain does not block from underneath and the fallback carries the load.
        const FVector3f upFrom{ploc.X, ploc.Y, ploc.Z - 100000.0f};
        const FVector3f upTo  {ploc.X, ploc.Y, ploc.Z + 100000.0f};
        const TraceResult u = RunTrace(pawn, upFrom, upTo);
        if (!u.Blocking) {
            LogError("terrain: UPWARD ray found nothing - terrain does not block from below, "
                     "so sampling will fall back to downward rays");
        } else {
            LogInfo("terrain: upward ray hit Z=%.0f vs downward Z=%.0f (delta %.2f m) - %s",
                    u.Z, g.Z, (u.Z - g.Z) / 100.0f,
                    (std::fabs(u.Z - g.Z) < 100.0f) ? "AGREE, inversion is sound"
                                                    : "DISAGREE, investigate before trusting it");
        }
    }

    // =======================================================================================
    // one-shot: WHAT DOES A TRACE COST? This is the last guess in the topo plan - the tile
    // cache's fill rate and therefore the grid spacing both hang off it.
    //
    // Measured honestly: samples are spread across a real 16 m lattice rather than repeating
    // one point, because hammering a single spot would sit in a physics cache and report a
    // cost we could never achieve in practice. The ramp aborts as soon as a batch exceeds the
    // budget, so this cannot turn into a multi-second stall on a slow machine.
    //
    // It also reports the RELIEF across the samples, which is the number that decides the
    // vertical exaggeration the wireframe will need - real terrain over a 500 m span is far
    // flatter than it looks from the cockpit.
    // =======================================================================================
    void TerrainTimingProbe(API::UObject* pawn, const FVector3f& ploc) {
        if (!kTerrainProbe || !kTerrainTiming || TerrainTimingDone || !pawn || !TraceReady) return;
        TerrainTimingDone = true;

        static constexpr int      kBatches[]  = {16, 64, 256, 1024};
        static constexpr float    kSpacingCm  = 1600.0f;  // 16 m, the candidate grid spacing
        static constexpr long long kBudgetUs  = 20000;    // stop the ramp past a 20 ms batch

        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        if (freq.QuadPart == 0) { LogError("terrain timing: no performance counter"); return; }

        for (const int n : kBatches) {
            const int side = (int)std::ceil(std::sqrt((double)n));
            const float half = (side - 1) * kSpacingCm * 0.5f;

            int   hits = 0;
            float zMin = 1e9f, zMax = -1e9f;

            LARGE_INTEGER t0{}, t1{};
            QueryPerformanceCounter(&t0);
            for (int i = 0; i < n; ++i) {
                const float x = ploc.X - half + (i % side) * kSpacingCm;
                const float y = ploc.Y - half + (i / side) * kSpacingCm;
                float z = 0.0f;
                if (GroundAt(pawn, x, y, ploc.Z, z)) {
                    ++hits;
                    if (z < zMin) zMin = z;
                    if (z > zMax) zMax = z;
                }
            }
            QueryPerformanceCounter(&t1);

            const long long us = (t1.QuadPart - t0.QuadPart) * 1000000LL / freq.QuadPart;
            const double perUs = n ? (double)us / n : 0.0;

            LogInfo("terrain timing: %4d traces over %dx%d @16m -> %lld us total, %.1f us each, "
                    "%d/%d hit, relief %.1f m",
                    n, side, side, us, perUs, hits, n,
                    (hits > 1) ? (zMax - zMin) / 100.0f : 0.0f);

            if (us > kBudgetUs) {
                LogInfo("terrain timing: batch exceeded %lld us - stopping the ramp here", kBudgetUs);
                break;
            }
            if (perUs > 0.0) {
                LogInfo("      extrapolated: 32x32 tile (1024) ~%.1f ms, 128x128 grid (16384) ~%.0f ms",
                        perUs * 1024.0 / 1000.0, perUs * 16384.0 / 1000.0);
            }
        }
    }

    // =======================================================================================
    // TERRAIN TILE CACHE - phase A of the topographic map. Publishes absolute world ground
    // heights on a 16 m lattice into its own memory-mapped block (see TerrainShared.h).
    //
    // WORLD-ANCHORED, NOT PLAYER-RELATIVE. Tiles are keyed by integer world coordinates, so a
    // tile traced once stays valid for the rest of the mission - terrain is static. That is
    // what makes this cheap: no refresh, no invalidation, no sliding window to rebuild as the
    // player moves. New tiles fill as they come into range and finished ones simply stay.
    //
    // COST, MEASURED RATHER THAN ASSUMED. 4.7 us per trace, so kTracesPerStep = 200 costs
    // about 0.94 ms per probe. The timing probe's 1024-trace burst took 4.8 ms in one frame and
    // was a visible hitch, which is exactly why this chunks instead of filling a tile at once.
    // At 30 Hz a tile completes in ~0.16 s and a 2 km area in ~2.5 s.
    //
    // RESET ON PAWN CHANGE. Any pawn change throws the whole cache away, and a null pawn means
    // we are not in a mission. That is blunter than necessary - taking over an AI mech mid-
    // mission re-traces terrain that had not changed - but a full re-fill costs ~2.5 s, which
    // is cheap enough that correctness by simplicity wins: no world-pointer reflection, no
    // staleness edge cases, and one branch covers both "new mech" and "no mission".
    // =======================================================================================
    static constexpr int kTracesPerStep = 200; // ~0.94 ms at the measured 4.7 us per trace

    // Tiles claimed per probe. Claiming is a 4 KB memset plus a 5-trace ground probe, so it is
    // cheap, but it is not free and there is no reason to claim faster than tiles can be filled.
    static constexpr int kClaimsPerStep = 4;

    // Cheap rejection before committing a slot. A tile that turns out to be entirely off the
    // playable map still costs 1024 traces to discover the slow way, and a map boundary has a
    // lot of those tiles. Five traces - four corners and the centre - answers "is there any
    // ground here at all" for 0.4% of the price.
    //
    // This CAN be fooled by ground that exists only between the probe points. For terrain at
    // 512 m per tile that would mean a feature smaller than half a tile surrounded by void,
    // which is not a shape real maps have; it is a deliberate trade, not an oversight.
    static constexpr int kProbePoints = 5;

    // Ground probes per step, budgeted SEPARATELY from claims - and this is not a nicety.
    // A rejected tile is not a claim, so a claim-only budget lets the sweep keep probing until
    // it finds four claimable tiles; at a map boundary, where almost everything is void, a
    // simulation of this loop issued 1089 probes in a burst. That is ~5,000 traces, a 4.7 ms
    // spike on the game thread, which is the one place in this whole design where a spike
    // actually costs something. Twelve probes is 60 traces, about 0.28 ms.
    static constexpr int kProbesPerStep = 12;

    // How far past the furthest ground found so far the sweep is willing to look, in tiles.
    // Without it, exploration probes the entire 33 x 33 candidate area - 1089 tiles - to prove
    // that the 1001 empty ones are empty. With it, only a two-tile halo around the discovered
    // map gets probed.
    //
    // The trade: ground separated from the rest of the map by a gap wider than two tiles would
    // never be found. Mission terrain is contiguous, so this is a deliberate trade and not an
    // oversight - but it IS the assumption to revisit first if a map ever comes back clipped.
    static constexpr int kExploreHalo = 2;

    // Radius, in tiles, inside which a tile is never evicted. Eviction is now a FALLBACK for a
    // map bigger than the cache rather than the normal case, but if it does fire it must not
    // take ground out from under the pilot.
    static constexpr int kTerrKeepRadius = 3;

    static_assert((2 * kTerrKeepRadius + 1) * (2 * kTerrKeepRadius + 1) < TERR_MAX_TILES,
                  "the protected radius must be smaller than the cache, or eviction has no "
                  "victims and tile claiming stalls");

    HANDLE        TerrMmf   = nullptr;
    TerrainBlock *Terr      = nullptr;
    const API::UObject *TerrPawn = nullptr;  // the pawn this cache belongs to
    // Height of the pawn's actor origin above the ground beneath it, RE-MEASURED EVERY PROBE.
    //
    // This started as a one-shot measurement and that was a design error, not a tuning one. The
    // same mech reported 6.34 m on one mission and 12.01 m on another, because a single sample
    // catches whatever the mech happened to be doing at that instant - mid-hotdrop, on a slope,
    // or jump-jetting. A value that changes moment to moment cannot be a calibration, and no
    // amount of interpreting the number would have fixed the method.
    //
    // One extra trace per probe costs 3.4 us, which is nothing beside the 200-sample tile work,
    // and it turns a dubious constant into a live altitude-above-ground figure - which is what
    // the player marker needs to sit on the surface, and is independently useful with jump jets.
    float         OriginAboveGroundCm = -1.0f;
    uint32_t      TerrCompleteLogged  = 0xFFFFFFFFu;
    uint32_t      TerrEvictions       = 0;      // since the last cache clear
    ULONGLONG     TerrEvictLogTick    = 0;

    // ---- exploration state, producer-only -------------------------------------------------
    //
    // Indexed by tile coordinate offset from the anchor, so lookups are O(1) instead of a linear
    // scan of 512 slots per candidate. TERR_GRID_SIDE^2 is 1089 entries: 2 KB and 1 KB.
    enum : uint8_t { TC_Unknown = 0, TC_Claimed = 1, TC_OffMap = 2 };
    uint8_t  TerrCell[TERR_GRID_SIDE * TERR_GRID_SIDE] = {};
    int16_t  TerrSlotOf[TERR_GRID_SIDE * TERR_GRID_SIDE] = {};
    // How many samples of each slot are written. This used to live in the tile as Filled, but
    // the tile is pure heights now - the consumer has no business seeing a fill cursor, and
    // keeping it out of the mapping is what lets the slot table stay 16 bytes per entry.
    int32_t  TerrFillCursor[TERR_MAX_TILES] = {};
    // Trace-outcome counters are process-wide (the live-altitude probe and the five-point
    // ground probe bump them too), so a tile's own figures are the DELTA across its fill. v1
    // reported "up 1029" for a 1024-sample tile for exactly this reason.
    uint32_t TerrUpBase = 0, TerrDownBase = 0, TerrMissBase = 0, TerrUnderBase = 0, TerrRejectBase = 0;
    int32_t  TerrAnchorX = 0, TerrAnchorY = 0;
    bool     TerrAnchored = false;
    int32_t  TerrScanRing = 0;      // how far out exploration has swept
    int32_t  TerrGroundRing = 0;    // furthest ring from the anchor that yielded ground
    bool     TerrExploring = true;
    bool     TerrRangeWarned = false;
    ULONGLONG TerrProgressTick = 0;

    // Anchor-relative index, or -1 when the tile is outside the explorable area at all.
    int TerrCellIndex(int32_t tx, int32_t ty) const {
        const int32_t ax = tx - TerrAnchorX + TERR_MAX_RING;
        const int32_t ay = ty - TerrAnchorY + TERR_MAX_RING;
        if (ax < 0 || ay < 0 || ax >= TERR_GRID_SIDE || ay >= TERR_GRID_SIDE) return -1;
        return (int)(ay * TERR_GRID_SIDE + ax);
    }

    void SetupTerrainMmf() {
        TerrMmf = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(sizeof(TerrainBlock)), TERRAIN_MMF_NAME);
        if (!TerrMmf) { LogError("terrain: CreateFileMapping failed (%lu) - no heightfield", GetLastError()); return; }
        Terr = static_cast<TerrainBlock*>(
            MapViewOfFile(TerrMmf, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TerrainBlock)));
        if (!Terr) {
            LogError("terrain: MapViewOfFile failed (%lu) - no heightfield", GetLastError());
            CloseHandle(TerrMmf); TerrMmf = nullptr; return;
        }

        ZeroMemory(Terr, sizeof(TerrainBlock));
        Terr->H.Version        = TERRAIN_VERSION;
        Terr->H.SamplesPerSide = TERR_SAMPLES;
        Terr->H.SpacingCm      = TERR_SPACING_CM;
        Terr->H.TileCm         = TERR_TILE_CM;
        Terr->H.TileCount      = TERR_MAX_TILES;
        // bugsweep2 (2026-10-18, sweep #20): seeded per game session, not 1 - HUDDisplay keeps the block open across a game
        // restart, and a count that started at 1 again could hit the number it last saw (kept the old map's tiles).
        Terr->H.Generation     = (static_cast<uint32_t>(GetTickCount64()) & 0x7FFFFFF0u) | 1u;
        Terr->H.Exploring      = 1;
        Terr->H.FrameCounterEnd = Terr->H.FrameCounter;
        LogInfo("terrain: block ready (%.2f MB, %d slots of %dx%d @%.0f cm, explore radius %d tiles)",
                sizeof(TerrainBlock) / (1024.0 * 1024.0), TERR_MAX_TILES,
                TERR_SAMPLES, TERR_SAMPLES, TERR_SPACING_CM, TERR_MAX_RING);
    }

    // Discard everything. Bumping Generation is what tells the consumer its cached tiles are
    // meaningless now, because slot contents no longer correspond to what it read.
    void TerrainReset(const API::UObject* pawn, const char* why) {
        if (!Terr) return;
        ++Terr->H.FrameCounter;
        for (int i = 0; i < TERR_MAX_TILES; ++i) {
            Terr->H.Slots[i].State = TTS_Empty;
            Terr->H.Slots[i].Hits  = 0;
        }
        ++Terr->H.Generation;
        Terr->H.Complete  = 0;
        Terr->H.Claimed   = 0;
        Terr->H.OffMap    = 0;
        Terr->H.Exploring = 1;
        // Structures belong to the map just discarded. Clearing Scanned as well as the count is
        // what lets a consumer tell "this map has no buildings" from "nobody has looked yet".
        Terr->H.StructureCount    = 0;
        Terr->H.StructuresScanned = 0;
        Terr->H.SilhouetteCount   = 0;
        Terr->H.SilhouettesOk     = 0;
        Terr->H.MinTileX = Terr->H.MinTileY = 0;
        Terr->H.MaxTileX = Terr->H.MaxTileY = 0;
        Terr->H.FrameCounterEnd = Terr->H.FrameCounter;

        ZeroMemory(TerrCell, sizeof(TerrCell));
        for (int i = 0; i < TERR_GRID_SIDE * TERR_GRID_SIDE; ++i) TerrSlotOf[i] = -1;
        ZeroMemory(TerrFillCursor, sizeof(TerrFillCursor));
        TerrPawn           = pawn;
        TerrAnchored       = false;
        TerrScanRing       = 0;
        TerrGroundRing     = 0;
        TerrExploring      = true;
        TerrRangeWarned    = false;
        TerrCompleteLogged = 0xFFFFFFFFu;
        TerrEvictions      = 0;
        StructScanTick     = 0;
        SilScanTick        = 0;
        SilNamesWarned     = false;
        IdentityLogged     = false;
        StructScanCount    = 0;
        PrevStructCount    = 0;
        StructClassCount   = 0;
        StructCensusLogged = false;
        TransientBoxCount  = 0;
        TransientSeen      = false;
        TerrRetraceTiles   = 0;
        // Census belongs to the map that produced it, so print it before it is thrown away -
        // a mission that ends before the map completes would otherwise never report one.
        if (!HitCensusLogged) LogHitCensus("cache cleared");
        TerrVolumeFallback = 0;
        TerrDecoGround = 0;
        TerrDecoRock = 0;      // rockdeco1
        DecoComps.clear();
        DecoMeshOffs.clear();
        HitClassCount   = 0;
        HitCensusLogged = false;
        TerrHitsRejected = 0;
        LogInfo("terrain: cache cleared (%s) - generation %u", why, Terr->H.Generation);
    }

    float TileDistCm(int32_t tx, int32_t ty, const FVector3f& p) const {
        const float dx = (tx + 0.5f) * TERR_TILE_CM - p.X;
        const float dy = (ty + 0.5f) * TERR_TILE_CM - p.Y;
        return std::sqrt(dx * dx + dy * dy);
    }

    // Does this tile contain ANY ground? Five traces instead of 1024. See kProbePoints.
    bool TileHasGround(API::UObject* pawn, int32_t tx, int32_t ty, float refZ) {
        static const float fx[kProbePoints] = {0.08f, 0.92f, 0.08f, 0.92f, 0.50f};
        static const float fy[kProbePoints] = {0.08f, 0.08f, 0.92f, 0.92f, 0.50f};
        for (int i = 0; i < kProbePoints; ++i) {
            const float x = (tx + fx[i]) * TERR_TILE_CM;
            const float y = (ty + fy[i]) * TERR_TILE_CM;
            float z = 0.0f;
            if (GroundAt(pawn, x, y, refZ, z)) return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------------------
    // FIND THE SLOT HOLDING THIS TILE, OR MAKE ONE.
    //
    // In v1 this was the hot path and eviction was the whole story, because 25 slots could not
    // hold a map. With 512 slots it is normally a straight claim into an empty slot and the
    // eviction branch never runs at all - it survives for a map larger than the cache.
    //
    // The eviction rule is the one the v1 field log proved necessary: protect a RADIUS around
    // the mech, rather than requiring the victim to beat the candidate by a distance margin.
    // Those logs are worth keeping in mind -
    //     evicted tile (-6,2) at 1672 m for (-6,-3) at 1671 m
    //     evicted tile (-4,-2) at 1714 m for (1,-2) at 1713 m
    // - a one-metre difference. Any margin rule refuses those and the map silently stops
    // growing while the eviction code looks like it is working.
    // -------------------------------------------------------------------------------------
    int TerrainSlotFor(int32_t tx, int32_t ty, const FVector3f& ploc, int32_t ptx, int32_t pty) {
        const int cell = TerrCellIndex(tx, ty);
        if (cell >= 0 && TerrSlotOf[cell] >= 0) return TerrSlotOf[cell];

        int   firstEmpty = -1;
        int   victim     = -1;
        float victimD    = -1.0f;

        for (int i = 0; i < TERR_MAX_TILES; ++i) {
            const TerrainSlotInfo& s = Terr->H.Slots[i];
            if (s.State == TTS_Empty) {
                if (firstEmpty < 0) firstEmpty = i;
                continue;
            }
            const int32_t adx = (s.TileX - ptx) < 0 ? -(s.TileX - ptx) : (s.TileX - ptx);
            const int32_t ady = (s.TileY - pty) < 0 ? -(s.TileY - pty) : (s.TileY - pty);
            if ((adx > ady ? adx : ady) <= kTerrKeepRadius) continue;  // protected

            const float d = TileDistCm(s.TileX, s.TileY, ploc);
            if (d > victimD) { victimD = d; victim = i; }
        }

        int slot = firstEmpty;
        if (slot < 0) {
            if (victim < 0) return -1;     // everything held is close by; nothing to give up
            slot = victim;
            ++TerrEvictions;

            // The evicted tile goes back to Unknown, not OffMap: it had ground, we are simply
            // no longer storing it, and if the pilot returns it must be re-claimable.
            const int oldCell = TerrCellIndex(Terr->H.Slots[slot].TileX, Terr->H.Slots[slot].TileY);
            if (oldCell >= 0) { TerrCell[oldCell] = TC_Unknown; TerrSlotOf[oldCell] = -1; }
            if (Terr->H.Claimed) --Terr->H.Claimed;
            // Complete only ever counted tiles that actually had ground, so only those decrement.
            if (Terr->H.Slots[slot].State == TTS_Complete && Terr->H.Slots[slot].Hits > 0 &&
                Terr->H.Complete) --Terr->H.Complete;

            const ULONGLONG now = GetTickCount64();
            if (now - TerrEvictLogTick > 2000) {
                TerrEvictLogTick = now;
                LogInfo("terrain: cache full at %d tiles - evicted (%d,%d) at %.0f m for (%d,%d) "
                        "[%u evictions this generation]",
                        TERR_MAX_TILES, Terr->H.Slots[slot].TileX, Terr->H.Slots[slot].TileY,
                        victimD / 100.0f, tx, ty, TerrEvictions);
            }
        }

        // ORDER MATTERS, and it is the reverse of what looks natural. The slot table is stored
        // FIRST, so the moment anything touches the heights the slot already reads TTS_Filling
        // with its new coordinates. On the eviction path a consumer may be part-way through
        // reading these very heights; its post-read re-check of the slot is what saves it, and
        // that check only works if the slot was invalidated before the data was.
        ++Terr->H.FrameCounter;
        TerrainSlotInfo& info = Terr->H.Slots[slot];
        info.TileX = tx;
        info.TileY = ty;
        info.Hits  = 0;
        info.State = TTS_Filling;
        ++Terr->H.Claimed;
        Terr->H.FrameCounterEnd = Terr->H.FrameCounter;

        TerrFillCursor[slot] = 0;
        for (int s = 0; s < TERR_SAMPLES * TERR_SAMPLES; ++s) Terr->Tiles[slot].Height[s] = TERR_NO_HEIGHT;

        if (cell >= 0) { TerrCell[cell] = TC_Claimed; TerrSlotOf[cell] = (int16_t)slot; }
        return slot;
    }

    // -------------------------------------------------------------------------------------
    // EXPLORATION. Sweep outward from the player, claiming any tile that has ground and is not
    // already held, until the per-probe claim budget is spent or there is nothing left to find.
    //
    // Sweeping from the PLAYER rather than the anchor is what makes the near field appear first
    // no matter where you walk, while TerrScanRing - which only ever grows - is what stops the
    // sweep re-walking thousands of settled coordinates every probe once the map is done.
    // -------------------------------------------------------------------------------------
    void TerrainExplore(API::UObject* pawn, const FVector3f& ploc, int32_t ptx, int32_t pty) {
        int claims = 0;
        int probes = 0;
        bool sawCandidate = false;
        bool budgetHit = false;

        for (int ring = 0; ring <= TERR_MAX_RING * 2 && claims < kClaimsPerStep && !budgetHit; ++ring) {
            bool ringHadCandidate = false;
            for (int dy = -ring; dy <= ring && claims < kClaimsPerStep && !budgetHit; ++dy) {
                for (int dx = -ring; dx <= ring && claims < kClaimsPerStep && !budgetHit; ++dx) {
                    const int adx = dx < 0 ? -dx : dx;
                    const int ady = dy < 0 ? -dy : dy;
                    if ((adx > ady ? adx : ady) != ring) continue;   // this ring only

                    const int32_t tx = ptx + dx, ty = pty + dy;
                    const int cell = TerrCellIndex(tx, ty);
                    if (cell < 0) continue;                          // outside the explorable area
                    if (TerrCell[cell] != TC_Unknown) continue;      // claimed, or known empty

                    // Stay within a halo of the ground found so far, measured from the ANCHOR -
                    // otherwise the sweep spends itself proving that empty space is empty.
                    const int32_t aax = (tx - TerrAnchorX) < 0 ? -(tx - TerrAnchorX) : (tx - TerrAnchorX);
                    const int32_t aay = (ty - TerrAnchorY) < 0 ? -(ty - TerrAnchorY) : (ty - TerrAnchorY);
                    if ((aax > aay ? aax : aay) > TerrGroundRing + kExploreHalo) continue;

                    ringHadCandidate = true;
                    sawCandidate = true;

                    if (++probes > kProbesPerStep) { budgetHit = true; break; }

                    if (!TileHasGround(pawn, tx, ty, ploc.Z)) {
                        TerrCell[cell] = TC_OffMap;
                        ++Terr->H.OffMap;
                        continue;
                    }
                    const int32_t gr = (aax > aay ? aax : aay);
                    if (gr > TerrGroundRing) TerrGroundRing = gr;
                    if (TerrainSlotFor(tx, ty, ploc, ptx, pty) >= 0) {
                        ++claims;
                        // Extent grows HERE, not when the tile finishes filling. The five-point
                        // probe has already proved there is ground, and exploration finishes in
                        // about 3 s while filling takes 24 - so extending at completion made the
                        // "exploration complete" line report a 1.5 x 2.0 km map that was really
                        // 12 x 13 tiles. The data was right; the line describing it was not.
                        ++Terr->H.FrameCounter;
                        if (tx < Terr->H.MinTileX) Terr->H.MinTileX = tx;
                        if (tx > Terr->H.MaxTileX) Terr->H.MaxTileX = tx;
                        if (ty < Terr->H.MinTileY) Terr->H.MinTileY = ty;
                        if (ty > Terr->H.MaxTileY) Terr->H.MaxTileY = ty;
                        Terr->H.FrameCounterEnd = Terr->H.FrameCounter;
                    }
                }
            }
            // Nothing left anywhere in this ring and we are past where we have already swept:
            // the map has been walked out to here. Keep going only while rings still offer work.
            if (!ringHadCandidate && ring > TerrScanRing + 2) break;
            if (ringHadCandidate && ring > TerrScanRing) TerrScanRing = ring;
        }

        if (budgetHit) return;   // ran out of probe budget, not out of map

        if (!sawCandidate && TerrExploring) {
            TerrExploring = false;
            ++Terr->H.FrameCounter;
            Terr->H.Exploring = 0;
            Terr->H.FrameCounterEnd = Terr->H.FrameCounter;
            LogInfo("terrain: exploration complete - %u tiles with ground, %u rejected as off-map, "
                    "extent X %d..%d, Y %d..%d (%.1f x %.1f km) "
                    "[probes: sky %u, bootstrap %u, raised %u, gaveup %u, miss %u, rejected %u]",
                    Terr->H.Claimed, Terr->H.OffMap,
                    Terr->H.MinTileX, Terr->H.MaxTileX, Terr->H.MinTileY, Terr->H.MaxTileY,
                    (Terr->H.MaxTileX - Terr->H.MinTileX + 1) * TERR_TILE_CM / 100000.0f,
                    (Terr->H.MaxTileY - Terr->H.MinTileY + 1) * TERR_TILE_CM / 100000.0f,
                    TerrDownHits, TerrUpHits, TerrBootstrapRaised, TerrUnderFallback,
                    TerrMisses, TerrHitsRejected);
        } else if (sawCandidate && !TerrExploring) {
            TerrExploring = true;
            ++Terr->H.FrameCounter;
            Terr->H.Exploring = 1;
            Terr->H.FrameCounterEnd = Terr->H.FrameCounter;
        }
    }

    // One slice of work per probe. Game thread only - this calls ProcessEvent.
    void TerrainStep(API::UObject* pawn, const FVector3f& ploc, const FRotator3f& prot) {
        if (!Terr) return;
        if (!pawn) {                                   // no pawn: not in a mission
            if (TerrPawn) TerrainReset(nullptr, "left the mission");
            return;
        }
        if (pawn != TerrPawn) TerrainReset(pawn, "pawn changed");
        if (!EnsureTraceLayout()) return;

        // Live altitude, measured BEFORE the seqlock opens. A trace is ~3.4 us, and holding the
        // lock across it would make readers retry for no reason - the lock should cover the
        // stores, not the work that produces them.
        float gz = 0.0f;
        OriginAboveGroundCm = GroundAt(pawn, ploc.X, ploc.Y, ploc.Z, gz) ? (ploc.Z - gz) : -1.0f;

        const int32_t ptx = (int32_t)std::floor(ploc.X / TERR_TILE_CM);
        const int32_t pty = (int32_t)std::floor(ploc.Y / TERR_TILE_CM);

        if (TerrAnchored && !TerrRangeWarned && TerrCellIndex(ptx, pty) < 0) {
            TerrRangeWarned = true;
            LogInfo("terrain: mech is %d tiles from the anchor, past the %d-tile explore radius - "
                    "no new ground will be traced out here",
                    (ptx - TerrAnchorX) > (pty - TerrAnchorY) ? (ptx - TerrAnchorX) : (pty - TerrAnchorY),
                    TERR_MAX_RING);
        }

        if (!TerrAnchored) {
            TerrAnchorX  = ptx;
            TerrAnchorY  = pty;
            TerrAnchored = true;
            for (int i = 0; i < TERR_GRID_SIDE * TERR_GRID_SIDE; ++i) TerrSlotOf[i] = -1;
            Terr->H.AnchorTileX = ptx;
            Terr->H.AnchorTileY = pty;
            Terr->H.MinTileX = Terr->H.MaxTileX = ptx;
            Terr->H.MinTileY = Terr->H.MaxTileY = pty;
            LogInfo("terrain: anchored at tile (%d,%d) - exploring up to %d tiles out (%.1f km)",
                    ptx, pty, TERR_MAX_RING, TERR_MAX_RING * TERR_TILE_CM / 100000.0f);
        }

        // Header: cheap, every probe, so the consumer's marker and camera track the mech.
        ++Terr->H.FrameCounter;
        Terr->H.PlayerX = ploc.X;
        Terr->H.PlayerY = ploc.Y;
        Terr->H.PlayerZ = ploc.Z;
        Terr->H.OriginAboveGroundCm = OriginAboveGroundCm;
        Terr->H.PlayerYaw = prot.Yaw;
        Terr->H.PlayerTorsoYaw = 0.0f; // body heading is what the map needs; torso is the scope's
        Terr->H.FrameCounterEnd = Terr->H.FrameCounter;

        SlowT.Step("terrain trace");
        ResolveDestructibleClass();
        SlowT.Step("class resolve");
        // nostruct1 (2026-10-08): the structure scan is GONE (user). Nothing was published from it (kPublishStructures
        // off); it only re-traced terrain under a destroyed building, and it cost 3.5-4.5 ms on the game thread every
        // 5 s (1,614 actors, 1,500 of them meshless) - a likely hitch. Consumers see "scanned, none".
        if (Terr && !Terr->H.StructuresScanned) {
            ++Terr->H.FrameCounter;
            Terr->H.StructureCount = 0;
            Terr->H.StructuresScanned = 1;
            Terr->H.FrameCounterEnd = Terr->H.FrameCounter;
        }
        ScanSilhouettes(pawn, ploc);
        SlowT.Step("silhouettes");
        ScanMechInfo(pawn, ploc);
        SlowT.Step("mech records + dropships");

        // One identity line per mission, on the player's own mech - enough to prove the two
        // reads still work after a game patch without reinstating a probe pass.
        LogIdentityOnce(pawn);

        TerrainExplore(pawn, ploc, ptx, pty);

        // Fill the nearest incomplete tile. One tile per step keeps the work contiguous, which
        // is friendlier to the physics broadphase than scattering samples across tiles.
        int   best = -1;
        float bestD = 0.0f;
        for (int i = 0; i < TERR_MAX_TILES; ++i) {
            const TerrainSlotInfo& s = Terr->H.Slots[i];
            if (s.State != TTS_Filling) continue;
            const float d = TileDistCm(s.TileX, s.TileY, ploc);
            if (best < 0 || d < bestD) { best = i; bestD = d; }
        }
        if (best < 0) {
            // Everything claimed is complete: the map is done, and this is the moment the
            // census is worth printing - every class the terrain rays will ever hit on this
            // map has been seen by now.
            if (!HitCensusLogged && !TerrExploring) {
                HitCensusLogged = true;
                LogHitCensus("map complete");
            }
            return;
        }

        TerrainSlotInfo& info = Terr->H.Slots[best];
        TerrainTile&     t    = Terr->Tiles[best];
        const int total = TERR_SAMPLES * TERR_SAMPLES;
        if (TerrFillCursor[best] == 0) {
            TerrUpBase = TerrUpHits; TerrDownBase = TerrDownHits; TerrMissBase = TerrMisses;
            TerrUnderBase = TerrUnderFallback; TerrRejectBase = TerrHitsRejected;
        }
        // kTracesPerStep is now a genuine TRACE budget, not a sample count. It used to be the
        // latter, which worked while a sample cost one trace and stopped working the moment a
        // rejected hit could cost five: a tile under a dropship would have run five times the
        // intended game-thread time in a single tick. Spending the budget instead means a bad
        // neighbourhood fills more slowly and no frame ever spikes.
        int budget = kTracesPerStep;
        int end = total;

        for (int i = TerrFillCursor[best]; i < end; ++i) {
            if (budget <= 0) { end = i; break; }
            const int col = i % TERR_SAMPLES;
            const float x = info.TileX * TERR_TILE_CM + col * TERR_SPACING_CM + TERR_SPACING_CM * 0.5f;
            const float y = info.TileY * TERR_TILE_CM + (i / TERR_SAMPLES) * TERR_SPACING_CM + TERR_SPACING_CM * 0.5f;

            // The hint is the sample to the WEST, 16 m away, which is as good an estimate of
            // local ground as exists. Dropped at the start of each row, where the previous
            // sample is 512 m away at the far edge of the tile and would be a poor guess.
            float hint = TERR_NO_HEIGHT;
            if (col > 0 && i > 0) hint = t.Height[i - 1];

            float z = 0.0f;
            int used = 0;
            t.Height[i] = GroundAt(pawn, x, y, ploc.Z, z, hint, &used) ? z : TERR_NO_HEIGHT;
            budget -= (used > 0) ? used : 1;
        }
        TerrFillCursor[best] = end;

        if (end >= total) {
            // Before the statistics, and before the slot is published: a filled sample is a
            // real height as far as everything downstream is concerned, so it must be in the
            // elevation range and the hit count, not bolted on afterwards.
            const uint32_t filled = FillInteriorHoles(t);

            float zMin = 1e9f, zMax = -1e9f;
            uint32_t hits = 0;
            for (int i = 0; i < total; ++i) {
                if (t.Height[i] == TERR_NO_HEIGHT) continue;
                ++hits;
                if (t.Height[i] < zMin) zMin = t.Height[i];
                if (t.Height[i] > zMax) zMax = t.Height[i];
            }

            // Every height is in before State moves: that store is the release the consumer
            // relies on, so it must be last inside the lock.
            ++Terr->H.FrameCounter;
            info.Hits  = hits;
            if (hits > 0) ++Terr->H.Complete;   // extent was already grown at claim time
            info.State = TTS_Complete;
            Terr->H.FrameCounterEnd = Terr->H.FrameCounter;

            // Progress, not per tile. At 500-odd tiles a line each would be a wall of log; what
            // is worth knowing is the rate and whether it is still finding ground.
            // ...EXCEPT a tile that published any underside reading. Those are the ones the
            // canyon hunt is about, they are rare (zero on healthy ground), and a 5-second
            // throttle would swallow most of them - a diagnostic you can only see if it happens
            // to land on the right tick is not a diagnostic. Suspect tiles always print.
            const uint32_t underThisTile = TerrUnderFallback - TerrUnderBase;
            const ULONGLONG now = GetTickCount64();
            if (underThisTile > 0 || now - TerrProgressTick > 5000 || Terr->H.Complete <= 1) {
                TerrProgressTick = now;
                // Counter names kept, meanings inverted: the downward ray is the primary now
                // and the upward one is the bootstrap. Logged in the new order so the line does
                // not quietly describe the previous design.
                LogInfo("terrain: %u tiles complete, %u claimed, %u off-map%s - last (%d,%d) "
                        "%u/%d sampled, Z %.1f..%.1f m (relief %.1f) "
                        "(sky %u, bootstrap %u, gaveup %u, miss %u, rejected %u, filled %u)",
                        Terr->H.Complete, Terr->H.Claimed, Terr->H.OffMap,
                        TerrExploring ? ", still exploring" : ", exploration done",
                        info.TileX, info.TileY, hits, total,
                        // ABSOLUTE heights, not just the spread. Six tiles kilometres apart all
                        // reported exactly 146.0 m of relief, which is not something terrain
                        // does - it means zMin and zMax are landing on the same two fixed
                        // surfaces map-wide. The difference cannot say WHICH surfaces; the
                        // endpoints can, and if a flat plane is being hit its Z will be sitting
                        // here identically on every affected tile. Guarded on hits because an
                        // all-holes tile leaves the sentinels at +/-1e9.
                        (hits > 0) ? zMin / 100.0f : 0.0f,
                        (hits > 0) ? zMax / 100.0f : 0.0f,
                        (hits > 1) ? (zMax - zMin) / 100.0f : 0.0f,
                        TerrDownHits - TerrDownBase, TerrUpHits - TerrUpBase,
                        TerrUnderFallback - TerrUnderBase, TerrMisses - TerrMissBase,
                        TerrHitsRejected - TerrRejectBase, filled);
            }
        }
    }


    // Resolve UGameplayStatics CDO + GetAllActorsOfClass once (cached).
    void ResolveGameplayStatics() {
        if (GsCdo && GaacFn) return;
        auto* gs = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.GameplayStatics");
        if (!gs) return;
        GsCdo  = gs->get_class_default_object();
        GaacFn = gs->find_function(L"GetAllActorsOfClass");
        static bool logged = false;
        if (!logged) { logged = true; LogInfo("GAAC resolve: cdo=%p fn=%p", (void*)GsCdo, (void*)GaacFn); }
    }

    // Live world actors of a class (excludes CDOs), via GetAllActorsOfClass - far cheaper
    // than scanning the global UObject array. Returns a pointer array + count in the
    // REUSED GaacBuf: copy the pointers out before the next call (each call overwrites it).
    API::UObject** GetActorsOfClass(API::UObject* worldCtx, UEVR_UClassHandle cls, int& outCount) {
        outCount = 0;
        if (!GsCdo || !GaacFn || !cls || !worldCtx) return nullptr;
        struct Params { void* World; void* ActorClass; FScriptArrayLayout Out; } params{};
        params.World      = static_cast<void*>(worldCtx);
        params.ActorClass = reinterpret_cast<void*>(cls);
        params.Out        = GaacBuf;              // reuse existing allocation
        GsCdo->process_event(GaacFn, &params);
        GaacBuf  = params.Out;                    // capture the (possibly grown) buffer
        outCount = params.Out.Num;
        return reinterpret_cast<API::UObject**>(params.Out.Data);
    }

    // Read a bool UPROPERTY correctly. Native UE bools are often packed bitfields
    // (several share one byte), so a raw byte != 0 test is WRONG - it trips on
    // unrelated bits. FBoolProperty::get_value_from_object applies the real bitmask.
    bool ReadBoolProp(API::UObject* o, const wchar_t* name) {
        auto* cls = o->get_class();
        if (!cls) return false;
        auto* prop = cls->find_property(name);
        if (!prop) return false; // property absent on this class - null-safe
        auto* fc = prop->get_class();
        if (fc && fc->get_name() == L"BoolProperty")
            return reinterpret_cast<API::FBoolProperty*>(prop)
                       ->get_value_from_object(reinterpret_cast<void*>(o));
        if (const auto* p = o->get_property_data<uint8_t>(name)) return *p != 0;
        return false;
    }

    // Read an actor's world position + facing yaw WITHOUT a ProcessEvent, from its root
    // component's RelativeLocation / RelativeRotation (both reflected UPROPERTYs; HeadAim
    // reads RelativeRotation the same way). For an unattached root - which mechs, vehicles
    // and projectiles all have - relative == world. This replaces K2_GetActorLocation +
    // K2_GetActorRotation (two ProcessEvents per actor); in bulk on the 3 s scan frame that
    // burst of hooked dispatches was hitching the game thread once per pulse. Returns false
    // if the root/props aren't readable, so the caller skips the actor.
    // ATTACHED ROOTS (turretpose1, 2026-10-08). The relative read below is the world position only for an
    // UNATTACHED root. Turrets hang on a foundation - PopupTurretFoundation_C / TowerTurretFoundation_C, by
    // its AttachPoint, two levels down - and dropship turrets on the Leopard, so their RelativeLocation is an
    // offset from the parent (the pose1 probe, 2026-09-29: a dropship turret read (-6081, 0, 1421) while the
    // engine put it at (-192006, -117112, ...); a foundation turret read about (0, 0, 0) - drawn underground
    // near the map origin). For those few the engine is asked (two ProcessEvents, as MWVR and the game's own
    // map get it right); every mech / tank / VTOL keeps the cheap read. True depth: a popup turret still
    // retracted is reported below its hatch, and follows the AttachPoint up as it rises.
    // THE GAME'S OWN DEATH FLAG (deadflag1, 2026-10-08). Every unit's health is an MWLivingActorComponent -
    // MWMech.MechLivingActorComponent, MWTank / MWTurret / MWVTOL .SimpleLivingActorComponent - and its replicated
    // bIsDead (OnRep_bIsDead) is set on every kind of death: cored, head, both legs, ammo, ejection. The game's own
    // markers follow it (MWTargetComponent.OnLivingActorDeath / UpdateIsOwnerDead). The old tests missed a legged
    // mech (bIsPilotDead / bCenterTorsoDestroyed only) and every dead vehicle until it despawned
    // (bActorIsBeingDestroyed), so wrecks lingered on the scope, the maps and in the callouts. A property read (no
    // ProcessEvent); the old tests stay as the fallback; once dead, latched (keyed actor + class).
    // bugsweep2 (2026-10-18, sweep #4 finished): the latch is trusted only while the unit is checked continuously (every
    // scan asks for every listed unit, several times a second). A unit destroyed and freed mid-mission stops being listed;
    // a new unit of the same class that the allocator puts at the same address arrives after that gap - it is then
    // checked afresh instead of being reported dead (and hidden) for the rest of the mission.
    struct DeadMark { void* Cls; ULONGLONG Ms; };
    static constexpr ULONGLONG kDeadLatchGapMs = 1500;
    std::unordered_map<void*, DeadMark> DeadLatch;
    bool DeadFlagWarned = false;

    // THE TEAM BYTE (ba3, user 2026-10-17): "TeamID" on every unit - mechs, vehicles, the player and a battle armour
    // suit. The exe spells the suit's "TeamId", but shipping FNames are case-insensitive (first spelling registered
    // wins), so it reads "TeamID" at runtime too - ba2's "TeamId" fallback removed. First byte = the team.
    static const uint8_t* TeamByte(API::UObject* a) {
        if (!a) return nullptr;
        return a->get_property_data<uint8_t>(L"TeamID");
    }

    // OBJECT CHECK (objcheck1, user 2026-10-17; LanceControl had it crash twice, lancecontrol-62/63): a unit's living
    // component can already be freed while the unit itself is still an object (a unit being torn down at its death) -
    // reading its class through the dangling pointer crashed UEVR. The engine's own object table answers it: the
    // object's InternalIndex (UObjectBase + 0x0C in UE4) must give this same object back. The read is under SEH (no C++
    // object in that frame). Self-tested once on your pawn: if the table does not give it back the check switches itself
    // off (the old behaviour) and says so in the log. Existence only - no pending-kill flag test.
    static bool ObjIndexRead(API::UObject* o, int32_t& idx) {
        __try {
            idx = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(o) + 0x0C);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    int ObjCheckState = 0; // 0 not tested yet, 1 on, -1 off (self-test failed)

    bool ObjStillThere(API::UObject* o) {
        if (!o) return false;
        auto*      sdk = API::get()->param()->sdk;
        const auto arr = sdk->functions->get_uobject_array();
        if (!arr) return true;
        const int count = sdk->uobject_array->get_object_count(arr);
        if (ObjCheckState == 0) {
            auto* me = reinterpret_cast<API::UObject*>(API::get()->get_local_pawn(0));
            if (!me) return true; // tested once there is a pawn
            int32_t mi = -1;
            const bool ok = ObjIndexRead(me, mi) && mi >= 0 && mi < count &&
                            reinterpret_cast<API::UObject*>(sdk->uobject_array->get_object(arr, mi)) == me;
            ObjCheckState = ok ? 1 : -1;
            if (ok) LogInfo("object check: on (your pawn found in the object table at index %d)", mi);
            else    LogWarn("object check: OFF - your pawn was not at its object index (read %d) - freed components are not caught", mi);
        }
        if (ObjCheckState < 0) return true;
        int32_t i = -1;
        if (!ObjIndexRead(o, i) || i < 0 || i >= count) return false;
        return reinterpret_cast<API::UObject*>(sdk->uobject_array->get_object(arr, i)) == o;
    }

    bool UnitDead(API::UObject* a, bool isMech) {
        if (!a || !a->get_class()) return true;
        const ULONGLONG nowMs = GetTickCount64();
        auto it = DeadLatch.find(static_cast<void*>(a));
        if (it != DeadLatch.end()) {
            if (it->second.Cls == static_cast<void*>(a->get_class()) && nowMs - it->second.Ms <= kDeadLatchGapMs) {
                it->second.Ms = nowMs; // still listed: still the same dead unit
                return true;
            }
            DeadLatch.erase(it); // another class, or a gap: maybe another unit now - checked afresh below
        }
        bool dead = false, known = false;
        // ba1: a battle armour suit's living component may sit under the generic name - tried last
        for (const wchar_t* cn : { L"MechLivingActorComponent", L"SimpleLivingActorComponent", L"LivingActorComponent" }) {
            const auto* pp = a->get_property_data<void*>(cn);
            if (!pp || !*pp) continue;
            auto* lc = reinterpret_cast<API::UObject*>(*pp);
            if (!ObjStillThere(lc)) continue; // objcheck1: freed during the unit's death - the old tests below answer
            if (!lc->get_class() || !lc->get_class()->find_property(L"bIsDead")) continue;
            known = true;
            dead = ReadBoolProp(lc, L"bIsDead");
            break;
        }
        if (!known && !DeadFlagWarned) {
            DeadFlagWarned = true;
            LogWarn("death flag: no living component's bIsDead on %ls - old death test only",
                    a->get_class()->get_fname() ? a->get_class()->get_fname()->to_string().c_str() : L"?");
        }
        if (!dead)
            dead = isMech ? (ReadBoolProp(a, L"bIsPilotDead") || ReadBoolProp(a, L"bCenterTorsoDestroyed"))
                          : ReadBoolProp(a, L"bActorIsBeingDestroyed");
        if (dead) {
            if (DeadLatch.size() > 2048) DeadLatch.clear();
            DeadLatch[static_cast<void*>(a)] = DeadMark{static_cast<void*>(a->get_class()), nowMs};
        }
        return dead;
    }

    // DROPSHIPS (dropship1, TERRAIN v14). MWDropship (Leopard_C < AbstractLeopard_C < MWDropship) plus the DLC3
    // Union blueprints, which are plain actors (UnionDropship_Destructible_C: ~40 parts on a PivotPoint;
    // UnionDropship_Takeoff_C: a skeletal hull 62 m below the root). Published with position, yaw and side so
    // HUDDisplay can draw the hull outline (assets/dropships/*.txt) the turrets sit on. Side: the actor's TeamID
    // when it has one, else the TeamID of a turret attached to it (dropship turrets hang on the Leopard).
    UEVR_UClassHandle UnionDestrClass = nullptr, UnionTakeoffClass = nullptr;
    UEVR_UClassHandle UnionBaseClass = nullptr; // UnionDropship_C, a plain Actor like the DLC3 ones (2026-10-19)
    ULONGLONG UnionLookMs = 0;
    std::unordered_set<void*> DropshipLogged;

    uint32_t DropshipScan(API::UObject* pawn, uint8_t playerTeam, TerrainDropship* out, uint32_t max, ULONGLONG now) {
        if (!pawn) return 0;
        // unionfix1 (2026-10-08): the Union classes are NOT looked up by name any more. find_uobject on a blueprint
        // path that is not loaded walks the whole object array - 700-850 ms on the game thread, and dropship1 retried
        // it every 10 s: the release's 10-second hitch ("slow: LidarExport mech records + dropships 7xx ms"). They are
        // picked up for free instead, the first time a terrain ray lands on one (the hit census, HitClassFor).
        API::UObject* ships[16];
        int sn = 0;
        for (UEVR_UClassHandle cls : { DropshipClass, UnionDestrClass, UnionTakeoffClass, UnionBaseClass }) {
            if (!cls) continue;
            int n = 0;
            API::UObject** list = GetActorsOfClass(pawn, cls, n); // copied out at once: GaacBuf is reused
            for (int i = 0; i < n && sn < 16; ++i) {
                if (!list[i] || IsSpentActor(list[i])) continue;
                bool dup = false;
                for (int k = 0; k < sn; ++k) dup |= ships[k] == list[i];
                if (!dup) ships[sn++] = list[i];
            }
        }
        if (!sn) return 0;
        // turrets attached to a ship tell its side
        std::vector<std::pair<API::UObject*, uint8_t>> turretOwner;
        if (TurretClass) {
            int n = 0;
            API::UObject** list = GetActorsOfClass(pawn, TurretClass, n);
            std::vector<API::UObject*> ts(list, list + (list ? n : 0));
            for (auto* t : ts) {
                if (!t) continue;
                auto* rpp = t->get_property_data<void*>(L"RootComponent");
                if (!rpp || !*rpp) continue;
                const auto* ap = reinterpret_cast<API::UObject*>(*rpp)->get_property_data<void*>(L"AttachParent");
                if (!ap || !*ap) continue;
                uint8_t team = 0xFF;
                if (const auto* tp = TeamByte(t)) team = *tp;
                turretOwner.emplace_back(reinterpret_cast<API::UObject*>(*ap)->get_outer(), team);
            }
        }
        uint32_t w = 0;
        for (int i = 0; i < sn && w < max; ++i) {
            API::UObject* a = ships[i];
            FVector3f loc{}; float yaw = 0.0f;
            if (!ReadActorPose(a, loc, yaw)) continue;
            TerrainDropship& d = out[w];
            std::memset(&d, 0, sizeof(d));
            d.X = loc.X; d.Y = loc.Y; d.Z = loc.Z; d.Yaw = yaw;
            const std::wstring cn = (a->get_class() && a->get_class()->get_fname()) ? a->get_class()->get_fname()->to_string() : L"";
            if (cn.find(L"UnionDropship_Destructible") != std::wstring::npos) d.Kind = TDK_UnionDestructible;
            else if (cn.find(L"UnionDropship_Takeoff") != std::wstring::npos) d.Kind = TDK_UnionTakeoff;
            else if (cn == L"UnionDropship_C") d.Kind = TDK_Union;
            else if (cn.find(L"Broadsword") != std::wstring::npos) d.Kind = TDK_Broadsword; // DLC7 (< AbstractLeopard_C)
            else if (cn.find(L"Leopard") != std::wstring::npos) d.Kind = TDK_Leopard;
            else {
                d.Kind = TDK_Other;
                TerrainStructure b{};
                if (ActorBounds(a, b)) { d.X = b.X; d.Y = b.Y; d.Z = b.Z; d.EX = b.EX; d.EY = b.EY; d.EZ = b.EZ; }
                else { d.EX = d.EY = 4000.0f; d.EZ = 2000.0f; }
            }
            uint8_t team = 0xFF;
            if (const auto* tp = TeamByte(a)) team = *tp;
            for (auto& to : turretOwner)
                if (team == 0xFF && to.first == a) team = to.second;
            d.Side = team == 0xFF ? 0 : (team == playerTeam ? 1 : 2);
            if (DropshipLogged.insert(static_cast<void*>(a)).second && DropshipLogged.size() < 64)
                LogInfo("dropships: %ls kind %u, team %u (%s) at (%.0f, %.0f, %.0f) yaw %.0f", cn.c_str(), (unsigned)d.Kind,
                        (unsigned)team, d.Side == 1 ? "friendly" : d.Side == 2 ? "enemy" : "unknown", d.X, d.Y, d.Z, d.Yaw);
            ++w;
        }
        return w;
    }

    bool ReadActorPose(API::UObject* a, FVector3f& outLoc, float& outYaw) {
        auto* rpp = a->get_property_data<void*>(L"RootComponent");
        if (!rpp || !*rpp) return false;
        auto* root = reinterpret_cast<API::UObject*>(*rpp);
        if (const auto* ap = root->get_property_data<void*>(L"AttachParent"); ap && *ap) {
            FVector3f w{};
            FRotator3f r{};
            a->call_function(L"K2_GetActorLocation", &w);
            a->call_function(L"K2_GetActorRotation", &r);
            if (w.X == 0.0f && w.Y == 0.0f && w.Z == 0.0f) return false; // the call did not land
            outLoc = w;
            outYaw = r.Yaw;
            return true;
        }
        const auto* lp = root->get_property_data<FVector3f>(L"RelativeLocation");
        if (!lp) return false;
        outLoc = *lp;
        if (const auto* rp = root->get_property_data<FRotator3f>(L"RelativeRotation"))
            outYaw = rp->Yaw;
        return true;
    }

    void Gather() {
        if (!Block || !HookReady)
            return;

        // Hard guard: never run Gather re-entrantly or on two threads at once. If it's
        // already running (either thread), bail immediately. RAII clears the flag on every
        // exit path. This alone kills the two-thread data race on the throttle statics and
        // the MMF publish; GameThreadTickAlive additionally keeps the work on the game thread.
        if (GatherActive.exchange(true))
            return;
        struct ActiveGuard {
            std::atomic<bool>& f;
            ~ActiveGuard() { f.store(false, std::memory_order_release); }
        } activeGuard{ GatherActive };

        // Two independent cadences. The player transform (position / heading / torso)
        // publishes at ~5 Hz for responsiveness; the heavy contact array scan runs only
        // every ~3 s and drives the renderer's ping sweep (via ScanCounter). The
        // GatherActive guard above serializes all of this, so the throttle statics and the
        // seqlock publish are only ever touched by one thread at a time.
        const ULONGLONG now = GetTickCount64();
        static ULONGLONG lastFast = 0, lastScan = 0, lastAlly = 0;
        if (now - lastFast < 16) // transform + selection ~60 Hz - cheap, PLAYER-PAWN ONLY
            return;              // (a stable object; safe to read hot, unlike enumeration)
        // Allies re-enumerate via GetAllActorsOfClass, which is far too heavy to run every
        // tick - throttle it to ~10 Hz (smooth enough for lance mechs). Enumeration on the
        // game's render thread at high frequency crashes the game.
        const bool doAlly = (now - lastAlly) >= 100;
        if (doAlly) lastAlly = now;
        lastFast = now;
        const bool doScan = (now - lastScan) >= 3000; // contact scan ~every 3 s
        if (doScan) lastScan = now;
        const bool doLog = doScan; // periodic diagnostics ride the scan cadence

        // Projectiles: a third cadence (~15 Hz), and ONLY when the renderer asks for it
        // via the control bit - the full-array projectile scan is the expensive bit, so
        // it costs nothing while the layer is off.
        UpdateFar(now); // the 5 km radar mode (2026-10-15)
        const bool wantProj = (Block->ControlLayers & LIDAR_CTRL_PROJECTILES) != 0;
        uint32_t projInterval = Block->ControlProjIntervalMs; // renderer-chosen sample rate
        if (projInterval < 16)  projInterval = 33;            // 0/unset -> ~30 Hz default
        if (projInterval > 500) projInterval = 500;           // clamp
        static ULONGLONG lastProj = 0;
        const bool doProj = wantProj && (now - lastProj >= projInterval);
        if (doProj) lastProj = now;

        // The lock probe is a separate, much cheaper cadence: one GetActorsOfClass over
        // live MWMissilePath actors (a handful, one per salvo - not per round) plus a
        // call per salvo. Requesting projectiles implies it, so the scope keeps working
        // exactly as before; requesting THREAT alone gets the warning without the array.
        const bool wantThreat = (Block->ControlLayers & (LIDAR_CTRL_THREAT | LIDAR_CTRL_PROJECTILES)) != 0;
        static ULONGLONG lastThreat = 0;
        // When the array is being gathered, sample the lock on the SAME tick so both
        // describe the same instant in one seqlock body. The renderer attributes a salvo's
        // rounds by their appearing in the frame the lock arms; on two cadences (10 Hz
        // probe vs ~30 Hz array) that edge could be seen up to 100 ms late, by which point
        // the rounds were several publishes old and no longer "new", and the correlation
        // silently finds nothing. Without the array there is nothing to correlate against,
        // so the cheap 10 Hz cadence stands.
        const bool doThreat = wantProj ? doProj
                                       : (wantThreat && (now - lastThreat >= kThreatIntervalMs));
        if (doThreat) lastThreat = now;

        const auto playerPawn = API::get()->get_local_pawn(0);
        if (!playerPawn) {
            if (doLog) LogInfo("no local pawn yet (not piloting a mech?)");
            return;
        }

        // Identify the mech the way HUD.dll does: by checking the pawn has a
        // MechViewComponent (robust against the transient DefaultPawn). Only
        // re-evaluate when the pawn actually changes (HUD's OnNewPawn pattern), so we
        // don't repeat the property lookup every frame.
        static API::UObject* lastPawn = nullptr;
        if (NewWorld()) { // bugsweep1
            LogInfo("new world (controller, level or world clock changed) - death latch, address caches and terrain reset");
            DeadLatch.clear();
            DeathSeenMs.clear();
            TerrainReset(playerPawn, "new world");
            lastPawn = nullptr; // the class verdicts and the address caches go too (just below)
        }
        if (playerPawn != lastPawn) {
            lastPawn = playerPawn;
            ResetClassVerdicts();
            API::UObject* mechViewC = nullptr;
            if (TryGetProperty(playerPawn, L"MechViewComponent", mechViewC)) {
                MechClass = playerPawn->get_class();
                // Walk up the class chain to the native MWMech base, so enumeration
                // catches EVERY mech subclass (allies share our class; enemies use a
                // different MWMech subclass).
                auto* sdk     = API::get()->param()->sdk;
                BaseMechClass = reinterpret_cast<UEVR_UClassHandle>(MechClass); // fallback
                auto    s     = reinterpret_cast<UEVR_UStructHandle>(MechClass);
                wchar_t nb[128];
                PawnClass = nullptr; ActorClass = nullptr;
                for (int d = 0; s && d < 20; ++d) {
                    nb[0] = 0;
                    sdk->fname->to_string(sdk->uobject->get_fname(reinterpret_cast<UEVR_UObjectHandle>(s)), nb, 128);
                    if (wcscmp(nb, L"MWMech") == 0)
                        BaseMechClass = reinterpret_cast<UEVR_UClassHandle>(s);
                    if (wcscmp(nb, L"Pawn") == 0)
                        PawnClass = reinterpret_cast<UEVR_UClassHandle>(s);
                    if (wcscmp(nb, L"Actor") == 0) {
                        ActorClass = reinterpret_cast<UEVR_UClassHandle>(s);
                        break; // Actor sits above Pawn; all three captured, stop here
                    }
                    s = sdk->ustruct->get_super_struct(s);
                }
                LogInfo("In mech: class=%s, enumerating base=%s",
                        WideToNarrow(MechClass->get_fname()->to_string().c_str()).c_str(),
                        WideToNarrow(reinterpret_cast<API::UClass*>(BaseMechClass)->get_fname()->to_string().c_str()).c_str());

                // Cache the torso-twist component (for the aim line on the scan).
                TorsoTwistC = nullptr;
                TryGetProperty(playerPawn, L"TorsoTwistComponent", TorsoTwistC);

                // Cache the TargetTracking component (holds the target slots), as
                // HeadAim does, and enumerate its live-target pointer fields so the
                // highlight covers BOTH enemy and ally selections (they use separate
                // slots). We read the real field names rather than guessing.
                TargetTrackingC = nullptr;
                TargetFields.clear();
                if (TryGetProperty(playerPawn, L"TargetTracking", TargetTrackingC) && TargetTrackingC)
                    CollectTargetFields(TargetTrackingC);
            } else {
                MechClass       = nullptr;
                BaseMechClass   = nullptr;
                TorsoTwistC     = nullptr;
                TargetTrackingC = nullptr;
                LogInfo("pawn is not a mech (no MechViewComponent), class=%s",
                        WideToNarrow(playerPawn->get_class()->get_fname()->to_string().c_str()).c_str());
            }
        }
        if (!MechClass || !BaseMechClass)
            return;

        SlowT.Begin(); // SLOW-STEP TIMER (slowtimer1)
        // --- gather into a local buffer (NO seqlock held; this is the slow part) ---
        FVector3f  ploc{};
        FRotator3f prot{};
        playerPawn->call_function(L"K2_GetActorLocation", &ploc);
        playerPawn->call_function(L"K2_GetActorRotation", &prot);

        // One-shot terrain probe. Runs here because this is the GAME thread (reached via the
        // Blueprint tick detours) and ProcessEvent must not be called from the render thread -
        // doing so is what produced the access violation in a resizing actor array. Latches
        // itself, so after the first pass this line costs a single bool test.
        TerrainProbe(playerPawn, ploc);
        TerrainTimingProbe(playerPawn, ploc);
        TerrainStep(playerPawn, ploc, prot);
        SlowT.Step("terrain other");

        // Torso twist yaw (relative to the body) for the aim line. Part of the
        // transform, so it's read every fast tick and tracks at ~40 Hz.
        float torsoYaw = 0.0f;
        if (TorsoTwistC) {
            if (const auto* tw = TorsoTwistC->get_property_data<FVector2f>(L"TorsoTwist"))
                torsoYaw = tw->X;
        }

        // Selection is decoupled from the 3 s scan: every fast tick, map the live target
        // pointers to their stable contact ids (via IdMap, populated by scans) so the
        // renderer can move the highlight at transform rate.
        uint16_t selIds[4];
        uint8_t  selCount = 0;
        if (TargetTrackingC) {
            for (const auto& fname : TargetFields) {
                if (selCount >= 4) break;
                auto* pp = TargetTrackingC->get_property_data<void*>(fname.c_str());
                if (!pp || !*pp) continue;
                auto it = IdMap.find(*pp);
                if (it == IdMap.end()) continue;
                bool dup = false;
                for (uint8_t k = 0; k < selCount; ++k)
                    if (selIds[k] == it->second) { dup = true; break; }
                if (!dup) selIds[selCount++] = it->second;
            }
        }

        // --- allies: re-enumerate FRESH via GetAllActorsOfClass, but only every ~100 ms
        // (doAlly) - the enumeration is heavy and must NOT run every tick. Results are
        // value-copied into AllyBuf and republished every tick. We never hold actor
        // pointers across ticks (GAAC returns only live actors this frame). ---
        if (doAlly) {
            AllyBufCount = 0;
            ResolveGameplayStatics();
            uint8_t myTeam = 0xFF;
            if (const auto* p = TeamByte(playerPawn)) myTeam = *p;
            const struct { UEVR_UClassHandle cls; uint8_t kind; } abases[5] = {
                { BaseMechClass, LK_Mech }, { TankClass, LK_Tank },
                { TurretClass, LK_Turret }, { VtolClass, LK_Vtol }, { BaClass, LK_BattleArmor } // ba1: suits
            };
            for (int b = 0; b < 5 && AllyBufCount < LIDAR_MAX_ALLIES; ++b) {
                if (!abases[b].cls) continue;
                int n = 0;
                API::UObject** list = GetActorsOfClass(playerPawn, abases[b].cls, n);
                if (!list) continue; // process each base fully before the next GAAC reuses GaacBuf
                for (int i = 0; i < n && AllyBufCount < LIDAR_MAX_ALLIES; ++i) {
                    auto* am = list[i];
                    if (!am || am == playerPawn) continue;
                    uint8_t t = 0xFF;
                    if (const auto* p = TeamByte(am)) t = *p;
                    if (t != myTeam) continue; // allies only; enemies ride the 3 s scan
                    if (UnitDead(am, abases[b].kind == LK_Mech)) // deadflag1 (vehicles too now)
                        continue;
                    FVector3f aloc{}; float ayaw = 0.0f;
                    if (!ReadActorPose(am, aloc, ayaw)) continue; // no ProcessEvent (see helper)
                    const float adx = aloc.X - ploc.X, ady = aloc.Y - ploc.Y, adz = aloc.Z - ploc.Z;
                    if (adx * adx + ady * ady + adz * adz > RangeNowCm() * RangeNowCm()) continue; // 5 km mode: far
                    if (adz < -kUndergroundCm) continue;
                    uint16_t aid;
                    aid = AssignId(static_cast<void*>(am));
                    uint8_t aw = LW_None;
                    if (abases[b].kind == LK_Mech)
                        if (const auto* p = am->get_property_data<uint8_t>(L"ComputedWeightClass"))
                            if (*p <= LW_Assault) aw = *p;
                    uint8_t asel = 0;
                    for (uint8_t k = 0; k < selCount; ++k) if (selIds[k] == aid) { asel = 1; break; }
                    LidarContact& c = AllyBuf[AllyBufCount++];
                    c.X = aloc.X; c.Y = aloc.Y; c.Z = aloc.Z;
                    c.Yaw = ayaw;
                    c.Type = LCT_FriendlyMech;
                    c.Team = t;
                    c.Selected = asel;
                    c.Kind = abases[b].kind;
                    c.Weight = aw;
                    c.Id = aid;
                }
            }
        }

        // Contacts are refreshed only on scan ticks (~3 s). local[]/outCount are filled
        // inside and published below; between scans the block keeps its last contacts.
        LidarContact local[LIDAR_MAX_CONTACTS];
        uint32_t outCount    = 0;
        int      matched     = 0;
        int      total       = 0;
        size_t   targetCount = 0;
        if (doScan) {
            // Player team byte (discriminator; raw compare against each contact).
            uint8_t playerTeam = 0xFF;
            if (const auto* p = TeamByte(playerPawn))
                playerTeam = *p;

            // Player's live targets (enemy + ally) from the TargetTracking slots; a
            // contact matching any of these gets the on-scan highlight.
            void* targets[8];
            if (TargetTrackingC) {
                for (const auto& fname : TargetFields) {
                    if (targetCount >= 8) break;
                    if (auto* pp = TargetTrackingC->get_property_data<void*>(fname.c_str()))
                        if (*pp) targets[targetCount++] = *pp;
                }
            }

            // Resolve tank/turret/VTOL bases (walk the global array once, until resolved
            // - bounded and self-terminating) + GameplayStatics for the enumeration.
            ResolveVehicleClasses();
            ResolveGameplayStatics();

            // Enumerate cheaply: ask the engine for just the actors of each base class
            // (live world actors, no CDOs) instead of sweeping ~400k global objects. Copy
            // the returned pointers out immediately - each GetActorsOfClass reuses GaacBuf.
            struct Cand { API::UObject* obj; uint8_t kind; };
            static std::vector<Cand> cands;
            cands.clear();
            const struct { UEVR_UClassHandle cls; uint8_t kind; } cbases[5] = {
                { BaseMechClass, LK_Mech }, { TankClass, LK_Tank },
                { TurretClass, LK_Turret }, { VtolClass, LK_Vtol }, { BaClass, LK_BattleArmor } // ba1: suits
            };
            for (int b = 0; b < 5; ++b) {
                if (!cbases[b].cls) continue;
                int n = 0;
                API::UObject** list = GetActorsOfClass(playerPawn, cbases[b].cls, n);
                if (!list) continue;
                total += n;
                for (int i = 0; i < n; ++i)
                    if (list[i]) cands.push_back({ list[i], cbases[b].kind });
            }

            for (const auto& cand : cands) {
            if (outCount >= LIDAR_MAX_CONTACTS) break;
            auto*         mech = cand.obj;
            const auto    obj  = mech;        // alias for the later void* casts / id
            const uint8_t kind = cand.kind;
            ++matched;
            if (mech == playerPawn)
                continue;

            FVector3f loc{}; float yaw = 0.0f;
            if (!ReadActorPose(mech, loc, yaw)) continue; // pos + yaw, no ProcessEvent (see helper)

            const float dx = loc.X - ploc.X, dy = loc.Y - ploc.Y, dz = loc.Z - ploc.Z;
            if (dx * dx + dy * dy + dz * dz > RangeNowCm() * RangeNowCm())
                continue; // outside scan range (5 km in the 5 km radar mode)

            // Reject "underground phantoms": anything more than 300 m below the player
            // (pooled / not-yet-spawned actors sit far below the map). Real units,
            // including VTOLs which are ABOVE us, are unaffected.
            if (dz < -kUndergroundCm)
                continue;

            // Facing yaw (world) for the orientation triangle was already read via
            // ReadActorPose above (no separate K2_GetActorRotation ProcessEvent).

            // Skip dead units so the scan matches the in-game radar (the wreck actor
            // keeps matching until it sinks). Mech signals are confirmed
            // (bIsPilotDead + bCenterTorsoDestroyed). Vehicle signals are best-effort
            // pending the [TANK]/[TURRET] dump - all reads are null-safe, so a missing
            // property simply doesn't trip. Actor.bActorIsBeingDestroyed catches the
            // despawn for everything as a backstop.
            // deadflag1: the game's own MWLivingActorComponent.bIsDead for every kind (the old tests are its fallback)
            if (UnitDead(mech, kind == LK_Mech))
                continue;

            // Mech weight class, for the dot indicator. The game pre-computes it in
            // MWMech.ComputedWeightClass (EnumProperty, confirmed by the [MECH] dump),
            // so we just read the enum byte: 0=Light, 1=Medium, 2=Heavy, 3=Assault.
            // Always None for vehicles.
            uint8_t weight = LW_None;
            if (kind == LK_Mech) {
                if (const auto* p = mech->get_property_data<uint8_t>(L"ComputedWeightClass"))
                    if (*p <= LW_Assault) weight = *p;
            }

            // Friend/foe by the TeamID byte - works on every mech subclass. Same byte
            // as the player = ally; anything else = hostile.
            uint8_t teamId = 0xFF;
            if (const auto* p = TeamByte(mech))
                teamId = *p;
            const uint8_t type = (teamId == playerTeam)
                                     ? LCT_FriendlyMech : LCT_HostileMech;

            // Stable id: same actor keeps the same id across scans, so the renderer
            // can carry a target's last-known blip / fade until the next ping updates it.
            uint16_t id;
            id = AssignId(static_cast<void*>(obj));

            // Allies are handled on the fast path (fresh GAAC enumeration); the 3 s
            // contact list is enemies only (revealed + faded by the ping).
            if (type == LCT_FriendlyMech)
                continue;

            uint8_t selected = 0;
            for (size_t t = 0; t < targetCount; ++t)
                if (static_cast<void*>(obj) == targets[t]) { selected = 1; break; }

            LidarContact& c = local[outCount++];
            c.X = loc.X; c.Y = loc.Y; c.Z = loc.Z;
            c.Yaw = yaw;
            c.Type = type;
            c.Team = teamId;
            c.Selected = selected;
            c.Kind   = kind;
            c.Weight = weight;
            c.Id   = id;
        }

            // Diagnostics ride the scan cadence (once per scan).
            if (doLog) {
                uint32_t selCount = 0;
                for (uint32_t k = 0; k < outCount; ++k) selCount += local[k].Selected;
                if (gsp::VerboseLog()) LogInfo("scanned=%d matched=%d inRange=%u selected=%u targets=%zu player=(%.0f,%.0f,%.0f)",
                        total, matched, outCount, selCount, targetCount,
                        ploc.X, ploc.Y, ploc.Z);
            }
        } // end if (doScan)

        SlowT.Step("allies + contact scan");
        // --- MWMissilePath lock-target probe (the authoritative homing-on-ME signal) --------
        // Each MWMissilePath (LRM/Streak/Arrow/etc.) exposes GetLockedTarget() on the base
        // class: the actor the salvo is locked onto. Dumbfire SRMSpreadPath has no lock -> null.
        // So a path whose locked target == our pawn is a homing salvo guided onto US: exact
        // (pointer match, no trajectory guessing), homing-only (SRMs return null), universal
        // (base-class fn), every salvo, no Betty cooldown.
        //
        // KNOWN SOFT SPOT: if GetOwner fails, `owner` stays as the aim-location component,
        // which never matches the pawn - so the probe fails to a silent FALSE NEGATIVE, and
        // "locked>0 lockMe==0" is indistinguishable from a salvo genuinely aimed at someone
        // else. lockMe>0 has been observed in a real mission, so the chain does complete;
        // if the warning turns out to under-fire, this is the first place to look.
        if (!wantThreat) {
            ThreatFlagsNow = 0; ThreatSalvosNow = 0;   // nobody is listening; publish silence
            PathSeen.clear(); PrevProjPtrs.clear(); PendingLaunches.clear();
            ProjIdMap.clear(); ProjIdUsed.clear(); ProjIdQuarantine.clear();
            MinePtrs.clear(); MineLastPos.clear(); MineStill.clear();
        } else if (doThreat) {
            ResolveProjectileClasses();                // MissileClass is discovered lazily
            int pathCount = 0, pathLocked = 0, pathLockMe = 0;
            int newOnMe = 0, newOther = 0;
            ++ProbeSeq;
            EnsureActorFlagProps();
            // Rebuilt every probe - see PendingLaunches. Cleared here, refilled below from
            // whatever is enumerated now, so a path that has gone or has dropped its lock
            // simply is not a candidate any more.
            PendingLaunches.clear();
            if (MissileClass) {
                int mpn = 0;
                API::UObject** mpl = GetActorsOfClass(playerPawn, MissileClass, mpn);
                for (int i = 0; i < mpn; ++i) {
                    auto* mp = mpl ? mpl[i] : nullptr;
                    if (!mp) continue;
                    ++pathCount;
                    void* key = static_cast<void*>(mp);
                    // Re-arm a pointer that has been absent long enough to be a different
                    // actor; otherwise carry its record forward.
                    auto ls = PathSeen.find(key);
                    if (ls != PathSeen.end() && ProbeSeq - ls->second.LastSeen > kPathForgetProbes) {
                        PathSeen.erase(ls);
                        ls = PathSeen.end();
                    }
                    const bool firstSighting = (ls == PathSeen.end());
                    PathRec& rec = firstSighting ? (PathSeen[key] = PathRec{ProbeSeq, false})
                                                 : ls->second;
                    rec.LastSeen = ProbeSeq;

                    struct { void* Ret; } gp{};
                    mp->call_function(L"GetLockedTarget", &gp);
                    if (gp.Ret) {
                        ++pathLocked;
                        // GetLockedTarget returns an MWAIAimLocationComponent ON the target mech,
                        // not the mech itself. Resolve its owning actor and compare to our pawn.
                        auto* tgt = reinterpret_cast<API::UObject*>(gp.Ret);
                        API::UObject* owner = tgt;
                        struct { void* Ret; } op{};
                        tgt->call_function(L"GetOwner", &op);
                        if (op.Ret) owner = reinterpret_cast<API::UObject*>(op.Ret);
                        const bool onMe =
                            static_cast<void*>(owner) == static_cast<void*>(playerPawn);
                        if (onMe) ++pathLockMe;

                        // A LAUNCH IS THE TRANSITION INTO "LOCKED ONTO US", NOT THE PATH
                        // APPEARING. This cost a whole mission: the test used to be "have we
                        // seen this pointer before", evaluated BEFORE the lock was read, so a
                        // path whose GetLockedTarget only resolves to us a scan or two after
                        // it spawns was already stale and could never fire. The log showed it
                        // exactly - lockMe=1 sustained, newOnMe=0 and mine=0 for the entire
                        // session, not one round attributed.
                        //
                        // CountedOnMe latches, so a lock that flickers off and back on the
                        // same path actor does not re-arm; only a genuinely new or recycled
                        // actor does, via the absence check above.
                        // Every LIVE path is a match candidate for as long as it lives, not
                        // just on the probe it first appeared - that is what covers a rack
                        // still ripple-firing rounds seconds after its path was created.
                        // Paths aimed at OTHERS are recorded too: without something to be
                        // nearer to, every round matches us by default.
                        RememberLaunch(mp, onMe);

                        if (onMe && !rec.CountedOnMe) {
                            rec.CountedOnMe = true;
                            ++newOnMe;
                        } else if (!onMe && firstSighting) {
                            ++newOther;
                        }
                    } else {
                        // Lock cleared or never resolved. Still a launch candidate - a spent
                        // path that has dropped its lock must be able to CLAIM a round so the
                        // round is not then matched to a live on-me launch further away.
                        RememberLaunch(mp, false);
                        if (firstSighting) ++newOther;
                    }
                }
            }
            // Forget paths absent for long enough, so the map cannot grow across a mission.
            for (auto it = PathSeen.begin(); it != PathSeen.end(); )
                it = (ProbeSeq - it->second.LastSeen > kPathForgetProbes) ? PathSeen.erase(it)
                                                                          : std::next(it);
            LastPathCount = pathCount; LastPathLocked = pathLocked; LastPathLockMe = pathLockMe;
            LastNewPathsOnMe = newOnMe; LastNewPathsOther = newOther;

            // The burst diagnostic that used to sit here is gone. It existed to decide
            // whether a MWMissilePath is per salvo or per missile, and it allocated a
            // wstring for EVERY new path locked on us - every probe, whether or not it
            // logged - which is the same per-object allocation pattern that once froze the
            // game. The question is answered: one path per SALVO, and a mech in chain-fire
            // or semi-auto fires one missile per salvo, which is why bursts of same-class
            // paths looked per-missile.

            // THE launch signal. One per new path actor guiding on us, and nothing else can
            // produce it - not a path winking out and returning, not a count wobbling.
            if (newOnMe > 0) TotalLaunchOnMe += static_cast<unsigned>(newOnMe);
            if (newOnMe > 0 && newOther > 0) {
                // Still logged: a scan carrying launches at us and at others is exactly the
                // case the spatial match has to get right, so it is worth being able to line
                // these up against reports of a wrong round flashing.
                LogInfo("threat-mixed: %d new path(s) on me, %d on others in the same scan "
                        "- resolving by launch position", newOnMe, newOther);
            }

            // Instantaneous truth. The renderer owns every hold and debounce.
            // Explicit narrowing cast: LTHREAT_LOCKED is an unsigned literal, and MSVC
            // warns (C4244) on the implicit assignment to uint8_t even though the value
            // trivially fits. gcc does not, which is exactly the blind spot that has bitten
            // this project before - so spell it out rather than trust the syntax check.
            // ONE route to this fact: MWMissilePath's GetLockedTarget, resolved to its
            // owner and compared by pointer against our pawn. Exact, and it covers every
            // weapon that creates a path actor - which is every homing weapon except
            // artillery. See the note on the state fields for why nothing is OR-ed in here.
            ThreatFlagsNow  = static_cast<uint8_t>((pathLockMe > 0) ? LTHREAT_LOCKED : 0u);
            ThreatSalvosNow = static_cast<uint8_t>(pathLockMe > 255 ? 255 : pathLockMe);

            // Edge-triggered, UNTHROTTLED: log the moment the lock-on-us state changes.
            // The throttled line below cannot show this - it only prints when pathCount>0,
            // so a gap in the path actors is invisible in it, and the 2 s throttle hides
            // how long the gaps actually are. Transitions are rare, so this costs nothing
            // and it is the only way to size the renderer's release hold from data rather
            // than from inference.
            static int lastLoggedLockMe = -1;
            if (pathLockMe != lastLoggedLockMe) {
                LogInfo("threat-edge: lockMe %d -> %d (pathN=%d locked=%d)",
                        lastLoggedLockMe, pathLockMe, pathCount, pathLocked);
                lastLoggedLockMe = pathLockMe;
            }

            // Own log line: the proj: line only fires while the array scan is on, so
            // THREAT-only mode would otherwise be invisible. Throttled, and silent while
            // there are no live salvos at all so it cannot flood a quiet mission.
            static ULONGLONG lastThreatLog = 0;
            if (pathCount > 0 && now - lastThreatLog >= 2000) {
                lastThreatLog = now;
                if (gsp::VerboseLog()) LogInfo("threat: pathN=%d locked=%d lockMe=%d newOnMe=%d newOther=%d "
                        "pend=%zu mine=%d own=%d fb=%d amb=%d poseFail=%u/%u totLaunch=%u totAttr=%u "
                        "flags=%u proj=%d",
                        pathCount, pathLocked, pathLockMe, newOnMe, newOther,
                        PendingLaunches.size(), LastMineCount, LastOwnSkipped,
                        LastMatchFallbacks, LastAmbiguous, PoseFailOnMe, PoseFailAll,
                        TotalLaunchOnMe, TotalAttributed,
                        static_cast<unsigned>(ThreatFlagsNow), wantProj ? 1 : 0);
            }
        }

        SlowT.Step("missile lock probe");
        // --- projectiles (own ~15 Hz cadence; gathered into a local buffer) ---
        LidarProjectile projLocal[LIDAR_MAX_PROJECTILES];
        uint32_t projCount = 0;
        uint32_t projIncoming = 0; // missiles homing on the player this tick (diagnostic)
        if (doProj) {
            ResolveProjectileClasses();
            ResolveGameplayStatics();
            // Just the projectile actors (MWProjectile subclasses), straight from the
            // engine - no global sweep. MWMissilePath actors are fixed-point path
            // descriptors (impact / launch point), not the moving munition, so we don't
            // ask for them. The real missiles are Projectile_Missile_* / *Arrow* here.
            EnsureActorFlagProps();

            int pn = 0;
            API::UObject** plist = GetActorsOfClass(playerPawn, ProjClass, pn);
            LastProjEnum = pn;                              // what GAAC returned (pool + live)
            if (pn > PeakProjEnum) PeakProjEnum = pn;       // high-water since last log
            // Live-round pointer set for this scan. Built from every round that survives the
            // spent-round filter, NOT just the ones written - so a scan that hits the
            // LIDAR_MAX_PROJECTILES cap cannot retire an attributed round that is merely
            // past the cap. Swapped into PrevProjPtrs at the end of the scan.
            std::unordered_set<void*> curProjs;

            // A GAP in the projectile scan invalidates every pointer we are holding. While
            // the renderer is not asking for the array (scope not the active view, projectile
            // layers off) nothing is pruned, so MinePtrs keeps entries for rounds that have
            // long since impacted - and the pool will have handed those addresses to new
            // rounds fired at somebody else. Flagging happens before the prune later in this
            // same loop, so without this the first scan back would emit a burst of false
            // flags: a brief chirp on switching back to the scope.
            //
            // Attribution is cheap to rebuild - the next launch re-establishes it within a
            // scan - so dropping it outright is the right trade against sounding for rounds
            // we can no longer vouch for.
            if (LastProjScanMs > 0.0 && (now - LastProjScanMs) > kProjGapMs) {
                MinePtrs.clear(); MineLastPos.clear(); MineStill.clear();
                PrevProjPtrs.clear();
            }
            LastProjScanMs = static_cast<double>(now);
            // Attributed rounds that stopped moving this scan; retired below alongside the
            // ones the engine has marked spent.
            std::unordered_set<void*> landedMine;
            int ownSkipped = 0; // our own outgoing rounds refused attribution this scan
            // Is there any live launch on us at all? Necessary, not sufficient - the spatial
            // match below decides per round which launch a round actually came from.
            bool anyPendingOnMe = false;
            for (const auto& pl : PendingLaunches) if (pl.OnMe) { anyPendingOnMe = true; break; }
            const bool attributeNew = anyPendingOnMe;
            int matchFallbacks = 0;
            int ambiguous      = 0;
            uint32_t evictCursor = 0;  // sweeps once across projLocal looking for evictable slots
            int projDropped = 0;       // rounds the cap cost us
            int projEvicted = 0;       // flagged rounds that took an unflagged round's slot
            for (int i = 0; i < pn; ++i) {
                auto* p = plist ? plist[i] : nullptr;
                if (!p) continue;

                // Drop spent rounds immediately: after impact the pooled actor lingers,
                // so skip anything being destroyed or hidden. Cheap masked reads via the
                // cached FBoolProperty (no per-round property-chain walk).
                if (IsSpentActor(p)) continue;

                // Liveness is recorded HERE, above the range and underground filters. Those
                // reject a round for being far away or below the map, which is not the same
                // as it being gone - and MinePtrs is pruned to this set, so filtering here
                // would retire an attributed round the moment it clipped out. Only the spent
                // checks above (destroyed / hidden) mean the round is actually finished, and
                // they are exactly what retires it.
                void* pkey = static_cast<void*>(p);
                curProjs.insert(pkey);

                // Position WITHOUT a per-round ProcessEvent. K2_GetActorLocation IS a
                // ProcessEvent (hooked dispatch + marshaling); at hundreds of live LRM
                // rounds x ~30 Hz that was thousands of ProcessEvents/sec on the render
                // thread, contending with the game thread that's also moving those same
                // missiles - the FPS drop that tipped into a freeze during LRM40 salvos.
                // A flying projectile's root component is unattached, so its RelativeLocation
                // equals its world location (ComponentToWorld is NOT reflected in 4.26, but
                // RelativeLocation is a BlueprintReadOnly UPROPERTY and is what the world
                // transform is derived from). Both reads below are plain property-slot reads
                // (memory), no ProcessEvent.
                auto* rootpp = p->get_property_data<void*>(L"RootComponent");
                if (!rootpp || !*rootpp) continue;
                auto* root = reinterpret_cast<API::UObject*>(*rootpp);
                const auto* lp = root->get_property_data<FVector3f>(L"RelativeLocation");
                if (!lp) continue;
                const FVector3f loc = *lp;
                const float dx = loc.X - ploc.X, dy = loc.Y - ploc.Y, dz = loc.Z - ploc.Z;
                if (dx * dx + dy * dy + dz * dz > RangeNowCm() * RangeNowCm()) continue; // out of range (5 km mode: far)
                // 5 km mode: past 1 km, only rounds a detected enemy fired or near a lancemate (2026-10-15, user)
                if (FarOn && dx * dx + dy * dy + dz * dz > kRangeCm * kRangeCm) {
                    FarRefreshDetected(now);
                    if (!FarRoundShown(p, loc)) continue;
                }
                if (dz < -kUndergroundCm) continue;                              // underground phantom

                // Classify by subclass name, but CACHE it per class - all rounds of a
                // weapon share a type, so the (allocating) to_string runs once per class.
                void*   clsKey = static_cast<void*>(p->get_class());
                uint8_t ptype;
                if (auto ct = ProjTypeCache.find(clsKey); ct != ProjTypeCache.end()) {
                    ptype = ct->second;
                } else {
                    std::wstring cn = p->get_class()->get_fname()->to_string();
                    for (auto& ch : cn) ch = (wchar_t)towlower(ch);
                    auto has = [&](const wchar_t* k) { return cn.find(k) != std::wstring::npos; };
                    if (has(L"ppc") || has(L"particle"))                 ptype = LPT_PPC;
                    else if (has(L"arrow"))                              ptype = LPT_ArrowIV;
                    else if (has(L"missile") || has(L"srm") || has(L"lrm") ||
                             has(L"rocket")  || has(L"narc") || has(L"streak")) ptype = LPT_Missile;
                    else                                                 ptype = LPT_Ballistic;
                    ProjTypeCache[clsKey] = ptype;
                    // projclass1 (2026-10-08): PPC bolts never showed on V-POD / Enhanced Imaging - every round
                    // class the scan meets, and what it was taken for, so the log says whether they arrive at all.
                    static const char* kPt[] = {"ballistic", "PPC", "missile", "Arrow IV", "?", "?", "?", "?"};
                    LogInfo("projectiles: new class %s -> %s", WideToNarrow(p->get_class()->get_fname()->to_string().c_str()).c_str(),
                            kPt[ptype & 7]);
                }

                const bool isMissile = (ptype == LPT_Missile || ptype == LPT_ArrowIV);

                // ATTRIBUTION. A missile whose actor was not alive on the previous scan has
                // just spawned; if a salvo guiding on us launched on this same scan, this
                // round is one of its munitions. Membership is by POINTER and it is decided
                // once, at spawn - it never has to be re-derived, so nothing downstream can
                // lose or transfer it.
                if (isMissile && attributeNew && PrevProjPtrs.find(pkey) == PrevProjPtrs.end()) {
                    // WHICH launch did this round come out of? Nearest pending launch in 3D,
                    // evaluated at the moment the round first appears - when it is still on
                    // its launcher. A round matched to a launch aimed at someone else is
                    // rejected outright, which is the whole point: it is how an ally-targeted
                    // salvo stops being flagged as inbound on us.
                    // Nearest launch ON us and nearest launch on SOMEONE ELSE, tracked
                    // separately - the ambiguity rule needs both, not just the winner.
                    float bestMine2 = -1.0f, bestOther2 = -1.0f;
                    for (const auto& pl : PendingLaunches) {
                        const float ddx = loc.X - pl.Pos.X;
                        const float ddy = loc.Y - pl.Pos.Y;
                        const float ddz = loc.Z - pl.Pos.Z;
                        const float d2  = ddx * ddx + ddy * ddy + ddz * ddz;
                        float& slot = pl.OnMe ? bestMine2 : bestOther2;
                        if (slot < 0.0f || d2 < slot) slot = d2;
                    }
                    const float r2      = kLaunchMatchCm * kLaunchMatchCm;
                    const float amb2    = kAmbiguousMatchCm * kAmbiguousMatchCm;
                    const bool  mineOk  = (bestMine2  >= 0.0f && bestMine2  <= r2);
                    const bool  otherOk = (bestOther2 >= 0.0f && bestOther2 <= amb2);
                    const bool  best    = (bestMine2 >= 0.0f || bestOther2 >= 0.0f);

                    // SETTLES THE OPEN QUESTION, once every 3 s while salvos are flying.
                    // If a path's position is its LAUNCHER, `near` here is a few metres and
                    // the match is sound. If it is the IMPACT point, `near` will be hundreds
                    // of metres, every match will fail the sanity bound, and fb= in the proj:
                    // line will equal the number of new rounds - in which case the tiebreaker
                    // is inert and the fallback is carrying everything.
                    // `me` is the distance from the round to OUR pawn, so the impact-point
                    // case is recognisable too: for a salvo aimed at us it would be small.
                    if (best) {
                        static ULONGLONG lastMatchLog = 0;
                        if (now - lastMatchLog >= 3000) {
                            lastMatchLog = now;
                            const float mdx3 = loc.X - ploc.X;
                            const float mdy3 = loc.Y - ploc.Y;
                            const float mdz3 = loc.Z - ploc.Z;
                            // dMine / dOther are the whole diagnostic: a clean attribution has
                            // one small and one large (or absent, -1). Both small is the
                            // ambiguous case; both large means the round has no credible origin.
                            LogInfo("launch-match: dMine=%.0fm dOther=%.0fm pend=%zu me=%.0fm",
                                    bestMine2  >= 0.0f ? std::sqrt(bestMine2)  / 100.0f : -1.0f,
                                    bestOther2 >= 0.0f ? std::sqrt(bestOther2) / 100.0f : -1.0f,
                                    PendingLaunches.size(),
                                    std::sqrt(mdx3 * mdx3 + mdy3 * mdy3 + mdz3 * mdz3) / 100.0f);
                        }
                    }

                    bool takeIt;
                    if (mineOk && otherOk) {
                        // AMBIGUOUS - see kAmbiguousMatchCm. Two launches with opposite
                        // verdicts both close enough to have produced this round.
                        ++ambiguous;
                        takeIt = false;
                    } else if (mineOk) {
                        takeIt = true;                // unambiguously ours
                    } else {
                        // NO PLAUSIBLE LAUNCH FOR THIS ROUND -> DO NOT ATTRIBUTE IT.
                        //
                        // This used to fall back to the old permissive rule, which was the
                        // wrong shape and was the remaining leak: a round with no credible
                        // origin was waved through, so salvos guiding on an ally 500 m away
                        // were still flagged. Falling back to the very rule the geometry
                        // exists to replace can only reintroduce what it was fixing.
                        //
                        // It was written that way while the geometry was unproven, as a way
                        // of costing nothing if a path's position turned out to be its impact
                        // point. The field logs settled that - matches land at 16-28 m, so
                        // the position is the launcher - and the caution is no longer earning
                        // its keep. Silence for a round we cannot place is the honest answer.
                        ++matchFallbacks;
                        takeIt = false;
                    }
                    // NOT OUR OWN OUTGOING SALVO. Fire back on the scan an enemy launches at
                    // you and every round you just launched is "new this scan" too, so it was
                    // being attributed and then flashed as inbound - which is why the scope
                    // appeared to light up every missile on the field at once.
                    //
                    // Instigator on a projectile resolves to the firing mech (confirmed from
                    // the field: arrow-inst logged a valid pointer that dumped as MWMech).
                    // Read through SafeObjectProp, never as a raw slot.
                    //
                    // This IS a per-round property lookup, which the projectile hot path
                    // otherwise forbids - but it runs only for missile-type rounds that are
                    // new on a scan where a launch on us is live, which is a handful per
                    // salvo rather than the ~250 airborne. It never touches the common path.
                    if (takeIt) {
                        API::UObject* shooter = SafeObjectProp(p, L"Instigator");
                        if (shooter &&
                            static_cast<void*>(shooter) == static_cast<void*>(playerPawn)) {
                            ++ownSkipped;
                        } else if (MinePtrs.insert(pkey).second) {
                            ++TotalAttributed;
                        }
                    }
                }

                // LPROJ_INCOMING now means exactly "this round is guiding on YOU". It used to
                // mean "some missile, while a 3 s block-level lock hold was fresh", which
                // painted every round on the map during any lock. The frozen standalone reads
                // this flag to colour rounds; it will now colour fewer, and correctly.
                uint8_t projFlags = 0;
                if (isMissile && MinePtrs.count(pkey)) {
                    // Landed test, on the position we already read for this round - no extra
                    // property access. 3D, so a round dropping near-vertically is not
                    // mistaken for stationary.
                    auto lp2 = MineLastPos.find(pkey);
                    if (lp2 != MineLastPos.end()) {
                        const float sx = loc.X - lp2->second.X;
                        const float sy = loc.Y - lp2->second.Y;
                        const float sz = loc.Z - lp2->second.Z;
                        if (sx * sx + sy * sy + sz * sz < kMineStillCm * kMineStillCm) {
                            if (++MineStill[pkey] >= kMineStillScans) landedMine.insert(pkey);
                        } else {
                            MineStill[pkey] = 0;
                        }
                    }
                    MineLastPos[pkey] = loc;

                    if (!landedMine.count(pkey)) {
                        projFlags = LPROJ_INCOMING;
                        ++projIncoming;
                    }
                }

                // WRITE PRIORITY. The array is capped at LIDAR_MAX_PROJECTILES and has been
                // observed within five slots of full (peak=507, written=491), so truncation
                // is not hypothetical. The write order is whatever GetAllActorsOfClass hands
                // back, which meant a round guiding on US could be dropped while unrelated
                // ballistic traffic took its slot - and the renderer only ever sees flags on
                // rounds that made it into the array, so the alarm would go quiet mid-salvo
                // with nothing in the log to explain it.
                //
                // A flagged round therefore evicts an unflagged one rather than being lost.
                // evictCursor sweeps forward once across the buffer over the whole scan, so
                // this stays linear - it never rescans from the start.
                if (projCount >= LIDAR_MAX_PROJECTILES) {
                    if (!projFlags) { ++projDropped; continue; }   // unflagged: no room, drop
                    while (evictCursor < projCount &&
                           (projLocal[evictCursor].Flags & LPROJ_INCOMING) != 0)
                        ++evictCursor;
                    if (evictCursor >= projCount) { ++projDropped; continue; } // all flagged
                    const uint16_t eid = AcquireProjId(pkey);
                    LidarProjectile& ev = projLocal[evictCursor++];
                    ev.X = loc.X; ev.Y = loc.Y; ev.Z = loc.Z;
                    ev.Id = eid; ev.Type = ptype; ev.Flags = projFlags;
                    ++projEvicted;
                    continue;
                }

                // Stable, collision-free for this round's whole flight. See ProjIdMap.
                const uint16_t id = AcquireProjId(pkey);

                LidarProjectile& pc = projLocal[projCount++];
                pc.X = loc.X; pc.Y = loc.Y; pc.Z = loc.Z;
                pc.Id = id; pc.Type = ptype; pc.Flags = projFlags;
            }
            LastProjWritten = projCount;

            // RETIREMENT. Two signals, both positive, neither a timer:
            //   despawned - the actor left the live set (engine marked it spent), or
            //   landed    - it stopped moving for kMineStillScans scans.
            // The second exists because the first arrives late or never: a pooled round
            // lingers post-impact with no engine flag set. Still no absence timers, no
            // maximum lifetime and no id-recycle backstop - those existed downstream only
            // because the renderer was guessing at identity and needed guard rails.
            for (auto it = MinePtrs.begin(); it != MinePtrs.end(); ) {
                const bool gone   = (curProjs.find(*it) == curProjs.end()); // despawned
                const bool landed = (landedMine.find(*it) != landedMine.end()); // stopped moving
                if (gone || landed) {
                    MineLastPos.erase(*it);
                    MineStill.erase(*it);
                    it = MinePtrs.erase(it);
                } else {
                    ++it;
                }
            }
            // Release ids whose actor is no longer live, so the map stays bounded by rounds
            // in the air and the id space is recycled. Done here, against the same live set
            // that retires MinePtrs, so identity and attribution can never disagree about
            // which rounds still exist.
            DrainProjIdQuarantine(now);
            for (auto it = ProjIdMap.begin(); it != ProjIdMap.end(); ) {
                if (curProjs.find(it->first) == curProjs.end()) {
                    // Freed, but NOT yet reusable - it stays in ProjIdUsed until its
                    // quarantine expires, so no new round can be handed it meanwhile.
                    ProjIdQuarantine.push_back(FreedId{it->second, now});
                    it = ProjIdMap.erase(it);
                } else {
                    ++it;
                }
            }

            PrevProjPtrs.swap(curProjs);
            LastMineCount = static_cast<int>(MinePtrs.size());
            LastOwnSkipped = ownSkipped;
            // No ageing here any more: PendingLaunches is rebuilt by the probe each tick.
            LastMatchFallbacks = matchFallbacks;
            LastProjDropped    = projDropped;
            LastProjEvicted    = projEvicted;
            LastAmbiguous      = ambiguous;

            // Projectile visibility on its own ~3 s throttle (independent of the scan
            // cadence). enum = what GAAC returned (pool + live), peak = high-water since
            // the last log, written = published this tick. If a barrage hangs the game,
            // the LAST flushed line here tells us how many rounds we were chewing through.
            static ULONGLONG lastProjLog = 0;
            if (now - lastProjLog >= 3000) {
                lastProjLog = now;
                if (gsp::VerboseLog()) LogInfo("proj: enum=%d peak=%d written=%u mine=%u pathN=%d locked=%d lockMe=%d "
                        "newOnMe=%d newOther=%d own=%d drop=%d evict=%d interval=%ums idmap=%zu "
                        "projids=%zu quar=%zu",
                        LastProjEnum, PeakProjEnum, LastProjWritten, projIncoming,
                        LastPathCount, LastPathLocked, LastPathLockMe,
                        LastNewPathsOnMe, LastNewPathsOther, LastOwnSkipped,
                        LastProjDropped, LastProjEvicted, projInterval, IdMap.size(), ProjIdMap.size(), ProjIdQuarantine.size());
                PeakProjEnum = 0;
            }
        }


        SlowT.Step("projectiles");
        // --- publish: seqlock write. The transform is written every tick (~5 Hz);
        // the contacts and the ScanCounter bump happen only on scan ticks (~3 s), so
        // the renderer restarts its ping sweep only when the target set refreshes. ---
        const uint32_t seq = Block->FrameCounter + 1;
        Block->FrameCounter = seq;
        MemoryBarrier();
        Block->PlayerX = ploc.X; Block->PlayerY = ploc.Y; Block->PlayerZ = ploc.Z;
        Block->PlayerYaw      = prot.Yaw;
        Block->PlayerTorsoYaw = torsoYaw;
        Block->SelectedCount  = selCount;
        Block->ThreatFlags    = ThreatFlagsNow;
        Block->ThreatSalvos   = ThreatSalvosNow;
        Block->ThreatPad      = 0;
        for (uint8_t k = 0; k < 4; ++k)
            Block->SelectedIds[k] = (k < selCount) ? selIds[k] : 0;
        Block->AllyCount = AllyBufCount; // republished every tick; refreshed every ~100 ms
        if (AllyBufCount > 0)
            memcpy(Block->Allies, AllyBuf, AllyBufCount * sizeof(LidarContact));
        if (doScan) {
            Block->ContactCount = outCount;
            if (outCount > 0)
                memcpy(Block->Contacts, local, outCount * sizeof(LidarContact));
            Block->ScanCounter = Block->ScanCounter + 1;
        }
        if (doProj) {
            Block->ProjectileCount = projCount;
            if (projCount > 0)
                memcpy(Block->Projectiles, projLocal, projCount * sizeof(LidarProjectile));
            Block->ProjCounter = Block->ProjCounter + 1;
        } else if (!wantProj) {
            Block->ProjectileCount = 0; // layer off -> clear so the renderer shows none
        }
        MemoryBarrier();
        Block->FrameCounterEnd = seq;
        SlowT.Step("publish");
        SlowT.Flush([&](const char* line) { LogInfo("%s", line); });
    }
};

// Registering the global instance is what hands the plugin to UEVR (see
// PluginExtension / uevr::Plugin).
std::unique_ptr<LidarExport> g_plugin{new LidarExport()};
