// LidarShared.h
// Shared memory-mapped-file contract between the in-game plugin (writer) and the
// standalone display app (reader). Include this from BOTH projects so the layout
// can never drift.
#pragma once
#include <cstdint>
#include <cstddef>

// Memory-mapped file name (CreateFileMapping / OpenFileMapping).
#define LIDAR_MMF_NAME L"MW5_LidarExport"

#define LIDAR_MAGIC           0x3152444Cu // 'LDR1'
#define LIDAR_VERSION         1u
#define LIDAR_MAX_CONTACTS    256u
#define LIDAR_MAX_PROJECTILES 512u
#define LIDAR_MAX_ALLIES      32u

// Renderer -> plugin control bits (written into ControlLayers by the reader). The
// projectile scan is expensive, so it only runs when the renderer requests it.
#define LIDAR_CTRL_PROJECTILES 0x1u
// Request the MWMissilePath lock probe WITHOUT the full projectile array. The probe is
// cheap (one GetActorsOfClass + a call per live salvo) where the projectile scan is not,
// so the threat warning can run on every MFD page instead of only while the scope is up.
#define LIDAR_CTRL_THREAT      0x2u
// THE 5 KM RADAR MODE (2026-10-15, user): contacts, allies, rounds and buildings are kept out to 5 km instead of 1 km.
// Only while HUDDisplay's "Vanilla, 5 km" radar mode is selected. The renderer also bumps ControlPad every poll as a
// heartbeat: the plugin drops back to 1 km by itself when the bit is set but ControlPad has not moved for 5 s (a
// renderer that died with the bit set must not leave the costly ranges on).
#define LIDAR_CTRL_FAR         0x4u
#define LIDAR_FAR_RANGE_CM     500000.0f

// LidarExportBlock.ThreatFlags bits.
#define LTHREAT_LOCKED 0x1u // >=1 homing salvo is guiding on the player RIGHT NOW

// Contact attitude relative to the player. v1 fills FriendlyMech / HostileMech /
// NeutralMech once the team read is wired; until then it writes Unknown.
enum LidarContactType : uint8_t {
    LCT_FriendlyMech = 0,
    LCT_HostileMech  = 1,
    LCT_NeutralMech  = 2,
    LCT_Unknown      = 3,
};

// Unit category - drives the marker's shape/size in the renderer.
enum LidarKind : uint8_t {
    LK_Mech   = 0,
    LK_Tank   = 1,
    LK_Turret = 2,
    LK_Vtol   = 3,
    LK_BattleArmor = 4, // 2026-10-17 (user): one battle armour suit - Clan Elemental / IS battle armour (KelClanElemental);
                        // appended - no layout change; a reader that does not know it draws a small vehicle marker
};

// Mech weight class (dot count in the renderer). 0xFF = not a mech / unknown.
enum LidarWeight : uint8_t {
    LW_Light   = 0, // <= 35 t
    LW_Medium  = 1, // 40-55 t
    LW_Heavy   = 2, // 60-75 t
    LW_Assault = 3, // 80 t +
    LW_None    = 0xFF,
};

// Projectile family - drives the renderer color and which layer shows it.
enum LidarProjType : uint8_t {
    LPT_Ballistic = 0, // autocannon / gauss / MG shells  (brown)
    LPT_PPC       = 1, // energy bolt                      (blue)
    LPT_Missile   = 2, // SRM / LRM / etc.                 (red-ish)
    LPT_ArrowIV   = 3, // Arrow IV artillery missile       (red-ish, 3x size)
};

// LidarProjectile.Flags bits.
#define LPROJ_INCOMING 0x1u // this missile is HOMING ON THE PLAYER (definitive, plugin-side)

#pragma pack(push, 4)
struct LidarContact {
    float    X, Y, Z;  // world position, Unreal units (cm)
    float    Yaw;      // world facing yaw (degrees) - for the orientation triangle
    uint8_t  Type;     // LidarContactType (friend/foe)
    uint8_t  Team;     // raw team id (debug / future use)
    uint8_t  Selected; // 1 = player's current locked/selected target
    uint8_t  Kind;     // LidarKind (mech/tank/turret/vtol)
    uint8_t  Weight;   // LidarWeight (mech weight class; LW_None for vehicles)
    uint16_t Id;       // stable-ish per-frame id
};

struct LidarProjectile {
    float    X, Y, Z; // world position (cm)
    uint16_t Id;      // stable id (from actor pointer) - lets the renderer draw streaks
    uint8_t  Type;    // LidarProjType
    uint8_t  Flags;   // LPROJ_* bits (was _pad; same size, so MMF layout is unchanged)
};

