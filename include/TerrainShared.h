// TerrainShared.h  -  contract version 10
//
// The terrain heightfield contract between LidarExport.dll (producer, game thread) and
// HUDDisplay.exe (consumer). ONE definition, included by both - deliberately unlike
// ExportControlBlock, which is duplicated in two files and held together by static_asserts.
//
// WHY A SEPARATE MEMORY-MAPPED FILE. The LIDAR block (MW5_LidarExport, 15180 bytes) is frozen:
// LidarDisplay.exe shares it and must keep working, so its size and layout cannot change. This
// block is now ~2 MB, so it very much gets its own mapping and the frozen contract stays frozen.
//
// =============================================================================================
// WHAT CHANGED IN VERSION 2, AND WHY
//
// v1 cached 25 tiles around the player and evicted the farthest when you moved. That was a
// streaming design, and streaming is what you do when the world is too big to finish. A MW5
// mission map is not: at the measured 6,750 traces/second, a 5 km map is 15 seconds of
// background tracing and 411 KB of heights. Re-tracing ground because the pilot walked back
// over it was pure waste, and the map having an edge you could fly off was a direct consequence.
//
// So v2 traces the WHOLE map once and keeps it. Terrain is static for the life of a mission, so
// a tile traced at minute one is still correct at minute twenty. Eviction survives only as a
// fallback for a map larger than the cache, not as the normal case.
//
// THE SPLIT THAT MAKES THIS AFFORDABLE. Heights are expensive to acquire (a trace against the
// physics world) and cheap to store. Rendering geometry is the opposite: cheap to rebuild from
// heights, expensive to hold and expensive to draw. So they are cached on different horizons -
// the producer keeps heights for the whole map, and the consumer keeps drawing geometry only
// for the tiles it can currently see, rebuilding from the heightfield as you fly. That division
// is the whole reason a whole-map cache does not cost a whole-map render.
//
// LAYOUT CHANGE. v1 interleaved each tile's identity (TileX/TileY/State) with its 4 KB of
// heights, which meant the consumer had to copy the ENTIRE block under the seqlock just to find
// out which tiles existed - 100 KB a frame at 25 tiles, and 2 MB a frame at this size, which is
// absurd. v2 separates them: TerrainSlotInfo holds identity, lives in the header, and is the
// only thing copied per frame (~8 KB). Heights are read in place, at bake time only, guarded by
// re-checking the slot afterwards. See READING IT SAFELY below.
// =============================================================================================
//
// READING IT SAFELY.
//
//   Header + slot table: a seqlock. FrameCounter is bumped before the producer touches either
//   and FrameCounterEnd after, so a consumer that samples End first, copies, then samples
//   Counter, sees equal values only if no write straddled the copy.
//
//   Heights: NOT covered by the seqlock, because copying 2 MB per frame to read a few tiles
//   would be ridiculous. Instead they are immutable while a slot reads TTS_Complete - the
//   producer writes heights only while TTS_Filling, and the store of TTS_Complete is the
//   release. A consumer may therefore read Tiles[i] directly as long as it re-checks
//   Slots[i] (State, TileX, TileY) AFTER reading and discards the result if anything moved.
//   That is the one case where the producer can pull the rug out: eviction, which flips a slot
//   from Complete back to Filling and starts overwriting.
//
//   Generation is bumped whenever the producer throws everything away (pawn change, or leaving
//   a mission). A consumer that sees it move must drop every cached tile, because slot contents
//   no longer correspond to anything it read.

#pragma once

#include <cstdint>

#define TERRAIN_MMF_NAME L"MW5_TerrainExport"

// v2 split tile identity out of the tile data. v3 adds the mech's heading, because the LIDAR
// scope this map will eventually underlay is HEADING-UP, not north-up - it projects every
// contact through fwd/right built from PlayerYaw. A north-up terrain layer drawn under a
// heading-up scope is wrong by the mech's entire heading, and wrong in a way that still looks
// like a map. Neither version is compatible with the one before it; both binaries rebuild
// together.
static constexpr uint32_t TERRAIN_VERSION = 15; // v15: TerrainWeapon::Count (identical weapons grouped, 2026-10-19); v14: v14: Dropships[] (hull outlines on the maps); v13: v13: TerrainMechInfo::ActorLo/Hi (v12: Locked / Dead in the pad)

