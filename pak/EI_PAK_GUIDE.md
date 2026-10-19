# Enhanced Imaging .pak — build guide (EI_PAK 1)

The vision mode's scene edges, unit outlines and wave as an Unreal 4.26 post-process, so the engine supplies the depth, the view and VR eyes, with no buffer hunting or resolution guessing. The current method stays as the automatic fallback.

## Files (`pak\` in the project folder)

| File | What it is |
|---|---|
| `EI_Vision.ush` | **The shader — the one source.** Edit here, never in the editor. |
| `build_ei_materials.py` | Editor script: builds both materials from the .ush. Re-run after any shader edit. |
| `EI_PAK_GUIDE.md` | This guide. |

Plugin side (already built into the source, not yet tested): CameraCapture `cameracapture-23`, EnhancedImaging.dll `enhancedimaging-5`, Topo3DView (transparent overlay), GlobalSettings (four new Enhanced Imaging settings).

## One-time setup

1. **Create the mod** in the MW5 mod editor, named exactly **`EnhancedImaging`**. Its content then lives at `/EnhancedImaging/`, which is the path CameraCapture loads from. If you use another name, change `MOD_ROOT` in the script **and** `kPakMat` in `CameraCapture.cpp`.
2. **Enable Python:** Edit > Plugins, search "Python" and enable **Python Editor Script Plugin**; search "Editor Scripting" and enable **Editor Scripting Utilities**. Restart the editor.
   - If the Python plugin is missing, stop here and tell Claude. The fallback is a node-by-node manual build.

## Build (and after every shader change)

1. Window > Developer Tools > **Output Log**. Switch the box at the bottom from `Cmd` to **`Python`**, then run:
   ```
   exec(open(r"D:\Selfmade\mwvr POC\pak\build_ei_materials.py").read())
   ```
2. Expect `[EI pak] built …/M_EI_Vision_BT`, then `…_AT`, then `done - EI_PAK 1`.
3. Open **M_EI_Vision_BT** and **M_EI_Vision_AT** in the content browser and check the stats panel has **no red errors**.
   - Sampler-type complaint on UnitsTex: set the parameter's Sampler Type to whatever the editor says, save, and tell Claude so the script can be fixed.
   - Error inside the Custom node: copy the error text to Claude. Most likely a function name that differs in MW5's 4.26 build (`GetDefaultSceneTextureUV`, `GetViewportUV`, `SceneTextureLookup`).
4. **Shader code inside the materials (required):** the mod editor's `MW5Mercs\Config\DefaultGame.ini` must have `bShareMaterialShaderCode=False` (Project Settings > Packaging > Share Material Shader Code off). With it on, the shaders go into a separate `ShaderArchive-EnhancedImaging-PCD3D_SM5.ushaderbytecode` that the game does not load for the mod, and it silently uses its default (pass-through) post-process material instead: no vision, a stray border. MWVR ships its materials the same way (no archive). Check: the packaged .pak must contain no `.ushaderbytecode` file.
5. **Package the mod** with the mod editor's packaging / cook step, install it into the game's Mods folder, and enable it in the game's mod list.

## In the game

1. Copy `CameraCapture.dll` and `EnhancedImaging.dll` to `plugins`. Rebuild HUDDisplay.exe and GlobalSettings.exe.
2. In GlobalSettings, check that **Use the EnhancedImaging .pak** is ticked (it is by default).
3. **Load order:** put the EnhancedImaging mod high (last) in your mod manager. It overrides nothing, so this is for tidiness only. The order the effects draw in is set by the materials' Blendable Priority (set to 10000 so ours is drawn after other mods' effects at the same stage), not by load order.
4. Drop into a mission. `log.txt` should show, once you're in the mech:
   - `enhanced imaging .pak: running (version 1)`, which means it worked;
   - **or** `materials not found …`: the mod isn't loaded, or its name/path differs, and the current method is used;
   - **or** `function LoadAsset_Blocking not found`: the loading route isn't in MW5's build. Tell Claude; there is a plan B (a Blueprint in the mod that references the materials);
   - **or** `version N, this CameraCapture wants 1`: rebuild the .pak.
   - Then `effect on the player camera (CameraComponent)`. If the vision is switched on but nothing appears, set **Effect applies to** to **world**. That means MW5's camera doesn't pass camera post-process through; tell Claude.
