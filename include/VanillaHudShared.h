// VanillaHudShared.h
//
// Cross-process contract between VanillaHud.dll (producer, inside the game) and HUDDisplay.exe
// (consumer). ONE definition, included by both - the TerrainShared.h model, not the duplicated
// ExportControlBlock model. Ours to version: bump VHUD_VERSION with any layout change, and both
// sides refuse a block whose version differs.
//
// v13 - TacOps: the plugin echoes back the point a move was actually sent to (it snaps the click
//      onto walkable ground, which can be tens of metres away, and the consumer needs that point
//      to tell when a lancemate has arrived), and the player's own mech can be told to walk to a
//      point (MWMech::MoveTo) and to stop (CancelMoveTo) - the autopilot.
// v12 - TACOPS: HUDDisplay's TacOps page orders the lance. The plugin publishes the lance (who
//      stands in which order slot, where, alive or not) and runs the orders HUDDisplay queues -
//      the game's own squad orders, posted exactly the way the F1-F4 order keys post them.
// v11 - the ZOOM CAMERA: a full-colour second camera for HUDDisplay's CAP page, following the game
//      camera at its own magnification (VS_ZoomCam), plus - while IR is on - a units-only depth
//      image beside it (VS_ZoomUnits) and scene depth in the colour image's alpha, for the IR view.
// v10 - up to 128 beams (a Nova alone fires 12 at once).
// v9 - DepthBeams: the laser beams live in the frame the depth image was rendered (MWRay actors
//      that are firing: muzzle to hit point), travelling with DepthCam - lasers are translucent
//      effects and never reach a depth image, so V-POD draws them from this.
// v8 - the VS_Depth texture is TWO depth images side by side, rendered from the same pose in the
//      same frame: left half the full scene, right half the UNITS only (mechs, tanks, turrets,
//      VTOLs). Where the two agree a unit is visible; the consumer highlights only those pixels.
//      Camera::UnitsValid is 1 when the right half holds a units image.
// v7 - DepthLock: the game's locked target (TargetTracking.CurrentTarget) as it stood in the frame
//      the depth image was rendered - world bounds centre and half-size - travelling with DepthCam,
//      so the lock bracket is live (every frame, not the 3 s contact scan) and sits on the outline.
// v6 - DepthCam: the camera pose that produced the depth image currently in the VS_Depth texture,
//      written by the producer while it holds that texture's keyed mutex, read by the consumer
//      while it holds it - so the 3D terrain is drawn from exactly the pose the outlines were.
// v5 - optical zoom for the Targeting screen: HUDDisplay asks for a magnification and the plugin
//      narrows the depth capture's field of view to it (a real re-render, not an enlarged crop).
// v4 - the mission dialogue (priority transmission box, subtitles) leaves the main view too and
//      is shown on the Objectives screen while a transmission is up. Sources renumbered.
// v3 - the Targeting screen is drawn, not captured: HUDDisplay renders its 3D map from the game's
//      camera (published here every frame) and lays over it the outlines of the game's own meshes,
//      from a depth-only scene capture the plugin runs while a Targeting screen asks for it.
//      World markers (nav points, objective and unit markers, target boxes) leave the main view too.
// v2 - "the whole HUD lives in the app":
//   * every vanilla HUD element is taken off the main screen as soon as the HUD exists (not on
//     request), each into its own texture SOURCE;
//   * the frames around them are hidden;
//   * whatever is left of the HUD - crosshair, lock ring, hit arrows, prompts, D-pad panel - moves
//     too, as one full-viewport OVERLAY texture, so it can be laid back over the game image;
//   * the game image itself is published as a source while a targeting screen asks for it.
// HUDDisplay composes SCREENS out of these sources; the screen list is further down.
//
// Textures are shared exactly like the MWVR screens: one keyed-mutex D3D11 texture per source,
// producer acquires 0 / releases 1, consumer acquires 1 / releases 0.

#pragma once

#include <cstddef>
#include <cstdint>