// 32 samples of 16 m gives a 512 m tile. The spacing is the DATA resolution; the consumer picks
// a drawing stride on top of it, because at a 2560 m span a 16 m cell is 3.75 pixels and draws
// as mush no matter how many segments you spend on it.
static constexpr int32_t TERR_SAMPLES    = 32;
static constexpr float   TERR_SPACING_CM = 1600.0f;
static constexpr float   TERR_TILE_CM    = TERR_SAMPLES * TERR_SPACING_CM; // 51200 cm = 512 m

// 512 tiles is 2.1 MB of heights and covers 134 km2 of actual ground - an 11 x 11 km map if it
// were square, and real missions are a fraction of that. Filling all 512 is 524,288 traces,
// about 78 s at the measured rate, which runs in the background while you play. The cap exists
// so the mapping has a fixed size, not because the memory is scarce.
static constexpr int32_t TERR_MAX_TILES  = 512;

// How far from the anchor tile the producer is willing to look, in tiles. 16 gives a 33 x 33
// candidate area (16.9 km across), comfortably past any real mission boundary, and bounds the
// producer's search bitmaps to a fixed size.
static constexpr int32_t TERR_MAX_RING   = 16;
static constexpr int32_t TERR_GRID_SIDE  = 2 * TERR_MAX_RING + 1; // 33

// A sample whose trace found nothing. Distinct from any real world Z, and chosen so that naive
// min/max over an unfilled grid is obviously wrong rather than subtly plausible.
static constexpr float TERR_NO_HEIGHT = -1.0e9f;

enum TerrainTileState : uint32_t
{
    TTS_Empty    = 0, // slot unused, or discarded by a generation change
    TTS_Filling  = 1, // heights are being written - DO NOT READ
    TTS_Complete = 2  // every sample present; safe to read while the slot still says so
};

// Tile identity, separated from tile data so the consumer can copy the whole table cheaply.
// One of these per slot, all of them inside the header, all covered by the seqlock.
struct TerrainSlotInfo
{
    int32_t  TileX;  // world tile coordinate = floor(worldX / TERR_TILE_CM)
    int32_t  TileY;
    uint32_t State;  // TerrainTileState
    uint32_t Hits;   // samples that found ground, valid once Complete. 0 means the tile is
                     // entirely off the playable map - the consumer skips it, and the producer
                     // uses it to stop exploring in that direction.
};

// Pure data. Absolute world Z in centimetres, row-major: index = row * TERR_SAMPLES + col,
// col along +X, row along +Y. Sample CENTRES, not corners:
//     x = TileX * TERR_TILE_CM + col * TERR_SPACING_CM + TERR_SPACING_CM * 0.5
// Anything drawing this lattice must carry that half-cell offset or it sits 8 m off the tile
// grid and off the player marker.
struct TerrainTile
{
    float Height[TERR_SAMPLES * TERR_SAMPLES];
};

// =============================================================================================
// STRUCTURES - buildings, published as boxes rather than sampled into the heightfield
//
// Buildings used to arrive as terrain: a ray landing on a roof published the roof as ground, so
// a barracks became a 12 m lump in the landscape and a destroyed one stayed a lump forever.
//
// The obvious repair - sample them into the height lattice with a flag - is the wrong shape. At
// 16 m spacing a 30 m building covers two samples, so you would get presence and not shape: a
// coarse blob, doubling the cache to say "something is here". Buildings are ACTORS, and there
// are dozens of them rather than a million samples, so enumerating them costs almost nothing and
// gives real footprints. It also makes them live rather than baked - a building flattened in
// minute twenty stops being published, where a baked height could never change.
//
// The common base is MWDestructibleObject_C. That was not a guess: the hit census walked the
// ancestry of every class the terrain rays touched, and every structure on every map converged
// on it - QuonsetHut, BLD_*, Building_Brick_*, MediumWall_*, Silo, OilPumpJack, WindTurbine.
//
// AXIS-ALIGNED, because that is what GetActorBounds returns. A building rotated 30 degrees draws
// as a slightly oversized box. For a tactical display that is the right trade; recovering true
// orientation means reading the actor's rotation and its component bounds separately, which is a
// lot of reflection for a box that is already within a few metres of correct.
struct TerrainStructure
{
    float X, Y, Z;    // world centre, cm
    float EX, EY, EZ; // half-extent, cm - so the box is X +/- EX and the footprint is 2*EX by 2*EY
};

