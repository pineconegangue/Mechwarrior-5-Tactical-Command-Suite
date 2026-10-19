// =====================================================================================
// GlobalSettings.exe (2026-10-07f, v2 2026-10-07g) - the settings shared by every window and app, in
// one place: GlobalSettings.ini beside the exes (include/GlobalSettings.h). One tab per module
// (LanceControl, VoiceCommand with its own sub-tabs, HUDDisplay); every change is written at once and
// the running programs pick it up within half a second - there is no SAVE. Settings marked * are read
// by VoiceCommand at start only (model files, engines, threads, beeps).
// v2 (user test with a non-programmer review): plain wording, voices by name with a TEST button (asks
// a running VoiceCommand to speak, through GlobalSettings.ini's [test] section), percentages, greyed
// settings that do not apply, a VoiceCommand running / START indicator, MFD window launchers, reset a
// page to defaults and FORGET with a confirmation click, a pilot list that pages.
// 2026-10-07h (user): HUD DISPLAY tab gone; a CALLOUTS tab with a tick box per kind of callout (and "Time
// between callouts" in seconds instead of the chatter level); each voice has its own speed; Whisper / Windows
// and Kokoro / Windows voices named as such; push-to-talk is Always or Push-to-talk only.
// 2026-10-10 (user): tidy-up - every explanation (and why a setting is greyed) moved into a small "?" after its
// field or section title, shown on hover (DrawTip); rows on an even grid; Enhanced Imaging moved off HUD > IN GAME
// (it ran past the window) onto its own HUD > VISION page; dropdowns as wide as their longest choice; the
// push-to-talk warning matches VoiceCommand (no key = nothing heard).
// Looks like HUDDisplay's Esc menu (same colours, Bahnschrift, boxes and steppers), drawn with
// Direct2D on a fixed canvas scaled to the window.
// =====================================================================================
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "GlobalSettings.h"
#include "../common/PaperdollExtract.h" // 2026-10-17: PAPERDOLLS page - the masks from the player's own game files
#include "../hud_display/ThreatAlarm.h" // TEST ALARM (2026-10-13): the same synthesiser HUDDisplay uses

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

// HUDDisplay's UI colours (its UiCol namespace).
namespace UiCol
{
constexpr UINT32 Panel = 0x0A140E;
constexpr UINT32 PanelEdge = 0x1E6B3C;
constexpr UINT32 PanelText = 0x9CFFC0;
constexpr UINT32 PanelDim = 0x3F7A55;
constexpr UINT32 CheckOn = 0x2FD46B;
constexpr UINT32 Warn = 0xFFB020;
} // namespace UiCol

// ---- what can be set -----------------------------------------------------------------
enum Kind
{
    K_Header,  // a section title (and its underline)
    K_Note,    // a line of dim text
    K_Col,     // start the second column
    K_Check,
    K_Num,     // [-] value [+]
    K_Choice,  // a dropdown of fixed choices
    K_Voice,   // a dropdown of Kokoro voices + TEST
    K_Key,     // a key, captured
    K_Whisper, // a dropdown of speech recognition models + BROWSE (two lines)
    K_Kokoro,  // a dropdown of voice packs + BROWSE (two lines)
    K_Open,    // a button opening a text file in Notepad
    K_Pilots,  // PilotVoices.txt: one row per pilot
    K_Action,  // a button doing something (A_*)
    K_Tick,    // a compact tick box (the callout list)
};

enum Action
{
    A_None, A_TestRadio, A_TestAlarm,
    A_PdExtract, A_PdBrowse, A_PdOpen, // 2026-10-17: PAPERDOLLS page
};

struct Opt
{
    std::wstring Value, Label;
};

struct Item
{
    Kind         K;
    const wchar_t *Sec = L"", *Key = L"";
    std::wstring Label;
    std::wstring Def;
    double       Lo = 0, Hi = 1, Step = 1;
    int          Dec = 0;
    bool         Pct = false; // shown as a percentage (value x 100)
    bool         Inv = false; // ... of (1 - value): "+" lowers the stored number
    std::wstring Unit;        // after the number ("s", "Hz", "dB")
    std::vector<Opt> Opts;
    bool         Restart = false; // read by VoiceCommand at start only - shown with a *
    std::wstring Help;
    // Only when another setting has this value (else greyed, with the reason).
    const wchar_t *IfKey = nullptr, *IfVal = nullptr, *IfWhy = nullptr;
    int          Act = A_None;
};

struct Page
{
    std::wstring Tab, Sub;
    std::vector<Item> Items;
    bool         Reset = false; // a RESET PAGE button
};

// The Kokoro voices (ids 0..27 of kokoro-multi-lang-v1_0, the English ones - VoiceCommand's kKokoroNames),
// with names people can read.
struct VoiceName
{
    const wchar_t *Code, *Label;
};
static const VoiceName kVoices[] = {
    {L"af_alloy", L"Alloy - US female"},       {L"af_aoede", L"Aoede - US female"},     {L"af_bella", L"Bella - US female"},
    {L"af_heart", L"Heart - US female"},       {L"af_jessica", L"Jessica - US female"}, {L"af_kore", L"Kore - US female"},
    {L"af_nicole", L"Nicole - US female"},     {L"af_nova", L"Nova - US female"},       {L"af_river", L"River - US female"},
    {L"af_sarah", L"Sarah - US female"},       {L"af_sky", L"Sky - US female"},         {L"am_adam", L"Adam - US male"},
    {L"am_echo", L"Echo - US male"},           {L"am_eric", L"Eric - US male"},         {L"am_fenrir", L"Fenrir - US male"},
    {L"am_liam", L"Liam - US male"},           {L"am_michael", L"Michael - US male"},   {L"am_onyx", L"Onyx - US male"},
    {L"am_puck", L"Puck - US male"},           {L"am_santa", L"Santa - US male"},       {L"bf_alice", L"Alice - UK female"},
    {L"bf_emma", L"Emma - UK female"},         {L"bf_isabella", L"Isabella - UK female"}, {L"bf_lily", L"Lily - UK female"},
    {L"bm_daniel", L"Daniel - UK male"},       {L"bm_fable", L"Fable - UK male"},       {L"bm_george", L"George - UK male"},
    {L"bm_lewis", L"Lewis - UK male"}};
static const int kVoiceCount = (int)(sizeof(kVoices) / sizeof(kVoices[0]));
// VoiceCommand's own defaults per lance number (kKokoroDefault).
static const wchar_t *const kKokoroDefault[9] = {L"bm_george", L"am_adam", L"am_michael", L"af_heart", L"bm_lewis",
                                                 L"am_fenrir", L"bf_emma", L"am_puck", L"af_bella"};

static Item H(const wchar_t *s)
{
    Item i;
    i.K = K_Header;
    i.Label = s;
    return i;
}
// 2026-10-10: a section title with a "?" (its explanation, shown on hover).
static Item HH(const wchar_t *s, const wchar_t *help)
{
    Item i = H(s);
    i.Help = help;
    return i;
}
static Item N(const wchar_t *s)
{
    Item i;
    i.K = K_Note;
    i.Label = s;
    return i;
}
// 2026-10-17: a line of text filled in live (App::LiveText(key)) - the PAPERDOLLS status lines.
static Item Live(const wchar_t *key)
{
    Item i;
    i.K = K_Note;
    i.Key = key;
    i.Label = L"-";
    return i;
}
static Item Col()
{
    Item i;
    i.K = K_Col;
    return i;
}
// 2026-10-08: a helper's item moved to another section (the K_Key / number helpers write [voice]).
static Item OnSec(Item i, const wchar_t *sec)
{
    i.Sec = sec;
    return i;
}
static Item C(const wchar_t *sec, const wchar_t *key, const wchar_t *label, bool def, const wchar_t *help = L"", bool restart = false)
{
    Item i;
    i.K = K_Check;
    i.Sec = sec;
    i.Key = key;
    i.Label = label;
    i.Def = def ? L"1" : L"0";
    i.Help = help;
    i.Restart = restart;
    return i;
}
static Item Nm(const wchar_t *key, const wchar_t *label, double def, double lo, double hi, double step, int dec,
               const wchar_t *unit = L"", bool restart = false)
{
    Item i;
    i.K = K_Num;
    i.Sec = L"voice";
    i.Key = key;
    i.Label = label;
    wchar_t b[32];
    swprintf(b, 32, L"%.*f", dec, def);
    i.Def = b;
    i.Lo = lo;
    i.Hi = hi;
    i.Step = step;
    i.Dec = dec;
    i.Unit = unit;
    i.Restart = restart;
    return i;
}
// A percentage: stored 0..1 (or lo..hi), shown x 100.
static Item Pc(const wchar_t *key, const wchar_t *label, double def, double lo, double hi, bool inv = false)
{
    Item i = Nm(key, label, def, lo, hi, 0.05, 2);
    i.Pct = true;
    i.Inv = inv;
    return i;
}
static Item Ch(const wchar_t *key, const wchar_t *label, const wchar_t *def, std::vector<Opt> opts, const wchar_t *help = L"",
               bool restart = false)
{
    Item i;
    i.K = K_Choice;
    i.Sec = L"voice";
    i.Key = key;
    i.Label = label;
    i.Def = def;
    i.Opts = std::move(opts);
    i.Help = help;
    i.Restart = restart;
    return i;
}
static Item Sp(Kind k, const wchar_t *key, const wchar_t *label, const wchar_t *def, const wchar_t *help = L"", bool restart = false)
{
    Item i;
    i.K = k;
    i.Sec = L"voice";
    i.Key = key;
    i.Label = label;
    i.Def = def;
    i.Help = help;
    i.Restart = restart;
    return i;
}
// 2026-10-10: an explanation for a helper without a help argument (numbers, keys).
static Item Hp(Item i, const wchar_t *help)
{
    i.Help = help;
    return i;
}
static Item If(Item i, const wchar_t *key, const wchar_t *val, const wchar_t *why)
{
    i.IfKey = key;
    i.IfVal = val;
    i.IfWhy = why;
    return i;
}
static Item Act(const wchar_t *label, int act, const wchar_t *help = L"")
{
    Item i;
    i.K = K_Action;
    i.Label = label;
    i.Act = act;
    i.Help = help;
    return i;
}

static Item T(const wchar_t *key, const wchar_t *label, bool def, const wchar_t *ifKey = L"callouts", const wchar_t *ifVal = L"1",
              const wchar_t *why = L"(callouts are off on COMMS)")
{
    Item i;
    i.K = K_Tick;
    i.Sec = L"voice";
    i.Key = key;
    i.Label = label;
    i.Def = def ? L"1" : L"0";
    i.IfKey = ifKey;
    i.IfVal = ifVal;
    i.IfWhy = why;
    return i;
}