static constexpr const wchar_t *VHUD_MMF_NAME = L"MW5_VanillaHUDExport";
static constexpr uint32_t VHUD_VERSION = 23; // v23: EnhancedImaging.dll renders the vision mode (EiVisPluginSec), the wave (EiVisWant)
                                             // v22: ENHANCED IMAGING VISION MODE (EiVisionOn, EiVis*, GameW/H)
                                             // v21: ENHANCED IMAGING (VS_EiDepth, EiWantMs, EiCam / EiLock / EiBeams, VSC_Enhanced)
                                             // v20: MISSILE LOCK + ZOOM CAMERA POSE (+ VHS_InGame)
                                             // v19: NAV POINTS (Navs[], TCO_NavMark / TCO_NavUnmark)
static constexpr int32_t VHUD_MAX_SLOTS = 24;

// The depth capture runs only while a Targeting screen has asked for it this recently.
static constexpr uint64_t kVhudTargetingStaleMs = 2000;
static constexpr float    kVhudMaxZoom = 20.0f;

// ---- texture SOURCES (what the plugin publishes) -----------------------------------------------
enum VanillaSlot : uint8_t
{
    VS_Target = 0,       // HUD_TargetObject       - the target panel with its paperdoll
    VS_Weapons,          // HUD_WeaponLoadout
    VS_Owner,            // HUD_OwnerPanel         - your own mech's paperdoll
    VS_Objectives,       // HUD_ObjectivePanel
    VS_Map,              // MiniMap
    VS_Comms,            // LanceStatusWidget
    VS_Heat,             // HUD_HeatMeter
    VS_Throttle,         // HUD_Throttle
    VS_JumpJets,         // HUD_JumpJets
    VS_Compass,          // HUD_Compass
    VS_Torso,            // HUD_TorsoOrientation
    VS_Alerts,           // HUD_AlertMessages
    VS_ECM,              // HUD_ECMEquipment
    VS_PrimaryObjective, // HUD_PrimaryObjective
    VS_ELEMENT_COUNT,    // ---- the sources above are HUD elements; the ones below are not ----
    VS_Overlay = VS_ELEMENT_COUNT, // the rest of the HUD at full viewport size (aim + prompts)
    VS_Markers,                    // the world markers' layer at full viewport size, if they have one
    VS_Dialogue,                   // PriorityTransmissionWidget - the speaker's transmission box
    VS_Subtitles,                  // SubtitleWidget - when it is a widget of its own
    VS_Depth,                      // linear scene depth (cm, R32F) from the game camera, no HUD
    VS_ZoomCam,                    // the zoom camera: final colour (RGBA8), or with IR on scene colour
                                   //   HDR in RGB and scene depth (cm) in A (RGBA32F)
    VS_ZoomUnits,                  // the zoom camera's units-only depth (cm, R32F), while IR is on
    VS_EiDepth,                    // v21 ENHANCED IMAGING: the GAME's own scene depth (cm, R32F) | units-only capture
    VS_COUNT
};
static_assert(VS_COUNT <= VHUD_MAX_SLOTS, "more vanilla sources than the block holds");

// Element classes in the game's HUD_C, found by the class of what a HUD_C property points at.
static inline const wchar_t *VanillaSlotClass(int s)
{
    static const wchar_t *k[VS_ELEMENT_COUNT] = {
        L"HUD_TargetObject_C", L"HUD_WeaponLoadout_C",  L"HUD_OwnerPanel_C",    L"HUD_ObjectivePanel_C",
        L"MiniMap_C",          L"LanceStatusWidget_C",  L"HUD_HeatMeter_C",     L"HUD_Throttle_C",
        L"HUD_JumpJets_C",     L"HUD_Compass_C",        L"HUD_TorsoOrientation_C", L"HUD_AlertMessages_C",
        L"HUD_ECMEquipment_C", L"HUD_PrimaryObjective_C"};
    return (s >= 0 && s < VS_ELEMENT_COUNT) ? k[s] : L"";
}

static inline const wchar_t *VanillaSourceName(int s)
{
    static const wchar_t *k[VS_COUNT] = {
        L"target panel", L"weapon list", L"own paperdoll", L"objective list", L"minimap", L"lance status",
        L"heat meter", L"throttle", L"jump jets", L"compass", L"torso twist", L"alerts", L"ECM",
        L"primary objective", L"HUD overlay", L"world markers", L"transmission", L"subtitles",
        L"depth capture", L"zoom camera", L"zoom units", L"enhanced imaging"};
    return (s >= 0 && s < VS_COUNT) ? k[s] : L"?";
}