// Enough for any MW5 map seen so far, at 24 bytes each. The header is copied whole every frame
// under the seqlock, so this is 12 KB of per-frame copy on top of the 8 KB slot table - still
// nothing beside the 2 MB of heights that are deliberately NOT copied.
static constexpr int32_t TERR_MAX_STRUCTURES = 512;

// =============================================================================================
// MECH SILHOUETTES - joint positions, so a contact can be drawn as the mech it actually is
//
// The scope and the 3D page draw contacts as triangles. A triangle says where something is and
// which way it faces, and nothing else: an Atlas and a Locust are the same mark.
//
// MW5 makes better available. Every mech - stock or modded - is skinned to ONE shared skeleton,
// /Game/Objects/Mechs/_common/Mech_SKL, so a single joint-name list covers the entire game. And
// each chassis carries its own per-part geometry on that skeleton (the mod paks name them
// GRN_Thigh_Left, GRN_Calf_Left and so on, chassis-prefixed), so the joint POSITIONS differ per
// chassis. A stick figure built from them is therefore not a generic mech icon - it is that
// mech, in its current pose, at its real proportions.
//
// Published in WORLD centimetres and absolute, not relative to the mech's origin. A consumer
// draws the bones by joining indices; it needs no transform of its own and cannot get the pose
// wrong by disagreeing about the mech's facing.
//
// The joint ORDER is part of the contract. Consumers index it directly.
enum TerrainJoint : int32_t
{
    TJ_Pelvis = 0,
    TJ_ThighL, TJ_KneeL, TJ_CalfL, TJ_FootL,
    TJ_ThighR, TJ_KneeR, TJ_CalfR, TJ_FootR,
    TJ_TorsoCentre, TJ_TorsoHead, TJ_TorsoL, TJ_TorsoR,
    TJ_UpperarmL, TJ_UpperarmR,
    TJ_COUNT
};

// Nearest mechs only. Reading a joint costs one reflected call, so this is the figure that
// decides the per-scan cost: 15 joints x 8 mechs is 120 calls, and beyond a few hundred metres
// a silhouette is fewer pixels than the triangle it replaces.
static constexpr int32_t TERR_MAX_SILHOUETTES = 8;

struct TerrainSilhouette
{
    float    X[TJ_COUNT], Y[TJ_COUNT], Z[TJ_COUNT]; // world cm, one entry per TerrainJoint
    uint8_t  Valid;   // 0 = this slot holds nothing
    uint8_t  Hostile; // 1 = enemy, 0 = ally - so the consumer colours it like its contact
    uint8_t  Pad[2];
};

