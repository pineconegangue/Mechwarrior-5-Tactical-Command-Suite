// =====================================================================================================
// TestPilot.exe - an "instrument" test pilot for the suite's features in a running MW5 mission.
// testpilot-5 (2026-10-09): no keyboard, no mouse, no window focus.
//
// READS (never writes) the state the plugins already publish:
//   MW5_LidarExport       - the mech's position / heading / torso (~60 Hz), the player's lock, enemy contacts
//                           (the ~3 s scan), allies (~10 Hz)
//   MW5_VanillaHUDExport  - the mission's nav points, the missile-lock state and the current target's live
//                           bounds (every frame while V-POD / CAP / the vision ask for them)
// ACTS only through LanceControl's existing order ring (TacCmds - the documented multi-writer contract that
// HUDDisplay and VoiceCommand use): TCO_SelfMove / TCO_SelfStop (the navmesh autopilot), TCO_TorsoTarget /
// TCO_TorsoCentre / TCO_TorsoRelease, TCO_LockTarget. It never fires a weapon. Nothing else is written.
//
// Started once; then idles in the background and takes commands from TestPilot.cmd beside the exe (read and
// deleted every 0.5 s - one command per line):
//   approach [stop_m]   go for the nearest enemy (none on the scan: the nearest nav point until one shows),
//                       stop stop_m short of it, torso onto it, lock it, then hold (torso kept on it)
//   nav [letter]        walk to a nav point (default: the nearest), then stop
//   lock                lock the nearest enemy (as it was on the last scan)
//   torso target|centre|release
//   stop                autopilot off, torso released (the pilot has everything back)
//   status              log the full state now
//   quit                stop and exit
// Defaults from TestPilot.ini [pilot]: stop_m (300), hold_s (0 = hold until told otherwise),
// target_timeout_s (30: a target unseen that long is dropped, 0 = never), target_ignore_s (60: then avoided that long).
//   ba                  testpilot-16: battle armour report (suits by side, team bytes, nearest, in the lance feed,
//                       on a friendly); also logged by itself whenever it changes (every 5 s)
//   approach [m] ba / lock ba   - only battle armour suits are picked
//   vision on|off       the Enhanced Imaging vision mode (VHUD EiVisionOn - the flag HUDDisplay / VoiceCommand set)
// Log: TestPilot.log beside the exe (state every 2 s while a task runs).
// testpilot-6 also reads MW5_LanceStatus (every hostile at any range, twice a second, with its actor address and
// whether it is on the PLAYER's sensors) and MW5_TerrainExport (the mech records: Locked = the game's own
// TargetTracking.CurrentTarget). Targets come from LanceStatus (fresh, any range), the lock is sent for the
// exact actor and only once the unit is on the player's sensors (else the game drops it at once), and "locked"
// means the game's own flag - not LidarExport's selection, which outlived a dropped lock in testpilot-5.
// =====================================================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <sstream>
#include "LidarShared.h"
#include "VanillaHudShared.h"
#include "LanceStatusShared.h"
#include "TerrainShared.h"
#include "TestPilotFire.h"

static const char  *kBuild = "testpilot-20"; // 2026-10-19: rebuilt for TERRAIN_VERSION 15 (grouped weapons) // 2026-10-17 bug sweep: seqlock reads take FrameCounterEnd first // // 2026-10-17: torso on TestPilot.dll's led / raised aim point (TpFireBlock v2) // // 2026-10-17: battle armour - "ba" report (+ suit records, -17) + watch, approach / lock ba // // 2026-10-16: rebuilt for LANCE_VERSION 7 (256 units) // approach [m] mech: mechs only (called-shot tests)
static std::wstring g_dir;
static FILE        *g_log = nullptr;

static void Log(const char *fmt, ...)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char    buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log)
    {
        fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

// ---- shared memory -----------------------------------------------------------------------------------------
static const LidarExportBlock *g_lidar = nullptr;
static VanillaExportBlock     *g_vhud  = nullptr; // mapped read/write ONLY for the order ring and EiVisionOn
static const LanceStatusBlock *g_lance = nullptr;
static const TerrainHeader    *g_terr  = nullptr;

static bool Attach()
{
    if (!g_lidar)
        if (HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, LIDAR_MMF_NAME))
            g_lidar = (const LidarExportBlock *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(LidarExportBlock));
    if (!g_vhud)
        if (HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, VHUD_MMF_NAME))
            g_vhud = (VanillaExportBlock *)MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(VanillaExportBlock));
    if (!g_lance)
        if (HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, LANCE_MMF_NAME))
            g_lance = (const LanceStatusBlock *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(LanceStatusBlock));
    if (!g_terr)
        if (HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, TERRAIN_MMF_NAME))
            g_terr = (const TerrainHeader *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(TerrainBlock)); // header + heights
    return g_lidar && g_vhud && g_lidar->Magic == LIDAR_MAGIC && g_vhud->Version == VHUD_VERSION;
}

// ---- every hostile (LanceStatus, 2 Hz, any range) and the game's own lock (Terrain mech records) -------
struct Unit
{
    uint64_t actor;
    float    x, y, z;
    bool     onMyRadar, seen;
    bool     detected = false; // testpilot-12: the game's detection (lock gate / radar mode test): SeenBy | lance | my radar
    int      los = -1; // filled by the pilot: 1 clear over the hills, 0 blocked, -1 unknown
};
static std::vector<Unit> ReadUnits()
{
    std::vector<Unit> out;
    for (int tries = 0; g_lance && g_lance->Version == LANCE_VERSION && tries < 8; ++tries)
    {
        const volatile LanceStatusBlock *b = g_lance;
        uint32_t s0 = b->UnitSeq;
        if (s0 & 1u) { Sleep(1); continue; }
        MemoryBarrier();
        out.clear();
        uint32_t n = b->UnitCount > (uint32_t)kLsMaxUnits ? (uint32_t)kLsMaxUnits : b->UnitCount;
        for (uint32_t i = 0; i < n; ++i)
        {
            const volatile LsUnit &u = b->Units[i];
            if (u.Dead)
                continue;
            out.push_back({((uint64_t)u.ActorHi << 32) | u.ActorLo, u.Loc[0], u.Loc[1], u.Loc[2],
                           (u.Flags & LSU_OnPlayerRadar) != 0, u.Seen != 0});
            out.back().detected = u.SeenBy || (u.Flags & (LSU_OnLanceRadar | LSU_OnPlayerRadar));
        }
        MemoryBarrier();
        if (s0 == b->UnitSeq)
            return out;
    }
    out.clear();
    return out;
}
// the actor the game has as the player's target (TerrainMechInfo.Locked), 0 = none / unknown
static uint64_t ReadGameLock(bool *known = nullptr)
{
    if (known) *known = false;
    for (int tries = 0; g_terr && g_terr->Version == TERRAIN_VERSION && tries < 8; ++tries)
    {
        const volatile TerrainHeader *h = g_terr;
        uint32_t f0 = h->FrameCounterEnd; // END first (bug sweep)
        MemoryBarrier();
        uint64_t got = 0;
        uint32_t n = h->MechInfoCount > (uint32_t)TERR_MAX_MECHS ? (uint32_t)TERR_MAX_MECHS : h->MechInfoCount;
        for (uint32_t i = 0; i < n; ++i)
            if (h->Mechs[i].Valid && h->Mechs[i].Locked)
                got = ((uint64_t)h->Mechs[i].ActorHi << 32) | h->Mechs[i].ActorLo;
        bool scanned = h->MechInfoScanned != 0;
        MemoryBarrier();
        if (f0 == h->FrameCounter)
        {
            if (known) *known = scanned;
            return got;
        }
        Sleep(1);
    }
    return 0;
}

struct State
{
    bool  ok = false;
    float x = 0, y = 0, z = 0, yaw = 0, torso = 0;
    std::vector<LidarContact> enemies;
    uint16_t selected[4] = {};
    int      selectedCount = 0;
    uint32_t scan = 0;
};

