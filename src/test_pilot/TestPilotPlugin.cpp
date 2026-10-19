// =====================================================================================================
// TestPilot.dll - the test pilot's own small in-game part (UEVR plugin). testpilot-plugin-1 (2026-10-09).
//
// ONE job: fire the PLAYER's weapons when TestPilot.exe asks. Nothing else of the suite is touched - no other
// plugin, no existing shared block. It owns one small block of its own, MW5_TestPilotFire (TestPilotFire.h).
// Without TestPilot.exe asking it does nothing (it only hooks the cockpit tick and reads that block).
//
// On a request (Secs > 0): every 250 ms, for that long, at the PLAYER's current target - the game's own lock
// (TargetTracking.CurrentTarget), never anything else:
//   - what was started last tick is stopped (K2_StopFire);
//   - nothing fires unless the torso's aim (GetAimDirection) is within MaxAimDeg of the target's aim point and
//     heat (the heat component's CurrentHeat / MaxHeatCapacity) is under HeatCap;
//   - every weapon on the mech whose own fire check (GetWeaponFireResults, the game's) says Success is started
//     (K2_StartFire) - cooldown, ammo and minimum range are the game's own answer.
// testpilot-plugin-3 (user 2026-10-17): the game's fire check does NOT test range (a machine gun "succeeds" at 600 m),
// so, as LanceControl's RATED RANGE: no weapon fires at a target past its rated maximum range (MaxRange in its data).
// LEAD AND DROP (as LanceControl's aim marks): a projectile weapon's aim gate is checked against the aim point led by
// the target's velocity x flight time and raised by 1/2 g GravityScale t^2; the point for the in-range projectile weapon
// needing the biggest correction is published (TpFireBlock v2 AimPt) and TestPilot.exe points the torso at it.
// Runs on the GAME thread: the player cockpit's ReceiveTick (UEVR's engine-tick callback is not delivered in
// this setup), hooked the way LanceControl / LidarExport do - chained, never unhooked.
// =====================================================================================================
#include <Plugin.hpp>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>
#include "TestPilotFire.h"

using namespace uevr;

static constexpr const char *kBuild = "BUILD=testpilot-plugin-8 (BUG SWEEP 2: the render thread looks for the cockpit only while its tick is not reaching us, 2026-10-18) (testpilot-plugin-7 RANGE ALWAYS CM: WeaponRanges.MaxRange (StatsData.Range / .Ranges) in cm, as LanceControl-69 - the MG read as 19.5 km, 2026-10-17) (testpilot-plugin-6 (LOOP GUARD on the cockpit tick chain, 2026-10-17) (testpilot-plugin-5 NO AMS, NO EMPTY HARDPOINTS: never started, 2026-10-17) (testpilot-plugin-4 RANGE LOOKUP AS LANCECONTROL: float fields only, path logged - the HMG read a same-named integer, 2026-10-17) (testpilot-plugin-3 RATED RANGE + LEAD AND DROP as LanceControl: no weapon fires past its rated range; projectile weapons aim led and raised for drop, the aim point published for the torso, 2026-10-17) (testpilot-plugin-2 no melee; lock-on missiles only with a full missile lock - kinds from the MissilePath as LanceControl; the player's weapons at the player's lock on TestPilot.exe's request - own fire check, aim gate, heat ceiling; own block MW5_TestPilotFire)";

#define TP_NATIVE_FUNCTION_OFFSET 0xD8 // UFunction::Func, as PluginExtension.h

class TestPilotPlugin final : public Plugin
{
  public:
    class FFrame
    {
      public:
        char            Pad0[0x10];
        API::UFunction *Node;
        API::UObject   *Object;
        uint8_t        *Code;
        uint8_t        *Locals;
    };
    using BP_FUNC = void *(*)(API::UObject *, FFrame *, void *);

    inline static TestPilotPlugin *Instance = nullptr;
    TestPilotPlugin() { Instance = this; }
    ~TestPilotPlugin() override
    {
        if (Blk)
        {
            Blk->PluginAlive = 0;
            UnmapViewOfFile(Blk);
        }
        if (Map)
            CloseHandle(Map);
    }

    void on_initialize() override
    {
        API::get()->log_info("[TestPilot] %s", kBuild);
        Ensure();
    }