// =============================================================================================
// MECH IDENTITY AND CONDITION - what a contact IS, not just where it is
//
// The scope draws a contact as a mark with a facing. That answers "where" and nothing else: an
// Atlas and a Locust are the same triangle, and a mech with its right torso gone looks exactly
// like one that just walked out of the bay.
//
// This block carries the rest of it, so the 3D page can draw each contact as a chevron with an
// eight-part damage doll above it and a label naming the chassis, variant, weight and weapons.
//
// JOINED BY Id, NOT BY POSITION. Every LIDAR contact already carries a stable actor id
// (LidarContact::Id, assigned from the actor pointer and kept across scans), and both the enemy
// contact list and the ally list use the same id space. So a record here names its contact by id
// and the consumer does a lookup. That matters for two reasons. Matching by proximity would be
// guesswork the moment two mechs brawl at melee range. And going through the contact list means
// this layer inherits the ping reveal and fade for free - a mech the sweep has not lit up yet has
// no contact, so it gets no doll, which is the whole point of a scope.
//
// EVERY FIELD DEGRADES ON ITS OWN. Chassis, variant and weight class are read and proven. Armour,
// structure, heat and the weapon mounts come off native components (MWMechLivingActorComponent,
// MWMechWeaponSystemComponent, MWMechCriticalSlotComponent), whose property names live only in
// the shipped binary's reflection data - no amount of reading cooked content recovers them. So
// each group carries its own Have* flag and the renderer draws what resolved and omits what did
// not, rather than one failed read blanking the label.
//
// The PART ORDER is part of the contract - the consumer indexes Armour[] and Structure[] directly
// and TerrainWeapon::Part refers to it. It matches the game's own paperdoll layout (the MWVR
// target screen breaks out EMechParts in exactly these eight, plus rear faces for the torsos,
// which are deliberately not carried here: a doll eight pixels tall cannot show a rear face).
enum TerrainMechPart : int32_t
{
    TMP_Head = 0,
    TMP_CentreTorso,
    TMP_LeftTorso,
    TMP_RightTorso,
    TMP_LeftArm,
    TMP_RightArm,
    TMP_LeftLeg,
    TMP_RightLeg,
    TMP_COUNT
};

static constexpr uint8_t TMP_UNKNOWN = 0xFF; // mount could not be determined

// REAR FACES. Only the three torsos have distinct rear ARMOUR, which is what this list carries.
//
// An earlier version of this comment said arms, legs and head are "single-sided". That was too
// strong, and reading MWVR's own doll showed it: VR_MechPaperDoll has LeftArmArmor_Rear,
// RightLegArmor_Rear and the rest, because it draws a complete second silhouette - MechRearPanel -
// with every part seen from behind. What those extra widgets do NOT have is separate damage
// animations: only AnimRearCenterTorso, AnimRearLeftTorso and AnimRearRightTorso exist. So the
// arms and legs appear on the rear view showing their FRONT values, and the three torsos are the
// only parts with a second set of numbers. The data model below is therefore right; the claim
// about the display was not.
//
// why this is a separate three-entry list rather than three unused slots in the eight above.
// The order is the game's own: MWMechLivingActorComponent declares RearCenterTorso, then
// RearLeftTorso, then RearRightTorso, and they are separate health containers with their own
// armour, not a second reading of the front.
//
// Worth carrying because rear armour is where a mech is actually killed. A target showing a
// healthy front can have an open rear centre torso, and on the front-only doll those two look
// identical - which is the most dangerous thing a display can do.
enum TerrainRearPart : int32_t
{
    TRP_CentreTorso = 0,
    TRP_LeftTorso,
    TRP_RightTorso,
    TRP_COUNT
};

// One mounted weapon. The name is the game's SHORTENED name (MechHudWeaponEntry::Weapon_ShortName,
// e.g. "ML", "AC20") because these are drawn at label size, where the full name does not fit and
// would wrap. Part is what makes the colour rule work: the renderer turns the text amber when
// Armour[Part] is gone and red when Structure[Part] is gone, so you can read at a glance that a
// target's large lasers are still live but its autocannon is buried in a wrecked side torso.
struct TerrainWeapon
{
    char    Name[20];
    uint8_t Part;  // TerrainMechPart, or TMP_UNKNOWN
    uint8_t Flags; // bit 0: the weapon itself is destroyed
    uint8_t Count; // v15: how many identical weapons (same Name, Part and Flags) this entry stands for; 0 = 1
    uint8_t Pad;
};

static constexpr uint8_t TWF_Destroyed = 0x01;

// Twelve is past what any stock variant mounts as separate hardpoints, and the label has room for
// about eight lines before it fights the next contact for space.
static constexpr int32_t TERR_MAX_WEAPONS = 12;