static std::vector<Page> BuildPages()
{
    std::vector<Page> p;
    {
        Page g{L"LANCE CONTROL", L"", {}, false};
        g.Items = {
            HH(L"LANCEMATES", L"The game reads these itself - HUDDisplay and VoiceCommand do not need to be open."),
            OnSec(Ch(L"insane_allies", L"Allies", L"0",
                     {{L"0", L"Vanilla allies"}, {L"1", L"Insane allies"}, {L"2", L"Impossibly strong allies"}},
                     L"Vanilla: the game fires for your lancemates. Insane: every weapon the moment it is ready, with a human-like spread and some misses. Impossibly strong: every weapon, every shot on the aim point - cockpit kills galore."),
                  L"lance"),
            C(L"lance", L"mute_game_voices", L"Mute the game's own lancemate voices", true,
              L"For when our pilots' radio voices (VoiceCommand) speak instead. Mission dialogue (Ryana, clients) still plays."),
        };
        p.push_back(g);
    }
    {
        // 2026-10-08 (vanillahud-105): who takes each vanilla HUD panel. Ticked = HUDDisplay takes it off
        // the game's HUD onto its V- page (the default); unticked = it stays on the game's HUD. Keys: VhudHudKey.
        Page g{L"HUD", L"PANELS", {}, true};
        g.Items = {
            HH(L"PANELS ON HUDDISPLAY",
               L"Ticked: HUDDisplay takes the panel onto its V- page - it is never drawn on the game's screen, not even as a "
               L"mission starts. Unticked: the game keeps it (the V- page then says SHOWN IN GAME). Applies live. The "
               L"crosshair, markers and prompts are on the IN GAME page."),
            C(L"hud", L"target", L"Target panel (V-TGT)", true),
            C(L"hud", L"weapons", L"Weapon list (V-WPN)", true),
            C(L"hud", L"owner", L"Own mech paperdoll (V-OWN)", true),
            C(L"hud", L"heat", L"Heat meter (V-OWN)", true),
            C(L"hud", L"throttle", L"Throttle (V-OWN)", true),
            C(L"hud", L"jumpjets", L"Jump jets (V-OWN)", true),
            C(L"hud", L"objectives", L"Objective list (V-OBJ)", true),
            C(L"hud", L"primary_objective", L"Primary objective (V-OBJ)", true),
            Col(),
            H(L""),
            C(L"hud", L"alerts", L"Alerts (V-OBJ)", true),
            C(L"hud", L"dialogue", L"Mission dialogue + subtitles (V-OBJ)", true),
            C(L"hud", L"minimap", L"Minimap (V-MAP)", true),
            C(L"hud", L"comms", L"Lance status (V-COM)", true),
            C(L"hud", L"compass", L"Compass", true),
            C(L"hud", L"torso", L"Torso twist", true),
        };
        p.push_back(g);
    }
    {
        // 2026-10-08 (vanillahud-105b): the game's HUD pieces nothing captures - shown or hidden on the game's screen.
        Page g{L"HUD", L"IN GAME", {}, true};
        g.Items = {
            HH(L"SHOWN ON THE GAME'S SCREEN",
               L"Ticked: shown on the game's screen. Unticked: hidden from the moment it is created - never drawn, not even "
               L"as a mission starts. Applies live."),
            C(L"hud", L"game_crosshair", L"Crosshair", true, L"Hiding it hides the lock ring too."),
            C(L"hud", L"game_lock_ring", L"Missile lock ring", true),
            C(L"hud", L"game_target_box", L"Target box (on the targeted unit)", true),
            C(L"hud", L"game_markers", L"World markers (units, objectives, navs)", true, L"The minimap's own icons are not affected."),
            C(L"hud", L"game_hit_arrows", L"Hit direction indicator", true),
            Col(),
            H(L""),
            C(L"hud", L"game_incoming", L"Incoming fire arrows", true),
            C(L"hud", L"game_prompts", L"Interaction prompts", true),
            C(L"hud", L"game_dpad", L"D-pad / command panel", true),
            C(L"hud", L"game_detection", L"Detection status (spotted)", true),
            C(L"hud", L"game_airstrike", L"Airstrike indicators", true),
            C(L"hud", L"game_ecm", L"ECM", true),
        };
        p.push_back(g);
    }
    {
        // 2026-10-10: Enhanced Imaging moved off IN GAME onto its own page (IN GAME ran past the window).
        Page g{L"HUD", L"VISION", {}, true};
        g.Items = {
            HH(L"ENHANCED IMAGING VISION", L"The vision mode drawn over the game by EnhancedImaging.dll."),
            OnSec(Sp(K_Key, L"ei_vision_key", L"Vision mode key", L"0",
                     L"Toggles Enhanced Imaging over the game (EnhancedImaging.dll; also the E-VIS bezel action and \"activate enhanced imaging\")."),
                  L"hud"),
            C(L"hud", L"ei_vision_resume", L"Back on at the next drop", false,
              L"The vision mode switches off when you leave your mech (death, eject, mission end). Ticked: it comes back on once you pilot again."),
            OnSec(Hp(Nm(L"ei_wave_s", L"Wave duration", 1.0, 0.2, 3.0, 0.1, 1, L"s"), L"How long the wave takes to sweep out when the vision switches on."), L"hud"),
            OnSec(Hp(Nm(L"ei_wave_band_m", L"Wave fade band", 50.0, 5.0, 500.0, 5.0, 0, L"m"), L"How deep the wave's soft edge is."), L"hud"),
            // 2026-10-17 (user): each centre crosshair can be switched off; live, default on.
            HH(L"CENTRE CROSSHAIRS", L"The green cross drawn at the centre of a view. Unticked: not drawn (the game's own crosshair is unaffected)."),
            C(L"hud", L"ei_crosshair", L"Enhanced Imaging", true, L"The cross at the centre of the Enhanced Imaging vision (EnhancedImaging.dll)."),
            C(L"hud", L"vpod_crosshair", L"V-POD page", true, L"The cross on HUDDisplay's V-POD page."),
            C(L"hud", L"cap_crosshair", L"CAP zoom camera page", true, L"The cross on HUDDisplay's CAP page (the magnification label stays)."),
            Col(),
            HH(L"IN-GAME EFFECT (.pak)", L"Used when the EnhancedImaging mod (.pak) is installed and enabled in the game's MODS menu."),
            C(L"hud", L"ei_pak", L"Use the EnhancedImaging .pak", true,
              L"When the EnhancedImaging mod is installed, the scene edges, units and wave are drawn inside the game (exact, VR-ready). Unticked or not installed: the current method."),
            OnSec(Ch(L"ei_pak_layer", L"Layering", L"under",
                     {{L"under", L"Everything under the vision"}, {L"over", L"Effects, glass and MWVR HUD over it"}},
                     L".pak only, live. Under: the vision covers the whole scene, the exact look (After Tonemapping). Over: explosions, smoke, cockpit glass and MWVR's in-world HUD are drawn on top of it (Before Translucency) - weather colour grading (UltraVisualAndWeather) then tints the vision."),
                  L"hud"),
            OnSec(Ch(L"ei_pak_attach", L"Effect applies to", L"camera",
                     {{L"camera", L"The player camera only"}, {L"world", L"The whole world"}},
                     L".pak only. Camera: your view only (not CAP or MechWarriorVR's camera screens). World: every view, including those - use only if Camera shows nothing."),
                  L"hud"),
            OnSec(Hp(Nm(L"ei_edge_gain", L"Edge brightness", 1.0, 0.2, 3.0, 0.1, 1, L"x"), L"Brightness of the scene edges."), L"hud"),
            OnSec(Hp(Nm(L"ei_units_scale", L"Units capture size", 0.6, 0.25, 2.0, 0.05, 2, L"x"),
                     L"Resolution of the unit outlines' capture: bigger is sharper but costs more. 1.0 = your screen; "
                     L"above 1.0 smooths the outlines further (2.0 draws the units at 4x the pixels - GPU cost)."), L"hud"),
        };
        p.push_back(g);
    }
    {
        // 2026-10-12: HUDDisplay's heading compass (gs::CompassOpts). Each window's CMPS bezel button hides it there.
        Page g{L"HUD", L"COMPASS", {}, true};
        g.Items = {
            HH(L"HEADING COMPASS", L"For heading orders (\"two, heading two-seven-zero, five hundred metres\"). Applies live to every "
                                   L"HUDDisplay window; a window's CMPS bezel button hides the compass on that window."),
            C(L"hud", L"compass_strip", L"Heading strip on the 3D and TacOps maps", true,
              L"Our strip along the top, turning with your torso, in place of the game's captured compass."),
            C(L"hud", L"compass_rings", L"Compass rings on the 3D and TacOps maps", true,
              L"A compass ring on the ground around you at 1 km (N in amber), with range rings at 250 and 500 m."),
            C(L"hud", L"compass_clock", L"Clock positions on the 3D and TacOps maps", true,
              L"12 / 3 / 6 / 9 around you, from where your torso faces - for \"move to my 3 o'clock\"."),
            C(L"hud", L"compass_lidar", L"Compass ring on the LIDAR scope", true,
              L"Degrees round the scope's edge, turning with your mech, and your heading at the top."),
            C(L"hud", L"compass_lidar_clock", L"Clock positions on the LIDAR scope", true),
            C(L"hud", L"compass_dots", L"Bearing dots", true,
              L"Green dots for lancemates and red for enemies: under the heading strip, and round the LIDAR scope's rim."),
        };
        p.push_back(g);
    }
    {
        // 2026-10-13 (user): the incoming-missile alarm (gs::AlarmOpts) - was per HUDDisplay profile.
        Page g{L"HUD", L"ALARM", {}, true};
        g.Items = {
            HH(L"INCOMING MISSILE ALARM", L"The warning tone and the INCOMING MISSILE cue when a missile is locked on you. Applies live "
                                          L"to every HUDDisplay window."),
            C(L"hud", L"alarm_on", L"Missile alarm", true, L"Off: no tone and no on-screen cue."),
            OnSec(Hp(Nm(L"alarm_vol", L"Alarm volume", 70, 0, 100, 5, 0, L"%"), L"Loudness of the warning tone."), L"hud"),
            OnSec(Ch(L"alarm_sound", L"Alarm sound", L"0",
                     {{L"0", L"Warble"}, {L"1", L"Double pip"}, {L"2", L"Triple pip"}, {L"3", L"Fast warble"},
                      {L"4", L"Single beep (original)"}},
                     L"Warble: two alternating tones. Double pip / triple pip: quick high beeps. Fast warble: a rapid, "
                     L"insistent trill. Single beep: the first alarm. Every sound repeats slowly while the missile is far "
                     L"and fast in the last 250 m."),
                  L"hud"),
            Act(L"TEST ALARM", A_TestAlarm, L"Plays the chosen sound at the chosen volume: 2 s as for a far missile, then 2 s "
                                            L"as for one in the last 250 m."),
        };
        p.push_back(g);
    }
    {
        // 2026-10-13 (user): HUDDisplay's LIDAR layers + smoothing (gs::LidarOpts) - were per profile in HUDDisplay's menu.
        Page g{L"HUD", L"LIDAR", {}, true};
        g.Items = {
            HH(L"LIDAR SCOPE", L"What HUDDisplay's LIDAR scope draws. Applies live to every HUDDisplay window. The range is "
                               L"each window's own zoom (its ZM+ / ZM- buttons or its menu)."),
            C(L"hud", L"lidar_rings", L"Range rings", true),
            C(L"hud", L"lidar_ping", L"Ping sweep", true),
            C(L"hud", L"lidar_aim", L"Torso aim line", true),
            C(L"hud", L"lidar_mechs", L"Mechs", true),
            C(L"hud", L"lidar_vehicles", L"Vehicles", true),
            C(L"hud", L"lidar_weight", L"Weight dots", true, L"A dot per mech showing its weight class."),
            C(L"hud", L"lidar_target", L"Target highlight", true, L"Marks your locked target."),
            C(L"hud", L"lidar_projectiles", L"Projectiles", false, L"Rounds in flight."),
            C(L"hud", L"lidar_missiles", L"Missiles", false, L"Missiles in flight."),
            C(L"hud", L"lidar_terrain", L"Terrain", true, L"The ground's contour lines under the scope."),
            C(L"hud", L"lidar_smooth", L"Smooth projectiles", true,
              L"Moves rounds smoothly between updates instead of in steps. A small cost per round."),
        };
        p.push_back(g);
    }
    {
        // 2026-10-12: radar mode (gs::RadarOpts) - which hostiles HUDDisplay shows. Display only.
        Page g{L"HUD", L"RADAR", {}, true};
        g.Items = {
            HH(L"RADAR MODE", L"Which enemies HUDDisplay's LIDAR scope, 3D / TacOps maps and compass dots show. Applies live "
                              L"to every HUDDisplay window. The game itself is not changed."),
            OnSec(Ch(L"radar_mode", L"Radar mode", L"0",
                     {{L"0", L"Full LIDAR"}, {L"1", L"Vanilla"}, {L"2", L"Vanilla, 5 km"}},
                     L"Full LIDAR: every enemy within 1 km. Vanilla: only enemies the game's own detection has - on your "
                     L"radar (including what your lance relays), on a lancemate's radar, or in a lancemate's sight. Needs "
                     L"LanceControl; without its feed everything is shown. An enemy that drops off stays as a grey "
                     L"last-known mark on the 3D / TacOps maps. Vanilla, 5 km: the same rules, with the LIDAR scope, maps, "
                     L"terrain and buildings out to 5 km; past 1 km only rounds fired by a detected enemy or within 500 m "
                     L"of a lancemate are shown. Costs more game time, only while selected."),
                  L"hud"),
        };
        p.push_back(g);
    }
    {
        Page g{L"VOICE COMMAND", L"COMMS", {}, true};
        g.Items = {
            H(L"TALKING"),
            C(L"voice", L"speak", L"Pilots speak", true, L"Off: replies only appear as text in VoiceCommand."),
            C(L"voice", L"callouts", L"Pilots report on their own", true, L"Which reports: the CALLOUTS tab."),
            If(Hp(Nm(L"callout_gap", L"Time between callouts", 6, 0, 60, 1, 0, L"s"),
                  L"The quiet time after anything said before the next ordinary report. Urgent ones (a lancemate down...) never wait."),
               L"callouts", L"1", L"(callouts are off)"),
            HH(L"LISTENING FOR ORDERS", L"How well it hears you: VOICE COMMAND > ENGINE."),
            Ch(L"listen", L"Listen", L"ptt", {{L"always", L"Always"}, {L"ptt", L"Push-to-talk only"}},
               L"Push-to-talk (the default) needs the key below - nothing is heard until one is set."),
            Sp(K_Key, L"ptt_vk", L"Push-to-talk key", L"0",
               L"Hold it while you speak an order. HOTAS / controller: map a button to this key in your stick software."),
            Col(),
            HH(L"WORD LISTS", L"Open in Notepad. Saved changes are used at once."),
            Sp(K_Open, L"VoiceLines.txt", L"EDIT PILOT LINES", L"", L"What the pilots say."),
            Sp(K_Open, L"Pronunciations.txt", L"FIX PRONUNCIATION", L"", L"Words the voices say wrong."),
            Sp(K_Open, L"HeardAs.txt", L"FIX MISHEARD WORDS", L"", L"Words it hears wrong."),
        };
        p.push_back(g);
    }
    {
        // Defaults as VoiceCommand's old "Normal" chatter level (VoiceCommand writes the real ones the first time).
        Page g{L"VOICE COMMAND", L"CALLOUTS", {}, true};
        g.Items = {
            C(L"voice", L"callouts", L"Pilots report on their own (all of these)", true, L"Off: none of these are said."),
            H(L"ABOUT THEMSELVES AND THE LANCE"),
            T(L"say_lancemate_down", L"A lancemate goes down", true),
            T(L"say_part_lost", L"Lost an arm, leg or torso", true),
            T(L"say_critical", L"Critical damage", true),
            T(L"say_shredded", L"Badly damaged", true),
            T(L"say_hit_hard", L"Hit hard", true),
            T(L"say_taking_fire", L"Taking fire", true),
            T(L"say_ally_trouble", L"Another lancemate in trouble", true),
            T(L"say_overheat", L"Overheating", true),
            T(L"say_ammo_low", L"Ammo low", true),
            T(L"say_ammo_out", L"Out of ammo", true),
            T(L"say_weapon_lost", L"A weapon destroyed", true),
            T(L"say_in_position", L"In position (after a move order)", true),
            T(L"say_line_of_fire", L"\"You're in my line of fire\"", true),
            T(L"say_flanked", L"An enemy close behind them", true),
            Col(),
            N(L""), // one row: level with the left list, below its switch
            H(L"ABOUT THE FIGHT"),
            T(L"say_kills", L"Kills", true),
            T(L"say_kill_tally", L"   ... and their kill count", true, L"say_kills", L"1", L"(Kills is off)"),
            T(L"say_spotted", L"Enemy spotted", false),
            T(L"say_heavy_contact", L"Heavy enemy contact", true),
            T(L"say_air", L"Enemy aircraft", true),
            T(L"say_turrets", L"Turrets", true),
            T(L"say_missiles", L"Missiles coming at you", true),
            T(L"say_missile_shooter", L"   ... and who fired them", false, L"say_missiles", L"1", L"(Missiles is off)"),
            T(L"say_you_hit", L"You're taking hits", true),
            T(L"say_target_damage", L"Your target's damage", true),
            T(L"say_outnumbered", L"Outnumbered", true),
            T(L"say_last_enemy", L"Last enemy left", true),
            T(L"say_area_clear", L"Area clear", true),
            T(L"say_uav", L"UAV launched", true),
        };
        p.push_back(g);
    }
    {
        Page g{L"VOICE COMMAND", L"VOICES", {}, true};
        g.Items = {
            HH(L"STANDARD VOICES", L"Used until a pilot has a voice of its own. Speed belongs to the voice: rows using the same voice change together."),
            If(Sp(K_Voice, L"kokoro_voice_0", L"Whole lance at once", kKokoroDefault[0]), L"tts", L"kokoro", L"(Kokoro voices only)"),
            If(Sp(K_Voice, L"kokoro_voice_2", L"Lancemate 2", kKokoroDefault[2]), L"tts", L"kokoro", L"(Kokoro voices only)"),
            If(Sp(K_Voice, L"kokoro_voice_3", L"Lancemate 3", kKokoroDefault[3]), L"tts", L"kokoro", L"(Kokoro voices only)"),
            If(Sp(K_Voice, L"kokoro_voice_4", L"Lancemate 4", kKokoroDefault[4]), L"tts", L"kokoro", L"(Kokoro voices only)"),
            HH(L"PILOTS' OWN VOICES", L"Each pilot keeps its voice in every mission. FORGET: that pilot gets a new voice next time."),
            Sp(K_Pilots, L"", L"", L""),
        };
        p.push_back(g);
    }
    std::vector<Opt> cues = {{L"beep", L"Beep"}, {L"chirp", L"Chirp"}, {L"squelch", L"Squelch"}, {L"file", L"Own sound file"}, {L"none", L"None"}};
    {
        Page g{L"VOICE COMMAND", L"RADIO", {}, true};
        g.Items = {
            H(L"RADIO"),
            C(L"voice", L"radio", L"Radio effect", true, L"Off: clean voices, no static."),
            Pc(L"radio_volume", L"Voice volume", 0.9, 0, 1),
            Pc(L"radio_cue_volume", L"Beep volume", 0.6, 0, 1),
            Ch(L"radio_start", L"Start beep", L"beep", cues, L"", true),
            Ch(L"radio_end", L"End beep", L"beep", cues, L"Own sound: radio_start.wav / radio_end.wav.", true),
            Nm(L"radio_gap", L"Pause after a message", 0.35, 0, 3, 0.05, 2, L"s"),
            Act(L"TEST TRANSMISSION", A_TestRadio, L"Lancemate 2 says a line through the radio."),
            Col(),
            HH(L"FINE-TUNING THE SOUND", L"Optional. Use TEST TRANSMISSION (left) to hear changes."),
            Pc(L"radio_static", L"Static hiss", 0.35, 0, 1),
            Nm(L"radio_crackle", L"Loud pops", 1.5, 0, 20, 0.5, 1, L"a sec"),
            Pc(L"radio_fry", L"Fizz", 0.5, 0, 1),
            Pc(L"radio_wash", L"Static waves", 0.5, 0, 1),
            Pc(L"radio_whistle", L"Whistle", 0.3, 0, 1),
            Pc(L"radio_hum", L"Engine buzz", 0, 0, 1),
            Nm(L"radio_dropout", L"Signal dropouts", 0.4, 0, 5, 0.1, 1, L"a sec"),
            Nm(L"radio_band_low", L"Tone: lowest pitch", 250, 50, 1000, 10, 0, L"Hz"),
            Nm(L"radio_band_high", L"Tone: highest pitch", 3600, 1500, 8000, 100, 0, L"Hz"),
            Nm(L"radio_presence", L"Voice clarity", 4, 0, 12, 0.5, 1),
            Hp(Pc(L"radio_limit", L"Compression", 0.45, 0.1, 1, true), L"Higher compression: louder, flatter voices."),
        };
        p.push_back(g);
    }
    {
        Page g{L"VOICE COMMAND", L"ENGINE", {}, true};
        g.Items = {
            H(L"HEARING YOU"),
            Ch(L"recognizer", L"Recognition", L"whisper", {{L"whisper", L"Whisper"}, {L"windows", L"Windows"}},
               L"Whisper: free speech. Windows: set phrases only.", true),
            If(Sp(K_Whisper, L"whisper_model", L"Whisper model", gs::kDefWhisperModel, L"Bigger models: more accurate, slower.", true),
               L"recognizer", L"whisper", L"(Whisper only)"),
            If(Nm(L"whisper_threads", L"Whisper: CPU cores", 4, 1, 16, 1, 0, L"", true), L"recognizer", L"whisper", L"(Whisper only)"),
            // 2026-10-14 (user): Whisper on the graphics card (Vulkan) - off by default, the card is the game's
            If(C(L"voice", L"whisper_gpu", L"Whisper: use the graphics card", false,
                 L"Faster, but shares the card with the game - check VR stays smooth.", true),
               L"recognizer", L"whisper", L"(Whisper only)"),
            If(C(L"voice", L"whisper_trim", L"Skip silence (faster)", true), L"recognizer", L"whisper", L"(Whisper only)"),
            // 2026-10-15 (user, overnight round trip C2): on by default
            If(C(L"voice", L"callsign_guess", L"Guess a clipped callsign", true,
                 L"Fast speech can clip the callsign (\"Do\" for Two, \"Re\" for Three). Only used when the rest is an order.", true),
               L"recognizer", L"whisper", L"(Whisper only)"),
            If(Ch(L"min_confidence", L"How sure it must be", L"1", {{L"0", L"Any"}, {L"1", L"Normal"}, {L"2", L"Very"}}),
               L"recognizer", L"windows", L"(Windows only)"),
            Col(),
            H(L"PILOT VOICES"),
            Ch(L"tts", L"Voices", L"kokoro", {{L"kokoro", L"Kokoro"}, {L"windows", L"Windows voices"}},
               L"Kokoro: lifelike voices from a voice pack.", true),
            If(Sp(K_Kokoro, L"kokoro_dir", L"Kokoro voice pack folder", gs::kDefKokoroDir, L"A folder with model.onnx and voices.bin.", true), L"tts",
               L"kokoro", L"(Kokoro only)"),
            If(Nm(L"kokoro_threads", L"Kokoro: CPU cores", 2, 1, 8, 1, 0, L"", true), L"tts", L"kokoro", L"(Kokoro only)"),
            If(C(L"voice", L"english_only", L"English voices only", true, L"", true), L"tts", L"windows", L"(Windows voices only)"),
            If(Hp(Nm(L"rate", L"Windows voice speed", 1, -10, 10, 1, 0, L"", true), L"-10 slow ... 10 fast."), L"tts", L"windows",
               L"(Windows voices only)"),
        };
        p.push_back(g);
    }
    {
        // 2026-10-17 (user): the paperdoll pictures come from the player's own game files - extracted ONLY here, when
        // the player presses EXTRACT (never automatically). HUDDisplay and Enhanced Imaging just read the folder.
        Page g{L"PAPERDOLLS", L"", {}, false};
        g.Items = {
            HH(L"PAPERDOLLS", L"The unit pictures on the TacOps map, the 3D page and Enhanced Imaging come from your own copy of "
                              L"the game - nothing of the game ships with this suite. Press EXTRACT once, and again after a game "
                              L"update or when you add mods. It only READS the game's .pak files (changes nothing) and takes a "
                              L"few seconds. Until then the labels show plain outlines."),
            Live(L"pd_status"),
            Live(L"pd_game"),
            Live(L"pd_cache"),
            N(L""),
            C(L"paperdolls", L"include_mods", L"Include mod paks", true,
              L"Also read the paks of your mods (the game's Mods folder and the Steam workshop), so mod mechs and vehicles get "
              L"their pictures. The base game is read first and wins a name clash."),
            Act(L"EXTRACT / REBUILD", A_PdExtract, L"Reads the paperdoll pictures out of the game's .pak files into the folder "
                                                  L"shown above. Replaces what was there. The game may be running."),
            Act(L"GAME FOLDER...", A_PdBrowse, L"Only needed when the game folder is not found by itself (it is looked up "
                                               L"through Steam). Pick the folder that holds MW5Mercs."),
            Act(L"OPEN FOLDER", A_PdOpen, L"Opens the paperdoll folder in Explorer."),
        };
        p.push_back(g);
    }
    {
        // 2026-10-12 (lancecontrol-41): debug probes, read by LanceControl straight from the ini. Both off by default.
        Page g{L"DEBUG", L"", {}, false};
        g.Items = {
            HH(L"DEBUG PROBES", L"For testing and bug reports only. They cost game-thread time and write large logs, so leave "
                                L"them off for normal play. LanceControl picks a change up within a second - no restart."),
            C(L"debug", L"ai_probe", L"AI probe (ai_probe.log)", false,
              L"Logs everything the lancemates' AI does - targets, orders, behaviour tree, weapons, heat - to ai_probe.log "
              L"beside the game log. A few MB per mission and about 3 ms of game-thread time four times a second."),
            C(L"debug", L"aim_probe", L"Aim-writer probe", false,
              L"While a vanilla lancemate's called shot runs: logs each time the game puts its own aim back, and once per "
              L"mission lists (and hooks, where possible) the game functions that set the aim. Lines in the game log, "
              L"'aim probe:'."),
            C(L"debug", L"verbose_log", L"Detailed logging (profiles, periodic status)", false,
              L"Adds the periodic timing profiles and status summaries (LIDAR scan, projectiles, lance feed, insane allies, "
              L"HUD copies) to the game log - for testing and bug reports. Off: the log keeps events, warnings and 'slow:' "
              L"lines only."),
        };
        p.push_back(g);
    }
    return p;
}

