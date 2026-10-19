// LanceStatusShared.h - the LANCE STATUS FEED and EVENT LIST (VanillaHud plugin -> any reader).
//
// A separate shared block from MW5_VanillaHUDExport, so that one's layout never has to change for
// it. Written by the VanillaHud plugin 4 times a second; read by the voice module (and anything
// else). Everything is read-only for readers.
//
// STATUS: one entry per lance slot (the player's own included, IsPlayer), seqlock like the TacOps
// table - StatusSeq is odd while writing; a reader copies Slots and keeps the copy only if
// StatusSeq was even and unchanged across it.
//
// EVENTS: a ring of kLsEventRing entries, ONE writer (the plugin). Event n (from 0) goes into
// Events[n % kLsEventRing] with Seq = n + 1 written LAST, then EventHead = n + 1. A reader keeps its
// own count, reads entries whose Seq equals the number it expects, and skips ahead if it fell more
// than a ring behind.
//
// Units: world cm for positions, metres for distances, GetTickCount64() milliseconds (low 32 bits)
// for times. Actor addresses are split Lo/Hi exactly as VanillaExportBlock::TacMember::ActorLo/Hi
// and TerrainMechInfo::ActorLo/Hi - join on them for names and paperdolls (LidarExport's data).
#pragma once

#include <cstddef>
#include <cstdint>

static constexpr const wchar_t *LANCE_MMF_NAME = L"MW5_LanceStatus";
static constexpr uint32_t LANCE_VERSION = 7; // v7: kLsMaxUnits 64 -> 256 (2026-10-15, the 5 km radar mode); v6: Pilots[] - the game's pilot names (V-comms panel), by lance number; v2: Cond[] - armour / structure per part; v3: Ammo[] - ammo bins by type; v4: Units[] - hostile positions; v5: Shooters[] - who fired rounds at the player

static constexpr int kLsMaxSlots = 8;
static constexpr int kLsMaxWeapons = 32;
static constexpr int kLsEventRing = 256;

// LsWeapon::State - the weapon's own fire check (EMWWeaponFireResults) against the unit's current
// target, as the game names them.
enum LsWeaponState : uint8_t
{
    LSW_Ready = 0,
    LSW_Destroyed = 1,
    LSW_Firing = 2,
    LSW_Cooldown = 3,
    LSW_Jammed = 4,
    LSW_NeedsLock = 5,
    LSW_TooClose = 6,
    LSW_OutOfRange = 7,
    LSW_NoAmmo = 8,
    // 9.. other game results, raw
    LSW_Unknown = 255,
};

// LsWeapon::Kind (from its firing data).
enum LsWeaponKind : uint8_t
{
    LSK_Unknown = 0,
    LSK_Beam,
    LSK_Continuous, // MG / flamer / TAG
    LSK_AMS,
    LSK_Projectile,
    LSK_LockOn,     // fired as a lock-on weapon
};

struct LsWeapon
{
    uint8_t  Category;    // kFc* bit (VanillaHudShared.h)
    uint8_t  State;       // LsWeaponState
    uint8_t  Kind;        // LsWeaponKind
    uint8_t  Flags;       // bit 0: the plugin started it last
    int16_t  Ammo;        // rounds left in the bins feeding it (its SelectedAmmoType tag); -1 no ammo (energy) / unknown
    int16_t  AmmoMax;     // those bins' capacity; -1 as above
    uint32_t LastFiredMs; // 0 = not seen firing
    char     Name[36];    // its class name, trimmed ("CTF_Torso_Left_EH1_Laser")
};

// LsSlot::Mode bits.
static constexpr uint16_t LSM_PluginFire = 0x001; // the plugin does its firing
static constexpr uint16_t LSM_InsaneAllies = 0x002;
static constexpr uint16_t LSM_IndirectHold = 0x004; // hold position (indirect fire only)
static constexpr uint16_t LSM_Hold = 0x008;         // hold here
static constexpr uint16_t LSM_FallBack = 0x010;     // fall back (facing the enemy)
static constexpr uint16_t LSM_AttackOrder = 0x020;  // an attack order's unit still lives
static constexpr uint16_t LSM_AdvanceFb = 0x040;    // advance / fall back to the player
static constexpr uint16_t LSM_MoveFire = 0x080;     // move, fire on the move
static constexpr uint16_t LSM_GameHoldFire = 0x100; // the game's own Hold Fire (obeyed)

struct LsSlot
{
    uint8_t  Valid;
    uint8_t  IsPlayer;
    uint8_t  Alive;
    uint8_t  Slot;          // the order slot
    uint16_t Mode;          // LSM_*
    uint8_t  FireDisc, FireMask, FireHeat; // as TacMember
    uint8_t  WeaponCount;
    uint8_t  TargetVisible; // it sees its target (the game's own visible list)
    uint8_t  SeenBy;        // enemies (not VTOLs) that see it now
    float    Heat;          // 0..1 of capacity; -1 unknown
    float    Loc[3];        // world cm
    float    TargetDistM;   // -1 no target
    float    NearestSpotterM; // -1 nobody sees it
    uint32_t ActorLo, ActorHi;
    uint32_t TargetLo, TargetHi; // its current combat target, 0 = none
    LsWeapon Weapons[kLsMaxWeapons];
};

