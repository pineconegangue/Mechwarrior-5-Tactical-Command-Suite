# MW5 Tactical Command Suite

Companion app for MechWarrior 5: Mercenaries 

Players: download the release package and follow its `README.txt`.

Description
DISCLAIMER: AI was used in the making of this mod - source code: https://github.com/pineconegangue/Mechwarrior-5-Tactical-Command-Suite

Mechwarrior 5 Tactical Command Suite is a companion app that reimagines the way Mechwarrior 5 Mercenaries is played. It did not begin that way - it was a humble app that transferred HUD screens to an external monitor, but things escalated and here we are. There is a lot to unpack here, please be patient, take your time to read through everything so that you have a good understanding of what to expect before proceeding. I will make a video demonstrating thee installation steps in a visual format at a later date, unfortunately I have no time currently - you can subscribe to me at https://www.youtube.com/@Gangue_    to stay up-to-date


Main features:

- MWVR HUD screen export - exporting HUD screens to an external window
- Vanilla HUD screen export - exporting HUD screens to an external window
- 2d topographical map (superceded but still there if you want to see it)
- 2d radar with projectile tracking (integrating 2d topographical map)
- 3d topographical map with projectile tracking
- 3d radar with projectile tracking, enemy tracking, target info, lancemate selection, command interface, unique AI commands, unique AI firing modes. unique AI behaviours, called shots at specific locations (cockpit might be OP, have not tested but recommend not to abuse..)
- Zoom camera with thermal imaging mode (accidentally named IR, very performance heavy, not recommended to use)
- Enhanced Imaging mode (both integrated in-game and on external app screen according to user preference)
- Lancemate summary screen - includes status of ongoing commands, targeting
- Voice command function - give dynamic voice commands to your lancemates who will translate your commands into actual functions, or ask them questions and they can give you answers - apparently 56 different commands or questions you can ask, but over 4.6 million ways to ask not accounting for distance and bearing, over 1 billion possibilities with direction and bearing considered. The pilot voices use a TTS model and not the in-game voices as I was not comfortable with ingesting existing pilot voicelines to supplement this mod. In-game pilot voices will need to be muted for the best experience (toggle provided).
- AI firing mode overrides - vanilla, insane (rarely misses, fires all weapon groups simultaneously) and impossibly strong (same as insane, rarely misses CT/cockpit).

Installation instructions:
NOTE: I DID NOT VERIFY THAT THESE STEPS WORK YET AS MY ENVIRONMENT WAS ALREADY SET UP. PLEASE LET ME KNOW IF YOU RUN INTO ANY ISSUES (THERE MAY BE BUGS). Make sure you take the time to actually go through these steps 1 by 1, including reading the settings guides after.

1. Download release .zip from Nexus (this mod page)
2. Download UEVR v1.05 - https://github.com/praydog/UEVR/releases
3. Extract the UEVR zip file somewhere, you may need to add an exception to your antivirus software for this folder
4. Extract the mod .zip folder somewhere (except program files)
5. Navigate within the .zip to 'mods' folder - move EnhancedImaging into your game's mods folder
6. Navigate within the .zip to 'plugins' folder - copy all the .DLLs
7. Navigate to where you extracted UEVR - Open UEVRInjector.exe
8. Click "Open Global Dir", then navigate to MechWarrior-Win64-Shipping>plugins
9. Paste all the .DLLs you copied earlier into this folder.
10. Start up Mechwarrior 5 Mercenaries
11. Once loaded into main menu, go to mods and activate Enhanced Imaging
12. Restart game
14. Once back in-game and in main menu, go to UEVR, select the game and press "Inject" (before you do this though, please read the next section on settings setup).
15. If your cursor seems to remain on the screen, UEVR will have an option to hide your cursor while in-game, you can set this as a default setting in UEVR's config file (open global dir>Mechwarrior-Win64-Shipping>config.txt>edit line "FrameworkConfig_AlwaysShowCursor" to = false, and save.

SETTINGS GUIDE: GlobalSettings.exe (open it up to follow along) - Navigate within the MW5 Tactical Command Suite folder to to "apps" - open GlobalSettings.exe:

