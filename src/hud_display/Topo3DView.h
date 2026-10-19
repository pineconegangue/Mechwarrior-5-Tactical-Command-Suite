// Topo3DView.h
//
// The 3D tactical page: terrain as a depth-buffered fishnet with the full LIDAR picture standing
// on it. Same producers as everything else - MW5_TerrainExport for the ground, MW5_LidarExport
// (via LidarView) for the contacts - and the same 1000 m horizon as the scope, so the two pages
// show the same world at the same scale and can be compared button-to-button.
//
// =========================================================================================
// WHY THIS ONE IS D3D11 AND THE OTHERS ARE NOT
//
// The 2D page bakes its wireframe in WORLD space and places it with a Direct2D matrix, so
// panning and rotating are free and the geometry is never rebuilt. That trick dies here:
// perspective projection is not affine, so there is no 2D matrix that maps world metres onto
// the screen, and every vertex has to be re-projected whenever the camera moves. Doing that on
// the CPU each frame resurrects exactly the per-frame cost the 2D page spent two revisions
// removing.
//
// In D3D11 it costs nothing: vertices live in vertex buffers in world metres, the camera is a
// matrix in a constant buffer, and moving it is a 64-byte upload. The depth buffer then gives
// hidden-surface removal for free, which is what makes a fishnet read as solid ground instead
// of a transparent mess of crossing lines.
//
// (An earlier note in this project claimed 3D needed D3D11 *because* of the depth buffer. That
// was the right answer for the wrong reason - occlusion can be faked by sorting. It is the
// projection that forces the issue.)
//
// =========================================================================================
// WHAT IS DRAWN WHERE
//
//   D3D11 - the terrain only. Thousands of primitives, needs depth.
//     pass 1: triangles in background colour, depth write on. These are invisible; their only
//             job is to occlude what is behind the hill.
//     pass 2: the lattice as lines, depth-tested, faded with distance.
//
//   Direct2D - everything else: contacts, stalks, the ping sweep, the aim ray, the player, and
//     the readout. There are only ever dozens of these, CPU-projecting them is free, and D2D
//     draws far better text and shapes than a line shader would.
//
// Contacts are drawn ON TOP of terrain rather than depth-tested against it. That is deliberate:
// a tactical display exists to show you the mech behind the ridge. Hiding it behind the ridge
// would be more physically honest and less useful.
//
// =========================================================================================
// THE CAMERA
//
// Heading-up chase by default - the mech's body heading is "away from you", matching the 2D page
// and the scope, so all three agree about which way is forward. The pilot can orbit and pan off
// that, and RECENTRE snaps back.
//
// Vertical exaggeration is 1.2:1 and fixed. 1:1 was the honest choice and it read flatter than
// the ground actually is, because perspective compresses height much harder than a top-down view
// does - the same relief that looks dramatic from above looks like gentle swells from a chase
// camera. 1.2 restores the impression without turning a 30 degree slope into a cliff.

#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID2D1Factory;
struct ID2D1RenderTarget;
struct IDWriteFactory;

struct LidarScene;
class Topo3DViewImpl;

class Topo3DView
{
  public:
    Topo3DView();
    ~Topo3DView();

    Topo3DView(const Topo3DView &) = delete;
    Topo3DView &operator=(const Topo3DView &) = delete;

    // Camera limits. Range is shared with the LIDAR scope on purpose - the pilot should not have
    // to learn two different notions of "how far can I see".
    static constexpr float kMinPitchDeg = 8.0f;
    static constexpr float kMaxPitchDeg = 80.0f;
    static constexpr float kDefaultPitchDeg = 30.0f;
    static constexpr float kPanStepM = 100.0f;

