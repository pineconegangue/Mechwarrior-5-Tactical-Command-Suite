MW5 Tactical Command Suite - MechWarrior 5: Mercenaries in VR (UEVR + MechWarriorVR)
====================================================================================

What you get
------------
  plugins\  UEVR plugins that run inside the game
            LanceControl.dll   lance orders, TacOps, insane allies, the lance status feed
            LidarExport.dll    LIDAR contacts, terrain, mech records, dropships for the maps
            VanillaHud.dll     the game's HUD elements, captured for the MFD screens
            CameraCapture.dll  targeting / zoom cameras and the in-game TacOps overlay
            ScreenExport.dll   MechWarriorVR's cockpit screens for the MFD's MWVR pages
            EnhancedImaging.dll the Enhanced Imaging vision mode (edges, units, lock bracket over the game)
  mods\     EnhancedImaging\   the vision mode's in-game part (a normal MW5 .pak mod)
  apps\     programs you run beside the game
            HUDDisplay.exe     the MFD screens (LIDAR, TOPO, 3D, TacOps, cameras, HUD pages)
            VoiceCommand.exe   voice orders to your lance, pilots that answer and call out
            GlobalSettings.exe every shared setting, in one window

Requirements
------------
  - MechWarrior 5: Mercenaries (PC), Windows 10/11 64-bit
  - UEVR, with the MechWarriorVR (MWVR) mod and its UEVR profile set up
  - A CPU with AVX2 (any recent Intel / AMD) for the speech recognition (or, optionally, the graphics
    card - see "Orders are heard slowly" below)
  - A microphone for voice orders (headphones recommended - the pilots can hear the speakers)

Install
-------
  1. Copy everything in plugins\ into
         %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\plugins\
     (paste that path into Explorer's address bar). Make sure the game is closed.
  2. Put the apps\ folder anywhere you like (e.g. Documents\MW5 Tactical Command Suite). Keep its contents together:
     the programs find their voices, models and art in the folders beside them.
  3. Copy the mods\EnhancedImaging folder into the game's Mods folder, e.g.
         C:\Program Files (x86)\Steam\steamapps\common\MechWarrior 5 Mercenaries\MW5Mercs\Mods\
     and make sure it is enabled in the game's MODS menu. (It may be listed as made for an older game
     version - that is fine.) Without it the vision mode still works, the older way.
  4. Start the game in VR as usual, then run HUDDisplay.exe and VoiceCommand.exe from apps\.
     GlobalSettings.exe changes settings at any time; changes apply at once.