// ---- the app --------------------------------------------------------------------------
struct Hit
{
    D2D1_RECT_F R;
    int Id, A, B;
};

enum HitId
{
    HI_None, HI_Tab, HI_Sub, HI_Check, HI_Minus, HI_Plus, HI_Drop, HI_DropPick, HI_Rebind, HI_Clear, HI_Browse, HI_Open,
    HI_PilotDrop, HI_PilotDel, HI_PilotTest, HI_PilotPage, HI_Test, HI_Action, HI_Reset, HI_StartVc, HI_VSpeed
};

class App
{
public:
    HWND                          Wnd = nullptr;
    ComPtr<ID2D1Factory>          D2d;
    ComPtr<ID2D1HwndRenderTarget> Rt;
    ComPtr<ID2D1SolidColorBrush>  Br;
    ComPtr<IDWriteFactory>        Dw;
    ComPtr<IDWriteTextFormat>     Fmt, FmtC, FmtTitle, FmtWrap; // FmtWrap: the "?" boxes (word-wrapped, top-aligned)
    // 2026-10-10: the "?" marks drawn this frame - where each is and what it explains (shown on hover)
    struct Tip
    {
        D2D1_RECT_F  R;
        std::wstring S;
    };
    std::vector<Tip>              Tips;
    std::vector<Page>             Pages;
    std::vector<std::wstring>     Tabs;
    int                           Tab = 0, Sub = 0; // Sub: index into Pages of the open page
    std::vector<Hit>              Hits;
    // TEST ALARM (2026-10-13): the alarm's own synthesiser - 2 s far, 2 s close, topped up by timer 2
    ThreatAlarm                   AlarmTest;
    int                           AlarmPhase = 0; // 0 idle, 1 far, 2 close
    ULONGLONG                     AlarmStartMs = 0;
    void AlarmTestStart()
    {
        gs::AlarmOpts o;
        o.Load();
        AlarmTest.SetVolume(o.Vol);
        AlarmTest.SetSound(o.Sound);
        AlarmTest.Preview(false);
        AlarmPhase = 1;
        AlarmStartMs = GetTickCount64();
        SetTimer(Wnd, 2, 50, nullptr);
    }
    void AlarmTestTick()
    {
        const ULONGLONG t = GetTickCount64() - AlarmStartMs;
        if (AlarmPhase == 1 && t >= 2000)
        {
            AlarmTest.Preview(true);
            AlarmPhase = 2;
        }
        else if (AlarmPhase == 2 && t >= 4000)
        {
            AlarmTest.PreviewStop();
            AlarmPhase = 0;
            KillTimer(Wnd, 2);
            return;
        }
        AlarmTest.PreviewTick();
    }
    float                         Mx = -1, My = -1;
    bool                          Tracking = false;
    float                         Sc = 1, Ox = 0, Oy = 0;
    static constexpr float        kW = 780.0f, kH = 560.0f;
    static constexpr float        kColW = 360.0f, kLabelW = 186.0f, kCtlX = 190.0f, kCtlW = 170.0f;
    // the open dropdown
    int                           DropItem = -1; // item index (or 1000 + pilot row)
    std::vector<Opt>              DropOpts;
    D2D1_RECT_F                   DropAt{};
    int                           DropScroll = 0;
    // key capture
    int                           Capturing = -1;
    // two-click confirmations: what is armed, and when
    int                           ArmedId = 0, ArmedA = -1;
    ULONGLONG                     ArmedMs = 0;
    std::wstring                  Status;
    bool                          StatusWarn = false;
    ULONGLONG                     StatusMs = 0;
    gs::Watch                     IniWatch{gs::Path()};
    gs::Watch                     PvWatch{gs::Dir() + L"PilotVoices.txt"};
    std::vector<std::pair<std::wstring, std::wstring>> Pilots; // PilotVoices.txt
    int                           PilotTop = 0;
    bool                          VoiceHeads = false;      // the VOICES table's column titles are drawn
    std::wstring                  PilotArmed, PilotDrop;   // the pilot a FORGET confirmation / an open dropdown is for (by name)
    std::map<std::wstring, std::wstring> Cache;            // "sec/key" -> value, emptied when the file changes
    bool                          VcRunning = false;