// v20: WHO TAKES EACH PANEL - GlobalSettings.ini [hud], one key per element (and "dialogue" for the
// transmission + subtitles): 1 (default) = HUDDisplay takes it off the game's HUD onto its V- page,
// 0 = it stays on the game's HUD. VanillaHud reads the file live (found through GlobalSettingsPath.txt);
// GlobalSettings writes it. "" = no switch (the depth / zoom captures, the overlay layers).
static inline const wchar_t *VhudHudKey(int s)
{
    static const wchar_t *k[VS_ELEMENT_COUNT] = {
        L"target",   L"weapons", L"owner",   L"objectives", L"minimap", L"comms",  L"heat",
        L"throttle", L"jumpjets", L"compass", L"torso",     L"alerts",  L"ecm",    L"primary_objective"};
    if (s == VS_ECM)
        return L""; // HUD_C has no HUD_ECMEquipment_C member (its ECM is a plain Overlay): an IN GAME piece instead
    if (s >= 0 && s < VS_ELEMENT_COUNT)
        return k[s];
    return (s == VS_Dialogue || s == VS_Subtitles) ? L"dialogue" : L"";
}

// ---- SCREENS (what HUDDisplay shows; the bezel button's parameter) ------------------------------
// Named after the MWVR screens with a Vanilla prefix. APPEND ONLY: configs store the number.
enum VanillaScreen : uint8_t
{
    VSC_Target = 0,  // target panel
    VSC_Weapons,     // weapon list
    VSC_Owner,       // own paperdoll + heat + throttle + jump jets
    VSC_Objectives,  // primary objective + objective list + alerts + transmissions
    VSC_Map,         // minimap
    VSC_Comms,       // lance status
    VSC_Tactical,    // HUDDisplay's 3D topographic map + compass + torso twist
    VSC_Targeting,   // pilot's-eye 3D map + the game's mesh outlines + crosshair / lock / markers / prompts
    VSC_TacOps,      // overhead 3D map: select lancemates with the mouse and order them
    VSC_Enhanced,    // v21: ENHANCED IMAGING - V-POD built on the game's own depth (no second scene render at 1x)
    VSC_COUNT
};

static inline const wchar_t *VanillaScreenName(int s)
{
    static const wchar_t *k[VSC_COUNT] = {
        L"Vanilla Target Screen",     L"Vanilla Weapons Screen", L"Vanilla Owner Screen",
        L"Vanilla Objectives Screen", L"Vanilla Map Screen",     L"Vanilla Comms Screen",
        L"Vanilla Tactical Screen",   L"Vanilla Targeting Screen", L"TacOps Screen", L"Enhanced Imaging"};
    return (s >= 0 && s < VSC_COUNT) ? k[s] : L"Vanilla (retired screen)";
}

static inline const wchar_t *VanillaScreenAbbrev(int s)
{
    static const wchar_t *k[VSC_COUNT] = {L"V-TGT", L"V-WPN", L"V-OWN", L"V-OBJ",
                                          L"V-MAP", L"V-COM", L"V-3D",  L"V-POD", L"TACOPS", L"E-IMG"};
    return (s >= 0 && s < VSC_COUNT) ? k[s] : L"V-?";
}

enum VanillaHostState : uint8_t
{
    VHS_Idle = 0,     // on the main HUD
    VHS_Hosted,       // moved to its off-screen component; texture published once it renders
    VHS_NotFound,     // this HUD has no element of that class
    VHS_Failed,       // hosting was attempted and could not be completed
    VHS_InGame,       // v20: left on the game's HUD by the player's choice (GlobalSettings [hud])
};

struct VanillaExportBlock
{
    uint32_t Version;        // VHUD_VERSION
    uint32_t ExportEnabled;  // 1 while the plugin is live
    uint64_t ProducerTickMs; // last GetTickCount64 the plugin published at

    struct Slot
    {
        uint64_t SharedHandle; // HANDLE of the keyed-mutex texture, 0 when none
        uint32_t Width;
        uint32_t Height;
        uint8_t  Active;    // a current texture exists this frame
        uint8_t  HostState; // VanillaHostState (elements and the overlay)
        uint8_t  Pad[6];
    } Slots[VHUD_MAX_SLOTS];

