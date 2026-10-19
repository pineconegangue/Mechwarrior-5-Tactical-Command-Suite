> **Moved here from CLAUDE.md on 2026-10-05, unchanged below this note.** This is the deep guide to
> the LIDAR side (LidarExport, HUDDisplay's LIDAR / TOPO views, threading, reflection facts). Read it
> only when working on those. Stale points: paths say `D:\Selfmade\mwvr` - the working copy is
> `D:\Selfmade\mwvr POC`; "read the log via the file tool, not the bash mount" no longer holds
> (the device-shell mount read the live log correctly all through the 2026-09/10 sessions); it
> predates the three-plugin split (VanillaHud / CameraCapture / LanceControl - see CLAUDE.md).

# CLAUDE.md — MW5 LIDAR Scanner (LidarExport + HUDDisplay)

Checkpoint / working memory for the MechWarrior 5: Mercenaries second-screen LIDAR
tactical scanner. Read this before making changes.

## What this project is

A **second-monitor tactical radar** for MW5 that runs as an external app — NOT an
in-game HUD element. Pieces talk through memory-mapped files (MMFs):

1. **LidarExport** — a UEVR (praydog) C++ plugin loaded into the game. It reads live
   game state via UEVR reflection and writes it to the MMF `MW5_LidarExport`. It only
   READS the game; it modifies nothing (no Blueprint mods — deliberately, to avoid
   touching gameplay assets). It *does* detour a couple of Blueprint tick UFunctions,
   but only to get a game-thread callback — it always chains the original (see Threading).
2. **HUDDisplay** — the external MFD app, and **the primary renderer since Aug 2026**.
   It mirrors the MWVR HUD widgets (via `HUD.dll` and the `MechWarriorVR_HUDExport` MMF)
   *and* hosts the LIDAR scope as one selectable view. See "HUDDisplay integration" below.
3. **LidarDisplay** — the original standalone Direct2D scope. **Now a reference copy, not
   the primary renderer.** It still builds and still works, and is deliberately kept
   byte-frozen so the port in `src/hud_display/LidarView.cpp` can be diffed against it.
   Do not modify it without a specific reason.

Built on the author's existing UEVR stack (`pineconegangue/MWVR-Screen-export`,
`MechFFB`) and `sicsix/MW5-UEVR-Plugins` `PluginExtension` framework (MIT).

**Standing instructions:** Never reference vonHUD. Reference `HUD.dll` (src/hud)
patterns before changing pawn/mech detection logic. Reference-first: don't guess
reflected names — dump/verify, then wire.

## File map

- `include/LidarShared.h` — the MMF contract (`LidarExportBlock`, `LidarContact`,
  `LidarProjectile`, enums). **Included by ALL readers** — layout must never drift.
- `src/lidar_export/LidarExport.cpp` — the plugin (writer).
- `src/hud_display/Main.cpp` — the MFD app: widget views, screen capture, the MFD bezel,
  edit mode, profiles. Owns the window, the swap chain and the Direct2D stack.
- `src/hud_display/LidarView.h/.cpp` — the LIDAR scope hosted inside HUDDisplay. A port of
  `src/lidar_display/Main.cpp`; see "HUDDisplay integration".
- `src/lidar_display/Main.cpp` — the original standalone renderer. Frozen reference copy.
- `cmake.toml` — targets `LidarExport` (plugin), `HUDDisplay` and `LidarDisplay`.
- `build_release_lidar.ps1` — builds the plugin only (Release).