Lance control tab:
Here, you can set your lancemate competence (impossibly strong allies really trivialises the game, just a warning) and choose to mute the game's lancemate's voicelines (untick this if you're not going to use Voicecommand)

HUD tab: 
Panels: All in-game HUD elements are disabled by default, turn them all back on if you want the vanila experience
In-game: All in-game widget elements are disabled by default except the crosshair, turn them all back on if you want the vanilla experience
Vision: This is where you set your keybind for enhanced imaging mode. You can also choose to toggle off the crosshairs that come with enhanced imaging mode in this tab. Recommend not to touch any other setting here except unit capture size (lower if you want more FPS).
Compass: To toggle compass settings on the various radar modes if you wish (can be done from the screen itself)
Alarm: To toggle and customize the custom incoming missile alarm
LIDAR: To toggle the various things that are tracked by the various radar modes.
Radar: Sorry this tab is cut off a bit, will fix next time. Basically choose between vanilla detection or a cheatier 1km/5km detection (the larger ranges will clash with the in-game target acquisition system, you can still give orders to your lancemates and some will override the game, but some will not).

Voice command tab:
Comms: If you want to use voice command, this tab lets you set your push-to-talk button (default is open mic). You can also edit the pilot lines to change what they say (good luck if you're messing with this), fix pronounciation if the pilots pronounce something wrong and fix misheard words if you find that the software is just not understanding some common phrases (avoid adding common words though, it will mess up the order parsing logic)
Callouts: If you find that the pilots are telling you too much info, you can choose what type of info you no longer wish to hear.
Voices: You can give your lancemates new voices - campaign pilots will require at least 1 mission running with voicecommand open for names to be detected. Some voices are better than others, and generally they sound best when sped up to 120-130% depending on the speaker.
Radio: Probably nothing you'd want to touch here, this just lets you tweak the radio static effects if you prefer it sounding clearer/messier.
Engine: Default settings for the most part, if you have a strong GPU you may want to enable Whisper on the GPU for possibly faster processing. 

Paperdolls tab:
VERY IMPORTANT - select your game directory and press extract/rebuild to have paperdolls enabled in the different radar modes.

HUD profiles:
This is where you can launch the various HUDDisplay windows that you save if you decide to run multiple instances.

Debug: Ignore this tab unless troubleshooting



SETTINGS GUIDE: HUDDisplay.exe (open it up to follow along) - Navigate within the MW5 Tactical Command Suite folder to "apps" - open HUDDisplay.exe:

Press esc to view the settings for each HUDDisplay instance - since you can have multiple windows open at once, you can also save your settings for each individual window. You can click a bezel slot, drag it around if you want, and assign a keybind and a function. Have a play through the functions, I would advise to avoid V-POD, CAP and IR due to poor optimization, but if you have a killer GPU then go for it. When you are done with your preferred settings, press SAVE AS and name it whatever you want. This will create a shortcut that you'll be able to launch (either by itself or via Globalsettings) so you can keep all your settings.


Requirements
UEVR v1.05
Everything else should be included in the release .zip

Shout outs
Thank you to sicsix, Perq and psycho063, the team behind the MechwarriorVR mod for the inspiration to venture out and try to accomplish this on my own.


FOR THOSE INTERESTED IN THE REPO:

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

## Models (not in the repository - too large for GitHub)

VoiceCommand needs two downloads to speak and to understand speech; `package_release.ps1` will not run
without them. Players get both inside the release zip.

- **Whisper speech recognition**: `ggml-base.en.bin` (148 MB) from
  [ggerganov/whisper.cpp on Hugging Face](https://huggingface.co/ggerganov/whisper.cpp/blob/main/ggml-base.en.bin)
  (or whisper.cpp's `models\download-ggml-model.cmd base.en`). Put it at `models\ggml-base.en.bin`.
  SHA-1 `137c40403d78fd54d454da0f9bd998f78703390c`.
- **Kokoro voices**: sherpa-onnx's
  [kokoro-multi-lang-v1_0.tar.bz2](https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-multi-lang-v1_0.tar.bz2)
  (350 MB). Unpack it so that `kokoro-82m\kokoro-multi-lang-v1_0\model.onnx` exists.



## Licence

MIT (`LICENSE`); third-party components: `THIRD_PARTY_NOTICES.txt`.
