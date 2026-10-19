// LidarView.h
//
// Host-facing surface of the LIDAR tactical scope inside HUDDisplay.
//
// The implementation in LidarView.cpp is a copy of src/lidar_display/Main.cpp with the
// window, message loop and options menu removed. The standalone LidarDisplay.exe is
// deliberately untouched by this port.
//
// Pimpl so Main.cpp never needs the Direct2D or LidarShared headers.

#pragma once

#include <cstdint>

struct ID2D1Factory;
struct ID2D1RenderTarget;
struct IDWriteFactory;

class ITerrainUnderlay;

// ---------------------------------------------------------------------------------------
// A renderer-agnostic view of one frame of LIDAR data.
//
// Plain data on purpose. The scope's contact state is not simple - tracks are revealed by the
// ping, fade over about a second, expire when the sweep passes where they used to be, and are
// gated by per-kind layer toggles - and that machinery took two missions in the field to get
// right. A second renderer has no business re-deriving any of it, so what it gets is the
// ANSWER: the contacts that should be drawn, each with its fade and selection already
// resolved. Keeping these POD also keeps LidarShared.h out of this header.
// ---------------------------------------------------------------------------------------
struct LidarSceneContact
{
    float X, Y, Z;    // world position, cm - Z is real, published by the plugin, not inferred
    float Yaw;        // world facing, degrees
    uint8_t Type;     // LidarContactType
    uint8_t Kind;     // LidarKind
    uint8_t Weight;   // LidarWeight
    uint8_t Selected; // 1 = the player's locked target
    float Alpha;      // ping fade, already applied: 1 at reveal, 0 when it should vanish
    uint16_t Id;      // the producer's stable per-actor id, carried through so the terrain
                      // block's mech identity records can be joined to this contact BY ID.
                      // Matching on position instead would swap two mechs brawling at melee
                      // range, and would do it silently.
};

// A round, with everything the scope's own styling depends on already decided. Matching the
// 2D look means matching its DECISIONS - the extrapolated position, the streak origin, whether
// this round is part of the salvo guiding on us and therefore flashing amber in phase with the
// tone - not just its colours. Re-deriving any of that in a second renderer would drift, and
// the drift would be invisible until the two pages disagreed about an incoming missile.
struct LidarSceneTrailPt
{
    float X, Y, Z;
    // 1 = do NOT draw the segment from the previous point to this one, but keep walking the
    // polyline. The scope drops SEGMENTS that fail the jump guard or leave scope range while
    // leaving the point sequence intact; dropping the POINTS instead would splice the next
    // point onto the previous surviving one - drawing a line straight across the very gap the
    // guard exists to prevent - and would shift the 0.45*k/(n-1) fade ramp, which is indexed
    // over the original array.
    uint8_t Break;
};

struct LidarSceneProjectile
{
    float X, Y, Z;             // cm, already extrapolated the way the scope extrapolates
    float FromX, FromY, FromZ; // streak origin; only meaningful when HasStreak
    uint8_t Type;              // LidarProjType
    uint8_t Flags;             // LPROJ_* bits
    // Colour class, resolved HERE against the real LidarProjType constants: 0 ballistic,
    // 1 PPC, 2 missile. A second renderer mirroring those enum values locally would be one
    // silent renumbering away from painting PPC bolts brown.
    uint8_t ColorIdx;
    uint8_t IsMissile;
    uint8_t IsArrowIV;  // draws 3x larger, as on the scope
    uint8_t Flashing;   // guiding on us AND the alarm tone is in its ON phase
    uint8_t HasStreak;
    int TrailStart;     // index into LidarScene::Trail, or -1
    int TrailCount;     // smoke points; >= 2 to draw
};

struct LidarScene
{
    bool Valid = false; // false when the publisher is absent or has stopped advancing
    float PlayerX = 0, PlayerY = 0, PlayerZ = 0;    // cm
    float PlayerYaw = 0, PlayerTorsoYaw = 0;        // degrees
    float RangeCm = 0;
    float PulseCm = 0;   // how far the ping sweep has travelled; 0 when none is in flight
    uint32_t Layers = 0; // the scope's layer mask, so a second renderer honours the same toggles
    int ContactCount = 0;
    const LidarSceneContact *Contacts = nullptr;
    int ProjectileCount = 0;
    const LidarSceneProjectile *Projectiles = nullptr;
    // Missile smoke, all trails packed end to end; each projectile indexes its own run. Jump
    // guards and range clipping are already applied, so a consumer can draw the run as-is.
    int TrailPointCount = 0;
    const LidarSceneTrailPt *Trail = nullptr;
};

class LidarViewImpl;

class LidarView
{
  public:
    LidarView();
    ~LidarView();

    LidarView(const LidarView &) = delete;
    LidarView &operator=(const LidarView &) = delete;

    // Scope range band, in metres. The ceiling matches the plugin's fixed 1000 m scan
    // range, so every step shows real contacts rather than empty margin. Raising it means
    // raising the plugin's kRangeCm too - and fixing the standalone LidarDisplay.exe,
    // which shares the same memory-mapped file and does not clip to its own range.
    static constexpr uint32_t kMinRangeM = 100;
    static constexpr uint32_t kMaxRangeM = 1000;
    static constexpr uint32_t kRangeStepM = 100;
    // THE 5 KM RADAR MODE (2026-10-15, user): with SetFar(true) the band goes to 5 km (500 m steps past 1 km) and the
    // plugin is asked for its far ranges (LIDAR_CTRL_FAR). LidarDisplay.exe is retired, so the old lock-step is gone.
    static constexpr uint32_t kMaxRangeFarM = 5000;
    static constexpr uint32_t kRangeStepFarM = 500;