    // Consumer-written: GetTickCount64 of the last frame a Targeting screen was drawn. The depth
    // capture is a second scene render, so it only exists while someone is looking.
    uint64_t TargetingWantMs;

    // Producer-written every frame: the game camera. Seq is odd while being written; a reader
    // copies the struct and uses it only if Seq was even and unchanged across the copy.
    struct Camera
    {
        uint32_t Seq;
        uint32_t Valid;
        float    Loc[3];  // world cm
        float    Rot[3];  // pitch, yaw, roll, degrees
        float    FovDeg;  // horizontal
        float    UnitsValid; // DepthCam only: 1 = the depth texture's right half is the units-only image
    } Cam;

    // Consumer-written: the Targeting screen's magnification, 1..kVhudMaxZoom (0 is read as 1).
    // The capture renders at the game camera's field of view narrowed by this factor.
    float    TargetingZoom;
    // Producer (CameraCapture): 1 while the Enhanced Imaging .pak post-process is running - the scene edges,
    // units and wave are then drawn by the engine, and EnhancedImaging.dll draws only its overlay (grid,
    // labels, plates, crosshair, lock) on a transparent image. Was ZoomPad - same place, same size, no bump.
    uint32_t EiPakOn;

    // The pose (and field of view, already narrowed by the zoom) the depth image in the VS_Depth
    // texture was rendered from. Only touched under that texture's keyed mutex: see v6 above.
    Camera DepthCam;

    // The locked target in that same frame: axis-aligned world bounds (cm). Valid = 0 when there
    // is no lock. Same keyed-mutex rule as DepthCam.
    struct Lock
    {
        uint32_t Valid;
        uint32_t Pad;
        float    Origin[3]; // bounds centre
        float    Extent[3]; // half-size
    } DepthLock;

    // Lasers firing in that same frame (world cm). Same keyed-mutex rule as DepthCam.
    static constexpr int kMaxBeams = 128;
    struct Beam
    {
        float From[3]; // muzzle
        float To[3];   // hit point
    };
    uint32_t BeamCount;
    uint32_t BeamPad;
    Beam     Beams[kMaxBeams];

    // Consumer-written: the CAP page's zoom camera. It runs only while ZoomCamWantMs is fresh
    // (kVhudTargetingStaleMs), at ZoomCamZoom (1..kVhudMaxZoom) times the game camera's
    // magnification, in IR mode (ZoomCamIR = 1) or normal colour.
    uint64_t ZoomCamWantMs;
    float    ZoomCamZoom;
    uint32_t ZoomCamIR;

    // ---- v12: TACOPS ------------------------------------------------------------------------
    // Consumer-written: GetTickCount64 of the last frame the TacOps page was drawn. The lance
    // table below is only kept up to date while this is fresh (kVhudTargetingStaleMs).
    uint64_t TacWantMs;

    // Producer-written, seqlock like Cam (odd while writing). One entry per order slot the game
    // uses: OrderRecipients numbers 1..kTacMaxMembers, the numbers the F1-F4 keys send. The
    // player's own slot is marked IsPlayer and is never ordered.
    static constexpr int kTacMaxMembers = 8;
    struct TacMember
    {
        uint8_t Valid;    // a unit answers to this slot
        uint8_t IsPlayer; // it is the player
        uint8_t Dead;
        uint8_t Slot;     // the order slot, 1..kTacMaxMembers
        float   Loc[3];   // world cm, actor origin
        float   Yaw;      // degrees
        // The last move actually sent for this slot: the point after the plugin snapped it onto
        // walkable ground, and the command's Seq. Arrival must be judged against THIS, not
        // against the clicked point.
        float    LastMove[3];
        uint32_t LastMoveSeq; // 0 = none
        // v14: the unit's actor (its pawn) address, split for 4-byte alignment. LidarExport
        // publishes the same address in TerrainMechInfo::ActorLo/Hi; HUDDisplay joins on it.
        uint32_t ActorLo, ActorHi;
        // v18: its FIRE SETTINGS as the plugin holds them (TCO_FireSettings), and whether the plugin
        // fires for it right now (insane allies, the indirect hold, or non-default settings).
        uint8_t FireDisc;   // 0 weapons free, 1 return fire, 2 hold fire
        uint8_t FireMask;   // kFc* bits: the weapon categories it may fire
        uint8_t FireHeat;   // 0 normal (85 %), 1 cool (60 %), 2 hot (95 %), 3 alpha (no limit)
        uint8_t FirePlugin; // bit 0: the plugin does its firing; bit 1: the game's own Hold Fire is on (obeyed)
    };
    uint32_t  TacSeq;
    uint32_t  TacCount; // entries in use, in slot order
    TacMember TacMembers[kTacMaxMembers];