static State ReadLidar()
{
    State s;
    if (!g_lidar || g_lidar->Magic != LIDAR_MAGIC)
        return s;
    for (int tries = 0; tries < 8; ++tries)
    {
        const volatile LidarExportBlock *b = g_lidar;
        uint32_t f0 = b->FrameCounterEnd; // END first (bug sweep)
        MemoryBarrier();
        s.x = b->PlayerX; s.y = b->PlayerY; s.z = b->PlayerZ;
        s.yaw = b->PlayerYaw; s.torso = b->PlayerTorsoYaw;
        s.selectedCount = b->SelectedCount > 4 ? 4 : b->SelectedCount;
        for (int i = 0; i < 4; ++i)
            s.selected[i] = b->SelectedIds[i];
        s.scan = b->ScanCounter;
        uint32_t n = b->ContactCount > LIDAR_MAX_CONTACTS ? LIDAR_MAX_CONTACTS : b->ContactCount;
        s.enemies.clear();
        for (uint32_t i = 0; i < n; ++i)
        {
            LidarContact c;
            std::memcpy(&c, (const void *)&b->Contacts[i], sizeof(c));
            if (c.Type == LCT_HostileMech)
                s.enemies.push_back(c);
        }
        MemoryBarrier();
        if (f0 == b->FrameCounter)
        {
            s.ok = true;
            return s;
        }
        Sleep(1);
    }
    return s;
}

struct Nav
{
    float x, y, z;
    int   letter;
};
static std::vector<Nav> ReadNavs()
{
    std::vector<Nav> out;
    for (int tries = 0; g_vhud && tries < 8; ++tries)
    {
        const volatile VanillaExportBlock *b = g_vhud;
        uint32_t s0 = b->NavSeq;
        if (s0 & 1u) { Sleep(1); continue; }
        MemoryBarrier();
        out.clear();
        uint32_t n = b->NavCount > (uint32_t)VanillaExportBlock::kNavMax ? (uint32_t)VanillaExportBlock::kNavMax : b->NavCount;
        for (uint32_t i = 0; i < n; ++i)
            out.push_back({b->Navs[i].Loc[0], b->Navs[i].Loc[1], b->Navs[i].Loc[2], b->Navs[i].Letter});
        MemoryBarrier();
        if (s0 == b->NavSeq)
            return out;
    }
    out.clear();
    return out;
}

struct LiveLock
{
    bool     valid = false; // live bounds of the current target this frame
    float    x = 0, y = 0, z = 0;
    uint32_t state = 0;
    float    percent = 0;
};
static LiveLock ReadLiveLock()
{
    LiveLock l;
    for (int tries = 0; g_vhud && tries < 8; ++tries)
    {
        const volatile VanillaExportBlock::MissileLockInfo *m = &g_vhud->MissileLock;
        uint32_t s0 = m->Seq;
        if (s0 & 1u) { Sleep(1); continue; }
        MemoryBarrier();
        l.state   = m->State;
        l.percent = m->Percent;
        l.valid   = m->Bounds.Valid != 0;
        l.x = m->Bounds.Origin[0]; l.y = m->Bounds.Origin[1]; l.z = m->Bounds.Origin[2];
        MemoryBarrier();
        if (s0 == m->Seq)
            return l;
    }
    return LiveLock{};
}

// ---- the order ring (claim n by compare-exchange of the head, fill, Seq = n + 1 last) ------------------
static const char *OrderName(uint8_t o)
{
    switch (o)
    {
    case VanillaExportBlock::TCO_SelfMove: return "SelfMove";
    case VanillaExportBlock::TCO_SelfStop: return "SelfStop";
    case VanillaExportBlock::TCO_TorsoTarget: return "TorsoTarget";
    case VanillaExportBlock::TCO_TorsoPoint: return "TorsoPoint";
    case VanillaExportBlock::TCO_TorsoCentre: return "TorsoCentre";
    case VanillaExportBlock::TCO_TorsoRelease: return "TorsoRelease";
    case VanillaExportBlock::TCO_LockTarget: return "LockTarget";
    case VanillaExportBlock::TCO_CalledShot: return "CalledShot";
    default: return "?";
    }
}

static bool Post(uint8_t order, float x = 0, float y = 0, float z = 0, bool quiet = false)
{
    if (!g_vhud)
        return false;
    volatile LONG *head = (volatile LONG *)&g_vhud->TacCmdHead;
    uint32_t       n;
    for (;;)
    {
        n = (uint32_t)*head;
        if ((uint32_t)InterlockedCompareExchange(head, (LONG)(n + 1), (LONG)n) == n)
            break;
    }
    VanillaExportBlock::TacCmd *c = &g_vhud->TacCmds[n % VanillaExportBlock::kTacCmdRing];
    c->Seq = 0;
    MemoryBarrier();
    c->Order  = order;
    c->Slot   = 1; // the player's own orders ignore the slot, but it must be a valid one (1..8)
    c->Pad[0] = c->Pad[1] = 0;
    c->Loc[0] = x; c->Loc[1] = y; c->Loc[2] = z;
    MemoryBarrier();
    c->Seq = n + 1;
    if (!quiet)
        Log("order %s (%.0f, %.0f, %.0f) m  [ring %u]", OrderName(order), x * 0.01, y * 0.01, z * 0.01, n + 1);
    return true;
}

// testpilot-12: any order with a slot and the two pad bytes (TCO_CalledShot: Pad[0] part, Pad[1] 1 = your lock)
static bool PostEx(uint8_t order, int slot, uint8_t pad0, uint8_t pad1, float x = 0, float y = 0, float z = 0)
{
    if (!g_vhud || slot < 1 || slot > VanillaExportBlock::kTacMaxMembers)
        return false;
    volatile LONG *head = (volatile LONG *)&g_vhud->TacCmdHead;
    uint32_t       n;
    for (;;)
    {
        n = (uint32_t)*head;
        if ((uint32_t)InterlockedCompareExchange(head, (LONG)(n + 1), (LONG)n) == n)
            break;
    }
    VanillaExportBlock::TacCmd *c = &g_vhud->TacCmds[n % VanillaExportBlock::kTacCmdRing];
    c->Seq = 0;
    MemoryBarrier();
    c->Order  = order;
    c->Slot   = (uint8_t)slot;
    c->Pad[0] = pad0;
    c->Pad[1] = pad1;
    c->Loc[0] = x; c->Loc[1] = y; c->Loc[2] = z;
    MemoryBarrier();
    c->Seq = n + 1;
    Log("order %s slot %d pad %u/%u  [ring %u]", OrderName(order), slot, pad0, pad1, n + 1);
    return true;
}