    // Layer metadata, for the host's edit-mode menu.
    static int LayerCount();
    static const wchar_t *LayerName(int index);
    static uint32_t DefaultLayers();
    // Bit for the missile-alarm layer, so the host can migrate old configs that predate it
    // without hard-coding an index that would rot the moment a layer is inserted.
    static uint32_t AlarmLayerMask();

    // Factories are the host's, so there is one D2D/DWrite stack in the process.
    bool InitResources(ID2D1Factory *factory, IDWriteFactory *dw);

    // Terrain underlay: the ground wireframe drawn inside the scope disc, beneath the rings and
    // contacts. Borrowed, never owned; null means no terrain, which is also what happens when
    // the terrain producer is not running. The scope passes its own centre, radius, range and
    // heading, so both layers project world positions through identical numbers - see
    // TerrainUnderlay.h. Gated by the "Terrain" layer toggle like every other layer.
    void SetTerrainUnderlay(ITerrainUnderlay *t);
    uint32_t TerrainSegments() const; // last frame's count, for the host's log

    // ---- sharing this view's data with another renderer --------------------------------
    // The scene state machine ticks in Poll(), which runs every frame for every page, so this
    // is valid whether or not the 2D scope is the view on screen. Pointers inside the returned
    // scene are owned by LidarView and stay valid until the next BuildScene call.
    // allRounds: the pilot views (V-POD, Enhanced Imaging, CAP, the vision mode) show every round -
    // ballistic, PPC, missile - whatever the scope's Projectiles / Missiles layers say, and keep the
    // plugin's projectile scan asked for while they are on screen.
    void BuildScene(LidarScene &out, bool allRounds = false);

    // Tell this view that ANOTHER renderer is showing its data. The control channel stops
    // asking the plugin for the expensive projectile scan when the scope stops drawing, so
    // without this the 3D page would quietly lose projectiles and missiles a second after
    // being selected. Call it once per frame while that page is up.
    void NotifyRendered();

    // Pass nullptr before ResizeBuffers and the fresh target afterwards. The scope's
    // device-bound brush is released and recreated to match.
    void SetRenderTarget(ID2D1RenderTarget *rt);

    // Draws the scope into the current render target. The host brackets nothing: this
    // does its own BeginDraw/EndDraw, which on a DXGI surface target does not present.
    void Render();

    // Call every frame regardless of the active view: retries the MW5_LidarExport
    // connection in the background so switching to the scope is instant.
    void Poll();

    // Stop the plugin's projectile scan when the view is switched away - otherwise the
    // plugin keeps paying for a scan nobody is looking at.
    void ReleaseControl();

    void Shutdown();
    bool IsConnected() const;

    // Settings live in HUDDisplay's config and are pushed in here.
    uint32_t GetLayers() const;
    void SetLayers(uint32_t layers);
    uint32_t GetRangeM() const;
    void SetRangeM(uint32_t metres);
    void SetFar(bool on); // the 5 km radar mode (2026-10-15)
    // COMPASS (2026-10-12): ring + heading box, clock positions, rim bearing dots.
    void SetCompass(bool ring, bool clock, bool dots);
    // RADAR MODE (2026-10-12): a host filter for hostile contacts (world cm); false = hide it. Null = show all.
    // Friendly contacts and the selected target are never filtered.
    typedef bool (*ContactKeepFn)(void *ctx, float x, float y, float z);
    void SetContactFilter(ContactKeepFn fn, void *ctx);

    // Fraction of the window the host reserves around the edge for bezel legends. The
    // scope disc and its text overlays shrink to match.
    void SetContentInset(float inset);
    // NAV POINTS (2026-10-07k): drawn on the scope; one past its range sits on the outer ring, with its distance.
    struct NavPoint
    {
        float   X, Y;     // world cm
        uint8_t Mine;     // 1 = marked by you (another colour)
        wchar_t Name[16]; // "NAV ALPHA"
    };
    void SetNavPoints(const NavPoint *pts, int n);
    // DROPSHIPS (2026-10-08): the hull outline seen from above (assets/dropships), clipped to the scope's disc.
    // Kind: TerrainDropshipKind (0 = a box of EX / EY around X / Y). Side: 0 unknown (grey), 1 friendly (blue), 2 enemy (red).
    struct Dropship
    {
        float   X, Y, Yaw, EX, EY;
        uint8_t Kind, Side;
    };
    void SetDropships(const Dropship *d, int n);

    // Interpolate projectile positions between the plugin's ~30 Hz publishes.
    void SetProjectileSmoothing(bool on);

    // Snapshot of the alarm/control state, for the host's log. Multi-instance ownership and
    // the projectile request are invisible from the plugin's log, and HUDDisplay kept no log
    // of its own - which is why a control-arbitration bug took two missions to even see.
    struct Diag {
        bool Connected, OwnsControl, HoldsScope, ProjRequested, Locked, BoundAny, Sounding;
        int  BoundRounds;
    };
    Diag GetDiag() const;

    // Incoming-missile warning volume, 0-100. Applied to the open audio device, so this
    // is safe to call from a slider drag. The alarm itself is a layer toggle (the last
    // entry in the layer table) and is driven from Poll(), so it sounds on every MFD page
    // rather than only while the scope is the active view.
    void SetAlarmVolume(int percent);
    void SetAlarmSound(int sound); // 2026-10-13: ThreatAlarm::SetSound (0 warble, 1 double pip, 2 triple pip, 3 fast warble)
    int  GetAlarmVolume() const;
    bool IsAlarmSounding() const;

  private:
    LidarViewImpl *P = nullptr;
};