    // ---- values ----
    std::wstring Get(const wchar_t *sec, const wchar_t *key, const std::wstring &def)
    {
        const std::wstring ck = std::wstring(sec) + L"/" + key;
        auto it = Cache.find(ck);
        if (it == Cache.end())
            it = Cache.emplace(ck, gs::Str(sec, key, L"\x01")).first;
        return it->second == L"\x01" ? def : it->second;
    }
    // release (2026-10-08): the model paths may be relative to the exe's folder - shown and checked resolved
    std::wstring Val(const Item &it)
    {
        const std::wstring v = Get(it.Sec, it.Key, it.Def);
        return (it.K == K_Whisper || it.K == K_Kokoro) ? gs::Resolve(v) : v;
    }
    double NumVal(const Item &it)
    {
        const std::wstring v = Val(it);
        const double d = v.empty() ? _wtof(it.Def.c_str()) : _wtof(v.c_str());
        return std::max(it.Lo, std::min(it.Hi, d));
    }
    void Write(const wchar_t *sec, const wchar_t *key, const std::wstring &v)
    {
        gs::Set(sec, key, v);
        Cache.erase(std::wstring(sec) + L"/" + key);
    }
    void Saved(const Item &it)
    {
        if (it.Restart && VcRunning)
            Note(L"Saved. VoiceCommand uses this after a restart: close its window, then press START.", true);
        else if (it.Restart)
            Note(L"Saved. Used the next time VoiceCommand starts.");
        else
            Note(L"Saved - in use now.");
    }
    void Put(const Item &it, const std::wstring &v)
    {
        Write(it.Sec, it.Key, v);
        Saved(it);
    }
    void Note(const std::wstring &s, bool warn = false)
    {
        Status = s;
        StatusWarn = warn;
        StatusMs = GetTickCount64();
    }
    // An item that only applies when another setting has a value.
    bool Applies(const Item &it)
    {
        if (!it.IfKey)
            return true;
        const std::wstring k = it.IfKey;
        const std::wstring def = k == L"tts" ? L"kokoro" : k == L"recognizer" ? L"whisper" : (k == L"callouts" || k == L"say_kills" || k == L"say_missiles") ? L"1" : L"";
        return _wcsicmp(Get(L"voice", it.IfKey, def).c_str(), it.IfVal) == 0;
    }
    bool Armed(int id, int a) const { return ArmedId == id && ArmedA == a && GetTickCount64() - ArmedMs < 3000; }
    void Arm(int id, int a)
    {
        ArmedId = id;
        ArmedA = a;
        ArmedMs = GetTickCount64();
    }
    bool Natural() { return _wcsicmp(Get(L"voice", L"tts", L"kokoro").c_str(), L"windows") != 0; }
    bool CanTest() { return VcRunning && Natural() && _wtoi(Get(L"voice", L"speak", L"1").c_str()) != 0; }