// A hundred and twenty-eight. This figure has now been raised three times - 24, then 40 when
// vehicles joined, then 64 - and each time the next mission walked straight into it: a live log
// showed "40 known" on three separate frames, which is an array running out rather than a
// coincidence. The lesson is that this cap is not a memory decision at all. At ~450 bytes each,
// 128 units is 58 KB inside a header already copied whole every frame, against the 2 MB of
// heights deliberately NOT copied - so the cost of being generous is noise, and the cost of
// being tight is a turret that silently never appears.
static constexpr int32_t TERR_MAX_MECHS = 128;

// What this record describes. Mirrors LidarKind so a consumer can compare it directly against
// the contact it joined to.
// NINE KINDS, AND THEY ARE NOT MINE. This list is the set of paperdoll classes the game itself
// distinguishes: MWVR's target readout reads PaperDollClass off MWTargetComponent and class-casts
// it against exactly these nine, never looking at the actor's own class. Taking the same list
// means a unit is classified the way the game classifies it, including for mods, which is why
// Coyote's Mission's copy of the Manticore lands on Tank without anybody writing its name down.
//
// The four-kind version of this enum was my own guess and it collapsed five real types. It cost
// two of them outright: the Igor is a VTOL_DUAL_ENGINE, which is why it ships engine_l_top and
// engine_r_top art that nothing referenced, and the artillery piece and radar tower are separate
// target types whose art was sitting in the extract unlabelled.
enum TerrainUnitKind : uint8_t
{
    TUK_Mech = 0,
    TUK_Tank = 1,
    TUK_Turret = 2,
    TUK_Vtol = 3,
    TUK_VtolDualEngine = 4, // two engine pods, drawn as separate damage zones
    TUK_Artillery = 5,
    TUK_Generator = 6,
    TUK_RadarTower = 7,
    TUK_SatelliteDish = 8,
    TUK_BattleArmor = 9,    // 2026-10-17 (user): one battle armour suit (KelClanElemental) - one body, one health number,
                            // art "clanelemental". Appended: no layout change (TERRAIN_VERSION unchanged, as LK_BattleArmor)
    TUK_COUNT
};

// True for everything that is not a mech: one body, one health number, one mask.
static inline bool TukIsSimple(uint8_t k) { return k != TUK_Mech && k < TUK_COUNT; }

static constexpr uint32_t TERR_DEAD_HOLD_MS = 5000;

struct TerrainMechInfo
{
    uint16_t Id;      // LidarContact::Id of the contact this describes
    uint8_t  Valid;   // 0 = this slot holds nothing
    uint8_t  Hostile; // echoed so a consumer can colour without a second lookup
    uint8_t  Kind;    // TerrainUnitKind - see the note below on what it changes
    // v12 - carved out of what was KindPad[3], so size and offsets are unchanged.
    uint8_t  Locked;  // 1 = this actor is the player's TargetTracking.CurrentTarget - the game's
                      // own lock, the slot MWVR's HeadAim treats as THE target. Clears with it.
    uint8_t  Dead;    // 1 = destroyed (mech: bIsPilotDead || bCenterTorsoDestroyed). Published
                      // for at most TERR_DEAD_HOLD_MS after first seen dead, then dropped, so a
                      // consumer sees the kill land and then the unit is gone.
    uint8_t  KindPad;

    char Chassis[32]; // "viper", "Catapult" - from the mech's own MechTargetComponent class
    char Variant[32]; // "VPR-A" - from GetMechDataAsset()

    // The pilot, for units in the player's squad only - empty for everyone else.
    //
    // A lancemate is not usefully identified by its chassis: you know your own lance's mechs, and
    // what you actually want to find on a cluttered scope is Ash or Sledge. MWVR's target readout
    // shows this (MWPersonaComponent -> GetPersonaData -> Callsign, gated on IsUnitInPlayerSquad)
    // and it is the one label that tells you something the silhouette cannot.
    char Callsign[24];