// Seqlock-guarded block. Writer bumps FrameCounter (start), writes the body, then
// sets FrameCounterEnd == FrameCounter. Reader copies the body and only trusts it
// when FrameCounter == FrameCounterEnd (else retry — it caught a write in flight).
struct LidarExportBlock {
    uint32_t Magic;          // == LIDAR_MAGIC once initialised
    uint32_t Version;        // == LIDAR_VERSION

    // Reverse channel: the RENDERER writes these, the plugin reads them. Not part of
    // the seqlock body (the plugin never writes them, so its publishes leave them
    // intact). ControlProjIntervalMs = requested projectile sample interval (0 = use
    // the plugin default).
    uint32_t ControlLayers;
    uint16_t ControlProjIntervalMs;
    uint16_t ControlPad;

    uint32_t FrameCounter;   // set BEFORE writing the body

    float    PlayerX, PlayerY, PlayerZ; // player mech world position (cm), fast tick
    float    PlayerYaw;                 // player heading (mech body), degrees, fast tick
    float    PlayerTorsoYaw;            // torso twist relative to the body, degrees, fast tick

    // Locked-target contact ids (stable ids), refreshed every fast tick so the
    // highlight follows selection changes without waiting for the 3 s scan. 0 = unused.
    uint16_t SelectedIds[4];
    uint8_t  SelectedCount;

    // Threat state, from the MWMissilePath lock probe. These three bytes were dead
    // padding before ScanCounter's 4-byte alignment, so claiming them leaves the MMF
    // exactly 15180 bytes and the frozen LidarDisplay.exe reads on unaffected.
    //
    // INSTANTANEOUS TRUTH, no hold: the plugin publishes what is guiding on the player
    // this tick and nothing more. Any latch, debounce or minimum-duration is the
    // renderer's business - a stalled plugin must not be able to pin the alarm on, which
    // is exactly how the previous implementation got stuck.
    uint8_t  ThreatFlags;    // LTHREAT_* bits
    uint8_t  ThreatSalvos;   // homing salvos currently locked onto the player (saturates at 255)
    uint8_t  ThreatPad;      // reserved - keep zeroed

    // Bumped only when the contact array is refreshed (the ~3 s ping scan), NOT on the
    // frequent player-transform updates. The renderer restarts its ping sweep on this.
    uint32_t     ScanCounter;

    // Enemies only: refreshed on the ~3 s ping scan (the renderer reveals + fades them).
    uint32_t     ContactCount;               // <= LIDAR_MAX_CONTACTS
    LidarContact Contacts[LIDAR_MAX_CONTACTS];

    // Allies: refreshed every fast tick (positions re-read from cached actors), so
    // friendlies track smoothly and are always shown - independent of the ping.
    uint32_t     AllyCount;                   // <= LIDAR_MAX_ALLIES
    LidarContact Allies[LIDAR_MAX_ALLIES];

    // Projectiles run on their own ~15 Hz cadence (only when requested via
    // ControlLayers). ProjCounter bumps each projectile refresh so the renderer knows
    // when to advance streaks.
    uint32_t        ProjCounter;
    uint32_t        ProjectileCount;         // <= LIDAR_MAX_PROJECTILES
    LidarProjectile Projectiles[LIDAR_MAX_PROJECTILES];

    uint32_t FrameCounterEnd; // set == FrameCounter AFTER the body is written
};
#pragma pack(pop)

// The standalone LidarDisplay.exe is frozen and maps this same block. If any of these
// fire, that binary is reading garbage and must be rebuilt - which the project rules say
// we do not do. The threat bytes came out of existing alignment padding, so the size and
// every pre-existing offset below must be untouched by their addition.
static_assert(sizeof(LidarExportBlock) == 15180, "MMF size changed - frozen LidarDisplay.exe would misread the block");
static_assert(offsetof(LidarExportBlock, SelectedCount) == 48, "SelectedCount moved");
static_assert(offsetof(LidarExportBlock, ThreatFlags)   == 49, "ThreatFlags must sit in the old padding");
static_assert(offsetof(LidarExportBlock, ThreatSalvos)  == 50, "ThreatSalvos must sit in the old padding");
static_assert(offsetof(LidarExportBlock, ThreatPad)     == 51, "ThreatPad must sit in the old padding");
static_assert(offsetof(LidarExportBlock, ScanCounter)   == 52, "ScanCounter moved");