    // ---- mesh resolution -----------------------------------------------------------------
    // The flat page uses 64 m cells and that is right for it: seen from directly above, a 64 m
    // graticule reads cleanly and the shape of the ground comes from the colour ramp.
    //
    // In perspective the shape comes from the SILHOUETTE, and decimating 16 m data to 64 m is
    // point sampling - it keeps whichever sample happens to land on the lattice and discards
    // the three between. A ridge crest that falls between samples flattens; a lone high sample
    // becomes a spike. That is exactly the "some hills too steep, some too flat" you get, and
    // it is aliasing rather than a bug to be found.
    //
    // So this page draws the RAW 16 m data with no decimation at all: every sample the producer
    // traced becomes a vertex, and the silhouette is exactly the ground the plugin measured.
    // About 34,000 lines at 1000 m range, which sounds like a lot until you remember the flat
    // page was drawing 50,000 through Direct2D - these go through one vertex buffer per tile and
    // a depth test, which is what D3D11 is for.
    //
    // The cost that DOES move is tile building: 1,089 vertices instead of 81, each needing a
    // slot lookup, which is why SlotForTile grew a memo. Set topo3d_cell_m to 32 or 64 if the
    // build hitch ever shows up in the log.
    static constexpr int kDefaultCellM = 16;

    // Device objects are the host's; this view borrows them and owns only its own depth buffer,
    // shaders and vertex buffers.
    bool InitResources(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID2D1Factory *d2dFactory,
                       IDWriteFactory *dw);

    // Called on every resize, and with nulls before the swap chain is resized. The depth buffer
    // is rebuilt to match; nothing else is device-dependent.
    void SetTargets(ID3D11RenderTargetView *rtv, ID2D1RenderTarget *d2d, unsigned width, unsigned height);

    void SetContentInset(float inset);

    // Metres from the mech to the edge of the view. Driven from the same value as the scope.
    void SetRangeM(unsigned metres);

    // Cell size of the terrain lattice, metres - the same fixed graticule the 2D page uses.
    void SetCellM(int metres);

    // Pilot camera: draw from the game's own camera (UE world cm, degrees pitch/yaw/roll,
    // HORIZONTAL field of view) instead of the chase pose, at true vertical scale, green only,
    // with nothing but the ground and the rounds - the base layer of the vanilla Targeting
    // screen. Set every frame before Render; `on = false` returns to the normal page.
    void SetPilotCamera(bool on, const float *locCm, const float *rotDeg, float hfovDeg);

    // Cheap; opens MW5_TerrainExport if it is not open yet. Safe to call every frame.
    void Poll();

    // Draws the page. `scene` comes from LidarView::BuildScene, so contacts arrive with their
    // ping fade and selection already resolved and this view never re-derives any of it.
    void Render(const LidarScene &scene);

    // Pilot camera only, called AFTER whatever is laid over the page (mesh outlines, HUD): the
    // red acquisition bracket around the game's locked unit and its doll-and-weapons plate on a
    // stalk above it. Does nothing outside pilot mode or without a lock.
    void RenderPilotLock(const LidarScene &scene);

    // Pilot camera only: lasers firing in the frame the outlines come from, `n` pairs of world
    // points in cm laid out {fromX, fromY, fromZ, toX, toY, toZ}. Set before, draw after the
    // outlines (RenderPilotBeams).
    void SetPilotBeams(int n, const float *fromTo);
    void RenderPilotBeams();

    // Pilot camera only, after Render: window-pixel boxes (x0, y0, x1, y1) around every contact
    // on screen this frame. Returns how many were written (at most `max`).
    int PilotUnitBoxes(float (*out)[4], int max) const;

    // Pilot camera only, before Render: the game's locked target as it stood in the frame the
    // outlines come from - world bounds centre and half-size, cm. Drives the lock bracket live.
    void SetPilotLiveLock(bool valid, const float *originCm, const float *extentCm);