    // WHICH ART TO DRAW, TAKEN FROM THE UNIT'S OWN TEXTURE REFERENCE - not from its name.
    //
    // Every unit blueprint stores its paperdoll textures as a Texture2D on its target component
    // (SetupTexture), and the component hands them to the widget with SetBrushFromTexture. That
    // is how the game draws a Coyote's Mission tank whose class is "Manticoreconvoyfast" with the
    // real Manticore doll: it follows a pointer, it does not match a name.
    //
    // So this carries the TEXTURE's object name, reduced to the extractor's folder key - the
    // Manticore tank publishes "manticore" whatever its blueprint is called. Matching on the
    // chassis name instead is what made the Mist Lynx fall back to a generic doll: the data
    // asset's ChassisFullName is the display string "Mist Lynx", with a space, while the art
    // folder is named from the asset path and is "mistlynx". Empty when nothing resolved, and
    // the consumer then falls back to Chassis and finally to the kind's generic silhouette.
    char ArtKey[32];

    float   Tons;        // 0 when unknown; the weight CLASS below is the reliable figure
    uint8_t WeightClass; // 0=Light 1=Medium 2=Heavy 3=Assault, 0xFF unknown
    uint8_t WeaponCount;
    uint8_t HaveCondition; // 1 = Armour[] and Structure[] are real reads
    uint8_t HaveHeat;

    float HeatPct; // 0..1 of capacity; meaningless unless HaveHeat

    // Fraction remaining, 0..1, indexed by TerrainMechPart. Armour is the ablative outer layer and
    // goes first; structure is what is under it, and a part whose structure reaches zero is gone
    // along with everything mounted in it.
    //
    // VEHICLES USE ONLY TMP_CentreTorso, and only Structure. That is not a shortcut - it is what
    // the game models. MWVR's tank, turret and VTOL paperdolls all cast to their own actor type,
    // find one MWSimpleLivingActorComponent with a single MWHealthComponent, and drive a single
    // widget called "Body" from GetHealthAsPercentage(). There is no armour layer and no per-part
    // breakdown to read, so publishing eight parts for a turret would be inventing seven of them.
    // Every other entry stays at -1, which already means "not read".
    float Armour[TMP_COUNT];
    float Structure[TMP_COUNT];

    // Rear torso faces, indexed by TerrainRearPart. HaveRear distinguishes "this mech's back is
    // intact" from "the rear containers did not resolve" - without it a consumer would draw an
    // untouched rear on a mech whose back it never read.
    float   ArmourRear[TRP_COUNT];
    float   StructureRear[TRP_COUNT];
    uint8_t HaveRear;
    uint8_t RearPad[3];

    TerrainWeapon Weapons[TERR_MAX_WEAPONS];

    // v13 - THE UNIT'S OWN ACTOR ADDRESS, split in two so the struct keeps 4-byte alignment.
    // VanillaHud publishes the same address for each lance member (TacMember::ActorLo/Hi), and
    // the TacOps plates join on it: exact, whatever the pilots are called (Instant Action names
    // them all "AI"), and never fooled by two mechs standing close together.
    uint32_t ActorLo, ActorHi;
};

// v14: DROPSHIPS (LidarExport dropship1). Every live dropship - MWDropship (the Leopard and kin) and the DLC3
// Union blueprints - with where it stands and which side it is on. Kind picks the hull outline HUDDisplay
// draws (assets/dropships/*.txt, from tools_dropship_outline.py); an unknown kind gets a box of its bounds.
static constexpr int32_t TERR_MAX_DROPSHIPS = 8;
enum TerrainDropshipKind : uint8_t
{
    TDK_Other = 0,           // a box of EX / EY / EZ around X / Y / Z
    TDK_Leopard,             // leopard.txt
    TDK_UnionDestructible,   // union_destructible.txt
    TDK_UnionTakeoff,        // union_takeoff.txt
    TDK_Broadsword,          // broadsword.txt (DLC7, 2026-10-19)
    TDK_Union,               // union.txt (the base-game landing Union, UnionDropship_C; 2026-10-19)
};
struct TerrainDropship
{
    float   X, Y, Z;    // the actor's origin, world cm (the outline files are in its frame)
    float   Yaw;        // degrees
    float   EX, EY, EZ; // bounds half-extent, cm (TDK_Other only; 0 otherwise)
    uint8_t Kind;       // TerrainDropshipKind
    uint8_t Side;       // 0 unknown, 1 friendly (your team), 2 enemy
    uint8_t Pad[2];
};