    // Consumer-written command ring, MORE THAN ONE WRITER (HUDDisplay, the voice module): a writer
    // CLAIMS command n by an interlocked compare-exchange of TacCmdHead from n to n + 1, then fills
    // TacCmds[n % kTacCmdRing] - Seq = 0 first, the fields, then Seq = n + 1 last. The producer
    // runs every command in order; a claimed slot whose Seq is not written yet is waited for
    // (up to 250 ms, then skipped as torn).
    enum TacOrder : uint8_t
    {
        TCO_Move = 0,     // GoToLocationOrder: Loc
        TCO_Attack,       // AttackOrder: the unit nearest Loc (within 40 m) that is not ours - or the exact unit (kTacLocActor, lancecontrol-61)
        TCO_Follow,       // FollowOrder: form on the player
        TCO_HoldFire,     // ToggleHoldFireWeaponsFreeOrder: the game's toggle
        TCO_SelfMove,     // the PLAYER's own mech walks to Loc (MWMech::MoveTo) - the autopilot
        TCO_SelfStop,     // and lets go of it again (CancelMoveTo)
        TCO_MoveFire,     // a Move during which the plugin keeps the lancemate out of combat
                          // manoeuvring (so it keeps walking) - an experiment: does it still shoot?
        TCO_Fallback,     // a Move during which, whenever the lancemate has an enemy, the plugin
                          // holds it IN combat manoeuvring - facing the enemy, reversing to the point
        TCO_HoldHere,     // v15: a Move to Loc, then HOLD there: the AI's own "Wait" switch keeps it
                          // standing, and the plugin keeps it supplied with a target to shoot
        // The PLAYER's torso (Slot is ignored). The plugin slews it and keeps it there until another
        // torso order, TCO_TorsoRelease, or the pilot's own look input.
        TCO_TorsoAngle,   // hold the torso at the angle, relative to the legs, that points at Loc now
        TCO_TorsoPoint,   // keep the torso on the world point Loc
        TCO_TorsoTarget,  // keep the torso on the unit nearest Loc (within 40 m), as it moves
        TCO_TorsoCentre,  // back in line with the legs, then let go
        TCO_TorsoRelease, // let go
        // A lancemate order: advance to Loc; on a contact (its AI has an enemy) or on taking damage,
        // fall back toward the player facing the enemy, hold near the player, and follow the player
        // again once there has been no contact for 10 s.
        TCO_AdvanceFallback,
        // A lancemate order: HOLD POSITION, INDIRECT FIRE ONLY - move to Loc and hold; the game's
        // firing is suppressed and the plugin fires the lock-on weapons; all weapons on the nearest
        // enemy that sees it while it is seen. Until another order.
        TCO_IndirectCover,
        // v18: a lancemate's FIRE SETTINGS (Slot): Loc[0] discipline (0 weapons free, 1 return fire,
        // 2 hold fire), Loc[1] the weapon-category mask (kFc* bits), Loc[2] heat (0 normal 85 %,
        // 1 cool 60 %, 2 hot 95 %, 3 alpha - no limit). Anything but all-defaults makes the plugin
        // do that lancemate's firing; back to defaults hands it to the game (unless insane allies).
        TCO_FireSettings,
        // An Attack on the PLAYER'S current target (TargetTracking.CurrentTarget), resolved by the
        // plugin when it runs - Loc is ignored. For voice commands ("attack my target").
        TCO_AttackMyTarget,
        // lancecontrol-11 (appended - no layout change): the PLAYER's lock on the unit nearest Loc
        // (within 40 m) - "patch it through" from a lancemate that sees it. Slot = that lancemate.
        TCO_LockTarget,
        // v19 (lancecontrol-22): NAV POINTS - not squad orders, run at once. Slot: any valid slot (ignored).
        TCO_NavMark,   // a nav point of your own at Loc (TacOps right-click menu); named with the next letter
        TCO_NavUnmark, // remove your own nav point nearest Loc (within 60 m)
        // lancecontrol-32 (appended - no layout change): a CALLED SHOT for lancemate Slot, and an attack on that
        // unit. Pad[0] the part: 0 cancel (no attack), 1 legs, 2 left leg, 3 right leg, 4 arms, 5 left arm,
        // 6 right arm, 7 side torsos, 8 left torso, 9 right torso, 10 centre torso, 11 head (left / right = the
        // target mech's own). Pad[1]: 1 = the PLAYER's current lock (Loc ignored), 0 = the unit nearest Loc (40 m) or the exact unit (kTacLocActor).
        // The aim applies to that unit only, until it dies, a cancel, another called shot, or a new mission.
        TCO_CalledShot,
    };
    // TCO_LockTarget (lancecontrol-13), TCO_Attack and TCO_CalledShot (lancecontrol-61) with Loc[2] == kTacLocActor: Loc[0] / Loc[1] carry the unit's address
    // (low / high 32 bits, copied bit for bit) instead of a position - the exact unit, not the nearest one.
    static constexpr float kTacLocActor = -9.0e30f;
    static constexpr int kTacCmdRing = 64;
    struct TacCmd
    {
        uint32_t Seq;
        uint8_t  Order; // TacOrder
        uint8_t  Slot;  // one recipient, 1..kTacMaxMembers
        uint8_t  Pad[2];
        float    Loc[3]; // world cm
    };
    uint32_t TacCmdHead;
    uint32_t TacCmdPad;
    TacCmd   TacCmds[kTacCmdRing];