// testpilot-12: REPORT - every unit record the game publishes (TerrainExport): chassis, the game's lock, and per
// part armour / structure (H CT LT RT LA RA LL RL, percent), so a called shot can be checked by the part it hits.
static void Report()
{
    if (!g_terr || g_terr->Version != TERRAIN_VERSION)
    {
        Log("report: no terrain feed");
        return;
    }
    static TerrainMechInfo m[TERR_MAX_MECHS];
    uint32_t n = 0;
    bool     got = false;
    for (int tries = 0; tries < 8 && !got; ++tries)
    {
        const volatile TerrainHeader *h = g_terr;
        uint32_t f0 = h->FrameCounterEnd; // END first (bug sweep)
        MemoryBarrier();
        n = h->MechInfoCount > (uint32_t)TERR_MAX_MECHS ? (uint32_t)TERR_MAX_MECHS : h->MechInfoCount;
        std::memcpy(m, (const void *)h->Mechs, sizeof(TerrainMechInfo) * n);
        MemoryBarrier();
        got = f0 == h->FrameCounter;
        if (!got) Sleep(1);
    }
    if (!got)
    {
        Log("report: terrain feed busy");
        return;
    }
    auto units = ReadUnits();
    int det = 0;
    for (auto &u : units) det += u.detected;
    Log("report: %u unit records; hostiles alive %d, detected by the game %d", n, (int)units.size(), det);
    for (uint32_t i = 0; i < n; ++i)
    {
        const TerrainMechInfo &r = m[i];
        if (!r.Valid || !r.Hostile)
            continue;
        const uint64_t actor = ((uint64_t)r.ActorHi << 32) | r.ActorLo;
        int detected = -1;
        for (auto &u : units)
            if (u.actor == actor) detected = u.detected;
        char parts[200] = "";
        if (r.HaveCondition)
        {
            static const char *kN[TMP_COUNT] = {"H", "CT", "LT", "RT", "LA", "RA", "LL", "RL"};
            int o = 0;
            for (int k = 0; k < TMP_COUNT && o < (int)sizeof(parts) - 20; ++k)
                o += snprintf(parts + o, sizeof(parts) - o, " %s %.0f/%.0f", kN[k], r.Armour[k] * 100.0f, r.Structure[k] * 100.0f);
        }
        Log("  %s %s%s%s detected %d:%s", r.Chassis, r.Variant, r.Locked ? " [LOCKED]" : "", r.Dead ? " [DEAD]" : "", detected,
            r.HaveCondition ? parts : " (no condition)");
    }
}

// testpilot-13: is this actor a live MECH (TerrainExport record)? - "lock mech" skips turrets and vehicles
static bool IsMechActor(uint64_t actor)
{
    if (!g_terr || g_terr->Version != TERRAIN_VERSION)
        return false;
    const volatile TerrainHeader *h = g_terr;
    uint32_t n = h->MechInfoCount > (uint32_t)TERR_MAX_MECHS ? (uint32_t)TERR_MAX_MECHS : h->MechInfoCount;
    for (uint32_t i = 0; i < n; ++i)
        if (h->Mechs[i].Valid && !h->Mechs[i].Dead && h->Mechs[i].Kind == TUK_Mech &&
            (((uint64_t)h->Mechs[i].ActorHi << 32) | h->Mechs[i].ActorLo) == actor)
            return true;
    return false;
}

// testpilot-12: a lock sent by "lock undetected" is checked 3 s later against the game's own lock
static uint64_t  g_lockCheckActor = 0;
static ULONGLONG g_lockCheckAt = 0;

static bool PostLockActor(uint64_t actor)
{
    float f[3];
    uint32_t lo = (uint32_t)actor, hi = (uint32_t)(actor >> 32);
    std::memcpy(&f[0], &lo, 4);
    std::memcpy(&f[1], &hi, 4);
    f[2] = VanillaExportBlock::kTacLocActor;
    if (!g_vhud)
        return false;
    volatile LONG *head = (volatile LONG *)&g_vhud->TacCmdHead;
    uint32_t       n;
    for (;;)
    {
        n = (uint32_t)*head;
        if ((uint32_t)InterlockedCompareExchange(head, (LONG)(n + 1), (LONG)n) == n)
            break;
    }
    VanillaExportBlock::TacCmd *c = &g_vhud->TacCmds[n % VanillaExportBlock::kTacCmdRing];
    c->Seq = 0;
    MemoryBarrier();
    c->Order = VanillaExportBlock::TCO_LockTarget;
    c->Slot  = 1;
    c->Pad[0] = c->Pad[1] = 0;
    std::memcpy((void *)c->Loc, f, sizeof(f));
    MemoryBarrier();
    c->Seq = n + 1;
    Log("order LockTarget (exact unit %08x%08x)  [ring %u]", hi, lo, n + 1);
    return true;
}

// ---- LINE OF SIGHT over the terrain heightfield (TerrainExport tiles, 16 m samples) - testpilot-9 ---------
// The player's own line-of-sight flag is not exported, but the ground is: a ray from the cockpit to the
// target's middle, sampled every 8 m; blocked where the ground stands more than 3 m above it. Buildings and
// trees are not in the heightfield (a known gap - the result is "the hills do not hide it").
static int Los(float ax, float ay, float az, float bx, float by, float bz)
{
    if (!g_terr || g_terr->Version != TERRAIN_VERSION)
        return -1;
    const TerrainBlock *tb = (const TerrainBlock *)g_terr;
    struct Slot { int32_t x, y; uint32_t i; };
    std::vector<Slot> slots;
    for (int tries = 0; tries < 4 && slots.empty(); ++tries)
    {
        uint32_t f0 = g_terr->FrameCounterEnd; // END first (bug sweep)
        MemoryBarrier();
        for (uint32_t i = 0; i < (uint32_t)TERR_MAX_TILES; ++i)
            if (g_terr->Slots[i].State == TTS_Complete && g_terr->Slots[i].Hits)
                slots.push_back({g_terr->Slots[i].TileX, g_terr->Slots[i].TileY, i});
        MemoryBarrier();
        if (f0 != g_terr->FrameCounter)
            slots.clear();
    }
    if (slots.empty())
        return -1;
    auto ground = [&](float x, float y, float &z) -> bool {
        int32_t tx = (int32_t)floorf(x / TERR_TILE_CM), ty = (int32_t)floorf(y / TERR_TILE_CM);
        for (auto &sl : slots)
            if (sl.x == tx && sl.y == ty)
            {
                int col = (int)((x - tx * TERR_TILE_CM) / TERR_SPACING_CM), row = (int)((y - ty * TERR_TILE_CM) / TERR_SPACING_CM);
                col = col < 0 ? 0 : (col >= TERR_SAMPLES ? TERR_SAMPLES - 1 : col);
                row = row < 0 ? 0 : (row >= TERR_SAMPLES ? TERR_SAMPLES - 1 : row);
                z = tb->Tiles[sl.i].Height[row * TERR_SAMPLES + col];
                return z > TERR_NO_HEIGHT * 0.5f;
            }
        return false;
    };
    const double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    const int    n   = (int)(len / 800.0);
    int          known = 0;
    for (int i = 1; i < n; ++i) // skip the two ends (the mechs' own ground)
    {
        double t = (double)i / n;
        float  x = (float)(ax + (bx - ax) * t), y = (float)(ay + (by - ay) * t), rz = (float)(az + (bz - az) * t), gz;
        if (!ground(x, y, gz))
            continue;
        ++known;
        if (gz > rz + 300.0f)
            return 0;
    }
    return known ? 1 : -1;
}

// allies near the line from us to the target (LidarExport, ~10 Hz) - no firing through a lancemate
static bool AllyInLine(const State &s, float bx, float by)
{
    if (!g_lidar)
        return false;
    const uint32_t n = g_lidar->AllyCount > LIDAR_MAX_ALLIES ? LIDAR_MAX_ALLIES : g_lidar->AllyCount;
    const double   dx = bx - s.x, dy = by - s.y, L2 = dx * dx + dy * dy;
    for (uint32_t i = 0; i < n && L2 > 1.0; ++i)
    {
        const LidarContact &a = g_lidar->Allies[i];
        double t = ((a.X - s.x) * dx + (a.Y - s.y) * dy) / L2;
        if (t <= 0.02 || t >= 1.0)
            continue;
        double px = s.x + dx * t - a.X, py = s.y + dy * t - a.Y;
        if (px * px + py * py < 1500.0 * 1500.0) // within 15 m of the line
            return true;
    }
    return false;
}