// LsEvent::Type.
enum LsEventType : uint16_t
{
    LSE_None = 0,
    LSE_UnitDestroyed,   // Slot's unit was destroyed
    LSE_TookDamage,      // Slot was hit (at most every 3 s per slot)
    LSE_Kill,            // Slot's target (A) died within 5 s of Slot firing at it
    LSE_Spotted,         // enemies now see Slot (Value = how many)
    LSE_Hidden,          // nobody sees Slot any more
    LSE_TargetChanged,   // Slot's combat target is now A (0 = none)
    LSE_PlayerLock,      // the player's lock is now A (0 = none); Slot = the player's slot
    LSE_FriendlyInLine,  // Slot held a shot: friendly A in the line of fire
    LSE_WeaponDestroyed, // Slot's weapon (Value = index into Weapons) was destroyed
    LSE_AmmoEmpty,       // Slot's weapon (Value = index) is out of ammo
    LSE_Overheat,        // Slot passed 95 % heat (Value = heat fraction)
    LSE_OrderReceived,   // Slot was given an order (Value = TacOrder); A = its target unit, if any
    LSE_Arrived,         // Slot reached its hold point (Value = 1 hold here, 2 indirect hold)
    LSE_AmmoLow,         // Slot's weapon (Value = index) is down to a quarter of its ammo
    LSE_UavLaunched,     // the player (Slot) launched a UAV (Value = UAVs left); appended in v3
    // Appended in lancecontrol-8 (same layout, still v3 - an older reader ignores them):
    LSE_PlayerHit,       // the player (Slot) damaged enemy A: A's RecentDamageTarget turned to the player
    LSE_EnemyOnPlayer,   // enemy A's combat target turned to the player (Slot)
    // Appended in lancecontrol-21 (same layout, still v6 - an older reader ignores it):
    LSE_Flanked,         // enemy A is close BEHIND lancemate Slot (rear arc, Value = metres)
    // Appended in lancecontrol-34 (same layout - an older reader ignores it):
    LSE_AttackDropped,   // Slot's attack order on A ended - Value 2 the lance lost sight (1 s), 3 never seen (1 s)
    // LSE_TookDamage's A = the attacker (Slot's RecentDamageTarget), 0 = unknown - lancecontrol-8
};

struct LsEvent
{
    uint32_t Seq;     // n + 1, written last
    uint32_t TimeMs;
    uint16_t Type;    // LsEventType
    uint8_t  Slot;
    uint8_t  Pad;
    uint32_t ALo, AHi; // the other unit involved, 0 = none
    float    Value;
};

// v2: a slot's CONDITION, read by LanceControl itself off the unit's own health containers (the
// same ArmorPoints / HitPoints fields LidarExport reads for the paperdoll) - so it is there at any
// distance, not only inside LidarExport's 1000 m scan. Cond[i] belongs to Slots[i] and is written
// under the same StatusSeq. Fractions remaining 0..1, -1 = not read. Part order as TerrainMechPart:
// head, centre torso, left torso, right torso, left arm, right arm, left leg, right leg.
static constexpr int kLsParts = 8;
struct LsCondition
{
    uint8_t Have; // 1 = Armour / Structure are real reads
    uint8_t Pad[3];
    float   Armour[kLsParts];
    float   Structure[kLsParts];
};

// v3: a slot's AMMO BY TYPE - its MWWeaponAmmoComponent bins, one entry per ammo type (bins of
// the same type summed), consumables (Ammo.Consumable.*, the UAV) left out. Tag is the bin's
// AmmoType gameplay tag ("Ammo.Missile.LRM.Clan", "Ammo.Ballistic.AutoCannon.Standard.AC10"), so
// a reader can name the ammo after its weapon. Ammo[i] belongs to Slots[i], under StatusSeq.
static constexpr int kLsAmmoTypes = 12;
struct LsAmmoBin
{
    char    Tag[56];
    int16_t Count; // rounds left
    int16_t Max;   // capacity
};
struct LsAmmoList
{
    uint8_t   Count; // entries used
    uint8_t   Pad[3];
    LsAmmoBin Bins[kLsAmmoTypes];
};