    // ---- v16: THE TACOPS OVERLAY ---------------------------------------------------------------
    // HUDDisplay's window image, drawn over the game (flat screen) so TacOps can be used without
    // leaving it. HUDDisplay copies its back buffer into a shared keyed-mutex texture (it acquires
    // key 0 and releases 1; the plugin acquires 1 and releases 0) and keeps OvWantMs fresh while
    // the overlay is up. The plugin, while OvWantMs is fresh: zeroes the throttle once, puts the
    // game in UI-only input with the cursor showing (the game stops using mouse and keyboard),
    // draws the image over the frame, and reports where the cursor is over it.
    uint64_t OvWantMs;  // consumer: GetTickCount64 of the last frame the overlay should be up
    uint64_t OvHandle;  // consumer: the shared texture's handle, 0 = none
    uint32_t OvW, OvH;  // consumer: its size
    float    OvOpacity; // consumer: 0..1
    float    OvScale;   // consumer: the overlay's height as a fraction of the game's
    uint32_t OvShown;   // producer: 1 while it is drawn and the game is in UI-only input
    uint32_t OvMouseSeq; // producer: bumped with every cursor report
    float    OvMouseU, OvMouseV; // producer: the cursor within the overlay, 0..1 (outside: beyond)
    uint32_t OvButtons; // producer: bit 0 left, bit 1 right, bit 2 middle
    // v17: where it goes, and where it went.
    float    OvPosX, OvPosY; // consumer: top-left, as fractions of the game's width / height; -10 = centred
    float    OvRectX, OvRectY, OvRectW, OvRectH; // producer: where it was drawn, same fractions - the
                                                 // cursor report (OvMouseU/V) is relative to this rect
    // Consumer-written option switches (was OvPad - same place, same size, so no version bump).
    // (2026-10-07, lancecontrol-17: TWO bits now - HUDDisplay and LanceControl must both be rebuilt.)
    // Bit 0: insane allies. Bit 1: the game's own lancemate voices PLAY (unset = muted, the default).
    // Bits 2-31: the time it was written (GetTickCount64() / 1000, seconds;
    // the same clock in every process). Only a HUDDisplay instance that WANTS insane allies writes
    // it, every frame; an instance with it off leaves it alone. The plugin takes it as on while the
    // stamp is at most kVhudOptFreshS old - so any instance wanting it turns it on, instances cannot
    // overwrite each other, and it goes off by itself when none wants it (closed or crashed).
    uint32_t OptFlags;