// ---- WEAPONS: the test pilot's own in-game part, TestPilot.dll (own block MW5_TestPilotFire) - testpilot-10 ----
static TpFireBlock *g_fire = nullptr;
static bool FireReady()
{
    if (!g_fire)
        if (HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, TP_FIRE_MMF_NAME))
            g_fire = (TpFireBlock *)MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(TpFireBlock));
    return g_fire && g_fire->Magic == TP_FIRE_MAGIC && g_fire->Version == TP_FIRE_VERSION &&
           (uint32_t)(GetTickCount64() / 1000) - g_fire->PluginAlive < 5;
}
static void RequestFire(float secs, float heatCap = 0.85f, float maxAim = 4.0f)
{
    if (!FireReady())
    {
        static ULONGLONG warned = 0;
        if (GetTickCount64() - warned > 10000)
            warned = GetTickCount64(), Log("weapons: TestPilot.dll is not running in the game - no firing");
        return;
    }
    uint32_t seq = g_fire->ReqSeq;
    g_fire->ReqSeq = seq | 1u; // odd: being written
    MemoryBarrier();
    g_fire->ReqSecs = secs;
    g_fire->ReqHeatCap = heatCap;
    g_fire->ReqMaxAimDeg = maxAim;
    MemoryBarrier();
    g_fire->ReqSeq = (seq | 1u) + 1; // even: complete, a new request
    Log("fire request: %.1f s%s", secs, secs > 0 ? "" : " (cease fire)");
}

static void SetVision(bool on)
{
    if (!g_vhud)
        return;
    g_vhud->EiVisionOn = on ? 1u : 0u;
    Log("vision %s (EiVisionOn = %u)", on ? "ON" : "off", on ? 1u : 0u);
}

// ---- maths --------------------------------------------------------------------------------------------
static double Dist2D(double ax, double ay, double bx, double by) { return sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)) * 0.01; }
static const LidarContact *Nearest(const State &s)
{
    const LidarContact *best = nullptr;
    double              bd   = 1e18;
    for (auto &e : s.enemies)
    {
        double d = Dist2D(s.x, s.y, e.X, e.Y);
        if (d < bd) bd = d, best = &e;
    }
    return best;
}

// ---- the task -----------------------------------------------------------------------------------------
enum Task { Idle, Approach, GoNav };
struct Pilot
{
    Task      task = Idle;
    double    stopM = 300, holdS = 0;
    bool      mechsOnly = false;         // testpilot-14: "approach [m] mech" - only mechs are picked
    bool      baOnly = false;            // testpilot-16: "approach [m] ba" - only battle armour suits are picked
    uint16_t  target = 0;               // 1 while an enemy is the goal (testpilot-6: actor below)
    uint64_t  actor = 0;                // the target unit's own address (LanceStatus / Terrain records)
    ULONGLONG lastWaitLog = 0, lastCloseIn = 0, lastRetarget = 0, lastFire = 0, lastFireLog = 0;
    bool      weaponsFree = true;   // approach fires (TestPilot.dll) once locked and in sight; "weapons hold" stops it
    double    fireRangeM = 700;
    double    closeIn = 0;
    float     tx = 0, ty = 0, tz = 0; // where the target was (scan) or is (live bounds)
    float     goalX = 0, goalY = 0;   // the autopilot's current point
    bool      moving = false, torsoOn = false, locked = false;
    int       lockTries = 0;
    ULONGLONG lastLockTry = 0, lockedAt = 0, startedAt = 0;
    uint32_t  scanAtTry = 0;
    int       navLetter = -1;
    float     nx = 0, ny = 0, nz = 0;
    ULONGLONG lastStatus = 0;
    double    lastDist = 1e18;
    ULONGLONG lastProgress = 0;
    // testpilot-11: a target not seen for target_timeout_s (no line of sight, not on our sensors, not locked)
    // is dropped and avoided for target_ignore_s (only picked again when nothing else is left)
    double    timeoutS = 30, ignoreS = 60;
    ULONGLONG lastSeen = 0;
    std::vector<std::pair<uint64_t, ULONGLONG>> dropped; // actor, avoided until
    // testpilot-11: LEGS FOLLOW THE TORSO - the twist limit is learned (the torso stops short of the target)
    float     twistLimit = 0;        // learned, degrees (0 = not yet: 60 assumed)
    ULONGLONG pinnedSince = 0, pivotAt = 0;
    bool      pivoting = false;
};

static Pilot Fresh(const Pilot &p)
{
    Pilot q;
    q.stopM = p.stopM;
    q.holdS = p.holdS;
    q.weaponsFree = p.weaponsFree;
    return q;
}

static void StopAll(Pilot &p, const char *why)
{
    if (p.task != Idle || p.moving)
        Post(VanillaExportBlock::TCO_SelfStop);
    if (p.task != Idle || p.torsoOn)
        Post(VanillaExportBlock::TCO_TorsoRelease);
    if (p.lastFire)
        RequestFire(0.0f); // cease fire
    p = Fresh(p);
    Log("stopped: %s", why);
}

static void Status(const Pilot &p, const State &s)
{
    LiveLock l = ReadLiveLock();
    auto     units = ReadUnits();
    int      radar = 0;
    double   nearest = -1;
    for (auto &u : units)
    {
        radar += u.onMyRadar;
        double d = Dist2D(s.x, s.y, u.x, u.y);
        if (nearest < 0 || d < nearest) nearest = d;
    }
    const uint64_t gl = ReadGameLock();
    double         td = p.actor ? Dist2D(s.x, s.y, p.tx, p.ty) : -1;
    const int      tl = p.actor ? Los(s.x, s.y, s.z + 900.0f, p.tx, p.ty, p.tz + 300.0f) : -1;
    if (FireReady())
        Log("weapons: TestPilot.dll alive, state %u, %u weapon start(s), aim %.1f deg, %.0f m, heat %.0f %% - %s", g_fire->State,
            g_fire->Shots, g_fire->AimErrDeg, g_fire->DistM, g_fire->Heat * 100.0f, g_fire->Last);
    Log("status: task %s  pos (%.0f, %.0f, %.0f) m  body %.1f  torso %.1f  hostiles %d (on my sensors %d, nearest %.0f m)  target %.0f m (sight %d)  game lock %s%s  live bounds %s  missile lock %u %.0f%%  vision %u",
        p.task == Idle ? "idle" : p.task == Approach ? "approach" : "nav", s.x * 0.01, s.y * 0.01, s.z * 0.01, s.yaw, s.torso,
        (int)units.size(), radar, nearest, td, tl, gl ? "yes" : "no", gl && gl == p.actor ? " (ours)" : "", l.valid ? "yes" : "no", l.state,
        l.percent * 100.0, g_vhud ? g_vhud->EiVisionOn : 0u);
}

// approach (testpilot-8): walk toward the TARGET's own position - it stands on walkable ground, where a point
// on the line short of it can be a slope with no navmesh ("no walkable ground near", -6 / -7) - and stop the
// autopilot once within stopM. Re-planned only when the target moved 100 m; no progress for 6 s re-sends it.
static void MoveShortOf(Pilot &p, const State &s, float x, float y, float z, double stopM)
{
    const double    d   = Dist2D(s.x, s.y, x, y);
    const ULONGLONG now = GetTickCount64();
    if (d <= stopM)
    {
        if (p.moving)
        {
            Post(VanillaExportBlock::TCO_SelfStop);
            p.moving = false;
            Log("within %.0f m of the target (%.0f m) - autopilot off", stopM, d);
        }
        return;
    }
    if (d > stopM + 40.0 || p.moving)
    {
        const bool stuck = p.moving && now - p.lastProgress > 6000;
        if (!p.moving || stuck || Dist2D(x, y, p.goalX, p.goalY) > 100.0)
        {
            if (stuck)
                Log("autopilot made no progress for 6 s - sending the move again");
            Post(VanillaExportBlock::TCO_SelfMove, x, y, z);
            p.goalX = x; p.goalY = y;
            p.moving = true;
            p.lastProgress = now;
            p.lastDist = d;
        }
        if (d < p.lastDist - 10.0)
            p.lastDist = d, p.lastProgress = now;
    }
}