Reference material available on disk: the UE4.26 engine source + MW5 mod editor live at
`F:\UE4` (`UE_4.26/Engine/Source/...` has authoritative signatures, e.g. SceneComponent.h,
Actor.h, GameplayStatics.h). `MechFFB` (C# FFB app) is the sibling telemetry consumer.

## Build & run

Build from **Developer PowerShell for VS 2022** (or the VS CMake integration):

```
# Plugin:
& 'D:\Selfmade\mwvr\build_release_lidar.ps1'
# The MFD app (primary renderer; NOT built by the script above):
cmake --build 'D:\Selfmade\mwvr\build' --config Release --target HUDDisplay
# The frozen standalone scope, only if you need it:
cmake --build 'D:\Selfmade\mwvr\build' --config Release --target LidarDisplay
```

`HUDDisplay.exe [profile=<name>] [screen X Y WIDTH HEIGHT]`. The profile names the config
file (`MFDDisplay_<name>.cfg`, beside the exe) and the window title; omit it for `default`.
Several instances run at once, one per screen — give each its own profile.

The user **injects the plugin via UEVR** and runs the renderer themselves. UEVR keeps the
plugin resident and won't hot-reload, so restart/re-inject the game after a rebuild.

**Confirm which build is actually live from the log, never from a DLL mtime.** Two markers:
`BUILD=...` (a stamp printed once at init) and `MMF created, N bytes` (struct size). Current
build: `BUILD=gamethread-dualhook-relpose-proj512-lockpath-pawnswap`, `MMF created, 15180 bytes`.

Runtime truth is the log: `%APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\log.txt`.
**Read it via the file tool on the Windows path, NOT the bash mount** (the mount caches a
stale snapshot — see Diagnostics).

## Threading model (the single most important thing here)

**All enumeration/reflection MUST run on the GAME thread.** `GetAllActorsOfClass` /
`ProcessEvent` walk and marshal the live actor list; doing that on any other thread races
the game thread mutating that list → it reads mid-realloc → `EXCEPTION_ACCESS_VIOLATION`
(often reading a **page-aligned** address inside a `VCRUNTIME140` memcpy, with a recursive
`ProcessEvent`/`UnrealEngineModLoader` stack). This crashed on mission load (actor spawn
churn) and the instant an LRM salvo spawned. Throttles, caches, and cheaper reads could
NOT fix it because none of them changed the *thread*.

How the current build gets onto the game thread (the whole journey lives here so we don't
repeat it):

- **`on_present` is the RENDER thread.** It was the original (wrong) driver.
- **`on_pre_engine_tick` (the SDK game-thread callback) is NOT delivered to this plugin**
  in this UEVR/MWVR setup — its log line never appears, even though HeadAim/Cameras use it.
  Do not rely on it. (Likely because those plugins complete the MWVR VR_Global version
  handshake via `VersionCheckFnName`, which we skip — and MWVR has no VR_Global entry for
  LidarExport anyway, so we can't do that handshake.)
- **The working game-thread driver is a Blueprint UFunction detour** (`AddEventHook`, the
  same mechanism HeadAim/Cameras/MechShakerBridge use). A hooked BP UFunction's detour runs
  on the game thread whenever the game calls it.
- **We hook TWO functions on the mech COCKPIT and use whichever fires:**
  - `ReceiveTick` — the cockpit's OWN Actor Event Tick. Fires every frame while piloting,
    **regardless of head-aim setting** → the head-aim-independent primary.
  - `OnCalculateHeadAim` — fires only when head aim is in an active mode → backup.
- **Hook the COCKPIT, not the pawn.** The candidate dump (`DumpHookCandidates`) was decisive:
  `DerivedMech_C` (the pawn) has NO own `ReceiveTick`, so `find_function` there returns the
  inherited `AActor` stub the mech never calls — it installs yet never FIRES. The cockpit
  (`pawn → MechMeshComponent → FirstPersonCockpitComponent → ChildActor`) has its own
  `ReceiveTick` (it updates heat/damage/HUD every frame) and its own `OnCalculateHeadAim`.
- **Each hook has its OWN detour** (`OnReceiveTickDetour` / `OnHeadAimDetour`) chaining its
  OWN saved original (`ReceiveTickOrig` / `HeadAimOrig`); both call the shared
  `RunGatherOnGameThread`. Always chaining the original is why HeadAim / the game still work.
- **`GatherActive`** (atomic) makes `Gather` non-re-entrant and never two-thread during the
  render→game handover. **`GameThreadTickAlive`** flips the instant a hook FIRES; after that
  `on_present` returns immediately and never enumerates again.
- **`on_present` fallback:** until a hook fires it installs the hooks (cheap raw property
  reads, no logging) and runs `Gather` on the render thread only as a stopgap — out of a
  mech `Gather` no-ops, and once hooks install it holds off for a 2 s grace so the game
  thread takes over cleanly. Net: no sustained render-thread enumeration.
- **`OnCalculateHeadAim` alone was the earlier bug:** it installed but `... FIRED` never
  logged when the user had head aim off → we fell back to the render thread → still crashed.
  Lesson baked into the design: the driver UFunction must be called EVERY frame regardless
  of settings — hence cockpit `ReceiveTick` as primary.
- **HeadAim also hooks `OnCalculateHeadAim`.** Whoever installs last chains the other; we
  always call the chained original so both run. Our `ReceiveTick` hook is independent of
  HeadAim, so even if the head-aim chain ordering is unlucky, `ReceiveTick` still drives us.

**Verify in the log:** `GAME-THREAD hook installed on ReceiveTick (cockpit)` (+ the
OnCalculateHeadAim one) then `GAME-THREAD tick hook FIRED - Gather now runs on the GAME
thread`. All present = enumeration is on the game thread.

**Validated:** heavy mission with **peak 248 projectiles airborne**, `scanned=33` contacts,
7 enemies in range, and a mid-mission transition — zero crash/hang, `idmap` bounded. The
100-projectile scenario that used to crash within seconds on the render thread now holds.

## Architecture / data flow

MMF is guarded by a **seqlock** (writer bumps `FrameCounter`, writes body, sets
`FrameCounterEnd == FrameCounter`; reader retries until they match). It's **mostly one-way**
except a small reverse control channel (below). `Gather()` runs on the game thread (above)
and self-throttles into **three independent cadences** sharing one seqlock publish (the
`GatherActive` guard serializes everything, so the throttle statics are single-threaded):

- **Fast (~16 ms / ~60 Hz):** player position/heading, torso-twist yaw, locked-target ids
  (`SelectedIds`). Bumps `FrameCounter`. Reads ONLY the stable player pawn (cheap). Allies
  are republished every fast tick from `AllyBuf`.
- **Ally sub-throttle (~100 ms / 10 Hz):** allies re-enumerated FRESH via
  `GetAllActorsOfClass` into `AllyBuf`. Throttled because enumeration is the heavy part.
- **Scan (~3 s):** enemies only — the contact list. Bumps a **separate `ScanCounter`**.
- **Projectiles (~30 Hz, tunable):** their own array. Bumps `ProjCounter`. **Opt-in** —
  only runs when the renderer requests it via the control channel.

**Reverse control channel:** the RENDERER writes `ControlLayers` (bitmask;
`LIDAR_CTRL_PROJECTILES`) and `ControlProjIntervalMs` into the block. The plugin reads them
to decide whether/how often to run the projectile scan. Single aligned words → atomic; the
plugin never writes them, so its seqlock publishes leave them intact. The renderer opens the
MMF **read/write** for this.

**Allies vs enemies (deliberately split):**
- **Enemies** ride the 3 s scan → the renderer reveals them with the ping sweep and
  **fades them to nothing over ~1 s** (`kFadeMs`). They pulse in and out.
- **Allies** are enumerated fresh on the 10 Hz sub-throttle into a value buffer and
  republished every fast tick. Always visible, no ping/fade. **No actor pointers cached
  across ticks** — a freed pointer dereffed later crashes the game; re-enumerate every time.

**Renderer:** heading-up circular scope. Ping expands at **500 m/s**, restarts only on
`ScanCounter` change. Persistent enemy tracks keyed by **stable id**. Player transform +
allies + selection refresh at ~60 Hz.

## Enumeration: `GetAllActorsOfClass`

The per-scan enumeration calls **`UGameplayStatics::GetAllActorsOfClass`** via reflection
instead of sweeping the global UObject array (measured: `scanned=` dropped from ~360k to the
handful of live actors of the class). Wired via `ResolveGameplayStatics` + `GetActorsOfClass`:
- Resolve once: `find_uobject<UClass>("Class /Script/Engine.GameplayStatics")` →
  `get_class_default_object()` (CDO) + `find_function("GetAllActorsOfClass")`.
- Call: `cdo->process_event(fn, &params)` with
  `struct Params { void* WorldContext; void* ActorClass; FScriptArray Out; }` (32 bytes).
  WorldContext = the **player pawn**; ActorClass = a base handle
  (`MWMech`/`MWTank`/`MWTurret`/`MWVTOL`/`MWProjectile`); `Out` is filled.
- Contacts/allies call it 4× (one per base) and **copy the returned pointers out immediately**
  — `GaacBuf` (the reused `FScriptArray { void* Data; int32 Num; int32 Max; }`) is clobbered
  on the next call. Projectiles call it once and iterate directly.
- Class bases are resolved from a live instance by walking the super chain
  (`ResolveVehicleClasses` gated on `is_a(PawnClass)`, `ResolveProjectileClasses` gated on
  `is_a(ActorClass)`) — bounded, self-terminating, retried until resolved then it stops.
  `PawnClass`/`ActorClass` come from the mech's super-chain on pawn change.

## Reflection facts (hard-won — DO NOT re-guess)

Player mech chain: `DerivedMech_C -> AbstractMech_C -> MWMech -> Character -> Pawn -> Actor`.

| Need | Real reflected member | Notes |
|------|----------------------|-------|
| Am I in a mech? | property `MechViewComponent` on the pawn | HUD.dll's OnNewPawn pattern; robust vs transient DefaultPawn/SpectatorPawn |
| Mech cockpit (game-thread hook target) | `MechMeshComponent` → `FirstPersonCockpitComponent` → `ChildActor` | has its own `ReceiveTick` + `OnCalculateHeadAim` (HeadAim uses the same chain) |
| Enumerate mechs / vehicles | bases `MWMech`, `MWTank`, `MWTurret`, `MWVTOL` | all `Pawn`-derived, all carry `TeamID`; feed to GetAllActorsOfClass |
| Team (friend/foe) | `MWMech.TeamID` — **StructProperty** | read the first BYTE, compare raw; player≈0, hostile≈255 |
| Mech dead? | `bIsPilotDead`, `bCenterTorsoDestroyed` (BoolProperty) | via masked `ReadBoolProp` |
| Vehicle dead? | no health/dead bool; only `Actor.bActorIsBeingDestroyed` + `MWTurret.PowerState` | best-effort; lingers briefly |
| Weight class | `MWMech.ComputedWeightClass` — **EnumProperty** | 0=Light 1=Medium 2=Heavy 3=Assault |
| Torso twist | component `TorsoTwistComponent`, property `TorsoTwist` (vec2) | `.X` = yaw degrees |
| Locked target | component `TargetTracking`, ObjectProperty target slots | keep names with "target"; drop previous/pending/override/last |
| Actor world pose (CHEAP, preferred) | `RootComponent` → `RelativeLocation` (FVector) + `RelativeRotation` (FRotator) | plain memory reads, **no ProcessEvent** (`ReadActorPose`). For an unattached root — mechs/vehicles/projectiles — relative == world. `ComponentToWorld` is NOT a UPROPERTY in 4.26 |
| Actor location/rotation (avoid in bulk) | `call_function(K2_GetActorLocation / K2_GetActorRotation)` | works, but each is a **ProcessEvent**; only the single player pawn still uses it |
| Projectiles (traveling) | `MWProjectile` subclasses (`Projectile_AC10_C`, `PPCProjectile_C`, `Projectile_Missile_*`, `*Arrow*`) | classify by subclass NAME |
| Is a guided salvo locked on ME? | enumerate `MWMissilePath` (`MissileClass` base) → `GetLockedTarget()` (→ `MWAIAimLocationComponent`) → `GetOwner()` == `get_local_pawn(0)` | **primary**, exact, no guessing; homing-only (dumbfire `SRMSpreadPath` has null lock), you-specific, every salvo, no cooldown. **Arrow IV does NOT work this way — see the Arrow IV dead ends.** See feature notes |
| Am I being shot at (REMOVED) | mech `BitchinBettyComponent` → hook `OnIncomingMissile_Event_0` (+ `StartPlayingBBEvent`, arg `161`) | **Gone — do not reinstate.** Throttled by a ~13 s voice-precedence timer, so it structurally cannot warn on every salvo. Kept here only so the hook facts aren't rediscovered: `HandleIncomingMissile`/`AddEvent` are direct calls and never fire when hooked; `OnIncomingMissile_Event` compiles to `..._Event_0` |
| Enumerate cheaply | `UGameplayStatics.GetAllActorsOfClass` (Engine module) | excludes CDOs; needs a world-context UObject |

Do NOT reintroduce: `GetTeamId`, `IsDead`, `GetAttitudeToPlayer` (don't exist / no-op).

## Projectiles & missiles

- Enumerate `MWProjectile` only. Classify by lowercased subclass name:
  `ppc`/`particle` → PPC (blue); `arrow` → Arrow IV (red, **3× size**); `missile`/`srm`/
  `lrm`/`rocket`/`narc`/`streak` → Missile (red); else → Ballistic (brown).
- **`MWMissilePath` actors are FIXED-POINT path descriptors**, NOT the moving munition, so
  never use their position for rendering or as a missile position. The real moving munitions
  are the `Projectile_Missile_*` / `*Arrow*` actors under `MWProjectile`.
  - **That position IS THE LAUNCHER, and it is measured, not assumed.** Field logging of
    round-to-path distance at the moment a round first appears gives **15-28 m** for a correct
    match against **1100-1350 m** for a wrong one. This is what the attribution tiebreaker
    runs on (see feature notes). An earlier version of this file said "impact / launch point"
    in one sentence and "reads the launcher" in the next; it is the launcher.
  - **ONE PATH PER SALVO — except in chain-fire.** A mech firing in chain-fire / semi-auto
    fires one missile per salvo, and the property is unchanged by the mod, so it registers as
    one path per MISSILE. `pathN` is therefore NOT a reliable salvo count: `pathN=64
    locked=64 lockMe=64` can be one chain-firing LRM boat, not 64 attackers. This cost two
    reversed conclusions in one session — do not re-derive it from path counts.
- Spent rounds: skip `bActorIsBeingDestroyed` || `bHidden` (pooled actors linger, so `enum`
  in the log is always ≥ `written`).
- **Hot path — hundreds of rounds can be airborne (peak ~250 observed) and they linger after
  impact.** Everything per-round is kept cheap:
  1. **Classification cached per class** (`ProjTypeCache`) so the allocating
     `get_fname()->to_string()` runs once per weapon type, not per round — a per-round wstring
     alloc froze the game via heap-lock contention.
  2. **Spent-round bools resolved once** as `FBoolProperty` off `ActorClass`
     (`PropBeingDestroyed`/`PropHidden`), read with `get_value_from_object` — no per-round
     property-chain walk.
  3. **Projectile ids are ASSIGNED** (`ProjIdMap`, actor pointer → id), pruned to the live set
     every scan so the map is bounded by rounds in the air. They used to be a pointer hash,
     which is not an identity: expected collisions among n live rounds are n²/2m, so at the
     ~500 seen in a large battle roughly two unrelated missiles always shared an id. Every
     renderer structure keyed on the id then merged two missiles — trails zig-zagged between
     two flight paths, and a shared `ProjStill` counter marked a LIVE round landed whenever
     its collision partner sat still, which is what made rounds blink and teleport.
     - A freed id is held in **quarantine** (`kIdQuarantineMs`, 1500 ms) before reuse. See the
       cross-binary invariant with `LidarView::kTrailRetainMs` in the invariants list.
     - Mechs/vehicles/allies still use `IdMap` via `AssignId()`, **bounded at 8192**.
  4. **Position via `RootComponent → RelativeLocation`** (`ReadActorPose`), NOT
     `K2_GetActorLocation` (a ProcessEvent). At hundreds of rounds × ~30 Hz the ProcessEvents
     were thousands/sec of hooked dispatches → FPS drop then freeze during LRM40 salvos.
- Renderer: small dots (~1.1 px; Arrow IV 3.3). Missiles get a fading **gray smoke trail**;
  ballistic/PPC get a short colored streak. All disappear on impact. Separate
  `Projectiles` / `Missiles` view layers. Cap is `LIDAR_MAX_PROJECTILES` (512).
- **Landed-round cleanup (renderer-only, `DeadProj`):** a pooled projectile actor lingers
  after impact (still enumerated in-game, not yet despawned) and would keep reading as
  "active" — including holding the incoming alarm on. Each publish the renderer compares every
  round's **3D** position to the previous one; if it moved `< kStillEpsCm` (1 m) for `kDeadFrames`
  (3) publishes in a row it's marked landed and dropped from **rendering, its smoke trail, and the
  incoming/alarm set**. Purely display-side — the game keeps its actor, and if the round ever
  moves again the counter resets and it reappears, so no live round is ever hidden. **3D matters:**
  a vertically-dropping Arrow IV has near-zero XY motion, so an XY-only test would flag it as
  landed mid-air; `MissileTrailZ` (parallel altitude history) also makes the closing test 3D.

## View-layer framework (renderer)

`enum ViewLayer` + `kLayers[]` table (name, default key, implemented) + `EnabledLayers`
bitmask. Data-producing layers (Projectiles/Missiles/IncomingWarn) set
`LIDAR_CTRL_PROJECTILES` so the plugin only scans when they're on.

- **In HUDDisplay (current):** the layer checkboxes, alarm-volume
  stepper and scope range live in HUDDisplay's **edit-mode panel** (Numpad `.`), and persist
  to `MFDDisplay_<profile>.cfg` beside the exe. The ported `LidarView` has no menu, no click
  hit-testing and no config of its own — layers, interval, volume and range are host-set
  fields pushed in through `LidarView::Set*`.
- **In the frozen standalone:** right-click opens its own options menu and it persists to
  `%APPDATA%\...\lidar_view.cfg`. The two no longer share settings, which is fine because
  they are not meant to run at the same time (see below).

## Fork provenance & the HUD texture export (the ONLY invasive change to upstream)

Upstream is **sicsix/MW5-UEVR-Plugins**. The oldest local snapshot
(`Selfmade/mwvr backups/mwvr stable cmake backup...`, 2026-02-03) is **byte-identical to
upstream tag 2.30** (`63bf49b`, 2026-01-30) across all 66 source files — a clean fork point.

**Everything we have added is additive** — `src/hud_display`, `src/lidar_export`,
`src/lidar_display`, `include/LidarShared.h` — EXCEPT six files. Verified by hash against 2.30:

| Upstream-owned file | 2.30 | ours | what we did |
|---|---|---|---|
| `src/hud/Renderer.cpp` | 575 | 745 | the export itself |
| `src/hud/Renderer.h` | 170 | 222 | export decls + shared-texture state |
| `src/hud/HUD.cpp` | 359 | 562 | MFD screen discovery, export MMF setup |
| `src/hud/HUD.h` | 79 | 83 | screen ComponentIds, MMF handles |
| `src/hud/WidgetRenderData.h` | 33 | 44 | six new `WidgetType` entries |
| `include/Log.h` | 20 | 45 | file logging (unrelated to export) |

`Cameras.cpp`, `cameras/Offsets.h`, `HeadAim.cpp`, `MechShakerBridge.cpp`, `hud/Offsets.h`,
`PluginExtension.h`, `MW5.h`, `RE.h`, `Util.h` and all 51 `include/mw5/` headers are PRISTINE.
Do not "tidy" them — an untouched file is a file that merges for free.

**Most of the apparent diff is clang-format reflow** (Allman braces, alignment stripped). The
real modification is ~150 lines of substance.

### What it does

1. **Six MFD widget types** added to `WidgetType`: `TargetScreen`, `WeaponsScreen`,
   `OwnerScreen`, `ObjectivesScreen`, `MapScreen`, `CommsScreen`. Upstream only knew crosshairs.
2. **Screen discovery** (`HUD.cpp`): on mech entry, capture each cockpit screen's
   `FPrimitiveComponentId` (plus `TargetBoxId`); later, match an incoming `componentId`
   against them to tag which widget a render belongs to. Logged as
   `Captured TargetScreen ComponentId: N`.
3. **The export** (`Renderer.cpp`), at the END of the widget render pass:
   - `EnsureSharedTextureForWidget` — lazily creates a per-widget
     `DXGI_FORMAT_B8G8R8A8_UNORM_SRGB` texture with
     `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`, recreated only on a size change, and takes a
     handle via `IDXGIResource::GetSharedHandle`.
   - `CopyToSharedTextureForWidget` — `AcquireSync(0, 0)` → `CopyResource` → `ReleaseSync(1)`.
     The **non-blocking acquire with a `WAIT_TIMEOUT` early-out is deliberate**: the game frame
     must never stall waiting for the consumer.
   - The export block publishes `Active` / `Width` / `Height` / `Type` / shared handle per slot
     into the MMF control block, and CLEARS `Active` + `SharedTextureHandle` for inactive slots.
4. **File logging** in `Log.h` — independent of the export; would not need porting.

`HUDDisplay.exe` opens those shared handles. It knows nothing about how they were produced.

### Two hazards if this is ever re-implemented

1. **`eye == 0` gates the export to once per frame.** It depends on the widget render pass
   still being invoked per-eye with an eye index. If a refactor changes that signature, the
   export silently runs twice or never — no error either way.
2. **The `10` widget slots and the `ExportControl->Widgets[i]` layout are a CONTRACT with
   `HUDDisplay.exe`,** and unlike the LIDAR block there is no `static_assert` guarding it.
   Change one side and you must rebuild both. Adding a size assert is a cheap win.

### Merging upstream

Between 2.30 and 2.52 (`2214b0e`, 2026-03-31) upstream changed only `PluginExtension.h`,
`Cameras.cpp`, `cameras/Offsets.h` and `HeadAim.cpp`. **`src/hud` was untouched for fourteen
months.** Our changes and theirs have ZERO overlap, so moving to 2.52 is close to mechanical.
A future single-DLL refactor is a structural problem (files moving, plugin lifecycle), not a
merge-conflict problem.

## HUDDisplay integration (Aug 2026)

The scope now lives inside HUDDisplay as one selectable view. `src/lidar_display/Main.cpp`
was **not** modified — `LidarView.cpp` is a copy of it. Keep it that way: the frozen original
is what makes the port diffable.

**Why it needed a port at all.** HUDDisplay is D3D11 (swap chain + textured quad on its HWND);
`ID2D1HwndRenderTarget` creates its own hidden D3D device and swap chain for the same HWND.
Two swap chains on one window have no arbitration → tearing, no z-order, ambiguous `WM_SIZE`.
The fix is **Direct2D 1.1**: `ID2D1Factory1` → `CreateDevice(IDXGIDevice)` → `CreateDeviceContext`
→ `CreateBitmapFromDxgiSurface(back buffer)`. D2D 1.0's `CreateDxgiSurfaceRenderTarget`
**cannot** target a D3D11 surface (it needs a D3D10.1 device) — don't try it. Requires
`D3D11_CREATE_DEVICE_BGRA_SUPPORT` on the device; the swap chain was already
`DXGI_FORMAT_B8G8R8A8_UNORM`, which the D2D bitmap accepts. `ID2D1DeviceContext` derives from
`ID2D1RenderTarget`, so the copied drawing code needed no changes.

**What changed in the copy:** `ID2D1HwndRenderTarget*` → `ID2D1RenderTarget*`;
`Init`/`Run`/`WndProc`/`WinMain` removed (the host owns those); the options menu, click
hit-testing, number-key toggles and `lidar_view.cfg` removed; `RangeCm` and `Inset` became
mutable members. Everything else — MMF reader, seqlock, tracks/fade, projectiles, smoke
trails, landed-round cleanup, incoming detection, the synthesized alarms — is verbatim.

### Invariants — breaking one reintroduces a verified bug

1. **`RT` is BORROWED.** `LidarView::SetRenderTarget` never AddRefs. The
   `D2DERR_RECREATE_TARGET` branch nulls the pointer, it must never `Release` it.
2. **`WM_SIZE` must unbind from the immediate context, not just drop ComPtrs.**
   `ResizeBuffers` fails while any back-buffer reference is outstanding, and the D3D11
   immediate context holds its own reference to bound views. Required order:
   `ReleaseD2DTarget()` → `backBufferRTV.Reset()` → `OMSetRenderTargets(0,...)` +
   `PSSetShaderResources(0,1,null)` → `Flush()` → `ResizeBuffers` (check the HRESULT).
   Getting this wrong makes the resize fail *silently* — the swap chain keeps its old size
   while the bezel is hit-tested against the new client rect.
3. **Stop the alarm before rebuilding its buffers.** `PlaySound(SND_MEMORY|SND_LOOP)` leaves
   winmm reading the WAV vectors on its own thread; `BuildAlarms()` clears and refills them.
   `SetAlarmVolume` must `SetAlarm(0)` first.
4. **`SyncHotkeys()` is the only hotkey registration path.** Hotkeys are live iff
   `!capturingKey && !editingText`. Never hand-pair Register/Unregister around a mode
   transition. `WM_KILLFOCUS` → `CancelTransientModes()`, or Alt+Tab mid-rebind leaves every
   global hotkey unregistered for the session.
5. **`EndDrags()` is the only drag teardown**, and `WM_CAPTURECHANGED` must call it with
   `releaseCapture = false`.
6. **Numpad `.` (`kEditModeKey`) is reserved** and refused by the rebind capture — binding it
   makes `WM_HOTKEY` swallow the keystroke and locks edit mode away permanently.
7. **`pendingWidgetType` is a widget TYPE; `currentWidget` is a table INDEX.** They are not
   interchangeable; they only ever coincided because `HUD.dll` happens to place each widget at
   the index matching its type.
8. **Nothing draws outside the scope's outer ring.** `DrawContact` returns `bool`; projectiles
   and trail segments are filtered by `inRange`. The display range is independent of the
   plugin's scan range, so this cannot be assumed away.
9. **Only one instance drives the LIDAR control channel and the alarm.** Named mutex
   `Local\MW5_LidarControlOwner`, claimed inside `Render()` so ownership follows actually
   drawing the scope. Non-owners render normally but stay silent and show `[MUTED]`.
   **This does NOT cover the standalone `LidarDisplay.exe`** — it knows nothing of the mutex,
   so running it alongside a HUDDisplay showing the scope brings back the conflict.
10. **`kIdQuarantineMs` (plugin) must stay LONGER than `kTrailRetainMs` (renderer).** The
    plugin recycles projectile ids; the renderer keeps a round's smoke trail for 1000 ms after
    it vanishes so a round bumped out of a truncated array doesn't lose it. Reuse an id sooner
    than that and a fresh missile inherits a dead one's trail — drawn with a smoke line
    leading back to where it never flew. Two constants in two binaries whose ORDERING is
    load-bearing and which nothing checks. If the trail retention ever grows, grow the
    quarantine first.
11. **Flagged rounds must survive array truncation.** `LIDAR_MAX_PROJECTILES` is 512 and
    `peak=507 written=491` has been logged, so the cap is reachable. Write order is whatever
    `GetAllActorsOfClass` returns, so a round guiding on you could be dropped while unrelated
    ballistic traffic took its slot — the renderer only sees flags on rounds that made it into
    the array, so the alarm would go quiet mid-salvo with nothing in the log. A flagged round
    now evicts an unflagged one (`evict=` in `proj:`).
12. **Scan range must stay 1000 m** unless the standalone is fixed in the same pass. The
    standalone's `kRangeCm` is 100000 and it has **no range clipping**; raising the plugin's
    range makes it draw contacts outside its own ring. (Tried at 1200 m, reverted.)

### Build standard
`src/hud_display/*.cpp` compile with **zero warnings** under `-Wall -Wextra`. A quick
non-Windows syntax check that catches most errors before a real build:

```
x86_64-w64-mingw32-g++ -fsyntax-only -std=c++20 -Wall -Wextra \
    -D__STDC_WANT_SECURE_LIB__=1 -Iinclude -Isrc/hud_display \
    src/hud_display/Main.cpp src/hud_display/LidarView.cpp
grep -n "begin(), .*end()" src/hud_display/*.cpp
```

The grep is not optional: `std::string(wide.begin(), wide.end())` narrows `wchar_t` to `char`
and MSVC raises C4244, but **gcc stays silent even with `-Wconversion`** because the conversion
happens inside a libstdc++ template. Use the `NarrowLabel()`/`WidenLabel()` helpers instead.

## Pitfalls & the rules that avoid them

1. **All enumeration/reflection runs on the GAME thread.** See the Threading section — this
   is the root cause behind every page-aligned `EXCEPTION_ACCESS_VIOLATION` with a recursive
   `ProcessEvent`/ModLoader stack. Never move `Gather` back onto `on_present`.
2. **Don't guess UFUNCTION names.** `call_function` silently no-ops on a bad name; dump and
   verify (`DumpHookCandidates` / `get_property_data`). `GetTeamId`/`IsDead` cost us real bugs.
3. **Never cache actor pointers across ticks and deref them later.** Freed/reused memory →
   garbage vtable → hard crash. Re-enumerate via `GetAllActorsOfClass` (live actors only)
   each time. `IdMap` stores pointers ONLY for hashing (never dereferenced), so it's safe.
4. **Native bools are packed bitfields — read with the mask** (`ReadBoolProp` /
   `FBoolProperty::get_value_from_object`). A raw byte read of `bActorIsBeingDestroyed` once
   filtered out **every** contact.
5. **Prefer `RootComponent → RelativeLocation/RelativeRotation` over `K2_GetActor*`.** The
   latter are ProcessEvents; in bulk (contacts + allies + projectiles on the same game-thread
   scan frame) that burst hitched the game once per ping. `ReadActorPose` reads memory instead.
6. **Any `LidarShared.h` layout change = rebuild BOTH binaries.** A stale reader misreads the
   whole block. Verify via `MMF created, N bytes` matching in both. (15180 bytes currently.)
7. **Confirm the live build from the log, not the DLL mtime** — UEVR won't hot-reload;
   re-inject after a rebuild. `BUILD=...` stamp + `MMF created, N bytes` are the ground truth.
8. **`GetAllActorsOfClass` needs a world-context UObject** (pass the player pawn) and
   **excludes CDOs** — its count is the true live-actor count.
9. **`GaacBuf` is reused every call** — copy returned pointers out before the next
   `GetActorsOfClass` call, or they'll be clobbered.
10. **`ULevel.Actors` is NOT reflected** here, and `UWorld.Levels` reads 0 — you cannot
    iterate the level actor list by reflection. `GetAllActorsOfClass` is the working path;
    `get_objects_by_class`/UObjectHook is empty.
11. **`find_uobject("Class /Script/MechWarrior.MWMech")` returns null** for game classes —
    resolve from a live instance (walk super chain). Engine classes
    (`Engine.GameplayStatics`) DO resolve by path.
12. **Keep the seqlock write window tiny** — gather into locals, publish with `memcpy`.
13. **Pawn detection follows HUD.dll** (OnNewPawn + MechViewComponent); re-derive classes
    only on pawn change. **Re-arm the game-thread driver on pawn change too** (`DriverPawn`
    check in `on_present`): when you die and possess a lancemate, the cockpit `ReceiveTick` we
    hooked belongs to the DEAD mech. If the new chassis has a different cockpit class the old
    hook never fires, and `GameThreadTickAlive` stayed latched → the render fallback was off too
    → total freeze. On pawn change we clear `GameThreadTickAlive`/`BpTickInstalled`;
    `TryInstallGameThreadHook` compares the new cockpit class to `HookedCockpitClass` and
    **only re-hooks when it differs** (re-hooking the SAME UFunction would capture our own detour
    as the "original" → infinite recursion). MWMissilePath detection is pawn-agnostic already
    (reads `get_local_pawn(0)` fresh), so once the driver re-arms everything recovers.
14. **Throttle enumeration even on the game thread.** Moving to the game thread fixed the
    crashes, but bunching heavy enumeration on one frame still causes a visible frame hitch.
    Keep the cheap player-only transform on the 16 ms fast tick; keep enumeration on slow
    sub-throttles (allies 10 Hz, contacts 3 s, projectiles ~30 Hz) and use `ReadActorPose`.
15. **Underground phantoms:** drop contacts > **300 m below** the player (`kUndergroundCm`);
    pooled/not-yet-spawned actors sit far below the map.
16. **`MWMissilePath` ≠ the missile** (see Projectiles): it's a fixed-point path actor. Its
    position IS the launcher, which is what attribution matches rounds against — and there is
    one path per SALVO, except under chain-fire where it looks per-missile.

## Current feature state (working)

- Detects player mech; enumerates mechs + tanks/turrets/VTOLs via `GetAllActorsOfClass`,
  all on the game thread (cockpit tick hook).
- Friend/foe by `TeamID` byte. Dead filtering. 1000 m top-down circular scope.
- Torso-twist aim line. Hollow orientation triangles (mech 0.8, vehicle 0.39) + weight
  dots (light 0 / medium 1 / heavy 2 / assault 3).
- **Allies:** fast-path (~60 Hz republish, 10 Hz re-enum), always shown, green.
- **Enemies:** 3 s ping reveal, fade to nothing over ~1 s.
- Locked-target highlight (either team), fast-updated via `SelectedIds`.
- **Projectiles/missiles:** ballistic (brown), PPC (blue), missiles (red) + gray smoke
  trails, Arrow IV (red, 3×). Opt-in via layers; tunable sample rate; vanish on impact.
- **Incoming-missile warning** (`VL_IncomingWarn`, on by default): RWR-style synthesized
  alarm (in-memory WAV, `PlaySound` loop, **menu volume slider**) + a flashing red "INCOMING
  MISSILE" text bottom-right. **Universal — works on every mech, no AMS needed.** Fires for
  **guided missiles locked on YOU** (LRM, Streak/SSRM, Arrow IV), every salvo, continuously;
  **silent on dumbfire SRMs and on guided salvos locked on an ally.**

  **ARCHITECTURE — the principle every bug in this feature violated:**
  **the plugin owns what exists and what it is; the renderer owns how it looks and sounds.**
  Attribution ("is this round guiding on me?") is decided in `LidarExport` and published
  per round as `LPROJ_INCOMING`. The renderer never reconstructs it. Earlier versions had the
  renderer bind rounds to salvos itself from timing and the block-level lock flag; every
  iteration of that leaked, because `LidarProjectile.Id` is a 16-bit key and the renderer
  cannot see actor pointers. If you find yourself re-deriving attribution in `LidarView`,
  stop.

  **Detection chain, plugin side:**
  1. **A launch is the TRANSITION into locked-on-us**, per path actor (`PathSeen`,
     `CountedOnMe` latched), NOT the path first appearing. `GetLockedTarget` can resolve to
     us a scan or two after the path spawns; testing newness before reading the lock
     permanently disqualified those paths. Symptom when this broke: `lockMe=1` sustained with
     `newOnMe=0` and `mine=0` for a whole session.
  2. **`PendingLaunches` is a live view**, rebuilt every probe from the paths enumerated now,
     each carrying its position and whether its lock resolves to us *at that moment*. Not a
     timer: a launch stops being a candidate when its path goes or drops its lock. Paths
     aimed at OTHERS are recorded too — without something to be nearer to, every round
     matches us by default.
  3. **Spatial tiebreaker.** A newly spawned round is matched to the nearest pending launch
     in 3D and taken only if that launch is on us. A round is on its launcher when it first
     appears, so this discriminates between launcher positions, not trajectories. Bound is
     `kLaunchMatchCm` (**60 m**), sized from measured data (correct 15-28 m, wrong 1100 m+).
     **Unmatched rounds are REJECTED**, never waved through — falling back to a permissive
     rule re-imports exactly what the geometry exists to remove.
  4. **Ambiguity rule.** If the nearest on-us launch AND the nearest on-someone-else launch
     are both inside the radius (one mech with an old salvo on us and a new one on an ally),
     do not guess — reject. Silence beats being right half the time.
  5. **Own rounds excluded** via projectile `Instigator` == player pawn (read through
     `SafeObjectProp`, never a raw slot). Firing back while locked used to flag your own
     outgoing salvo as inbound.
  6. **Retirement**, two positive signals, no timers: the actor left the live set, or it has
     been stationary for `kMineStillScans` (3) scans within `kMineStillCm` (1 m). The engine's
     spent flags alone retire far too late — pooled rounds linger post-impact with neither
     set — which is why `mine` once ratcheted to 47 and held.

  **Renderer side** reads `LPROJ_INCOMING` and nothing else: alarm sounds while any flagged
  round is alive, nearest flagged round sets the cadence, flagged rounds flash amber at 4x
  radius in phase with the tone. **Read the flag off `pj.Flags` directly — never mirror it
  into an id-keyed set.** Doing that laundered exact per-round truth through a colliding
  16-bit id and made unrelated rounds flash.

  **Alarm cadence: exactly TWO states**, by range of the nearest flagged round against
  `ThreatAlarm::kCloseCm` (**250 m**) — 180 ms pulse inside, 560 ms outside, switching
  instantly both ways. No hysteresis band (a closing round's range is monotone, so it crosses
  once), no salvo-count table (jittery and not actionable), one pitch. A second salvo
  launching mid-episode changes neither cadence nor phase. `nearestCm < 0` means "no
  measurement" and HOLDS the current state rather than stepping down.

  **Diagnostic counters** (`threat:` / `proj:`): `totLaunch`/`totAttr` are cumulative — the
  instantaneous fields are sampled on a 2 s throttle over a 30 Hz probe and read 0 on ~97% of
  samples, which once made a build that attributed NOTHING look identical to a working one.
  `fb` = rounds rejected for having no credible origin, `amb` = rejected as ambiguous,
  `poseFail=onMe/all` = paths with no readable position (a silent way to lose a salvo),
  `drop`/`evict` = array-cap pressure.

  **ARROW IV — NO DETECTION. Four routes are dead; do not retry:**
  1. `MWMissilePath` — arrows create none the probe can use (`arrow>0` observed with
     `pathN=0`). This CONTRADICTS an older claim in this file that "Arrow IV locks the mech
     too (validated)". The field evidence is newer, but it was gathered before paths were
     properly understood and was never re-tested, so treat both as unproven if it matters.
  2. `ProjectileMovementComponent` — `bIsHomingProjectile` reads 0 and `HomingTargetComponent`
     is an unset `TWeakObjectPtr` (index -1) on 79/79 sampled rounds, despite the round
     visibly re-aiming at a walking mech.
  3. `TargetSharingActor` on the round — null on every round sampled.
  4. The pawn's own aim-location component — every name-matched property is the wrong type or
     null. Only the pawn class's OWN properties were walked, so this is "not found at this
     level", not proven absent.
  A fifth was left unexplored: projectile `Instigator` resolves to the firing mech, which does
  expose `TargetTracking`. It answers "that mech is targeting you", not "that shell is guiding
  on you" — a mech that fired at an ally then turned would read as a false positive.
  Arrows are still classified and drawn (`LPT_ArrowIV`, red, 3x); they simply never produce a
  lock, so they never warn. Same as a dumbfire SRM.

  **Other dead ends (do not retry):** the missile itself carries NO reflected target ref
  (`ImpelComponent` is impulse-only). Trajectory / "closing" inference gave false positives —
  note the spatial tiebreaker above is NOT this: it compares LAUNCHER positions at spawn, not
  trajectories over time. There is no reflectable per-missile `isHoming` field
  (`MWProjectile.bIsMissile` is true for dumbfire too, and the class hierarchies don't
  separate — Arrow IV shares `Projectile_Missile_C` with the SRM).
  `AMSComponent.MissileDetectionActor.GetNumberOfMissilesInRadius()` works without AMS only
  after `EnableMissileDetection()` + `SetDetectionRadius()`, but counts EVERY missile in the
  sphere including dumbfire SRMs → wrong discriminator, don't arm from it.
- **Options** live in HUDDisplay's edit-mode panel (Numpad `.`): per-layer checkboxes,
  alarm volume, scope range. Persisted per profile. (The projectile-rate stepper was
  removed — see Key constants. `lidar_projms` is still parsed and ignored so older configs
  load cleanly.)
- **Scope zoom:** 100–1000 m in 100 m steps (`LidarView::kMinRangeM`/`kMaxRangeM`), from the
  `ZM+`/`ZM-` bezel buttons (numpad `+`/`-`) or the panel. Renderer-side only.

## Key constants

- `kRangeCm = 100000` (1000 m). `kUndergroundCm = 30000` (300 m below).
- Fast throttle `16 ms` (~60 Hz, player-pawn reads only). Ally re-enumeration sub-throttle
  `100 ms` (~10 Hz). Renderer loop `Sleep(16)` (~60 fps). Scan `3000 ms`. Projectiles
  **fixed at 33 ms / 30 Hz** (`LidarView::kProjIntervalMs`) — the user-facing stepper was
  REMOVED. Both landed tests measure "moved < 1 m since the last sample", a distance per
  sample, so changing the rate silently changes what counts as landed and can retire live
  rounds mid-flight. Do not make it adjustable again without converting both tests to a
  speed threshold. Ping
  `kPulseCmPerSec = 50000` (500 m/s). Enemy `kFadeMs = 1000`.
- Incoming alarm — **renderer**: `ThreatAlarm::kCloseCm = 25000` (250 m, the ONLY cadence
  input), pulse 180 ms near / 560 ms far, `kReleaseMs = 100`, `kTrailRetainMs = 1000`,
  `kSalvoFlashScale = 4`. Landed cleanup: `kDeadFrames = 3` publishes, `kStillEpsCm = 100`.
- Incoming alarm — **plugin**: `kLaunchMatchCm = 6000` (60 m match bound, measured),
  `kAmbiguousMatchCm = 6000`, `kMineStillCm = 100` / `kMineStillScans = 3` (landed),
  `kPathForgetProbes = 8`, `kIdQuarantineMs = 1500`, `kProjGapMs = 250`.
- **Don't raise these rates blindly** — enumeration is heavy; even on the game thread,
  bunching it per frame hitches (pitfall #14).
- Caps: `LIDAR_MAX_CONTACTS 256`, `LIDAR_MAX_PROJECTILES 512`, `LIDAR_MAX_ALLIES 32`.
  Raising `LIDAR_MAX_PROJECTILES` is cheap now that per-round reads are `RelativeLocation`
  (memory, no ProcessEvent) — each slot is 16 bytes — but it changes the MMF layout, so
  **rebuild BOTH binaries** (pitfall #6) and confirm via `MMF created, N bytes`.

## Known follow-ups / best-effort

- **Single-driver cleanup:** once a run with **head aim OFF** confirms cockpit `ReceiveTick`
  is the hook that FIREs (both detours currently log the same line), `OnCalculateHeadAim`
  can be dropped for a single clean driver.
- **Vehicle death** best-effort (`bActorIsBeingDestroyed`/`bHidden`); no clean per-frame
  destroyed flag on tanks/turrets, so a wreck can linger briefly.
- **Turret orientation** shows base facing, not the barrel.
- **ECM detection** — hypothesized. `DerivedMech_C` exposes `bStealthState`,
  `DisableStealthTimer`, `sensorStealthDisableSensorsECM`; need to confirm which means
  "active ECM" (vs stealth armor) and find a signal covering all enemy subclasses. The mech
  `AwarenessComponent` is the dream cheap contact source but didn't yield a usable list at
  runtime; worth another look via the editor/a dump.

## Diagnostics quick reference (grep the log)

**Read the LIVE `log.txt`, never a cached snapshot.** Whichever tool you use, confirm the
mtime is current and that the `BUILD=` line matches the build you think you are testing —
reading a stale copy has produced wrong diagnoses more than once. The rest of this note
describes one such cache; the specific mechanism varies by environment, the discipline does
not.
The bash mount serves a **stale, cached snapshot** — its mtime and contents freeze early in
the session (e.g. shows an old timestamp / fixed byte size while the game keeps writing), so
`grep`/`tail` there reports an old truncated log and makes a working build look broken. The
file `Read` tool hits the path directly and sees the **live** file. UE writes the log
promptly (it is NOT only flushed on exit — that was a red herring; the staleness was purely
the bash-mount cache), and a crash flushes a partial buffer (hence null-byte tails on crash
logs). To see current state, `Read` the Windows path directly.

What the log shows (most one-time dumps were stripped once reflection was pinned down):
- `BUILD=...` — build stamp, printed once at init. **Grep this FIRST** when a fix "didn't
  work" — it's the only reliable confirmation of which DLL is injected.
- `MMF created, N bytes` — struct size / layout check (must match the renderer's).
- `GAME-THREAD hook installed on ReceiveTick (cockpit)` / `... OnCalculateHeadAim (cockpit)`
  then `GAME-THREAD tick hook FIRED` — the game-thread driver is live (Threading section).
  If `installed` prints but `FIRED` never does, that hook's UFunction isn't being called;
  `hook-candidate [pawn]/[cockpit]:` lines (dumped once on mech entry) name other options.
- `In mech: class=... enumerating base=...` — pawn/mech detection.
- `resolved MWTank/MWTurret/MWVTOL/MWProjectile/MWMissilePath base` — class resolution.
- `GAAC resolve: cdo=... fn=...` — GetAllActorsOfClass resolved (non-null = good).
- `TargetTracking: N live target field(s)` — selection slots found.
- `scanned=... matched=... inRange=... selected=... targets=...` — per-scan summary
  (`scanned` is tiny — the GetAllActorsOfClass count, not ~400k).
- `proj: enum=N peak=N written=N mine=N pathN=N locked=N lockMe=N newOnMe=N newOther=N
  own=N drop=N evict=N interval=Nms idmap=N projids=N quar=N` — projectile scan, ~3 s throttle.
  `enum` = pointers GAAC returned (pool + live), `peak` = high-water since the last line,
  `written` = published that tick, `mine` = rounds flagged `LPROJ_INCOMING`, `own` = your own
  outgoing rounds refused attribution, `drop`/`evict` = array-cap pressure, `projids`/`quar` =
  assigned-id map size and quarantine depth. If a barrage hangs the game, the last flushed
  `proj:` line is the evidence.
- `threat: pathN=N locked=N lockMe=N newOnMe=N newOther=N pend=N mine=N own=N fb=N amb=N
  poseFail=onMe/all totLaunch=N totAttr=N flags=N proj=N` — the lock probe, ~2 s throttle.
  **`totLaunch`/`totAttr` are the ones to read**; everything instantaneous is sampled on a 2 s
  throttle over a 30 Hz probe and reads 0 on ~97% of samples. Reading `mine=0` as "nothing was
  attributed" from throttled samples once produced a completely wrong diagnosis — the totals
  exist so that cannot happen again.
  - `totLaunch` climbing, `totAttr` flat → launches detected but rounds aren't matching.
  - both flat while `lockMe>0` → the launch edge isn't firing.
  - `poseFail` with a non-zero `onMe` → a path we can't place, whose salvo can never warn.
- `threat-edge: lockMe A -> B (pathN=N locked=N)` — **UNTHROTTLED and edge-triggered**, so
  unlike the sampled lines it cannot miss a brief event. This is the line that proves whether
  anything was ever locked onto you in a session.
- `launch-match: dMine=Nm dOther=Nm pend=N me=Nm` — spatial tiebreaker, ~3 s throttle. A clean
  attribution is one small distance and one large (or `-1` = absent). Both small = the
  ambiguous case; both large = a round with no credible origin, rejected.
- `threat-mixed: N new path(s) on me, M on others in the same scan` — a scan the tiebreaker
  has to get right; worth lining up against any report of a wrong round flashing.