    // ---- other programs ----
    static bool VoiceCommandRunning()
    {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\MW5_VoiceCommand");
        if (m)
        {
            CloseHandle(m);
            return true;
        }
        return false;
    }
    void Launch(const std::wstring &exe, const std::wstring &args)
    {
        const std::wstring path = gs::Dir() + exe;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Note(exe + L" is not in this folder.", true);
            return;
        }
        const HINSTANCE r = ShellExecuteW(Wnd, L"open", path.c_str(), args.empty() ? nullptr : args.c_str(), gs::Dir().c_str(), SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32)
            Note(L"Could not start " + exe + L".", true);
        else
            Note(L"Starting " + exe + L"...");
    }
    // Ask a running VoiceCommand to speak (GlobalSettings.ini [test]: it watches the file).
    void Test(int slot, const std::wstring &voice, const std::wstring &text)
    {
        if (!CanTest())
        {
            Note(VcRunning ? L"Testing needs natural voices (ENGINE) and \"Pilots speak\" on (COMMS)."
                           : L"VoiceCommand is not running - press START (top right), then TEST again.",
                 true);
            return;
        }
        gs::Set(L"test", L"slot", std::to_wstring(slot));
        gs::Set(L"test", L"voice", voice);
        gs::Set(L"test", L"text", text);
        gs::Set(L"test", L"seq", std::to_wstring(GetTickCount64() & 0x7FFFFFFF)); // last: VoiceCommand reacts to it
        Note(L"Test sent - listen for it.");
    }

    // ---- PilotVoices.txt (VoiceCommand's format: "Name = voice", # comments) ----
    void LoadPilots()
    {
        Pilots.clear();
        FILE *f = nullptr;
        if (_wfopen_s(&f, (gs::Dir() + L"PilotVoices.txt").c_str(), L"rb") != 0 || !f)
            return;
        std::string raw;
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0)
            raw.append(b, n);
        fclose(f);
        std::wstring w;
        const int k = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
        if (k > 0)
        {
            w.resize((size_t)k);
            MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &w[0], k);
        }
        auto trim = [](const std::wstring &t) {
            const size_t a = t.find_first_not_of(L" \t\r\n"), z = t.find_last_not_of(L" \t\r\n");
            return a == std::wstring::npos ? std::wstring() : t.substr(a, z - a + 1);
        };
        size_t pos = 0;
        while (pos <= w.size())
        {
            size_t e = w.find(L'\n', pos);
            if (e == std::wstring::npos)
                e = w.size();
            const std::wstring line = trim(w.substr(pos, e - pos));
            pos = e + 1;
            if (line.empty() || line[0] == L'#')
                continue;
            const size_t eq = line.find(L'=');
            if (eq != std::wstring::npos)
                Pilots.push_back({trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
        }
    }
    // A change to one pilot, made on the file as it is NOW (VoiceCommand may have added a pilot since it
    // was last read): re-read, change that pilot by name, write.
    bool EditPilot(const std::wstring &name, const std::wstring *voice)
    {
        LoadPilots();
        for (size_t k = 0; k < Pilots.size(); ++k)
            if (Pilots[k].first == name)
            {
                if (voice)
                    Pilots[k].second = *voice;
                else
                    Pilots.erase(Pilots.begin() + (long)k);
                SavePilots();
                return true;
            }
        Note(name + L" is no longer in the list.", true);
        return false;
    }
    void SavePilots()
    {
        std::wstring out = L"# PilotVoices.txt - which voice each pilot speaks with, kept across missions.\r\n"
                           L"# Written by VoiceCommand the first time it hears of a pilot; edited in GlobalSettings.\r\n"
                           L"# Name = Kokoro voice (af_*, am_*, bf_*, bm_*). Delete a line to let that pilot be given a voice again.\r\n";
        for (auto &p : Pilots)
            out += p.first + L" = " + p.second + L"\r\n";
        const int k = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), nullptr, 0, nullptr, nullptr);
        std::string u((size_t)std::max(0, k), '\0');
        if (k > 0)
            WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), &u[0], k, nullptr, nullptr);
        FILE *f = nullptr;
        if (_wfopen_s(&f, (gs::Dir() + L"PilotVoices.txt").c_str(), L"wb") == 0 && f)
        {
            fwrite(u.data(), 1, u.size(), f);
            fclose(f);
            Note(L"Saved - in use now.");
        }
        else
            Note(L"PilotVoices.txt could not be written.", true);
        PvWatch.Stamp = gs::FileStamp(PvWatch.File); // our own write is not news
    }

    // ---- choices that come from disk ----
    static std::vector<Opt> VoiceOpts(bool withDefault)
    {
        std::vector<Opt> o;
        if (withDefault)
            o.push_back({L"", L"(standard)"});
        for (auto &v : kVoices)
            o.push_back({v.Code, v.Label});
        return o;
    }
    static std::wstring VoiceCode(const std::wstring &v)
    {
        if (!v.empty() && iswdigit(v[0]))
        {
            const int id = _wtoi(v.c_str());
            if (id >= 0 && id < kVoiceCount)
                return kVoices[id].Code;
        }
        return v;
    }
    static std::wstring VoiceLabel(const std::wstring &v)
    {
        if (v.empty())
            return L"(standard)";
        const std::wstring c = VoiceCode(v);
        for (auto &x : kVoices)
            if (_wcsicmp(x.Code, c.c_str()) == 0)
                return x.Label;
        if (iswdigit(v[0]))
            return L"Voice " + v + L" (not English)";
        return v;
    }
    // The voice a row speaks with: its own, or VoiceCommand's standard for that number.
    std::wstring RowVoice(const Item &it)
    {
        std::wstring v = VoiceCode(Val(it));
        if (v.empty())
        {
            const int slot = wcslen(it.Key) > 13 ? std::max(0, std::min(8, _wtoi(it.Key + 13))) : 0;
            v = kKokoroDefault[slot];
        }
        return v;
    }
    // A voice's own speed (kokoro_speed_<voice>), else the old one-for-all kokoro_speed, else 100 %.
    double SpeedOf(const std::wstring &code)
    {
        const double all = _wtof(Get(L"voice", L"kokoro_speed", L"1.0").c_str());
        std::wstring v = Get(L"voice", (L"kokoro_speed_" + code).c_str(), L"");
        double d = v.empty() ? all : _wtof(v.c_str());
        if (d < 0.5 || d > 2.0)
            d = 1.0;
        return d;
    }
    // A voice row: [ who | voice v | - 100% + | TEST ] (and FORGET for pilots).
    void VoiceRow(float x, float y, const std::wstring &who, const std::wstring &voiceShown, const std::wstring &code, int dropId,
                  int rowA, int testId, bool on, int speedA)
    {
        Text(x, y, 196.0f, 22.0f, who, on ? UiCol::PanelText : UiCol::PanelDim);
        DropBox(x + 200.0f, y, 210.0f, voiceShown, dropId, rowA, on);
        bool known = false; // VoiceCommand only has per-voice speeds for the named (English) voices
        for (auto &kv : kVoices)
            known = known || _wcsicmp(kv.Code, code.c_str()) == 0;
        const double sp = known ? SpeedOf(code) : 1.0;
        wchar_t b[16];
        swprintf(b, 16, L"%d%%", (int)std::lround(sp * 100.0));
        Button(x + 420.0f, y, 26.0f, 22.0f, L"-", HI_VSpeed, speedA, -1, false, on && known && sp > 0.5 + 1e-9);
        Text(x + 448.0f, y, 56.0f, 22.0f, b, on ? UiCol::PanelText : UiCol::PanelDim, 1.0f, true);
        Button(x + 506.0f, y, 26.0f, 22.0f, L"+", HI_VSpeed, speedA, 1, false, on && known && sp < 2.0 - 1e-9);
        Button(x + 542.0f, y, 52.0f, 22.0f, L"TEST", testId, rowA, 0, false, on);
    }

    static std::wstring FolderOf(const std::wstring &p)
    {
        const size_t k = p.find_last_of(L"\\/");
        return k == std::wstring::npos ? std::wstring() : p.substr(0, k + 1);
    }
    static std::wstring NameOf(const std::wstring &p)
    {
        std::wstring s = p;
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/'))
            s.pop_back();
        const size_t k = s.find_last_of(L"\\/");
        return k == std::wstring::npos ? s : s.substr(k + 1);
    }
    // Speech recognition models: the ggml-*.bin files beside the current one.
    static std::vector<Opt> WhisperOpts(const std::wstring &cur)
    {
        std::vector<Opt> o;
        const std::wstring dir = FolderOf(cur);
        WIN32_FIND_DATAW fd;
        HANDLE h = dir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"ggml*.bin").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    o.push_back({dir + fd.cFileName, fd.cFileName});
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return o;
    }
    // Voice packs: the folders beside the current one that hold model.onnx and voices.bin.
    static std::vector<Opt> KokoroOpts(const std::wstring &cur)
    {
        std::vector<Opt> o;
        std::wstring c = cur;
        while (!c.empty() && (c.back() == L'\\' || c.back() == L'/'))
            c.pop_back();
        const std::wstring parent = FolderOf(c);
        WIN32_FIND_DATAW fd;
        HANDLE h = parent.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((parent + L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                    continue;
                const std::wstring d = parent + fd.cFileName;
                if (GetFileAttributesW((d + L"\\model.onnx").c_str()) != INVALID_FILE_ATTRIBUTES &&
                    GetFileAttributesW((d + L"\\voices.bin").c_str()) != INVALID_FILE_ATTRIBUTES)
                    o.push_back({d, fd.cFileName});
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return o;
    }

    // ---- drawing helpers (HUDDisplay's menu style) ----
    void Col(UINT32 rgb, float a = 1.0f) { Br->SetColor(D2D1::ColorF(rgb, a)); }
    bool Hover(float x, float y, float w, float h) const
    {
        const float mx = (Mx - Ox) / Sc, my = (My - Oy) / Sc;
        return DropItem < 0 && mx >= x && mx < x + w && my >= y && my < y + h;
    }
    void Text(float x, float y, float w, float h, const std::wstring &s, UINT32 c, float a = 1.0f, bool centred = false,
              IDWriteTextFormat *f = nullptr)
    {
        if (!f)
            f = centred ? FmtC.Get() : Fmt.Get();
        if (!f || s.empty() || w <= 0)
            return;
        Col(c, a);
        ComPtr<IDWriteTextLayout> l;
        if (SUCCEEDED(Dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, h, &l)))
            Rt->DrawTextLayout(D2D1::Point2F(x, y), l.Get(), Br.Get());
    }
    void AddHit(float x, float y, float w, float h, int id, int a = 0, int b = 0)
    {
        Hits.push_back({D2D1::RectF(x, y, x + w, y + h), id, a, b});
    }
    // The width text takes in a format (for putting a "?" right after it).
    float TextWidth(const std::wstring &s, IDWriteTextFormat *f = nullptr)
    {
        if (!f)
            f = Fmt.Get();
        ComPtr<IDWriteTextLayout> l;
        DWRITE_TEXT_METRICS m{};
        if (s.empty() || !f || FAILED(Dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 2000.0f, 40.0f, &l)) || FAILED(l->GetMetrics(&m)))
            return 0.0f;
        return m.widthIncludingTrailingWhitespace;
    }
    // 2026-10-10: a small "?" at x (vertically centred in a 22 px row at y); hovering it shows s.
    void HelpMark(float x, float y, const std::wstring &s)
    {
        if (s.empty())
            return;
        const float cx = x + 7.0f, cy = y + 11.0f;
        const bool hov = DropItem < 0 && Capturing < 0 && Hover(x - 2.0f, y + 2.0f, 18.0f, 18.0f);
        Col(hov ? UiCol::PanelText : UiCol::PanelDim, hov ? 1.0f : 0.9f);
        Rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.5f, 6.5f), Br.Get(), hov ? 1.5f : 1.1f);
        Text(x, y + 1.0f, 14.0f, 20.0f, L"?", hov ? UiCol::PanelText : UiCol::PanelDim, 1.0f, true);
        Tips.push_back({D2D1::RectF(x - 2.0f, y + 2.0f, x + 16.0f, y + 20.0f), s});
    }
    // What an item's "?" says: why it is greyed (if it is), then its explanation.
    std::wstring TipOf(const Item &it, bool on) const
    {
        std::wstring s;
        if (!on && it.IfWhy)
            s = std::wstring(L"Not used now ") + it.IfWhy + L".";
        if (!it.Help.empty())
            s += (s.empty() ? L"" : L"\n\n") + it.Help;
        return s;
    }
    // The hovered "?" box, over everything but an open dropdown; kept inside the window.
    void DrawTip()
    {
        if (DropItem >= 0 || Capturing >= 0 || !FmtWrap)
            return;
        const float mx = (Mx - Ox) / Sc, my = (My - Oy) / Sc;
        for (auto &t : Tips)
        {
            if (!(mx >= t.R.left && mx < t.R.right && my >= t.R.top && my < t.R.bottom))
                continue;
            const float w = 320.0f, pad = 8.0f;
            ComPtr<IDWriteTextLayout> l;
            DWRITE_TEXT_METRICS m{};
            if (FAILED(Dw->CreateTextLayout(t.S.c_str(), (UINT32)t.S.size(), FmtWrap.Get(), w - 2 * pad, 600.0f, &l)) || FAILED(l->GetMetrics(&m)))
                return;
            const float h = m.height + 2 * pad;
            float x = t.R.right + 4.0f, y = t.R.top;
            if (x + w > kW - 4.0f)
                x = std::max(4.0f, t.R.left - 4.0f - w);
            if (y + h > kH - 4.0f)
                y = std::max(4.0f, kH - 4.0f - h);
            const D2D1_RECT_F box = D2D1::RectF(x, y, x + w, y + h);
            Col(UiCol::Panel);
            Rt->FillRectangle(box, Br.Get());
            Col(UiCol::PanelText);
            Rt->DrawRectangle(box, Br.Get(), 1.2f);
            Col(UiCol::PanelText);
            Rt->DrawTextLayout(D2D1::Point2F(x + pad, y + pad), l.Get(), Br.Get());
            return;
        }
    }
    void Header(float x, float y, float w, const std::wstring &s, const std::wstring &help = L"")
    {
        Text(x, y, w, 18.0f, s, UiCol::PanelDim);
        if (!help.empty())
            HelpMark(x + TextWidth(s) + 6.0f, y - 2.0f, help);
        Col(UiCol::PanelEdge, 0.8f);
        Rt->DrawLine(D2D1::Point2F(x, y + 19.0f), D2D1::Point2F(x + w, y + 19.0f), Br.Get(), 1.0f);
    }
    void Button(float x, float y, float w, float h, const std::wstring &s, int id, int a = 0, int b = 0, bool lit = false,
                bool enabled = true, UINT32 textCol = UiCol::PanelText)
    {
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
        const bool hov = enabled && Capturing < 0 && Hover(x, y, w, h);
        if (lit || hov)
        {
            Col(lit ? UiCol::CheckOn : UiCol::PanelText, lit ? (hov ? 0.40f : 0.28f) : 0.14f);
            Rt->FillRectangle(r, Br.Get());
        }
        Col(hov ? UiCol::PanelText : UiCol::PanelEdge, enabled ? 1.0f : 0.4f);
        Rt->DrawRectangle(r, Br.Get(), hov ? 1.6f : 1.2f);
        Text(x + 2.0f, y, w - 4.0f, h, s, enabled ? textCol : UiCol::PanelDim, enabled ? 1.0f : 0.6f, true);
        if (enabled && id)
            AddHit(x, y, w, h, id, a, b);
    }
    void Check(float x, float y, float w, const std::wstring &s, bool on, int a, bool enabled)
    {
        const bool hov = enabled && Capturing < 0 && Hover(x, y, w, 20.0f);
        if (hov)
        {
            Col(UiCol::PanelText, 0.10f);
            Rt->FillRectangle(D2D1::RectF(x - 2.0f, y, x + w, y + 20.0f), Br.Get());
        }
        const D2D1_RECT_F box = D2D1::RectF(x, y + 3.0f, x + 14.0f, y + 17.0f);
        Col(hov ? UiCol::PanelText : UiCol::PanelEdge, enabled ? 1.0f : 0.4f);
        Rt->DrawRectangle(box, Br.Get(), 1.4f);
        if (on)
        {
            Col(UiCol::CheckOn, enabled ? 1.0f : 0.35f);
            Rt->FillRectangle(D2D1::RectF(box.left + 3, box.top + 3, box.right - 3, box.bottom - 3), Br.Get());
        }
        Text(x + 22.0f, y, w - 24.0f, 20.0f, s, enabled ? UiCol::PanelText : UiCol::PanelDim);
        if (enabled)
            AddHit(x - 2.0f, y, w + 2.0f, 20.0f, HI_Check, a);
    }
    // A dropdown box: the current value and a small arrow.
    void DropBox(float x, float y, float w, const std::wstring &s, int id, int a, bool enabled = true)
    {
        const bool hov = enabled && Capturing < 0 && Hover(x, y, w, 22.0f);
        if (hov)
        {
            Col(UiCol::PanelText, 0.14f);
            Rt->FillRectangle(D2D1::RectF(x, y, x + w, y + 22.0f), Br.Get());
        }
        Col(hov ? UiCol::PanelText : UiCol::PanelEdge, enabled ? 1.0f : 0.4f);
        Rt->DrawRectangle(D2D1::RectF(x, y, x + w, y + 22.0f), Br.Get(), hov ? 1.6f : 1.2f);
        Text(x + 6.0f, y, w - 24.0f, 22.0f, s, enabled ? UiCol::PanelText : UiCol::PanelDim);
        const float ax = x + w - 13.0f, ay = y + 9.0f;
        Col(enabled ? UiCol::PanelText : UiCol::PanelDim);
        Rt->DrawLine(D2D1::Point2F(ax - 4, ay), D2D1::Point2F(ax, ay + 4), Br.Get(), 1.4f);
        Rt->DrawLine(D2D1::Point2F(ax, ay + 4), D2D1::Point2F(ax + 4, ay), Br.Get(), 1.4f);
        if (enabled)
            AddHit(x, y, w, 22.0f, id, a);
    }
    static std::wstring KeyName(int vk)
    {
        if (!vk)
            return L"none";
        switch (vk)
        {
        case VK_LBUTTON: return L"Left mouse";
        case VK_RBUTTON: return L"Right mouse";
        case VK_MBUTTON: return L"Middle mouse";
        case VK_XBUTTON1: return L"Mouse 4";
        case VK_XBUTTON2: return L"Mouse 5";
        default: break;
        }
        UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
        switch (vk)
        {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU:
            sc |= 0x100;
            break;
        default: break;
        }
        wchar_t b[64] = L"";
        if (GetKeyNameTextW((LONG)(sc << 16), b, 64) > 0)
            return b;
        wchar_t h[16];
        swprintf(h, 16, L"key 0x%02X", vk);
        return h;
    }
    // The label of a setting: dim when it does not apply (with why, if there is room), * when it needs a restart.
    // The label of a setting (dim when it does not apply, * when it needs a restart), with its "?" right after it.
    void Label(float x, float y, const Item &it, bool on, float w = kLabelW)
    {
        const std::wstring s = it.Label + (it.Restart ? L" *" : L"");
        const std::wstring tip = TipOf(it, on);
        const float room = tip.empty() ? w : w - 20.0f;
        Text(x, y, room, 22.0f, s, on ? UiCol::PanelText : UiCol::PanelDim);
        if (!tip.empty())
            HelpMark(x + std::min(TextWidth(s), room) + 4.0f, y, tip);
    }
    // A tick box with its "?" right after its text.
    void CheckRow(float x, float y, float w, const Item &it, int a, bool on)
    {
        const std::wstring s = it.Label + (it.Restart ? L" *" : L"");
        const std::wstring tip = TipOf(it, on);
        const float room = tip.empty() ? w : w - 20.0f;
        Check(x, y, room, s, _wtoi(Val(it).c_str()) != 0, a, on);
        if (!tip.empty())
            HelpMark(x + std::min(22.0f + TextWidth(s), room) + 4.0f, y - 1.0f, tip);
    }

    // ---- one page ----
    void DrawItems(const Page &pg)
    {
        const float x0 = 16.0f, x1 = 404.0f, top = 104.0f;
        bool twoCol = false; // a page with one column gives its help lines the whole width
        for (auto &it : pg.Items)
            twoCol = twoCol || it.K == K_Col;
        const float textW = twoCol ? kColW : kW - 32.0f;
        float x = x0, y = top;
        VoiceHeads = false;
        for (size_t i = 0; i < pg.Items.size(); ++i)
        {
            const Item &it = pg.Items[i];
            const int a = (int)i;
            const bool on = Applies(it);
            const float cx = x + kCtlX;
            switch (it.K)
            {
            case K_Col:
                x = x1;
                y = top;
                break;
            case K_Header:
                if (y > top + 6.0f)
                    y += 8.0f;
                Header(x, y, twoCol ? kColW : kW - 32.0f, it.Label, it.Help);
                y += 28.0f;
                break;
            case K_Note:
                if (it.Label.empty())
                {
                    y += 26.0f; // 2026-10-10: an empty note is a spacer one row high
                    break;
                }
                if (*it.Key) // 2026-10-17: a live line
                {
                    bool warn = false;
                    const std::wstring t = LiveText(it.Key, warn);
                    Text(x, y, textW, 18.0f, t, warn ? UiCol::Warn : UiCol::PanelText);
                    y += 22.0f;
                    break;
                }
                Text(x, y, textW, 18.0f, it.Label, UiCol::PanelDim);
                y += 18.0f;
                break;
            case K_Check:
                CheckRow(x, y, kColW, it, a, on);
                y += 26.0f;
                break;
            case K_Num:
            {
                const double v = NumVal(it);
                const double shown = it.Inv ? 1.0 - v : v;
                wchar_t b[32];
                if (it.Pct)
                    swprintf(b, 32, L"%d%%", (int)std::lround(shown * 100.0));
                else
                    swprintf(b, 32, L"%.*f%ls%ls", it.Dec, shown, it.Unit.empty() ? L"" : L" ", it.Unit.c_str());
                const bool canDown = it.Inv ? v < it.Hi - 1e-9 : v > it.Lo + 1e-9;
                const bool canUp = it.Inv ? v > it.Lo + 1e-9 : v < it.Hi - 1e-9;
                Label(x, y, it, on);
                Button(cx, y, 26.0f, 22.0f, L"-", HI_Minus, a, 0, false, on && canDown);
                Text(cx + 30.0f, y, 80.0f, 22.0f, b, on ? UiCol::PanelText : UiCol::PanelDim, 1.0f, true);
                Button(cx + 114.0f, y, 26.0f, 22.0f, L"+", HI_Plus, a, 0, false, on && canUp);
                y += 26.0f;
                break;
            }
            case K_Choice:
            {
                const std::wstring v = Val(it);
                std::wstring shown = v;
                for (auto &o : it.Opts)
                    if (_wcsicmp(o.Value.c_str(), v.c_str()) == 0)
                        shown = o.Label;
                Label(x, y, it, on);
                DropBox(cx, y, kCtlW, shown, HI_Drop, a, on);
                y += 26.0f;
                break;
            }
            case K_Voice:
            {
                if (!VoiceHeads)
                {
                    VoiceHeads = true; // column titles, once, above the first row
                    Text(x + 200.0f, y - 6.0f, 210.0f, 16.0f, L"VOICE", UiCol::PanelDim, 0.9f);
                    Text(x + 420.0f, y - 6.0f, 112.0f, 16.0f, L"SPEED (per voice)", UiCol::PanelDim, 0.9f, true);
                    y += 12.0f;
                }
                VoiceRow(x, y, it.Label, VoiceLabel(Val(it)), RowVoice(it), HI_Drop, a, HI_Test, on, a);
                y += 26.0f;
                break;
            }
            case K_Tick:
                CheckRow(x, y, kColW, it, a, on);
                y += 22.0f;
                break;
            case K_Whisper:
            case K_Kokoro:
            {
                // two lines: the label, then the choice across the column with BROWSE
                const std::wstring v = Val(it);
                Label(x, y, it, on, kColW);
                y += 22.0f;
                const bool found = !(it.K == K_Whisper ? WhisperOpts(v) : KokoroOpts(v)).empty();
                std::wstring shown = v.empty() ? L"(none chosen - click BROWSE)" : NameOf(v);
                if (!v.empty() && GetFileAttributesW(v.c_str()) == INVALID_FILE_ATTRIBUTES)
                    shown += L"  (missing!)";
                DropBox(x, y, kColW - 74.0f, shown, HI_Drop, a, on && found);
                Button(x + kColW - 70.0f, y, 70.0f, 22.0f, L"BROWSE", HI_Browse, a, 0, false, on);
                y += 26.0f;
                if (on && (v.empty() || GetFileAttributesW(v.c_str()) == INVALID_FILE_ATTRIBUTES))
                {
                    Text(x + 8.0f, y - 4.0f, kColW - 8.0f, 16.0f,
                         it.K == K_Whisper ? L"Not installed: get ggml-base.en.bin, then BROWSE."
                                           : L"Not installed: get the Kokoro voice pack, then BROWSE.",
                         UiCol::Warn);
                    y += 14.0f;
                }
                y += 2.0f;
                break;
            }
            case K_Key:
            {
                const int vk = _wtoi(Val(it).c_str());
                const bool cap = Capturing == a;
                Label(x, y, it, on);
                Text(cx, y, 76.0f, 22.0f, cap ? L"press key" : KeyName(vk), cap ? UiCol::Warn : UiCol::PanelText);
                // (its own button stays live while capturing - it is how to cancel)
                const int saved = Capturing;
                Capturing = -1;
                Button(cx + 80.0f, y, 46.0f, 22.0f, cap ? L"CANCEL" : L"SET", HI_Rebind, a, 0, cap, saved < 0 || cap);
                Capturing = saved;
                Button(cx + 130.0f, y, 40.0f, 22.0f, L"NONE", HI_Clear, a, 0, false, vk != 0);
                y += 26.0f;
                // 2026-10-10: push-to-talk without a key hears nothing (VoiceCommand, 2026-10-09) - say so
                const std::wstring lv = Get(L"voice", L"listen", L"ptt");
                if (!vk && _wcsicmp(it.Key, L"ptt_vk") == 0 && _wcsicmp(lv.c_str(), L"ptt") == 0)
                {
                    Text(x + 8.0f, y - 4.0f, kColW - 8.0f, 16.0f, L"No key set: nothing is heard until you set one.", UiCol::Warn);
                    y += 14.0f;
                }
                break;
            }
            case K_Open:
            {
                Button(x, y, 176.0f, 22.0f, it.Label, HI_Open, a);
                HelpMark(x + 184.0f, y, it.Help);
                y += 28.0f;
                break;
            }
            case K_Action:
            {
                y += 4.0f;
                Button(x, y, 176.0f, 24.0f, it.Label, HI_Action, a, 0, false, true);
                HelpMark(x + 184.0f, y + 1.0f,
                         (CanTest() || it.Act != A_TestRadio) ? it.Help
                                   : it.Help + std::wstring(VcRunning ? L"\n\nNeeds natural voices (ENGINE) and \"Pilots speak\" on (COMMS)."
                                                                      : L"\n\nStart VoiceCommand (top right) to test."));
                y += 30.0f;
                break;
            }
            case K_Pilots:
                y = DrawPilots(x, y);
                break;
            }
        }
    }

    float DrawPilots(float x, float y)
    {
        if (!Natural())
        {
            Text(x, y, 600.0f, 18.0f, L"Not used now (Kokoro voices only).", UiCol::PanelDim);
            return y + 20.0f;
        }
        if (Pilots.empty())
        {
            Text(x, y, 700.0f, 18.0f, L"None yet. A pilot appears here once VoiceCommand has heard its name in a mission.", UiCol::PanelDim);
            return y + 20.0f;
        }
        const int rows = 6;
        PilotTop = std::max(0, std::min(PilotTop, (int)Pilots.size() - rows));
        for (int k = PilotTop; k < (int)Pilots.size() && k < PilotTop + rows; ++k)
        {
            const std::wstring code = VoiceCode(Pilots[(size_t)k].second);
            VoiceRow(x, y, Pilots[(size_t)k].first, VoiceLabel(Pilots[(size_t)k].second), code, HI_PilotDrop, k, HI_PilotTest, true, 1000 + k);
            const bool armed = Armed(HI_PilotDel, k) && Pilots[(size_t)k].first == PilotArmed;
            Button(x + 604.0f, y, 70.0f, 22.0f, armed ? L"SURE?" : L"FORGET", HI_PilotDel, k, 0, armed, true,
                   armed ? UiCol::Warn : UiCol::PanelText);
            y += 26.0f;
        }
        if ((int)Pilots.size() > rows)
        {
            wchar_t b[64];
            swprintf(b, 64, L"%d-%d of %d", PilotTop + 1, std::min((int)Pilots.size(), PilotTop + rows), (int)Pilots.size());
            Button(x, y, 60.0f, 22.0f, L"UP", HI_PilotPage, -1, 0, false, PilotTop > 0);
            Text(x + 64.0f, y, 120.0f, 22.0f, b, UiCol::PanelDim, 1.0f, true);
            Button(x + 188.0f, y, 60.0f, 22.0f, L"DOWN", HI_PilotPage, 1, 0, false, PilotTop + rows < (int)Pilots.size());
            y += 26.0f;
        }
        return y;
    }

    void Render()
    {
        if (!Rt)
            return;
        RECT rc;
        GetClientRect(Wnd, &rc);
        const float W = (float)(rc.right - rc.left), Hh = (float)(rc.bottom - rc.top);
        Sc = std::max(0.3f, std::min(W / kW, Hh / kH));
        Ox = (W - kW * Sc) * 0.5f;
        Oy = (Hh - kH * Sc) * 0.5f;
        Hits.clear();
        Tips.clear();
        Rt->BeginDraw();
        Rt->SetTransform(D2D1::Matrix3x2F::Identity());
        Rt->Clear(D2D1::ColorF(0x000000));
        Rt->SetTransform(D2D1::Matrix3x2F::Scale(Sc, Sc) * D2D1::Matrix3x2F::Translation(Ox, Oy));
        Col(UiCol::Panel, 0.96f);
        Rt->FillRectangle(D2D1::RectF(0, 0, kW, kH), Br.Get());
        Col(UiCol::PanelEdge, 0.9f);
        Rt->DrawRectangle(D2D1::RectF(0.5f, 0.5f, kW - 0.5f, kH - 0.5f), Br.Get(), 1.0f);

        Text(16.0f, 8.0f, 200.0f, 24.0f, L"GLOBAL SETTINGS", UiCol::Warn, 1.0f, false, FmtTitle.Get());
        // VoiceCommand: running, or START
        if (VcRunning)
            Text(560.0f, 10.0f, 204.0f, 20.0f, L"VoiceCommand: running", UiCol::CheckOn);
        else
        {
            Text(496.0f, 10.0f, 190.0f, 20.0f, L"VoiceCommand: not running", UiCol::Warn);
            Button(690.0f, 8.0f, 74.0f, 22.0f, L"START", HI_StartVc);
        }
        // module tabs
        float tx = 16.0f;
        for (size_t t = 0; t < Tabs.size(); ++t)
        {
            // 2026-10-17: five tabs (PAPERDOLLS) - narrower so they fit the canvas
            const float w = std::min(150.0f, (kW - 32.0f - 6.0f * (float)(Tabs.size() - 1)) / (float)Tabs.size());
            Button(tx, 40.0f, w, 26.0f, Tabs[t], HI_Tab, (int)t, 0, (int)t == Tab);
            tx += w + 6.0f;
        }
        // sub-tabs of the module
        float sx = 16.0f;
        int subs = 0;
        for (size_t p = 0; p < Pages.size(); ++p)
            if (Pages[p].Tab == Tabs[(size_t)Tab] && !Pages[p].Sub.empty())
            {
                Button(sx, 72.0f, 110.0f, 22.0f, Pages[p].Sub, HI_Sub, (int)p, 0, (int)p == Sub);
                sx += 116.0f;
                ++subs;
            }
        if (!subs)
        {
            Col(UiCol::PanelEdge, 0.5f);
            Rt->DrawLine(D2D1::Point2F(16.0f, 84.0f), D2D1::Point2F(kW - 16.0f, 84.0f), Br.Get(), 1.0f);
        }
        if (Sub >= 0 && Sub < (int)Pages.size())
            DrawItems(Pages[(size_t)Sub]);

        // bottom bar: what just happened, and RESET PAGE
        const bool reset = Sub >= 0 && Sub < (int)Pages.size() && Pages[(size_t)Sub].Reset;
        std::wstring st = Status;
        UINT32 sc = StatusWarn ? UiCol::Warn : UiCol::PanelText;
        if (GetTickCount64() - StatusMs > 5000)
        {
            st = L"All changes are saved as you click.   * = used after VoiceCommand restarts";
            sc = UiCol::PanelDim;
        }
        Col(UiCol::PanelEdge, 0.5f);
        Rt->DrawLine(D2D1::Point2F(16.0f, kH - 38.0f), D2D1::Point2F(kW - 16.0f, kH - 38.0f), Br.Get(), 1.0f);
        Text(16.0f, kH - 32.0f, reset ? kW - 232.0f : kW - 32.0f, 24.0f, st, sc);
        if (reset)
        {
            const bool armed = Armed(HI_Reset, Sub);
            Button(kW - 212.0f, kH - 32.0f, 196.0f, 24.0f, armed ? L"CLICK AGAIN TO RESET" : L"RESET PAGE TO DEFAULTS", HI_Reset, Sub, 0,
                   armed, true, armed ? UiCol::Warn : UiCol::PanelText);
        }

        DrawTip(); // the hovered "?"

        // the open dropdown, over everything
        if (DropItem >= 0)
        {
            const int rows = std::min(14, (int)DropOpts.size());
            float widest = 0.0f; // 2026-10-10: wide enough for its longest choice
            for (auto &o : DropOpts)
                widest = std::max(widest, TextWidth(o.Label) + 16.0f);
            const float rh = 20.0f, w = std::min(kW - 8.0f, std::max({DropAt.right - DropAt.left, 190.0f, widest}));
            float y = DropAt.bottom + 2.0f;
            if (y + rows * rh > kH - 4.0f)
                y = std::max(4.0f, DropAt.top - 2.0f - rows * rh);
            const float lx = std::min(DropAt.left, kW - 4.0f - w);
            const D2D1_RECT_F box = D2D1::RectF(lx, y, lx + w, y + rows * rh);
            Col(UiCol::Panel);
            Rt->FillRectangle(box, Br.Get());
            Col(UiCol::PanelText);
            Rt->DrawRectangle(box, Br.Get(), 1.4f);
            DropScroll = std::max(0, std::min(DropScroll, (int)DropOpts.size() - rows));
            const float mx = (Mx - Ox) / Sc, my = (My - Oy) / Sc;
            for (int r = 0; r < rows; ++r)
            {
                const int k = DropScroll + r;
                const float ry = y + r * rh;
                const bool hov = mx >= box.left && mx < box.right && my >= ry && my < ry + rh;
                if (hov)
                {
                    Col(UiCol::CheckOn, 0.3f);
                    Rt->FillRectangle(D2D1::RectF(box.left + 1, ry, box.right - 1, ry + rh), Br.Get());
                }
                Text(box.left + 6.0f, ry, w - 12.0f, rh, DropOpts[(size_t)k].Label, UiCol::PanelText);
                AddHit(box.left, ry, w, rh, HI_DropPick, k);
            }
            if ((int)DropOpts.size() > rows)
            {
                Col(UiCol::Panel);
                const float ty = box.bottom > kH - 24.0f ? box.top - 18.0f : box.bottom;
                Rt->FillRectangle(D2D1::RectF(box.left, ty, box.right, ty + 16.0f), Br.Get());
                Text(box.left, ty, w, 16.0f, L"mouse wheel: more", UiCol::PanelDim, 0.9f, true);
            }
        }
        if (Rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET)
        {
            Rt.Reset();
            Br.Reset();
            CreateTarget();
            InvalidateRect(Wnd, nullptr, FALSE);
        }
    }

    // ---- input ----
    Page &Cur() { return Pages[(size_t)Sub]; }

    void OpenDrop(int item, std::vector<Opt> opts, const D2D1_RECT_F &at, const std::wstring &cur)
    {
        DropItem = item;
        DropOpts = std::move(opts);
        DropAt = at;
        DropScroll = 0;
        for (size_t k = 0; k < DropOpts.size(); ++k) // the current choice in view
            if (_wcsicmp(DropOpts[k].Value.c_str(), cur.c_str()) == 0)
                DropScroll = (int)k - 3;
    }

    void Click(float wx, float wy)
    {
        const float mx = (wx - Ox) / Sc, my = (wy - Oy) / Sc;
        struct Clear // the targets belong to what was drawn: none survive a click (the next paint makes new ones),
        {            // so a second click queued before that paint cannot act on a page that is gone
            std::vector<Hit> &H;
            ~Clear() { H.clear(); }
        } clearAfter{Hits};
        const Hit *h = nullptr;
        for (auto it = Hits.rbegin(); it != Hits.rend(); ++it) // the dropdown was drawn last: it wins
            if (mx >= it->R.left && mx < it->R.right && my >= it->R.top && my < it->R.bottom)
            {
                h = &*it;
                break;
            }
        if (DropItem >= 0)
        {
            if (h && h->Id == HI_DropPick)
                PickDrop(h->A);
            DropItem = -1; // any click closes it
            return;
        }
        if (Capturing >= 0 && !(h && h->Id == HI_Rebind))
            return; // the click is the key (handled in WndProc)
        if (!h)
            return;
        switch (h->Id) // an item on the page (indexes are of the page that was drawn - still the same page here)
        {
        case HI_Check: case HI_Minus: case HI_Plus: case HI_Drop: case HI_Test: case HI_Action: case HI_Rebind:
        case HI_Clear: case HI_Browse: case HI_Open:
            if (h->A < 0 || h->A >= (int)Cur().Items.size())
                return;
            break;
        default:
            break;
        }
        // A pilot row can be gone since it was drawn (PilotVoices.txt re-read in between): ignore that click.
        if ((h->Id == HI_PilotDrop || h->Id == HI_PilotDel || h->Id == HI_PilotTest) && (h->A < 0 || h->A >= (int)Pilots.size()))
            return;
        const bool wasArmed = Armed(h->Id, h->A);
        ArmedId = 0; // any click disarms (a confirming one is checked above)
        switch (h->Id)
        {
        case HI_Tab:
            Tab = h->A;
            for (size_t p = 0; p < Pages.size(); ++p)
                if (Pages[p].Tab == Tabs[(size_t)Tab])
                {
                    Sub = (int)p;
                    break;
                }
            break;
        case HI_Sub:
            Sub = h->A;
            break;
        case HI_Check:
        {
            const Item &it = Cur().Items[(size_t)h->A];
            Put(it, _wtoi(Val(it).c_str()) ? L"0" : L"1");
            break;
        }
        case HI_Minus:
        case HI_Plus:
        {
            const Item &it = Cur().Items[(size_t)h->A];
            double v = NumVal(it);
            const bool up = h->Id == HI_Plus;
            v += (up != it.Inv) ? it.Step : -it.Step; // "+" on an inverted one lowers the stored number
            v = std::max(it.Lo, std::min(it.Hi, std::round(v / it.Step) * it.Step));
            Cache.erase(std::wstring(it.Sec) + L"/" + it.Key);
            gs::SetNum(it.Sec, it.Key, v, it.Dec);
            Saved(it);
            break;
        }
        case HI_Drop:
        {
            const Item &it = Cur().Items[(size_t)h->A];
            const std::wstring cur = it.K == K_Voice ? VoiceCode(Val(it)) : Val(it);
            std::vector<Opt> o = it.K == K_Choice ? it.Opts
                                 : it.K == K_Voice ? VoiceOpts(true)
                                 : it.K == K_Whisper ? WhisperOpts(Val(it))
                                                     : KokoroOpts(Val(it));
            if (!o.empty())
                OpenDrop(h->A, std::move(o), h->R, cur);
            break;
        }
        case HI_PilotDrop:
            PilotDrop = Pilots[(size_t)h->A].first;
            OpenDrop(1000 + h->A, VoiceOpts(false), h->R, VoiceCode(Pilots[(size_t)h->A].second));
            break;
        case HI_PilotDel:
            if (!wasArmed || PilotArmed != Pilots[(size_t)h->A].first)
            {
                Arm(HI_PilotDel, h->A);
                PilotArmed = Pilots[(size_t)h->A].first;
                Note(L"Click SURE? to forget " + PilotArmed + L"'s voice.", true);
            }
            else
            {
                const std::wstring who = PilotArmed;
                if (EditPilot(who, nullptr))
                    Note(who + L" forgotten - gets a new voice next time.");
            }
            break;
        case HI_PilotPage:
            PilotTop += h->A * 6;
            break;
        case HI_PilotTest:
            Test(2, VoiceCode(Pilots[(size_t)h->A].second), L"This is " + Pilots[(size_t)h->A].first + L". Radio check, how copy?");
            break;
        case HI_Test:
        {
            const Item &it = Cur().Items[(size_t)h->A];
            int slot = 0;
            if (wcslen(it.Key) > 13)
                slot = std::max(0, std::min(8, _wtoi(it.Key + 13))); // kokoro_voice_N
            std::wstring v = VoiceCode(Val(it));
            if (v.empty())
                v = kKokoroDefault[slot]; // "(standard)": VoiceCommand's own default for that number
            Test(slot, v, slot ? L"Lancemate " + std::to_wstring(slot) + L" here. Radio check, how copy?"
                               : std::wstring(L"Lance copies. Radio check, how copy?"));
            break;
        }
        case HI_Action:
            if (Cur().Items[(size_t)h->A].Act == A_TestRadio)
                Test(2, L"", L"Two here. Contact, bearing two seven zero, moving to engage.");
            else if (Cur().Items[(size_t)h->A].Act == A_TestAlarm)
                AlarmTestStart();
            else if (Cur().Items[(size_t)h->A].Act == A_PdExtract)
                PdExtract();
            else if (Cur().Items[(size_t)h->A].Act == A_PdBrowse)
                PdBrowse();
            else if (Cur().Items[(size_t)h->A].Act == A_PdOpen)
            {
                std::error_code ec;
                const std::filesystem::path d = pde::CacheDir();
                std::filesystem::create_directories(d, ec);
                ShellExecuteW(Wnd, L"open", d.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case HI_Reset:
            if (!wasArmed)
            {
                Arm(HI_Reset, h->A);
                Note(L"Click again to put every setting on this page back to its default.", true);
            }
            else
                ResetPage();
            break;
        case HI_Rebind:
            Capturing = Capturing == h->A ? -1 : h->A;
            if (Capturing >= 0)
                Note(L"Press the key (or mouse button 3-5) to use. Esc cancels.");
            break;
        case HI_Clear:
            Put(Cur().Items[(size_t)h->A], L"0");
            break;
        case HI_Browse:
            Browse(Cur().Items[(size_t)h->A]);
            break;
        case HI_Open:
        {
            const std::wstring f = gs::Dir() + Cur().Items[(size_t)h->A].Key;
            if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES)
                Note(L"Not there yet - VoiceCommand creates it the first time it starts.", true);
            else
                ShellExecuteW(Wnd, L"open", L"notepad.exe", (L"\"" + f + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case HI_StartVc:
            Launch(L"VoiceCommand.exe", L"");
            break;
        case HI_VSpeed:
        {
            // A = item index, or 1000 + pilot row; B = +1 / -1. The speed belongs to the VOICE the row uses.
            std::wstring code;
            if (h->A >= 1000)
            {
                if (h->A - 1000 < (int)Pilots.size())
                    code = VoiceCode(Pilots[(size_t)(h->A - 1000)].second);
            }
            else if (h->A >= 0 && h->A < (int)Cur().Items.size())
                code = RowVoice(Cur().Items[(size_t)h->A]);
            if (code.empty())
                break;
            double v = SpeedOf(code) + 0.05 * h->B;
            v = std::max(0.5, std::min(2.0, std::round(v / 0.05) * 0.05));
            Cache.erase(L"voice/kokoro_speed_" + code);
            gs::SetNum(L"voice", (L"kokoro_speed_" + code).c_str(), v, 2);
            Note(VoiceLabel(code) + L": speed saved - in use now.");
            break;
        }
        default:
            break;
        }
    }

    // RESET PAGE: every plain setting on the page back to its default. Model folders, the push-to-talk key and
    // pilots' own voices are left alone (they have no sensible default to go back to).
    void ResetPage()
    {
        int n = 0;
        for (auto &it : Cur().Items)
            if ((it.K == K_Check || it.K == K_Tick || it.K == K_Num || it.K == K_Choice || it.K == K_Voice) && it.Key[0])
            {
                Write(it.Sec, it.Key, it.Def);
                ++n;
            }
        bool voices = false;
        for (auto &it : Cur().Items)
            voices = voices || it.K == K_Voice;
        if (voices) // every voice back to normal speed (and the old one-for-all speed they fall back on)
        {
            Write(L"voice", L"kokoro_speed", L"1");
            for (auto &v : kVoices)
                if (gs::Has(L"voice", (std::wstring(L"kokoro_speed_") + v.Code).c_str()))
                {
                    Write(L"voice", (std::wstring(L"kokoro_speed_") + v.Code).c_str(), L"1");
                    ++n;
                }
        }
        Note(L"Back to defaults (" + std::to_wstring(n) + L" settings).");
    }

    void PickDrop(int k)
    {
        if (k < 0 || k >= (int)DropOpts.size())
            return;
        if (DropItem >= 1000)
        {
            const std::wstring v = DropOpts[(size_t)k].Value;
            EditPilot(PilotDrop, &v); // by name: the rows may have moved since the list opened
            return;
        }
        const Item &it = Cur().Items[(size_t)DropItem];
        Put(it, DropOpts[(size_t)k].Value);
    }

    // ---- PAPERDOLLS (2026-10-17) -------------------------------------------------------------
    // Extraction runs on its own thread into <cache>.new, then replaces <cache> - a reader never sees half a set.
    // source.txt beside the masks holds the pak fingerprint (pde::PakSignature) the set was made from.
    std::atomic<bool> PdBusy{false}, PdJustEnded{false};
    std::atomic<int>  PdDone{0}, PdTotal{0};
    std::mutex        PdMx;
    std::wstring      PdLast;          // the last run's result line (PdMx)
    bool              PdLastWarn = false;
    ULONGLONG         PdCheckMs = 0;   // the fingerprint check is cached for 3 s (stats ~70 files)
    std::wstring      PdStatus, PdGameLine;
    bool              PdStatusWarn = false, PdGameWarn = false;

    std::filesystem::path PdGameDir()
    {
        const std::wstring v = Get(L"paperdolls", L"game_dir", L"");
        if (!v.empty() && pde::IsGameDir(v))
            return v;
        static std::filesystem::path found;
        static bool looked = false;
        if (!looked)
        {
            looked = true;
            found = pde::FindGameDir();
        }
        return found;
    }
    bool PdMods() { return Get(L"paperdolls", L"include_mods", L"1") != L"0"; }

    static std::wstring Widen(const std::string &s)
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w((size_t)std::max(0, n), L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
        return w;
    }

    void PdRefresh()
    {
        const ULONGLONG now = GetTickCount64();
        if (PdCheckMs && now - PdCheckMs < 3000)
            return;
        PdCheckMs = now;
        const std::filesystem::path game = PdGameDir(), cache = pde::CacheDir();
        PdGameWarn = game.empty();
        PdGameLine = game.empty() ? L"Game folder: not found - press GAME FOLDER... and pick the folder that holds MW5Mercs."
                                  : L"Game folder: " + game.wstring();
        std::error_code ec;
        std::string stored;
        {
            std::ifstream f(cache / "source.txt", std::ios::binary);
            stored.assign(std::istreambuf_iterator<char>(f), {});
        }
        if (stored.empty())
        {
            PdStatus = L"Not extracted yet - press EXTRACT. Until then the labels show plain outlines.";
            PdStatusWarn = true;
            return;
        }
        int units = 0;
        for (auto &d : std::filesystem::directory_iterator(cache, ec))
            if (d.is_directory() && d.path().filename().wstring()[0] != L'_')
                ++units;
        const auto t = std::filesystem::last_write_time(cache / "source.txt", ec);
        std::wstring when;
        {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
            std::tm lt{};
            localtime_s(&lt, &tt);
            wchar_t b[64];
            wcsftime(b, 64, L"%Y-%m-%d %H:%M", &lt);
            when = b;
        }
        // 2026-10-19: a set from an older decoder (no / another "decoder N" first line) is out of date too.
        const bool oldDecoder = stored.compare(0, pde::DecoderTag().size(), pde::DecoderTag()) != 0;
        const bool changed = oldDecoder || (!game.empty() && pde::PakSignature(pde::FindPaks(game, PdMods())) != stored);
        PdStatusWarn = changed;
        PdStatus = std::wstring(oldDecoder ? L"Made by an older version of this tool - press EXTRACT to update the pictures. "
                                : changed  ? L"Game or mod files changed since the last extraction - press EXTRACT to update. "
                                           : L"Ready. ") +
                   std::to_wstring(units) + L" units, extracted " + when + L".";
    }

    std::wstring LiveText(const wchar_t *key, bool &warn)
    {
        warn = false;
        if (!wcscmp(key, L"pd_status"))
        {
            if (PdBusy.load())
            {
                const int d = PdDone.load(), t = PdTotal.load();
                return L"Extracting... " + (t ? std::to_wstring(d) + L" of " + std::to_wstring(t) : std::wstring(L"reading the paks"));
            }
            {
                std::lock_guard<std::mutex> lk(PdMx);
                if (!PdLast.empty() && PdLastWarn)
                {
                    warn = true;
                    return PdLast;
                }
            }
            PdRefresh();
            warn = PdStatusWarn;
            return PdStatus;
        }
        if (!wcscmp(key, L"pd_game"))
        {
            PdRefresh();
            warn = PdGameWarn;
            return PdGameLine;
        }
        if (!wcscmp(key, L"pd_cache"))
            return L"Saved in: " + pde::CacheDir().wstring();
        return L"";
    }

    void PdExtract()
    {
        if (PdBusy.load())
            return;
        const std::filesystem::path game = PdGameDir(), cache = pde::CacheDir();
        if (game.empty())
        {
            Note(L"Game folder not found - press GAME FOLDER... first.", true);
            return;
        }
        if (cache.empty())
        {
            Note(L"No folder for the pictures (LOCALAPPDATA unavailable).", true);
            return;
        }
        const std::vector<std::filesystem::path> paks = pde::FindPaks(game, PdMods());
        {
            std::lock_guard<std::mutex> lk(PdMx);
            PdLast.clear();
            PdLastWarn = false;
        }
        PdDone = 0;
        PdTotal = 0;
        PdBusy = true;
        Note(L"Extracting the paperdolls from " + std::to_wstring(paks.size()) + L" pak file(s)...");
        std::thread([this, paks, cache]() {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path tmp = cache.wstring() + L".new", old = cache.wstring() + L".old";
            // bug sweep 2026-10-17: nothing thrown on this thread may end the app (a damaged pak, a file-system error)
            try
            {
            fs::remove_all(tmp, ec);
            pde::Result r;
            const bool ok = pde::Extract(paks, tmp, r, [this](int d, int t, const std::string &) {
                PdDone = d;
                PdTotal = t;
                return true;
            });
            std::wstring msg;
            bool warn = false;
            if (!ok || r.Files == 0)
            {
                msg = L"Extraction failed - nothing was changed." +
                      (r.Failures.empty() ? std::wstring() : L" " + Widen(r.Failures.front()));
                warn = true;
                fs::remove_all(tmp, ec);
            }
            else
            {
                {
                    std::ofstream f(tmp / "source.txt", std::ios::binary);
                    const std::string sig = pde::PakSignature(paks);
                    f.write(sig.data(), (std::streamsize)sig.size());
                }
                fs::remove_all(old, ec);
                if (fs::exists(cache, ec))
                    fs::rename(cache, old, ec);
                fs::rename(tmp, cache, ec);
                if (ec)
                {
                    msg = L"Could not replace the old pictures (a display may be reading them) - try again.";
                    warn = true;
                    if (!fs::exists(cache) && fs::exists(old))
                        fs::rename(old, cache, ec);
                }
                else
                {
                    fs::remove_all(old, ec);
                    msg = L"Done: " + std::to_wstring(r.Chassis) + L" units, " + std::to_wstring(r.Files) + L" pictures from " +
                          std::to_wstring(r.Paks) + L" pak file(s).";
                }
            }
            {
                std::lock_guard<std::mutex> lk(PdMx);
                PdLast = msg;
                PdLastWarn = warn;
            }
            PdCheckMs = 0;
            PdBusy = false;
            PdJustEnded = true;
            }
            catch (...)
            {
                std::error_code ec2;
                fs::remove_all(tmp, ec2);
                if (!fs::exists(cache, ec2) && fs::exists(old, ec2)) // caught between the two renames: the old set back
                    fs::rename(old, cache, ec2);
                {
                    std::lock_guard<std::mutex> lk(PdMx);
                    PdLast = L"Extraction failed - a pak file could not be read. Your existing pictures were kept.";
                    PdLastWarn = true;
                }
                PdCheckMs = 0;
                PdBusy = false;
                PdJustEnded = true;
            }
        }).detach();
    }

    void PdBrowse()
    {
        ComPtr<IFileOpenDialog> d;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
            return;
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Pick the MechWarrior 5 Mercenaries folder (the one that holds MW5Mercs)");
        if (FAILED(d->Show(Wnd)))
            return;
        ComPtr<IShellItem> r;
        PWSTR p = nullptr;
        if (FAILED(d->GetResult(&r)) || FAILED(r->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p)
            return;
        std::filesystem::path path = p;
        CoTaskMemFree(p);
        // also accept MW5Mercs, Content or Paks inside it
        for (int up = 0; up < 4 && !pde::IsGameDir(path); ++up)
            path = path.parent_path();
        if (!pde::IsGameDir(path))
        {
            Note(L"That is not the game folder (no MW5Mercs\\Content\\Paks\\MW5Mercs-WindowsNoEditor.pak) - not changed.", true);
            return;
        }
        Write(L"paperdolls", L"game_dir", path.wstring());
        PdCheckMs = 0;
        Note(L"Game folder set.");
    }

    void Browse(const Item &it)
    {
        ComPtr<IFileOpenDialog> d;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
            return;
        DWORD o = 0;
        d->GetOptions(&o);
        if (it.K == K_Kokoro)
            d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        else
        {
            COMDLG_FILTERSPEC f[] = {{L"Speech recognition models (ggml-*.bin)", L"*.bin"}, {L"All files", L"*.*"}};
            d->SetFileTypes(2, f);
            d->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        }
        d->SetTitle(it.K == K_Kokoro ? L"Pick the voice pack folder (it holds model.onnx and voices.bin)"
                                     : L"Pick a speech recognition model (ggml-....bin)");
        if (FAILED(d->Show(Wnd)))
            return;
        ComPtr<IShellItem> r;
        PWSTR p = nullptr;
        if (SUCCEEDED(d->GetResult(&r)) && SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
        {
            std::wstring path = p;
            CoTaskMemFree(p);
            if (it.K == K_Kokoro && GetFileAttributesW((path + L"\\model.onnx").c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                Note(L"That folder has no model.onnx - not changed.", true);
                return;
            }
            Put(it, path);
        }
    }

    // A key (or mouse button) while capturing the push-to-talk key.
    bool KeyPressed(int vk)
    {
        if (Capturing < 0)
            return false;
        const int a = Capturing;
        Capturing = -1;
        if (vk == VK_ESCAPE)
        {
            Note(L"Not changed.");
            return true;
        }
        Put(Cur().Items[(size_t)a], std::to_wstring(vk));
        return true;
    }

    // Twice a second: someone else changed a file (VoiceCommand, HUDDisplay, a text editor), and what is running.
    void Poll()
    {
        const ULONGLONG now = GetTickCount64();
        bool redraw = false;
        if (IniWatch.Changed(now))
        {
            Cache.clear();
            redraw = true;
        }
        if (PvWatch.Changed(now))
        {
            LoadPilots();
            redraw = true;
        }
        if (PdBusy.load() || PdJustEnded.exchange(false))
            redraw = true;
        const bool vc = VoiceCommandRunning();
        if (vc != VcRunning)
        {
            VcRunning = vc;
            redraw = true;
        }
        if (redraw || (StatusMs && now - StatusMs > 5000 && now - StatusMs < 5600) || (ArmedId && now - ArmedMs > 3000 && now - ArmedMs < 3600))
            InvalidateRect(Wnd, nullptr, FALSE);
    }

    // ---- set-up ----
    bool CreateTarget()
    {
        RECT rc;
        GetClientRect(Wnd, &rc);
        if (FAILED(D2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                               D2D1::HwndRenderTargetProperties(Wnd, D2D1::SizeU((UINT32)(rc.right - rc.left), (UINT32)(rc.bottom - rc.top))),
                                               &Rt)))
            return false;
        Rt->SetDpi(96.0f, 96.0f);
        return SUCCEEDED(Rt->CreateSolidColorBrush(D2D1::ColorF(0x9CFFC0), &Br));
    }

    const wchar_t *PickFont()
    {
        static const wchar_t *kC[] = {L"Bahnschrift SemiCondensed", L"Bahnschrift", L"Lucida Console", L"Consolas"};
        ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(Dw->GetSystemFontCollection(&fonts, FALSE)) && fonts)
            for (auto *n : kC)
            {
                UINT32 i = 0;
                BOOL ex = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(n, &i, &ex)) && ex)
                    return n;
            }
        return L"Consolas";
    }

    bool Init(HINSTANCE inst)
    {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, D2d.GetAddressOf())))
            return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(Dw.GetAddressOf()))))
            return false;
        const wchar_t *fam = PickFont();
        for (int c = 0; c < 3; ++c)
        {
            ComPtr<IDWriteTextFormat> &f = c == 0 ? Fmt : (c == 1 ? FmtC : FmtTitle);
            if (FAILED(Dw->CreateTextFormat(fam, nullptr, c == 2 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, c == 2 ? 18.0f : 13.0f, L"en-us", &f)))
                return false;
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (c == 1)
                f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> dots;
            if (SUCCEEDED(Dw->CreateEllipsisTrimmingSign(f.Get(), &dots)))
                f->SetTrimming(&t, dots.Get());
        }
        if (FAILED(Dw->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f,
                                        L"en-us", &FmtWrap)))
            return false;
        FmtWrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        FmtWrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        Pages = BuildPages();
        for (auto &p : Pages)
            if (std::find(Tabs.begin(), Tabs.end(), p.Tab) == Tabs.end())
                Tabs.push_back(p.Tab);
        LoadPilots();
        VcRunning = VoiceCommandRunning();

        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = &App::Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW (this project is not built UNICODE)
        wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));   // IDI_APPLICATION
        wc.lpszClassName = L"MW5GlobalSettings";
        RegisterClassExW(&wc);
        RECT r = {0, 0, (LONG)kW, (LONG)kH};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        Wnd = CreateWindowExW(0, wc.lpszClassName, L"MW5 Global Settings", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
        if (!Wnd || !CreateTarget())
            return false;
        SetTimer(Wnd, 1, 500, nullptr);
        ShowWindow(Wnd, SW_SHOW);
        return true;
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        App *a = reinterpret_cast<App *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE)
        {
            a = reinterpret_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)a);
            a->Wnd = h;
        }
        if (!a)
            return DefWindowProcW(h, m, w, l);
        switch (m)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            a->Render();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE:
            if (a->Rt)
                a->Rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            a->Mx = (float)GET_X_LPARAM(l);
            a->My = (float)GET_Y_LPARAM(l);
            if (!a->Tracking)
            {
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
                a->Tracking = TrackMouseEvent(&t) != FALSE;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSELEAVE:
            a->Tracking = false;
            a->Mx = a->My = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            a->Click((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            if (a->Capturing >= 0)
            {
                const int vk = m == WM_RBUTTONDOWN ? VK_RBUTTON
                               : m == WM_MBUTTONDOWN ? VK_MBUTTON
                                                     : (GET_XBUTTON_WPARAM(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
                a->KeyPressed(vk);
                InvalidateRect(h, nullptr, FALSE);
                return m == WM_XBUTTONDOWN ? TRUE : 0;
            }
            if (a->DropItem >= 0)
            {
                a->DropItem = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSEWHEEL:
            if (a->DropItem >= 0)
            {
                a->DropScroll += GET_WHEEL_DELTA_WPARAM(w) > 0 ? -3 : 3;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (a->KeyPressed((int)w))
            {
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (w == VK_ESCAPE && a->DropItem >= 0)
            {
                a->DropItem = -1;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_TIMER:
            if (w == 2)
                a->AlarmTestTick(); // TEST ALARM
            else
                a->Poll();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    SetProcessDPIAware();
    // One window is enough: a second start brings the first to the front.
    HANDLE one = CreateMutexW(nullptr, TRUE, L"Local\\MW5_GlobalSettings");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND w = FindWindowW(L"MW5GlobalSettings", nullptr))
        {
            ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Settings that used to live in VoiceCommand.ini come over once, so the first look is right.
    gs::MigrateSection(L"voice", gs::Dir() + L"VoiceCommand.ini");
    gs::PublishPath(); // tells LanceControl (in the game) where GlobalSettings.ini is (2026-10-07h)
    static App app;
    if (!app.Init(inst))
    {
        MessageBoxW(nullptr, L"GlobalSettings could not start (Direct2D).", L"MW5 Global Settings", MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