// ---- LEGS FOLLOW THE TORSO (testpilot-11) ------------------------------------------------------------------
// Standing still (inside stop_m) the legs never turned, so a target moving round us ran the torso into its
// twist stop. Now: the target's bearing off the BODY is watched; past (twist limit - 15 deg) the legs are turned
// toward it with a short autopilot walk (40 m toward the target), stopped once it is within 15 deg ahead.
// The limit is each mech's own: learned when the torso sits still short of the target (60 deg until then).
static double Wrap180(double a)
{
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}
// ---- BATTLE ARMOUR (testpilot-16, user 2026-10-17) ----------------------------------------------------
// LidarExport's suits (LK_BattleArmor, ba1): hostile ones ride the ~3 s contact scan, friendly ones the ~10 Hz ally
// list. Team byte 0xFF on a hostile suit = LidarExport read no team (it then counts as hostile) - the TeamID check.
// LanceStatus units carry no kind, so a unit is a suit when a hostile suit contact lies within 20 m of it.
struct BaSuit
{
    float   x, y, z;
    bool    friendly;
    uint8_t team;
};
static std::vector<BaSuit> ReadBa(std::vector<LidarContact> *alliesOut = nullptr)
{
    std::vector<BaSuit> out;
    if (!g_lidar || g_lidar->Magic != LIDAR_MAGIC)
        return out;
    for (int tries = 0; tries < 8; ++tries)
    {
        const volatile LidarExportBlock *b = g_lidar;
        uint32_t f0 = b->FrameCounterEnd; // END first (bug sweep)
        MemoryBarrier();
        out.clear();
        if (alliesOut) alliesOut->clear();
        uint32_t n = b->ContactCount > LIDAR_MAX_CONTACTS ? LIDAR_MAX_CONTACTS : b->ContactCount;
        for (uint32_t i = 0; i < n; ++i)
        {
            LidarContact c;
            std::memcpy(&c, (const void *)&b->Contacts[i], sizeof(c));
            if (c.Kind == LK_BattleArmor)
                out.push_back({c.X, c.Y, c.Z, c.Type == LCT_FriendlyMech, c.Team});
        }
        uint32_t na = b->AllyCount > LIDAR_MAX_ALLIES ? LIDAR_MAX_ALLIES : b->AllyCount;
        for (uint32_t i = 0; i < na; ++i)
        {
            LidarContact c;
            std::memcpy(&c, (const void *)&b->Allies[i], sizeof(c));
            if (c.Kind == LK_BattleArmor)
                out.push_back({c.X, c.Y, c.Z, true, c.Team});
            else if (alliesOut)
                alliesOut->push_back(c);
        }
        MemoryBarrier();
        if (f0 == b->FrameCounter)
            return out;
        Sleep(1);
    }
    out.clear();
    return out;
}
static bool IsBaUnit(const Unit &u, const std::vector<BaSuit> &ba)
{
    for (auto &b : ba)
        if (!b.friendly && Dist2D(u.x, u.y, b.x, b.y) < 20.0)
            return true;
    return false;
}
// the report (logged when force is set); returns a short signature so the watch logs only changes
static std::string BaReport(const State &s, bool force)
{
    std::vector<LidarContact> allies;
    auto ba    = ReadBa(&allies);
    auto units = ReadUnits();
    int  hostile = 0, friendly = 0, noTeam = 0, inFeed = 0, onFriend = 0;
    std::string teams;
    double nd = 1e18, nbrg = 0;
    for (auto &b : ba)
    {
        if (b.friendly) { ++friendly; continue; }
        ++hostile;
        if (b.team == 0xFF) ++noTeam;
        char t[8];
        snprintf(t, sizeof(t), "%u", (unsigned)b.team);
        if (("," + teams).find(std::string(",") + t + ",") == std::string::npos)
            teams += std::string(t) + ",";
        for (auto &u : units)
            if (Dist2D(u.x, u.y, b.x, b.y) < 20.0) { ++inFeed; break; }
        // LanceControl's 15 m rule: a suit within 15 m flat of the player or a friendly unit
        bool byFriend = Dist2D(s.x, s.y, b.x, b.y) < 15.0; // ("near" is a windows.h macro)
        for (auto &a : allies)
            if (!byFriend && Dist2D(a.X, a.Y, b.x, b.y) < 15.0) byFriend = true;
        onFriend += byFriend;
        double d = Dist2D(s.x, s.y, b.x, b.y);
        if (d < nd) nd = d, nbrg = Wrap180(atan2((double)b.y - s.y, (double)b.x - s.x) * 57.29578 - s.yaw);
    }
    if (!teams.empty()) teams.pop_back();
    // the per-unit records (LidarExport ba4): suits published with a name / HP like every other enemy
    int recs = 0, recHp = 0;
    char recName[32] = "";
    if (g_terr && g_terr->Version == TERRAIN_VERSION)
    {
        const volatile TerrainHeader *h = g_terr;
        uint32_t n = h->MechInfoCount > (uint32_t)TERR_MAX_MECHS ? (uint32_t)TERR_MAX_MECHS : h->MechInfoCount;
        for (uint32_t i = 0; i < n; ++i)
            if (h->Mechs[i].Valid && !h->Mechs[i].Dead && h->Mechs[i].Kind == TUK_BattleArmor)
            {
                ++recs;
                recHp += h->Mechs[i].HaveCondition != 0;
                if (!recName[0])
                    for (int k = 0; k < 31; ++k)
                        recName[k] = h->Mechs[i].Chassis[k];
            }
    }
    char sig[64];
    snprintf(sig, sizeof(sig), "%d/%d/%d/%d/%d/%d", hostile, friendly, noTeam, inFeed, onFriend, recs);
    if (force)
    {
        char nearTxt[64] = "";
        if (hostile) snprintf(nearTxt, sizeof(nearTxt), ", nearest %.0f m at %+.0f deg off the legs", nd, nbrg);
        Log("ba: hostile suits %d (team bytes %s%s), friendly suits %d%s; hostile suits in the lance feed %d of %d; "
            "within 15 m of you / a friendly %d; suit records %d (with HP %d, name '%s')",
            hostile, teams.empty() ? "-" : teams.c_str(), noTeam ? " - NO TEAM READ on some" : "", friendly, nearTxt, inFeed,
            hostile, onFriend, recs, recHp, recName);
    }
    return sig;
}

static void LegsFollow(Pilot &p, const State &s, double d, ULONGLONG now)
{
    const double brg = atan2((double)p.ty - s.y, (double)p.tx - s.x) * 57.29578;
    const double rel = Wrap180(brg - s.yaw);          // the target off the legs' heading
    const double tw  = fabs((double)s.torso);
    // learn the twist limit: the torso held still well short of the target for 1.5 s
    if (p.torsoOn && tw > 25.0 && fabs(rel) > tw + 10.0)
    {
        if (!p.pinnedSince) p.pinnedSince = now;
        else if (now - p.pinnedSince > 1500 && fabs(tw - p.twistLimit) > 2.0)
        {
            p.twistLimit = (float)tw;
            Log("torso twist limit of this mech: %.0f deg (learned - the torso stopped there, target %.0f deg off the legs)", tw, rel);
        }
    }
    else
        p.pinnedSince = 0;
    const double guard = (p.twistLimit > 0 ? p.twistLimit : 60.0) - 15.0;
    if (p.moving) // the autopilot walks the legs toward the target already
    {
        p.pivoting = false;
        return;
    }
    if (p.pivoting)
    {
        if (fabs(rel) < 15.0 || now - p.pivotAt > 8000)
        {
            Post(VanillaExportBlock::TCO_SelfStop);
            p.pivoting = false;
            Log("legs %s (target %.0f deg off the legs)", fabs(rel) < 15.0 ? "turned onto the target" : "turn timed out", rel);
        }
        return;
    }
    if (fabs(rel) > guard && now - p.pivotAt > 3000)
    {
        const double step = d > 60.0 ? 40.0 : d * 0.5; // metres toward the target
        const double r = brg / 57.29578;
        p.pivotAt = now;
        p.pivoting = true;
        Post(VanillaExportBlock::TCO_SelfMove, (float)(s.x + cos(r) * step * 100.0), (float)(s.y + sin(r) * step * 100.0), s.z);
        Log("target %.0f deg off the legs (torso %.0f, limit %.0f%s) - turning the legs toward it", rel, s.torso,
            p.twistLimit > 0 ? p.twistLimit : 60.0, p.twistLimit > 0 ? "" : " assumed");
    }
}