First run: the game is unchanged until you say so
-------------------------------------------------
  The shipped settings keep MW5 exactly as it is without the suite. Turn on what you want in
  GlobalSettings.exe:
    - LANCE CONTROL > Allies: Vanilla / Insane / Impossibly strong lancemates, and muting the game's own
      lancemate voices (when you use VoiceCommand's pilots instead). Lancemates the suite fires for (insane
      allies, or custom fire settings) hold their TAG on the target, and punch / swing melee weapons at an
      enemy already within arm's reach in front of them (melee always on; hold fire stops it too).
    - HUD > PANELS ON HUDDISPLAY: which game HUD panels move off the game's screen onto the MFD pages.
    - HUD > ENHANCED IMAGING VISION > Vision mode key (or say "activate enhanced imaging").
    - VOICE COMMAND > LISTENING FOR ORDERS > Push-to-talk key. Voice orders are push-to-talk by default
      and there is NO key set: nothing is heard until you pick one. HOTAS / controller users: map a
      stick button to that key with your stick software (or JoyToKey). "Always" listening is there too
      (use headphones - the pilots' replies can be heard by the mic).
    - PAPERDOLLS > EXTRACT: the mech damage pictures are not shipped - press EXTRACT once to make them
      from your own game files (and again after the game or a mech mod updates).

More than one MFD screen (HUDDisplay profiles)
---------------------------------------------
  Each HUDDisplay window is a "profile" with its own layout, pages, bezel labels, hotkeys and
  window position. Run one HUDDisplay.exe per screen, each with a different profile name:
    1. Right-click HUDDisplay.exe > Create shortcut (do this once per screen you want).
    2. Right-click the shortcut > Properties, and add the profile to the end of Target, after a space:
           "...\apps\HUDDisplay.exe" profile=left
       Use a different name for each shortcut (left, right, centre, ...). Letters and digits only.
    3. Start each shortcut, move/size each window onto its screen and set it up (edit mode).
       It saves on its own - next time each shortcut opens exactly as you left it.
  Without profile= the window uses the "default" profile.
  Each profile is saved in apps\ as MFDDisplay_<name>.cfg (log: HUDDisplay_<name>.log). Copy a
  .cfg to a new name to start a new profile from an existing layout; delete one to reset it.
  Don't run two windows with the SAME profile name - they share one file and the last to close wins.
  Easier ways to make one:
    - In a HUDDisplay window's edit mode: SAVE AS..., type a name, Enter - it saves this layout as a new
      profile and makes a "HUDDisplay - <name>" shortcut beside HUDDisplay.exe; OPEN IT starts it.
    - GlobalSettings > HUD PROFILES: NEW PROFILE, and per profile LAUNCH / RENAME / SHORTCUT / REMOVE,
      plus OPEN ALL MY SCREENS. A profile that is running cannot be renamed or removed.

Hotkeys (HUDDisplay, default layout - every one can be changed in edit mode)
--------------------------------------------------------------------------
  Esc           edit mode (labels, pages, hotkeys, layout)       F11   borderless window
  Num0          LIDAR scope              Num + / Num -  LIDAR zoom in / out
  Num7          TOPO map                 Num8           3D TacOps map        Num *  heading compass on / off
  Num9          zoom camera (CAP)        Page Down      TacOps overlay over the game, on / off
  Bezel buttons with no key: TAC (TacOps page), and on the right the 3D camera - FWD, BACK, UP, DWN, CTR.
  Num1 .. Num6  game HUD pages: target, weapons, own mech, objectives, map, lance status
  While HUDDisplay runs, its hotkeys belong to it - the game does not see those keys. If you have
  numpad keys bound in MW5 or your HOTAS software, change one side.

TacOps map (Num8, or the in-game overlay)
-----------------------------------------
  Select lancemates: click one; Shift / Ctrl-click to add or remove; drag a box around several.
  Right-click the ground   beeline here (also: firing on the move, fall back facing the enemy,
                           hold with indirect fire only, beeline and hold), add waypoint,
                           form on me, cancel route, your own torso, mark / remove a nav point
  Right-click an enemy     attack, focus fire (whole lance), move to engage,
                           lock this enemy - your lock on it; greyed out "Lock (not detected)" until you
                             or a lancemate detects it (line of sight or sensors),
                           called shot > legs, arms, side torsos, either leg / arm / torso, centre torso,
                             head, or cancel - for the selected lancemates, who attack it and aim there,
                           flank - the selected lancemates swing round to its side, firing on the move;
                           pincer - two or more selected lancemates take a side each,
                           torso: track this target / release
  Right-click a lancemate  its fire settings (and every selected lancemate's)
  Heading compass          a heading strip along the top of the 3D and TacOps maps (turns with your torso; green dots
                           = lancemates, red = enemies), a compass ring on the ground at 1 km with 250 / 500 m rings,
                           clock positions around you, and a compass ring on the LIDAR scope - for orders like
                           "two, heading two-seven-zero, five hundred metres". Each part can be switched off in
                           GlobalSettings > HUD > COMPASS; a CMPS bezel button hides the compass on that screen.
  Radar mode               GlobalSettings > HUD > RADAR. Full LIDAR (default): every enemy within 1 km. Vanilla:
                           only enemies the game itself detects (your radar, what your lance relays, a lancemate's
                           radar or sight) on the LIDAR scope, 3D / TacOps maps and compass dots - an enemy that
                           drops off leaves a grey last-known mark on the maps. Display only; the game is unchanged.
  Missile alarm            GlobalSettings > HUD > ALARM: on / off, volume, and the sound (warble, double pip,
                           triple pip, fast warble). Sounds while a missile is locked on you - slow while it is far,
                           fast in the last 250 m - with an INCOMING MISSILE cue on every HUDDisplay page.

Voice orders - cheat sheet
--------------------------
  Who: "one" .. "four", a pilot's name, "two and three", or "lance" / "everyone" / "all units". Leave it out
  within a few seconds to talk to the same one again. Speak naturally - these are examples, not the only
  wording. Say "scratch that" / "never mind" / "belay that" to cancel, "actually ..." to correct the last order.
    Movement   form on me / follow me - hold position / hold here - fall back (to me) - regroup / rally
               move north 200 metres - fall back 100 metres - push east 2 klicks
               move to my 3 o'clock, 400 metres - heading 270, 1200 metres, and hold
               advance to nav Bravo - hold at nav Charlie (nav points are Alpha, Bravo, Charlie, Delta ...;
               the suite never uses Kilo or Victor)
               Every move order moves while firing on the move (move / go / advance / push, nav points,
               "get eyes on the Atlas"). Only "make a run for it" / "get out of there" / "bug out" /
               "break off" is a beeline: straight to 200 m behind you, not firing.
               get up on that hill / take the high ground - the pilot marks 3-4 hilltops (150-800 m, all
               around you) as nav points and asks "which hill?"; answer with its name ("Foxtrot", "nav Foxtrot"),
               "the highest", "the closest" or "the second one" - the others disappear. "Never mind" or 30 s
               with no answer clears them. Hills already at a nav point are not offered again.
    Combat     attack my target - attack the nearest enemy - pick your targets - focus fire on my target
               attack the Atlas / take down that Atlas / engage that Atlas (by its name)
               weapons free - hold fire - cease fire - return fire - indirect fire only
               two, go give four some support / back up two / assist three - attack that lancemate's target
               (if it has none: whoever is hitting it, else the nearest enemy it can see)
               take out the turrets / kill the tanks first / go for the VTOLs - the nearest of that kind,
               then the next, until none are left on the 1 km scan (any new order ends it)
    Flank      flank my target - two, flank left / flank right (your left / right) - flank the Atlas on his left
               - flank behind it - flank my target, stay 300 out (stand-off; otherwise the pilot picks the
               range its weapons do most damage at)
               two and three, pincer my target / pincer the Atlas - each takes a side (needs two lancemates).
               They swing round firing on the move, hold that side as the target turns, and keep it.
               Best for direct-fire mechs - an LRM boat is better left at range.
    Called     aim for the Atlas's left torso - legs on the Atlas - shoot his head - centre torso on my target
    shots      (legs, arms, side torsos, left / right leg, arm or torso, centre torso, head, cockpit)
               cancel called shot
    Weapons    lasers only - missiles only - PPCs only - ballistics only - all weapons - reset fire orders
    Heat       run cool - normal heat - run hot - alpha strike
    Reports    status / status report / sitrep ("two, status") - report in / sound off - how are you holding up
               radio check / comms check / do you copy - each pilot answers about the radio ("loud and clear")
    Questions  where's the Atlas? - is the Atlas down? - can you see the Atlas? - who's closest to it?
               which way is the Atlas facing? - who's firing at me? - where are you?
    Lock       patch it through ("patch in the Atlas") - a lancemate's target becomes your lock; only works
               on an enemy you or a lancemate detects
    Vision     activate / initiate / deactivate enhanced imaging
  Misheard words: add them to HeardAs.txt (GlobalSettings > VOICE COMMAND > COMMS opens it).

Troubleshooting
---------------
  The apps say they are waiting for the game
      UEVR must be injected (the plugins only load in VR). Check that the six DLLs are in
      %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\plugins\ and look for lines with BUILD= in
      log.txt in the folder above it - one per plugin.
  Voice orders are not heard
      Push-to-talk is the default and has no key until you set one (see First run). Check VoiceCommand's
      window: it shows what it heard. The microphone is Windows' default recording device.
  "Say again?" a lot
      Start with who ("two, ..."), speak clearly, and add misheard words to HeardAs.txt.
      A clipped callsign ("Do" for Two, "Re" for Three) is guessed when the rest is an order: GlobalSettings >
      VOICE COMMAND > ENGINE: "Guess a clipped callsign" (on by default).
  Orders are heard slowly
      GlobalSettings > VOICE COMMAND > ENGINE: "Whisper: use the graphics card" (restart VoiceCommand).
      Much faster, but it shares the card with the game - check VR stays smooth. Off by default.
  The pilots' own replies trigger orders
      Use headphones, or keep push-to-talk.
  The vision mode does nothing
      Set the vision key (GlobalSettings > HUD) or say "activate enhanced imaging". For the best look,
      install mods\EnhancedImaging and enable it in the game's MODS menu; log.txt then shows ".pak".
  Wrong or empty data after an update
      Plugins and apps must come from the same release - update both together, and close VoiceCommand
      before starting the game again.
  Windows SmartScreen / antivirus warns about the programs
      They are unsigned hobby builds. Allow them, or build them yourself from the source.

Co-op and multiplayer
---------------------
  The suite is made for and tested in SINGLE-PLAYER only. In co-op the lance orders, insane allies and
  status feeds are untested and may misbehave or desync. Before playing co-op, move the plugin DLLs out
  of the plugins folder (or set Allies to Vanilla and don't give orders).

Updating
--------
  Close the game, HUDDisplay and VoiceCommand, then replace the plugins, apps and mods\EnhancedImaging
  with the new ones. Your settings (GlobalSettings.ini, the MFD profiles, PilotVoices.txt) live in apps\ -
  back them up first, as the new apps\ folder brings a fresh GlobalSettings.ini.

Uninstalling
------------
  1. Close the game and the apps.
  2. Delete the six DLLs above from %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\plugins\.
  3. Delete the EnhancedImaging folder from the game's MW5Mercs\Mods\ folder.
  4. Delete the apps folder.
  5. Optional: in %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\ delete GlobalSettingsPath.txt and
     ai_probe.log (the suite's; log.txt and the rest belong to UEVR).

Notes
-----
  - The game must be running in VR (UEVR) for the plugins to load; the apps wait for them.
  - VoiceLines.txt, HeardAs.txt and Pronunciations.txt in apps\ can be edited to change what pilots say
    and to fix misheard words (open them from GlobalSettings > VOICE COMMAND > COMMS).
  - Logs: the plugins write to %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\log.txt (and, only with
    GlobalSettings > DEBUG > AI probe ticked, LanceControl to ai_probe.log there - both start fresh each game
    session); VoiceCommand writes VoiceCommand_heard.log beside itself (emptied each time it starts).
  - GlobalSettings > DEBUG: probes for bug reports (AI probe, aim-writer probe). Off by default - they cost
    game-thread time and write large logs.

Licence: MIT (LICENSE.txt). Third-party components and their licences: THIRD_PARTY_NOTICES.txt.