    // ---- v19: NAV POINTS (lancecontrol-22) -----------------------------------------------------
    // Producer (LanceControl): the mission's nav points (the game's MWObjectiveIndicator nav markers) and your
    // own (TCO_NavMark), named Nav Alpha, Bravo, Charlie... (Letter indexes VhudNavName). The game's are lettered
    // by the game's own name for them when it has one, else by distance from the drop point; yours carry on
    // the letters. Cleared every mission. NavSeq is odd while writing (seqlock, as TacSeq).
    static constexpr int kNavMax = 16;
    struct NavPoint
    {
        float   Loc[3]; // world cm
        uint8_t Letter; // 0 = Alpha (VhudNavName)
        uint8_t Mine;   // 1 = marked by you (TacOps), 0 = the game's
        uint8_t Pad[2];
    };
    uint32_t NavSeq;
    uint32_t NavCount;
    NavPoint Navs[kNavMax];

    // ---- v20: MISSILE LOCK + ZOOM CAMERA POSE (cameracapture-5) -------------------------------
    // Producer (CameraCapture), every frame, seqlock like Cam (Seq odd while writing). The player's
    // own LockOnComponent (WeaponSystemComponent.LockOnComponent): State is ELockOnState as the game
    // stores it (3 = locked - the value LanceControl's insane allies has relied on; the others are
    // logged by the plugin as they are first seen), Percent is CurrentLockPercent normalised to
    // 0..1, OnTarget = 1 when the lock is on the CURRENT target (PendingLockTarget null or equal).
    // Bounds: the current target's world bounds this frame, only while V-POD or CAP is up.
    struct MissileLockInfo
    {
        uint32_t Seq;
        uint32_t State;
        float    Percent;
        uint32_t OnTarget;
        Lock     Bounds;
    } MissileLock;

    // Producer, seqlock like Cam: the pose and field of view (zoom included) the CAP page's zoom
    // camera was last pointed with.
    Camera ZoomCam;

    // ---- v21: ENHANCED IMAGING (cameracapture-12) ----------------------------------------------
    // Consumer-written: GetTickCount64 of the last frame an Enhanced Imaging screen was drawn at 1x.
    // While fresh (kVhudTargetingStaleMs) the plugin publishes VS_EiDepth: the left half is the GAME's
    // own scene depth turned into cm (no second scene render), the right half a units-only capture
    // rendered from the same pose at the same size. Zoomed, Enhanced Imaging uses V-POD's capture.
    uint64_t EiWantMs;
    // Producer, under VS_EiDepth's keyed mutex (as DepthCam): the pose / lock / lasers of that frame.
    Camera   EiCam;
    Lock     EiLock;
    uint32_t EiBeamCount;
    float    EiNearCm; // the near plane used for the conversion (diagnostic)
    Beam     EiBeams[kMaxBeams];

    // ---- v22: ENHANCED IMAGING VISION MODE (cameracapture-13) -----------------------------------
    // Enhanced Imaging full screen over the game, opaque, in place of the view (flat screen).
    // EiVisionOn: consumer-written by any HUDDisplay (hotkey / bezel action) or VoiceCommand ("activate /
    // deactivate enhanced imaging"): 1 = on. ONE HUDDisplay (the holder of the Local\MW5_HUDDisplay_EiVision
    // mutex) renders the page at the game's size into a shared image (EiVisHandle, keyed mutex like the
    // overlay: it releases 1, the plugin acquires 1) and stamps EiVisFrameMs; CameraCapture draws it over the
    // whole back buffer while the stamp is fresh. The game keeps its input - nothing is captured.
    uint32_t EiVisionOn;
    // v23: EnhancedImaging.dll's heartbeat, GetTickCount64() / 1000 - while fresh (3 s) it renders the vision
    // image and HUDDisplay does not.
    uint32_t EiVisPluginSec;
    uint64_t EiVisHandle;
    uint32_t EiVisW, EiVisH;
    uint64_t EiVisFrameMs;
    // Producer (CameraCapture): the game's back buffer size, for the vision image.
    uint32_t GameW, GameH;
    // v23, producer (CameraCapture): 1 while it wants vision frames - on, or the closing wave still running.
    // The renderers render while this is set (not EiVisionOn, which drops the moment it is switched off).
    uint32_t EiVisWant;
    // v23, consumer: 1 = the depth capture (VS_Depth) at the FULL viewport resolution (Enhanced Imaging zoomed),
    // 0 = half (V-POD, unchanged). The plugin remakes the capture when this changes.
    uint32_t TargetingFullRes;
};