    // Pilot camera only, before Render: draw NOTHING of the page itself (no ground, no rounds) -
    // just work out the view, so RenderPilotLock can lay the bracket and plate over an image that
    // is not this page's own (the CAP page's zoom camera). Set back to false after.
    void SetPilotBare(bool bare);
    // Transparent page (the Enhanced Imaging .pak overlay): the buffer is cleared to nothing instead of the
    // background colour, and the terrain occluders hide lines behind hills without painting the ground.
    void SetTransparent(bool on);

    // Pilot camera only, before RenderPilotLock: the player's missile lock on the bracketed unit.
    // acquiring + progress 0..1: a thin square closing in onto the bracket; locked: the four
    // corners become one solid square of the same line weight.
    void SetPilotMissileLock(bool acquiring, float progress, bool locked);

    // ---- TacOps --------------------------------------------------------------------------
    // The command page: the 3D map exactly as the 3D page draws it (chase camera, torso slew),
    // plus the lance - each lancemate ringed and numbered by its order slot, its queued route
    // drawn - and the lancemates' plates in a column down the right-hand side.
    struct TacMate
    {
        uint8_t Slot;     // the game's order slot, shown as the lancemate's number
        uint8_t Selected;
        uint8_t Dead;
        uint8_t IsPlayer; // the pilot's own mech - drawn as YOU, driven by the autopilot
        float   Loc[3];   // world cm
        float   Yaw;      // degrees
        int     WpCount;  // queued moves, Wp[0] being walked to now
        float   Wp[8][3]; // world cm
        int     Order;    // what it was last told: 0 none, 1 move, 2 attack, 3 follow
        uint32_t ActorLo, ActorHi; // the unit's actor address: the plate's record is the one with the same
        // Fire settings (TCO_FireSettings) and whether the plugin fires for it: a tag on its plate.
        uint8_t FsDisc = 0, FsMask = 0xFF, FsHeat = 0, FsPlugin = 0;
        // 2026-10-19: VoiceCommand's voice trace (MW5_VoiceIntent), drawn at the end of the variant line.
        uint8_t HaveVoice = 0, Speaking = 0;
        int8_t  WaveLo[144] = {}, WaveHi[144] = {}; // oldest first
        // 2026-10-19: what it is actually doing (the lance status feed, V-LNC's wording, short). Empty = no feed:
        // the plate falls back to the TacOps order word. "{T}" = the target (TargetLo/Hi) by name.
        wchar_t  OrderText[48] = {};
        uint32_t OrderCol = 0;
        uint32_t TargetLo = 0, TargetHi = 0;
    };
    struct TacOpsView
    {
        int     MateCount;
        TacMate Mates[8];
        uint8_t OwnImage; // the caller will draw the game's own-mech paperdoll into the player's plate
    };
    // nullptr returns to the normal page. The pointer is read during Render only.
    void SetTacOps(const TacOpsView *v);

    // V-LNC (2026-10-19): the lance page - one big plate per lancemate (the player left out), drawn
    // with the TacOps plate pieces at page size. The caller fills it (TacOps members, the lance
    // status feed) and calls RenderLancePage instead of Render; nothing 3D is drawn.
    struct LanceAmmo
    {
        wchar_t Name[24];
        int     Count, Max;
    };
    struct LanceCard
    {
        uint8_t  Slot;
        uint8_t  Dead;
        uint8_t  HaveFeed;    // the lance status feed has this slot (order, heat, ammo below are real)
        uint32_t ActorLo, ActorHi; // the plate's LidarExport record is the one with the same actor
        wchar_t  Name[40];    // pilot callsign / name ("" = from the record)
        wchar_t  Order[96];   // what it is doing now ("ATTACKING Warhammer - 320 m")
        uint32_t OrderCol;    // 0xRRGGBB for the order line
        wchar_t  Order2[96];  // the detail line ("target in sight", "ordered 12 s ago")
        uint32_t TargetLo, TargetHi; // its combat target: "{T}" in Order / Order2 becomes the target's name
        uint8_t  FsDisc, FsMask, FsHeat, FsPlugin; // fire settings, shown as on the TacOps plate
        float    Heat;        // 0..1, -1 unknown
        // VoiceCommand (MW5_VoiceIntent): its open question, and its voice on the radio.
        wchar_t  Pending[96]; // "" = none ("waiting for your hill choice: Nav B, C or D")
        uint8_t  Speaking;    // on the air now
        uint8_t  Queued;      // lines waiting to be spoken
        uint8_t  HaveVoice;   // VoiceCommand is running (the waveform is real)
        int8_t   WaveLo[144]; // peak waveform, oldest first: each 11 ms column's lowest sample (-127..0)
        int8_t   WaveHi[144]; // ... and highest (0..127)
        int      AmmoCount;
        LanceAmmo Ammo[12];
    };
    struct LancePageView
    {
        int       Count;
        LanceCard Cards[3];
    };
    void RenderLancePage(const LancePageView &v);