static void Tick(Pilot &p)
{
    State s = ReadLidar();
    if (!s.ok || p.task == Idle)
        return;
    const ULONGLONG now = GetTickCount64();

    if (p.task == GoNav)
    {
        double d = Dist2D(s.x, s.y, p.nx, p.ny);
        if (!p.moving)
        {
            Post(VanillaExportBlock::TCO_SelfMove, p.nx, p.ny, p.nz);
            Post(VanillaExportBlock::TCO_TorsoCentre);
            p.moving = true;
            p.lastProgress = now;
            p.lastDist = d;
        }
        if (d < 60.0)
        {
            Log("nav point %c reached", 'A' + p.navLetter);
            StopAll(p, "nav point reached");
            return;
        }
    }
    else if (p.task == Approach)
    {
        auto units = ReadUnits();
        if (!p.actor)
        {
            const std::vector<BaSuit> baList = p.baOnly ? ReadBa() : std::vector<BaSuit>();
            // the nearest live hostile, any range (LanceStatus); none at all -> a nav point meanwhile
            // testpilot-9: prefer what we can SEE - line of sight over the terrain, then on our sensors, then nearest
            const Unit *best = nullptr;
            double      bd = 1e18, bscore = 1e18;
            for (auto &u : units)
            {
                if (p.mechsOnly && !IsMechActor(u.actor))
                    continue; // testpilot-14: vehicles and turrets have no legs / arms to call
                if (p.baOnly && !IsBaUnit(u, baList))
                    continue; // testpilot-16: battle armour only
                double d = Dist2D(s.x, s.y, u.x, u.y);
                u.los = Los(s.x, s.y, s.z + 900.0f, u.x, u.y, u.z + 300.0f);
                double score = d + (u.los == 1 ? 0.0 : 3000.0) + (u.onMyRadar ? 0.0 : 1500.0);
                for (auto &dr : p.dropped)
                    if (dr.first == u.actor && now < dr.second)
                        score += 100000.0; // testpilot-11: timed out lately - only when nothing else is left
                if (score < bscore) bscore = score, bd = d, best = &u;
            }
            if (best)
            {
                p.actor = best->actor;
                p.target = 1;
                p.tx = best->x; p.ty = best->y; p.tz = best->z;
                p.moving = false; // a nav leg (if any) is replaced by the approach
                p.lockTries = 0;
                p.torsoOn = false;
                p.lastSeen = now;
                Log("target: hostile unit %016llx at %.0f m (line of sight %d, on my sensors %d, seen by the lance %d)",
                    (unsigned long long)p.actor, bd, best->los, best->onMyRadar, best->seen);
            }
            else if (p.navLetter < 0)
            {
                auto       navs = ReadNavs();
                const Nav *nn   = nullptr;
                double     nb   = 1e18;
                for (auto &v : navs)
                {
                    double d = Dist2D(s.x, s.y, v.x, v.y);
                    if (d > 150.0 && d < nb) nb = d, nn = &v;
                }
                if (nn)
                {
                    p.navLetter = nn->letter;
                    p.nx = nn->x; p.ny = nn->y; p.nz = nn->z;
                    Log("no hostile in the mission list - walking to nav point %c (%.0f m)", 'A' + nn->letter, nb);
                    Post(VanillaExportBlock::TCO_SelfMove, p.nx, p.ny, p.nz);
                    Post(VanillaExportBlock::TCO_TorsoCentre);
                    p.moving = true;
                    p.lastProgress = now;
                    p.lastDist = nb;
                }
                else if (now - p.lastStatus > 5000)
                    Log("no hostile and no nav point - waiting");
            }
            else if (Dist2D(s.x, s.y, p.nx, p.ny) < 60.0)
            {
                StopAll(p, "nav point reached, no hostile");
                return;
            }
        }
        if (p.actor)
        {
            const Unit *me = nullptr;
            for (auto &u : units)
                if (u.actor == p.actor) me = &u;
            if (!me && !units.empty())
            {
                Log("target %016llx gone (destroyed or out of the world) - picking again", (unsigned long long)p.actor);
                p.actor = 0; p.target = 0; p.locked = false; p.torsoOn = false;
                return;
            }
            LiveLock live = ReadLiveLock();
            if (me)
                p.tx = me->x, p.ty = me->y, p.tz = me->z;
            if (p.locked && live.valid) // live bounds of OUR lock: every frame, better than 2 Hz
                p.tx = live.x, p.ty = live.y, p.tz = live.z;
            const bool onRadar = me && me->onMyRadar;
            // testpilot-7: not on our sensors (behind a hill) -> close in 100 m every 10 s, down to 100 m
            if (onRadar || p.locked)
                p.closeIn = 0, p.lastCloseIn = now;
            else if (now - p.lastCloseIn > 10000 && p.stopM - p.closeIn > 100.0 && !p.moving)
            {
                p.lastCloseIn = now;
                p.closeIn += 100.0;
                Log("not on my sensors at %.0f m - closing in to %.0f m", Dist2D(s.x, s.y, p.tx, p.ty), p.stopM - p.closeIn);
            }
            MoveShortOf(p, s, p.tx, p.ty, p.tz, p.stopM - p.closeIn);
            const double d = Dist2D(s.x, s.y, p.tx, p.ty);
            LegsFollow(p, s, d, now);
            // torso on it once it is on our sensors (or within 1000 m) - before that it is behind hills anyway
            if (!p.torsoOn && (onRadar || d < 1000.0))
            {
                Post(VanillaExportBlock::TCO_TorsoTarget, p.tx, p.ty, p.tz);
                p.torsoOn = true;
            }
            // the game's own lock (Terrain mech records), not LidarExport's selection
            bool           known    = false;
            const uint64_t gameLock = ReadGameLock(&known);
            const bool     lockedNow = gameLock == p.actor;
            if (lockedNow && !p.locked)
            {
                p.locked = true;
                p.lockedAt = now;
                Log("LOCKED (the game's own target): %016llx at %.0f m after %d tr%s", (unsigned long long)p.actor, d,
                    p.lockTries, p.lockTries == 1 ? "y" : "ies");
            }
            // testpilot-18: LEAD AND DROP - once locked, the torso follows TestPilot.dll's aim point (the lock's aim point,
            // led and raised for the in-range projectile weapon needing it most), re-posted every 750 ms; logged every 10 s
            if (lockedNow && FireReady() && g_fire->AimOn)
            {
                static uint32_t  aimSeq = 0;
                static ULONGLONG aimPost = 0, aimLog = 0;
                if (g_fire->AimSeq != aimSeq && now - aimPost >= 750)
                {
                    aimSeq = g_fire->AimSeq;
                    aimPost = now;
                    Post(VanillaExportBlock::TCO_TorsoPoint, g_fire->AimPt[0], g_fire->AimPt[1], g_fire->AimPt[2], true);
                    if (now - aimLog > 10000)
                    {
                        aimLog = now;
                        char who[49] = {};
                        std::memcpy(who, (const void *)g_fire->AimWho, 48);
                        Log("aim: torso on the led / raised point - %.1f m of correction (%s)", g_fire->AimCorrM, who);
                    }
                }
            }
            if (!lockedNow && p.locked)
            {
                p.locked = false;
                Log("lock lost on %016llx (game target now %016llx)", (unsigned long long)p.actor, (unsigned long long)gameLock);
            }
            // lock only once it is on the PLAYER's sensors (the game drops a lock on a unit it does not know),
            // the exact unit, at most every 3 s, up to 8 tries
            if (!lockedNow && onRadar && now - p.lastLockTry > 3000)
            {
                if (p.lockTries >= 8)
                {
                    StopAll(p, "could not lock the target in 8 tries");
                    return;
                }
                ++p.lockTries;
                p.lastLockTry = now;
                PostLockActor(p.actor);
                Post(VanillaExportBlock::TCO_TorsoTarget, p.tx, p.ty, p.tz); // re-aim at the fresh position
            }
            if (!onRadar && !p.locked && now - p.lastWaitLog > 5000)
            {
                p.lastWaitLog = now;
                Log("target %.0f m away, not on my sensors yet - lock waits", d);
            }
            // testpilot-9: no line of sight to ours but another one in sight -> switch (every 4 s, not while firing)
            const int los = Los(s.x, s.y, s.z + 900.0f, p.tx, p.ty, p.tz + 300.0f);
            // testpilot-11: not seen for a while -> drop it and avoid it for a while
            if (los == 1 || onRadar || p.locked)
                p.lastSeen = now;
            else if (p.timeoutS > 0 && now - p.lastSeen > (ULONGLONG)(p.timeoutS * 1000.0))
            {
                Log("target %016llx not seen for %.0f s (no line of sight, not on my sensors) - dropped for %.0f s, picking again",
                    (unsigned long long)p.actor, p.timeoutS, p.ignoreS);
                for (size_t i = p.dropped.size(); i-- > 0;)
                    if (p.dropped[i].first == p.actor || now >= p.dropped[i].second)
                        p.dropped.erase(p.dropped.begin() + i);
                p.dropped.push_back({p.actor, now + (ULONGLONG)(p.ignoreS * 1000.0)});
                p.actor = 0; p.target = 0; p.locked = false; p.torsoOn = false; p.lockTries = 0; p.closeIn = 0;
                return;
            }
            if (los == 0 && now - p.lastRetarget > 4000)
            {
                p.lastRetarget = now;
                for (auto &u : units)
                    if (u.actor != p.actor && Dist2D(s.x, s.y, u.x, u.y) < 1200.0 && Los(s.x, s.y, s.z + 900.0f, u.x, u.y, u.z + 300.0f) == 1)
                    {
                        Log("no line of sight to %016llx - switching to %016llx (in sight, %.0f m)", (unsigned long long)p.actor,
                            (unsigned long long)u.actor, Dist2D(s.x, s.y, u.x, u.y));
                        p.actor = 0; p.target = 0; p.locked = false; p.torsoOn = false; p.lockTries = 0;
                        return;
                    }
            }
            // WEAPONS (testpilot-10: TestPilot.dll, its own in-game part): locked by the game, in sight, in range,
            // nobody of ours in the line -> 2 s bursts; LanceControl checks each weapon, the aim and the heat
            if (p.weaponsFree && p.locked && los != 0 && d < p.fireRangeM && now - p.lastFire > 2000)
            {
                if (AllyInLine(s, p.tx, p.ty))
                {
                    if (now - p.lastFireLog > 3000)
                        p.lastFireLog = now, Log("holding fire - a lancemate is in the line");
                }
                else
                {
                    p.lastFire = now;
                    RequestFire(2.0f);
                }
            }
            if (p.locked && p.holdS > 0 && now - p.lockedAt > (ULONGLONG)(p.holdS * 1000))
            {
                StopAll(p, "hold time over");
                return;
            }
        }
    }
    // no progress for 20 s while walking -> send the move again (the autopilot also re-plans itself)
    if (p.moving)
    {
        double d = (p.task == GoNav || !p.target) ? Dist2D(s.x, s.y, p.nx, p.ny) : Dist2D(s.x, s.y, p.tx, p.ty);
        if (d < p.lastDist - 20.0)
            p.lastDist = d, p.lastProgress = now;
        else if (now - p.lastProgress > 20000)
        {
            Log("no progress for 20 s (%.0f m to go) - sending the move again", d);
            p.moving = false;
            p.lastProgress = now;
            if (p.task == GoNav || !p.target)
                Post(VanillaExportBlock::TCO_SelfMove, p.nx, p.ny, p.nz), p.moving = true;
        }
    }
    if (now - p.lastStatus >= 2000)
    {
        p.lastStatus = now;
        Status(p, s);
    }
}

