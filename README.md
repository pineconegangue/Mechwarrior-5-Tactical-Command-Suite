# MW5 Tactical Command Suite

UEVR plugins and companion apps for MechWarrior 5: Mercenaries in VR (UEVR + the
[MechWarriorVR](https://www.nexusmods.com/mechwarrior5mercenaries/mods/1009) mod). Grew out of
[MW5-UEVR-Plugins](https://github.com/sicsix) by Tim Butler (MIT).

Players: download the release package and follow its `README.txt`.

## What is here

| Piece | Source | Does |
|---|---|---|
| LanceControl.dll | `src/lance_control` | lance orders, TacOps, insane allies, nav points, the lance status feed |
| LidarExport.dll | `src/lidar_export` | LIDAR contacts, projectiles, terrain, mech records, dropships |
| VanillaHud.dll | `src/vanilla_hud` | the game's HUD elements captured as textures |
| CameraCapture.dll | `src/camera_capture` | targeting / zoom cameras, the in-game TacOps overlay |
| ScreenExport.dll | `src/screen_export` | MechWarriorVR's cockpit screens for the MFD |
| HUDDisplay.exe | `src/hud_display` | the MFD app (LIDAR, TOPO, 3D, TacOps, camera and HUD pages) |
| VoiceCommand.exe | `src/voice_command` | voice orders (Whisper), pilot replies and callouts (Kokoro) |
| GlobalSettings.exe | `src/global_settings` | the shared settings window (`GlobalSettings.ini`) |

Shared-memory layouts between them are in `include/` (`VanillaHudShared.h`, `LanceStatusShared.h`,
`LidarShared.h`, `TerrainShared.h`).

## Building

Everything needed is in this repository except whisper.cpp:

- Visual Studio 2022 with C++ and CMake.
- Open the folder; the CMake configuration `x64-Debug` (`CMakeSettings.json`) builds **Release** with the
  static runtime into `out\build\poc-x64-Debug\`. It configures with `-DCMKR_SKIP_GENERATION=ON`:
  `CMakeLists.txt` is maintained by hand (keep `cmake.toml` in step).
- Clone with submodules: `git clone --recursive <this repo>` (or `git submodule update --init` afterwards) -
  `ext/glm` and `ext/minhook` are submodules. Vendored: `ext/uevr`, `ext/miniz`, `ext/sig`, `ext/sherpa-onnx`
  (prebuilt Windows package).
- **whisper.cpp is not included.** Put [whisper.cpp](https://github.com/ggml-org/whisper.cpp) at commit
  `6e4ab854f67f743900934a703d5603419384c961` (2026-09-28, "v1.9.4-182"; the tested version - tagged releases
  differ) in `ext\whisper.cpp\`:

      git clone https://github.com/ggml-org/whisper.cpp ext/whisper.cpp
      git -C ext/whisper.cpp checkout 6e4ab854f67f743900934a703d5603419384c961

  Without it VoiceCommand still builds, but with the fixed Windows phrases only (no free speech).
- Optional: the [Vulkan SDK](https://vulkan.lunarg.com/) lets Whisper run on the graphics card
  (`VC_WHISPER_VULKAN`, on by default when the SDK is found); without it Whisper is CPU only.

## Packaging

After a build: `.\package_release.ps1` assembles `release\MW5 Tactical Command Suite\` (plugins, apps with voices, the
Whisper model, dropship outlines, readme and notices; paperdolls are made by each player with
GlobalSettings > PAPERDOLLS > EXTRACT). It checks for everything first and never
deletes anything.

Data that ships with the apps: `models\` (Whisper), `kokoro-82m\kokoro-multi-lang-v1_0` (voices),
`paperdolls\` and `assets\dropships\` (regenerate them from the game with `tools_paperdoll_extract.py` and
`tools_dropship_outline.py` after a game update), `src\voice_command\HeardAs.txt` / `Pronunciations.txt`.

## Licence

MIT (`LICENSE`); third-party components: `THIRD_PARTY_NOTICES.txt`.