    // bug sweep 21 (2026-10-18): around a render for someone else (the Enhanced Imaging fallback), keep the window's
    // camera, TacOps picking state and tile cache selection as they were.
    void SaveView();
    void RestoreView();
    // NAV POINTS (2026-10-07k): drawn on the 3D page and TacOps - a flag on the ground with its name. Copied.
    struct NavMark
    {
        float   Loc[3];   // world cm
        uint8_t Mine;     // 1 = marked by you (another colour)
        wchar_t Name[16]; // "NAV ALPHA"
    };
    void SetNavPoints(const NavMark *pts, int n);
    // DROPSHIPS (2026-10-08, TERRAIN v14): drawn here (3D page, TacOps) as their hull outline; also handed to the
    // host for the LIDAR scope. Side: 0 unknown, 1 friendly, 2 enemy. Kind: TerrainDropshipKind.
    struct DropshipMark
    {
        float   X, Y, Z, Yaw, EX, EY, EZ;
        uint8_t Kind, Side;
    };
    int GetDropships(DropshipMark *out, int max) const;
    // Same as Recentre.
    void TacRecentre();
    // Window pixel -> the ground under it (world cm), from the last frame drawn.
    bool TacPickGround(float sx, float sy, float outCm[3]) const;
    // World cm -> window pixel, from the last frame drawn.
    bool TacProject(const float cm[3], float &sx, float &sy) const;
    // The hostile contact drawn nearest (sx, sy) within radiusPx: its Id, world cm in outCm.
    // 0 when none.
    int TacHostileAt(float sx, float sy, float radiusPx, float outCm[3]) const;
    // 2026-10-12 (COMPASS): the cockpit heading (body + torso twist, degrees, X north / Y east - the bearing the
    // voice orders use), where you are (world cm), and the live hostiles the map shows (world cm), all from the
    // last frame drawn.
    float CompassHeadingDeg() const;
    bool CompassPlayerCm(float out[3]) const;
    int CompassHostilesCm(float (*out)[3], int max) const;
    // RADAR MODE (2026-10-12): the host's hostile filter (world cm; false = hide) for the enemy skeletons, which
    // come from LidarExport's terrain feed rather than the LIDAR scene. Set = Vanilla mode: the compass dots then
    // only use hostiles live this frame (not grey last-known marks). Null = Full LIDAR.
    typedef bool (*HostileKeepFn)(void *ctx, float x, float y, float z);
    void SetHostileFilter(HostileKeepFn fn, void *ctx);
    // The live hostile contact nearest `fromCm` within maxM: its Id, 0 when none.
    int TacNearestHostile(const float fromCm[3], float maxM, float outCm[3]) const;
    // Which lancemate panel (by slot) is under (sx, sy); 0 for none.
    int TacPanelAt(float sx, float sy) const;
    // Where the lancemate panel column starts, window x - page controls stay left of it.
    float TacPanelLeft() const;
    // The body of the player's own plate in the last frame drawn (window pixels), for the caller
    // to draw the game's own-mech paperdoll into. False when there is no player plate.
    bool TacOwnPlate(float &l, float &t, float &r, float &b) const;
    // Your torso arrow in the last frame drawn (window pixels): origin and tip. False when not drawn.
    bool TacTorsoArrow(float &ox, float &oy, float &tx, float &ty) const;
    // Move the TacOps camera over the map, world metres.
    void TacPanWorld(float dxM, float dyM);
    // Ground height under a world point (cm in, cm out); false where the map has no heights.
    bool TacGroundAt(float xCm, float yCm, float &zCm) const;
    // A contact's latest position by id; false once it is dead or unknown.
    bool TacContactLoc(int id, float outCm[3]) const;
    // 2026-10-16 (lancecontrol-61): a mech contact's unit address (its LidarExport mech record); false for anything else.
    bool TacContactActor(int id, uint32_t &lo, uint32_t &hi) const;
    // The lancemate panel column: fixed-size plates from kTacPanelTop below the content's top.
    static constexpr float kTacPanelW = 230.0f;
    static constexpr float kTacPanelH = 84.0f;
    static constexpr float kTacPanelGap = 8.0f;
    static constexpr float kTacPanelTop = 40.0f;