// v4: EVERY hostile unit's position (lancecontrol-9) - the voice module's "where is it?" at any
// range (LidarExport's scan stops at 1000 m). Twice a second, its own seqlock (UnitSeq odd while
// writing). A unit missing from the list is gone (or out of the world); Dead = a wreck still there.
static constexpr int kLsMaxUnits = 256; // v7 (2026-10-15, user): was 64
struct LsUnit
{
    uint32_t ActorLo, ActorHi;
    float    Loc[3]; // world cm
    uint8_t  Dead;
    uint8_t  Seen;   // in a lancemate's sight (its TargetVisibilityComponent) or within 500 m of the player
    // lancecontrol-11 (was Pad[2] - same size and layout, so LANCE_VERSION stays 4):
    uint8_t  Flags;  // LSU_* bits
    uint8_t  SeenBy; // which lancemates see it: bit (slot - 1)
};
static constexpr uint8_t LSU_TargetsPlayer  = 0x01; // its AI's combat target is the player (changes often)
static constexpr uint8_t LSU_OnLanceRadar   = 0x02; // on a lancemate's sensors (its tracker: bInSensorRange) - lancecontrol-12
static constexpr uint8_t LSU_OnPlayerRadar  = 0x04; // on the player's own sensors (incl. relayed from the team)
// SeenBy (lancecontrol-12): from each lancemate's own tracker - bInLineOfSight (the game's flag)

// v5 (lancecontrol-14): WHO HAS BEEN FIRING AT THE PLAYER - rounds (MWProjectile) seen heading at the player,
// each traced once to the unit that fired it (the round's Instigator). Lasers are beams, not rounds: never in
// here. Rounds[] count the current spell of fire (reset after 10 s without one); LastMs = the low 32 bits of
// GetTickCount64 at its last round. Most recent first; own seqlock ShotSeq (odd while writing).
static constexpr int kLsMaxShooters = 8;
enum LsShotType : uint8_t
{
    LST_Missile = 0, // LRM / SRM / Streak / ATM / MRM / rockets
    LST_Arrow,       // Arrow IV
    LST_Ballistic,   // autocannon / Gauss / machine gun
    LST_PPC,
    LST_Count
};
struct LsShooter
{
    uint32_t ActorLo, ActorHi;
    uint32_t LastMs;
    uint8_t  Rounds[LST_Count]; // capped at 255
};

// v6 (lancecontrol-17): the pilot in each lance number, from the game's own lance panel
// (MWSquadMemberDisplayInfo: SquadMemberId, PilotName, PlayerPawn). Pilots[lance number - 1]. The
// name stays with the PILOT when pilots change mechs (a takeover); ActorLo/Hi is the mech it is in now.
struct LsPilot
{
    uint8_t  Valid;
    uint8_t  Slot;      // lance number (1 = the player)
    uint8_t  MemberId;  // the game's SquadMemberId (1..4), 0 unknown
    uint8_t  Pad;
    uint32_t ActorLo, ActorHi;
    char     Name[36];  // UTF-8, the game's PilotName; "" unknown
};

struct LanceStatusBlock
{
    uint32_t Version;      // LANCE_VERSION
    uint32_t Enabled;      // 1 while the plugin is live
    uint64_t TickMs;       // last GetTickCount64 the plugin published at
    uint32_t StatusSeq;    // seqlock over SlotCount / Slots
    uint32_t SlotCount;    // entries in use, in slot order
    LsSlot   Slots[kLsMaxSlots];
    uint32_t EventHead;    // events written so far
    uint32_t EventPad;
    LsEvent  Events[kLsEventRing];
    LsCondition Cond[kLsMaxSlots]; // v2, parallel to Slots (under StatusSeq)
    LsAmmoList  Ammo[kLsMaxSlots]; // v3, parallel to Slots (under StatusSeq)
    uint32_t    UnitSeq;           // v4: seqlock over UnitCount / Units
    uint32_t    UnitCount;
    LsUnit      Units[kLsMaxUnits];
    uint32_t    ShotSeq;           // v5: seqlock over ShooterCount / Shooters
    uint32_t    ShooterCount;
    LsShooter   Shooters[kLsMaxShooters];
    LsPilot     Pilots[kLsMaxSlots];   // v6: under StatusSeq, with Slots
};
static_assert(sizeof(LsPilot) == 48, "LsPilot layout changed");
static_assert(sizeof(LsShooter) == 16, "LsShooter layout changed");

static_assert(sizeof(LsWeapon) == 48, "LsWeapon layout changed");
static_assert(sizeof(LsEvent) == 24, "LsEvent layout changed");
static_assert(sizeof(LsUnit) == 24, "LsUnit layout changed");
static_assert(sizeof(LsCondition) == 68, "LsCondition layout changed");
static_assert(sizeof(LsAmmoBin) == 60, "LsAmmoBin layout changed");
static_assert(sizeof(LsAmmoList) == 4 + 60 * kLsAmmoTypes, "LsAmmoList layout changed");
static_assert(offsetof(LsSlot, Weapons) == 52, "LsSlot layout changed");
static_assert(sizeof(LsSlot) == 52 + 48 * kLsMaxWeapons, "LsSlot layout changed");