5. Switch the vision on. EnhancedImaging.dll logs `the .pak draws the scene - overlay only`.

## Settings (GlobalSettings, HUD tab, Enhanced Imaging Vision — all live)

| Setting | Ini key `[hud]` | What it does |
|---|---|---|
| Use the EnhancedImaging .pak | `ei_pak` | Off: the current method. |
| Layering (.pak) | `ei_pak_layer` | **under** (default): everything under the vision, the exact look (After Tonemapping). **over**: explosions, smoke, cockpit glass and MWVR's in-world HUD drawn on top (Before Translucency); weather colour grading from UltraVisualAndWeather then tints the vision. |
| Effect applies to (.pak) | `ei_pak_attach` | **camera** (default): your view only. **world**: every view, including the CAP camera and MechWarriorVR's camera screens. Use world only if camera shows nothing. |
| Edge brightness (.pak) | `ei_edge_gain` | Scene edge strength. |
| Units capture size (.pak) | `ei_units_scale` | Units-only capture size as a share of the screen; lower is cheaper. |
| — (ini only) | `ei_wave_ripple` | Ripple strength at the wave front (default 0.006). |

The wave duration and fade band (`ei_wave_s`, `ei_wave_band_m`) apply as before.

**What needs a rebuild of the .pak:** only a change to the shader's maths in `EI_Vision.ush`. Colours (EdgeCol, UnitCol, GlowCol, BgCol) are material parameters. They're fixed at the script's defaults for now, and can be made live from the plugin the same way as the others if you want them tunable.

## How it fits together

- **CameraCapture** loads both materials and makes a dynamic instance of each. It holds them at weight 0 on a PostProcessComponent on the controller, and adds them to the **player's active camera** (the view target's camera, else the pawn's), re-checked every second. While the vision is up, the chosen one gets weight 1 on that camera. That keeps the effect off scene captures (our CAP camera, MechWarriorVR's HUD and thermal camera views). Every frame, after this frame's units capture, it sets: Opacity, WaveOn, Front, Band, Fade, Ripple, EdgeGain, UnitsOn and UnitsTex (that frame's ring texture). While the .pak runs, game-depth hunting is only active if HUDDisplay's E-IMG page asks for it.
- **The material** reads scene depth and scene colour for its own view, draws the edges (the same maths as `kDepthEdgePs`) and the unit outlines (units capture against scene depth), then mixes game and vision by the wave and its fade band, with the glow and ripple.
- **EnhancedImaging.dll**, when VHUD `EiPakOn` is set, draws only the overlay (terrain grid, labels and plates, crosshair, lock marker) on a transparent image. CameraCapture lays that over the frame, faded with the wave.

## Known limits of EI_PAK 1

- **The overlay** (grid, labels) is still drawn outside the engine and laid over the frame at present time. It uses the pose this frame renders with, but it can trail the engine-drawn scene by about a frame while turning fast. A later step can shift it by the turn since its pose, or move the grid into the material.
- **Unit outlines** still come from the separate units capture: one camera, so in VR they're right for the centre view only. The scene edges and the wave are per eye.
- **Before Translucency** runs before the tonemapper. The material cancels auto exposure (EyeAdaptation input), but the tonemapper's curve still shifts the colours a little, so tune the colours there, or use After Tonemapping for the exact look.
- **HUDDisplay's fallback renderer** (used only when EnhancedImaging.dll isn't running) isn't .pak-aware.