    void Shutdown();
    bool IsConnected() const;

    // ---- camera control ------------------------------------------------------------------
    // Orbit and pan are offsets FROM the chase pose, never absolute, so Recentre is always a
    // return to a known state rather than a guess at one.
    void Orbit(float dYawDeg, float dPitchDeg);
    void Pan(float dForwardM, float dRightM);
    void Recentre();
    bool IsFree() const; // true once the pilot has moved off the chase pose
    // The contact whose paperdoll is shown (0 = none), and the one drawn nearest a window pixel
    // in the last frame (its label's anchor, or its doll while shown; 0 = none within radiusPx).
    int  PickContact(float sx, float sy, float radiusPx) const;
    void SetShownContact(int id);
    // ART BUDGET (2026-10-13): ms spent decoding paperdoll PNGs since the last call (for a host's slow-frame log).
    double TakeArtLoadMs();
    int  ShownContact() const;

    // ---- the label layer: click to show ---------------------------------------------------
    //   the game's lock and the clicked contact (one at a time) - a full plate (doll, rear strip,
    //                         name, weapons) standing above the unit, as on V-POD; bottom-left /
    //                         bottom-right with an OFF SCREEN hint when the unit is not on the page
    //   every other one     - the variant name only, cut to kFarNameChars, no doll
    static constexpr int   kFarNameChars = 10;

    struct Diag
    {
        bool Connected;
        bool Free;
        int TilesDrawn;
        uint32_t Lines;
        int Contacts;
        int Projectiles;
        int Silhouettes; // mechs drawn as posed skeletons
    int Labels;      // contacts labelled this frame
    int DollArt;     // dolls drawn with the chassis's real silhouette
    int CulledFar;       // contacts dropped past the horizon
    int CulledOffScreen; // contacts that did not project
    int NoRecord;        // MECH contacts with no identity record to join to
    int MechContacts;    // scene contacts by kind, to separate "absent" from "invisible"
    int VehicleContacts;
    int ArtMisses;        // units drawn with boxes because no art matched their name
    char MissingArt[32];  // the most recent such name, so it can be chased down
    int  FocusWeapons;    // weapons on the focused unit, -1 when nothing is focused
    char FocusName[32];   // and which unit that is
    int InArc;       // contacts drawn with their doll this frame (the clicked one)
    int MechInfo;    // identity records the producer is publishing
    int LockedId;    // the game's primary lock as the cue panel sees it, 0 for none
        int Trails;   // rounds carrying smoke; 0 with missiles present means the trails are dead
        int Flashing; // rounds guiding on us during the tone's ON phase
        float BuildMs;
        float YawDeg, PitchDeg;
        unsigned RangeM;
        int CellM;
    };
    Diag GetDiag() const;

  private:
    Topo3DViewImpl *P = nullptr;
};