struct TerrainHeader
{
    uint32_t FrameCounter; // seqlock: bumped before the producer touches the header or slots
    uint32_t Version;      // TERRAIN_VERSION; another value means do not read on

    // Geometry echoed from the constants above so a consumer built against a different header
    // can detect the mismatch instead of misreading the grid.
    int32_t  SamplesPerSide;
    float    SpacingCm;
    float    TileCm;
    uint32_t TileCount;

    float PlayerX, PlayerY, PlayerZ; // world position, for the marker and the camera

    // Mech body heading and torso twist, degrees, in the same convention LidarShared uses, so a
    // consumer can project terrain exactly the way LidarView projects contacts:
    //     fwd   =  dx*cos(yaw) + dy*sin(yaw)
    //     right = -dx*sin(yaw) + dy*cos(yaw)
    // Duplicated from the LIDAR block on purpose: this page must work when the scope is not
    // running, and one float is a cheaper dependency than a second mapping.
    float PlayerYaw, PlayerTorsoYaw;

    // Measured height of the pawn's actor origin above the ground beneath it, or negative when
    // unknown. NOT a constant: it varies with the terrain under the mech. Published because the
    // origin is demonstrably not at the feet (measured 6.34 m), so anything placing the player
    // marker at PlayerZ would sit it well above the surface.
    float OriginAboveGroundCm;

    uint32_t Generation; // bumped when the cache is discarded; consumers must drop their copy

    // Where exploration started - the player's tile when the mission began. All search is
    // relative to this, so it is published for the consumer's readout and for debugging.
    int32_t AnchorTileX, AnchorTileY;

    // Discovered extent of ground, inclusive, over tiles that came back with Hits > 0. Only
    // meaningful once at least one tile is complete; Exploring says whether it is still growing.
    int32_t MinTileX, MinTileY, MaxTileX, MaxTileY;

    uint32_t Complete;  // slots reading TTS_Complete with Hits > 0
    uint32_t Claimed;   // slots in use at all
    uint32_t OffMap;    // candidate tiles rejected as having no ground
    uint32_t Exploring; // 1 while the producer is still looking for new ground
    uint32_t Pad0;

    TerrainSlotInfo Slots[TERR_MAX_TILES];

    // Buildings. Re-enumerated periodically rather than cached for the mission, so a structure
    // that is destroyed stops being published. StructureCount is 0 until the first scan
    // completes, and StructuresScanned says a scan has happened at all - a consumer must be able
    // to tell "no buildings on this map" from "not looked yet", or it will draw an empty layer
    // and call it an answer.
    uint32_t StructureCount;
    uint32_t StructuresScanned;
    TerrainStructure Structures[TERR_MAX_STRUCTURES];

    // Mech silhouettes, refreshed on the producer's contact cadence rather than per frame.
    // SilhouetteCount is how many slots are live; SilhouettesOk is 0 while the joint names have
    // not resolved, so a consumer can fall back to triangles instead of drawing a heap of
    // coincident points.
    uint32_t SilhouetteCount;
    uint32_t SilhouettesOk;
    TerrainSilhouette Silhouettes[TERR_MAX_SILHOUETTES];

    // Mech identity and condition, refreshed on its own cadence (the reads are far too heavy for
    // every tick). MechInfoScanned distinguishes "no mechs in range" from "not looked yet", the
    // same way StructuresScanned does - a consumer that cannot tell those apart will draw an empty
    // layer and present it as an answer.
    uint32_t MechInfoCount;
    uint32_t MechInfoScanned;
    TerrainMechInfo Mechs[TERR_MAX_MECHS];

    // v14: the dropships (see TerrainDropship), refreshed with the mech records.
    uint32_t DropshipCount;
    uint32_t DropshipPad;
    TerrainDropship Dropships[TERR_MAX_DROPSHIPS];

    uint32_t FrameCounterEnd; // equals FrameCounter when the header is consistent
};

struct TerrainBlock
{
    TerrainHeader H;
    TerrainTile   Tiles[TERR_MAX_TILES];
};