static constexpr uint32_t kVhudLockLocked = 3; // ELockOnState value for "locked" (see MissileLock)

// v19: a nav point's letter as said and shown ("Alpha"); "" past the end.
inline const wchar_t *VhudNavName(int letter)
{
    // 2026-10-14 (user): the NATO alphabet (was Greek - Mu / Nu, Upsilon / Epsilon, Chi / Rho were misheard). Kilo and
    // Victor are never given out (LanceControl): "kilo" is a distance, Victor a mech.
    static const wchar_t *k[] = {L"Alpha", L"Bravo",  L"Charlie", L"Delta",  L"Echo",   L"Foxtrot", L"Golf",    L"Hotel",
                                 L"India", L"Juliet", L"Kilo",    L"Lima",   L"Mike",   L"November", L"Oscar",  L"Papa",
                                 L"Quebec", L"Romeo", L"Sierra",  L"Tango",  L"Uniform", L"Victor", L"Whiskey", L"X-ray",
                                 L"Yankee", L"Zulu"};
    return (letter >= 0 && letter < 26) ? k[letter] : L"";
}
static constexpr int kVhudNavLetters = 26;

// Weapon categories for TCO_FireSettings / TacMember::FireMask.
static constexpr uint8_t kFcLaser = 1, kFcPPC = 2, kFcBallistic = 4, kFcLockMissile = 8, kFcDumbMissile = 16,
                         kFcMGFlamer = 32, kFcTAG = 64, kFcNARC = 128, kFcAll = 0xFF;

// OptFlags bits.
static constexpr uint32_t kVhudOptInsaneAllies = 1u; // the plugin fires every ready lancemate weapon
static constexpr uint32_t kVhudOptGameVoices   = 2u; // the game's own lancemate voice lines play (default: muted)
static constexpr uint32_t kVhudOptFreshS = 2u;
inline uint32_t VhudOptStampNow(uint64_t tickMs) { return (uint32_t)((tickMs / 1000u) & 0x3FFFFFFFu); }
inline uint32_t VhudOptMake(uint32_t bits, uint64_t tickMs) { return (bits & 3u) | (VhudOptStampNow(tickMs) << 2); }
inline bool VhudOptOn(uint32_t flags, uint32_t bit, uint64_t tickMs)
{
    return (flags & bit) != 0 && ((VhudOptStampNow(tickMs) - (flags >> 2)) & 0x3FFFFFFFu) <= kVhudOptFreshS;
}

static_assert(sizeof(VanillaExportBlock::Slot) == 24, "VanillaExportBlock::Slot layout changed");
static_assert(offsetof(VanillaExportBlock, Slots) == 16, "VanillaExportBlock::Slots moved");
static_assert(sizeof(VanillaExportBlock::Camera) == 40, "VanillaExportBlock::Camera layout changed");
static_assert(sizeof(VanillaExportBlock::Lock) == 32, "VanillaExportBlock::Lock layout changed");
static_assert(sizeof(VanillaExportBlock::TacMember) == 48, "TacMember layout changed");
static_assert(sizeof(VanillaExportBlock::TacCmd) == 20, "TacCmd layout changed");
static_assert(sizeof(VanillaExportBlock) == 16 + 24 * VHUD_MAX_SLOTS + 8 + 40 + 8 + 40 + 32 + 8 + 24 * 128 + 16 +
                                               8 + 8 + 48 * 8 + 8 + 20 * 64 + 80 + 8 + 16 * 16 + 48 + 40 +
                                               8 + 40 + 32 + 8 + 24 * 128 + 40 + 8,
              "VanillaExportBlock size changed");
static_assert(sizeof(VanillaExportBlock::NavPoint) == 16, "NavPoint layout changed");
static_assert(sizeof(VanillaExportBlock::MissileLockInfo) == 48, "MissileLockInfo layout changed");