// ---- commands -------------------------------------------------------------------------------------------
static bool Command(Pilot &p, const std::string &line)
{
    std::istringstream in(line);
    std::string        cmd;
    in >> cmd;
    if (cmd.empty() || cmd[0] == '#')
        return true;
    Log("command: %s", line.c_str());
    State s = ReadLidar();
    if (cmd == "quit")
    {
        StopAll(p, "quit");
        return false;
    }
    if (cmd == "update")
    {
        // hand over to a newer build beside this one (e.g. "update TestPilot-7.exe"): this copy lets go of
        // everything and exits, the new one waits for the single-instance mutex and carries on
        std::string f;
        in >> f;
        std::wstring wf(f.begin(), f.end());
        std::wstring exe = g_dir + wf;
        if (f.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Log("update: no such file '%s'", f.c_str());
            return true;
        }
        StopAll(p, "update");
        STARTUPINFOW        si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        std::wstring cl = L"\"" + exe + L"\" --after-update";
        if (CreateProcessW(exe.c_str(), &cl[0], nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Log("update: started %s - this copy exits", f.c_str());
            return false;
        }
        Log("update: could not start %s (error %lu)", f.c_str(), GetLastError());
        return true;
    }
    if (cmd == "stop")
        StopAll(p, "stop command");
    else if (cmd == "status")
        Status(p, s);
    else if (cmd == "approach")
    {
        double m = p.stopM;
        std::string what;
        in >> m >> what; // testpilot-14: "approach 250 mech" = mechs only
        if (p.task != Idle)
            StopAll(p, "new task");
        p.task      = Approach;
        p.stopM     = m > 0 ? m : 300;
        p.mechsOnly = what == "mech";
        p.baOnly    = what == "ba";
        p.startedAt = GetTickCount64();
        Log("task: approach the nearest %s, stop %.0f m short, torso on it, lock it%s",
            p.mechsOnly ? "MECH" : p.baOnly ? "BATTLE ARMOUR SUIT" : "enemy", p.stopM,
            p.holdS > 0 ? "" : ", hold until told");
    }
    else if (cmd == "nav")
    {
        std::string l;
        in >> l;
        auto       navs = ReadNavs();
        const Nav *pick = nullptr;
        double     best = 1e18;
        for (auto &v : navs)
        {
            double d    = Dist2D(s.x, s.y, v.x, v.y);
            bool   want = l.empty() ? d > 150.0 : (toupper((unsigned char)l[0]) - 'A') == v.letter;
            if (want && d < best) best = d, pick = &v;
        }
        if (!pick)
            Log("no such nav point (%d known)", (int)navs.size());
        else
        {
            if (p.task != Idle)
                StopAll(p, "new task");
            p.task      = GoNav;
            p.navLetter = pick->letter;
            p.nx = pick->x; p.ny = pick->y; p.nz = pick->z;
            Log("task: walk to nav point %c (%.0f m)", 'A' + pick->letter, best);
        }
    }
    else if (cmd == "lock")
    {
        std::string what;
        in >> what; // testpilot-13: "lock mech" = the nearest MECH on my sensors
        const bool mechOnly = what == "mech";
        const bool baOnly   = what == "ba"; // testpilot-16
        const std::vector<BaSuit> baList = baOnly ? ReadBa() : std::vector<BaSuit>();
        auto        units = ReadUnits();
        const Unit *best  = nullptr;
        double      bd    = 1e18;
        for (auto &u : units)
            if (u.onMyRadar && (!mechOnly || IsMechActor(u.actor)) && (!baOnly || IsBaUnit(u, baList)))
            {
                double d = Dist2D(s.x, s.y, u.x, u.y);
                if (d < bd) bd = d, best = &u;
            }
        if (best) PostLockActor(best->actor), Log("(nearest %s on my sensors, %.0f m)", mechOnly ? "mech" : baOnly ? "battle armour suit" : "hostile", bd);
        else Log("no hostile on my sensors to lock");
    }
    else if (cmd == "calledshot")
    {
        // testpilot-12: calledshot <slot> <part> - lancemate <slot> shoots <part> of YOUR lock (LanceControl's
        // TCO_CalledShot, Pad[1] = 1). Parts: 0 cancel, 1 legs, 2 LL, 3 RL, 4 arms, 5 LA, 6 RA, 7 side torsos,
        // 8 LT, 9 RT, 10 CT, 11 head. Check it with "report" before and after.
        int slot = 0, part = -1;
        in >> slot >> part;
        if (slot < 1 || part < 0 || part > 11)
            Log("calledshot <slot 1-8> <part 0-11>");
        else if (!ReadGameLock())
            Log("calledshot: you have no lock - 'lock' or 'approach' first");
        else
            PostEx(VanillaExportBlock::TCO_CalledShot, slot, (uint8_t)part, 1);
    }
    else if (cmd == "lockundetected")
    {
        // testpilot-12: try to lock the nearest hostile the game does NOT detect - lancecontrol-37 must refuse it
        // ("patch-through refused" in log.txt, and the game's lock must not land on it).
        auto        units = ReadUnits();
        const Unit *best  = nullptr;
        double      bd    = 1e18;
        for (auto &u : units)
            if (!u.detected)
            {
                double d = Dist2D(s.x, s.y, u.x, u.y);
                if (d < bd) bd = d, best = &u;
            }
        if (!best)
            Log("lockundetected: every hostile is detected - nothing to test");
        else
        {
            PostLockActor(best->actor);
            g_lockCheckActor = best->actor;
            g_lockCheckAt = GetTickCount64() + 3000;
            Log("lockundetected: sent for an undetected hostile %.0f m away - checking the game's lock in 3 s", bd);
        }
    }
    else if (cmd == "ba")
        BaReport(s, true);
    else if (cmd == "report")
        Report();
    else if (cmd == "weapons")
    {
        std::string a;
        in >> a;
        p.weaponsFree = a == "free";
        if (!p.weaponsFree && p.lastFire)
            RequestFire(0.0f);
        Log("weapons %s", p.weaponsFree ? "FREE (fires once locked and in sight)" : "HOLD");
    }
    else if (cmd == "fire")
    {
        double secs = 2;
        in >> secs;
        RequestFire((float)secs);
        p.lastFire = GetTickCount64();
    }
    else if (cmd == "vision")
    {
        std::string a;
        in >> a;
        SetVision(a == "on" || a == "1");
    }
    else if (cmd == "torso")
    {
        std::string a;
        in >> a;
        if (a == "centre" || a == "center")
            Post(VanillaExportBlock::TCO_TorsoCentre);
        else if (a == "release")
            Post(VanillaExportBlock::TCO_TorsoRelease), p.torsoOn = false;
        else if (a == "target")
        {
            const LidarContact *n = Nearest(s);
            if (n) Post(VanillaExportBlock::TCO_TorsoTarget, n->X, n->Y, n->Z), p.torsoOn = true;
            else Log("no enemy on the scan");
        }
        else
            Log("torso target | centre | release");
    }
    else
        Log("unknown command '%s' (approach [m] [mech] | nav [letter] | lock | lockundetected | calledshot slot part | report | torso target|centre|release | vision on|off | weapons free|hold | fire [s] | stop | status | update <exe> | quit)", cmd.c_str());
    return true;
}

static bool ReadCommands(Pilot &p)
{
    std::wstring path = g_dir + L"TestPilot.cmd";
    FILE        *f    = _wfopen(path.c_str(), L"rb");
    if (!f)
        return true;
    std::string all;
    char        buf[512];
    size_t      n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        all.append(buf, n);
    fclose(f);
    DeleteFileW(path.c_str());
    std::istringstream lines(all);
    std::string        line;
    while (std::getline(lines, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!Command(p, line))
            return false;
    }
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    g_log = _wfopen((g_dir + L"TestPilot.log").c_str(), L"a");
    Log("==== TestPilot %s started (instrument pilot: orders through LanceControl, no keyboard / mouse) ====", kBuild);
    const bool after = wcsstr(GetCommandLineW(), L"--after-update") != nullptr;
    HANDLE     one   = nullptr;
    for (int i = 0; i < (after ? 50 : 1); ++i) // after an update: wait (up to 5 s) for the old copy to exit
    {
        one = CreateMutexW(nullptr, TRUE, L"MW5_TestPilot_Single");
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
        CloseHandle(one);
        one = nullptr;
        Sleep(100);
    }
    if (!one)
    {
        Log("already running - this copy exits");
        return 0;
    }
    Pilot        p;
    wchar_t      b[64];
    std::wstring ini = g_dir + L"TestPilot.ini";
    GetPrivateProfileStringW(L"pilot", L"stop_m", L"300", b, 64, ini.c_str());
    p.stopM = _wtof(b) > 0 ? _wtof(b) : 300;
    GetPrivateProfileStringW(L"pilot", L"hold_s", L"0", b, 64, ini.c_str());
    p.holdS = _wtof(b);
    GetPrivateProfileStringW(L"pilot", L"target_timeout_s", L"30", b, 64, ini.c_str());
    p.timeoutS = _wtof(b);
    GetPrivateProfileStringW(L"pilot", L"target_ignore_s", L"60", b, 64, ini.c_str());
    p.ignoreS = _wtof(b);
    bool      attached = false;
    ULONGLONG lastCmd = 0, lastWarn = 0;
    Log("waiting for commands in TestPilot.cmd (approach [m] | nav [letter] | lock | torso ... | stop | status | quit)");
    for (;;)
    {
        const bool ok = Attach();
        if (ok != attached)
        {
            attached = ok;
            if (ok)
                Log("game data found (LidarExport + VanillaHud v%u)", VHUD_VERSION);
            else
            {
                Log("game data gone - waiting");
                if (p.task != Idle)
                    p = Fresh(p);
            }
        }
        else if (!ok && GetTickCount64() - lastWarn > 30000)
        {
            lastWarn = GetTickCount64();
            Log("waiting for the game (shared memory not found%s)", g_vhud && g_vhud->Version != VHUD_VERSION ? " - VHUD version differs" : "");
        }
        if (GetTickCount64() - lastCmd >= 500)
        {
            lastCmd = GetTickCount64();
            if (!ReadCommands(p))
                break;
        }
        if (attached)
            Tick(p);
        // testpilot-16: the battle armour watch - logged when the picture changes
        static ULONGLONG lastBa = 0;
        static std::string baSig;
        if (attached && GetTickCount64() - lastBa >= 5000)
        {
            lastBa = GetTickCount64();
            State bs = ReadLidar();
            const std::string sig = BaReport(bs, false);
            if (sig != baSig && (baSig.size() || sig != "0/0/0/0/0/0"))
                BaReport(bs, true);
            baSig = sig;
        }
        if (g_lockCheckAt && GetTickCount64() >= g_lockCheckAt)
        {
            const uint64_t gl = ReadGameLock();
            Log("lockundetected: the game's lock is %s - %s", gl == g_lockCheckActor ? "ON that unit" : gl ? "on another unit" : "none",
                gl == g_lockCheckActor ? "GATE FAILED" : "gate held");
            g_lockCheckAt = 0;
            g_lockCheckActor = 0;
        }
        Sleep(50);
    }
    Log("exit");
    if (one)
        CloseHandle(one);
    if (g_log)
        fclose(g_log);
    return 0;
}