    // render thread: only the block and the driver hook - nothing of the game is touched here
    void on_present() override
    {
        if (!Ensure())
            return;
        Blk->PluginAlive = (uint32_t)(GetTickCount64() / 1000);
        DrvInstall();
    }

  private:
    // ---- own block --------------------------------------------------------------------------------
    HANDLE         Map = nullptr;
    TpFireBlock   *Blk = nullptr;
    bool Ensure()
    {
        if (Blk)
            return true;
        Map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(TpFireBlock), TP_FIRE_MMF_NAME);
        if (!Map)
            return false;
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        Blk = (TpFireBlock *)MapViewOfFile(Map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TpFireBlock));
        if (!Blk)
            return false;
        if (!existed || Blk->Magic != TP_FIRE_MAGIC || Blk->Version != TP_FIRE_VERSION)
        {
            std::memset(Blk, 0, sizeof(*Blk));
            Blk->Magic = TP_FIRE_MAGIC;
            Blk->Version = TP_FIRE_VERSION;
        }
        DoneSeq = Blk->ReqSeq; // anything asked before we were running is stale
        return true;
    }

    // ---- the game-thread driver: the player cockpit's ReceiveTick ----------------------------------
    struct DrvHook
    {
        API::UFunction *Fn;
        BP_FUNC         Orig;
    };
    std::vector<DrvHook> DrvHooks;
    std::mutex           DrvMx;
    const void          *DrvClass = nullptr;
    ULONGLONG            DrvNextTryMs = 0;
    std::atomic<ULONGLONG> DrvSeenMs{0}; // bug sweep 15: the player's cockpit tick last reached us

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
            return;
        API::UClass *cls = cockpit->get_class();
        if (!cls || static_cast<const void *>(cls) == DrvClass)
            return;
        API::UFunction *fn = cls->find_function(L"ReceiveTick");
        if (!fn)
            return;
        std::lock_guard<std::mutex> lk(DrvMx);
        for (auto &h : DrvHooks)
            if (h.Fn == fn)
            {
                DrvClass = cls;
                return;
            }
        auto **slot = reinterpret_cast<BP_FUNC *>(reinterpret_cast<uintptr_t>(fn) + TP_NATIVE_FUNCTION_OFFSET);
        DrvHooks.push_back({fn, *slot});
        *slot = &OnDrvTick;
        DrvClass = cls;
        API::get()->log_info("[TestPilot] game-thread driver: hooked %ls::ReceiveTick", cls->get_fname()->to_string().c_str());
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
                orig = Instance->DrvHooks.back().Orig;
        }
        // testpilot-plugin-6 (user 2026-10-17): LOOP GUARD. If a hook chain ever leads back into us (LidarExport re-hooked
        // the same function after us on a takeover and saved OUR detour as its original - a stack-overflow crash), the
        // chain is cut at depth 8 instead of recursing until the stack runs out; logged once.
        static thread_local int depth = 0;
        if (depth >= 8)
        {
            static bool said = false;
            if (!said && Instance)
            {
                said = true;
                API::get()->log_error("[TestPilot] game-thread driver: the cockpit tick chain loops back into TestPilot - cut");
            }
            return nullptr;
        }
        ++depth;
        void *r = orig ? orig(self, frame, result) : nullptr; // the cockpit (and anyone hooked before us) first
        --depth;
        if (Instance && self && self == LocalCockpit())
        {
            Instance->DrvSeenMs = GetTickCount64(); // bug sweep 15
            Instance->GameTick();
        }
        return r;
    }

    // ---- reflection helpers (as LanceControl's) -------------------------------------------------------
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
    static std::wstring PropType(API::FProperty *p)
    {
        return (p && p->get_class()) ? p->get_class()->get_name() : L"";
    }
    static API::UObject *ObjProp(API::UObject *obj, const wchar_t *name)
    {
        if (!obj || !obj->get_class())
            return nullptr;
        auto *p = FindProp(obj->get_class(), name);
        if (!p || PropType(p) != L"ObjectProperty")
            return nullptr;
        return *reinterpret_cast<API::UObject **>(reinterpret_cast<uint8_t *>(obj) + p->get_offset());
    }
    static API::UFunction *FindFn(API::UObject *obj, const wchar_t *name)
    {
        if (!obj || !obj->get_class())
            return nullptr;
        API::UFunction *fn = nullptr;
        for (auto *st = static_cast<API::UStruct *>(obj->get_class()); st && !fn; st = st->get_super_struct())
            fn = st->find_function(name);
        return fn;
    }
    static int32_t ParamOffset(API::UFunction *fn, const wchar_t *name)
    {
        for (auto *f = fn->get_child_properties(); f; f = f->get_next())
            if (f->get_fname() && f->get_fname()->to_string() == name)
                return reinterpret_cast<API::FProperty *>(f)->get_offset();
        return -1;
    }
    uint8_t CallBuf[1024];
    // a call whose only interesting part is the return value (no arguments)
    bool CallRet0(API::UObject *obj, const wchar_t *name, void *ret, size_t retSize)
    {
        API::UFunction *fn = obj ? FindFn(obj, name) : nullptr;
        if (!fn)
            return false;
        const int32_t frame = fn->get_properties_size();
        const int32_t roff  = ParamOffset(fn, L"ReturnValue");
        if (frame < 0 || frame > (int32_t)sizeof(CallBuf) - 16 || roff < 0 || (size_t)roff + retSize > (size_t)frame)
            return false;
        std::memset(CallBuf, 0, (size_t)frame + 16);
        obj->process_event(fn, CallBuf);
        std::memcpy(ret, CallBuf + roff, retSize);
        return true;
    }
    bool CallNoArgs(API::UObject *obj, const wchar_t *name)
    {
        API::UFunction *fn = obj ? FindFn(obj, name) : nullptr;
        if (!fn)
            return false;
        const int32_t frame = fn->get_properties_size();
        if (frame < 0 || frame > 512)
            return false;
        std::vector<uint8_t> f((size_t)frame + 16, 0);
        obj->process_event(fn, f.data());
        return true;
    }
    // the actor's components (K2_GetComponentsByClass(ActorComponent))
    API::UFunction      *CbcFn = nullptr;
    int32_t              CbcClassOff = -1, CbcRetOff = -1;
    std::vector<uint8_t> CbcBuf;
    bool Components(API::UObject *actor, std::vector<API::UObject *> &out)
    {
        out.clear();
        static API::UClass *acc = nullptr;
        if (!acc)
            acc = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.ActorComponent");
        if (!actor || !acc)
            return false;
        if (!CbcFn)
        {
            CbcFn = FindFn(actor, L"K2_GetComponentsByClass");
            if (!CbcFn)
                return false;
            CbcClassOff = ParamOffset(CbcFn, L"ComponentClass");
            CbcRetOff = ParamOffset(CbcFn, L"ReturnValue");
            const int32_t frame = CbcFn->get_properties_size();
            if (CbcClassOff < 0 || CbcRetOff < 0 || frame <= 0)
            {
                CbcFn = nullptr;
                return false;
            }
            CbcBuf.assign((size_t)frame + 16, 0);
        }
        std::memcpy(CbcBuf.data() + CbcClassOff, &acc, sizeof(void *));
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
    bool IsDead(API::UObject *a)
    {
        if (!a || !a->get_class())
            return true;
        for (const wchar_t *cn : {L"MechLivingActorComponent", L"SimpleLivingActorComponent"})
            if (API::UObject *lc = ObjProp(a, cn))
                if (auto *p = FindProp(lc->get_class(), L"bIsDead"))
                    if (PropType(p) == L"BoolProperty")
                        return reinterpret_cast<API::FBoolProperty *>(p)->get_value_from_object(lc);
        return false;
    }
    float HeatFrac(const std::vector<API::UObject *> &comps)
    {
        for (auto *c : comps)
            if (c && ClassName(c).find(L"Heat") != std::wstring::npos)
            {
                auto *cp = FindProp(c->get_class(), L"CurrentHeat");
                auto *mp = FindProp(c->get_class(), L"MaxHeatCapacity");
                if (cp && mp && PropType(cp) == L"FloatProperty" && PropType(mp) == L"FloatProperty")
                {
                    const uint8_t *b = reinterpret_cast<uint8_t *>(c);
                    const float cur = *reinterpret_cast<const float *>(b + cp->get_offset());
                    const float mx = *reinterpret_cast<const float *>(b + mp->get_offset());
                    return mx > 0.0f ? cur / mx : -1.0f;
                }
            }
        return -1.0f;
    }
    // the target's aim hint, else its AI aim point, else its root (as LanceControl's AimPointOf)
    API::UObject *AimPointOf(API::UObject *tgt)
    {
        std::vector<API::UObject *> comps;
        if (Components(tgt, comps))
        {
            for (auto *c : comps)
                if (c && ClassName(c).find(L"AimHint") != std::wstring::npos)
                    return c;
            for (auto *c : comps)
                if (c && ClassName(c).find(L"AIAimLocation") != std::wstring::npos)
                    return c;
        }
        return ObjProp(tgt, L"RootComponent");
    }

    // ---- the weapons' own fire check (GetWeaponFireResults), resolved once -----------------------------
    API::UFunction      *FrFn = nullptr;
    bool                 FrTried = false, FrOk = false;
    int32_t              FrRet = -1;
    std::vector<int32_t> FrObjParams;
    std::vector<uint8_t> FrBuf;
    void PrepFire(API::UObject *wc)
    {
        if (FrTried)
            return;
        FrTried = true;
        FrFn = FindFn(wc, L"GetWeaponFireResults");
        if (!FrFn)
        {
            API::get()->log_warn("[TestPilot] GetWeaponFireResults not found on %ls - no firing", ClassName(wc).c_str());
            return;
        }
        FrOk = true;
        for (auto *f = FrFn->get_child_properties(); f; f = f->get_next())
        {
            auto *p = reinterpret_cast<API::FProperty *>(f);
            if (!p->is_param())
                continue;
            const std::wstring t = PropType(p);
            if (p->is_return_param())
            {
                FrRet = p->get_offset();
                continue;
            }
            if (t == L"ObjectProperty")
                FrObjParams.push_back(p->get_offset());
            else if (p->is_out_param() || !(t == L"BoolProperty" || t == L"FloatProperty" || t == L"IntProperty" ||
                                            t == L"ByteProperty" || t == L"EnumProperty"))
                FrOk = false;
        }
        if (FrRet < 0)
            FrOk = false;
        if (FrOk)
            FrBuf.assign((size_t)FrFn->get_properties_size() + 16, 0);
        API::get()->log_info("[TestPilot] weapons' fire check %s", FrOk ? "ready" : "has parameters it cannot fill - no firing");
    }

    // ---- firing -------------------------------------------------------------------------------------
    uint32_t                    DoneSeq = 0;
    ULONGLONG                   Until = 0, TickMs = 0, LogMs = 0;
    float                       HeatCap = 0.85f, MaxAim = 4.0f;
    bool                        Active = false;
    std::vector<API::UObject *> Firing;

    // ---- testpilot-plugin-2: what kind of weapon (copied from LanceControl's insane allies) --------------
    // 0 direct, 1 lock-on missile (arcing / homing MissilePath, or a MissilePath that cannot be read),
    // 2 melee (never fired). Decided once per weapon class and logged.
    struct WpnKind
    {
        API::UClass *C;
        int          K;
    };
    std::vector<WpnKind> Kinds;
    int KindOf(API::UObject *w)
    {
        for (auto &e : Kinds)
            if (e.C == w->get_class())
                return e.K;
        int k = 0;
        std::wstring how = L"no MissilePath (direct)";
        for (auto *st = static_cast<API::UStruct *>(w->get_class()); st && k != 2; st = st->get_super_struct())
            if (st->get_fname() && st->get_fname()->to_string().find(L"Melee") != std::wstring::npos)
                k = 2, how = L"melee - never fired";
        if (k != 2)
        {
            API::UObject *da = nullptr;
            if (FindFn(w, L"GetWeaponDataAsset"))
                CallRet0(w, L"GetWeaponDataAsset", &da, sizeof(da));
            // testpilot-plugin-5 (user 2026-10-17): AMS and empty hardpoints are NOT weapons to fire - plugin-4 started
            // every AMS (its fire check says Success) and the Blank mounts. AMS as LanceControl tells it (trace data with
            // Duration < -0.5 and damage over duration), or by name; a mount with no weapon data is an empty hardpoint.
            std::wstring cn = ClassName(w);
            for (auto &ch : cn)
                ch = (wchar_t)towlower(ch);
            float dur = 0.0f, dod = 0.0f;
            const bool amsData = da && FindNum(da->get_class(), reinterpret_cast<uint8_t *>(da), 0, L"Duration", dur) && dur < -0.5f &&
                                 FindNum(da->get_class(), reinterpret_cast<uint8_t *>(da), 0, L"DamageOverDuration", dod) && dod > 0.0f;
            if (!da)
                k = 2, how = L"no weapon data - an empty hardpoint, never fired";
            else if (amsData || cn.find(L"_ams") != std::wstring::npos)
                k = 2, how = amsData ? L"AMS (its data) - never fired" : L"AMS (its name) - never fired";
            if (k == 2)
                ;
            else if (!da)
                how = L"no data asset (direct)";
            else if (auto *pp = FindProp(da->get_class(), L"MissilePath"))
            {
                const std::wstring t = PropType(pp);
                uint8_t *at = reinterpret_cast<uint8_t *>(da) + pp->get_offset();
                std::wstring raw;
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
                std::wstring lo = raw;
                for (auto &ch : lo)
                    ch = (wchar_t)towlower(ch);
                if (raw.empty())
                    k = 1, how = L"MissilePath not readable - taken as lock-on";
                else if (lo.find(L"spread") != std::wstring::npos)
                    k = 0, how = L"spread flight path (dumb-fire) " + raw;
                else
                    k = 1, how = (lo.find(L"homing") != std::wstring::npos || lo.find(L"streak") != std::wstring::npos ? L"homing flight path "
                                                                                                                         : L"arcing flight path ") + raw;
            }
        }
        Kinds.push_back({w->get_class(), k});
        API::get()->log_info("[TestPilot] weapon %ls: %s (%ls)", ClassName(w).c_str(), k == 2 ? "not fired" : (k == 1 ? "lock-on" : "direct"),
                             how.c_str());
        return k;
    }
    // the player's missile lock is complete on this target (LockOnState Locked = 3)
    bool LockedOn(API::UObject *pawn, API::UObject *tgt)
    {
        API::UObject *ws = ObjProp(pawn, L"WeaponSystemComponent");
        API::UObject *lc = ws ? ObjProp(ws, L"LockOnComponent") : nullptr;
        auto *p = lc ? FindProp(lc->get_class(), L"LockOnState") : nullptr;
        if (!p || *(reinterpret_cast<uint8_t *>(lc) + p->get_offset()) != 3)
            return false;
        API::UObject *pending = ObjProp(lc, L"PendingLockTarget");
        return !pending || pending == tgt;
    }

    void Say(uint32_t state, const char *fmt, ...)
    {
        char    buf[160];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        Blk->State = state;
        std::memcpy((void *)Blk->Last, buf, sizeof(buf) < sizeof(Blk->Last) ? sizeof(buf) : sizeof(Blk->Last));
        Blk->Last[sizeof(Blk->Last) - 1] = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - LogMs > 1500)
        {
            LogMs = now;
            API::get()->log_info("[TestPilot] fire: %s", buf);
        }
    }

    // ---- testpilot-plugin-3: rated range, projectile speed and drop per weapon class (as LanceControl) ----------
    struct WSt
    {
        API::UClass *C = nullptr;
        float        RangeCm = -1.0f, SpeedCms = 0.0f, Grav = 0.0f;
        bool         Proj = false;
    };
    std::vector<WSt> Stats;
    float            GravZ = 0.0f;

    // the first float / int / byte field named `name` anywhere in `st` at `base` (nested structs, 3 deep)
    static bool FindNum(API::UStruct *st, uint8_t *base, int depth, const wchar_t *name, float &out)
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
                        if (FindNum(ss, base + p->get_offset(), depth + 1, name, out))
                            return true;
            }
        return false;
    }

    // testpilot-plugin-4 (user 2026-10-17): the rated range is found EXACTLY as LanceControl's IcWeaponRangeCm / WsFind -
    // the first FLOAT field of that name (case-insensitive), nested structs 3 deep, with its path. plugin-3 took int / byte
    // fields too and read a same-named integer on the HMG (15000 -> "15000 m"; the real StatsData.Range.MaxRange is 200 m).
    static bool WsFind(API::UStruct *st, uint8_t *base, int depth, const wchar_t *name, float &out, std::wstring &path,
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


    // RANGE (lancecontrol-69 / testpilot-plugin-7, user 2026-10-17): a weapon's rated range is the MaxRange float of a
    // WeaponRanges struct - StatsData.Range on projectile / missile weapons, StatsData.Ranges on trace weapons (MG, lasers,
    // flamer) - ALWAYS in cm (the mod kit's assets: MG 13000, LRM5 90000). The old "under 20000 = metres" rule read a
    // 195 m MG (19500 cm) as 19.5 km. Only WeaponRanges is accepted, never other range data (MWO_RangeData).
    static bool WsFindRange(API::UStruct *st, uint8_t *base, int depth, bool inRanges, float &out, std::wstring &path,
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

    WSt StatOf(API::UObject *w)
    {
        for (auto &e : Stats)
            if (e.C == w->get_class())
                return e;
        WSt st;
        st.C = w->get_class();
        std::wstring rangePath;
        API::UObject *da = nullptr;
        if (FindFn(w, L"GetWeaponDataAsset"))
            CallRet0(w, L"GetWeaponDataAsset", &da, sizeof(da));
        if (da && da->get_class())
        {
            uint8_t *base = reinterpret_cast<uint8_t *>(da);
            float    v = 0.0f;
            if (WsFindRange(da->get_class(), base, 0, false, v, rangePath) && v > 0.0f)
                st.RangeCm = v; // testpilot-plugin-7: WeaponRanges.MaxRange, always cm
            // a projectile weapon: ProjectileWeaponData, or the game's StatsData whose type says Projectile
            API::FProperty *sp = FindProp(da->get_class(), L"ProjectileWeaponData");
            if (!sp)
                if (API::FProperty *sd = FindProp(da->get_class(), L"StatsData"))
                    if (PropType(sd) == L"StructProperty")
                        if (auto *ss = reinterpret_cast<API::FStructProperty *>(sd)->get_struct())
                            if (ss->get_fname()->to_string().find(L"Projectile") != std::wstring::npos)
                                sp = sd;
            if (sp && PropType(sp) == L"StructProperty")
                if (auto *ss = reinterpret_cast<API::FStructProperty *>(sp)->get_struct())
                {
                    float spd = 0.0f, g = 0.0f;
                    uint8_t *sb = base + sp->get_offset();
                    if (FindNum(ss, sb, 0, L"Speed", spd) && spd > 0.0f)
                    {
                        st.SpeedCms = spd > 5000.0f ? spd : spd * 100.0f; // StatsData speeds are cm/s already
                        st.Proj = true;
                        if (FindNum(ss, sb, 0, L"GravityScale", g) && g > 0.0f && g < 50.0f)
                            st.Grav = g;
                    }
                }
        }
        Stats.push_back(st);
        API::get()->log_info("[TestPilot] weapon %ls: rated range %s, %s", ClassName(w).c_str(),
                             st.RangeCm > 0.0f ? (std::to_string((int)(st.RangeCm * 0.01f)) + " m (" + std::string(rangePath.begin(), rangePath.end()) + ")").c_str()
                                               : "unknown (not limited)",
                             st.Proj ? ("projectile " + std::to_string((int)(st.SpeedCms * 0.01f)) + " m/s, gravity scale " +
                                        std::to_string(st.Grav)).c_str()
                                     : "no projectile speed (aimed straight)");
        return st;
    }

    float Gravity()
    {
        if (GravZ < 0.0f)
            return GravZ;
        GravZ = -980.0f;
        if (auto *c = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.PhysicsSettings"))
            if (API::UObject *cdo = c->get_class_default_object())
                if (auto *p = FindProp(cdo->get_class(), L"DefaultGravityZ"))
                    if (PropType(p) == L"FloatProperty")
                    {
                        const float g = *reinterpret_cast<const float *>(reinterpret_cast<uint8_t *>(cdo) + p->get_offset());
                        if (g < -1.0f)
                            GravZ = g;
                    }
        API::get()->log_info("[TestPilot] gravity %.0f cm/s2", GravZ);
        return GravZ;
    }

    static float Dist3(const float a[3], const float b[3])
    {
        const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return std::sqrt(x * x + y * y + z * z);
    }

    // the point a projectile weapon must be aimed at: led by the target's velocity, raised for drop (3 passes, t <= 4 s)
    void Solve(const float here[3], const float ap[3], const float vel[3], const WSt &st, float out[3])
    {
        float t = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            const float pt[3] = {ap[0] + vel[0] * t, ap[1] + vel[1] * t, ap[2] + vel[2] * t};
            t = Dist3(here, pt) / st.SpeedCms;
            if (t > 4.0f)
                t = 4.0f;
        }
        out[0] = ap[0] + vel[0] * t;
        out[1] = ap[1] + vel[1] * t;
        out[2] = ap[2] + vel[2] * t + 0.5f * -Gravity() * st.Grav * t * t;
    }

    static float AngleTo(const float dir[3], const float from[3], const float to[3])
    {
        const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz), al = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (d < 1.0f || al <= 0.0f)
            return 180.0f;
        float c = (dir[0] * dx + dir[1] * dy + dir[2] * dz) / (al * d);
        c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
        return std::acos(c) * 57.2958f;
    }

    // the torso's point (TpFireBlock v2): every 250 ms while the game has a lock, fire request or not
    ULONGLONG AimMs = 0;
    void UpdateAim()
    {
        API::UObject *pawn = API::get()->get_local_pawn(0);
        API::UObject *tt   = pawn ? ObjProp(pawn, L"TargetTracking") : nullptr;
        API::UObject *tgt  = tt ? ObjProp(tt, L"CurrentTarget") : nullptr;
        float here[3] = {}, ap[3] = {}, vel[3] = {};
        API::UObject *aim = (tgt && !IsDead(tgt)) ? AimPointOf(tgt) : nullptr;
        std::vector<API::UObject *> comps;
        if (!aim || !CallRet0(pawn, L"K2_GetActorLocation", here, sizeof(here)) || !CallRet0(aim, L"K2_GetComponentLocation", ap, sizeof(ap)) ||
            !Components(pawn, comps))
        {
            Blk->AimOn = 0;
            return;
        }
        CallRet0(tgt, L"GetVelocity", vel, sizeof(vel));
        const float dist = Dist3(here, ap);
        float best[3] = {ap[0], ap[1], ap[2]}, bestCorr = 0.0f;
        std::wstring who = L"aim point";
        for (auto *w : comps)
        {
            if (!w || !FindFn(w, L"K2_StartFire") || !FindFn(w, L"GetWeaponDataAsset") || KindOf(w) != 0)
                continue;
            const WSt st = StatOf(w);
            if (!st.Proj || (st.RangeCm > 0.0f && dist > st.RangeCm))
                continue;
            float pt[3];
            Solve(here, ap, vel, st, pt);
            const float corr = Dist3(pt, ap);
            if (corr > bestCorr)
                bestCorr = corr, std::memcpy(best, pt, sizeof(best)), who = ClassName(w);
        }
        std::memcpy((void *)Blk->AimPt, best, sizeof(best));
        Blk->AimCorrM = bestCorr * 0.01f;
        const std::string n(who.begin(), who.end());
        std::memset((void *)Blk->AimWho, 0, sizeof(Blk->AimWho));
        std::memcpy((void *)Blk->AimWho, n.c_str(), n.size() < sizeof(Blk->AimWho) - 1 ? n.size() : sizeof(Blk->AimWho) - 1);
        Blk->AimOn = 1;
        ++Blk->AimSeq;
    }

    void GameTick()
    {
        if (!Blk)
            return;
        const ULONGLONG now = GetTickCount64();
        if (now - AimMs >= 250)
        {
            AimMs = now;
            UpdateAim();
        }
        // a new request?
        const uint32_t seq = Blk->ReqSeq;
        if (seq != DoneSeq && !(seq & 1u))
        {
            DoneSeq = seq;
            float secs = Blk->ReqSecs;
            secs = secs < 0.0f ? 0.0f : (secs > 10.0f ? 10.0f : secs);
            HeatCap = (Blk->ReqHeatCap > 0.05f && Blk->ReqHeatCap <= 1.0f) ? Blk->ReqHeatCap : 0.85f;
            MaxAim = (Blk->ReqMaxAimDeg > 0.2f && Blk->ReqMaxAimDeg <= 15.0f) ? Blk->ReqMaxAimDeg : 4.0f;
            Until = secs > 0.0f ? now + (ULONGLONG)(secs * 1000.0f) : 0;
            Blk->AckSeq = seq;
            API::get()->log_info("[TestPilot] fire request %u: %.1f s, heat ceiling %.0f %%, aim within %.1f deg", seq / 2, secs,
                                 HeatCap * 100.0f, MaxAim);
        }
        if (!Active && Firing.empty() && now >= Until)
            return;
        if (now - TickMs < 250)
            return;
        TickMs = now;
        API::UObject *pawn = API::get()->get_local_pawn(0);
        std::vector<API::UObject *> comps;
        const bool have = pawn && Components(pawn, comps);
        for (auto *w : Firing) // stop what was started last tick (only weapons still on the mech)
            for (auto *c : comps)
                if (c == w)
                {
                    CallNoArgs(w, L"K2_StopFire");
                    break;
                }
        Firing.clear();
        if (now >= Until)
        {
            if (Active)
                Say(0, "done");
            Active = false;
            return;
        }
        Active = true;
        if (!have)
            return Say(2, "no mech");
        API::UObject *tt  = ObjProp(pawn, L"TargetTracking");
        API::UObject *tgt = tt ? ObjProp(tt, L"CurrentTarget") : nullptr;
        if (!tgt || IsDead(tgt))
            return Say(2, "holding - no lock (the game's own target) or it is dead");
        const float heat = HeatFrac(comps);
        if (heat > HeatCap)
            return Say(2, "holding - heat %.0f %% over the %.0f %% ceiling", heat * 100.0f, HeatCap * 100.0f);
        float here[3] = {}, ap[3] = {}, ad[3] = {};
        API::UObject *aim = AimPointOf(tgt);
        if (!aim || !CallRet0(pawn, L"K2_GetActorLocation", here, sizeof(here)) || !CallRet0(aim, L"K2_GetComponentLocation", ap, sizeof(ap)) ||
            !CallRet0(pawn, L"GetAimDirection", ad, sizeof(ad)))
            return Say(2, "holding - target or aim not readable");
        const float dx = ap[0] - here[0], dy = ap[1] - here[1], dz = ap[2] - here[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz), al = std::sqrt(ad[0] * ad[0] + ad[1] * ad[1] + ad[2] * ad[2]);
        if (dist < 1.0f || al <= 0.0f)
            return Say(2, "holding - aim not readable");
        float c = (ad[0] * dx + ad[1] * dy + ad[2] * dz) / (al * dist);
        c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
        const float err = std::acos(c) * 57.2958f;
        Blk->AimErrDeg = err;
        Blk->DistM = dist * 0.01f;
        Blk->Heat = heat;
        // testpilot-plugin-3: the gate is per weapon below (a projectile weapon against its led / raised point)
        float vel[3] = {};
        CallRet0(tgt, L"GetVelocity", vel, sizeof(vel));
        int started = 0, checked = 0, waitLock = 0, outRange = 0, offAim = 0;
        const bool locked = LockedOn(pawn, tgt);
        for (auto *w : comps)
        {
            if (!w || !FindFn(w, L"K2_StartFire") || !FindFn(w, L"GetWeaponFireResults"))
                continue;
            PrepFire(w);
            if (!FrOk)
                break;
            const int kind = KindOf(w);
            if (kind == 2)
                continue;
            ++checked;
            if (kind == 1 && !locked)
            {
                ++waitLock;
                continue;
            }
            const WSt st = StatOf(w);
            if (st.RangeCm > 0.0f && dist > st.RangeCm) // RATED RANGE: the game's own check would fire it anyway
            {
                ++outRange;
                continue;
            }
            if (kind == 0)
            {
                float pt[3] = {ap[0], ap[1], ap[2]};
                if (st.Proj)
                    Solve(here, ap, vel, st, pt);
                if (AngleTo(ad, here, pt) > MaxAim)
                {
                    ++offAim;
                    continue;
                }
            }
            else if (err > MaxAim)
            {
                ++offAim;
                continue;
            }
            std::vector<uint8_t> f(FrBuf.size(), 0);
            for (int32_t off : FrObjParams)
                std::memcpy(f.data() + off, &tgt, sizeof(tgt));
            w->process_event(FrFn, f.data());
            if (f[FrRet] != 0) // not Success: cooldown, ammo, range ...
                continue;
            if (CallNoArgs(w, L"K2_StartFire"))
            {
                Firing.push_back(w);
                ++started;
            }
        }
        Blk->Shots += started;
        Say(started ? 1 : 2, "%d of %d weapon(s) fired at %ls, %.0f m, aim %.1f deg off, heat %.0f %%%s; %d past rated range, %d off aim",
            started, checked, ClassName(tgt).c_str(), dist * 0.01f, err, heat * 100.0f,
            waitLock ? (locked ? "" : ", lock-on missiles wait for the missile lock") : "", outRange, offAim);
    }
};

// NOLINTNEXTLINE
static TestPilotPlugin *g_testpilot = new TestPilotPlugin();
