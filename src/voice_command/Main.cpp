// VoiceCommand.exe - FIXED-COMMAND VOICE PROTOTYPE for the lance.
//
// A separate program (nothing in HUDDisplay or the plugins depends on it). It listens with the
// Windows speech recogniser (SAPI, offline, no keys) against a FIXED grammar - every phrase it
// accepts is listed below - and turns each into the same orders HUDDisplay's TacOps page sends,
// through the VanillaHud plugin's shared order ring (MW5_VanillaHUDExport, claim-then-write, so it
// and HUDDisplay can both send). Replies are spoken with the installed Windows voices, one voice
// per lancemate where there are enough.
//
// Say: <who> <order>
//   who:   all | lance | everyone | one .. eight  (the order slot number, as the F-keys use)
//          or, with Whisper, the pilot's name ("Sledge, hold position") once VoiceCommand knows it
//   order: form on me | follow me
//          attack my target | engage my target          (your current lock - resolved in game)
//          hold position | hold here                     (holds where it stands)
//          indirect fire only | hold for indirect fire   (indirect hold where it stands)
//          fall back | fall back to me                   (to 50 m from you, facing the enemy)
//          move <fifty .. five hundred> [metres] <north|south|east|west|forward|back|left|right>
//                                                        (from YOUR position; forward = your heading)
//          fall back <fifty .. five hundred> [metres] <direction>   (as move, but the fall-back
//                                                        order: faces the enemy on the way)
//          hold fire | weapons free | return fire
//          run cool | normal heat | run hot | alpha strike
//          <category> only | hold <category> | all weapons
//             category: lasers | particle cannons | ballistics | missiles | lock on missiles |
//                       dumbfire missiles | machine guns | flamers | tag | narc
//          reset fire orders
//          status | status report | report | report in | sitrep | what's your status |
//          how are you holding up        (damage, ammo, heat and lost weapons, in its own voice)
//
// VoiceCommand.ini beside the exe (created with defaults on first run):
//   min_confidence = 0 low / 1 normal / 2 high (default 1)
//   ptt_vk         = virtual-key code to hold while speaking (default 0 = no key yet: with listen = ptt
//                    nothing is heard until one is set - 2026-10-09, push-to-talk is the release default)
//   speak          = 1 spoken replies, 0 silent (default 1)
//   rate           = voice rate -10..10 (default 1)
//   callouts       = 1 lancemates speak up on their own (stage 2), 0 off (default 1)
//   whisper_model  = ggml Whisper model for free speech (stage 3); empty = off
//   whisper_threads = CPU threads for Whisper (default 4)
//   recognizer     = whisper (default) | windows. whisper: every utterance goes to Whisper and its
//                    words are matched to the orders below (synonyms, numbers, near misses allowed;
//                    unclear -> "say again?"). windows: the fixed grammar (the fallback; Whisper then
//                    only prints what matched nothing). Falls back to windows if Whisper cannot run.
//   listen         = ptt (default, 2026-10-09: only while ptt_vk is held) | always | both (always; while ptt_vk
//                    is held the callsign may be left out - the order goes to the whole lance)
//   whisper_trim   = 1 (default) Whisper processes the speech plus a generous margin (at least ~10 s)
//                    instead of its full 30 s window - much quicker. The first, tight trim made it
//                    repeat itself ("Lands. Status update." x6); repeats are also collapsed and the
//                    output capped. 0 = the full window. The old whisper_fast key is no longer read.
//   Heard / matched / rejected utterances are written to VoiceCommand_heard.log beside the exe. (emptied at each start)
//   english_only   = 1 lancemates use English-language voices only (default 1)
// Kokoro voices + radio filter: tts = kokoro | windows, kokoro_dir, kokoro_threads, kokoro_speed,
//   kokoro_voice_N / kokoro_voice_<PilotName>, radio, radio_static, radio_crackle, radio_volume
//   (see KOKORO VOICES + RADIO).
// VoiceLines.txt beside the exe: everything the pilots say, by situation, with per-pilot groups
//   ([3:group]); written with the built-in lines when missing, re-read within a second of a save.
//   voice_0 .. voice_8 = part of a voice's name for that lance number (0 = replies from the lance);
//                    empty = picked automatically. VoiceCommand_voices.txt lists the voices found,
//                    including Windows 10/11's newer OneCore voices (Mark, George, Catherine, ...).
//   whisper_flash_attn = 1 Whisper's flash attention on, 0 off (default 0: on, the first start-up
//                    crashed on the CPU build)
//   whisper_gpu = 1 Whisper on the graphics card (Vulkan), 0 the CPU (default 0: the card is the game's -
//                    try it in VR; the round trip always uses the card). Needs a build with the Vulkan SDK
//                    (VC_HAVE_WHISPER_GPU); no Vulkan driver -> the CPU. Read at start-up only.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

#include "VanillaHudShared.h"
#include "LidarShared.h"
#include "LanceStatusShared.h"
#include "VoiceIntentShared.h" // 2026-10-19: V-LNC - what each lancemate was told, its open question, its voice
#include "TerrainShared.h"
#include "GlobalSettings.h" // 2026-10-07f: the settings now live in GlobalSettings.ini
#include <deque>
#include <initializer_list>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <mmsystem.h>
#include <memory>
#include <set>
#include <random>
#include <array>
#include <atomic>
#include <functional>
#include <chrono>
#ifdef VC_HAVE_TTS
#include <sherpa-onnx/c-api/c-api.h>
#endif
#ifdef VC_HAVE_WHISPER
#include <condition_variable>
#include <deque>
#include <thread>
#include "whisper.h"
#endif

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "winmm.lib")

namespace
{
// ---- config --------------------------------------------------------------------------------
struct Config
{
    int MinConf = 1, PttVk = 0, Speak = 1, Rate = 1, Callouts = 1, WhisperThreads = 4, WhisperFlashAttn = 0, WhisperGpu = 0, CallsignGuess = 0;
    std::wstring WhisperModel;
    int EnglishOnly = 1;       // voices: English-language ones only (a Japanese voice reading English sounded accented)
    std::wstring VoiceFor[9];  // voice_0 .. voice_8: part of a voice's name, per lance number (0 = the lance)
    int Recognizer = 1;        // recognizer = whisper (1, default) | windows (0, the fixed-phrase fallback)
    int Listen = 0;            // listen = always (0, default) | ptt (1) | both (2: always; key held = callsign optional)
    int WhisperFast = 1;       // whisper_trim = 1: a trimmed window with a safe margin (quicker; the old tight trim repeated itself)
    int Chatter = 2;           // chatter = 0 quiet (only what matters) .. 1 low .. 2 normal .. 3 busy (build 2026-10-05i)
} g_cfg;

std::wstring ExeDir()
{
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"" : s.substr(0, k + 1);
}

// GLOBAL SETTINGS (2026-10-07f): every VoiceCommand setting is in GlobalSettings.ini's [voice] section
// (include/GlobalSettings.h; edited with GlobalSettings.exe). VoiceCommand.ini is copied over once, the
// first time, and not read after that.
bool g_gsMigrated = false;

std::wstring VcIni() { return gs::Path(); }

void LoadConfig()
{
    g_gsMigrated = gs::MigrateSection(L"voice", ExeDir() + L"VoiceCommand.ini");
    gs::PublishPath(); // 2026-10-07h: tells LanceControl (in the game) where GlobalSettings.ini is
    const std::wstring ini = VcIni();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        WritePrivateProfileStringW(L"voice", L"min_confidence", L"1", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"ptt_vk", L"0", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"speak", L"1", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"rate", L"1", ini.c_str());
    }
    if (GetPrivateProfileIntW(L"voice", L"callouts", -1, ini.c_str()) == -1)
        WritePrivateProfileStringW(L"voice", L"callouts", L"1", ini.c_str()); // added in stage 2
    g_cfg.MinConf = (int)GetPrivateProfileIntW(L"voice", L"min_confidence", 1, ini.c_str());
    g_cfg.PttVk = (int)GetPrivateProfileIntW(L"voice", L"ptt_vk", 0, ini.c_str());
    g_cfg.Speak = (int)GetPrivateProfileIntW(L"voice", L"speak", 1, ini.c_str());
    g_cfg.Rate = gs::Int(L"voice", L"rate", 1); // (GetPrivateProfileInt reads a negative number as 0)
    g_cfg.Callouts = (int)GetPrivateProfileIntW(L"voice", L"callouts", 1, ini.c_str());
    // chatter: how much the lance talks on its own (callouts = 0 still switches them all off).
    //   0 quiet  - a lancemate down, a part lost, critical damage, missiles on you
    //   1 low    - + kills, heavy damage, ammo out, weapons lost, overheating, an ally in trouble,
    //              last enemy / area clear; long gaps between lines
    //   2 normal - + taking fire, hit hard, heavy contacts, VTOLs, your target's damage,
    //              outnumbered, ammo low, good hits by you, kill tallies (default)
    //   3 busy   - + spotted, a mech turning on you (with its bearing); short gaps, shorter cool-downs
    if (GetPrivateProfileIntW(L"voice", L"chatter", -1, ini.c_str()) == -1)
        WritePrivateProfileStringW(L"voice", L"chatter", L"2", ini.c_str());
    g_cfg.Chatter = std::max(0, std::min(3, (int)GetPrivateProfileIntW(L"voice", L"chatter", 2, ini.c_str())));
    // Free speech (stage 3): added to an existing ini the first time.
    wchar_t wm[MAX_PATH] = L"";
    if (GetPrivateProfileStringW(L"voice", L"whisper_model", L"\x01", wm, MAX_PATH, ini.c_str()) && wm[0] == 1)
    {
        WritePrivateProfileStringW(L"voice", L"whisper_model", gs::kDefWhisperModel, ini.c_str()); // release: beside the exe
        WritePrivateProfileStringW(L"voice", L"whisper_threads", L"4", ini.c_str());
        GetPrivateProfileStringW(L"voice", L"whisper_model", L"", wm, MAX_PATH, ini.c_str());
    }
    g_cfg.WhisperModel = gs::Resolve(wm); // release: a relative path is beside the exe
    g_cfg.WhisperThreads = (int)GetPrivateProfileIntW(L"voice", L"whisper_threads", 4, ini.c_str());
    if (GetPrivateProfileIntW(L"voice", L"whisper_flash_attn", -1, ini.c_str()) == (UINT)-1)
        WritePrivateProfileStringW(L"voice", L"whisper_flash_attn", L"0", ini.c_str()); // off: it crashed at start-up
    g_cfg.WhisperFlashAttn = (int)GetPrivateProfileIntW(L"voice", L"whisper_flash_attn", 0, ini.c_str());
    if (GetPrivateProfileIntW(L"voice", L"whisper_gpu", -1, ini.c_str()) == (UINT)-1)
        WritePrivateProfileStringW(L"voice", L"whisper_gpu", L"0", ini.c_str()); // 2026-10-14: off - the card is the game's
    g_cfg.WhisperGpu = (int)GetPrivateProfileIntW(L"voice", L"whisper_gpu", 0, ini.c_str());
    // 2026-10-14 (user, A/B test): a clipped callsign as the FIRST word ("Do" = Two, "Re" = Three, "Or" = Four)
    // 2026-10-15 (user, overnight round trip C2): ON by default (GlobalSettings checkbox "Guess a clipped callsign")
    if (GetPrivateProfileIntW(L"voice", L"callsign_guess", -1, ini.c_str()) == (UINT)-1)
        WritePrivateProfileStringW(L"voice", L"callsign_guess", L"1", ini.c_str());
    g_cfg.CallsignGuess = (int)GetPrivateProfileIntW(L"voice", L"callsign_guess", 1, ini.c_str());
    // Voices (added with the English-only / hidden-voices change): written once so they can be found.
    if (GetPrivateProfileIntW(L"voice", L"english_only", -1, ini.c_str()) == (UINT)-1)
    {
        WritePrivateProfileStringW(L"voice", L"english_only", L"1", ini.c_str());
        for (int i = 0; i <= 4; ++i)
            WritePrivateProfileStringW(L"voice", (L"voice_" + std::to_wstring(i)).c_str(), L"", ini.c_str());
    }
    g_cfg.EnglishOnly = (int)GetPrivateProfileIntW(L"voice", L"english_only", 1, ini.c_str());
    for (int i = 0; i <= 8; ++i)
    {
        wchar_t v[256] = L"";
        GetPrivateProfileStringW(L"voice", (L"voice_" + std::to_wstring(i)).c_str(), L"", v, 256, ini.c_str());
        g_cfg.VoiceFor[i] = v;
    }
    // Whisper as the recogniser (added with it): written once so they can be found.
    wchar_t rv[64] = L"";
    if (GetPrivateProfileStringW(L"voice", L"recognizer", L"\x01", rv, 64, ini.c_str()) && rv[0] == 1)
    {
        WritePrivateProfileStringW(L"voice", L"recognizer", L"whisper", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"listen", L"ptt", ini.c_str());     // 2026-10-09: push-to-talk by default
        WritePrivateProfileStringW(L"voice", L"whisper_trim", L"1", ini.c_str()); // (was 0 - against its own default)
    }
    GetPrivateProfileStringW(L"voice", L"recognizer", L"whisper", rv, 64, ini.c_str());
    g_cfg.Recognizer = _wcsicmp(rv, L"windows") == 0 ? 0 : 1;
    GetPrivateProfileStringW(L"voice", L"listen", L"ptt", rv, 64, ini.c_str());
    g_cfg.Listen = _wcsicmp(rv, L"ptt") == 0 ? 1 : (_wcsicmp(rv, L"both") == 0 ? 2 : 0);
    g_cfg.WhisperFast = (int)GetPrivateProfileIntW(L"voice", L"whisper_trim", 1, ini.c_str());
}

bool g_quiet = false; // 2026-10-14: the big parse audit - no console / heard-log line per sentence
void Log(const wchar_t *fmt, ...)
{
    if (g_quiet)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[1024];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf_s(b, _countof(b), _TRUNCATE, fmt, a);
    va_end(a);
    wprintf(L"[%02d:%02d:%02d] %ls\n", t.wHour, t.wMinute, t.wSecond, b);
    fflush(stdout);
}

// ---- the plugin's shared block ---------------------------------------------------------------
HANDLE               g_map = nullptr;
VanillaExportBlock  *g_blk = nullptr;
HANDLE               g_lmap = nullptr;
LidarExportBlock    *g_lid = nullptr;

bool Connect()
{
    if (g_blk && g_blk->Version == VHUD_VERSION)
        return true;
    if (g_blk)
    {
        UnmapViewOfFile(g_blk);
        CloseHandle(g_map);
        g_blk = nullptr;
        g_map = nullptr;
    }
    g_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, VHUD_MMF_NAME);
    if (!g_map)
        return false;
    g_blk = static_cast<VanillaExportBlock *>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VanillaExportBlock)));
    if (!g_blk || g_blk->Version != VHUD_VERSION)
    {
        if (g_blk)
        {
            Log(L"VanillaHud block is version %u, this build reads %u - rebuild them together", g_blk->Version, VHUD_VERSION);
            UnmapViewOfFile(g_blk);
        }
        CloseHandle(g_map);
        g_blk = nullptr;
        g_map = nullptr;
        return false;
    }
    Log(L"connected to the VanillaHud plugin (v%u)", VHUD_VERSION);
    return true;
}

// LidarExport: only whether you have a target locked (for a helpful "no target" reply).
int LockedCount()
{
    if (!g_lid)
    {
        g_lmap = OpenFileMappingW(FILE_MAP_READ, FALSE, LIDAR_MMF_NAME);
        if (g_lmap)
            g_lid = static_cast<LidarExportBlock *>(MapViewOfFile(g_lmap, FILE_MAP_READ, 0, 0, sizeof(LidarExportBlock)));
        if (!g_lid && g_lmap)
        {
            CloseHandle(g_lmap);
            g_lmap = nullptr;
        }
    }
    if (!g_lid || g_lid->Magic != LIDAR_MAGIC)
        return -1; // unknown
    return g_lid->SelectedCount;
}

// 2026-10-12 (lancecontrol-42): what kind of unit your lock is (LidarKind: LK_Mech, LK_Tank, LK_Turret, LK_Vtol), from
// LidarExport's scan (its Selected flag), or -1 when that is not known (no LidarExport, nothing marked selected).
int LockedKind()
{
    LockedCount(); // maps LidarExport's block if not yet
    if (!g_lid || g_lid->Magic != LIDAR_MAGIC)
        return -1;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_lid->FrameCounterEnd; // bug sweep 2026-10-17: END first (the writer bumps FrameCounter first)
        MemoryBarrier();
        uint32_t n = g_lid->ContactCount;
        if (n > (uint32_t)LIDAR_MAX_CONTACTS)
            n = LIDAR_MAX_CONTACTS;
        int kind = -1;
        for (uint32_t i = 0; i < n && kind < 0; ++i)
            if (g_lid->Contacts[i].Selected)
                kind = g_lid->Contacts[i].Kind;
        MemoryBarrier();
        if (g_lid->FrameCounterEnd != a || g_lid->FrameCounter != a)
        {
            Sleep(1);
            continue;
        }
        return kind;
    }
    return -1;
}

// Your COCKPIT heading (legs + torso twist, from LidarExport) - "forward" means where you look,
// not where your legs point. Falls back to the legs' heading if LidarExport is not there.
float CockpitYaw(float legsYaw)
{
    LockedCount(); // maps LidarExport's block if not yet
    if (!g_lid || g_lid->Magic != LIDAR_MAGIC)
        return legsYaw;
    return g_lid->PlayerYaw + g_lid->PlayerTorsoYaw;
}

// The nearest enemy LidarExport knows of (its ~3 s scan), or false.
bool NearestEnemy(float out[3], int weight = -1) // weight: -1 any, else LidarWeight (mechs of that class only)
{
    LockedCount();
    if (!g_lid || g_lid->Magic != LIDAR_MAGIC)
        return false;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_lid->FrameCounterEnd; // bug sweep 2026-10-17: END first (the writer bumps FrameCounter first)
        MemoryBarrier();
        const float px = g_lid->PlayerX, py = g_lid->PlayerY;
        uint32_t n = g_lid->ContactCount;
        if (n > (uint32_t)LIDAR_MAX_CONTACTS)
            n = LIDAR_MAX_CONTACTS;
        float best = 1e30f;
        bool found = false;
        for (uint32_t i = 0; i < n; ++i)
        {
            const LidarContact &c = g_lid->Contacts[i];
            if (c.Type != LCT_HostileMech) // 2026-10-11: hostiles only (neutral / unknown contacts are no target - "nothing within 40 m")
                continue;
            if (weight >= 0 && (c.Kind != LK_Mech || c.Weight != weight)) // "keep the lights busy" (2026-10-10)
                continue;
            const float d = (c.X - px) * (c.X - px) + (c.Y - py) * (c.Y - py);
            if (d < best)
            {
                best = d;
                out[0] = c.X;
                out[1] = c.Y;
                out[2] = c.Z;
                found = true;
            }
        }
        MemoryBarrier();
        if (g_lid->FrameCounterEnd == a && g_lid->FrameCounter == a)
            return found;
        Sleep(1);
    }
    return false;
}

// The lance table (seqlock: an even, unchanged TacSeq around the copy).
int ReadLance(VanillaExportBlock::TacMember *out)
{
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_blk->TacSeq;
        if (a & 1)
        {
            Sleep(1);
            continue;
        }
        MemoryBarrier();
        uint32_t n = g_blk->TacCount;
        if (n > (uint32_t)VanillaExportBlock::kTacMaxMembers)
            n = VanillaExportBlock::kTacMaxMembers;
        std::memcpy(out, (const void *)g_blk->TacMembers, sizeof(VanillaExportBlock::TacMember) * n);
        MemoryBarrier();
        if (g_blk->TacSeq == a)
            return (int)n;
    }
    return 0;
}

// NAV POINTS (2026-10-07k, VHUD v19): where nav point <letter> is (world cm), from LanceControl's list.
bool NavLoc(int letter, float out[3])
{
    if (!g_blk || letter < 0)
        return false;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_blk->NavSeq;
        if (a & 1)
        {
            Sleep(1);
            continue;
        }
        MemoryBarrier();
        bool found = false;
        const uint32_t n = std::min<uint32_t>(g_blk->NavCount, (uint32_t)VanillaExportBlock::kNavMax);
        for (uint32_t i = 0; i < n; ++i)
            if (g_blk->Navs[i].Letter == letter)
            {
                out[0] = g_blk->Navs[i].Loc[0];
                out[1] = g_blk->Navs[i].Loc[1];
                out[2] = g_blk->Navs[i].Loc[2];
                found = true;
            }
        MemoryBarrier();
        if (g_blk->NavSeq == a)
            return found;
    }
    return false;
}

std::wstring NavWord(int l); // below
// "nav alpha" / "navpoint bravo" / "nav point charlie" / "waypoint delta" at w[i]: the letter and how many words, else -1.
int NavWordsAt(const std::vector<std::wstring> &w, size_t i, size_t &len)
{
    if (i >= w.size())
        return -1;
    // 2026-10-14 (user, round trip): Whisper adds an "s" ("Nav Juliets", "Yankees", "Zulu's" -> "zulus") and glues the
    // letter on ("NavX-Ray" -> "navxray") - both still that nav letter. No NATO letter ends in "s" itself.
    auto letterOf = [](const std::wstring &x) -> int {
        for (int l = 0; l < kVhudNavLetters; ++l)
        {
            const std::wstring g = NavWord(l);
            if (x == g || (x.size() == g.size() + 1 && x.back() == L's' && x.compare(0, g.size(), g) == 0))
                return l;
        }
        return -1;
    };
    if (w[i].size() > 3 && w[i].compare(0, 3, L"nav") == 0)
    {
        const int l = letterOf(w[i].substr(3));
        if (l >= 0)
        {
            len = 1;
            return l;
        }
    }
    // 2026-10-15 (user, overnight round trip B9): "nav point" glued to the letter - "Nav Pointango" = nav point tango
    if (w[i] == L"nav" && i + 1 < w.size() && w[i + 1].size() > 5 && w[i + 1].compare(0, 4, L"poin") == 0)
    {
        // "pointango" shares the t: try after "point" and after "poin"
        int l = w[i + 1][4] == L't' ? letterOf(w[i + 1].substr(5)) : -1;
        if (l < 0)
            l = letterOf(w[i + 1].substr(4));
        if (l >= 0)
        {
            len = 2;
            return l;
        }
    }
    size_t k = 0;
    if (w[i] == L"nav" || w[i] == L"navpoint" || w[i] == L"waypoint")
        k = (w[i] == L"nav" && i + 1 < w.size() && w[i + 1] == L"point") ? 2 : 1;
    if (!k || i + k >= w.size())
        return -1;
    const int l = letterOf(w[i + k]);
    if (l >= 0)
        len = k + 1;
    return l;
}
std::wstring NavSaid(int letter) { return std::wstring(L"Nav ") + VhudNavName(letter); }
// A nav letter as one heard word: lower case, no hyphen ("X-ray" -> "xray"; HeardAs.txt joins "x ray")
std::wstring NavWord(int l)
{
    std::wstring g;
    for (const wchar_t *p = VhudNavName(l); *p; ++p)
        if (*p != L'-')
            g += (wchar_t)towlower(*p);
    return g;
}

// TACOPS ORDERS ANSWERED (2026-10-19, user): what WE sent each slot last (order, when) - an order the plugin then
// reports (LSE_OrderReceived) that matches it was a voice order, already answered by Execute; any other is TacOps's.
struct VcSent
{
    uint8_t   Order = 0xFF;
    ULONGLONG Ms = 0;
};
VcSent g_vcSent[16];
ULONGLONG g_vcNavMs = 0; // our last nav mark / unmark (hill offers) - not answered as yours

// Claim-then-write (see VanillaHudShared.h): HUDDisplay may be sending at the same moment.
void Send(uint8_t order, int slot, const float *loc, uint8_t pad0 = 0, uint8_t pad1 = 0) // pads: TCO_CalledShot
{
    if (slot >= 0 && slot < 16)
    {
        g_vcSent[slot].Order = order;
        g_vcSent[slot].Ms = GetTickCount64();
    }
    if (order == VanillaExportBlock::TCO_NavMark || order == VanillaExportBlock::TCO_NavUnmark)
        g_vcNavMs = GetTickCount64();
    uint32_t n = 0;
    for (;;)
    {
        n = g_blk->TacCmdHead;
        if ((uint32_t)InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&g_blk->TacCmdHead), (LONG)(n + 1), (LONG)n) == n)
            break;
    }
    VanillaExportBlock::TacCmd &c = g_blk->TacCmds[n % VanillaExportBlock::kTacCmdRing];
    c.Seq = 0;
    MemoryBarrier();
    c.Order = order;
    c.Slot = (uint8_t)slot;
    c.Loc[0] = loc ? loc[0] : 0.0f;
    c.Loc[1] = loc ? loc[1] : 0.0f;
    c.Loc[2] = loc ? loc[2] : 0.0f;
    c.Pad[0] = pad0;
    c.Pad[1] = pad1;
    MemoryBarrier();
    c.Seq = n + 1;
    MemoryBarrier();
}
// 2026-10-16 (lancecontrol-61): an order on the EXACT unit when its address is known (Loc[2] == kTacLocActor, as the
// patch-through lock) - a named attack / called shot went to whatever stood nearest its point (a dropship turret).
void SendUnit(uint8_t order, int slot, const float *p, uint32_t lo, uint32_t hi, uint8_t pad0 = 0, uint8_t pad1 = 0)
{
    if (!lo && !hi)
    {
        Send(order, slot, p, pad0, pad1);
        return;
    }
    float loc[3] = {0.0f, 0.0f, VanillaExportBlock::kTacLocActor};
    std::memcpy(&loc[0], &lo, 4);
    std::memcpy(&loc[1], &hi, 4);
    Send(order, slot, loc, pad0, pad1);
}

// ---- speech out ------------------------------------------------------------------------------
ISpVoice *g_voice = nullptr;
struct VoiceEntry
{
    ISpObjectToken *Token;
    std::wstring    Name;    // e.g. "Microsoft George - English (United Kingdom)"
    std::wstring    Lang;    // the Language attribute, hex LCIDs ("809")
    bool            OneCore; // one of Windows' newer voices, hidden from SAPI programs by default
};
std::vector<VoiceEntry> g_voices;  // the ones lancemates may use
ISpObjectToken *g_slotVoice[9] = {}; // chosen per lance number

std::wstring TokenAttr(ISpObjectToken *t, const wchar_t *name)
{
    std::wstring r;
    ISpDataKey *k = nullptr;
    if (SUCCEEDED(t->OpenKey(L"Attributes", &k)) && k)
    {
        wchar_t *v = nullptr;
        if (SUCCEEDED(k->GetStringValue(name, &v)) && v)
        {
            r = v;
            CoTaskMemFree(v);
        }
        k->Release();
    }
    return r;
}

// English if any of its LCIDs has the English primary language (0x09): 409 US, 809 UK, c09 AU...
bool IsEnglish(const std::wstring &lang)
{
    size_t i = 0;
    while (i < lang.size())
    {
        size_t j = lang.find(L';', i);
        if (j == std::wstring::npos)
            j = lang.size();
        const unsigned long id = wcstoul(lang.substr(i, j - i).c_str(), nullptr, 16);
        if ((id & 0x3FF) == 0x09)
            return true;
        i = j + 1;
    }
    return false;
}

// The voices of one registry category. The OneCore one (Speech_OneCore) holds Windows 10/11's
// newer voices (Mark, George, Catherine, ...), which SAPI does not list by default but can speak.
void AddVoices(const wchar_t *categoryId, bool oneCore, std::vector<VoiceEntry> &all)
{
    ISpObjectTokenCategory *cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void **)&cat)) || !cat)
        return;
    if (SUCCEEDED(cat->SetId(categoryId, FALSE)))
    {
        IEnumSpObjectTokens *en = nullptr;
        if (SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &en)) && en)
        {
            ISpObjectToken *t = nullptr;
            while (en->Next(1, &t, nullptr) == S_OK)
            {
                VoiceEntry e{t, TokenAttr(t, L"Name"), TokenAttr(t, L"Language"), oneCore};
                if (e.Name.empty())
                {
                    wchar_t *d = nullptr;
                    if (SUCCEEDED(t->GetStringValue(nullptr, &d)) && d)
                    {
                        e.Name = d;
                        CoTaskMemFree(d);
                    }
                }
                bool dup = false; // the same voice in both lists (David, Zira): the desktop one kept
                for (auto &x : all)
                    dup |= !x.Name.empty() && _wcsicmp(x.Name.c_str(), e.Name.c_str()) == 0;
                if (dup)
                    t->Release();
                else
                    all.push_back(e);
            }
            en->Release();
        }
    }
    cat->Release();
}

bool NameHas(const std::wstring &name, const std::wstring &part)
{
    std::wstring a = name, b = part;
    for (auto &c : a)
        c = (wchar_t)towlower(c);
    for (auto &c : b)
        c = (wchar_t)towlower(c);
    return !b.empty() && a.find(b) != std::wstring::npos;
}

void InitVoices()
{
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, (void **)&g_voice)))
    {
        Log(L"no text-to-speech available - replies are printed only");
        return;
    }
    g_voice->SetRate(g_cfg.Rate);
    std::vector<VoiceEntry> all;
    AddVoices(SPCAT_VOICES, false, all);
    AddVoices(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices", true, all);

    // Every voice found, and whether lancemates may use it - also to VoiceCommand_voices.txt, so a
    // name can be picked in GlobalSettings (VOICE COMMAND > VOICES) or typed into voice_N in GlobalSettings.ini.
    std::wstring list = L"Voices found (pick one per pilot in GlobalSettings > VOICE COMMAND > VOICES; voice_N in GlobalSettings.ini [voice] takes any part of a name):\r\n";
    for (auto &e : all)
    {
        const bool en = IsEnglish(e.Lang);
        const bool use = en || !g_cfg.EnglishOnly;
        list += (use ? L"  [used]    " : L"  [skipped] ") + e.Name + L"  (language " + e.Lang + (e.OneCore ? L", Windows OneCore voice)" : L")") +
                (use ? L"" : L" - not English") + L"\r\n";
        if (use)
            g_voices.push_back(e);
        else
            e.Token->Release();
    }
    for (size_t i = 0, j; i < list.size(); i = j + 2) // one log line per voice (the log line is capped)
    {
        j = list.find(L"\r\n", i);
        if (j == std::wstring::npos)
            j = list.size();
        Log(L"%ls", list.substr(i, j - i).c_str());
    }
    FILE *f = nullptr;
    if (_wfopen_s(&f, (ExeDir() + L"VoiceCommand_voices.txt").c_str(), L"wb") == 0 && f)
    {
        fwrite("\xEF\xBB\xBF", 1, 3, f);
        const int n = WideCharToMultiByte(CP_UTF8, 0, list.c_str(), (int)list.size(), nullptr, 0, nullptr, nullptr);
        std::string u((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, list.c_str(), (int)list.size(), &u[0], n, nullptr, nullptr);
        fwrite(u.data(), 1, u.size(), f);
        fclose(f);
    }
    if (g_voices.empty())
    {
        Log(L"no usable voice (english_only = %d) - the Windows default voice speaks for everyone", g_cfg.EnglishOnly);
        return;
    }
    // Per lance number: the voice named in voice_N, else a different voice each (the newer OneCore
    // voices first - they sound better), repeating only when there are more lancemates than voices.
    std::vector<size_t> order;
    for (size_t i = 0; i < g_voices.size(); ++i)
        if (g_voices[i].OneCore)
            order.push_back(i);
    for (size_t i = 0; i < g_voices.size(); ++i)
        if (!g_voices[i].OneCore)
            order.push_back(i);
    size_t next = 0;
    for (int s = 0; s <= 8; ++s)
    {
        const VoiceEntry *pick = nullptr;
        if (!g_cfg.VoiceFor[s].empty())
        {
            for (auto &e : g_voices)
                if (NameHas(e.Name, g_cfg.VoiceFor[s]))
                {
                    pick = &e;
                    break;
                }
            if (!pick)
                Log(L"voice_%d = \"%ls\": no usable voice has that in its name - one is picked instead", s, g_cfg.VoiceFor[s].c_str());
        }
        if (!pick)
            pick = &g_voices[order[next++ % order.size()]];
        g_slotVoice[s] = pick->Token;
        if (s >= 2 && s <= 4)
            Log(L"lancemate %d speaks with %ls", s, pick->Name.c_str());
    }
}

// Lines are spoken one at a time, each in its speaker's voice (SayTick, from the main loop): a
// voice switched while an earlier line was still queued could speak that line in the wrong voice
// - "lance, status" has every lancemate answer in turn.
// STEP-ON (build 2026-10-05o): a line also carries what it is about and when it happened, so a
// line that waited too long is dropped (StaleMs), an urgent one can break in on the transmission
// on the air ("Break, break!"), and the pilot it stepped on picks up again afterwards (ResumeTick).
struct SayJob
{
    int          Slot = 0;
    std::wstring Text;
    uint16_t     Type = 0;       // the callout kind (0 = a reply / report)
    ULONGLONG    Ms = 0;         // when it happened
    bool         Urgent = false; // steps on the transmission on the air
    int          Lead = 0;       // sentences in front that are not the message (a resume's "Two, continuing.")
    std::wstring Voice;          // a Kokoro voice (name or id) instead of the slot's - GlobalSettings' TEST (2026-10-07g)
};
std::deque<SayJob> g_sayQ;
std::wstring g_sayVoice; // set around a Say() to speak it in that voice (GlobalSettings' TEST)
const uint16_t K_Position = 200; // an answer giving a position ("your 3 o'clock, 300 metres") - stale sooner
bool RadioActive();                              // KOKORO VOICES + RADIO, below: the speech worker takes the lines
bool RadioBusy();
bool RadioKeyed();       // a transmission is on the air
int  RadioAirSlot();     // whose transmission is on the air (-1 = none) - 2026-10-06g
bool RadioUrgentOnAir(); // an urgent line is handed over and not yet played out
void RadioSpeak(const SayJob &j);

// What the stepped-on pilot still has to say (written by the playback thread, said by ResumeTick).
struct ResumeNote
{
    bool                      Have = false;
    int                       Slot = 0;
    uint16_t                  Type = 0;
    ULONGLONG                 Ms = 0;
    bool                      Mid = false; // cut mid-sentence: "I say again"
    std::vector<std::wstring> Rest;        // the sentences not yet heard
};
std::mutex g_resumeMx;
ResumeNote g_resume;

// A line's sentences ("Two here. Armour's good." -> two parts).
std::vector<std::wstring> SplitSentences(const std::wstring &tx)
{
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (size_t k = 0; k < tx.size(); ++k)
    {
        cur += tx[k];
        if ((tx[k] == L'.' || tx[k] == L'!' || tx[k] == L'?') && (k + 1 == tx.size() || tx[k + 1] == L' '))
        {
            const size_t f = cur.find_first_not_of(L' ');
            if (f != std::wstring::npos)
                parts.push_back(cur.substr(f));
            cur.clear();
        }
    }
    const size_t f = cur.find_first_not_of(L' ');
    if (f != std::wstring::npos)
        parts.push_back(cur.substr(f));
    if (parts.empty())
        parts.push_back(tx);
    return parts;
}

ULONGLONG g_lastSayMs = 0; // when anything was last said (callouts, replies, reports) - build 2026-10-05h

// ANSWERS FIRST (build 2026-10-06a): while RunSpokenActions answers you, what is said goes ahead of the
// chatter, and the first line cuts off whoever is talking (your transmission stepped on them).
bool               g_sayPriority = false;
bool               g_cutArmed = false;
std::vector<int>  *g_sayCollect = nullptr; // who answered (RunSpokenActions)
std::wstring       g_lastLine[9];          // each pilot's last line - "what was that?"
int                g_lastSpeaker = 0;
ULONGLONG          g_lastSpeakMs = 0;
std::atomic<int>       g_lastAnswerer{0};  // the pilot who last answered a question of yours alone:
std::atomic<ULONGLONG> g_lastAnswerMs{0};  // an order with no callsign goes to it (60 s)
// PATCH-THROUGH (build 2026-10-06c): a pilot that sees whoever is firing on you offers to put it on
// your screen; "yes" / "patch it through" within 15 s does (TCO_LockTarget), "negative" drops it.
struct PatchOffer
{
    int          Slot = 0;
    uint32_t     Lo = 0, Hi = 0;
    std::wstring Name;
};
PatchOffer             g_offer;        // main thread only
std::atomic<ULONGLONG> g_offerMs{0};   // when it was offered (0 = none) - Whisper's thread reads it
std::atomic<ULONGLONG> g_hillMs{0};    // HIGH GROUND (2026-10-14): when "which hill?" was asked (0 = no offer open)
std::atomic<uint32_t>  g_hillLetters{0}; // ... the candidates' letters (bit = VhudNavName index) - Whisper's thread reads it
void RadioCut(int noResumeSlot);           // KOKORO VOICES + RADIO, below
void RadioDropReplies(int slot);           // ... and its replies not yet on the air (CORRECTIONS)

void Say(int slot, const std::wstring &text, uint16_t type = 0, ULONGLONG ms = 0, bool urgent = false, int lead = 0)
{
    g_lastSayMs = GetTickCount64();
    Log(L"  > %ls", text.c_str());
    if (slot >= 1 && slot <= 8)
    {
        g_lastLine[slot] = text;
        g_lastSpeaker = slot;
        g_lastSpeakMs = g_lastSayMs;
    }
    if (g_sayCollect)
        g_sayCollect->push_back(slot);
    if ((!g_voice && !RadioActive()) || !g_cfg.Speak)
        return;
    if (g_sayPriority && RadioActive())
    {
        urgent = true; // ahead of the chatter
        if (g_cutArmed)
        {
            g_cutArmed = false;
            RadioCut(slot); // the one answering does not pick up what it was saying
        }
    }
    SayJob j;
    j.Slot = slot;
    j.Text = text;
    j.Type = type;
    j.Ms = ms ? ms : g_lastSayMs;
    j.Urgent = urgent;
    j.Lead = lead;
    j.Voice = g_sayVoice;
    g_sayQ.push_back(j);
}

void SayTick()
{
    if (g_sayQ.empty())
        return;
    if (RadioActive())
    {
        // Kokoro and / or the radio filter: handed over at once, in order - the next line is
        // synthesised while the current one plays.
        while (!g_sayQ.empty())
        {
            RadioSpeak(g_sayQ.front());
            g_sayQ.pop_front();
        }
        return;
    }
    if (!g_voice)
        return;
    SPVOICESTATUS st = {};
    if (SUCCEEDED(g_voice->GetStatus(&st, nullptr)) && st.dwRunningState == SPRS_IS_SPEAKING)
        return;
    const int slot = g_sayQ.front().Slot;
    const std::wstring text = g_sayQ.front().Text;
    g_sayQ.pop_front();
    const int s = (slot < 0 || slot > 8) ? 0 : slot;
    if (g_slotVoice[s] && FAILED(g_voice->SetVoice(g_slotVoice[s])))
    {
        Log(L"that voice would not load - the default voice speaks instead");
        g_slotVoice[s] = nullptr;
        g_voice->SetVoice(nullptr);
    }
    g_voice->Speak(text.c_str(), SPF_ASYNC, nullptr);
}

const wchar_t *kNum[9] = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight"};
// A lance number as a word: "Two".
std::wstring NumberName(int slot)
{
    std::wstring s = (slot >= 1 && slot <= 8) ? kNum[slot] : L"lancemate";
    s[0] = (wchar_t)towupper(s[0]);
    return s;
}

std::wstring PilotNameOf(int slot); // PILOT NAMES, below - "" until known

// A pilot's name as a callsign: the last name of a first-and-last name ("Jacob Hale" -> "Hale",
// build 2026-10-05o), the name itself otherwise.
std::wstring CallName(const std::wstring &full)
{
    size_t e = full.find_last_not_of(L" \"'");
    if (e == std::wstring::npos)
        return L"";
    size_t b = full.find_last_of(L' ', e);
    b = b == std::wstring::npos ? 0 : b + 1;
    while (b <= e && (full[b] == L'"' || full[b] == L'\''))
        ++b;
    return b <= e ? full.substr(b, e - b + 1) : full;
}

// What a lancemate is called out loud: its pilot's (last) name once known ("Sledge"), else its number.
std::wstring Callsign(int slot)
{
    const std::wstring n = CallName(PilotNameOf(slot));
    return n.empty() ? NumberName(slot) : n;
}

// ---- VOICE LINES (VoiceLines.txt beside the exe) ---------------------------------------------
// Everything the lancemates say, editable without a rebuild. Grouped by situation; a group may
// hold any number of lines and one is picked at random each time. "[3:group]" gives lancemate 3
// lines of its own for that group (the others keep the shared ones). Placeholders in {braces}
// are filled in when spoken. Missing / empty groups use the built-in lines below, so a mistake
// never leaves a pilot silent. Written with these defaults when missing; re-read within a second
// of being saved (checked from the main loop - lines are only ever used on the main thread).
// Reading a line is a map lookup: no latency is added to speech.
struct LineGroup
{
    const wchar_t               *Key;
    const wchar_t               *About; // written above the group in the file
    std::vector<const wchar_t *> Lines; // the built-in lines
};
const std::vector<LineGroup> &LineGroups()
{
    // The built-in lines (build 2026-10-05g: the phrase tree) live in VoiceLines.default.txt;
    // gen_voicelines.py turns it into VoiceLinesDefault.inc.
    static const std::vector<LineGroup> g = {
#include "VoiceLinesDefault.inc"
    };
    return g;
}

const LineGroup *FindLineGroup(const std::wstring &key)
{
    for (auto &g : LineGroups())
        if (key == g.Key)
            return &g;
    return nullptr;
}

std::map<std::wstring, std::vector<std::wstring>> g_lines; // "group" or "N:group" -> lines
FILETIME  g_linesTime = {};
ULONGLONG g_linesCheckMs = 0;

std::wstring LinesPath() { return ExeDir() + L"VoiceLines.txt"; }

void WriteFileUtf8(const std::wstring &path, const std::wstring &w)
{
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return;
    fwrite("\xEF\xBB\xBF", 1, 3, f);
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n > 0)
    {
        std::string u((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
        fwrite(u.c_str(), 1, u.size(), f);
    }
    fclose(f);
}

void WriteDefaultLines()
{
    std::wstring t =
        L"# VoiceLines.txt - everything the lancemates say. Edit and save: VoiceCommand picks it up\r\n"
        L"# within a second, no restart.   VoiceLines format 2 (phrase tree) - keep this line\r\n"
        L"#\r\n"
        L"#   [group]      lines for every lancemate; one is picked each time (not one said in the last\r\n"
        L"#                minute while the group has another)\r\n"
        L"#   [3:group]    lines for lancemate 3 only (any number 1-8) - the others keep the shared ones\r\n"
        L"#   [Sledge:group]  lines for the pilot called Sledge, whatever its number (names first, then\r\n"
        L"#                numbers, then the shared lines)\r\n"
        L"#\r\n"
        L"# Inside a line:\r\n"
        L"#   (a|b|c)      one of them is picked; an empty choice makes a part optional: moving( now|)\r\n"
        L"#                choices can be nested: (heading|moving) (out|over)( (now|right away)|)\r\n"
        L"#   {@pool_go}   a line from another group - the pool_ groups are shared pieces. Add your own\r\n"
        L"#                [pool_anything] groups; [Sledge:pool_ack] gives one pilot its own pieces\r\n"
        L"#   {callsign}   placeholders like this are filled in when spoken; each group's note lists them\r\n"
        L"#                ({callsign} works everywhere - the pilot's name, else the lance number)\r\n"
        L"# Spaces and commas left by an empty choice are tidied, a / an is fixed, and a new sentence\r\n"
        L"# gets its capital. However many pieces, a line is ONE radio transmission.\r\n"
        L"# Check a group without playing: VoiceCommand.exe --preview order_move 20\r\n"
        L"#   (--preview all lists how many different lines each group can make)\r\n"
        L"#\r\n"
        L"# One line per row; add as many as you like. Lines starting with # are notes. A group that is\r\n"
        L"# missing or empty uses the built-in lines. Delete this file to get the original lines back.\r\n"
        L"#\r\n"
        L"# Example - lancemate 3 with lines of its own:\r\n"
        L"#   [3:callout_ammo_low]\r\n"
        L"#   {callsign}, (I'm almost dry|nearly out) on {weapon}!\r\n"
        L"#   {callsign} here, {weapon} ammo's (nearly gone|running out).\r\n";
    for (auto &g : LineGroups())
    {
        t += L"\r\n# " + std::wstring(g.About) + L"\r\n[" + g.Key + L"]\r\n";
        for (auto *l : g.Lines)
            t += std::wstring(l) + L"\r\n";
    }
    WriteFileUtf8(LinesPath(), t);
}

void LoadLines()
{
    g_lines.clear();
    FILE *f = nullptr;
    if (_wfopen_s(&f, LinesPath().c_str(), L"rb") != 0 || !f)
        return;
    std::string raw;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        raw.append(buf, got);
    fclose(f);
    if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0)
        raw.erase(0, 3);
    std::wstring w;
    if (!raw.empty())
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
        if (n > 0)
        {
            w.resize((size_t)n);
            MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &w[0], n);
        }
    }
    std::wstring group;
    int lineNo = 0, count = 0, problems = 0;
    size_t i = 0;
    while (i <= w.size())
    {
        size_t j = w.find(L'\n', i);
        if (j == std::wstring::npos)
            j = w.size();
        std::wstring l = w.substr(i, j - i);
        i = j + 1;
        ++lineNo;
        const size_t a = l.find_first_not_of(L" \t\r"), b = l.find_last_not_of(L" \t\r");
        l = a == std::wstring::npos ? L"" : l.substr(a, b - a + 1);
        if (l.empty() || l[0] == L'#' || l[0] == L';')
            continue;
        if (l[0] == L'[' && l.back() == L']')
        {
            group = l.substr(1, l.size() - 2);
            std::wstring key = group;
            const size_t c = group.find(L':');
            if (c != std::wstring::npos)
            {
                std::wstring who = group.substr(0, c);
                key = group.substr(c + 1);
                bool digits = !who.empty();
                for (wchar_t ch : who)
                    digits &= iswdigit(ch) != 0;
                if (digits)
                {
                    // [3:group] - a lance number
                    const int s = _wtoi(who.c_str());
                    if (s < 1 || s > 8)
                    {
                        Log(L"VoiceLines.txt line %d: [%ls] - the number before ':' must be 1-8; group ignored", lineNo, group.c_str());
                        ++problems;
                        group.clear();
                        continue;
                    }
                    group = std::to_wstring(s) + L":" + key;
                }
                else
                {
                    // [Sledge:group] - a pilot, by name (any case)
                    for (auto &ch : who)
                        ch = (wchar_t)towlower(ch);
                    group = L"@" + who + L":" + key;
                }
            }
            if (!FindLineGroup(key) && key.compare(0, 5, L"pool_") != 0) // pool_ groups: your own pieces
            {
                Log(L"VoiceLines.txt line %d: unknown group [%ls] - ignored (check the spelling against the list in the file)",
                    lineNo, key.c_str());
                ++problems;
                group.clear();
            }
            continue;
        }
        if (group.empty())
            continue; // lines before any group, or under an ignored one
        g_lines[group].push_back(l);
        ++count;
    }
    Log(L"voice lines: %d line(s) in %zu group(s) from VoiceLines.txt%ls", count, g_lines.size(),
        problems ? L" - see the warnings above" : L"");
}

std::string Utf8(const std::wstring &w); // below
std::wstring LockedEnemyName();          // SITUATION CALLOUTS, below

// From the main loop: written when missing, re-read when saved.
void LinesTick(bool force = false)
{
    const ULONGLONG now = GetTickCount64();
    if (!force && now - g_linesCheckMs < 1000)
        return;
    g_linesCheckMs = now;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(LinesPath().c_str(), GetFileExInfoStandard, &fa))
    {
        WriteDefaultLines();
        Log(L"voice lines: VoiceLines.txt written with the built-in lines - edit it to change what the pilots say");
        if (!GetFileAttributesExW(LinesPath().c_str(), GetFileExInfoStandard, &fa))
            return; // could not be written: built-in lines
    }
    if (!force && CompareFileTime(&fa.ftLastWriteTime, &g_linesTime) == 0)
        return;
    // A VoiceLines.txt from before the phrase tree (no "VoiceLines format 2" line): kept as
    // VoiceLines.v1.txt and a new one written, so the new lines are heard.
    {
        static bool checked = false;
        if (!checked)
        {
            checked = true;
            FILE *f = nullptr;
            std::string head;
            if (_wfopen_s(&f, LinesPath().c_str(), L"rb") == 0 && f)
            {
                char buf[1024];
                head.assign(buf, fread(buf, 1, sizeof(buf), f));
                fclose(f);
            }
            if (head.find("VoiceLines format 2") == std::string::npos)
            {
                std::wstring keep = ExeDir() + L"VoiceLines.v1.txt";
                for (int k = 2; GetFileAttributesW(keep.c_str()) != INVALID_FILE_ATTRIBUTES && k < 100; ++k)
                    keep = ExeDir() + L"VoiceLines.v1 (" + std::to_wstring(k) + L").txt";
                if (MoveFileW(LinesPath().c_str(), keep.c_str()))
                {
                    WriteDefaultLines();
                    Log(L"voice lines: the old VoiceLines.txt is kept as %ls - a new one with the phrase tree was written",
                        keep.c_str());
                    if (!GetFileAttributesExW(LinesPath().c_str(), GetFileExInfoStandard, &fa))
                        return;
                }
            }
        }
    }
    g_linesTime = fa.ftLastWriteTime;
    LoadLines();
    // Groups added in a newer build than the file: appended once (with their notes), so every
    // group can be found and edited there.
    {
        static bool appended = false;
        if (!appended)
        {
            appended = true;
            FILE *f = nullptr;
            std::string raw;
            if (_wfopen_s(&f, LinesPath().c_str(), L"rb") == 0 && f)
            {
                char buf[4096];
                size_t got;
                while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
                    raw.append(buf, got);
                fclose(f);
            }
            std::wstring add;
            int groups = 0;
            for (auto &g : LineGroups())
                if (raw.find("[" + Utf8(g.Key) + "]") == std::string::npos)
                {
                    add += L"\r\n# " + std::wstring(g.About) + L"\r\n[" + g.Key + L"]\r\n";
                    for (auto *l : g.Lines)
                        add += std::wstring(l) + L"\r\n";
                    ++groups;
                }
            if (groups && !raw.empty() && _wfopen_s(&f, LinesPath().c_str(), L"ab") == 0 && f)
            {
                const std::string u = Utf8(L"\r\n# ---- added by a newer VoiceCommand ----\r\n" + add);
                fwrite(u.data(), 1, u.size(), f);
                fclose(f);
                Log(L"voice lines: %d new group(s) added to the end of VoiceLines.txt", groups);
            }
        }
    }
}

// No repeats (build 2026-10-05f): a line said in the last minute (by anyone) is not picked again
// while the group has one that was not; if all were, the one said longest ago is.
std::map<std::wstring, ULONGLONG> g_lineSaid;
const ULONGLONG kNoRepeatMs = 60000;

template <class S> std::wstring PickFresh(const std::vector<S> &lines, size_t minLen = 0)
{
    static std::mutex mx; // Line() runs on the main and the Whisper threads
    std::lock_guard<std::mutex> lk(mx);
    const ULONGLONG now = GetTickCount64();
    std::vector<size_t> fresh;
    size_t oldest = 0;
    ULONGLONG oldestAt = ~0ull;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::wstring key(lines[i]);
        auto it = key.size() < minLen ? g_lineSaid.end() : g_lineSaid.find(key);
        const ULONGLONG at = it == g_lineSaid.end() ? 0 : it->second;
        if (at == 0 || now - at >= kNoRepeatMs)
            fresh.push_back(i);
        if (at < oldestAt)
        {
            oldestAt = at;
            oldest = i;
        }
    }
    const size_t k = fresh.empty() ? oldest : fresh[(size_t)rand() % fresh.size()];
    if (std::wstring(lines[k]).size() >= minLen)
        g_lineSaid[std::wstring(lines[k])] = now;
    if (g_lineSaid.size() > 2000) // forget the old ones
        for (auto it = g_lineSaid.begin(); it != g_lineSaid.end();)
            it = now - it->second >= kNoRepeatMs ? g_lineSaid.erase(it) : std::next(it);
    return lines[k];
}

// One line of a group, for the lancemate in this lance number (its own lines first), with the
// placeholders filled in. {callsign} defaults to the speaker's own. cap: first letter upper case
// (off for a piece that goes inside another line, like {reply}).
// ---- THE PHRASE TREE (build 2026-10-05g) -------------------------------------------------------
// Inside any line of VoiceLines.txt:
//   (a|b|c)    one of them, picked fresh (not one said in the last minute while there is another);
//              an empty choice makes a part optional - "moving( now|)"; choices nest
//   {@group}   a line from another group, its own choices expanded - usually a pool_ group, the
//              shared pieces (pool_ack, pool_go, ...). The pilot's own [Name:pool_x] lines count.
// Then the line is tidied: spaces and commas around missing parts, a / an, capitals after a full
// stop. Several sentences stay ONE transmission. "VoiceCommand.exe --preview <group> [n] [lance
// number]" prints n random lines and how many different ones the group can make.

// The lines a group has for this lancemate: the pilot's own, then the lance number's, then the
// shared ones in the file, then the built-in ones.
std::vector<std::wstring> GroupLines(const std::wstring &group, int slot)
{
    if (slot >= 1 && slot <= 8)
    {
        const std::wstring full = PilotNameOf(slot);
        for (std::wstring pn : {full, CallName(full)}) // [Jacob Hale:group] or [Hale:group]
        {
            if (pn.empty())
                continue;
            for (auto &ch : pn)
                ch = (wchar_t)towlower(ch);
            auto it = g_lines.find(L"@" + pn + L":" + group);
            if (it != g_lines.end() && !it->second.empty())
                return it->second;
        }
        auto it = g_lines.find(std::to_wstring(slot) + L":" + group);
        if (it != g_lines.end() && !it->second.empty())
            return it->second;
    }
    auto it = g_lines.find(group);
    if (it != g_lines.end() && !it->second.empty())
        return it->second;
    std::vector<std::wstring> o;
    if (const LineGroup *g = FindLineGroup(group))
        for (auto *l : g->Lines)
            o.push_back(l);
    return o;
}

// The ')' that closes the '(' at i (npos if none).
size_t CloseParen(const std::wstring &s, size_t i)
{
    int d = 0;
    for (size_t j = i; j < s.size(); ++j)
    {
        if (s[j] == L'(')
            ++d;
        else if (s[j] == L')' && --d == 0)
            return j;
    }
    return std::wstring::npos;
}

// "a|b(c|d)|e" -> a, b(c|d), e
std::vector<std::wstring> SplitChoices(const std::wstring &s)
{
    std::vector<std::wstring> o(1);
    int d = 0;
    for (wchar_t c : s)
    {
        if (c == L'(')
            ++d;
        else if (c == L')')
            --d;
        if (c == L'|' && d == 0)
            o.emplace_back();
        else
            o.back() += c;
    }
    return o;
}

std::wstring ExpandText(const std::wstring &s, int slot, int depth)
{
    if (depth > 8)
        return L"";
    std::wstring o;
    for (size_t i = 0; i < s.size();)
    {
        if (s[i] == L'(')
        {
            const size_t j = CloseParen(s, i);
            if (j != std::wstring::npos)
            {
                o += ExpandText(PickFresh(SplitChoices(s.substr(i + 1, j - i - 1)), 4), slot, depth + 1);
                i = j + 1;
                continue;
            }
        }
        if (s[i] == L'{' && i + 1 < s.size() && s[i + 1] == L'@')
        {
            const size_t j = s.find(L'}', i);
            if (j != std::wstring::npos)
            {
                const std::vector<std::wstring> ls = GroupLines(s.substr(i + 2, j - i - 2), slot);
                if (!ls.empty())
                    o += ExpandText(PickFresh(ls), slot, depth + 1);
                i = j + 1;
                continue;
            }
        }
        o += s[i++];
    }
    return o;
}

// How many different lines a piece can make (for --preview).
double CountText(const std::wstring &s, int slot, int depth)
{
    if (depth > 8)
        return 1.0;
    double n = 1.0;
    for (size_t i = 0; i < s.size();)
    {
        if (s[i] == L'(')
        {
            const size_t j = CloseParen(s, i);
            if (j != std::wstring::npos)
            {
                double sum = 0.0;
                for (auto &c : SplitChoices(s.substr(i + 1, j - i - 1)))
                    sum += CountText(c, slot, depth + 1);
                n *= std::max(1.0, sum);
                i = j + 1;
                continue;
            }
        }
        if (s[i] == L'{' && i + 1 < s.size() && s[i + 1] == L'@')
        {
            const size_t j = s.find(L'}', i);
            if (j != std::wstring::npos)
            {
                double sum = 0.0;
                for (auto &l : GroupLines(s.substr(i + 2, j - i - 2), slot))
                    sum += CountText(l, slot, depth + 1);
                n *= std::max(1.0, sum);
                i = j + 1;
                continue;
            }
        }
        ++i;
    }
    return n;
}

double CountGroup(const std::wstring &group, int slot)
{
    double sum = 0.0;
    for (auto &l : GroupLines(group, slot))
        sum += CountText(l, slot, 0);
    return sum;
}

// "an" before a vowel sound: an LRM, an SRM, an AC 10, an hour; a UAV, a unit, a one.
bool AnBefore(const std::wstring &w)
{
    if (w.empty())
        return false;
    std::wstring l;
    for (wchar_t c : w)
        l += (wchar_t)towlower(c);
    bool acro = w.size() >= 2 && iswupper(w[0]) && (iswupper(w[1]) || iswdigit(w[1]));
    if (acro || w.size() == 1)
        return wcschr(L"aefhilmnorsx", l[0]) != nullptr;
    if (l.compare(0, 4, L"hour") == 0 || l.compare(0, 6, L"honest") == 0 || l.compare(0, 5, L"honou") == 0)
        return true;
    if (l.compare(0, 3, L"one") == 0 || l.compare(0, 4, L"once") == 0)
        return false;
    if (l[0] == L'u')
    {
        for (const wchar_t *p : {L"uni", L"use", L"usu", L"uti", L"ura", L"ure", L"uk"})
            if (l.compare(0, wcslen(p), p) == 0)
                return false;
        return true;
    }
    return wcschr(L"aeio", l[0]) != nullptr;
}

// Tidies an expanded line: spaces, commas left by empty parts, a / an, capitals.
std::wstring TidyLine(const std::wstring &in, bool cap)
{
    std::wstring t;
    for (wchar_t c : in) // single spaces
    {
        if (iswspace(c))
            c = L' ';
        if (c == L' ' && (t.empty() || t.back() == L' '))
            continue;
        t += c;
    }
    bool changed = true;
    while (changed)
    {
        changed = false;
        static const wchar_t *const fix[][2] = {{L" ,", L","}, {L" .", L"."}, {L" !", L"!"}, {L" ?", L"?"},
                                                 {L",,", L","}, {L",.", L"."}, {L",!", L"!"}, {L",?", L"?"},
                                                 {L"..", L"."}, {L".,", L"."}, {L"!.", L"!"}, {L"?.", L"?"},
                                                 {L"!,", L"!"}, {L"?,", L"?"}, {L" '", L"'"}};
        for (auto &f : fix)
        {
            const size_t at = t.find(f[0]);
            if (at != std::wstring::npos)
            {
                t.replace(at, wcslen(f[0]), f[1]);
                changed = true;
            }
        }
    }
    const size_t a = t.find_first_not_of(L" ,.;");
    t = a == std::wstring::npos ? L"" : t.substr(a);
    while (!t.empty() && (t.back() == L' ' || t.back() == L','))
        t.pop_back();
    // a / an
    for (size_t i = 0; i < t.size(); ++i)
    {
        if (i > 0 && iswalpha(t[i - 1]))
            continue;
        const bool isA = (t[i] == L'a' || t[i] == L'A') && i + 1 < t.size() && t[i + 1] == L' ';
        const bool isAn = (t[i] == L'a' || t[i] == L'A') && i + 2 < t.size() && t[i + 1] == L'n' && t[i + 2] == L' ';
        if (!isA && !isAn)
            continue;
        const size_t ws = i + (isA ? 2 : 3);
        size_t we = ws;
        while (we < t.size() && (iswalnum(t[we]) || t[we] == L'\''))
            ++we;
        const bool an = AnBefore(t.substr(ws, we - ws));
        if (an && isA)
            t.insert(i + 1, L"n");
        else if (!an && isAn)
            t.erase(i + 1, 1);
    }
    // capitals: the start, and after a full stop / ! / ?
    for (size_t i = 0; i < t.size(); ++i)
    {
        const bool start = i == 0 ? cap : (i >= 2 && t[i - 1] == L' ' && (t[i - 2] == L'.' || t[i - 2] == L'!' || t[i - 2] == L'?'));
        if (start && iswlower(t[i]))
            t[i] = (wchar_t)towupper(t[i]);
    }
    return t;
}

void GtNoteLine(const wchar_t *group, int slot); // IN-GAME TEST, below
// A / AN (2026-10-15, user): "a {class}" / "a {enemy}" lines said "a assault", "a Atlas". After the
// placeholders are in, a standalone "a" before a vowel sound becomes "an" (a few exceptions both ways).
std::wstring FixArticles(const std::wstring &in)
{
    static const wchar_t *kConsonantSound[] = {L"one", L"once", L"uni", L"use", L"usu", L"uti", L"ura", L"eu", L"ewe"};
    static const wchar_t *kVowelSound[] = {L"hour", L"honest", L"honor", L"honour", L"heir"};
    std::wstring t = in;
    for (size_t i = 0; i + 2 < t.size(); ++i)
    {
        if ((t[i] != L'a' && t[i] != L'A') || t[i + 1] != L' ' || (i > 0 && iswalpha(t[i - 1])) || (i > 0 && t[i - 1] == L'\''))
            continue;
        size_t e = i + 2;
        while (e < t.size() && (iswalnum(t[e]) || t[e] == L'-'))
            ++e;
        std::wstring w = t.substr(i + 2, e - i - 2);
        if (w.empty())
            continue;
        for (auto &c : w)
            c = (wchar_t)towlower(c);
        bool an = wcschr(L"aeiou", w[0]) != nullptr;
        for (auto p : kConsonantSound)
            if (w.compare(0, wcslen(p), p) == 0)
                an = false;
        for (auto p : kVowelSound)
            if (w.compare(0, wcslen(p), p) == 0)
                an = true;
        if (an)
            t.insert(i + 1, L"n");
    }
    return t;
}

std::wstring Line(const wchar_t *group, int slot, std::initializer_list<std::pair<const wchar_t *, std::wstring>> vars = {}, bool cap = true)
{
    GtNoteLine(group, slot);
    static bool seeded = false;
    if (!seeded)
    {
        srand((unsigned)GetTickCount64());
        seeded = true;
    }
    std::wstring t;
    {
        const std::vector<std::wstring> ls = GroupLines(group, slot);
        if (!ls.empty())
            t = ExpandText(PickFresh(ls), slot, 0);
    }
    // Placeholders.
    bool hasCallsign = false;
    for (auto &v : vars)
        hasCallsign |= wcscmp(v.first, L"callsign") == 0;
    auto put = [&](const std::wstring &name, const std::wstring &val) {
        const std::wstring k = L"{" + name + L"}";
        for (size_t at = t.find(k); at != std::wstring::npos; at = t.find(k, at + val.size()))
            t.replace(at, k.size(), val);
    };
    for (auto &v : vars)
        put(v.first, v.second);
    if (!hasCallsign)
        put(L"callsign", slot >= 1 && slot <= 8 ? Callsign(slot) : L"");
    t = FixArticles(t); // A / AN (2026-10-15)
    // No callsign to put in ("{callsign}, ..." for the lance voice): no stray comma in front.
    return TidyLine(t, cap);
}

// Pieces of a status report, joined with single spaces (empty pieces skipped).
void AddPiece(std::wstring &s, const std::wstring &piece)
{
    if (piece.empty())
        return;
    s += (s.empty() ? L"" : L" ") + piece;
}

// ---- the grammar -----------------------------------------------------------------------------
enum CmdType
{
    C_Follow, C_Attack, C_Hold, C_Indirect, C_Fallback, C_Move, C_Disc, C_Heat, C_CatOnly, C_CatHold, C_AllWeapons, C_Reset, C_FallMove, C_Status,
    C_AttackNearest, // ORDER PARSER: the nearest enemy LidarExport knows of
    C_FireSet, // ORDER PARSER: several fire settings at once (A disc / B heat, -1 = keep; C, D weapons)
    C_AttackNamed, // "attack that Bane": the last reported one of that name, else the nearest (build 2026-10-05m)
    C_Shadow,      // "shadow it" / "get eyes on the Bane": go to where it was last seen; "found it" when seen (2026-10-05n)
    C_Nav,         // NAV POINTS (2026-10-07k): A = the letter (VhudNavName), C = 0 move, 1 hold, 2 fall back, 3 advance, 4 indirect
    C_Vision,      // VHUD v22: Enhanced Imaging vision mode, A = 1 on / 0 off - the player's own, not an order
    C_CalledShot,  // 2026-10-10 (lancecontrol-32): A = the part (0 cancel, 1..11 - TCO_CalledShot); on your lock, or
                   // on the unit named (QueryName); an attack on it too
    C_Support,     // SUPPORT (2026-10-13, user): "Two, go give Four some support" - A = the lancemate to support: attack
                   // its target, or (none) whoever last hit it
    C_Hill,        // HIGH GROUND (2026-10-14, user): "get up on that hill" - 3-4 hilltops marked as nav points, "which hill?"
    C_AttackClass, // TARGET CLASSES (2026-10-14, user): "take out the turrets" - A = 1 turrets, 2 tanks / vehicles, 3 VTOLs;
                   // the nearest of that kind, then the next, until none are left on the scan
    C_Flank,       // FLANK (2026-10-20, user; lancecontrol-75): A = the side (TCO_Flank Pad[0]), B = stand-off m (0 auto);
                   // the unit named (QueryName), else your lock
    C_Pincer       // PINCER (2026-10-20): A = 1 its own flanks (else your left / right / rear), B = stand-off m; C / D = this
                   // lancemate's place / how many were named (PincerSlot) - the whole lance counts itself in Execute
};
struct Cmd
{
    int   Who;  // slot, 0 = all
    int   Type;
    int   A = 0;   // discipline / heat / category mask / distance (m)
    int   B = 0;   // move direction
    int   C = 0;   // C_FireSet: weapons op (0 keep, 1 only, 2 hold, 3 all)
    int   D = 0;   // C_FireSet: weapon type mask
};
std::map<std::wstring, Cmd> g_phrases;

void AddOrder(const std::wstring &words, int type, int a = 0, int b = 0, int cc = 0)
{
    static const struct
    {
        const wchar_t *W;
        int            Slot;
    } kWho[] = {{L"all", 0},   {L"lance", 0},   {L"everyone", 0}, {L"one", 1}, {L"two", 2},  {L"three", 3},
                {L"four", 4},  {L"five", 5},    {L"six", 6},      {L"seven", 7}, {L"eight", 8}};
    for (auto &w : kWho)
    {
        Cmd c;
        c.Who = w.Slot;
        c.Type = type;
        c.A = a;
        c.B = b;
        c.C = cc;
        g_phrases[std::wstring(w.W) + L" " + words] = c;
    }
}

void BuildPhrases()
{
    AddOrder(L"form on me", C_Follow);
    AddOrder(L"follow me", C_Follow);
    AddOrder(L"make a run for it", C_Move, 200, 5, 4); // RUN FOR IT (2026-10-13): beeline 200 m behind you
    AddOrder(L"get up on that hill", C_Hill);          // HIGH GROUND (2026-10-14): "which hill, Commander?"
    AddOrder(L"take the high ground", C_Hill);
    AddOrder(L"take out the turrets", C_AttackClass, 1); // TARGET CLASSES (2026-10-14)
    AddOrder(L"take out the tanks", C_AttackClass, 2);
    AddOrder(L"take out the vehicles", C_AttackClass, 2);
    AddOrder(L"take out the vtols", C_AttackClass, 3);
    AddOrder(L"take out the elementals", C_AttackClass, 8); // BATTLE ARMOUR (2026-10-17, user)
    AddOrder(L"take out the battle armor", C_AttackClass, 8);
    for (int k = 1; k <= 4; ++k) // SUPPORT (2026-10-13): "two, support four"
        AddOrder(std::wstring(L"support ") + kNum[k], C_Support, k);
    AddOrder(L"get out of there", C_Move, 200, 5, 4);
    AddOrder(L"attack my target", C_Attack);
    AddOrder(L"engage my target", C_Attack);
    AddOrder(L"flank my target", C_Flank); // FLANK / PINCER (2026-10-20): on your lock (Whisper takes names, sides, stand-off)
    AddOrder(L"flank left", C_Flank, 1);
    AddOrder(L"flank right", C_Flank, 2);
    AddOrder(L"pincer my target", C_Pincer);
    AddOrder(L"hold position", C_Hold);
    AddOrder(L"hold here", C_Hold);
    AddOrder(L"indirect fire only", C_Indirect);
    AddOrder(L"hold for indirect fire", C_Indirect);
    AddOrder(L"fall back", C_Fallback);
    AddOrder(L"fall back to me", C_Fallback);
    static const struct
    {
        const wchar_t *W;
        int            M;
    } kDist[] = {{L"fifty", 50}, {L"one hundred", 100}, {L"a hundred", 100}, {L"two hundred", 200},
                 {L"three hundred", 300}, {L"four hundred", 400}, {L"five hundred", 500}};
    static const wchar_t *kDir[8] = {L"north", L"south", L"east", L"west", L"forward", L"back", L"left", L"right"};
    for (auto &d : kDist)
        for (int k = 0; k < 8; ++k)
        {
            AddOrder(std::wstring(L"move ") + d.W + L" " + kDir[k], C_Move, d.M, k);
            AddOrder(std::wstring(L"move ") + d.W + L" metres " + kDir[k], C_Move, d.M, k);
            // Fall back to a point said the same way: the fall-back order (faces the enemy on the way).
            AddOrder(std::wstring(L"fall back ") + d.W + L" " + kDir[k], C_FallMove, d.M, k);
            AddOrder(std::wstring(L"fall back ") + d.W + L" metres " + kDir[k], C_FallMove, d.M, k);
            // the other word order: "move forward five hundred (metres)"
            AddOrder(std::wstring(L"move ") + kDir[k] + L" " + d.W, C_Move, d.M, k);
            AddOrder(std::wstring(L"move ") + kDir[k] + L" " + d.W + L" metres", C_Move, d.M, k);
            AddOrder(std::wstring(L"fall back ") + kDir[k] + L" " + d.W, C_FallMove, d.M, k);
            AddOrder(std::wstring(L"fall back ") + kDir[k] + L" " + d.W + L" metres", C_FallMove, d.M, k);
        }
    AddOrder(L"weapons free", C_Disc, 0);
    AddOrder(L"return fire", C_Disc, 1);
    AddOrder(L"hold fire", C_Disc, 2);
    AddOrder(L"normal heat", C_Heat, 0);
    AddOrder(L"run cool", C_Heat, 1);
    AddOrder(L"run hot", C_Heat, 2);
    AddOrder(L"alpha strike", C_Heat, 3);
    static const struct
    {
        const wchar_t *W;
        int            Mask;
    } kCat[] = {{L"lasers", kFcLaser},
                {L"particle cannons", kFcPPC},
                {L"P P C", kFcPPC},
                {L"P P Cs", kFcPPC},
                {L"ballistics", kFcBallistic},
                {L"missiles", kFcLockMissile | kFcDumbMissile},
                {L"lock on missiles", kFcLockMissile},
                {L"long range missiles", kFcLockMissile},
                {L"dumbfire missiles", kFcDumbMissile},
                {L"short range missiles", kFcDumbMissile},
                {L"machine guns", kFcMGFlamer},
                {L"flamers", kFcMGFlamer},
                {L"tag", kFcTAG},
                {L"narc", kFcNARC}};
    for (auto &c : kCat)
    {
        AddOrder(std::wstring(c.W) + L" only", C_CatOnly, c.Mask);
        AddOrder(std::wstring(L"hold ") + c.W, C_CatHold, c.Mask);
    }
    AddOrder(L"all weapons", C_AllWeapons);
    AddOrder(L"reset fire orders", C_Reset);
    // Status report: damage, ammo, heat, lost weapons.
    AddOrder(L"status", C_Status);
    AddOrder(L"status report", C_Status);
    AddOrder(L"report status", C_Status);
    AddOrder(L"report", C_Status);
    AddOrder(L"report in", C_Status);
    AddOrder(L"sitrep", C_Status);
    AddOrder(L"what's your status", C_Status);
    AddOrder(L"how are you holding up", C_Status);
    // ENHANCED IMAGING VISION MODE (VHUD v22): said by the player to the cockpit, no lancemate addressed.
    {
        Cmd on, off;
        on.Who = off.Who = 0;
        on.Type = off.Type = C_Vision;
        on.A = 1;
        off.A = 0;
        g_phrases[L"activate enhanced imaging"] = on;
        g_phrases[L"initiate enhanced imaging"] = on; // (2026-10-12)
        g_phrases[L"deactivate enhanced imaging"] = off;
    }
}

// ---- carrying out a command ------------------------------------------------------------------
enum StatusWhat { ST_Damage = 1, ST_Ammo = 2, ST_Heat = 4, ST_Weapons = 8, ST_All = 15, ST_HeatAlways = 16 };
std::wstring StatusLine(int slot, int what = ST_All); // STATUS REPORT, below
// "attack that Bane": the name the parser found (Whisper's thread) and the one being carried out (main).
std::wstring g_parsedName, g_execName;
std::wstring g_attackUnheard; // ParseOrder: "attack the <words that are no unit we know>" - asked to say again (2026-10-10)
bool ShadowTarget(int slot, const std::wstring &name, uint32_t &lo, uint32_t &hi, std::wstring &said); // SHADOW, below
int  ShadowStart(int slot, uint32_t lo, uint32_t hi, const std::wstring &name, float p[3]);           // below
std::wstring WhereSeenLine(int by, uint32_t lo, uint32_t hi, const std::wstring &name);                // below
bool FindNamedUnit(const std::wstring &name, float pos[3], std::wstring &said, uint32_t *lo = nullptr, uint32_t *hi = nullptr); // QUESTIONS, below
int SupportTarget(int t, bool &attacker, float p[3], std::wstring &enemy); // SUPPORT, below (3: feed frozen, 2026-10-16)
// The enemy names the pilots know right now (on the scan, or reported): for the parser and Whisper.
std::mutex g_namesMx;
std::vector<std::wstring> g_knownNames; // as said ("Bane", "Man O' War")

// CORRECTIONS (2026-10-07a): each lancemate's fire settings from before the last batch of orders changed
// them (so "actually ..." can put them back first), whether that batch moved / held / attacked with it, and
// within one batch the settings just sent (the feed only catches up a moment later).
uint32_t g_orderBatch = 0; // bumped per batch of orders you give (RunSpokenActions)
struct FirePrev
{
    uint8_t  Disc = 0, Mask = 0, Heat = 0;
    uint32_t Batch = 0;
};
FirePrev g_firePrev[9];
bool     g_fireBaseOn[9] = {};
uint8_t  g_fireBase[9][3] = {};
uint32_t g_moveBatch[9] = {};
bool     g_execCorrection = false; // the reply is a correction's ("scratch that, ...")

// ENHANCED IMAGING VISION MODE (VHUD v22): HUDDisplay renders it, CameraCapture draws it; this only flips the switch.
void SetVision(bool on)
{
    if (!g_blk || g_blk->Version != VHUD_VERSION)
    {
        Log(L"Enhanced Imaging: the VanillaHud block is not connected");
        return;
    }
    g_blk->EiVisionOn = on ? 1u : 0u;
    Log(on ? L"Enhanced Imaging vision mode ON" : L"Enhanced Imaging vision mode off");
}

// ---- TARGET CLASSES (2026-10-14, user): "take out the turrets / tanks / VTOLs" ---------------------------------
// Each lancemate told attacks the nearest hostile of that kind on LidarExport's scan (TCO_Attack at it); SweepTick
// sends it to the next nearest when that one is gone (dead units leave the scan), and when none are left it says
// so and the sweep ends (the plugin's own targeting takes over). Any other order for that lancemate ends it.
struct ClassSweep
{
    bool      On = false;
    bool      Paused = false; // 2026-10-14 (user): held while this lancemate is on hold fire, until weapons free
    int       Kind = 0; // 1 turrets, 2 tanks / vehicles, 3 VTOLs; 4..7 light / medium / heavy / assault mechs; 8 battle armour
    uint16_t  Id = 0;   // the contact being attacked
    float     X = 0, Y = 0;
};
ClassSweep g_sweep[9];
ULONGLONG  g_classDoneMs[9] = {};
const wchar_t *ClassWord(int k, bool single = false)
{
    // 8 (2026-10-17, user): battle armour - Clan Elementals / IS battle armour suits (LidarExport's LK_BattleArmor)
    static const wchar_t *kMany[] = {L"targets", L"turrets", L"tanks", L"VTOLs", L"lights", L"mediums", L"heavies", L"assaults",
                                     L"battle armour"};
    static const wchar_t *kOne[] = {L"target", L"turret", L"tank", L"VTOL", L"light", L"medium", L"heavy", L"assault",
                                    L"battle armour suit"};
    return (k >= 0 && k < 9) ? (single ? kOne[k] : kMany[k]) : L"targets";
}
bool ClassMatch(const LidarContact &c, int k)
{
    if (k == 8)
        return c.Kind == LK_BattleArmor;
    if (k >= 4)
        return c.Kind == LK_Mech && c.Weight == (uint8_t)(k - 4);
    return c.Kind == (k == 1 ? (uint8_t)LK_Turret : (k == 2 ? (uint8_t)LK_Tank : (uint8_t)LK_Vtol));
}

// The hostiles of that kind on the scan now (seqlock copy), or false when the scan could not be read.
bool ScanKind(int kind, std::vector<LidarContact> &out)
{
    out.clear();
    LockedCount();
    if (!g_lid || g_lid->Magic != LIDAR_MAGIC)
        return false;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_lid->FrameCounterEnd; // bug sweep 2026-10-17: END first (the writer bumps FrameCounter first)
        MemoryBarrier();
        out.clear();
        const uint32_t n = std::min<uint32_t>(g_lid->ContactCount, (uint32_t)LIDAR_MAX_CONTACTS);
        for (uint32_t i = 0; i < n; ++i)
        {
            const LidarContact &c = g_lid->Contacts[i];
            if (c.Type == LCT_HostileMech && ClassMatch(c, kind))
                out.push_back(c);
        }
        MemoryBarrier();
        if (g_lid->FrameCounterEnd == a && g_lid->FrameCounter == a)
            return true;
        Sleep(1);
    }
    return false;
}

// Lancemate s on the nearest of that kind to `from`; false = none on the scan. single: that one attack, no sweep.
// paused: the sweep is set up but nothing sent (hold fire) - SweepTick sends it at weapons free.
bool ClassStart(int s, int kind, const float from[3], bool single = false, bool paused = false)
{
    std::vector<LidarContact> cs;
    if (!ScanKind(kind, cs) || cs.empty())
        return false;
    const LidarContact *best = nullptr;
    float bd = 1e30f;
    for (auto &c : cs)
    {
        const float d = (c.X - from[0]) * (c.X - from[0]) + (c.Y - from[1]) * (c.Y - from[1]);
        if (d < bd)
        {
            bd = d;
            best = &c;
        }
    }
    const float p[3] = {best->X, best->Y, best->Z};
    if (!paused)
        Send(VanillaExportBlock::TCO_Attack, s, p); // the plugin attacks the unit nearest the point
    ClassSweep &w = g_sweep[s];
    w.On = !single;
    w.Paused = paused;
    w.Kind = kind;
    w.Id = best->Id;
    w.X = best->X;
    w.Y = best->Y;
    Log(L"class %ls: %ls on the %ls - %zu on the scan, the nearest %.0f m away%ls", single ? L"attack" : L"sweep", Callsign(s).c_str(),
        ClassWord(kind, single), cs.size(), std::sqrt(bd) / 100.0f, paused ? L" (paused: hold fire)" : L"");
    return true;
}

void SweepTick()
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 1000)
        return;
    last = now;
    bool any = false;
    for (int s = 1; s <= 8; ++s)
        any |= g_sweep[s].On;
    if (!any || !g_blk)
        return;
    VanillaExportBlock::TacMember m[VanillaExportBlock::kTacMaxMembers] = {};
    const int n = ReadLance(m);
    for (int s = 1; s <= 8; ++s)
    {
        ClassSweep &w = g_sweep[s];
        if (!w.On)
            continue;
        const VanillaExportBlock::TacMember *me = nullptr;
        for (int i = 0; i < n; ++i)
            if (m[i].Valid && !m[i].IsPlayer && !m[i].Dead && m[i].Slot == s)
                me = &m[i];
        if (!me)
        {
            w.On = false; // gone (or out of the lance)
            continue;
        }
        // 2026-10-14 (user): hold fire pauses the sweep (no new targets); only weapons free resumes it, on the nearest
        if (me->FireDisc == 2 && !w.Paused)
        {
            w.Paused = true;
            Log(L"class sweep: %ls on hold fire - sweep paused until weapons free", Callsign(s).c_str());
            continue;
        }
        if (w.Paused)
        {
            if (me->FireDisc != 0)
                continue;
            Log(L"class sweep: %ls weapons free - sweep resumed", Callsign(s).c_str());
            if (!ClassStart(s, w.Kind, me->Loc))
            {
                w.On = false;
                w.Paused = false;
            }
            continue;
        }
        std::vector<LidarContact> cs;
        if (!ScanKind(w.Kind, cs))
            continue; // no scan this second: try again
        bool still = false;
        for (auto &c : cs) // the same one: its id, or one of that kind within 25 m of where it was (the id is only "stable-ish")
            if (c.Id == w.Id || (c.X - w.X) * (c.X - w.X) + (c.Y - w.Y) * (c.Y - w.Y) < 2500.0f * 2500.0f)
            {
                still = true;
                w.Id = c.Id;
                w.X = c.X;
                w.Y = c.Y;
                break;
            }
        if (still)
            continue;
        if (cs.empty())
        {
            w.On = false;
            Log(L"class sweep: %ls - no %ls left on the scan, sweep over", Callsign(s).c_str(), ClassWord(w.Kind));
            if (now - g_classDoneMs[w.Kind] > 10000) // one "turrets are down" for the lance, not one each
            {
                g_classDoneMs[w.Kind] = now;
                Say(s, Line(L"class_done", s, {{L"enemy", ClassWord(w.Kind)}}));
            }
            continue;
        }
        ClassStart(s, w.Kind, me->Loc); // that one is down: the next nearest
    }
}

// HIGH GROUND (2026-10-14), below with the terrain readers
void HillStart(int who, int speaker, int nearNav = -1);
void HillPickLetter(int letter);

// =====================================================================================
// VOICE INTENT (2026-10-19, user): MW5_VoiceIntent for HUDDisplay's V-LNC page - what each lancemate was
// told in words, the question it waits on you to answer, and who is on the radio (a loudness trace).
// =====================================================================================
HANDLE            g_viMap = nullptr;
VoiceIntentBlock *g_vi = nullptr;
void ViOpen()
{
    if (g_vi)
        return;
    g_viMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(VoiceIntentBlock), VOICE_INTENT_MMF_NAME);
    if (!g_viMap)
        return;
    g_vi = static_cast<VoiceIntentBlock *>(MapViewOfFile(g_viMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VoiceIntentBlock)));
    if (!g_vi)
        return;
    std::memset(g_vi, 0, sizeof(VoiceIntentBlock));
    g_vi->Version = VOICE_INTENT_VERSION;
    Log(L"voice intent: MW5_VoiceIntent up (v%u)", VOICE_INTENT_VERSION);
}
void ViCopy(char *dst, size_t n, const std::wstring &w)
{
    const std::string u = Utf8(w);
    const size_t k = std::min(n - 1, u.size());
    std::memcpy(dst, u.data(), k);
    dst[k] = 0;
}
// Main thread only.
void ViSetIntent(int slot, const std::wstring &txt)
{
    if (!g_vi || slot < 1 || slot >= kViSlots)
        return;
    volatile uint32_t &seq = g_vi->Seq;
    seq = seq + 1; // odd: writing
    MemoryBarrier();
    ViCopy(g_vi->Slots[slot].Intent, sizeof(g_vi->Slots[slot].Intent), txt);
    g_vi->Slots[slot].IntentMs = (uint32_t)GetTickCount64();
    MemoryBarrier();
    seq = seq + 1;
}
// Playback thread: nobody on the air.
void ViSpeakingOff()
{
    if (!g_vi)
        return;
    for (int s = 0; s < kViSlots; ++s)
        g_vi->Slots[s].Speaking = 0;
}
std::wstring ViClock(float deg)
{
    int h = (int)std::lround(std::fmod(std::fmod(deg, 360.0f) + 360.0f, 360.0f) / 30.0f) % 12;
    if (h == 0)
        h = 12;
    return std::to_wstring(h) + L" o'clock";
}
// "200 m north", "1500 m your 2 o'clock" - a voice move's distance and direction (as Execute aims it).
std::wstring ViMoveText(const Cmd &c)
{
    std::wstring d;
    switch (c.B)
    {
    case 0: d = L"north"; break;
    case 1: d = L"south"; break;
    case 2: d = L"east"; break;
    case 3: d = L"west"; break;
    case 4: d = L"forward"; break;
    case 5: d = L"back"; break;
    case 6: d = L"left"; break;
    case 7: d = L"right"; break;
    case 8: d = L"north-east"; break;
    case 9: d = L"north-west"; break;
    case 10: d = L"south-east"; break;
    case 11: d = L"south-west"; break;
    default:
        if (c.B >= 2000)
            d = L"its " + ViClock((float)(c.B - 2000));
        else if (c.B >= 1000)
            d = L"your " + ViClock((float)(c.B - 1000));
        else if (c.B >= 100)
        {
            wchar_t b[24];
            swprintf_s(b, L"bearing %03d", (c.B - 100 + 360) % 360);
            d = b;
        }
        else
            d = L"forward";
        break;
    }
    return std::to_wstring(c.A) + L" m " + d;
}
// What a lancemate was told, in words (from the reply it gives). Fire settings change nothing here.
void ViOrderIntent(int s, const Cmd &c, const wchar_t *reply, const std::wstring &enemy, const std::wstring &part,
                   const std::wstring &who)
{
    const std::wstring r = reply ? reply : L"";
    std::wstring t;
    if (r == L"order_follow") t = L"form on you";
    else if (r == L"order_support" || r == L"order_support_attacker") t = L"support " + who + (enemy.empty() ? L"" : L": attack the " + enemy);
    else if (r == L"order_attack_nearest") t = L"attack the nearest enemy";
    else if (r == L"order_shadow") t = L"shadow the " + enemy + L" (fire on the move)";
    else if (r == L"order_nav_move") t = L"to " + enemy + L" (fire on the move)";
    else if (r == L"order_nav_hold") t = L"to " + enemy + L", then hold";
    else if (r == L"order_nav_fall_back") t = L"fall back to " + enemy;
    else if (r == L"order_nav_advance") t = L"advance to " + enemy + L" (fire on the move)";
    else if (r == L"order_nav_indirect") t = L"to " + enemy + L", then hold - indirect fire";
    else if (r == L"order_attack_class") t = L"attack " + enemy + L", one after another";
    else if (r == L"order_called_shot" || r == L"order_called_shot_named")
        t = L"called shot: " + part + (enemy.empty() ? L" on your target" : L" on the " + enemy);
    else if (r == L"order_called_cancel") t = L"called shot cancelled";
    else if (r == L"order_flank") t = L"flank the " + enemy + L" - " + part + L" (fire on the move)";
    else if (r == L"order_pincer") t = L"pincer the " + enemy + L" - " + part + L" (fire on the move)";
    else if (r == L"order_pincer_need_two") t = L"a pincer needs two lancemates";
    else if (r == L"order_attack_named") t = (c.Type == C_Attack ? L"attack your target - the " : L"attack the ") + enemy;
    else if (r == L"order_attack") t = L"attack your target";
    else if (r == L"order_hold") t = c.Type == C_Move ? L"move " + ViMoveText(c) + L", then hold" : L"hold here";
    else if (r == L"order_indirect") t = c.Type == C_Move ? L"move " + ViMoveText(c) + L", then hold - indirect fire" : L"hold - indirect fire only";
    else if (r == L"order_fall_back_to_me") t = L"fall back to you";
    else if (r == L"order_fall_back") t = L"fall back " + ViMoveText(c);
    else if (r == L"order_run") t = L"make a run for it - " + ViMoveText(c);
    else if (r == L"order_move" || r == L"order_move_capped") t = L"move " + ViMoveText(c) + L" (fire on the move)";
    if (!t.empty())
        ViSetIntent(s, t);
}

void Execute(const Cmd &c)
{
    if (c.Type == C_Vision)
    {
        SetVision(c.A != 0);
        return;
    }
    VanillaExportBlock::TacMember m[VanillaExportBlock::kTacMaxMembers] = {};
    const int n = ReadLance(m);
    const VanillaExportBlock::TacMember *me = nullptr;
    for (int i = 0; i < n; ++i)
        if (m[i].Valid && m[i].IsPlayer)
            me = &m[i];
    std::vector<const VanillaExportBlock::TacMember *> to;
    for (int i = 0; i < n; ++i)
    {
        if (!m[i].Valid || m[i].IsPlayer || m[i].Dead)
            continue;
        if (c.Who == 0 || m[i].Slot == c.Who)
            to.push_back(&m[i]);
    }
    if (to.empty())
    {
        if (c.Who && me && me->Slot == c.Who)
            Say(0, Line(L"problem_is_you", 0));
        else if (!n)
            Say(0, Line(L"problem_no_lance_data", 0));
        else
            Say(0, c.Who ? Line(L"problem_no_answer", 0, {{L"callsign", NumberName(c.Who)}}) : Line(L"problem_no_lancemates", 0));
        return;
    }
    if ((c.Type == C_Attack || ((c.Type == C_Flank || c.Type == C_Pincer) && g_execName.empty())) && LockedCount() == 0)
    {
        Say(to[0]->Slot, Line(L"problem_no_lock", to[0]->Slot));
        return;
    }
    if ((c.Type == C_Fallback || c.Type == C_Move || c.Type == C_FallMove) && !me)
    {
        Say(0, Line(L"problem_no_position", 0));
        return;
    }
    if (c.Type == C_Hill) // HIGH GROUND (2026-10-14): one offer for whoever was told; its answer sends them
    {
        HillStart(c.Who, to.front()->Slot, c.A - 1); // A: 0 around you, else the nav letter + 1
        return;
    }
    if (c.Type == C_Nav)
        HillPickLetter(c.A); // "move to Nav Foxtrot" while "which hill?" is open picks that hill (the others go)
    for (const auto *u : to)
    {
        const int s = u->Slot;
        if (s >= 1 && s <= 8 && c.Type != C_Status && c.Type != C_Disc && c.Type != C_Heat && c.Type != C_CatOnly &&
            c.Type != C_CatHold && c.Type != C_AllWeapons && c.Type != C_Reset && c.Type != C_FireSet)
        {
            g_moveBatch[s] = g_orderBatch; // CORRECTIONS: this batch gave it a move / hold / attack / follow ...
            if (c.Type != C_AttackClass)
                g_sweep[s].On = false; // TARGET CLASSES: a new order ends the sweep
        }
        const wchar_t *reply = L"fire_reset"; // the voice-line group of its reply
        std::wstring replyEnemy;              // {enemy} in it, when known
        std::wstring replyPart;               // {part}: a called shot's part
        std::wstring replyWho;                // {who}: the lancemate supported (C_Support)
        switch (c.Type)
        {
        case C_Status:
            Say(s, StatusLine(s)); // each lancemate answers for itself, in its own voice
            continue;
        case C_Follow:
            Send(VanillaExportBlock::TCO_Follow, s, nullptr);
            reply = L"order_follow";
            break;
        case C_Support: // SUPPORT (2026-10-13): attack the target of lancemate A - or, if it has none, whoever last hit it
        {
            const int t = c.A;
            if (t == s)
            {
                if (to.size() == 1)
                    Say(s, Line(L"problem_support_self", s));
                continue;
            }
            const std::wstring mateName = Callsign(t);
            bool attacker = false;
            float p[3];
            std::wstring enemy;
            const int st = SupportTarget(t, attacker, p, enemy); // 0 ok, 1 the lancemate is gone, 2 nothing to shoot, 3 no live data
            if (st == 3) // 2026-10-16: the lance status feed is frozen (out of the mech / paused) - no attack posted
            {
                Say(s, Line(L"problem_no_lance_data", s));
                continue;
            }
            if (st == 1)
            {
                Say(s, Line(L"problem_support_gone", s, {{L"who", mateName}}));
                continue;
            }
            if (st == 2)
            {
                Say(s, Line(L"problem_support_nothing", s, {{L"who", mateName}}));
                continue;
            }
            Send(VanillaExportBlock::TCO_Attack, s, p); // the plugin attacks the unit nearest the point
            replyEnemy = enemy;
            replyWho = mateName;
            reply = attacker ? L"order_support_attacker" : L"order_support";
            break;
        }
        case C_AttackNearest:
        {
            float e[3];
            if (!NearestEnemy(e, c.A - 1)) // A: 0 any, 1..4 light / medium / heavy / assault
            {
                Say(s, Line(L"problem_no_contacts", s));
                continue;
            }
            Send(VanillaExportBlock::TCO_Attack, s, e); // the plugin attacks the unit nearest the point
            reply = L"order_attack_nearest";
            break;
        }
        case C_Shadow:
        {
            // the unit: named, else what this pilot (or anyone) last reported
            std::wstring said;
            uint32_t lo = 0, hi = 0;
            if (!ShadowTarget(s, g_execName, lo, hi, said))
            {
                Say(s, Line(L"answer_where_none", s));
                continue;
            }
            float p[3];
            const int how = ShadowStart(s, lo, hi, said, p); // 0 no position, 1 out of sight (go), 2 in sight now, -1 dead
            if (how == -1)
            {
                Say(s, Line(L"answer_where_dead", s, {{L"enemy", said}}));
                continue;
            }
            if (how == 0)
            {
                Say(s, Line(L"answer_where_unknown", s, {{L"enemy", said}}));
                continue;
            }
            if (how == 2)
            {
                Say(s, WhereSeenLine(s, lo, hi, said), K_Position); // "it's right there, your 3 o'clock"
                continue;
            }
            Send(VanillaExportBlock::TCO_MoveFire, s, p); // fire on the move (2026-10-13, user)
            replyEnemy = said;
            reply = L"order_shadow";
            break;
        }
        case C_Nav: // NAV POINTS (2026-10-07k)
        {
            float p[3];
            const std::wstring nn = NavSaid(c.A);
            if (!NavLoc(c.A, p))
            {
                Say(s, Line(L"problem_no_nav", s, {{L"nav", nn}}));
                continue;
            }
            static const uint8_t kO[] = {VanillaExportBlock::TCO_MoveFire, VanillaExportBlock::TCO_HoldHere, VanillaExportBlock::TCO_Fallback, // 0: fire on the move (2026-10-13)
                                         VanillaExportBlock::TCO_MoveFire, VanillaExportBlock::TCO_IndirectCover}; // 3: "advance to nav" = fire on the move (2026-10-13, was advance / fall back to me)
            static const wchar_t *kR[] = {L"order_nav_move", L"order_nav_hold", L"order_nav_fall_back", L"order_nav_advance",
                                          L"order_nav_indirect"};
            const int m = std::max(0, std::min(4, c.C));
            Send(kO[m], s, p);
            replyEnemy = nn;
            reply = kR[m];
            break;
        }
        case C_AttackClass: // TARGET CLASSES (2026-10-14): the nearest of that kind, then the next (SweepTick)
        {
            const bool single = c.B == 1;
            const wchar_t *word = ClassWord(c.A, single);
            if (!ClassStart(s, c.A, u->Loc, single, !single && u->FireDisc == 2)) // on hold fire: set up, paused
            {
                if (u == to.front()) // the scan is the lance's: none for one is none for all
                    Say(s, Line(L"problem_class_none", s, {{L"enemy", word}}));
                continue;
            }
            replyEnemy = word;
            reply = L"order_attack_class";
            break;
        }
        case C_AttackNamed:
        {
            float p[3];
            std::wstring said;
            uint32_t nlo = 0, nhi = 0;
            if (!FindNamedUnit(g_execName, p, said, &nlo, &nhi))
            {
                std::wstring nm = g_execName;
                if (!nm.empty())
                    nm[0] = (wchar_t)towupper(nm[0]);
                Say(s, Line(L"answer_where_named_none", s, {{L"enemy", nm}}));
                continue;
            }
            SendUnit(VanillaExportBlock::TCO_Attack, s, p, nlo, nhi); // that exact unit (lancecontrol-61; the point if unknown)
            replyEnemy = said;
            reply = L"order_attack_named";
            break;
        }
        case C_CalledShot: // 2026-10-10 (lancecontrol-32): the part on your lock / the named unit, and an attack on it
        {
            static const wchar_t *kPart[] = {L"", L"legs", L"left leg", L"right leg", L"arms", L"left arm", L"right arm",
                                             L"side torsos", L"left torso", L"right torso", L"centre torso", L"cockpit"};
            replyPart = c.A >= 0 && c.A <= 11 ? kPart[c.A] : L"";
            if (c.A <= 0 || c.A > 11)
            {
                Send(VanillaExportBlock::TCO_CalledShot, s, nullptr, 0, 0);
                reply = L"order_called_cancel";
                break;
            }
            if (!g_execName.empty())
            {
                float p[3];
                std::wstring said;
                uint32_t nlo = 0, nhi = 0;
                if (!FindNamedUnit(g_execName, p, said, &nlo, &nhi))
                {
                    std::wstring nm = g_execName;
                    if (!nm.empty())
                        nm[0] = (wchar_t)towupper(nm[0]);
                    Say(s, Line(L"answer_where_named_none", s, {{L"enemy", nm}}));
                    continue;
                }
                SendUnit(VanillaExportBlock::TCO_CalledShot, s, p, nlo, nhi, (uint8_t)c.A, 0); // the exact unit (lancecontrol-61)
                replyEnemy = said;
            }
            else
            {
                // 2026-10-12 (lancecontrol-42): a vehicle, turret or VTOL has no legs to call - "That target doesn't
                // have legs, Commander!" and no order (LanceControl refuses it too).
                const int kind = LockedKind();
                if (kind >= 0 && kind != LK_Mech)
                {
                    reply = L"order_called_not_mech";
                    break;
                }
                Send(VanillaExportBlock::TCO_CalledShot, s, nullptr, (uint8_t)c.A, 1); // your lock, resolved in the game
                replyEnemy = LockedEnemyName();
            }
            reply = replyEnemy.empty() ? L"order_called_shot" : L"order_called_shot_named";
            break;
        }
        case C_Flank: // FLANK / PINCER (2026-10-20, user; lancecontrol-75): round to one side of the unit, firing on the move
        case C_Pincer:
        {
            int side = c.A;
            const bool pin = c.Type == C_Pincer;
            if (pin)
            {
                int idx = c.C, cnt = c.D; // named lancemates: their place, how many (PincerSlot); the lance: its order here
                if (c.Who == 0)
                {
                    idx = (int)(std::find(to.begin(), to.end(), u) - to.begin());
                    cnt = (int)to.size();
                }
                if (cnt < 2)
                {
                    reply = L"order_pincer_need_two";
                    break;
                }
                static const int kOur[] = {1, 2, 5}, kIts[] = {3, 4, 5};
                side = idx < 3 ? (c.A ? kIts[idx] : kOur[idx]) : (c.A ? 3 : 1) + (idx % 2);
            }
            const uint8_t pad0 = (uint8_t)((side & 7) | (pin ? 8 : 0));
            const uint8_t pad1 = (uint8_t)((std::max(0, std::min(127, (c.B + 5) / 10))) << 1);
            if (!g_execName.empty())
            {
                float p[3];
                std::wstring said;
                uint32_t nlo = 0, nhi = 0;
                if (!FindNamedUnit(g_execName, p, said, &nlo, &nhi))
                {
                    std::wstring nm = g_execName;
                    if (!nm.empty())
                        nm[0] = (wchar_t)towupper(nm[0]);
                    Say(s, Line(L"answer_where_named_none", s, {{L"enemy", nm}}));
                    continue;
                }
                SendUnit(VanillaExportBlock::TCO_Flank, s, p, nlo, nhi, pad0, pad1); // the exact unit
                replyEnemy = said;
            }
            else
            {
                Send(VanillaExportBlock::TCO_Flank, s, nullptr, pad0, (uint8_t)(pad1 | 1)); // your lock, resolved in the game
                replyEnemy = LockedEnemyName();
            }
            if (replyEnemy.empty())
                replyEnemy = L"target";
            static const wchar_t *kSide[] = {L"flank", L"left flank", L"right flank", L"left flank", L"right flank", L"rear", L"flank", L"flank"};
            replyPart = kSide[side & 7]; // {part}: where it goes
            reply = pin ? L"order_pincer" : L"order_flank";
            break;
        }
        case C_Attack:
            Send(VanillaExportBlock::TCO_AttackMyTarget, s, nullptr);
            replyEnemy = LockedEnemyName(); // "engaging the Atlas"
            reply = replyEnemy.empty() ? L"order_attack" : L"order_attack_named";
            break;
        case C_Hold:
            Send(VanillaExportBlock::TCO_HoldHere, s, u->Loc);
            reply = L"order_hold";
            break;
        case C_Indirect:
            Send(VanillaExportBlock::TCO_IndirectCover, s, u->Loc);
            reply = L"order_indirect";
            break;
        case C_Fallback:
        {
            // 50 m from you, on its own side (as the advance / fall back order does).
            float dx = u->Loc[0] - me->Loc[0], dy = u->Loc[1] - me->Loc[1];
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f)
            {
                dx /= len;
                dy /= len;
            }
            else
            {
                dx = 1.0f;
                dy = 0.0f;
            }
            const float p[3] = {me->Loc[0] + dx * 5000.0f, me->Loc[1] + dy * 5000.0f, me->Loc[2]};
            Send(VanillaExportBlock::TCO_Fallback, s, p);
            reply = L"order_fall_back_to_me";
            break;
        }
        case C_Move:
        case C_FallMove:
        {
            // X north, Y east (the maps' convention); forward / back / left / right from your heading.
            float ang = 0.0f;
            switch (c.B)
            {
            case 0: ang = 0.0f; break;
            case 1: ang = 180.0f; break;
            case 2: ang = 90.0f; break;
            case 3: ang = -90.0f; break;
            case 4: ang = CockpitYaw(me->Yaw); break; // forward / back / left / right: where you LOOK
            case 5: ang = CockpitYaw(me->Yaw) + 180.0f; break;
            case 6: ang = CockpitYaw(me->Yaw) - 90.0f; break;
            case 7: ang = CockpitYaw(me->Yaw) + 90.0f; break;
            case 8: ang = 45.0f; break;   // north-east (ORDER PARSER)
            case 9: ang = -45.0f; break;  // north-west
            case 10: ang = 135.0f; break; // south-east
            case 11: ang = -135.0f; break; // south-west
            default:
                // BEARINGS (2026-10-10): 100 + compass, 1000 + from your heading (o'clock), 2000 + from its own facing
                if (c.B >= 2000)
                    ang = u->Yaw + (float)(c.B - 2000);
                else if (c.B >= 1000)
                    ang = CockpitYaw(me->Yaw) + (float)(c.B - 1000);
                else if (c.B >= 100)
                    ang = (float)(c.B - 100);
                else
                    ang = CockpitYaw(me->Yaw);
                break;
            }
            const float r = ang * 3.14159265f / 180.0f, d = (float)c.A * 100.0f;
            const float *from = c.B >= 2000 ? u->Loc : me->Loc; // "your 2 o'clock": from where the lancemate stands
            const float p[3] = {from[0] + std::cos(r) * d, from[1] + std::sin(r) * d, from[2]};
            if (c.Type == C_FallMove)
            {
                Send(VanillaExportBlock::TCO_Fallback, s, p);
                reply = L"order_fall_back";
            }
            else if (c.C == 1) // ORDER PARSER: "move ... and hold" - hold at that point
            {
                Send(VanillaExportBlock::TCO_HoldHere, s, p);
                reply = L"order_hold";
            }
            else if (c.C == 2) // "move ... and hold for indirect fire" - indirect hold at that point
            {
                Send(VanillaExportBlock::TCO_IndirectCover, s, p);
                reply = L"order_indirect";
            }
            else if (c.C == 4) // "make a run for it" / "get out of there" - the beeline (2026-10-13)
            {
                Send(VanillaExportBlock::TCO_Move, s, p);
                reply = L"order_run";
            }
            else // every other move order: fire on the move (2026-10-13, user)
            {
                Send(VanillaExportBlock::TCO_MoveFire, s, p);
                reply = L"order_move";
            }
            if (c.D == 1) // MOVE CAP (2026-10-15, user): asked for more than 2 km - say it was cut to 2 km
                reply = L"order_move_capped";
            break;
        }
        default:
        {
            // Fire settings: change the one thing said, keep the rest as the plugin reports it.
            uint8_t disc = u->FireDisc, mask = u->FireMask, heat = u->FireHeat;
            if (s >= 1 && s <= 8 && g_fireBaseOn[s]) // just sent / put back this batch: the feed has not caught up
            {
                disc = g_fireBase[s][0];
                mask = g_fireBase[s][1];
                heat = g_fireBase[s][2];
            }
            if (s >= 1 && s <= 8 && g_firePrev[s].Batch != g_orderBatch) // CORRECTIONS: as it was before this batch
            {
                g_firePrev[s].Disc = disc;
                g_firePrev[s].Mask = mask;
                g_firePrev[s].Heat = heat;
                g_firePrev[s].Batch = g_orderBatch;
            }
            switch (c.Type)
            {
            case C_Disc: disc = (uint8_t)c.A; reply = c.A == 0 ? L"fire_weapons_free" : (c.A == 1 ? L"fire_return_fire" : L"fire_hold"); break;
            case C_Heat:
                heat = (uint8_t)c.A;
                reply = c.A == 0 ? L"heat_normal" : (c.A == 1 ? L"heat_cool" : (c.A == 2 ? L"heat_hot" : L"heat_alpha"));
                break;
            case C_CatOnly: mask = (uint8_t)c.A; reply = L"weapons_only"; break;
            case C_CatHold: mask = (uint8_t)(mask & ~c.A); reply = L"weapons_hold"; break;
            case C_AllWeapons: mask = kFcAll; reply = L"weapons_all"; break;
            case C_FireSet: // the order parser: several settings in one, the rest kept
                if (c.A >= 0)
                    disc = (uint8_t)c.A;
                if (c.B >= 0)
                    heat = (uint8_t)c.B;
                if (c.C == 1)
                    mask = (uint8_t)c.D;
                else if (c.C == 2)
                    mask = (uint8_t)(mask & ~c.D);
                else if (c.C == 3)
                    mask = kFcAll;
                reply = c.A >= 0 ? (c.A == 0 ? L"fire_weapons_free" : (c.A == 1 ? L"fire_return_fire" : L"fire_hold"))
                      : c.B >= 0 ? (c.B == 0 ? L"heat_normal" : (c.B == 1 ? L"heat_cool" : (c.B == 2 ? L"heat_hot" : L"heat_alpha")))
                      : c.C == 1 ? L"weapons_only" : (c.C == 2 ? L"weapons_hold" : L"weapons_all");
                break;
            default: disc = 0; mask = kFcAll; heat = 0; reply = L"fire_reset"; break;
            }
            const float v[3] = {(float)disc, (float)mask, (float)heat};
            Send(VanillaExportBlock::TCO_FireSettings, s, v);
            if (s >= 1 && s <= 8)
            {
                g_fireBaseOn[s] = true;
                g_fireBase[s][0] = disc;
                g_fireBase[s][1] = mask;
                g_fireBase[s][2] = heat;
            }
            break;
        }
        }
        ViOrderIntent(s, c, reply, replyEnemy, replyPart, replyWho); // VOICE INTENT (2026-10-19)
        if (to.size() == 1)
            Say(s, Line(g_execCorrection ? L"reply_correction" : L"reply_single", s,
                        {{L"reply", Line(reply, s, {{L"enemy", replyEnemy}, {L"nav", replyEnemy}, {L"part", replyPart}, {L"who", replyWho}}, false)}}));
        else if (u == to.front())
            Say(s, Line(g_execCorrection ? L"reply_correction_lance" : L"reply_lance", s,
                        {{L"reply", Line(reply, s, {{L"enemy", replyEnemy}, {L"nav", replyEnemy}, {L"part", replyPart}, {L"who", replyWho}}, false)}}));
    }
}
// ---- WHISPER COMMANDS: what Whisper wrote down, turned into one of the orders above -----------
// One list of orders for both recognisers: every phrase above, without its callsign, is a
// candidate. The words heard are cleaned up (numbers, jargon, common synonyms), the leading
// callsign(s) taken off, and the rest scored against every order word by word (a word that is
// nearly right - "positions" for "position" - costs less than a wrong one). The best one is
// carried out only if it is clearly the best; a near miss gets "say again?", never a guess.
struct OrderWords
{
    std::vector<std::wstring> W;
    Cmd                       C;
};
std::vector<OrderWords> g_orders;
std::vector<std::wstring> NormalizeHeard(const std::wstring &text);

std::vector<std::wstring> SplitWords(const std::wstring &s)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t ch : s)
    {
        if (iswalnum(ch))
            cur += (wchar_t)towlower(ch);
        else if (ch == L'\'')
            continue; // "don't" -> "dont"
        else if (!cur.empty())
        {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::wstring JoinWords(const std::vector<std::wstring> &w, size_t from = 0)
{
    std::wstring s;
    for (size_t i = from; i < w.size(); ++i)
        s += (s.empty() ? L"" : L" ") + w[i];
    return s;
}

void BuildOrderList()
{
    g_orders.clear();
    for (auto &p : g_phrases)
        if (p.first.compare(0, 4, L"all ") == 0) // every order has an "all ..." phrase
        {
            OrderWords o;
            o.W = NormalizeHeard(p.first.substr(4)); // cleaned up the same way as what is heard
            o.C = p.second;
            g_orders.push_back(o);
        }
}

std::wstring NumberWords(int n)
{
    static const wchar_t *kOnes[] = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine",
                                     L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen",
                                     L"seventeen", L"eighteen", L"nineteen"};
    static const wchar_t *kTens[] = {L"", L"", L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety"};
    if (n < 0 || n > 9999)
        return std::to_wstring(n);
    std::wstring s;
    if (n >= 1000)
    {
        s = NumberWords(n / 1000) + L" thousand";
        n %= 1000;
        if (!n)
            return s;
        s += L" ";
    }
    if (n >= 100)
    {
        s += std::wstring(kOnes[n / 100]) + L" hundred";
        n %= 100;
        if (!n)
            return s;
        s += L" ";
    }
    if (n < 20)
        return s + kOnes[n];
    s += kTens[n / 10];
    if (n % 10)
        s += std::wstring(L" ") + kOnes[n % 10];
    return s;
}

// Numbers as words, units and jargon spelled the way the phrases have them, filler dropped.
// ---- HEARD AS (build 2026-10-06b) --------------------------------------------------------------
// HeardAs.txt beside the exe: what Whisper writes for a word it does not know, and the word you meant
// ("wall honk = warhawk", "badator = gladiator"). Applied to every utterance first, whole words, any
// case. No rules live in the code: a missing file is written with its instructions only (a copy of the
// user's rules is kept in src/voice_command/HeardAs.txt). Saved changes are used within a second.
struct HeardRule
{
    std::vector<std::wstring> From, To; // lower-case words
    // 2026-10-12 (user): also the possessive - "blackhursts" (Blackhurst's) with "blackhurst = blackheart" gives
    // "blackhearts", which the name matcher reads as Blackheart's. Only for a rule whose result is a name (no order
    // words in it - see HeardAsTick).
    bool Poss = false;
};
std::vector<HeardRule> g_heardAs;
std::mutex             g_heardMx; // Whisper's thread reads them
FILETIME               g_heardTime = {};
ULONGLONG              g_heardCheckMs = 0;

std::wstring HeardAsPath() { return ExeDir() + L"HeardAs.txt"; }

void HeardAsTick()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_heardCheckMs < 1000)
        return;
    g_heardCheckMs = now;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(HeardAsPath().c_str(), GetFileExInfoStandard, &fa))
    {
        WriteFileUtf8(HeardAsPath(),
                      L"# HeardAs.txt - what Whisper writes for a word it gets wrong, and the word you meant.\r\n"
                      L"# One per line:  heard = meant   (whole words, any case), e.g.  wall honk = warhawk\r\n"
                      L"# Check VoiceCommand_heard.log for what it wrote (the words in [brackets]). Used within a second of saving.\r\n"
                      L"\r\n");
        if (!GetFileAttributesExW(HeardAsPath().c_str(), GetFileExInfoStandard, &fa))
            return;
    }
    if (CompareFileTime(&fa.ftLastWriteTime, &g_heardTime) == 0)
        return;
    g_heardTime = fa.ftLastWriteTime;
    std::vector<HeardRule> rules;
    FILE *f = nullptr;
    if (_wfopen_s(&f, HeardAsPath().c_str(), L"rb") == 0 && f)
    {
        std::string raw;
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            raw.append(buf, got);
        fclose(f);
        if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0)
            raw.erase(0, 3);
        std::wstring w;
        const int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
        if (n > 0)
        {
            w.resize((size_t)n);
            MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &w[0], n);
        }
        size_t i = 0;
        while (i < w.size())
        {
            size_t j = w.find(L'\n', i);
            if (j == std::wstring::npos)
                j = w.size();
            const std::wstring l = w.substr(i, j - i);
            i = j + 1;
            const size_t eq = l.find(L'=');
            if (l.empty() || l[0] == L'#' || eq == std::wstring::npos)
                continue;
            HeardRule r;
            r.From = SplitWords(l.substr(0, eq)); // (lower case, no punctuation - as Whisper's words are)
            r.To = SplitWords(l.substr(eq + 1));
            if (!r.From.empty() && !r.To.empty())
                rules.push_back(r);
        }
    }
    std::sort(rules.begin(), rules.end(), [](const HeardRule &a, const HeardRule &b) { return a.From.size() > b.From.size(); });
    {
        // 2026-10-12: which rules also take the possessive (HeardRule::Poss): those whose result has none of the
        // order words below - a unit name ("blackheart", "black hawk", "primus pilus"), not "fall back right" / "four".
        static const wchar_t *kOrderWords[] = {
            L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine", L"ten", L"hundred",
            L"thousand", L"metres", L"meters", L"k", L"klicks", L"shot", L"shots", L"heat", L"hot", L"cool", L"back",
            L"right", L"left", L"north", L"south", L"east", L"west", L"nav", L"will", L"hows", L"how", L"where",
            L"wheres", L"what", L"whos", L"see", L"you", L"your", L"youre", L"youve", L"got", L"are", L"is", L"the",
            L"that", L"this", L"run", L"only", L"go", L"for", L"kill", L"attack", L"engage", L"move", L"hold",
            L"report", L"in", L"cockpit", L"head", L"his", L"torso", L"torsos", L"side", L"leg", L"legs", L"arm",
            L"arms", L"cease", L"firing", L"indirect", L"bug", L"out", L"aim", L"anywhere", L"oclock", L"clock",
            L"status", L"sitrep", L"and", L"to", L"on", L"at", L"my", L"me", L"it", L"up", L"down", L"weapons",
            L"lasers", L"missiles", L"ballistics", L"flamers", L"lrms", L"srms", L"ammo", L"contacts", L"alpha",
            L"strike", L"rally", L"regroup", L"patch", L"through", L"form", L"follow", L"advance", L"push", L"pull",
            L"fall", L"nearest", L"enemy", L"target", L"closest", L"facing", L"find", L"can", L"do", L"check",
            L"withdraw", L"pick", L"off", L"say", L"again", L"set", L"heading", L"anyone", L"low", L"lance", L"any",
            L"all", L"units", L"wait", L"here", L"stay", L"close", L"everyone", L"sound", L"bearing", L"take",
            L"hitting", L"artillery", L"focus", L"stand", L"by", L"open", L"return"};
        static const std::set<std::wstring> orderWords(std::begin(kOrderWords), std::end(kOrderWords));
        for (auto &r : rules)
        {
            r.Poss = true;
            for (auto &x : r.To)
                r.Poss = r.Poss && !orderWords.count(x);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_heardMx);
        g_heardAs.swap(rules);
    }
    Log(L"heard-as: %zu rule(s) from HeardAs.txt", g_heardAs.size());
}

std::vector<std::wstring> ApplyHeardAs(const std::vector<std::wstring> &in)
{
    std::lock_guard<std::mutex> lk(g_heardMx);
    if (g_heardAs.empty())
        return in;
    std::vector<std::wstring> out;
    for (size_t i = 0; i < in.size();)
    {
        bool hit = false;
        for (auto &r : g_heardAs) // longest first
        {
            if (i + r.From.size() > in.size())
                continue;
            if (std::equal(r.From.begin(), r.From.end(), in.begin() + (long)i))
            {
                out.insert(out.end(), r.To.begin(), r.To.end());
                i += r.From.size();
                hit = true;
                break;
            }
            // 2026-10-12: the possessive - the last heard word is the rule's last word + "s" (Whisper's "X's")
            const size_t k = r.From.size() - 1;
            if (r.Poss && in[i + k] == r.From[k] + L"s" && std::equal(r.From.begin(), r.From.end() - 1, in.begin() + (long)i))
            {
                out.insert(out.end(), r.To.begin(), r.To.end());
                if (out.back().back() != L's') // "blackhurst = blackheart's" already is one; "atlas" reads either way
                    out.back() += L"s";
                i += r.From.size();
                hit = true;
                break;
            }
        }
        if (!hit)
            out.push_back(in[i++]);
    }
    return out;
}

std::vector<std::wstring> NormalizeHeard(const std::wstring &text0)
{
    // "1.5 k" -> "1 point 5 k": the dot would split it into two numbers (2026-10-10)
    std::wstring text;
    for (size_t i = 0; i < text0.size(); ++i)
        if (text0[i] == L'.' && i > 0 && i + 1 < text0.size() && iswdigit(text0[i - 1]) && iswdigit(text0[i + 1]))
            text += L" point ";
        // "1,275" -> "1275" (2026-10-11, round trip: Whisper writes thousands with a comma, which split it into 1 and 275)
        else if (text0[i] == L',' && i > 0 && iswdigit(text0[i - 1]) && i + 3 < text0.size() && iswdigit(text0[i + 1]) &&
                 iswdigit(text0[i + 2]) && iswdigit(text0[i + 3]) && (i + 4 == text0.size() || !iswdigit(text0[i + 4])))
            continue;
        else
            text += text0[i];
    std::vector<std::wstring> in = ApplyHeardAs(SplitWords(text)), out;
    // 2026-10-15 (user, rerun review): "push to nav Romeo" heard as "an avromio" / "in avpapa" / "an aftango" - "to nav"
    // glued onto the letter. "an"/"in" + a word "av..."/"af..." whose rest is a nav letter (matched by its consonants:
    // "romio" = r-m = romeo, "yenki" = y-n-k = yankee) or "point..." becomes "to nav <letter>" ("near nav" after "ground").
    {
        auto skel = [](const std::wstring &x) {
            std::wstring k;
            for (size_t j = 0; j < x.size(); ++j)
            {
                const wchar_t c = x[j];
                if (j > 0 && wcschr(L"aeiouy", c))
                    continue;
                if (!k.empty() && k.back() == c)
                    continue;
                k += c;
            }
            return k;
        };
        auto navLetter = [&](const std::wstring &x) -> int {
            if (x.size() < 2)
                return -1;
            int hit = -1;
            for (int l = 0; l < kVhudNavLetters; ++l)
            {
                const std::wstring g = NavWord(l);
                if (x == g || skel(x) == skel(g))
                {
                    if (hit >= 0 && hit != l)
                        return -1; // two letters fit: leave it alone
                    hit = l;
                }
            }
            return hit;
        };
        std::vector<std::wstring> keep;
        for (size_t i = 0; i < in.size(); ++i)
        {
            if ((in[i] == L"an" || in[i] == L"in") && i + 1 < in.size() && in[i + 1].size() > 3 &&
                (in[i + 1].compare(0, 2, L"av") == 0 || in[i + 1].compare(0, 2, L"af") == 0))
            {
                const std::wstring rest = in[i + 1].substr(2);
                const bool point = rest.compare(0, 4, L"poin") == 0;
                const int l = point ? -1 : navLetter(rest);
                if (point || l >= 0)
                {
                    keep.push_back(i > 0 && in[i - 1] == L"ground" ? L"near" : L"to");
                    keep.push_back(L"nav");
                    keep.push_back(point ? (rest == L"points" ? L"point" : rest) : NavWord(l)); // "point..." stays for NavWordsAt ("nav pointango")
                    ++i;
                    continue;
                }
            }
            // 2026-10-15 (user, parse check): the same with "av" / "af" as its own word - "push an av point Delta",
            // "in af tango" = "to nav point delta" / "to nav tango"
            if ((in[i] == L"an" || in[i] == L"in") && i + 2 < in.size() && (in[i + 1] == L"av" || in[i + 1] == L"af"))
            {
                const std::wstring &rest = in[i + 2];
                const bool point = rest.compare(0, 4, L"poin") == 0;
                const int l = point ? -1 : navLetter(rest);
                if (point || l >= 0)
                {
                    keep.push_back(i > 0 && in[i - 1] == L"ground" ? L"near" : L"to");
                    keep.push_back(L"nav");
                    keep.push_back(point ? (rest == L"points" ? L"point" : rest) : NavWord(l));
                    i += 2;
                    continue;
                }
            }
            keep.push_back(in[i]);
        }
        in.swap(keep);
    }
    // OFF-SCRIPT PHRASES (2026-10-15, user approved list "Off-script orders - suggested fixes"): natural wording turned
    // into the words the order parser already knows, before anything else reads them. Tested with ParseUnscripted.tsv.
    {
        auto isCallsign = [](const std::wstring &x) {
            for (auto *c : {L"lance", L"all", L"everyone", L"everybody", L"one", L"two", L"three", L"four", L"guys", L"team",
                            L"boys", L"folks", L"lads", L"lancemate"})
                if (x == c)
                    return true;
            return false;
        };
        auto isNumWord = [](const std::wstring &x) {
            if (!x.empty() && iswdigit(x[0]))
                return true;
            for (auto *c : {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine", L"ten",
                            L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen", L"seventeen", L"eighteen",
                            L"nineteen", L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety",
                            L"hundred", L"thousand", L"point"})
                if (x == c)
                    return true;
            return false;
        };
        auto isUnit = [](const std::wstring &x) {
            for (auto *c : {L"metres", L"meters", L"metre", L"meter", L"m", L"klick", L"klicks", L"click", L"clicks", L"k",
                            L"kilometre", L"kilometres", L"kilometer", L"kilometers", L"km"})
                if (x == c)
                    return true;
            return false;
        };
        auto isDir = [](const std::wstring &x) {
            for (auto *c : {L"north", L"south", L"east", L"west", L"northeast", L"northwest", L"southeast", L"southwest", L"left",
                            L"right", L"forward", L"back", L"forwards", L"backwards"})
                if (x == c)
                    return true;
            return false;
        };
        auto at = [&](size_t i, std::initializer_list<const wchar_t *> p) {
            size_t k = 0;
            for (auto *x : p)
            {
                if (i + k >= in.size() || in[i + k] != x)
                    return false;
                ++k;
            }
            return true;
        };
        auto put = [&](size_t i, size_t n, const wchar_t *to) {
            const std::vector<std::wstring> t = SplitWords(to);
            in.erase(in.begin() + (long)i, in.begin() + (long)std::min(in.size(), i + n));
            in.insert(in.begin() + (long)i, t.begin(), t.end());
        };
        // a distance starts at i: number words with a unit after them, "hundred" / "thousand" in them, or not a lone 1-4
        auto distAt = [&](size_t i) {
            size_t j = i;
            bool big = false;
            while (j < in.size() && isNumWord(in[j]))
            {
                big |= in[j] == L"hundred" || in[j] == L"thousand" || iswdigit(in[j][0]);
                ++j;
            }
            if (j == i)
                return false;
            const bool lone = j == i + 1 && (in[i] == L"one" || in[i] == L"two" || in[i] == L"three" || in[i] == L"four");
            return big || (j < in.size() && isUnit(in[j])) || !lone;
        };
        // a word before the callsign ("So, Two and Three ...", "Right, Three ...", "Uh Two ...") - and "um" after it
        for (int pass = 0; pass < 2 && in.size() >= 2; ++pass)
        {
            static const wchar_t *kLead[] = {L"so", L"right", L"well", L"listen", L"ok", L"okay", L"alright", L"hey", L"a",
                                             L"ah", L"uh", L"um", L"er", L"erm", L"now", L"sorry"};
            bool lead = false;
            for (auto *k : kLead)
                lead |= in[0] == k;
            if (lead && isCallsign(in[1]) && !distAt(1)) // not "right, two hundred"
                in.erase(in.begin());
            else if (in[0] == L"listen" && in[1] == L"up" && in.size() > 2 && isCallsign(in[2]))
                in.erase(in.begin(), in.begin() + 2);
        }
        // "Two, let's get that Jenner": "lets" is a misheard "Lance" in the callsign list - after a callsign it is just "let's"
        for (size_t i = 1; i + 1 < in.size(); ++i)
            if (in[i] == L"lets" && isCallsign(in[i - 1]))
                in.erase(in.begin() + (long)i);
        if (in.size() > 2 && isCallsign(in[0]))
            for (auto *k : {L"arm", L"um", L"uh", L"ah", L"er", L"erm", L"a"})
                if (in[1] == k)
                {
                    in.erase(in.begin() + 1);
                    break;
                }
        // MID-SENTENCE CORRECTIONS: "300 east, no wait, west" / "200 ... actually 400" / "nav Alpha, sorry, nav Bravo" /
        // "200, I mean 300" - what follows the correction word replaces the same kind of thing before it
        {
            static const wchar_t *kMark[][2] = {{L"no", L"wait"}, {L"wait", L"no"}, {L"i", L"mean"}, {L"make", L"that"},
                                                {L"actually", nullptr}, {L"sorry", nullptr}, {L"correction", nullptr}};
            size_t m = in.size(), ml = 0;
            for (size_t i = 1; i < in.size() && m == in.size(); ++i)
                for (auto &k : kMark)
                    if (in[i] == k[0] && (!k[1] || (i + 1 < in.size() && in[i + 1] == k[1])))
                    {
                        m = i;
                        ml = k[1] ? 2 : 1;
                        break;
                    }
            if (m < in.size())
            {
                size_t first = 0; // the order starts after the callsign words
                while (first < m && (isCallsign(in[first]) || in[first] == L"and"))
                    ++first;
                bool aDir = false, aNum = false, aNav = false;
                for (size_t i = m + ml; i < in.size(); ++i)
                {
                    aDir |= isDir(in[i]);
                    aNum |= isNumWord(in[i]) && distAt(i);
                    aNav |= (in[i] == L"nav" || in[i] == L"waypoint" || in[i] == L"navpoint") && i + 1 < in.size();
                }
                std::vector<bool> drop(in.size(), false);
                bool any = false;
                auto lastSpan = [&](auto pred) {
                    size_t e = m;
                    while (e > first && !pred(in[e - 1]))
                        --e;
                    size_t s = e;
                    while (s > first && pred(in[s - 1]))
                        --s;
                    if (e > s)
                    {
                        for (size_t k = s; k < e; ++k)
                            drop[k] = true;
                        any = true;
                    }
                };
                if (aDir)
                    lastSpan(isDir);
                if (aNum)
                    lastSpan(isNumWord);
                if (aNav)
                    for (size_t k = m; k-- > first;)
                        if ((in[k] == L"nav" || in[k] == L"waypoint" || in[k] == L"navpoint") && k + 1 < m)
                        {
                            drop[k] = drop[k + 1] = true;
                            any = true;
                            break;
                        }
                if (any)
                {
                    for (size_t k = m; k < m + ml; ++k)
                        drop[k] = true;
                    std::vector<std::wstring> keep;
                    for (size_t k = 0; k < in.size(); ++k)
                        if (!drop[k])
                            keep.push_back(in[k]);
                    in.swap(keep);
                }
            }
        }
        for (size_t i = 0; i < in.size(); ++i)
        {
            const bool last = i + 1 == in.size();
            // wrong actions
            if (at(i, {L"back", L"it", L"up"}))
                put(i, 3, L"back");
            else if (at(i, {L"back", L"up"}) && i + 2 < in.size() && distAt(i + 2))
                put(i, 2, L"back");
            else if (at(i, {L"backpedal"}))
                put(i, 1, L"back");
            else if (at(i, {L"half", L"a"}) && i + 2 < in.size() && isUnit(in[i + 2]))
                put(i, 3, L"five hundred metres");
            else if ((at(i, {L"a"}) || at(i, {L"one"})) && i + 4 < in.size() && isUnit(in[i + 1]) && in[i + 2] == L"and" &&
                     in[i + 3] == L"a" && in[i + 4] == L"half")
                put(i, 5, L"fifteen hundred metres");
            else if (at(i, {L"one", L"and", L"a", L"half"}) && i + 4 < in.size() && isUnit(in[i + 4]))
                put(i, 5, L"fifteen hundred metres");
            else if (at(i, {L"light", L"up"}) || at(i, {L"light", L"em", L"up"}) || at(i, {L"light", L"it", L"up"}) ||
                     at(i, {L"light", L"them", L"up"}))
            {
                const size_t n = in[i + 1] == L"up" ? 2 : 3;
                put(i, n, i + n >= in.size() ? L"weapons free" : L"attack");
            }
            else if (at(i, {L"hang", L"back"}))
                put(i, 2, i + 2 < in.size() && distAt(i + 2) ? L"back" : L"hold here");
            else if (at(i, {L"sit", L"on"}) && i + 2 < in.size() && (in[i + 2] == L"nav" || in[i + 2] == L"waypoint"))
                put(i, 2, L"hold at");
            else if (at(i, {L"drop", L"back"}))
                put(i, 2, L"fall back");
            else if ((at(i, {L"shell"}) || at(i, {L"bombard"})) && i + 1 < in.size() && (in[i + 1] == L"nav" || in[i + 1] == L"waypoint"))
                put(i, 1, L"indirect fire at");
            else if (at(i, {L"artillery", L"on"}))
                put(i, 2, L"indirect fire at");
            else if (at(i, {L"all", L"fire", L"on"}))
                put(i, 3, L"all attack");
            // Whisper's "you're" for "your" before a clock
            else if (in[i] == L"youre" && i + 2 < in.size() &&
                     (in[i + 2] == L"oclock" || (in[i + 2] == L"o" && i + 3 < in.size() && in[i + 3] == L"clock")))
                in[i] = L"your";
            // "Two, fall in" heard as "Two fallen"
            else if (in[i] == L"fallen" && last && (i == 0 || isCallsign(in[i - 1])))
                put(i, 1, L"fall in");
            // phrasings that were not understood
            else if (at(i, {L"sit", L"tight"}) || at(i, {L"park", L"it"}))
                put(i, 2, L"hold position");
            else if (at(i, {L"freeze"}))
                put(i, 1, L"hold position");
            else if (at(i, {L"dont", L"move"}) && (i + 2 == in.size() || in[i + 2] == L"here" || in[i + 2] == L"there" || in[i + 2] == L"until"))
                put(i, 2, L"hold position");
            else if (at(i, {L"on", L"my", L"six"}))
                put(i, 3, L"on me");
            else if (at(i, {L"get", L"over", L"here"}))
                put(i, 3, L"come here");
            else if (at(i, {L"go", L"after", L"my", L"target"}))
                put(i, 4, L"attack my target");
            else if (at(i, {L"shoot", L"what", L"im", L"shooting"}) || at(i, {L"shoot", L"what", L"i", L"am", L"shooting"}))
                put(i, in[i + 2] == L"im" ? 4 : 5, L"attack my target");
            else if (at(i, {L"cleared", L"to", L"fire"}) || at(i, {L"clear", L"to", L"fire"}))
                put(i, 3, L"weapons free");
            else if (at(i, {L"only", L"fire", L"if", L"fired", L"upon"}) || at(i, {L"fire", L"only", L"if", L"fired", L"upon"}))
                put(i, 5, L"return fire");
            else if (at(i, {L"let", L"it", L"rip"}))
                put(i, 3, L"run hot");
            else if (at(i, {L"cover"}) && i + 1 < in.size() &&
                     (in[i + 1] == L"two" || in[i + 1] == L"three" || in[i + 1] == L"four" || in[i + 1] == L"one"))
                put(i, 1, L"support");
            else if (i + 2 < in.size() && (in[i] == L"two" || in[i] == L"three" || in[i] == L"four" || in[i] == L"one") &&
                     (in[i + 1] == L"needs" || in[i + 1] == L"need") && (in[i + 2] == L"help" || in[i + 2] == L"support"))
            {
                const std::wstring s = in[i];
                put(i, 3, (L"support " + s).c_str());
            }
            else if (at(i, {L"some", L"elevation"}) || at(i, {L"elevation"}))
                put(i, in[i] == L"some" ? 2 : 1, L"some high ground");
            else if ((at(i, {L"are", L"you", L"okay"}) || at(i, {L"are", L"you", L"ok"}) || at(i, {L"are", L"you", L"alright"})) &&
                     i + 3 == in.size())
                put(i, 3, L"how are you");
            else if ((at(i, {L"you", L"okay"}) || at(i, {L"you", L"ok"}) || at(i, {L"you", L"alright"}) || at(i, {L"you", L"good"})) &&
                     i + 2 == in.size())
                put(i, 2, L"how are you");
        }
    }
    // "your 2 o'clock" is the LANCEMATE's clock (2026-10-10, user) - "your" is filler everywhere else, so it is kept here as "yours"
    for (size_t i = 0; i + 2 < in.size(); ++i)
        if (in[i] == L"your" && (in[i + 2] == L"oclock" || (in[i + 2] == L"o" && i + 3 < in.size() && in[i + 3] == L"clock")))
            in[i] = L"yours";
    // 2026-10-11 (round trip): "all units" is "all" (questions stopped at "units"); "two dash four dash zero" (a heading
    // read out digit by digit) loses the dashes between number words
    {
        static const wchar_t *kDigitWord[] = {L"zero", L"oh", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight",
                                              L"nine", L"niner"};
        auto digitWord = [&](const std::wstring &x) {
            for (auto *d : kDigitWord)
                if (x == d)
                    return true;
            return false;
        };
        std::vector<std::wstring> keep;
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == L"units" && i > 0 && in[i - 1] == L"all")
                continue;
            if (in[i] == L"dash" && i > 0 && i + 1 < in.size() && digitWord(in[i - 1]) && digitWord(in[i + 1]))
                continue;
            keep.push_back(in[i]);
        }
        in.swap(keep);
    }
    static const struct
    {
        const wchar_t *From, *To;
    } kWord[] = {
        {L"meters", L"metres"}, {L"meter", L"metres"}, {L"metre", L"metres"}, {L"m", L"metres"},
        {L"backwards", L"back"}, {L"backward", L"back"}, {L"forwards", L"forward"}, {L"ahead", L"forward"},
        {L"ppc", L"p p c"}, {L"ppcs", L"p p cs"}, {L"pbc", L"p p c"}, {L"pbcs", L"p p cs"},
        {L"lrm", L"long range missiles"}, {L"lrms", L"long range missiles"},
        {L"srm", L"short range missiles"}, {L"srms", L"short range missiles"},
        {L"mg", L"machine guns"}, {L"mgs", L"machine guns"}, {L"laser", L"lasers"}, {L"missile", L"missiles"},
        {L"flamer", L"flamers"}, {L"ballistic", L"ballistics"}, {L"tagged", L"tag"}, {L"nark", L"narc"},
        {L"everybody", L"everyone"}, {L"retreat", L"fall back"}, {L"regroup", L"form on me"},
        {L"cooler", L"cool"}, {L"cold", L"cool"}, {L"positions", L"position"}, {L"targets", L"target"},
        // build 2026-10-06b, from the heard log
        {L"guys", L"everyone"}, {L"boys", L"everyone"}, {L"team", L"everyone"}, {L"folks", L"everyone"},
        {L"lads", L"everyone"}, {L"gents", L"everyone"}, {L"gentlemen", L"everyone"}, {L"fellas", L"everyone"},
        {L"yall", L"everyone"}, // 2026-10-10 (user): group words = the whole lance
        {L"studies", L"status"}, {L"statues", L"status"}, {L"stats", L"status"}, {L"army", L"on me"},
    };
    static const wchar_t *kFiller[] = {L"please", L"okay", L"ok", L"um", L"uh", L"er", L"erm", L"hey", L"alright",
                                       L"copy", L"now", L"the", L"lancemate", L"number", L"unit", L"your"};
    for (auto &w : in)
    {
        // Digits: "200" -> "two hundred", "200m" -> "two hundred metres".
        size_t d = 0;
        while (d < w.size() && iswdigit(w[d]))
            ++d;
        if (d > 0 && d <= 4)
        {
            for (auto &x : SplitWords(NumberWords(_wtoi(w.substr(0, d).c_str()))))
                out.push_back(x);
            const std::wstring rest = w.substr(d);
            if (rest.empty() || rest == L"s") // "Atlas 3's" -> "atlas three" (2026-10-11: the "s" was left as a word)
                continue;
            if (rest == L"m" || rest == L"metres" || rest == L"meters")
            {
                out.push_back(L"metres");
                continue;
            }
            out.push_back(rest); // "2nd" and the like: left as said
            continue;
        }
        bool filler = false;
        for (auto *f : kFiller)
            filler |= w == f;
        // RADIO CHECK (2026-10-13): "how copy" / "do you copy" / "anyone copy" - here "copy" is the question, not filler
        // 2026-10-14 (user): "how copy" dropped - Whisper hears it as "help" (round trip, 248 of ~300)
        if (filler && w == L"copy" && !out.empty() && (out.back() == L"you" || out.back() == L"anyone"))
            filler = false;
        if (filler)
            continue;
        bool mapped = false;
        for (auto &k : kWord)
            if (w == k.From)
            {
                for (auto &x : SplitWords(k.To))
                    out.push_back(x);
                mapped = true;
                break;
            }
        if (!mapped)
            out.push_back(w);
    }
    // "to hundred" is "two hundred", "for hundred" "four hundred".
    for (size_t i = 0; i + 1 < out.size(); ++i)
        if (out[i + 1] == L"hundred")
        {
            if (out[i] == L"to" || out[i] == L"too")
                out[i] = L"two";
            else if (out[i] == L"for" || out[i] == L"fore")
                out[i] = L"four";
        }
    // "Lance, status" run together: "landstoas", "lanstatus" (build 2026-10-06b).
    for (size_t i = 0; i < out.size(); ++i)
    {
        static const wchar_t *kRunS[] = {L"landst", L"lanst", L"lancest", L"lantst", L"lancst"};
        for (auto *k : kRunS)
            if (out[i].size() > wcslen(k) && out[i].compare(0, wcslen(k), k) == 0)
            {
                out[i] = L"lance";
                out.insert(out.begin() + (long)i + 1, L"status");
                break;
            }
    }
    // "Lance, move" run together by Whisper: "landsmuth", "landsmov", "lansmove", "land smooth".
    for (size_t i = 0; i < out.size(); ++i)
    {
        const std::wstring &t = out[i];
        static const wchar_t *kRun[] = {L"landsm", L"lansm", L"lancem", L"lantsm", L"lancesm"};
        for (auto *k : kRun)
            if (t.size() > wcslen(k) && t.compare(0, wcslen(k), k) == 0)
            {
                out[i] = L"lance";
                out.insert(out.begin() + (long)i + 1, L"move");
                break;
            }
    }
    for (size_t i = 0; i + 1 < out.size(); ++i)
        if ((out[i + 1] == L"smooth" || out[i + 1] == L"smove" || out[i + 1] == L"smoove") &&
            (out[i] == L"land" || out[i] == L"lands" || out[i] == L"lance" || out[i] == L"lamps"))
        {
            out[i] = L"lance";
            out[i + 1] = L"move";
        }
    // "hundred" on its own is "one hundred". (2026-10-10: any one..nineteen before it counts - "six hundred" was
    // "six one hundred", read as 106, since the list stopped at five when moves stopped at 500 m)
    static const wchar_t *kBeforeHundred[] = {L"a", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine",
                                              L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen",
                                              L"seventeen", L"eighteen", L"nineteen"};
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == L"hundred" &&
            (i == 0 || std::none_of(std::begin(kBeforeHundred), std::end(kBeforeHundred), [&](const wchar_t *x) { return out[i - 1] == x; })))
        {
            out.insert(out.begin() + (long)i, L"one");
            ++i;
        }
    // "2K ride" / "500 metres ride" = right (2026-10-11, user): only straight after a distance or its unit
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i] == L"ride")
            for (const wchar_t *k : {L"k", L"km", L"klick", L"klicks", L"kilometres", L"metres", L"hundred", L"thousand", L"fifty",
                                     L"twenty", L"thirty", L"forty", L"sixty", L"seventy", L"eighty", L"ninety", L"one", L"two",
                                     L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine", L"ten"})
                if (out[i - 1] == k)
                {
                    out[i] = L"right";
                    break;
                }
    // Whole-phrase synonyms.
    static const struct
    {
        const wchar_t *From, *To;
    } kPhrase[] = {
        {L"cease fire", L"hold fire"}, {L"ceasefire", L"hold fire"}, {L"stop firing", L"hold fire"},
        {L"hold your fire", L"hold fire"}, {L"weapons hold", L"hold fire"}, {L"fire at will", L"weapons free"},
        {L"open fire", L"weapons free"}, {L"free fire", L"weapons free"}, {L"return fire only", L"return fire"},
        {L"pull back", L"fall back"}, {L"form up", L"form on me"}, {L"on me", L"form on me"},
        {L"cool down", L"run cool"}, {L"go hot", L"run hot"}, {L"alpha strikes", L"alpha strike"},
        {L"reset fire order", L"reset fire orders"}, {L"sit rep", L"sitrep"}, {L"what is", L"whats"},
        {L"status update", L"status report"}, {L"watch status", L"whats status"}, {L"what status", L"whats status"},
        {L"wants status", L"whats status"},
    };
    std::wstring s = L" " + JoinWords(out) + L" ";
    for (auto &k : kPhrase)
    {
        const std::wstring f = L" " + std::wstring(k.From) + L" ", t = L" " + std::wstring(k.To) + L" ";
        for (size_t at = s.find(f); at != std::wstring::npos; at = s.find(f, at + t.size() - 1))
            s.replace(at, f.size(), t);
    }
    // "form on me" from "on me" inside "form on me" would double up: undo that.
    for (size_t at = s.find(L" form form on me "); at != std::wstring::npos; at = s.find(L" form form on me "))
        s.replace(at, 17, L" form on me ");
    return SplitWords(s);
}

// Callsign words at the start of what was heard. Homophones only count here ("to" is "two").
int WhoWord(const std::wstring &w)
{
    static const struct
    {
        const wchar_t *W;
        int            Slot;
    } kWho[] = {{L"all", 0},  {L"lance", 0}, {L"everyone", 0}, {L"lands", 0}, {L"lants", 0}, {L"lanz", 0}, {L"lantz", 0},
                {L"lens", 0}, {L"lanes", 0}, {L"lancs", 0}, {L"land", 0}, {L"lamps", 0}, {L"lamp", 0}, {L"lents", 0},
                {L"lets", 0}, {L"lances", 0}, {L"everybody", 0},
                // misheard "Lance" (heard log, build 2026-10-06b) - only at the start, before an order
                {L"plant", 0}, {L"plants", 0}, {L"learn", 0}, {L"line", 0}, {L"lines", 0}, {L"mans", 0}, {L"science", 0},
                {L"lan", 0}, {L"lanc", 0},
                {L"one", 1},   {L"won", 1},    {L"two", 2},
                {L"to", 2},   {L"too", 2},   {L"tu", 2},       {L"three", 3}, {L"tree", 3},   {L"four", 4},
                {L"for", 4},  {L"fore", 4},  {L"five", 5},     {L"six", 6},   {L"seven", 7},  {L"eight", 8}};
    for (auto &k : kWho)
        if (w == k.W)
            return k.Slot;
    return -1;
}

double WordSim(const std::wstring &a, const std::wstring &b)
{
    if (a == b)
        return 1.0;
    const size_t n = a.size(), m = b.size();
    std::vector<size_t> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j)
        prev[j] = j;
    for (size_t i = 1; i <= n; ++i)
    {
        cur[0] = i;
        for (size_t j = 1; j <= m; ++j)
            cur[j] = std::min(std::min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1));
        std::swap(prev, cur);
    }
    const double mx = (double)std::max(n, m);
    return mx > 0 ? 1.0 - prev[m] / mx : 1.0;
}

// ---- UNIT NAMES WITH NUMERALS (2026-10-10) ------------------------------------------------------
// "Atlas II", "Hunchback IIC", "Mad Cat Mk II": the numeral is the same word however it is written or
// said - "ii" / "two" / "2" -> "2", "iic" -> "2 c" ("two see"), "mk" / "mark" -> "mk" - and a name said
// WITHOUT its numeral ("attack the Atlas") also finds an Atlas II when no plain Atlas is about (the base
// name: its numeral words taken off the end).
std::wstring NumTok(const std::wstring &t)
{
    if (t == L"ii" || t == L"two" || t == L"2" || t == L"iis") // "iis" / "iiis": "Atlas III's" (2026-10-11)
        return L"2";
    if (t == L"iii" || t == L"three" || t == L"3" || t == L"iiis" || t == L"threes")
        return L"3";
    if (t == L"iv" || t == L"four" || t == L"4")
        return L"4";
    if (t == L"mark" || t == L"mk")
        return L"mk";
    return t;
}
static bool IsNumeralWord(const std::wstring &t) { return t == L"2" || t == L"3" || t == L"4" || t == L"c" || t == L"mk"; }
// A name's words in that common form ("Hunchback IIC" -> hunchback 2 c).
std::vector<std::wstring> NameTokens(const std::wstring &name)
{
    std::vector<std::wstring> out;
    for (auto &t : SplitWords(name))
    {
        if (t == L"iic" || t == L"iiic" || t == L"iics" || t == L"iiics") // "iics": "Rifleman IIC's" (2026-10-11)
        {
            out.push_back(t == L"iic" || t == L"iics" ? L"2" : L"3");
            out.push_back(L"c");
            continue;
        }
        // "Hermes too", "Mad Cat Mark too" (2026-10-10) - after a word or "mark", not after another number
        if ((t == L"to" || t == L"too" || t == L"tu" || t == L"twos") && !out.empty() &&
            (out.back() == L"mk" || !IsNumeralWord(out.back())))
        {
            out.push_back(L"2");
            continue;
        }
        if ((t == L"see" || t == L"sea" || t == L"c" || t == L"cs" || t == L"sees" || t == L"seas") && !out.empty() &&
            (out.back() == L"2" || out.back() == L"3")) // "cs": "Hunchback two C's" (2026-10-11)
        {
            out.push_back(L"c");
            continue;
        }
        out.push_back(NumTok(t));
    }
    return out;
}
static bool IsNumeralTok(const std::wstring &t) { return t == L"2" || t == L"3" || t == L"4" || t == L"c" || t == L"mk"; }
// The name without its numeral ("Atlas II" -> "atlas", "Mad Cat Mk II" -> "mad cat"); empty if it has none.
std::wstring BaseUnitName(const std::wstring &name)
{
    std::vector<std::wstring> t = NameTokens(name);
    const size_t n = t.size();
    while (t.size() > 1 && IsNumeralTok(t.back()))
        t.pop_back();
    return t.size() == n ? std::wstring() : JoinWords(t);
}
bool HasNumeral(const std::wstring &name) { return !BaseUnitName(name).empty(); }
// The patterns the parser looks for: each known name in its common form, and (Base) its name without
// the numeral. Name = what the parser hands on (FindNamedUnit / ShadowTarget understand both).
struct NamePattern
{
    std::vector<std::wstring> Tok;
    std::wstring              Name;
    bool                      Base;
};
std::vector<NamePattern> NamePatterns(const std::vector<std::wstring> &known)
{
    std::vector<NamePattern> p;
    for (auto &k : known)
        if (!NameTokens(k).empty())
            p.push_back({NameTokens(k), k, false});
    for (auto &k : known)
    {
        const std::wstring b = BaseUnitName(k);
        if (b.empty())
            continue;
        bool have = false; // a plain one of that name is known, or this base is listed already
        for (auto &x : p)
            have |= JoinWords(x.Tok) == b;
        if (!have)
            p.push_back({SplitWords(b), b, true});
    }
    return p;
}
// How many heard words from i match the pattern (0 = no match). A heard word matches a name word when
// it is the same (numerals in common form), its plural, or nearly the same for words of 4+ letters.
size_t MatchNameAt(const std::vector<std::wstring> &w, const std::vector<bool> *used, size_t i, const std::vector<std::wstring> &tok)
{
    size_t j = i;
    for (size_t k = 0; k < tok.size(); ++k)
    {
        if (j >= w.size() || (used && (*used)[j]))
            return 0;
        const std::wstring &e = tok[k];
        // a heard "iic" is two name words
        if (w[j] == L"iic" || w[j] == L"iiic" || w[j] == L"iics" || w[j] == L"iiics") // + possessive (2026-10-11)
        {
            if (e == (w[j] == L"iic" || w[j] == L"iics" ? L"2" : L"3") && k + 1 < tok.size() && tok[k + 1] == L"c")
            {
                ++k;
                ++j;
                continue;
            }
            return 0;
        }
        std::wstring h = NumTok(w[j]);
        if (e == L"c" && (h == L"see" || h == L"sea" || h == L"cs" || h == L"sees" || h == L"seas")) // "two C's" (2026-10-11)
            h = L"c";
        // "Hermes too" (2026-10-10) - only at the end or before "c" ("Jenner to the north" is not a Jenner II)
        if (e == L"2" && k > 0 && (h == L"to" || h == L"too" || h == L"tu") &&
            (j + 1 == w.size() || w[j + 1] == L"c" || w[j + 1] == L"see" || w[j + 1] == L"sea" || w[j + 1] == L"cs"))
            h = L"2";
        auto closeTo = [](const std::wstring &a, const std::wstring &b) {
            return a == b || a == b + L"s" || (b.size() >= 4 && WordSim(a, b) >= 0.75);
        };
        if (closeTo(h, e))
        {
            ++j;
            continue;
        }
        // one word heard for two name words ("madcat" = Mad Cat) or two heard for one ("black heart" =
        // Blackheart) - 2026-10-10
        if (k + 1 < tok.size() && closeTo(h, e + tok[k + 1]))
        {
            ++k;
            ++j;
            continue;
        }
        if (j + 1 < w.size() && !(used && (*used)[j + 1]) && closeTo(h + NumTok(w[j + 1]), e))
        {
            j += 2;
            continue;
        }
        return 0;
    }
    return j - i;
}
// The best name in the heard words: full names first (the longest wins: "Atlas II" over "Atlas"), a
// base name only when no full name is there. at = from which word (anchor: only there).
bool FindNameInWords(const std::vector<std::wstring> &w, const std::vector<bool> *used, const std::vector<std::wstring> &known,
                     std::wstring &name, size_t &at, size_t &len, size_t anchor = std::wstring::npos)
{
    const std::vector<NamePattern> pats = NamePatterns(known);
    for (int pass = 0; pass < 2; ++pass)
    {
        size_t bestLen = 0;
        for (auto &pt : pats)
        {
            if (pt.Base != (pass == 1))
                continue;
            const size_t lo = anchor == std::wstring::npos ? 0 : anchor, hi = anchor == std::wstring::npos ? w.size() : anchor + 1;
            for (size_t i = lo; i < hi && i < w.size(); ++i)
            {
                const size_t m = MatchNameAt(w, used, i, pt.Tok);
                if (m > bestLen)
                {
                    bestLen = m;
                    name = pt.Name;
                    at = i;
                    len = m;
                }
            }
        }
        if (bestLen)
            return true;
    }
    return false;
}

// 1 = the same words; a wrong / missing / extra word costs 1, a nearly right one less.
double OrderScore(const std::vector<std::wstring> &heard, size_t from, const std::vector<std::wstring> &order)
{
    const size_t n = heard.size() - from, m = order.size();
    std::vector<double> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j)
        prev[j] = (double)j;
    for (size_t i = 1; i <= n; ++i)
    {
        cur[0] = (double)i;
        for (size_t j = 1; j <= m; ++j)
        {
            // A nearly right word ("positions") costs little; a different one ("south" for "north",
            // "three" for "two") costs as much as a wrong one.
            const double sim = WordSim(heard[from + i - 1], order[j - 1]);
            cur[j] = std::min(std::min(prev[j] + 1.0, cur[j - 1] + 1.0), prev[j - 1] + (sim >= 0.75 ? 1.0 - sim : 1.0));
        }
        std::swap(prev, cur);
    }
    const double mx = (double)std::max(n, m);
    return mx > 0 ? 1.0 - prev[m] / mx : 0.0;
}

bool SameOrder(const Cmd &a, const Cmd &b) { return a.Type == b.Type && a.A == b.A && a.B == b.B; }

// What the main loop is handed (Execute and Say run there only: one thread uses the voice).
struct SpokenAction
{
    bool SayAgain = false;
    std::wstring ReplyGroup; // 2026-10-10: a remark answered with a line only ("good kill" -> reply_thanks), no order
    bool Correct = false; // CORRECTIONS (2026-10-07a): "actually ..." - replaces the last order, does not add to it
    bool Partial = false; // ... only the last move's distance / direction ("actually, 300") - nothing put back
    bool Cancel = false;  // ... "belay that" with no new order
    Cmd  C{};
    int  Query = 0;          // QUESTIONS: Q_* (C.Who = who was asked, -1 = nobody named)
    std::wstring QueryName;  // "where's the Atlas" -> "atlas"
};
enum QuestionKind
{
    Q_None = 0, Q_Where, Q_WhereNamed, Q_WhereYou, Q_What, Q_HowBad,
    // build 2026-10-06a (MORE QUESTIONS)
    Q_HowMany, Q_Facing, Q_Closest, Q_WhoHits, Q_Target, Q_CanSee, Q_IsDown, Q_Ammo, Q_Heat, Q_Hurt, Q_SoundOff, Q_UnitHow,
    Q_Air, Q_WhoClosest, Q_Repeat,
    // build 2026-10-06c: who is firing on YOU, and the answer to a pilot's patch-through offer
    Q_WhoFiring, Q_PatchYes, Q_PatchNo,
    Q_PatchCmd, // "patch it through" / "patch in the Atlas" / "patch the closest contact in" with no offer (2026-10-06g)
    Q_Class,    // "any heavies?" / "any turrets out there?" - how many of that kind, the nearest one (2026-10-07j)
    Q_Heading,  // "which way is the Shadow Hawk facing?" - toward you / a lancemate / away / side-on (2026-10-11)
    Q_RadioCheck, // "radio check" / "comms check" / "how copy" - each answers about the RADIO, not its damage (2026-10-13)
    Q_HillPick,   // HIGH GROUND (2026-10-14): the answer to "which hill?" - QueryName = the code (HillAnswerCode)
    Q_HillCancel  // ... "never mind" / "none of them": the marks go
};

// "I'm taking fire, can anyone see where it's coming from?" / "can anyone see who's firing those LRMs" /
// "does anyone have visual on whoever's firing those arrows?" (build 2026-10-06c). Any length: a
// question word (who / where / see / visual / eyes) with a firing word (firing / shooting / coming
// from / taking fire ...), about fire on YOU (not "who's shooting at you" - that is Q_WhoHits).
// weapon: lrm srm arrow artillery missile ppc laser gauss ac mg, or "".
bool FiringQuestion(const std::vector<std::wstring> &w, std::wstring &weapon)
{
    const std::wstring t = L" " + JoinWords(w) + L" ";
    auto has = [&](std::initializer_list<const wchar_t *> l) {
        for (auto *x : l)
            if (t.find(std::wstring(L" ") + x + L" ") != std::wstring::npos)
                return true;
        return false;
    };
    const bool fire = has({L"firing", L"shooting", L"coming from", L"hitting me", L"hitting us", L"taking fire", L"shot at",
                           L"launching", L"lobbing", L"incoming", L"fired", L"throwing"});
    if (!has({L"who", L"whos", L"where", L"wheres", L"see", L"visual", L"eyes", L"spot", L"which", L"whoever", L"whats", L"what"}))
        return false;
    if (has({L"at you", L"hitting you", L"on you", L"shooting you"}) && !has({L"me", L"us", L"taking fire"}))
        return false; // about the pilot itself
    weapon.clear();
    static const struct
    {
        const wchar_t *W, *K;
    } kW[] = {{L"long range missiles", L"lrm"}, {L"short range missiles", L"srm"}, {L"streaks", L"srm"}, {L"streak", L"srm"},
              {L"arrows", L"arrow"},           {L"arrow", L"arrow"},           {L"artillery", L"artillery"}, {L"arty", L"artillery"},
              {L"mortars", L"artillery"},      {L"missiles", L"missile"},      {L"rockets", L"missile"},     {L"p p cs", L"ppc"},
              {L"p p c", L"ppc"},              {L"lasers", L"laser"},          {L"gauss", L"gauss"},         {L"autocannons", L"ac"},
              {L"autocannon", L"ac"},          {L"cannons", L"ac"},            {L"machine guns", L"mg"}};
    for (auto &k : kW)
        if (has({k.W}))
        {
            weapon = k.K;
            break;
        }
    if (weapon.empty()) // AC / UAC / RAC / LB-X with any number: "ac2s", "ac tens", "uac5" (2026-10-06e)
        for (auto x : w)
        {
            if (!x.empty() && x.back() == L's')
                x.pop_back();
            size_t k = 0;
            if (x.compare(0, 3, L"uac") == 0 || x.compare(0, 3, L"rac") == 0)
                k = 3;
            else if (x.compare(0, 2, L"ac") == 0)
                k = 2;
            else if (x == L"lbx" || x == L"lb" || x.compare(0, 3, L"lbx") == 0)
                k = x.size();
            if (!k)
                continue;
            bool digits = true;
            for (size_t i = k; i < x.size(); ++i)
                digits &= iswdigit(x[i]) != 0;
            if (digits)
            {
                weapon = L"ac";
                break;
            }
        }
    if (!fire && weapon.empty()) // "can anyone see where those LRMs" - a weapon and a question word is enough
        return false;
    // fire on YOU: "taking fire", "me / us", a weapon, "whoever", "those", "coming from", "incoming"
    return !weapon.empty() || has({L"me", L"us", L"taking fire", L"whoever", L"those", L"these", L"coming from", L"incoming", L"anyone",
                                   L"anybody"});
}

// QUESTIONS (build 2026-10-05l): what you ask a pilot about what it just reported, or about itself.
// "where?" / "from where?" / "say position" / "how far?"   -> where the unit it last named is NOW
// "where's the Atlas?" / "find the Hunchback"                -> the last reported (else nearest) one
// "where are you?" / "say your position"                     -> where the pilot is, from you
// "what is it?" / "what are we looking at?"                  -> what the unit it last named is
// "how bad?" / "you okay?" / "damage report"                 -> its damage, briefly
// No callsign needed: it goes to whoever last reported something (or everyone, for "where are you").
int ParseQuestion(std::vector<std::wstring> w, std::wstring &name)
{
    static const wchar_t *kLead[] = {L"and", L"so", L"ok", L"okay", L"copy", L"roger", L"um", L"uh", L"wait", L"hey"};
    static const wchar_t *kTail[] = {L"now", L"again", L"exactly", L"then", L"commander", L"please", L"over"};
    bool cut = true;
    while (cut && !w.empty())
    {
        cut = false;
        for (auto *k : kLead)
            if (!w.empty() && w.front() == k)
            {
                w.erase(w.begin());
                cut = true;
            }
        for (auto *k : kTail)
            if (w.size() > 1 && w.back() == k)
            {
                w.pop_back();
                cut = true;
            }
    }
    // "Where is Nav Alpha?" / "How far to Nav Bravo?" / "Bearing to Nav Charlie" (2026-10-07k)
    if (!w.empty() && (w[0] == L"where" || w[0] == L"wheres" || w[0] == L"how" || w[0] == L"hows" || w[0] == L"bearing" ||
                       w[0] == L"distance" || w[0] == L"range" || w[0] == L"direction" || w[0] == L"which" || w[0] == L"what" ||
                       w[0] == L"whats" || w[0] == L"find" || w[0] == L"locate"))
        for (size_t i = 1; i < w.size(); ++i)
        {
            size_t len = 0;
            const int nl = NavWordsAt(w, i, len);
            if (nl >= 0)
            {
                name = L"nav " + std::to_wstring(nl);
                return Q_WhereNamed;
            }
        }
    // "What? Where did the Dasher go?" (2026-10-07j): a "what" in front of another question is dropped
    if (w.size() > 2 && w[0] == L"what" && (w[1] == L"where" || w[1] == L"wheres" || w[1] == L"who" || w[1] == L"whos" ||
                                            w[1] == L"how" || w[1] == L"hows" || w[1] == L"what" || w[1] == L"whats"))
        w.erase(w.begin());
    if (w.size() > 8) // a long one: only "I'm taking fire, can anyone see where it's coming from?" (build 2026-10-06c)
        return FiringQuestion(w, name) ? Q_WhoFiring : Q_None;
    if (w.empty())
        return Q_None;
    const std::wstring t = JoinWords(w);
    auto in = [&](std::initializer_list<const wchar_t *> l) {
        for (auto *x : l)
            if (t == x)
                return true;
        return false;
    };
    if (in({L"where are you", L"where you at", L"where are you at", L"your position", L"say your position", L"whats your position",
            L"what is your position", L"position check", L"where are you guys", L"where is everyone", L"wheres everyone",
            L"where are you all", L"give me your position", L"report your position", L"report position"}))
        return Q_WhereYou;
    if (in({L"what is it", L"whats it", L"what is that", L"whats that", L"what are they", L"what are those", L"what kind",
            L"what type", L"what kind of mech", L"what mech", L"what chassis", L"identify", L"identify it", L"what are we looking at",
            L"what have you got", L"whatve you got", L"what you got", L"what do you see", L"which one", L"what is he", L"whats he",
            L"what kind is it", L"what is it exactly", L"what are we dealing with", L"what is that thing", L"whats that thing"}))
        return Q_What;
    // CLASS (2026-10-07j): "any heavies", "are there any turrets out there", "how many assaults"
    {
        static const wchar_t *kPre[] = {L"are there any", L"is there any", L"do we have any", L"have we got any", L"do you see any",
                                        L"can you see any", L"anyone see any", L"anybody see any", L"you see any", L"you got any",
                                        L"got any", L"any sign of", L"how many", L"any"};
        static const struct
        {
            const wchar_t *W, *Canon;
        } kCls[] = {{L"heavies", L"heavy"},   {L"heavy", L"heavy"},     {L"heavy mechs", L"heavy"},   {L"assaults", L"assault"},
                    {L"assault", L"assault"}, {L"assault mechs", L"assault"}, {L"mediums", L"medium"}, {L"medium mechs", L"medium"},
                    {L"lights", L"light"},    {L"light mechs", L"light"}, {L"turrets", L"turret"},    {L"turret", L"turret"},
                    {L"tanks", L"vehicle"},   {L"vehicles", L"vehicle"},  {L"armor", L"vehicle"},     {L"armour", L"vehicle"},
                    {L"mechs", L"mech"},      {L"big ones", L"heavy"},
                    // 2026-10-17 (user): battle armour - "any elementals?", "how many battle armor?"
                    {L"elementals", L"battlearmor"}, {L"elemental", L"battlearmor"}, {L"battle armor", L"battlearmor"},
                    {L"battle armour", L"battlearmor"}, {L"battle armor suits", L"battlearmor"}, {L"battle armour suits", L"battlearmor"}};
        for (auto *pf : kPre)
        {
            const std::vector<std::wstring> pw = SplitWords(pf);
            if (pw.size() >= w.size() || !std::equal(pw.begin(), pw.end(), w.begin()))
                continue;
            std::vector<std::wstring> r(w.begin() + (long)pw.size(), w.end());
            while (r.size() > 1 && (r.back() == L"there" || r.back() == L"left" || r.back() == L"around" || r.back() == L"nearby" ||
                                    r.back() == L"close" || r.back() == L"out" || r.back() == L"in" || r.back() == L"sight" ||
                                    r.back() == L"remaining" || r.back() == L"still"))
                r.pop_back();
            if (r.size() >= 2 && r.back() == L"out")
                r.pop_back();
            const std::wstring rt = JoinWords(r);
            for (auto &c : kCls)
                if (rt == c.W)
                {
                    name = c.Canon;
                    return Q_Class;
                }
            break; // the longest prefix that fits decides
        }
    }
    // CONTACT REPORT (2026-10-07j): "anyone have eyes?", "anything on scopes?", "anyone see anything?"
    if (in({L"anyone have eyes", L"anybody have eyes", L"anyone got eyes", L"anyone have visual", L"anybody have visual",
            L"anyone got visual", L"does anyone have eyes", L"does anyone have visual", L"anyone see anything",
            L"anybody see anything", L"can anyone see anything", L"does anyone see anything", L"eyes on anything",
            L"anyone eyes on anything", L"any eyes", L"eyes", L"visual", L"any visual", L"anything on scopes",
            L"anything on your scopes", L"anything on radar", L"anything on sensors", L"anything on your scope",
            L"contact report", L"what do we have", L"what have we got", L"whats on scope", L"whats on the scope",
            L"what do you have", L"anyone got anything", L"anybody got anything", L"talk to me lance"}))
        return Q_HowMany;
    // ARMOUR REPORT (2026-10-07j): damage only
    if (in({L"armour report", L"armor report", L"armour status", L"armor status", L"armour check", L"armor check",
            L"hows your armour", L"hows your armor", L"how is your armour", L"how is your armor", L"armour", L"armor"}))
        return Q_HowBad;
    if (in({L"how bad", L"how bad is it", L"how bad is the damage", L"you okay", L"you ok", L"are you okay", L"are you ok",
            L"you alright", L"are you alright", L"you good", L"are you good", L"damage report", L"how are you holding up",
            L"you holding up", L"talk to me", L"how you holding up", L"hows the damage", L"how is the damage", L"how are you",
            L"hows it going", L"how is it going", L"how you doing", L"status check", L"are you hurt", L"you hurt"}))
        return Q_HowBad;
    // MORE QUESTIONS (build 2026-10-06a)
    if (in({L"how many", L"how many contacts", L"how many left", L"how many are there", L"how many hostiles", L"how many enemies",
            L"how many of them", L"how many remaining", L"how many are left", L"how many bad guys", L"how many targets",
            L"contact count", L"head count", L"whats left", L"what is left", L"how many still up", L"how many more",
            L"any contacts", L"any contact", L"contacts", L"got any contacts", L"you got any contacts", L"do you have any contacts",
            L"anything out there", L"any hostiles", L"any enemies", L"any targets", L"see anything", L"you see anything",
            L"do you see anything", L"anything on scope", L"anything on the scope"}))
        return Q_HowMany;
    if (in({L"what are we facing", L"what are we up against", L"what are we fighting", L"any heavies", L"anything heavy",
            L"any assaults", L"whats out there", L"what is out there", L"what have we got out there", L"whats the opposition",
            L"enemy composition", L"how heavy", L"what are they fielding", L"what do they have", L"what have they got",
            L"what are we dealing with out there", L"what kind of force", L"what kind of mechs", L"how heavy are they"}))
        return Q_Facing;
    if (in({L"whats closest", L"what is closest", L"whats nearest", L"what is nearest", L"nearest contact", L"closest contact",
            L"closest enemy", L"nearest enemy", L"nearest hostile", L"closest hostile", L"closest target", L"nearest target",
            L"nearest threat", L"closest threat", L"whats the closest", L"what is the closest", L"whats the nearest",
            L"closest one", L"nearest one", L"anything close", L"anything near", L"whats near me", L"whats close"}))
        return Q_Closest;
    if (in({L"whos shooting at you", L"who is shooting at you", L"whos hitting you", L"who is hitting you", L"whos firing at you",
            L"who is firing at you", L"whos on you", L"who is on you", L"whos shooting", L"who is shooting", L"whos hitting",
            L"who is attacking you", L"whos attacking you", L"who is firing", L"whos firing", L"who hit you", L"who shot you",
            L"who is engaging you", L"whos engaging you", L"whos shooting you", L"who is shooting you", L"what hit you",
            L"what is hitting you", L"whats hitting you", L"whats shooting at you", L"what is shooting at you"}))
        return Q_WhoHits;
    if (in({L"whats your target", L"what is your target", L"what are you shooting at", L"what are you shooting",
            L"what are you targeting", L"what are you engaging", L"who are you shooting", L"who are you shooting at",
            L"what are you firing at", L"who are you engaging", L"who are you targeting", L"what are you on", L"who are you on",
            L"what are you attacking", L"who are you attacking", L"your target", L"what are your targets",
            L"whats everyone shooting at", L"what is everyone shooting at"}))
        return Q_Target;
    if (in({L"hows ammo", L"how is ammo", L"hows ammo looking", L"anyone low on ammo", L"anybody low on ammo", L"whos low on ammo",
            L"who is low on ammo", L"low on ammo", L"any ammo", L"hows your ammo", L"how is your ammo", L"ammo check", L"ammo status", L"ammo", L"how much ammo", L"ammo count",
            L"you got ammo", L"how are you on ammo", L"ammo report", L"how much ammo do you have", L"ammo state", L"ammo levels",
            L"how is everyone on ammo", L"hows everyone on ammo", L"how are we on ammo", L"hows the ammo", L"how is the ammo",
            L"you need ammo", L"are you low on ammo", L"you low on ammo", L"how are you for ammo"}))
        return Q_Ammo;
    if (in({L"hows your heat", L"how is your heat", L"heat check", L"heat status", L"heat", L"how hot are you", L"are you running hot",
            L"you running hot", L"heat level", L"whats your heat", L"what is your heat", L"temperature", L"how hot", L"heat report",
            L"hows the heat", L"how is the heat", L"you overheating", L"are you overheating", L"temps", L"heat levels",
            L"hows heat", L"how is heat", L"whats heat", L"what is heat", L"hows heat looking", L"how is heat looking"})) // "your" / "the" are filler (2026-10-11)
        return Q_Heat;
    if (in({L"anyone hurt", L"is anyone hurt", L"anybody hurt", L"is anybody hurt", L"whos hurt", L"who is hurt", L"whos in worst shape",
            L"whos in the worst shape", L"who is in the worst shape", L"who is in worst shape", L"who needs help", L"everyone okay",
            L"is everyone okay", L"everyone ok", L"is everyone ok", L"everyone alright", L"is everyone alright", L"everybody okay",
            L"is everybody okay", L"whos hurting", L"who is hurting", L"casualties", L"any casualties", L"hows the lance",
            L"how is the lance", L"how are we doing", L"hows everyone", L"how is everyone", L"whos damaged", L"who is damaged",
            L"whos worst off", L"who is worst off", L"lance condition", L"everyone in one piece", L"is everyone in one piece"}))
        return Q_Hurt;
    // RADIO CHECK (2026-10-13, user): its own question - "loud and clear", not a damage report
    if (in({L"radio check", L"comms check", L"comm check", L"coms check", L"com check", L"radio check lance", L"lance radio check",
            L"comms check lance", L"lance comms check", L"you copy", L"how do you copy", L"how do you read", L"how do you read me",
            L"do you read me", L"do you copy", L"anyone copy", L"does anyone copy", L"signal check", L"mic check", L"testing testing",
            L"check check", L"radio test", L"comms test"}))
        return Q_RadioCheck;
    if (in({L"sound off", L"check in", L"roll call", L"report in", L"all units check in", L"everyone check in", L"everyone report in",
            L"sound off lance", L"lance sound off", L"lance check in", L"call in", L"give me a check in",
            L"everyone sound off", L"all units report in"}))
        return Q_SoundOff;
    if (in({L"any air", L"any air contacts", L"any vtols", L"any vtol", L"any choppers", L"any helicopters", L"any helos",
            L"air contacts", L"anything in the air", L"any aircraft", L"air check", L"any birds", L"any flyers",
            L"anything flying", L"air status", L"any air threats", L"any air contact"}))
        return Q_Air;
    if (in({L"what was that", L"say again", L"say again last", L"repeat", L"repeat that", L"come again", L"what did you say",
            L"didnt copy", L"i didnt copy", L"say that again", L"repeat last", L"one more time", L"what",
            L"repeat your last", L"say again your last", L"didnt catch that", L"i didnt catch that", L"what did you just say",
            L"repeat last transmission", L"say again all after", L"say again over",
            // ("again" is cut off the end first, as filler)
            L"say", L"say that", L"say it", L"come", L"say all after", L"say your last", L"say last", L"say it all"}))
        return Q_Repeat;
    if (in({L"target status", L"target condition", L"status of my target", L"status on my target", L"how is my target", L"hows my target",
            L"my targets condition", L"how is the target", L"hows the target", L"condition of my target", L"how is my target doing",
            L"hows my target doing", L"is my target hurt", L"how bad is my target"}))
    {
        name = L"my target";
        return Q_UnitHow;
    }
    if (in({L"wheres my target", L"where is my target", L"where is our target", L"wheres our target", L"where did my target go",
            L"bearing to my target", L"range to my target", L"how far is my target", L"how far away is my target"}))
    {
        name = L"my target";
        return Q_WhereNamed;
    }
    if (in({L"where", L"where at", L"wheres that", L"where is that", L"where is it", L"wheres it", L"where is he", L"wheres he",
            L"where are they", L"wheres they", L"from where", L"where from", L"say position", L"position", L"bearing",
            L"what bearing", L"which way", L"how far", L"how far out", L"how close", L"distance", L"where exactly", L"wear",
            L"were", L"ware", L"whereabouts", L"which direction", L"what direction", L"where is the contact", L"wheres the contact",
            L"where is the target", L"wheres the target", L"where is it at", L"wheres it at", L"where are they at",
            L"where is he at", L"wheres he at", L"which side", L"what side", L"where did it go", L"where did he go",
            L"where is it coming from", L"wheres it coming from", L"where from exactly", L"where is that coming from",
            L"range", L"what range", L"whats the range", L"how far away", L"direction", L"give me a bearing", L"bearing and range",
            L"range and bearing", L"whats the bearing", L"what is the bearing", L"how far away is it", L"how far is it"}))
        return Q_Where;
    if (FiringQuestion(w, name)) // before "can you see <name>" takes "can you see who's firing"
        return Q_WhoFiring;
    // "<question> [the] <name>" about one unit (build 2026-10-06a): "can you see the Atlas", "is it down",
    // "status on the Bane", "who's closest to it". No name (it / him / that one) = what was last reported.
    auto tailName = [&](std::initializer_list<const wchar_t *> prefixes, std::initializer_list<const wchar_t *> ends,
                        std::wstring &out) -> bool {
        for (auto *pf : prefixes)
        {
            const std::vector<std::wstring> pw = SplitWords(pf);
            if (pw.size() > w.size() || !std::equal(pw.begin(), pw.end(), w.begin()))
                continue;
            std::vector<std::wstring> r(w.begin() + (long)pw.size(), w.end());
            while (!r.empty() && (r.back() == L"now" || r.back() == L"yet" || r.back() == L"still"))
                r.pop_back();
            bool endOk = ends.size() == 0;
            for (auto *e : ends)
                if (!endOk && !r.empty() && r.back() == e)
                {
                    r.pop_back();
                    endOk = true;
                }
            if (!endOk)
                continue;
            if (r.size() >= 2 && r[r.size() - 2] == L"holding" && r.back() == L"up")
                r.resize(r.size() - 2);
            while (!r.empty() && (r.back() == L"doing" || r.back() == L"looking" || r.back() == L"at" || r.back() == L"go" ||
                                  r.back() == L"gone" || r.back() == L"went"))
                r.pop_back();
            while (!r.empty() && (r.front() == L"the" || r.front() == L"that" || r.front() == L"this" || r.front() == L"a" ||
                                  r.front() == L"an" || r.front() == L"our" || r.front() == L"those"))
                r.erase(r.begin());
            if (r.size() > 5) // 2026-10-10: was 3 ("status on the Mad Cat Mark II")
                continue;
            out = JoinWords(r);
            if (out == L"it" || out == L"him" || out == L"he" || out == L"them" || out == L"they" || out == L"one" ||
                out == L"contact" || out == L"target" || out == L"enemy" || out == L"hostile" || out == L"bogey" || out == L"thing")
                out.clear();
            return true;
        }
        return false;
    };
    // "which way is the Shadow Hawk facing?" (2026-10-11)
    if (tailName({L"which way is", L"which way are", L"which direction is", L"what direction is", L"where is", L"wheres",
                  L"what way is", L"whats", L"what is"},
                 {L"facing", L"pointing", L"looking", L"pointed", L"turned", L"heading"}, name) && !name.empty())
        return Q_Heading;
    if (tailName({L"can anyone see", L"can anybody see", L"does anyone see", L"does anybody see", L"anyone see", L"anybody see",
                  L"can any of you see", L"does anyone have eyes on", L"anyone have eyes on", L"anyone got eyes on",
                  L"does anyone have visual on", L"anyone have visual on", L"anyone got visual on", L"who can see", L"who sees",
                  L"who has eyes on", L"whos got eyes on", L"who has visual on", L"whos got visual on"},
                 {}, name)) // 2026-10-11: to the lance - whoever sees it (or last saw it) answers
        return Q_CanSee;
    if (tailName({L"can you see", L"do you see", L"can you still see", L"do you still see", L"do you have eyes on", L"you have eyes on",
                  L"have you got eyes on", L"you got eyes on", L"you still have eyes on", L"do you still have eyes on",
                  L"do you have visual on", L"you have visual on", L"have you got visual on", L"do you have a visual on",
                  L"you got visual on", L"do you have visual", L"you have visual", L"have you got visual", L"you got visual",
                  L"do you have eyes", L"you have eyes", L"is it visible"},
                 {}, name))
        return Q_CanSee;
    if (tailName({L"is", L"its", L"was", L"has", L"are", L"did"}, {L"down", L"downed", L"downs", L"downer", L"dead", L"destroyed", L"out", L"finished", L"done", L"scrap", L"toast"}, name) ||
        tailName({L"did we get", L"did we kill", L"did you get", L"did you kill", L"did we drop", L"did you drop", L"did we take out",
                  L"did you take out", L"did we destroy", L"did you destroy", L"confirm kill on", L"confirm the kill on"},
                 {}, name))
        return Q_IsDown;
    if (in({L"kill confirmed", L"confirm kill", L"confirm the kill", L"is that a kill", L"did it die"}))
        return Q_IsDown;
    if (tailName({L"whos closest to", L"who is closest to", L"whos nearest to", L"who is nearest to", L"whos nearest", L"who is nearest",
                  L"whos closest", L"who is closest", L"whos in range of", L"who is in range of"},
                 {}, name))
        return Q_WhoClosest;
    if (tailName({L"status on", L"status of", L"whats the status of", L"what is the status of", L"whats the status on", L"condition of",
                  L"how bad is", L"how hurt is", L"how damaged is", L"how beat up is", L"hows", L"how is"},
                 {}, name))
    {
        if (name == L"you" || (name.compare(0, 5, L"your ") == 0 && name != L"your target"))
        {
            name.clear();
            return Q_HowBad;
        }
        if (name == L"everyone" || name == L"everybody" || name == L"lance" || name == L"we" || name == L"us")
        {
            name.clear();
            return Q_Hurt;
        }
        return Q_UnitHow;
    }
    // "where's the Atlas", "where is that Hunchback", "where did the Jenner go", "find the Catapult"
    size_t k = 0;
    if (w[0] == L"wheres" || w[0] == L"locate" || w[0] == L"find") // 2026-10-10: SHADOW removed, "find X" asks again
        k = 1;
    else if (w[0] == L"where" && w.size() > 2 && (w[1] == L"is" || w[1] == L"are" || w[1] == L"did" || w[1] == L"was"))
        k = 2;
    if (!k)
        return Q_None;
    std::vector<std::wstring> r(w.begin() + (long)k, w.end());
    while (!r.empty() && (r.front() == L"the" || r.front() == L"that" || r.front() == L"this" || r.front() == L"those" ||
                          r.front() == L"these" || r.front() == L"a" || r.front() == L"an" || r.front() == L"our"))
        r.erase(r.begin());
    while (!r.empty() && (r.back() == L"at" || r.back() == L"go" || r.back() == L"gone" || r.back() == L"now" || r.back() == L"went"))
        r.pop_back();
    if (r.empty() || r.size() > 5) // 2026-10-10: was 3 ("where's the Mad Cat Mark II")
        return Q_None;
    name = JoinWords(r);
    if (name == L"it" || name == L"he" || name == L"they" || name == L"contact" || name == L"target" || name == L"enemy" ||
        name == L"hostile" || name == L"bogey")
        return Q_Where;
    return Q_WhereNamed;
}
std::mutex                g_amx;
std::vector<SpokenAction> g_actions;

void HeardLog(const std::wstring &line)
{
    if (g_quiet)
        return;
    FILE *f = nullptr;
    if (_wfopen_s(&f, (ExeDir() + L"VoiceCommand_heard.log").c_str(), L"ab") != 0 || !f)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t ts[32];
    swprintf_s(ts, L"%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring w = ts + line + L"\r\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string u((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
    fwrite(u.c_str(), 1, u.size(), f);
    fclose(f);
}

// Whisper's words -> orders (queued for the main loop). pttHeld: the push-to-talk key was held
// (listen = both: then the callsign may be left out - the order goes to the whole lance).
std::vector<std::pair<std::vector<std::wstring>, int>> PilotNameWords(); // PILOT NAMES, below

// A heard word that is the START of a one-word callsign (2026-10-07i: Whisper wrote "Book status" for "Booker,
// status"): 4+ letters, at least half the name, and not a word of an order or a callsign itself.
bool NamePrefix(const std::wstring &heard, const std::wstring &name)
{
    if (heard.size() < 4 || heard.size() >= name.size() || heard.size() * 2 < name.size() || name.compare(0, heard.size(), heard) != 0)
        return false;
    static const wchar_t *kNot[] = {L"move", L"hold", L"form", L"fall", L"fire", L"focus", L"attack", L"stay", L"wait", L"back",
                                    L"push", L"follow", L"shadow", L"report", L"status", L"cover", L"halt", L"stop", L"head",
                                    L"engage", L"return", L"reset", L"indirect", L"weapons", L"advance", L"regroup", L"rally",
                                    L"walk", L"come", L"lance", L"lances", L"three", L"four", L"seven", L"eight", L"everyone", L"what",
                                    L"where", L"whos", L"whats", L"anyone", L"does", L"there", L"this", L"that", L"those", L"with"};
    for (auto *k : kNot)
        if (heard == k)
            return false;
    return true;
}

// ---- ORDER PARSER: orders understood by their parts, in any word order ------------------------
// Tried before the whole-phrase matcher. It looks for the PARTS of an order anywhere in what was
// said - fire discipline, heat, weapon types, status, attack, indirect fire, fall back, follow,
// hold, move with a distance and a direction - so "two, push up north about three hundred",
// "sledge get back here", "everyone go hot" and "three, run cool and fall back" all work, with
// filler words ignored. Several parts make several orders (fire settings are merged into one, so
// they do not overwrite each other). Nothing recognised -> the whole-phrase matcher decides, as
// before. The words come cleaned up by NormalizeHeard (numbers as words, jargon, synonyms).
// ATTACK WORDS (2026-10-15, user): ONE list for every attack - a named unit, a mech weight class, a vehicle class, the
// nearest enemy - so a word that works for one works for all. Longer phrases first (they are taken before their words).
// "fire" is not in it: only the weight classes and the nearest enemy take it ("fire on the move", "fire at nav Alpha").
static const wchar_t *kAtkWords[] = {L"focus fire on", L"take out", L"take down", L"knock out", L"wipe out", L"go for", L"go after",
                                     L"go get", L"deal with", L"tie up", L"focus on", L"focus fire", L"get on", L"get that",
                                     L"get this", L"lets get", L"light up", L"attack", L"kill", L"hit", L"shoot", L"destroy",
                                     L"engage", L"target", L"focus", L"take", L"drop", L"nail", L"smoke", L"clear", L"smash",
                                     L"waste", L"hunt", L"wreck", L"flatten", L"eliminate", L"neutralize", L"neutralise", L"strip",
                                     L"handle"};
// Is one of the attack words at w[i]? Its length in words, else 0.
static size_t AtkWordAt(const std::vector<std::wstring> &w, size_t i)
{
    for (auto *a : kAtkWords)
    {
        const std::vector<std::wstring> p = SplitWords(a);
        if (i + p.size() > w.size())
            continue;
        bool ok = true;
        for (size_t k = 0; k < p.size() && ok; ++k)
            ok = w[i + k] == p[k];
        if (ok)
            return p.size();
    }
    return 0;
}

bool ParseOrder(const std::vector<std::wstring> &w, std::vector<Cmd> &out, std::wstring &desc)
{
    out.clear();
    desc.clear();
    std::vector<bool> used(w.size(), false);
    // Finds a phrase (words in a row, none used yet) and marks it used.
    auto take = [&](const wchar_t *phrase) -> bool {
        const std::vector<std::wstring> p = SplitWords(phrase);
        if (p.empty() || p.size() > w.size())
            return false;
        for (size_t i = 0; i + p.size() <= w.size(); ++i)
        {
            bool ok = true;
            for (size_t k = 0; k < p.size() && ok; ++k)
                ok = !used[i + k] && w[i + k] == p[k];
            if (ok)
            {
                for (size_t k = 0; k < p.size(); ++k)
                    used[i + k] = true;
                return true;
            }
        }
        return false;
    };
    auto takeAny = [&](std::initializer_list<const wchar_t *> ps) -> bool {
        for (auto *p : ps)
            if (take(p))
                return true;
        return false;
    };
    auto add = [&](const wchar_t *what) { desc += (desc.empty() ? L"" : L" + ") + std::wstring(what); };
    // "hold here and don't push" (2026-10-10, user): a NEGATED move is no move
    while (takeAny({L"dont push", L"do not push", L"dont advance", L"do not advance", L"dont move up", L"dont go forward",
                    L"dont chase", L"dont follow them", L"no pushing"}))
    {
    }
    // NEGATION (2026-10-14, user audit): "do not engage" became an attack, "do not fire" did nothing. "dont / do not /
    // never" + a FIRE verb = hold fire; + a MOVE verb = that move is not made. The negation and its verb are used up, so
    // no later block reads the verb ("attack", "advance", ...) as an order.
    bool negFire = false;
    {
        static const wchar_t *kFire[] = {L"fire", L"shoot", L"engage", L"attack", L"open", L"hit", L"kill", L"destroy", L"target",
                                         L"strike", L"alpha", L"focus", L"waste", L"take"};
        static const wchar_t *kMove[] = {L"move", L"advance", L"push", L"go", L"follow", L"chase", L"charge", L"rush", L"run",
                                         L"pursue", L"get", L"head", L"leave", L"retreat", L"fall", L"break", L"pull", L"bug",
                                         L"climb", L"support", L"help", L"assist", L"flank", L"pincer", L"come"};
        for (size_t k = 0; k < w.size(); ++k)
        {
            if (used[k])
                continue;
            size_t v = w.size();
            if (w[k] == L"dont" || w[k] == L"never")
                v = k + 1;
            else if (w[k] == L"do" && k + 1 < w.size() && w[k + 1] == L"not")
                v = k + 2;
            else if (w[k] == L"no" && k + 1 < w.size() && (w[k + 1] == L"firing" || w[k + 1] == L"shooting" || w[k + 1] == L"engaging"))
            {
                used[k] = used[k + 1] = true;
                negFire = true;
                continue;
            }
            if (v >= w.size() || used[v] || w[v] == L"use") // "dont use lasers" = hold lasers (fire settings), not a negation
                continue;
            const bool fire = std::any_of(std::begin(kFire), std::end(kFire), [&](const wchar_t *x) { return w[v] == x; });
            const bool mv = std::any_of(std::begin(kMove), std::end(kMove), [&](const wchar_t *x) { return w[v] == x; });
            if (!fire && !mv)
                continue;
            for (size_t j = k; j <= v; ++j)
                used[j] = true;
            negFire |= fire;
        }
    }
    // NEGATION SCOPE (2026-10-14, user): blocks that need no verb (a nav letter = a move, "that hill" = the hill offer,
    // "his legs" = a called shot) skip what a "dont / do not / never" covers: from it to the next order verb or
    // "and / then / but / instead". "Dont go to nav Alpha, go to nav Bravo" -> Bravo only.
    std::vector<bool> negated(w.size(), false);
    {
        static const wchar_t *kEnd[] = {L"and", L"then", L"but", L"instead", L"move", L"go", L"hold", L"advance", L"attack", L"kill",
                                        L"take", L"engage", L"push", L"run", L"support", L"assist", L"help", L"follow", L"focus",
                                        L"hit", L"destroy", L"get", L"head", L"fall", L"form", L"stay", L"wait", L"climb", L"find",
                                        L"break", L"pull", L"bug", L"regroup", L"rally", L"retreat", L"cover", L"flank", L"pincer", L"charge",
                                        L"weapons", L"open", L"fire", L"shoot"};
        for (size_t k = 0; k < w.size(); ++k)
        {
            size_t st = w.size();
            if (w[k] == L"dont" || w[k] == L"never")
                st = k + 1;
            else if (w[k] == L"do" && k + 1 < w.size() && w[k + 1] == L"not")
                st = k + 2;
            if (st >= w.size() || w[st] == L"use") // "dont use lasers": the weapon setting's own phrase (2026-10-14 audit)
                continue;
            negated[k] = true;
            if (st == k + 2)
                negated[k + 1] = true;
            negated[st] = true; // its own verb ("go" in "dont go to nav Alpha")
            for (size_t j = st + 1; j < w.size(); ++j)
            {
                if (std::any_of(std::begin(kEnd), std::end(kEnd), [&](const wchar_t *x) { return w[j] == x; }))
                    break;
                negated[j] = true;
            }
        }
        // and nothing else reads them either ("dont move 200 metres north" was still a move - distance / direction need no verb)
        for (size_t j = 0; j < w.size(); ++j)
            if (negated[j])
                used[j] = true;
    }

    // ---- fire settings (merged into one order). 2026-10-14 audit (user): read FIRST, before support / hill / target
    // classes / called shots / nav (which end the sentence) - "Two, legs on the Atlas, weapons free" keeps both, and
    // "go to nav Foxtrot and hold fire" is no longer a hold at the nav.
    int disc = -1, heat = -1, maskOp = 0, mask = 0;
    bool reset = false;
    // "engage at will" (2026-10-10, user): weapons free AND attack the closest contact
    const bool atWill = takeAny({L"engage at will", L"attack at will", L"targets of opportunity", L"pick your targets",
                                L"pick target", L"pick a target"}); // "pick your targets" arrives as "pick target" (2026-10-11)
    if (atWill || takeAny({L"weapons free", L"open fire", L"free fire", L"weapons hot", L"fire at will"}))
        disc = 0;
    else if (takeAny({L"return fire"}))
        disc = 1;
    else if (takeAny({L"hold fire", L"stop firing", L"stop shooting", L"weapons hold", L"weapons tight", L"dont shoot",
                      L"do not shoot", L"cease firing", L"stop attacking", L"stop attack", L"stop your attack", L"stop engaging",
                      L"quit firing", L"quit shooting"})) // "check fire": a remark (2026-10-10); "stop attacking" (2026-10-14 audit) was hold position
        disc = 2;
    if (negFire && disc < 0) // "do not engage", "don't fire", "never attack" (2026-10-14 audit)
        disc = 2;
    // "alpha" alone is not one (2026-10-07i: "where is Nav Alpha" became an alpha strike) - only "Two, alpha" said on its own
    if (takeAny({L"alpha strike", L"alpha strikes", L"everything you got", L"everything youve got"}))
        heat = 3;
    else if (takeAny({L"run hot", L"go hot", L"run it hot", L"running hot", L"heat up"}))
        heat = 2;
    else if (takeAny({L"run cool", L"cool down", L"keep cool", L"keep it cool", L"stay cool", L"watch heat", L"ease off",
                      L"running cool", L"cool"}))
        heat = 1;
    else if (takeAny({L"normal heat", L"heat normal"}))
        heat = 0;
    if (takeAny({L"reset fire orders", L"standard fire orders", L"reset", L"default"}))
        reset = true;
    // weapon types
    static const struct
    {
        const wchar_t *W;
        int            M;
    } kCatWords[] = {{L"long range missiles", kFcLockMissile}, {L"short range missiles", kFcDumbMissile},
                     {L"lock on missiles", kFcLockMissile},    {L"dumbfire missiles", kFcDumbMissile},
                     {L"machine guns", kFcMGFlamer},           {L"particle cannons", kFcPPC},
                     {L"p p cs", kFcPPC},                       {L"p p c", kFcPPC},
                     {L"missiles", kFcLockMissile | kFcDumbMissile}, {L"lasers", kFcLaser},
                     {L"ballistics", kFcBallistic},             {L"autocannons", kFcBallistic},
                     {L"cannons", kFcBallistic},                {L"gauss", kFcBallistic},
                     {L"flamers", kFcMGFlamer},                 {L"tag", kFcTAG},
                     {L"narc", kFcNARC}};
    if (takeAny({L"all weapons", L"all guns", L"everything"}))
        maskOp = 3;
    for (auto &c : kCatWords)
        while (take(c.W))
            mask |= c.M;
    // 2026-10-10 (user): a weapon type is a fire setting only with "only" ("missiles only", "just lasers", "switch to
    // lasers") or a hold word ("hold lasers") - "incoming missiles!" is a warning, not an order
    // 2026-10-14 audit (user): "only" wins over a hold word - "lasers only, hold position" is lasers only + hold position
    if (mask && maskOp != 3)
        maskOp = takeAny({L"only", L"just", L"switch to"})
                     ? 1
                     : (takeAny({L"hold", L"stop", L"no", L"dont use", L"do not use", L"without", L"drop", L"cut", L"cease", L"stop using"}) ? 2 : 0);
    if (maskOp == 0)
        mask = 0;
    if (reset)
    {
        Cmd c{};
        c.Type = C_Reset;
        out.push_back(c);
        add(L"reset fire orders");
    }
    else if (disc >= 0 || heat >= 0 || maskOp)
    {
        Cmd c{};
        c.Type = C_FireSet;
        c.A = disc;
        c.B = heat;
        c.C = maskOp;
        c.D = mask;
        out.push_back(c);
        static const wchar_t *kD[] = {L"weapons free", L"return fire", L"hold fire"};
        static const wchar_t *kH[] = {L"normal heat", L"run cool", L"run hot", L"alpha strike"};
        if (disc >= 0)
            add(kD[disc]);
        if (heat >= 0)
            add(kH[heat]);
        if (maskOp == 3)
            add(L"all weapons");
        else if (maskOp)
            add(maskOp == 1 ? L"weapons only" : L"hold weapons");
    }

    // ---- status
    if (takeAny({L"status report", L"whats status", L"status", L"sitrep", L"report in", L"report", L"how are you holding up",
                 L"holding up", L"condition"}))
    {
        Cmd c{};
        c.Type = C_Status;
        out.push_back(c);
        add(L"status");
    }
    // ---- FLANK / PINCER (2026-10-20, user; lancecontrol-75 TCO_Flank). "flank the Atlas", "flank left on that Atlas" (YOUR
    // left - the line you -> it), "flank that Atlas on his left" / "flank the Atlas's left" (ITS own left), "get behind it" is
    // not this - only "flank ... behind / from the rear". "stay 300 out" sets the stand-off (else the most damage in range).
    // "Two, three, set up a pincer on the Atlas": the lancemates go left / right (/ rear) of it as YOU see it; "on his flanks"
    // = its own sides. No name: your lock. Fire settings in the same sentence were read above and stay.
    {
        size_t fAt = w.size();
        bool pincer = false;
        for (size_t k = 0; k < w.size(); ++k)
        {
            if (used[k] || negated[k])
                continue;
            const std::wstring &x = w[k];
            if (x == L"pincer" || x == L"pincers" || x == L"pincher" || x == L"pinchers" || x == L"pinch")
            {
                fAt = k;
                pincer = true;
                break;
            }
            // only the order itself: "it's flanking us" / "on our left flank" are reports, not orders
            if (fAt == w.size() && (x == L"flank" || x == L"outflank") &&
                !(k > 0 && (w[k - 1] == L"our" || w[k - 1] == L"my" || w[k - 1] == L"your" || w[k - 1] == L"left" || w[k - 1] == L"right" ||
                            w[k - 1] == L"us" || w[k - 1] == L"the")))
                fAt = k;
        }
        if (fAt < w.size())
        {
            used[fAt] = true;
            auto isPoss = [&](size_t k) {
                return k < w.size() && (w[k] == L"his" || w[k] == L"her" || w[k] == L"its" || w[k] == L"their");
            };
            // the unit named (Whisper's "Atlas's" -> "atlass")
            std::vector<std::wstring> known;
            {
                std::lock_guard<std::mutex> lk(g_namesMx);
                known = g_knownNames;
            }
            std::wstring found;
            size_t nAt = 0, nLen = 0;
            bool named = FindNameInWords(w, &used, known, found, nAt, nLen);
            if (!named)
            {
                std::vector<std::wstring> w2 = w;
                for (auto &x : w2)
                    if (x.size() > 3 && x.back() == L's')
                        x.pop_back();
                named = FindNameInWords(w2, &used, known, found, nAt, nLen);
            }
            if (named)
                for (size_t k = 0; k < nLen; ++k)
                    used[nAt + k] = true;
            // the side
            int side = 0, sides = 0; // sides (pincer): 1 = its own flanks
            for (size_t k = 0; k < w.size() && !side; ++k)
            {
                if (used[k])
                    continue;
                const std::wstring &x = w[k];
                const bool its = (k > 0 && isPoss(k - 1)) || (named && k == nAt + nLen);
                if (x == L"left" || x == L"right")
                {
                    side = (its ? 3 : 1) + (x == L"right" ? 1 : 0);
                    used[k] = true;
                    if (k > 0 && isPoss(k - 1))
                        used[k - 1] = true;
                }
                // "his six" only - a bare "six" is a number ("stay six hundred out" was read as behind it + 100 m, 2026-10-20 test)
                else if (x == L"behind" || x == L"rear" || (x == L"six" && k > 0 && isPoss(k - 1)) || x == L"backside" ||
                         (x == L"back" && k > 0 && (isPoss(k - 1) || w[k - 1] == L"the" || w[k - 1] == L"from" || w[k - 1] == L"around")))
                {
                    side = 5;
                    used[k] = true;
                    if (k > 0 && isPoss(k - 1))
                        used[k - 1] = true;
                }
                else if (pincer && (x == L"flanks" || x == L"flank" || x == L"sides" || x == L"side") &&
                         (its || (k > 0 && w[k - 1] == L"both" && k > 1 && isPoss(k - 2))))
                {
                    sides = 1;
                    used[k] = true;
                    if (k > 0 && isPoss(k - 1))
                        used[k - 1] = true;
                }
            }
            // the stand-off: "stay 300 out", "at three hundred metres", "keep 250 off it"
            int dist = 0;
            for (size_t i = 0; i < w.size() && !dist; ++i)
            {
                if (used[i])
                    continue;
                static const wchar_t *kUnit[] = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine",
                                                 L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen",
                                                 L"seventeen", L"eighteen", L"nineteen"};
                static const wchar_t *kTen[] = {L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety"};
                int v = 0, cur = 0;
                size_t j = i;
                if (iswdigit(w[i][0]))
                {
                    for (wchar_t ch : w[i])
                        if (iswdigit(ch) && v < 100000)
                            v = v * 10 + (ch - L'0');
                    j = i + 1;
                }
                else
                {
                    for (; j < w.size() && !used[j]; ++j)
                    {
                        int u = -1, t = -1;
                        for (int k = 0; k < 20; ++k)
                            if (w[j] == kUnit[k])
                                u = k;
                        for (int k = 0; k < 8; ++k)
                            if (w[j] == kTen[k])
                                t = 20 + 10 * k;
                        if (u >= 0)
                            cur += u;
                        else if (t >= 0)
                            cur += t;
                        else if (w[j] == L"hundred")
                            cur = (cur ? cur : 1) * 100;
                        else if (w[j] == L"a" && j + 1 < w.size() && w[j + 1] == L"hundred")
                            continue;
                        else
                            break;
                    }
                    v = cur;
                }
                if (v >= 50 && v <= 1200)
                {
                    dist = v;
                    for (size_t k = i; k < j; ++k)
                        used[k] = true;
                    if (j < w.size() && (w[j] == L"metres" || w[j] == L"meters" || w[j] == L"m"))
                        used[j] = true;
                }
            }
            takeAny({L"set up a", L"set up", L"stay", L"keep", L"out", L"off", L"away", L"back", L"on", L"at", L"the", L"that",
                     L"this", L"him", L"it", L"them", L"my target", L"target", L"attack", L"around", L"go", L"and"});
            Cmd c{};
            c.Type = pincer ? C_Pincer : C_Flank;
            c.A = pincer ? sides : side;
            c.B = dist;
            out.push_back(c);
            g_parsedName = named ? found : std::wstring();
            static const wchar_t *kSide[] = {L"the nearer side", L"your left", L"your right", L"its left", L"its right", L"behind it"};
            std::wstring d = pincer ? std::wstring(L"pincer ") + (named ? L"the " + found : std::wstring(L"your target")) +
                                          (sides ? L" (its flanks)" : L" (your left / right)")
                                    : std::wstring(L"flank ") + (named ? L"the " + found : std::wstring(L"your target")) + L", " + kSide[side];
            if (dist)
                d += L", stand-off " + std::to_wstring(dist) + L" m";
            add(d.c_str());
            return true;
        }
    }

    // ---- SUPPORT (2026-10-13, user): "Two, go give Four some support" / "help Sledge out" / "back up three" - a
    // support word plus ANOTHER lancemate (number or pilot name) somewhere after the addressee.
    {
        size_t sv = w.size();
        for (size_t i = 0; i < w.size() && sv == w.size(); ++i)
            if (!used[i] && (w[i] == L"support" || w[i] == L"assist" || w[i] == L"help" ||
                             (w[i] == L"follow" && !(i + 1 < w.size() && (w[i + 1] == L"me" || w[i + 1] == L"us"))) || // 2026-10-14 (user)
                             (w[i] == L"back" && i + 1 < w.size() && w[i + 1] == L"up") ||
                             (w[i] == L"back" && i + 2 < w.size() && w[i + 2] == L"up")))
                sv = i;
        if (sv < w.size())
        {
            int who = -1;
            size_t whoAt = w.size();
            const auto names = PilotNameWords();
            for (size_t i = 0; i < w.size() && who < 0; ++i)
            {
                if (used[i] || i == sv)
                    continue;
                for (auto &nm : names)
                {
                    bool ok = i + nm.first.size() <= w.size();
                    for (size_t k = 0; ok && k < nm.first.size(); ++k)
                        ok = w[i + k] == nm.first[k] || (nm.first[k].size() >= 4 && WordSim(w[i + k], nm.first[k]) >= 0.75);
                    if (ok)
                    {
                        who = nm.second;
                        whoAt = i;
                        break;
                    }
                }
                if (who < 0)
                {
                    const int n = WhoWord(w[i]);
                    // a number that is a distance ("support four hundred") or a bearing ("back up to my two o'clock") is not
                    // a lancemate (2026-10-14 audit)
                    static const wchar_t *kAfter[] = {L"hundred", L"thousand", L"metres", L"meters", L"metre", L"meter", L"klicks",
                                                      L"klick", L"k", L"kilometres", L"kilometers", L"oclock", L"o", L"clock",
                                                      L"degrees", L"of", L"enemies", L"hostiles", L"mechs", L"contacts"};
                    const bool notMate = (i + 1 < w.size() && std::any_of(std::begin(kAfter), std::end(kAfter),
                                                                          [&](const wchar_t *x) { return w[i + 1] == x; })) ||
                                         (i > 0 && (w[i - 1] == L"my" || w[i - 1] == L"your" || w[i - 1] == L"yours" ||
                                                    w[i - 1] == L"heading" || w[i - 1] == L"bearing"));
                    // "to" / "too" / "for" / "fore" / "won" sound like Two / Four / One - a lancemate only at the end or
                    // before "out" ("back up too", "help for out"), never "back up to my two o'clock" (2026-10-14 audit)
                    const bool homo = w[i] == L"to" || w[i] == L"too" || w[i] == L"tu" || w[i] == L"for" || w[i] == L"fore" ||
                                      w[i] == L"won";
                    const bool homoOk = !homo || i + 1 == w.size() || w[i + 1] == L"out";
                    if (n >= 1 && !notMate && homoOk)
                    {
                        who = n;
                        whoAt = i;
                    }
                }
            }
            // "help" / "follow" only with the lancemate right after ("help Three out", "follow Sledge") - "Lance, help,
            // two of them are on me" is no support order (2026-10-14 audit)
            if (who >= 1 && (w[sv] == L"help" || w[sv] == L"follow") && !(whoAt > sv && whoAt <= sv + 3))
                who = -1;
            if (who >= 1)
            {
                Cmd c{};
                c.Type = C_Support;
                c.A = who;
                out.push_back(c);
                add((L"support " + std::wstring(kNum[who])).c_str());
                return true;
            }
        }
    }

    // ---- HIGH GROUND (2026-10-14, user): "get up on that hill" / "take the high ground" / "find some high ground" /
    // "get on that ridge" - the pilot marks 3-4 hilltops as nav points and asks which. Not when the hill is only where
    // something is ("the Atlas on that hill", "behind that ridge", "flank around the hill").
    {
        bool hill = false, other = false;
        static const wchar_t *kTo[] = {L"on", L"up", L"onto", L"take", L"to", L"top", L"find", L"climb", L"get", L"grab", L"some", L"a"};
        static const wchar_t *kNot[] = {L"attack", L"kill", L"shoot", L"hit", L"fire", L"engage", L"destroy", L"target", L"enemy",
                                        L"enemies", L"contact", L"contacts", L"behind", L"around", L"over", L"past", L"flank",
                                        L"beyond", L"side", L"down", L"off", L"from"};
        for (size_t k = 0; k < w.size(); ++k)
        {
            const std::wstring &x = w[k];
            if (negated[k]) // "dont get on that hill" (2026-10-14)
                continue;
            if (x == L"high" && k + 1 < w.size() && (w[k + 1] == L"ground" || w[k + 1] == L"point"))
                hill = true;
            if (x == L"hill" || x == L"hills" || x == L"hilltop" || x == L"ridge" || x == L"ridgeline" || x == L"hillside")
                for (size_t b = k > 3 ? k - 3 : 0; b < k; ++b)
                    hill |= std::any_of(std::begin(kTo), std::end(kTo), [&](const wchar_t *t) { return w[b] == t; });
            other |= !used[k] && std::any_of(std::begin(kNot), std::end(kNot), [&](const wchar_t *t) { return x == t; });
        }
        if (hill && !other)
        {
            Cmd c{};
            c.Type = C_Hill;
            // 2026-10-14 (user): "take the high ground near nav Foxtrot" - hilltops within 400 m of that nav point instead
            for (size_t k = 0; k < w.size() && !c.A; ++k)
            {
                size_t len = 0;
                const int nl = used[k] ? -1 : NavWordsAt(w, k, len);
                if (nl >= 0)
                {
                    c.A = nl + 1;
                    for (size_t m = 0; m < len; ++m)
                        used[k + m] = true;
                }
            }
            out.push_back(c);
            add(c.A ? (L"high ground near " + NavSaid(c.A - 1) + L" - which hill?").c_str() : L"high ground - which hill?");
            return true;
        }
    }

    // ---- TARGET CLASSES (2026-10-14, user): "take out the turrets", "kill the tanks first", "go for the VTOLs" - an
    // attack word before a class word. Until none of that kind are left (SweepTick).
    {
        int kind = 0;
        bool single = false; // 2026-10-14 audit (user): "that tank" = one attack, "the tanks" = the sweep
        size_t at = w.size();
        for (size_t k = 0; k < w.size() && !kind; ++k)
        {
            if (used[k])
                continue;
            const std::wstring &x = w[k];
            // 2026-10-17 (user): BATTLE ARMOUR - "the elementals", "that elemental", "the battle armor" (checked before "armor",
            // which alone means the tanks)
            if (x == L"elemental" || x == L"elementals" || x == L"battlearmor" || x == L"battlearmour" ||
                (x == L"battle" && k + 1 < w.size() &&
                 (w[k + 1] == L"armor" || w[k + 1] == L"armour" || w[k + 1] == L"armors" || w[k + 1] == L"armours")))
                kind = 8;
            else if (x == L"turret" || x == L"turrets")
                kind = 1;
            else if (x == L"tank" || x == L"tanks" || x == L"vehicle" || x == L"vehicles" || x == L"armor" || x == L"armour" ||
                     x == L"apc" || x == L"apcs")
                kind = 2;
            else if (x == L"vtol" || x == L"vtols" || x == L"helicopter" || x == L"helicopters" || x == L"chopper" ||
                     x == L"choppers" || x == L"helo" || x == L"helos" || x == L"gunship" || x == L"gunships")
                kind = 3;
            if (kind)
            {
                at = k;
                single = x != L"armor" && x != L"armour" && x.back() != L's';
                if (kind == 8 && x != L"elemental") // "battle armor" is plural like "armor"; only "that / this ..." is one
                    single = false;
                // 2026-10-14 (user, round trip): Whisper drops the s of "VTOLs" - "the VTOL" is the sweep too; only
                // "that / this / a / one VTOL" is a single attack (the clean-up drops "the", so test the other way round)
                if (single && kind == 3 &&
                    !(k > 0 && (w[k - 1] == L"that" || w[k - 1] == L"this" || w[k - 1] == L"a" || w[k - 1] == L"an" || w[k - 1] == L"one")))
                    single = false;
                // 2026-10-15 (user, overnight round trip C1): "that / this" + a class word is one target, even with the
                // s Whisper adds ("that turrets" = that turret)
                if (!single && k > 0 && (w[k - 1] == L"that" || w[k - 1] == L"this"))
                    single = true;
            }
        }
        // "attack my target, it's a turret" / "kill the Bulldog tank": your lock / that unit, not a class (2026-10-14 audit)
        if (kind)
        {
            for (size_t k = 0; k + 1 < w.size(); ++k)
                if (w[k] == L"my" && w[k + 1] == L"target")
                    kind = 0;
            std::vector<std::wstring> known;
            {
                std::lock_guard<std::mutex> lk(g_namesMx);
                known = g_knownNames;
            }
            std::wstring nm;
            size_t nat = 0, nlen = 0;
            // 2026-10-16: the intel names hold generic words too ('VTOL') - "go for the VTOLs" is still the class, while
            // "kill the Bulldog tank" / "Tank Killer" stay named
            std::vector<bool> noClass = used;
            noClass[at] = true; // the class word itself is never the name ('VTOL', 'tank', 'turret' are in the list)
            if (kind == 8 && w[at] == L"battle" && at + 1 < noClass.size())
                noClass[at + 1] = true; // "battle armor": both words
            if (kind && (FindNameInWords(w, &noClass, known, nm, nat, nlen) ||
                         (FindNameInWords(w, &used, known, nm, nat, nlen) && nlen > 1))) // a name spanning it ("Tank Killer")
                kind = 0;
        }
        if (kind)
        {
            bool verb = false, neg = false;
            for (size_t k = 0; k < at; ++k)
            {
                if (used[k]) // "hold fire" taken by the fire settings: the sweep starts paused (2026-10-14)
                    continue;
                verb |= AtkWordAt(w, k) > 0; // the shared attack words (2026-10-15, user)
                neg |= w[k] == L"dont" || w[k] == L"cease" || w[k] == L"stop" || (w[k] == L"hold" && k + 1 < w.size() && w[k + 1] == L"fire") ||
                       (w[k] == L"do" && k + 1 < w.size() && w[k + 1] == L"not");
            }
            if (verb && !neg)
            {
                Cmd c{};
                c.Type = C_AttackClass;
                c.A = kind;
                c.B = single ? 1 : 0;
                out.push_back(c);
                add((std::wstring(L"attack the ") + ClassWord(kind, single) + (single ? L" (the nearest one)" : L" (until none are left)")).c_str());
                return true;
            }
        }
    }

    // ---- CALLED SHOTS (2026-10-10, user; lancecontrol-32): "get his legs", "take out his left torso", "Three, legs on
    // the Marauder", "go for the Atlas's right arm" (Whisper's "Atlas's" -> "atlass"). The part on YOUR LOCK, or on the
    // unit named in the order - and an attack on it (LanceControl). "head" only right after his / her / its / their / a
    // name (never "head north"); the other parts need that, an aim word, or a unit name in the order.
    if (takeAny({L"cancel called shots", L"cancel called shot", L"no called shots", L"no more called shots", L"forget called shot",
                 L"aim anywhere", L"any part", L"pick your shots", L"pick shots", L"pick own shots"})) // "your" is dropped as filler
    {
        Cmd c{};
        c.Type = C_CalledShot;
        c.A = 0;
        out.push_back(c);
        g_parsedName.clear();
        add(L"cancel called shot");
        return true;
    }
    {
        static const struct
        {
            const wchar_t *P;
            int            Code;
        } kPart[] = {{L"left side torso", 8}, {L"right side torso", 9}, {L"left torso", 8},   {L"right torso", 9},
                     {L"side torsos", 7},     {L"side torso", 7},      {L"left leg", 2},     {L"right leg", 3},
                     {L"left arm", 5},        {L"right arm", 6},       {L"center torso", 10}, {L"centre torso", 10},
                     {L"center mass", 10},    {L"centre mass", 10},    {L"ct", 10},          {L"cockpit", 11},
                     {L"head", 11},           {L"legs", 1},            {L"leg", 1},          {L"arms", 4},
                     {L"arm", 4}};
        auto findAt = [&](const wchar_t *phrase, size_t &at, size_t &n) -> bool {
            const std::vector<std::wstring> p = SplitWords(phrase);
            for (size_t i = 0; !p.empty() && i + p.size() <= w.size(); ++i)
            {
                bool ok = true;
                for (size_t k = 0; k < p.size() && ok; ++k)
                    ok = !used[i + k] && !negated[i + k] && w[i + k] == p[k]; // "dont go for his legs" (2026-10-14)
                if (ok)
                {
                    at = i;
                    n = p.size();
                    return true;
                }
            }
            return false;
        };
        size_t pAt = 0, pLen = 0;
        int code = 0;
        for (auto &k : kPart)
            if (findAt(k.P, pAt, pLen))
            {
                code = k.Code;
                break;
            }
        if (code)
        {
            static const wchar_t *kPoss[] = {L"his", L"her", L"its", L"their", L"target", L"mechs", L"enemys"};
            const bool poss = pAt > 0 && std::any_of(std::begin(kPoss), std::end(kPoss), [&](const wchar_t *x) { return w[pAt - 1] == x; });
            std::vector<std::wstring> known;
            {
                std::lock_guard<std::mutex> lk(g_namesMx);
                known = g_knownNames;
            }
            std::wstring found;
            size_t at = 0, len = 0;
            bool named = FindNameInWords(w, &used, known, found, at, len);
            if (!named) // "atlass" (Atlas's) -> "atlas"
            {
                std::vector<std::wstring> w2 = w;
                for (auto &x : w2)
                    if (x.size() > 3 && x.back() == L's')
                        x.pop_back();
                named = FindNameInWords(w2, &used, known, found, at, len);
            }
            if (named && at < pAt + pLen && pAt < at + len)
                named = false; // the part's own words
            static const wchar_t *kVerb[] = {L"aim for", L"aim at", L"go for", L"go after", L"take out", L"knock out", L"shoot out",
                                             L"blow off", L"shoot off", L"take off", L"focus on", L"take down", L"shoot", L"hit",
                                             L"get", L"target", L"focus", L"strip", L"cripple", L"core", L"break", L"destroy",
                                             L"kill", L"aim"};
            size_t vAt = 0, vLen = 0;
            bool verb = false;
            for (auto *v : kVerb)
                if (findAt(v, vAt, vLen))
                {
                    verb = true;
                    break;
                }
            const bool namePoss = named && at + len == pAt;
            const bool ok = w[pAt] == L"head" ? (poss || namePoss) : (poss || namePoss || verb || named);
            if (ok)
            {
                for (size_t k = 0; k < pLen; ++k)
                    used[pAt + k] = true;
                if (poss)
                    used[pAt - 1] = true;
                if (named)
                    for (size_t k = 0; k < len; ++k)
                        used[at + k] = true;
                if (verb)
                    for (size_t k = 0; k < vLen; ++k)
                        used[vAt + k] = true;
                takeAny({L"on", L"at", L"for", L"that", L"this"});
                Cmd c{};
                c.Type = C_CalledShot;
                c.A = code;
                out.push_back(c);
                g_parsedName = named ? found : std::wstring();
                static const wchar_t *kSaid[] = {L"", L"legs", L"left leg", L"right leg", L"arms", L"left arm", L"right arm",
                                                 L"side torsos", L"left torso", L"right torso", L"centre torso", L"cockpit"};
                add((std::wstring(L"called shot: ") + kSaid[code] + (named ? L" on the " + found : std::wstring(L" on your target"))).c_str());
                return true;
            }
        }
    }

    // ---- NAV POINTS (2026-10-07k): "move to Nav Alpha", "hold at Nav Bravo", "fall back to Nav Charlie", "advance to Nav Delta"
    for (size_t i = 0; i < w.size(); ++i)
    {
        size_t len = 0;
        const int nl = NavWordsAt(w, i, len);
        if (nl < 0)
            continue;
        for (size_t k = 0; k < len; ++k)
            used[i + k] = true;
        if (negated[i]) // "dont go to nav Alpha" (2026-10-14): not this one - a later, un-negated nav still counts
            continue;
        int mode = 0;
        if (takeAny({L"indirect fire only", L"hold for indirect fire", L"indirect fire", L"indirect"}))
            mode = 4;
        else if (takeAny({L"hold position", L"hold there", L"hold at", L"hold", L"stay", L"wait", L"dig in"}))
            mode = 1;
        else if (takeAny({L"fall back", L"pull back", L"withdraw", L"retreat", L"back off", L"get back"}))
            mode = 2;
        else if (takeAny({L"advance", L"push up", L"push"}))
            mode = 3;
        Cmd c{};
        c.Type = C_Nav;
        c.A = nl;
        c.C = mode;
        out.push_back(c);
        static const wchar_t *kD[] = {L"move to nav (fire on the move)", L"hold at nav", L"fall back to nav", L"advance to nav (fire on the move)", L"indirect fire at nav"};
        add(kD[mode]);
        desc += L" " + NavSaid(nl);
        return true;
    }

    // ---- SHADOW ("follow it", "shadow that mech", "get eyes on the Bane", "find it") - build 2026-10-05n
    // REMOVED 2026-10-10 (user): "shadow" clashed with Shadow Hawk / Shadow Cat - the order is no longer parsed.
    if (false)
    {
        std::vector<std::wstring> known;
        {
            std::lock_guard<std::mutex> lk(g_namesMx);
            known = g_knownNames;
        }
        static const struct
        {
            const wchar_t *V;
            bool           NeedsTarget; // "follow" alone is "follow me"
        } kVerb[] = {{L"get eyes on", false}, {L"keep eyes on", false}, {L"eyes on", false}, {L"go find", false},
                     {L"hunt down", false},   {L"shadow", false},       {L"track", false},   {L"find", false},
                     {L"hunt", false},        {L"chase", false},        {L"pursue", false},  {L"follow", true}};
        for (auto &v : kVerb)
        {
            const std::vector<std::wstring> vp = SplitWords(v.V);
            size_t at = std::wstring::npos;
            for (size_t i = 0; i + vp.size() <= w.size() && at == std::wstring::npos; ++i)
            {
                bool ok = true;
                for (size_t k = 0; k < vp.size() && ok; ++k)
                    ok = !used[i + k] && w[i + k] == vp[k];
                if (ok)
                    at = i;
            }
            if (at == std::wstring::npos)
                continue;
            size_t j = at + vp.size();
            while (j < w.size() && !used[j] && (w[j] == L"the" || w[j] == L"that" || w[j] == L"this" || w[j] == L"those"))
                ++j;
            std::wstring name;
            size_t tlen = 0, nat = 0;
            bool target = FindNameInWords(w, &used, known, name, nat, tlen, j); // 2026-10-10: numerals / base names
            if (!target && j < w.size() && !used[j] &&
                (w[j] == L"it" || w[j] == L"him" || w[j] == L"them" || w[j] == L"mech" || w[j] == L"contact" || w[j] == L"target" ||
                 w[j] == L"one" || w[j] == L"bogey" || w[j] == L"hostile"))
            {
                target = true;
                tlen = 1;
            }
            if (v.NeedsTarget && !target)
                continue; // "follow me" - not this
            for (size_t k = at; k < j + tlen && k < w.size(); ++k)
                used[k] = true;
            Cmd c{};
            c.Type = C_Shadow;
            out.push_back(c);
            g_parsedName = name;
            add(L"shadow it");
            break;
        }
    }
    // ---- attack the nearest ("attack closest target", "engage the nearest enemy")
    bool atkWord = false; // "attack the nearest" needs an attack word - "closest contact" alone is no order (2026-10-06g)
    for (size_t q = 0; q < w.size(); ++q) // the shared attack words + "fire" (2026-10-15, user); bare "get" stays out
        atkWord |= AtkWordAt(w, q) > 0 || w[q] == L"fire"; // (2026-10-14 audit: "get behind the Atlas")
    {
        // "keep the lights busy", "go for the heavies", "engage the nearest medium" (2026-10-10): the nearest of that class
        static const struct
        {
            const wchar_t *W;
            int            A;
        } kCls[] = {{L"lights", 1}, {L"light mechs", 1}, {L"light", 1}, {L"mediums", 2}, {L"medium mechs", 2}, {L"medium", 2},
                    {L"heavies", 3}, {L"heavy mechs", 3}, {L"heavy", 3}, {L"assaults", 4}, {L"assault mechs", 4}, {L"assault", 4}};
        // 2026-10-14 full audit (user): the class word FIRST, then the verbs - "go after the Atlas" lost its "go after" here
        // before (taken with no class word following), and weight classes now take the vehicle classes' verb list
        // ("knock out / wipe out / take down the heavies"; "take down" no longer left over as an attack on your target).
        auto hasUnused = [&](const wchar_t *phrase) -> bool {
            const std::vector<std::wstring> p = SplitWords(phrase);
            for (size_t i = 0; !p.empty() && i + p.size() <= w.size(); ++i)
            {
                bool ok = true;
                for (size_t k = 0; k < p.size() && ok; ++k)
                    ok = !used[i + k] && w[i + k] == p[k];
                if (ok)
                    return true;
            }
            return false;
        };
        std::vector<const wchar_t *> kWVerb(std::begin(kAtkWords), std::end(kAtkWords)); // shared list (2026-10-15)
        kWVerb.push_back(L"fire");
        bool thatOne = false; // 2026-10-15 (user, C1): "that heavies" = that heavy - one target
        for (size_t k = 0; k + 1 < w.size(); ++k)
            if ((w[k] == L"that" || w[k] == L"this") && (w[k + 1] == L"lights" || w[k + 1] == L"mediums" ||
                                                         w[k + 1] == L"heavies" || w[k + 1] == L"assaults"))
                thatOne = true;
        bool clsHere = false;
        for (auto &k : kCls)
            clsHere |= hasUnused(k.W);
        bool busy = false;
        if (clsHere && hasUnused(L"keep") && (hasUnused(L"busy") || hasUnused(L"occupied")))
        {
            take(L"keep");
            takeAny({L"busy", L"occupied"});
            busy = true;
        }
        bool clsVerb = busy;
        if (clsHere)
            for (auto *v : kWVerb)
                clsVerb |= hasUnused(v);
        if (clsHere && clsVerb)
            for (auto &k : kCls)
                if (take(k.W))
                {
                    for (auto *v : kWVerb) // every verb of it, so none is left over as another order
                        while (take(v))
                        {
                        }
                    takeAny({L"nearest", L"closest", L"keep", L"busy"});
                    // 2026-10-14 audit (user): mech weight classes sweep like the vehicle classes - "the heavies" until none
                    // are left, "the heavy" / "the nearest heavy" one attack. Kinds 4..7 = light / medium / heavy / assault.
                    const std::wstring kw = k.W;
                    // 2026-10-15 (user, off-script round trip): Whisper drops the s of "assaults" / "lights" - like the VTOLs,
                    // "the assault" is the sweep; only "that / this / a / one" (+ "nearest / closest") picks one target
                    bool det = false;
                    {
                        const std::wstring w0 = SplitWords(k.W)[0];
                        for (size_t q = 1; q < w.size(); ++q)
                            if (w[q] == w0)
                                for (auto *d : {L"that", L"this", L"a", L"an", L"one", L"single", L"nearest", L"closest"})
                                    det |= w[q - 1] == d;
                    }
                    const bool one = thatOne || ((kw == L"light" || kw == L"medium" || kw == L"heavy" || kw == L"assault") && det);
                    Cmd c{};
                    c.Type = C_AttackClass;
                    c.A = 3 + k.A;
                    c.B = one ? 1 : 0;
                    out.push_back(c);
                    add((std::wstring(L"attack the ") + ClassWord(c.A, one) + (one ? L" (the nearest one)" : L" (until none are left)")).c_str());
                    atkWord = false;
                    break;
                }
    }
    if (atWill)
    {
        Cmd c{};
        c.Type = C_AttackNearest;
        out.push_back(c);
        add(L"attack the nearest enemy");
    }
    else if (atkWord && takeAny({L"closest target", L"nearest target", L"closest enemy", L"nearest enemy", L"closest hostile",
                 L"nearest hostile", L"closest contact", L"nearest contact", L"closest one", L"nearest one", L"closest",
                 L"nearest"}))
    {
        bool tookVerb = false;
        for (auto *v : kAtkWords)
            if (!tookVerb && take(v))
                tookVerb = true;
        Cmd c{};
        c.Type = C_AttackNearest;
        out.push_back(c);
        add(L"attack the nearest enemy");
    }
    // ---- attack a unit by NAME ("attack that Bane", "lance, engage the Atlas") - build 2026-10-05m
    {
        std::vector<std::wstring> known;
        {
            std::lock_guard<std::mutex> lk(g_namesMx);
            known = g_knownNames;
        }
        std::wstring found;
        size_t at = 0, len = 0;
        if (!FindNameInWords(w, &used, known, found, at, len)) // 2026-10-10: numerals / base names
            found.clear();
        bool namedVerb = false;
        if (!found.empty())
            for (auto *v : kAtkWords) // the shared attack words (2026-10-15, user)
                if (!namedVerb && take(v))
                    namedVerb = true;
        if (namedVerb) // take / drop: heard log, 2026-10-06b; "get" removed 2026-10-14 (audit)
        {
            for (size_t k = 0; k < len; ++k)
                used[at + k] = true;
            takeAny({L"that", L"the", L"this", L"those"});
            Cmd c{};
            c.Type = C_AttackNamed;
            out.push_back(c);
            g_parsedName = found;
            desc += (desc.empty() ? L"" : L" + ") + (L"attack the " + found);
        }
    }
    // ---- attack
    // 2026-10-10 (user): "attack the <name>" where the name is none we know (misheard) is NOT "attack my target" -
    // HandleSpoken asks to say again (g_attackUnheard). Generic words (target, enemy, turret, tank ...) still mean
    // your target, as before.
    g_attackUnheard.clear();
    size_t atkAt = std::wstring::npos, atkLen = 0;
    for (auto *v : {L"on my target", L"attack my target", L"engage my target", L"take it down", L"take it out", L"focus fire",
                    L"take out", L"take down", L"attack",
                    L"engage", L"hit", L"shoot", L"kill", L"destroy", L"focus"})
    {
        const std::vector<std::wstring> p = SplitWords(v);
        for (size_t i = 0; i + p.size() <= w.size() && atkAt == std::wstring::npos; ++i)
        {
            bool ok = true;
            for (size_t k = 0; k < p.size() && ok; ++k)
                ok = !used[i + k] && w[i + k] == p[k];
            if (ok)
            {
                atkAt = i;
                atkLen = p.size();
            }
        }
        if (atkAt != std::wstring::npos)
            break;
    }
    if (atkAt != std::wstring::npos)
    {
        for (size_t k = 0; k < atkLen; ++k)
            used[atkAt + k] = true;
        size_t j = atkAt + atkLen;
        bool det = false;
        while (j < w.size() && !used[j] && (w[j] == L"on" || w[j] == L"the" || w[j] == L"that" || w[j] == L"this" || w[j] == L"those"))
        {
            det = true;
            ++j;
        }
        if (!det && j < w.size()) // "attack honor": no "the", but only a name-like tail to the end of the sentence
        {
            static const wchar_t *kJoin[] = {L"and", L"then", L"now", L"again", L"too", L"also", L"with", L"from", L"at", L"to",
                                             L"all", L"everyone", L"me", L"us", L"here", L"there", L"them", L"fast", L"quick"};
            bool tailOnly = w.size() - j <= 4;
            for (size_t k = j; k < w.size() && tailOnly; ++k)
                tailOnly = !used[k];
            det = tailOnly && std::none_of(std::begin(kJoin), std::end(kJoin), [&](const wchar_t *g) { return w[j] == g; });
        }
        static const wchar_t *kGeneric[] = {L"my", L"target", L"it", L"him", L"them", L"enemy", L"enemies", L"hostile", L"hostiles",
                                            L"contact", L"one", L"mech", L"mechs", L"bogey", L"guy", L"turret", L"turrets", L"tank",
                                            L"tanks", L"vehicle", L"vehicles", L"vtol", L"vtols", L"helicopter", L"chopper",
                                            L"building", L"dropship", L"thing", L"bastard", L"sucker", L"nearest", L"closest",
                                            L"elemental", L"elementals", L"battle"}; // 2026-10-17: battle armour
        if (det && j < w.size() && !used[j] &&
            std::none_of(std::begin(kGeneric), std::end(kGeneric), [&](const wchar_t *g) { return w[j] == g; }))
        {
            std::vector<std::wstring> rest;
            for (size_t k = j; k < w.size() && k < j + 4 && !used[k]; ++k)
                rest.push_back(w[k]);
            g_attackUnheard = JoinWords(rest);
        }
        takeAny({L"my target", L"target", L"that", L"it", L"him", L"them"});
        Cmd c{};
        c.Type = C_Attack;
        out.push_back(c);
        add(L"attack my target");
    }
    // ---- indirect fire
    // 2026-10-10 (user): "artillery" / "arty" alone is a warning ("Artillery! Move!", "bunched up for the arty"), not an order
    if (takeAny({L"indirect fire only", L"hold for indirect fire", L"indirect fire", L"indirect", L"artillery only", L"arty only",
                 L"use artillery", L"use arty", L"lob"}))
    {
        Cmd c{};
        c.Type = C_Indirect;
        out.push_back(c);
        add(L"indirect fire");
    }

    // ---- distance and direction (for move / fall back)
    // ---- BEARINGS (2026-10-10, user): "move to my 2 o'clock 750" (from you, 12 = where you look), "your 2 o'clock"
    // (from the lancemate, 12 = where it faces), "set heading 3-5-0, move 400", "bearing 2-4-0, mark 600" (compass).
    // Taken before the distance, whose number reader would add the digits up. dir: 100 + compass degrees,
    // 1000 + degrees from your heading, 2000 + degrees from the lancemate's facing.
    int bearing = -1;
    {
        auto digitOf = [](const std::wstring &x) -> int {
            static const wchar_t *k[] = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine"};
            for (int d = 0; d < 10; ++d)
                if (x == k[d])
                    return d;
            if (x == L"oh" || x == L"o")
                return 0;
            if (x == L"niner")
                return 9;
            return -1;
        };
        static const wchar_t *kHour[] = {L"twelve", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine",
                                         L"ten", L"eleven"};
        for (size_t i = 0; i + 1 < w.size() && bearing < 0; ++i)
        {
            if (used[i])
                continue;
            int h = -1;
            for (int k = 0; k < 12; ++k)
                if (w[i] == kHour[k])
                    h = k;
            if (h < 0)
                continue;
            size_t e = i + 1;
            if (w[e] == L"oclock")
                ++e;
            else if (w[e] == L"o" && e + 1 < w.size() && w[e + 1] == L"clock")
                e += 2;
            else
                continue;
            const bool lm = i > 0 && w[i - 1] == L"yours";
            bearing = (lm ? 2000 : 1000) + h * 30;
            for (size_t k = i; k < e; ++k)
                used[k] = true;
            if (i > 0 && (w[i - 1] == L"yours" || w[i - 1] == L"my"))
                used[i - 1] = true;
        }
        for (size_t i = 0; i < w.size() && bearing < 0; ++i)
        {
            if (used[i] || (w[i] != L"heading" && w[i] != L"bearing"))
                continue;
            size_t j = i + 1;
            int digits = 0, v = 0;
            while (j < w.size() && digits < 3 && digitOf(w[j]) >= 0)
            {
                v = v * 10 + digitOf(w[j]);
                ++digits;
                ++j;
            }
            if (digits != 3)
            {
                // a number said as one ("090" -> "ninety", "two forty", "three hundred fifty"); 0..360
                static const wchar_t *kU[] = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine",
                                              L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen",
                                              L"seventeen", L"eighteen", L"nineteen"};
                static const wchar_t *kT[] = {L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety"};
                j = i + 1;
                // "heading zero ten" = 010, "bearing zero point twenty five" (Whisper's "0.25") = 025 (2026-10-11, round trip)
                if (j + 1 < w.size() && (w[j] == L"zero" || w[j] == L"oh" || w[j] == L"o") && digitOf(w[j + 1]) < 0)
                {
                    ++j;
                    if (j + 1 < w.size() && w[j] == L"point")
                        ++j;
                }
                v = 0;
                int hund = -1, cur = 0, parts = 0;
                bool tens = false, units = false;
                while (j < w.size())
                {
                    int u = -1, t = -1;
                    for (int k = 0; k < 20; ++k)
                        if (w[j] == kU[k])
                            u = k;
                    for (int k = 0; k < 8; ++k)
                        if (w[j] == kT[k])
                            t = 20 + 10 * k;
                    if (w[j] == L"hundred" && parts == 1 && hund < 0)
                    {
                        hund = cur;
                        cur = 0;
                        tens = units = false;
                    }
                    else if (t >= 0 && !tens && (!units || (parts == 1 && hund < 0 && cur > 0 && cur < 10)))
                    {
                        // "two forty": the first word is the hundreds
                        if (units)
                        {
                            hund = cur;
                            cur = 0;
                            units = false;
                        }
                        cur += t;
                        tens = true;
                    }
                    else if (u >= 0 && !units && (u < 10 || (!tens && cur == 0)) &&
                             !(tens && j + 1 < w.size() && (w[j + 1] == L"point" || w[j + 1] == L"hundred" || w[j + 1] == L"thousand")))
                        // "ninety one point five k", "two forty eight hundred": that number is the distance
                    {
                        cur += u;
                        units = true;
                    }
                    else
                        break;
                    ++parts;
                    ++j;
                }
                v = (hund > 0 ? hund * 100 : 0) + cur;
                digits = parts > 0 && v <= 360 ? 3 : 0;
            }
            if (digits == 3 && v <= 360)
            {
                bearing = 100 + v % 360;
                for (size_t k = i; k < j; ++k)
                    used[k] = true;
                if (i > 0 && w[i - 1] == L"set")
                    used[i - 1] = true;
            }
        }
        if (bearing >= 0)
            takeAny({L"mark", L"to", L"at"});
    }
    int dist = -1;
    double distFrac = 0.0;
    for (size_t i = 0; i < w.size() && dist < 0; ++i)
    {
        if (used[i])
            continue;
        static const wchar_t *kUnit[] = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine",
                                         L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen",
                                         L"seventeen", L"eighteen", L"nineteen"};
        static const wchar_t *kTen[] = {L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety"};
        int total = 0, cur = 0;
        size_t j = i;
        bool any = false;
        for (; j < w.size() && !used[j]; ++j)
        {
            int u = -1;
            for (int k = 0; k < 20; ++k)
                if (w[j] == kUnit[k])
                    u = k;
            int t = -1;
            for (int k = 0; k < 8; ++k)
                if (w[j] == kTen[k])
                    t = 20 + 10 * k;
            if (u >= 0)
                cur += u;
            else if (t >= 0)
                cur += t;
            else if (w[j] == L"hundred")
                cur = (cur ? cur : 1) * 100;
            else if (w[j] == L"thousand")
            {
                total += (cur ? cur : 1) * 1000;
                cur = 0;
            }
            else if (w[j] == L"a" && j + 1 < w.size() && w[j + 1] == L"hundred")
                continue;
            else if (w[j] == L"point" && any && j + 1 < w.size()) // "one point five k"
            {
                int fd = -1;
                for (int k = 0; k < 10; ++k)
                    if (w[j + 1] == kUnit[k])
                        fd = k;
                if (fd < 0)
                    break;
                distFrac = fd / 10.0;
                ++j;
            }
            else
                break;
            any = true;
        }
        if (any && total + cur > 0)
        {
            dist = total + cur;
            for (size_t k = i; k < j; ++k)
                used[k] = true;
            // 2026-10-10: kilometres - "k", "klicks" ("clicks" was taken as metres)
            static const wchar_t *kKm[] = {L"k", L"km", L"klick", L"klicks", L"click", L"clicks", L"kilometres", L"kilometers",
                                           L"kilometre", L"kilometer", L"kay", L"kays"};
            if (j < w.size() && std::any_of(std::begin(kKm), std::end(kKm), [&](const wchar_t *x) { return w[j] == x; }))
            {
                used[j] = true;
                dist = (int)((dist + distFrac) * 1000.0 + 0.5);
            }
            else if (j < w.size() && w[j] == L"metres")
                used[j] = true;
        }
    }
    int dir = -1;
    static const struct
    {
        const wchar_t *W;
        int            D;
    } kDirWords[] = {{L"north east", 8}, {L"northeast", 8}, {L"north west", 9}, {L"northwest", 9}, {L"south east", 10},
                     {L"southeast", 10}, {L"south west", 11}, {L"southwest", 11}, {L"north", 0}, {L"south", 1},
                     {L"east", 2},       {L"west", 3},        {L"forward", 4},    {L"up", 4},          {L"back", 5},
                     {L"backward", 5},   {L"behind", 5},      {L"rear", 5},       {L"left", 6},        {L"right", 7}};
    if (bearing >= 0)
        dir = bearing;
    auto takeDir = [&]() {
        for (auto &d : kDirWords)
            if (take(d.W))
            {
                dir = d.D;
                return;
            }
    };
    // ---- follow / fall back. "get back here" is a follow; any other fall-back verb is a fall back
    // ("fall back to my position" = fall back to me, facing the enemy); then the other follow words.
    bool follow = takeAny({L"get back here", L"back here", L"come back"});
    // RUN FOR IT (2026-10-13, user): the only beeline left - straight to 200 m behind YOU (your cockpit's facing),
    // no firing on the way. The escape phrases moved here from fall back. Bare "run" is not one ("run cool / hot").
    const bool runAway = !follow && takeAny({L"make a run for it", L"make a break for it", L"run for it", L"run away",
                                             L"get out of there", L"get out of here", L"get out", L"bug out", L"pull out",
                                             L"break off", L"disengage", L"get clear"});
    const bool fallBack = !follow && !runAway && takeAny({L"fall back", L"pull back", L"back off", L"withdraw", L"get back"});
    if (!follow && !fallBack && !runAway)
        follow = takeAny({L"stay close", L"stay with me", L"keep close", L"stick with me", L"stay on me", L"dont wander off",
                          L"tighten it up", L"tighten up", L"close up", // 2026-10-10 (user): these are follow, not hold
                          L"form on me", L"form up on me", L"follow me", L"on me", L"with me", L"regroup", L"rally",
                          L"come to me", L"come here", L"form up", L"on my position", L"to my position", L"fall in"}); // bare "follow": support (2026-10-14)
    if (runAway)
    {
        takeDir(); // "get out of there, 300 west" still works; the default is 200 m straight back
        Cmd c{};
        c.Type = C_Move;
        c.A = std::max(25, std::min(2000, dist > 0 ? dist : 200));
        c.D = dist > 2000 ? 1 : 0; // MOVE CAP (2026-10-15, user): over 2 km is cut to 2 km - the pilot says so
        c.B = dir >= 0 ? dir : 5; // 5 = back, from your cockpit's facing
        c.C = 4;                  // 4 = beeline (TCO_Move)
        out.push_back(c);
        add(L"run for it (beeline)");
    }
    if (fallBack)
    {
        const bool toMe = takeAny({L"to me", L"on me", L"to my position"});
        takeDir();
        if (!toMe && (dir >= 0 || dist > 0))
        {
            Cmd c{};
            c.Type = C_FallMove;
            c.A = std::max(25, std::min(2000, dist > 0 ? dist : 100));
            c.D = dist > 2000 ? 1 : 0; // MOVE CAP (2026-10-15, user): the pilot says it was cut to 2 km
            c.B = dir >= 0 ? dir : 5;
            out.push_back(c);
            add(L"fall back to a point");
        }
        else
        {
            Cmd c{};
            c.Type = C_Fallback;
            out.push_back(c);
            add(L"fall back to me");
        }
    }
    else if (follow)
    {
        Cmd c{};
        c.Type = C_Follow;
        out.push_back(c);
        add(L"form on me");
    }
    // ---- move
    // "push (up)", "advance", "move up" with no direction mean forward
    const bool pushVerb = !fallBack && !follow && !runAway && takeAny({L"push up", L"move up", L"push", L"advance"});
    const bool moveVerb = pushVerb || (!fallBack && !follow && !runAway &&
                                       takeAny({L"move", L"go", L"head", L"proceed", L"walk", L"relocate", L"reposition", L"get"}));
    if (!fallBack && !follow && !runAway)
    {
        if (moveVerb && dir < 0)
            takeDir();
        else if (dist > 0 && dir < 0)
            takeDir();
        if (pushVerb && dir < 0)
            dir = 4; // "push", "advance" with no direction: forward
        if ((moveVerb || dist > 0) && dir >= 0)
        {
            Cmd c{};
            c.Type = C_Move;
            c.A = std::max(25, std::min(2000, dist > 0 ? dist : 100));
            c.D = dist > 2000 ? 1 : 0; // MOVE CAP (2026-10-15, user): the pilot says it was cut to 2 km
            c.B = dir;
            out.push_back(c); // C = 0: fire on the move (2026-10-13, user: every move order; only "run for it" beelines)
            add(L"move (fire on the move)");
        }
    }
    // ---- hold position (after "hold fire" / "hold <weapons>" took theirs)
    if (!fallBack && !follow &&
        takeAny({L"hold position", L"hold here", L"hold the line", L"hold", L"stay here", L"stay", L"wait here", L"wait there", L"halt", L"stop",
                 L"stand by", L"dig in"}))
    {
        Cmd c{};
        c.Type = C_Hold;
        out.push_back(c);
        add(L"hold position");
    }
    // "move ... and (then) hold" / "... hold for indirect fire": one order - hold (or indirect hold)
    // AT the move's point, not where it stands now.
    size_t mv = out.size(), hd = out.size();
    for (size_t k = 0; k < out.size(); ++k)
    {
        if (out[k].Type == C_Move && mv == out.size())
            mv = k;
        if ((out[k].Type == C_Hold || out[k].Type == C_Indirect) && hd == out.size())
            hd = k;
    }
    if (mv < out.size() && hd < out.size())
    {
        out[mv].C = out[hd].Type == C_Hold ? 1 : 2;
        desc += out[mv].C == 1 ? L" (hold there)" : L" (indirect fire there)";
        out.erase(out.begin() + (long)hd);
    }
    return !out.empty();
}

// Whisper sometimes says the same thing several times over ("Lance, status update. Lance, status
// update."): a run of the same words repeated is cut back to one.
std::vector<std::wstring> CollapseRepeats(const std::vector<std::wstring> &w)
{
    const size_t n = w.size();
    for (size_t p = 1; p * 2 <= n; ++p)
    {
        bool rep = true;
        for (size_t i = p; i < n && rep; ++i)
            rep = w[i] == w[i % p];
        if (rep)
            return std::vector<std::wstring>(w.begin(), w.begin() + (long)p);
    }
    return w;
}

std::vector<int> g_lastWho;   // who the last understood order went to (Whisper's thread only)
std::atomic<int>       g_lastReporter{0};   // the lancemate who last named a unit (NoteIntel) - "shadow it" with no callsign
std::atomic<ULONGLONG> g_lastReporterMs{0};
ULONGLONG        g_lastWhoMs = 0;

// HIGH GROUND (2026-10-14): what an answer to "which hill, Commander?" picks - 100 + letter ("Foxtrot", "Nav
// Foxtrot"), 0..3 ("the first / second ..." or "option two"), 200 the highest, 201 the closest, 202 the last,
// -2 none ("never mind", "none of them"), -1 not an answer (an ordinary order or question goes on as usual).
int HillAnswerCode(std::vector<std::wstring> b)
{
    while (!b.empty() && (b.back() == L"commander" || b.back() == L"please" || b.back() == L"over" || b.back() == L"now" ||
                          b.back() == L"sir" || b.back() == L"boss"))
        b.pop_back();
    if (b.empty() || b.size() > 6)
        return -1;
    const std::wstring t = JoinWords(b);
    static const wchar_t *kNone[] = {L"no", L"nope", L"negative", L"never mind", L"nevermind", L"cancel", L"cancel that",
                                     L"none", L"none of them", L"none of those", L"forget it", L"belay that", L"scratch that",
                                     L"disregard", L"disregard that", L"as you were", L"stand down", L"not now", L"no thanks"};
    for (auto *k : kNone)
        if (t == k)
            return -2;
    static const wchar_t *kYes[] = {L"yes", L"yeah", L"yep", L"affirmative", L"do it", L"go ahead", L"roger", L"roger that",
                                    L"sure", L"head there", L"move there", L"go there", L"go", L"please do", L"yes please",
                                    L"do that", L"copy", L"sounds good"};
    for (auto *k : kYes)
        if (t == k)
            return 300; // "move there anyway?" (NavOnly) - for a hill list it asks again
    static const wchar_t *kOrder[] = {L"attack", L"kill", L"shoot", L"fire", L"hit", L"engage", L"hold", L"follow", L"form",
                                      L"status", L"report", L"target", L"destroy", L"where", L"what", L"how", L"who"};
    for (auto &x : b)
        if (std::any_of(std::begin(kOrder), std::end(kOrder), [&](const wchar_t *o) { return x == o; }))
            return -1; // "Two, attack the closest" while the offer is open is an attack, not a pick
    // 2026-10-14 audit (user): a letter is a pick only when it IS one of the candidates ("move to nav Alpha" stays a move);
    // "first / highest / closest / last" only as a bare answer ("the highest one") - "take out the turrets first" is no pick
    const uint32_t cand = g_hillLetters;
    static const wchar_t *kBare[] = {L"the", L"one", L"nav", L"navpoint", L"waypoint", L"point", L"hill", L"option", L"number",
                                     L"go", L"with", L"take", L"lets", L"that", L"a", L"to", L"up", L"pick", L"i", L"want",
                                     L"highest", L"tallest", L"biggest", L"top", L"closest", L"nearest", L"last", L"first",
                                     L"second", L"third", L"fourth", L"two", L"three", L"four", L"yeah", L"yes", L"ok", L"okay"};
    bool bare = true;
    for (auto &x : b)
        bare = bare && std::any_of(std::begin(kBare), std::end(kBare), [&](const wchar_t *o) { return x == o; });
    for (size_t k = 0; k < b.size(); ++k)
    {
        const std::wstring &x = b[k];
        for (int l = 0; l < kVhudNavLetters; ++l)
        {
            if (x == NavWord(l))
                return (cand >> l) & 1u ? 100 + l : -1;
        }
    }
    if (!bare)
        return -1;
    for (size_t k = 0; k < b.size(); ++k)
    {
        const std::wstring &x = b[k];
        if (x == L"highest" || x == L"tallest" || x == L"biggest" || x == L"top")
            return 200;
        if (x == L"closest" || x == L"nearest")
            return 201;
        if (x == L"last")
            return 202;
        if (x == L"first")
            return 0;
        if (x == L"second")
            return 1;
        if (x == L"third")
            return 2;
        if (x == L"fourth")
            return 3;
        if ((x == L"option" || x == L"number") && k + 1 < b.size())
        {
            static const wchar_t *kN[] = {L"one", L"two", L"three", L"four"};
            for (int i = 0; i < 4; ++i)
                if (b[k + 1] == kN[i])
                    return i;
        }
    }
    return -1;
}

// PATCH-THROUGH COMMAND (2026-10-06g): "patch it through", "patch in the Atlas", "patch the closest contact
// in", "where's the closest contact? can you patch it in?" - your lock on that unit, no offer needed.
// name: "closest", a known unit name, or "" (the offer's unit, else the last one reported).
bool PatchCommand(const std::vector<std::wstring> &w, std::wstring &name)
{
    bool patch = false;
    for (auto &x : w)
        patch |= x == L"patch" || x == L"patched" || x == L"patching";
    if (!patch)
        return false;
    name.clear();
    for (auto &x : w)
        if (x == L"closest" || x == L"nearest")
        {
            name = L"closest";
            return true;
        }
    std::vector<std::wstring> known;
    {
        std::lock_guard<std::mutex> lk(g_namesMx);
        known = g_knownNames;
    }
    size_t at = 0, len = 0;
    FindNameInWords(w, nullptr, known, name, at, len); // 2026-10-10: numerals / base names
    return true;
}

// CORRECTIONS (2026-10-07a): the last batch of orders you gave (Whisper's thread), for "actually, 300".
std::vector<Cmd> g_lastOrders;
std::vector<Cmd> g_lastOrdersBy[9]; // 2026-10-07d: the last orders each lance number got ("Two, actually 300")
std::vector<int> g_corrWho;   // 2026-10-07d: who got the last REAL order (a status request does not count)
ULONGLONG        g_corrWhoMs = 0;
std::wstring     g_lastOrdersName; // bug sweep 17 (2026-10-18): the unit name that went with g_lastOrders

// What a correction corrects (2026-10-07d). A status request is not an order to correct: it leaves the
// last orders (and who got them) as they were - "two, move 200 north" / "lance, status" / "actually, 300".
// bug sweep 17: name = the unit named in those orders (g_parsedName is overwritten by every later probe parse).
// PINCER (2026-10-20): each NAMED lancemate's place in the pincer (C: 0 left, 1 right, 2 rear, ...) and how many were named
// (D) - "Two, three, pincer the Atlas". The whole lance ("lance, pincer it") is counted in Execute instead.
void PincerSlot(Cmd &c, const std::vector<int> &who, int slot)
{
    if (c.Type != C_Pincer || slot == 0)
        return;
    std::vector<int> u;
    for (int x : who)
        if (x && std::find(u.begin(), u.end(), x) == u.end())
            u.push_back(x);
    c.C = (int)(std::find(u.begin(), u.end(), slot) - u.begin());
    c.D = (int)u.size();
}

void RememberOrders(const std::vector<int> &who, const std::vector<Cmd> &cmds, const std::wstring &name)
{
    g_lastWho = who; // "then hold position" with no callsign still goes to whoever was just spoken to
    g_lastWhoMs = GetTickCount64();
    bool real = false;
    for (auto &c : cmds)
        real = real || c.Type != C_Status;
    if (!real)
        return;
    g_lastOrders = cmds;
    g_lastOrdersName = name;
    for (int s : who)
        for (int k = 1; k <= 8; ++k)
            if (s == 0 || s == k)
                g_lastOrdersBy[k] = cmds;
    g_corrWho = who;
    g_corrWhoMs = g_lastWhoMs;
}

// A distance said anywhere ("three hundred", "one thousand five hundred"), -1 = none.
int SpokenNumber(const std::vector<std::wstring> &w)
{
    static const wchar_t *kUnit[] = {L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine",
                                     L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen",
                                     L"seventeen", L"eighteen", L"nineteen"};
    static const wchar_t *kTen[] = {L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety"};
    for (size_t i = 0; i < w.size(); ++i)
    {
        int total = 0, cur = 0;
        bool any = false;
        for (size_t j = i; j < w.size(); ++j)
        {
            int u = -1, t = -1;
            for (int k = 0; k < 20; ++k)
                if (w[j] == kUnit[k])
                    u = k;
            for (int k = 0; k < 8; ++k)
                if (w[j] == kTen[k])
                    t = 20 + 10 * k;
            if (u >= 0)
                cur += u;
            else if (t >= 0)
                cur += t;
            else if (w[j] == L"hundred")
                cur = (cur ? cur : 1) * 100;
            else if (w[j] == L"thousand")
            {
                total += (cur ? cur : 1) * 1000;
                cur = 0;
            }
            else
                break;
            any = true;
        }
        if (any && total + cur > 0)
            return total + cur;
    }
    return -1;
}

// A direction said anywhere (the move parser's codes: 0 N, 1 S, 2 E, 3 W, 4 fwd, 5 back, 6 left, 7 right, 8-11 diagonals).
int SpokenDirection(const std::vector<std::wstring> &w)
{
    static const struct
    {
        const wchar_t *A, *B;
        int            D;
    } k2[] = {{L"north", L"east", 8}, {L"north", L"west", 9}, {L"south", L"east", 10}, {L"south", L"west", 11}};
    for (size_t i = 0; i + 1 < w.size(); ++i)
        for (auto &k : k2)
            if (w[i] == k.A && w[i + 1] == k.B)
                return k.D;
    static const struct
    {
        const wchar_t *W;
        int            D;
    } k1[] = {{L"northeast", 8}, {L"northwest", 9}, {L"southeast", 10}, {L"southwest", 11}, {L"north", 0}, {L"south", 1},
              {L"east", 2},      {L"west", 3},      {L"forward", 4},    {L"back", 5},       {L"backward", 5}, {L"left", 6},
              {L"right", 7}};
    for (size_t i = 0; i < w.size(); ++i)
    {
        // "fall back right 500" goes right: the "back" of fall / pull / get back is the verb (2026-10-11)
        if (w[i] == L"back" && i > 0 && (w[i - 1] == L"fall" || w[i - 1] == L"pull" || w[i - 1] == L"get"))
            continue;
        for (auto &k : k1)
            if (w[i] == k.W)
                return k.D;
    }
    return -1;
}

void HandleSpoken(const std::wstring &text, bool pttHeld, double secs)
{
    // SEVERAL CALLSIGNS (2026-10-10, user): "Two and Three, focus the Marauder; Four, keep the lights busy" - split the
    // sentence where a new part (after ; . ! ?) starts with a callsign, and take each part on its own.
    {
        std::vector<std::wstring> parts;
        std::wstring cur;
        for (size_t ci = 0; ci < text.size(); ++ci)
        {
            const wchar_t ch = text[ci];
            cur += ch;
            const bool decimal = ch == L'.' && ci > 0 && ci + 1 < text.size() && iswdigit(text[ci - 1]) && iswdigit(text[ci + 1]); // "1.5 k"
            if (ch == L';' || (ch == L'.' && !decimal) || ch == L'!' || ch == L'?')
            {
                parts.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty())
            parts.push_back(cur);
        std::vector<std::wstring> groups;
        for (auto &pt : parts)
        {
            const std::vector<std::wstring> pw = NormalizeHeard(pt);
            // "Three, move to my ten o'clock. Three hundred." / "heading 045. Five hundred and hold." (2026-10-11, round
            // trip): Whisper's full stop before a distance - a callsign word followed by a number word or a unit is the
            // distance, not a new callsign, so that part stays with the order before it
            auto numTail = [](const std::wstring &x) {
                for (const wchar_t *k : {L"hundred", L"thousand", L"metres", L"k", L"km", L"klick", L"klicks", L"click", L"clicks",
                                         L"kilometres", L"kilometre", L"point", L"oclock", L"twenty", L"thirty", L"forty", L"fifty",
                                         L"sixty", L"seventy", L"eighty", L"ninety"})
                    if (x == k)
                        return true;
                return false;
            };
            bool callsignStart = !pw.empty() && WhoWord(pw[0]) >= 2 && !(pw.size() > 1 && numTail(pw[1]));
            // "Two, go for the Wolfhound. Two's right arm." (2026-10-11, user): a full stop INSIDE a numbered unit name -
            // the part before ends with a unit whose II / III version is known and this part starts with that number
            if (callsignStart && !groups.empty())
            {
                std::vector<std::wstring> known;
                {
                    std::lock_guard<std::mutex> lk(g_namesMx);
                    known = g_knownNames;
                }
                std::vector<std::wstring> joined = NormalizeHeard(groups.back());
                joined.push_back(pw[0]);
                for (auto &pat : NamePatterns(known))
                {
                    if (pat.Base || pat.Tok.size() < 2 || !IsNumeralWord(pat.Tok.back()))
                        continue;
                    for (size_t L = 2; L <= std::min<size_t>(6, joined.size()) && callsignStart; ++L)
                        if (MatchNameAt(joined, nullptr, joined.size() - L, pat.Tok) == L)
                            callsignStart = false;
                    if (!callsignStart)
                        break;
                }
            }
            if (groups.empty() || !callsignStart)
            {
                if (groups.empty())
                    groups.push_back(pt);
                else
                    groups.back() += pt;
            }
            else
                groups.push_back(pt);
        }
        if (groups.size() > 1)
        {
            for (auto &g : groups)
                HandleSpoken(g, pttHeld, secs);
            return;
        }
    }
    // "No firing" / "no shooting" (2026-10-14, user audit): hold fire. Done on the raw text - the clean-up drops a leading
    // "no" (it made the sentence a correction with nothing to correct).
    std::wstring textN = text;
    {
        std::wstring lo = text;
        for (auto &ch : lo)
            ch = (wchar_t)towlower(ch);
        for (const wchar_t *k : {L"no firing", L"no shooting", L"no engaging"})
        {
            const size_t kl = wcslen(k);
            for (size_t p = lo.find(k); p != std::wstring::npos; p = lo.find(k, p + 1))
                if ((p == 0 || !iswalpha(lo[p - 1])) && (p + kl >= lo.size() || !iswalpha(lo[p + kl])))
                {
                    textN.replace(p, kl, L"hold fire");
                    lo.replace(p, kl, L"hold fire");
                }
        }
    }
    std::vector<std::wstring> w = CollapseRepeats(NormalizeHeard(textN));
    // CALLSIGN GUESS (2026-10-14, user; [voice] callsign_guess, on by default since 2026-10-15): fast speech clips the callsign - "Two" ->
    // "Do", "Three" -> "Re" / "Remove", "Four" -> "Or" / "Before" / "Force" / "Forward" / "Fall". Only the FIRST word, never
    // when it is already a valid start ("do not", "do you copy", "fall back"), and only if the rest is then an order.
    if (g_cfg.CallsignGuess && w.size() >= 2 && WhoWord(w[0]) < 0)
    {
        static const struct
        {
            const wchar_t *W, *To;
        } kGuess[] = {{L"do", L"two"},        {L"re", L"three"},     {L"remove", L"three move"}, {L"or", L"four"},
                      {L"before", L"four"},   {L"force", L"four"},   {L"forward", L"four"},      {L"fourth", L"four"},
                      {L"fall", L"four"},
                      // 2026-10-15 (user, overnight round trip A3/A4): more clipped callsigns + Three merged into its verb
                      {L"who", L"two"},         {L"new", L"two"},       {L"free", L"three"},      {L"brie", L"three"},
                      {L"retake", L"three take"}, {L"rehead", L"three head"}, {L"regal", L"three go"}};
        // 2026-10-15 (user, overnight round trip A1): "Two and Three" heard as "Do in three" / "Doin' three" / "Doing
        // three" / "Dewan three" / "Due in three" / "Who and three" - only when a lancemate's callsign follows.
        {
            size_t n = 0;
            if (w[0] == L"doin" || w[0] == L"doing" || w[0] == L"dewan")
                n = 1;
            else if (w.size() >= 3 && ((w[0] == L"do" && w[1] == L"in") || (w[0] == L"due" && w[1] == L"in") ||
                                       (w[0] == L"who" && w[1] == L"and")))
                n = 2;
            if (n && n < w.size() && WhoWord(w[n]) >= 1)
            {
                Log(L"callsign guessed: \"%ls\" -> two and", JoinWords(std::vector<std::wstring>(w.begin(), w.begin() + (long)n)).c_str());
                std::vector<std::wstring> cand = {L"two", L"and"};
                cand.insert(cand.end(), w.begin() + (long)n, w.end());
                w.swap(cand);
            }
        }
        const std::wstring &a = w[0], &b = w[1];
        for (auto &g : kGuess)
        {
            if (a != g.W)
                continue;
            size_t drop = 1;
            if (a == L"do" && (b == L"not" || b == L"it" || b == L"that" || b == L"so" || b == L"this"))
                break;
            if (a == L"do" && b == L"you")
            {
                if (w.size() < 3 || w[2] == L"copy" || w[2] == L"read" || w[2] == L"hear" || w[2] == L"see" || w[2] == L"know")
                    break;
                drop = 2; // "Do you move to Nav Echo" = "Two, move to Nav Echo"
            }
            if (a == L"fall" && (b == L"back" || b == L"in" || b == L"out" || b == L"behind"))
                break;
            if (a == L"free" && b == L"fire") // 2026-10-15: "free fire" is never "Three, fire"
                break;
            // 2026-10-14 (user, ParseRisk): an ORDER VERB must follow the guessed callsign - "Before we go, hold here"
            // was Four. (The guessed word itself may carry the verb: "remove" = "three move".)
            static const wchar_t *kVerb[] = {L"move", L"hold", L"advance", L"go", L"get", L"attack", L"kill", L"take", L"engage",
                                             L"push", L"run", L"support", L"assist", L"help", L"follow", L"focus", L"hit",
                                             L"destroy", L"bug", L"break", L"pull", L"make", L"find", L"climb", L"head", L"knock",
                                             L"keep", L"back", L"fall", L"form", L"stay", L"wait", L"weapons", L"open", L"cease",
                                             L"report", L"status", L"flank", L"pincer", L"charge", L"rush", L"shoot", L"fire", L"clear",
                                             L"retreat", L"regroup", L"rally", L"indirect", L"target", L"strip", L"aim", L"legs",
                                             L"light", L"lasers", L"missiles", L"run", L"alpha", L"come", L"advanced", L"moved",
                                             L"hit", L"deal", L"wipe", L"smash", L"neutralize", L"eliminate", L"cover", L"sound",
                                             L"radio", L"comms", L"check", L"proceed", L"relocate", L"reposition"};
            if (std::wstring(g.To).find(L' ') == std::wstring::npos &&
                !(drop < w.size() && std::any_of(std::begin(kVerb), std::end(kVerb), [&](const wchar_t *x) { return w[drop] == x; })))
                break;
            std::vector<std::wstring> cand = SplitWords(g.To);
            cand.insert(cand.end(), w.begin() + (long)drop, w.end());
            const std::vector<std::wstring> rest(cand.begin() + 1, cand.end());
            std::vector<Cmd> probe;
            std::wstring pd;
            const std::wstring keepName = g_parsedName;
            const bool isOrder = !rest.empty() && ParseOrder(rest, probe, pd);
            g_parsedName = keepName;
            if (isOrder)
            {
                Log(L"callsign guessed: \"%ls\" -> %ls", a.c_str(), cand[0].c_str());
                HeardLog(L"\"" + text + L"\"  callsign guessed: \"" + a + L"\" -> " + cand[0]);
                w.swap(cand);
            }
            break;
        }
    }
    // REMARKS (2026-10-10, user): praise and "watch your fire" are answered with a line, never turned into an order
    // ("Good kill, Two" made Two attack). The pilot named anywhere in the sentence answers (else Two).
    {
        const std::wstring all = L" " + JoinWords(w) + L" ";
        auto has = [&](std::initializer_list<const wchar_t *> l) {
            for (auto *x : l)
                if (all.find(L" " + std::wstring(x) + L" ") != std::wstring::npos)
                    return true;
            return false;
        };
        const wchar_t *grp = nullptr;
        if (has({L"good kill", L"nice kill", L"nice shot", L"good shot", L"good shooting", L"nice shooting", L"nice work", L"good work",
                 L"well done", L"great job", L"good job", L"nice one", L"great shot", L"great kill"}))
            grp = L"reply_thanks";
        else if (has({L"check fire", L"check your fire", L"watch fire", L"watch where youre shooting", L"watch where you are shooting", L"youre hitting me",
                      L"you are hitting me", L"stop hitting me", L"stop shooting me", L"friendly fire", L"in front of you"}) &&
                 !has({L"hold fire", L"cease fire", L"stop firing", L"stop shooting", L"cease firing"}))
            grp = L"reply_check_fire";
        if (grp)
        {
            int slot = 0;
            for (auto &x : w)
            {
                const int s = WhoWord(x);
                if (s >= 2 && s <= 8)
                {
                    slot = s;
                    break;
                }
            }
            SpokenAction a;
            a.ReplyGroup = grp;
            a.C.Who = slot;
            {
                std::lock_guard<std::mutex> lk(g_amx);
                g_actions.push_back(a);
            }
            Log(L"whisper heard \"%ls\" [%ls] -> a remark (%ls)", text.c_str(), JoinWords(w).c_str(), grp);
            HeardLog(L"\"" + text + L"\"  [" + JoinWords(w) + L"]  -> a remark (" + grp + L")");
            return;
        }
    }
    // ENHANCED IMAGING VISION MODE (VHUD v22): "activate / deactivate enhanced imaging" - the player's own
    // switch, not an order, so it is taken before any addressing. (Mishearings belong in HeardAs.txt.)
    for (size_t i = 0; i + 1 < w.size(); ++i)
        if (w[i] == L"enhanced" && w[i + 1] == L"imaging")
        {
            int on = -1;
            if (i > 0 && (w[i - 1] == L"activate" || w[i - 1] == L"enable" || w[i - 1] == L"engage" ||
                          w[i - 1] == L"initiate")) // initiate: 2026-10-12
                on = 1;
            else if (i > 0 && (w[i - 1] == L"deactivate" || w[i - 1] == L"disable" || w[i - 1] == L"disengage"))
                on = 0;
            else if (i + 2 < w.size() && w[i + 2] == L"on")
                on = 1;
            else if (i + 2 < w.size() && w[i + 2] == L"off")
                on = 0;
            if (on >= 0)
            {
                SetVision(on == 1);
                return;
            }
        }
    // CORRECTIONS (2026-10-07a): "actually ...", "wait, no ...", "scratch that ...", "belay that" - for whoever got
    // your last order (or the pilot named), replacing it. "wait ..." is a correction too (2026-10-07b) - "wait here" /
    // "wait there" hold - but a "wait" with NOTHING after it is ignored (2026-10-07d: a thinking pause, cut off by
    // Whisper; "belay that" / "never mind" cancel). "no" counts only when an order follows. Not while a
    // patch-through offer waits for a short yes / no.
    bool correction = false, weakNo = false, waitOnly = false, cancelWord = false, addToo = false;
    {
        const ULONGLONG om = g_offerMs;
        const ULONGLONG hm0 = g_hillMs; // HIGH GROUND (2026-10-14): "never mind" answers "which hill?"
        const bool offerShort = ((om && GetTickCount64() - om < 20000) || (hm0 && GetTickCount64() - hm0 < 31000)) && w.size() <= 3;
        static const std::vector<std::vector<std::wstring>> kLead = {
            {L"wait", L"scratch", L"that"}, {L"as", L"you", L"were"}, {L"wait", L"actually"}, {L"wait", L"no"}, {L"no", L"wait"},
            {L"wait", L"wait"},              {L"no", L"no"},           {L"scratch", L"that"},   {L"belay", L"that"},
            {L"belay", L"last"},             {L"never", L"mind"},      {L"disregard", L"that"}, {L"disregard", L"last"},
            {L"disregard"},                  {L"nevermind"},           {L"correction"},         {L"actually"},
            {L"belay"}, {L"wait"}}; // bare "wait": a correction (user, 2026-10-07b) - "wait here" / "wait there" hold
        const bool waitHere = w.size() > 1 && w[0] == L"wait" && (w[1] == L"here" || w[1] == L"there");
        if (!offerShort && !waitHere)
        {
            for (auto &k : kLead)
                if (k.size() <= w.size() && std::equal(k.begin(), k.end(), w.begin()))
                {
                    waitOnly = k.size() <= 2 && k[0] == L"wait" && k.back() == L"wait"; // "wait" / "wait wait"
                    cancelWord = k[0] == L"as" || k[0] == L"scratch" || k[0] == L"belay" || k[0] == L"never" ||
                                 k[0] == L"nevermind" || k[0] == L"disregard" || (k.size() > 1 && k[1] == L"scratch");
                    w.erase(w.begin(), w.begin() + (long)k.size());
                    correction = true;
                    break;
                }
            if (!correction && w.size() > 1 && w[0] == L"no")
            {
                w.erase(w.begin());
                correction = weakNo = true;
            }
            while (correction && !w.empty() && (w[0] == L"actually" || w[0] == L"no" || w[0] == L"sorry"))
                w.erase(w.begin());
            // "actually, Three too / also / as well" (2026-10-07e) - "too" would otherwise read as "two"
            if (correction && w.size() >= 2)
            {
                if (w.back() == L"too" || w.back() == L"also")
                {
                    w.pop_back();
                    addToo = true;
                }
                else if (w.size() >= 3 && w[w.size() - 2] == L"as" && w.back() == L"well")
                {
                    w.resize(w.size() - 2);
                    addToo = true;
                }
            }
        }
    }
    const auto names = PilotNameWords(); // "Sledge, hold position"
    // A pilot's name at w[at]: its lance number and how many words it took, or -1. A slightly
    // misheard name still counts (each word nearly right, like "Sledg" for "Sledge").
    auto nameAt = [&](size_t at, size_t &len) -> int {
        for (auto &nm : names)
        {
            if (at + nm.first.size() > w.size())
                continue;
            bool ok = true;
            for (size_t k = 0; k < nm.first.size() && ok; ++k)
                ok = w[at + k] == nm.first[k] || (nm.first[k].size() >= 4 && WordSim(w[at + k], nm.first[k]) >= 0.75) ||
                     (nm.first.size() == 1 && NamePrefix(w[at + k], nm.first[k])); // "book" = Booker (2026-10-07i)
            if (ok)
            {
                len = nm.first.size();
                return nm.second;
            }
        }
        return -1;
    };
    std::vector<int> who;
    size_t i = 0;
    size_t lastWhoAt = (size_t)-1; // where the last plain callsign word was (GIVE BACK below)
    for (; i < w.size(); ++i)
    {
        if (w[i] == L"and")
            continue;
        size_t len = 0;
        const int byName = nameAt(i, len);
        if (byName >= 0)
        {
            who.push_back(byName);
            i += len - 1;
            continue;
        }
        int s = WhoWord(w[i]);
        // "Actually, 400" / "two hundred metres north": a number, not a callsign (2026-10-11)
        if (s >= 1 && i + 1 < w.size() &&
            (w[i + 1] == L"hundred" || w[i + 1] == L"thousand" || w[i + 1] == L"metres" || w[i + 1] == L"k" || w[i + 1] == L"klicks" ||
             w[i + 1] == L"kilometres" || w[i + 1] == L"oclock" || w[i + 1] == L"point"))
            break;
        // "Form, move ..." - Whisper's "four" at the very start (not "form on me" / "form up")
        if (s < 0 && i == 0 && w[i] == L"form" && (w.size() < 2 || (w[1] != L"on" && w[1] != L"up")))
            s = 4;
        if (s < 0)
            break;
        // "all" words that are really part of an order ("all weapons") are left to the order.
        if (s == 0 && i + 1 < w.size() && w[i + 1] == L"weapons" && (i + 2 >= w.size() || w[i + 2] != L"free"))
            break;
        who.push_back(s);
        lastWhoAt = i;
    }
    // GIVE BACK (2026-10-15, user): "Lance, to my position" / "Lance, all guns" - the order's first word is also a callsign
    // word ("to" = Two, "all" = the lance), so it was taken as a second callsign and the rest made no sense. When the rest
    // is not an order but the word plus the rest is, the word goes back to the order. Only a sound-alike ("to", "too",
    // "tu", "for", "fore", "won") or a second group word after another callsign; only on the failure path.
    if (who.size() >= 2 && i < w.size() && lastWhoAt + 1 == i)
    {
        static const wchar_t *kSoundAlike[] = {L"to", L"too", L"tu", L"for", L"fore", L"won"};
        const std::wstring &lw = w[lastWhoAt];
        bool cand = who.back() == 0; // "all" / "everyone" / "lance" after another callsign
        for (auto *k : kSoundAlike)
            cand |= lw == k;
        if (cand)
        {
            std::vector<Cmd> probe;
            std::wstring pd;
            std::vector<std::wstring> rest(w.begin() + (long)i, w.end());
            if (!ParseOrder(rest, probe, pd))
            {
                rest.insert(rest.begin(), lw);
                probe.clear();
                if (ParseOrder(rest, probe, pd))
                {
                    who.pop_back();
                    i = lastWhoAt;
                }
            }
        }
    }
    std::wstring verdict;
    wchar_t sc[64] = L"";
    std::vector<std::wstring> body(w.begin() + (long)std::min(i, w.size()), w.end());
    // The callsign at the END ("status report, lance"): only the plain words and pilot names there
    // (not the "to" / "for" / "land" look-alikes, which end ordinary sentences).
    if (who.empty() && !w.empty())
    {
        static const struct
        {
            const wchar_t *W;
            int            S;
        } kEnd[] = {{L"lance", 0}, {L"all", 0}, {L"everyone", 0}, {L"one", 1}, {L"two", 2}, {L"three", 3}, {L"four", 4},
                    {L"five", 5},  {L"six", 6}, {L"seven", 7},    {L"eight", 8}};
        int s = -1;
        size_t len = 1;
        for (auto &k : kEnd)
            if (w.back() == k.W)
                s = k.S;
        // "where is the Atlas three" is the Atlas III, not "..., Three" (2026-10-11, round trip): a number word at the end
        // that completes a known numbered unit name stays part of the name
        if (s >= 1 && w.size() >= 2)
        {
            std::vector<std::wstring> known;
            {
                std::lock_guard<std::mutex> lk(g_namesMx);
                known = g_knownNames;
            }
            for (auto &pt : NamePatterns(known))
            {
                if (pt.Base || pt.Tok.empty() || !IsNumeralWord(pt.Tok.back()))
                    continue;
                for (size_t L = 2; L <= std::min<size_t>(6, w.size()) && s >= 0; ++L)
                    if (MatchNameAt(w, nullptr, w.size() - L, pt.Tok) == L)
                        s = -1;
                if (s < 0)
                    break;
            }
        }
        for (auto &nm : names)
            if (s < 0 && nm.first.size() <= w.size() && std::equal(nm.first.begin(), nm.first.end(), w.end() - (long)nm.first.size()))
            {
                s = nm.second;
                len = nm.first.size();
            }
        if (s >= 0 && w.size() > len)
        {
            who.push_back(s);
            body.assign(w.begin(), w.end() - (long)len);
        }
    }
    // "Two, wait ..." - a correction for the one named (2026-10-07b); "two, wait here" is a hold.
    if (!correction && !body.empty() && body[0] == L"wait" && !(body.size() > 1 && (body[1] == L"here" || body[1] == L"there")))
    {
        const ULONGLONG om = g_offerMs;
        if (!(om && GetTickCount64() - om < 20000 && w.size() <= 3))
        {
            body.erase(body.begin());
            waitOnly = true;
            while (!body.empty() && (body[0] == L"actually" || body[0] == L"no" || body[0] == L"wait"))
            {
                waitOnly = waitOnly && body[0] == L"wait";
                body.erase(body.begin());
            }
            correction = true;
        }
    }
    // "Two, belay that" / "Three, scratch that" / "Two, as you were" / "Four, actually, 400": a correction for the one
    // named (2026-10-11 - the lead words only counted at the very start, before the callsign).
    if (!correction && !body.empty())
    {
        static const std::vector<std::vector<std::wstring>> kNamedLead = {
            {L"belay", L"that"},  {L"belay", L"last"},  {L"scratch", L"that"},     {L"as", L"you", L"were"}, {L"never", L"mind"},
            {L"nevermind"},       {L"disregard", L"that"}, {L"disregard", L"last"}, {L"disregard"},           {L"belay"},
            {L"correction"},      {L"actually"}};
        for (auto &k : kNamedLead)
            if (k.size() <= body.size() && std::equal(k.begin(), k.end(), body.begin()))
            {
                cancelWord = k[0] != L"correction" && k[0] != L"actually";
                body.erase(body.begin(), body.begin() + (long)k.size());
                while (!body.empty() && (body[0] == L"actually" || body[0] == L"no" || body[0] == L"sorry"))
                    body.erase(body.begin());
                correction = true;
                break;
            }
    }
    // HIGH GROUND (2026-10-14): the answer to "which hill, Commander?" (30 s).
    {
        const ULONGLONG hm = g_hillMs;
        if (hm && GetTickCount64() - hm < 31000)
        {
            const int code = HillAnswerCode(body.empty() ? w : body);
            if (code != -1)
            {
                SpokenAction a;
                a.Query = code == -2 ? Q_HillCancel : Q_HillPick;
                a.QueryName = std::to_wstring(code);
                a.C.Who = -1; // the offer knows who goes; "the highest ONE" must not read as a question to One (2026-10-14 test)
                {
                    std::lock_guard<std::mutex> lk(g_amx);
                    g_actions.push_back(a);
                }
                const std::wstring v = code == -2 ? L"-> no hill (marks cleared)" : L"-> hill pick " + std::to_wstring(code);
                Log(L"whisper heard \"%ls\" [%ls] %ls (%.1f s)", text.c_str(), JoinWords(w).c_str(), v.c_str(), secs);
                HeardLog(L"\"" + text + L"\"  [" + JoinWords(w) + L"]  " + v);
                return;
            }
        }
    }
    // PATCH-THROUGH (build 2026-10-06c): the answer to a pilot's "want me to patch it through?" (15 s).
    {
        const ULONGLONG om = g_offerMs;
        if (om && GetTickCount64() - om < 20000) // 20 s (user, 2026-10-06e)
        {
            std::vector<std::wstring> b = body.empty() ? w : body;
            while (!b.empty() && (b.back() == L"commander" || b.back() == L"please" || b.back() == L"over" || b.back() == L"now"))
                b.pop_back();
            const std::wstring t = JoinWords(b);
            static const wchar_t *kYes[] = {L"yes", L"yeah", L"yep", L"yup", L"affirmative", L"affirm", L"do it", L"patch it through",
                                            L"patch it", L"patch through", L"patch it in", L"send it", L"send it through",
                                            L"send it over", L"go ahead", L"roger", L"roger that", L"sure", L"lock it", L"lock it up",
                                            L"give it to me", L"put it up", L"mark it", L"please do", L"yes please", L"paint it",
                                            L"light it up", L"yes do it", L"yeah do it", L"yes patch it through", L"do that"};
            static const wchar_t *kNo[] = {L"no", L"nope", L"negative", L"dont", L"do not", L"never mind", L"nevermind",
                                           L"belay that", L"cancel", L"no thanks", L"not now", L"hold off", L"skip it", L"forget it",
                                           L"dont bother", L"negative on that"};
            int q = 0;
            for (auto *k : kYes)
                if (t == k)
                    q = Q_PatchYes;
            for (auto *k : kNo)
                if (t == k)
                    q = Q_PatchNo;
            if (q)
            {
                SpokenAction a;
                a.Query = q;
                a.C.Who = who.empty() ? -1 : who[0];
                {
                    std::lock_guard<std::mutex> lk(g_amx);
                    g_actions.push_back(a);
                }
                const std::wstring v = q == Q_PatchYes ? L"-> patch it through" : L"-> don't patch it through";
                Log(L"whisper heard \"%ls\" [%ls] %ls (%.1f s)", text.c_str(), JoinWords(w).c_str(), v.c_str(), secs);
                HeardLog(L"\"" + text + L"\"  [" + JoinWords(w) + L"]  " + v);
                return;
            }
        }
    }
    // PATCH-THROUGH COMMAND (2026-10-06g)
    {
        std::wstring pname;
        if (PatchCommand(body.empty() ? w : body, pname))
        {
            SpokenAction a;
            a.Query = Q_PatchCmd;
            a.QueryName = pname;
            a.C.Who = who.empty() ? -1 : who[0];
            {
                std::lock_guard<std::mutex> lk(g_amx);
                g_actions.push_back(a);
            }
            const std::wstring v = L"-> patch through: " + (pname.empty() ? std::wstring(L"the unit last reported") : pname);
            Log(L"whisper heard \"%ls\" [%ls] %ls (%.1f s)", text.c_str(), JoinWords(w).c_str(), v.c_str(), secs);
            HeardLog(L"\"" + text + L"\"  [" + JoinWords(w) + L"]  " + v);
            return;
        }
    }
    // QUESTIONS: answered on the main thread (they read the game), no callsign needed.
    {
        std::wstring qname;
        {
            // "How's Two?" / "Two, how are you?" - that lancemate's own status report, as "Two, status" (2026-10-11)
            const std::wstring bt0 = JoinWords(body);
            if (!who.empty() && who[0] >= 1 &&
                (bt0 == L"hows" || bt0 == L"how is" || bt0 == L"how are you" || bt0 == L"how are you doing" || bt0 == L"hows it going" ||
                 bt0 == L"how you doing" || bt0 == L"hows it look" || bt0 == L"hows it looking"))
                body = SplitWords(L"status");
        }
        int q = ParseQuestion(body, qname);
        {
            // HIGH GROUND (2026-10-14 test): "Four, find some high ground" read as "where is <some high ground>"
            std::vector<Cmd> hp;
            std::wstring hd;
            if (q && ParseOrder(body, hp, hd) &&
                std::any_of(hp.begin(), hp.end(), [](const Cmd &c) { return c.Type == C_Hill || c.Type == C_AttackClass; }))
                q = 0;
        }
        if (q == Q_UnitHow && qname.find(L' ') == std::wstring::npos && WhoWord(qname) >= 1) // "how's Two doing?"
        {
            who.assign(1, WhoWord(qname));
            body = SplitWords(L"status");
            q = 0;
        }
        if (q)
        {
            // 2026-10-15 (user, correctness check): "Two and Three, report in" only reached Two. A question about the
            // pilot himself (report in, how bad, where are you, target, ammo, heat, who's shooting you) goes to EACH
            // pilot named; questions about the battlefield stay one answer. "Lance" / "all" among them = everyone.
            std::vector<int> qWho;
            const bool perPilot = q == Q_SoundOff || q == Q_HowBad || q == Q_WhereYou || q == Q_Target || q == Q_Ammo ||
                                  q == Q_Heat || q == Q_WhoHits;
            if (who.empty())
                qWho.push_back(-1);
            else if (!perPilot)
                qWho.push_back(who[0]); // as before
            else if (std::find(who.begin(), who.end(), 0) != who.end())
                qWho.push_back(0);
            else
                for (int ws : who)
                    if (std::find(qWho.begin(), qWho.end(), ws) == qWho.end())
                        qWho.push_back(ws);
            for (int ws : qWho)
            {
                SpokenAction a;
                a.Query = q;
                a.QueryName = qname;
                a.C.Who = ws;
                std::lock_guard<std::mutex> lk(g_amx);
                g_actions.push_back(a);
            }
            static const wchar_t *kQ[] = {L"", L"where", L"where is the named unit", L"where are you", L"what is it", L"how bad",
                                          L"how many", L"what are we facing", L"what's closest", L"who's shooting at you",
                                          L"what's your target", L"can you see it", L"is it down", L"ammo", L"heat", L"anyone hurt",
                                          L"sound off", L"how's that unit", L"any air", L"who's closest to it", L"say again",
                                          L"who's firing on you", L"patch it through", L"don't patch it through",
                                          L"patch a unit through", L"any of a kind", L"which way is it facing", L"radio check",
                                          L"pick a hill", L"no hill"};
            // 2026-10-13: bounds-checked - a new question kind without a name here read past the table (radio check hung)
            const wchar_t *qn = q >= 0 && q < (int)(sizeof(kQ) / sizeof(kQ[0])) ? kQ[q] : L"(unnamed question)";
            const std::wstring verdictQ = L"-> question: " + std::wstring(qn) + (qname.empty() ? L"" : L" (" + qname + L")") +
                                          (who.empty() ? L", to whoever reported last" : L"");
            Log(L"whisper heard \"%ls\" [%ls] %ls (%.1f s)", text.c_str(), JoinWords(w).c_str(), verdictQ.c_str(), secs);
            wchar_t tq[32];
            swprintf_s(tq, L"  (heard in %.2f s)", secs);
            HeardLog(L"\"" + text + L"\"  [" + JoinWords(w) + L"]  " + verdictQ + tq);
            return;
        }
    }
    // LANCE-WIDE ORDERS with no callsign (2026-10-07i): "focus fire on the Victor", "regroup", "rally" go to the whole
    // lance ("everyone / everybody on my target" and "let's get out of here" already start with a lance word).
    // Retired 2026-10-16 (user: NO CALLSIGN, NEVER THE LANCE) - "focus / regroup / rally" need a callsign too.
    if (false && who.empty() && !correction && !body.empty() &&
        (body[0] == L"focus" || body[0] == L"regroup" || body[0] == L"rally"))
        who.push_back(0);
    // No callsign at all, but an order to someone a few seconds ago ("four, move five hundred west"
    // ... "then hold position"): the same lancemate(s) - only if it parses as an order (below).
    // "Never mind, go to nav Charlie" / "wait, go to nav Charlie" with NOTHING to correct (2026-10-14, user): no correction -
    // the order itself, handled like any order said without the "never mind" (it was dropped as "nothing to correct").
    if (correction && who.empty() && (g_corrWho.empty() || GetTickCount64() - g_corrWhoMs >= 120000))
    {
        std::vector<std::wstring> b = body;
        while (!b.empty() && (b[0] == L"then" || b[0] == L"and" || b[0] == L"make" || b[0] == L"it" || b[0] == L"that" ||
                              b[0] == L"i" || b[0] == L"mean" || b[0] == L"meant" || b[0] == L"instead"))
            b.erase(b.begin());
        std::vector<Cmd> probe;
        std::wstring pd;
        const std::wstring keepName = g_parsedName;
        const bool isOrder = !b.empty() && ParseOrder(b, probe, pd);
        g_parsedName = keepName;
        if (isOrder)
        {
            correction = false;
            body = b;
        }
    }
    // CORRECTIONS (2026-10-07a): for whoever got your last order (or the callsign said), replacing it.
    if (correction)
    {
        std::vector<int> cwho = who;
        if (cwho.empty() && !g_corrWho.empty() && GetTickCount64() - g_corrWhoMs < 120000)
            cwho = g_corrWho; // (2026-10-07d: not a status request's)
        std::vector<std::wstring> b = body;
        while (!b.empty() && (b[0] == L"then" || b[0] == L"and" || b[0] == L"make" || b[0] == L"it" || b[0] == L"that" ||
                              b[0] == L"i" || b[0] == L"mean" || b[0] == L"meant" || b[0] == L"instead"))
            b.erase(b.begin());
        {
            const std::wstring t = JoinWords(b); // short forms, as for any order
            // 2026-10-14 (user): a bare "follow" is no order any more (it was "follow me")
            if (t == L"hold" || t == L"stay")
                b = SplitWords(L"hold position");
        }
        std::wstring v;
        std::vector<Cmd> parsed;
        std::wstring what;
        bool ok = false, partial = false, cancel = false, handOver = false, fromLast = false;
        if (cwho.empty())
            v = L"a correction, but no recent order to correct";
        else
        {
            // "wait, you ..." / "wait, I ..." is a remark, not a correction (2026-10-07i: "Wait, you shoot in two")
            const bool remark = waitOnly && !b.empty() && (b[0] == L"you" || b[0] == L"i" || b[0] == L"we" || b[0] == L"they" ||
                                                           b[0] == L"he" || b[0] == L"she" || b[0] == L"who" || b[0] == L"youre");
            ok = !remark && !b.empty() && ParseOrder(b, parsed, what);
            // 2026-10-07d: a pilot named -> that pilot's own last orders; else the last orders given.
            const std::vector<Cmd> &lastOf = (!who.empty() && who[0] >= 1 && who[0] <= 8) ? g_lastOrdersBy[who[0]] : g_lastOrders;
            const Cmd *lastMove = nullptr;
            for (auto &lc : lastOf)
                if (lc.Type == C_Move || lc.Type == C_FallMove)
                    lastMove = &lc;
            if (ok && lastMove && parsed.size() == 1 && parsed[0].Type == C_Move && parsed[0].C == 0)
            {
                parsed[0].Type = lastMove->Type; // "actually, 300 east": the same kind of move as before
                parsed[0].C = lastMove->C == 4 ? 0 : lastMove->C; // (and hold / indirect at the point, as before; a run -> a move)
            }
            if (!ok && !remark && lastMove && !b.empty())
            {
                const int d = SpokenNumber(b), dir = SpokenDirection(b);
                if (d > 0 || dir >= 0) // "actually, 300" / "wait no, east": the last move, that part changed
                {
                    Cmd c = *lastMove;
                    if (d > 0)
                    {
                        c.A = std::max(25, std::min(2000, d));
                        c.D = d > 2000 ? 1 : 0; // MOVE CAP (2026-10-15, user; braces fixed 2026-10-16)
                    }
                    if (dir >= 0)
                        c.B = dir;
                    parsed.assign(1, c);
                    what = L"last move corrected";
                    ok = partial = true;
                }
            }
            // "actually, Three" (2026-10-07e): only a callsign - the last order goes to the one named INSTEAD (the
            // ones who had it stand down); "actually, Three too / as well": to them as well.
            if (!ok && b.empty() && !who.empty() && !cancelWord && !waitOnly && !g_lastOrders.empty() && !g_corrWho.empty() &&
                GetTickCount64() - g_corrWhoMs < 120000 && g_corrWho != who)
            {
                parsed = g_lastOrders;
                g_parsedName = g_lastOrdersName; // bug sweep 17: the Atlas of "Two, attack the Atlas", not a later probe's
                ok = true;
                fromLast = true;
                handOver = !addToo;
                what = addToo ? L"the last order, as well" : L"the last order, instead";
            }
            if (!ok && b.empty() && !weakNo && !waitOnly)
                cancel = true; // "belay that", "as you were", "never mind"
            if (!ok && !cancel)
                v = remark ? L"\"wait, you ...\" - a remark, not a correction" : (b.empty() && waitOnly) ? L"a bare \"wait\" - ignored (\"belay that\" / \"never mind\" cancel)"
                                            : L"a correction, but no order in it";
        }
        if (ok || cancel)
        {
            const bool all = std::find(cwho.begin(), cwho.end(), 0) != cwho.end();
            std::wstring names;
            std::vector<int> done;
            std::lock_guard<std::mutex> lk(g_amx);
            std::vector<int> from; // a hand-over: who had the order
            if (fromLast)
                for (int s2 : g_corrWho)
                    if (std::find(cwho.begin(), cwho.end(), s2) == cwho.end())
                        from.push_back(s2);
            if (handOver)
                for (int s2 : from)
                {
                    SpokenAction a; // they stand down first (fire settings put back, a move called off)
                    a.Cancel = true;
                    a.C.Who = s2;
                    g_actions.push_back(a);
                }
            const bool asWell = fromLast && addToo;
            for (int s2 : cwho)
            {
                const int slot = all ? 0 : s2;
                if (std::find(done.begin(), done.end(), slot) != done.end())
                    continue;
                done.push_back(slot);
                names += (names.empty() ? L"" : L" + ") + std::wstring(slot ? kNum[slot] : L"all");
                if (cancel)
                {
                    SpokenAction a;
                    a.Cancel = true;
                    a.C.Who = slot;
                    g_actions.push_back(a);
                    continue;
                }
                for (size_t k = 0; k < parsed.size(); ++k)
                {
                    SpokenAction a;
                    a.C = parsed[k];
                    a.C.Who = slot;
                    a.Correct = !asWell; // "as well": a plain order, nothing of theirs is replaced
                    a.Partial = partial;
                    if (parsed[k].Type == C_AttackNamed || parsed[k].Type == C_Shadow || parsed[k].Type == C_CalledShot ||
                        parsed[k].Type == C_Flank || parsed[k].Type == C_Pincer)
                        a.QueryName = g_parsedName;
                    PincerSlot(a.C, cwho, slot);
                    g_actions.push_back(a);
                }
            }
            v = L"-> correction for " + names + L": " + (cancel ? std::wstring(L"belay the last order") : what);
            if (handOver)
            {
                std::wstring fn;
                for (int s2 : from)
                    fn += (fn.empty() ? L"" : L" + ") + std::wstring(s2 ? kNum[s2] : L"all");
                v = L"-> correction: the last order goes to " + names + L" instead of " + fn;
            }
            else if (asWell)
                v = L"-> the last order to " + names + L" as well";
            if (asWell)
            {
                std::vector<int> both = from;
                both.insert(both.end(), cwho.begin(), cwho.end());
                RememberOrders(both, parsed, g_parsedName);
            }
            else if (ok)
                RememberOrders(cwho, parsed, g_parsedName);
            else
            {
                g_lastWho = cwho; // a cancel: the next order with no callsign still goes to them
                g_lastWhoMs = GetTickCount64();
            }
        }
        Log(L"whisper heard \"%ls\" [%ls] %ls (%.1f s)", text.c_str(), JoinWords(w).c_str(), v.c_str(), secs);
        wchar_t tc[32];
        swprintf_s(tc, L"  (heard in %.2f s)", secs);
        HeardLog(L"\"" + text + L"\"  [" + JoinWords(w) + L"]  " + v + tc);
        return;
    }
    bool fromContext = false;
    // A sentence that starts like a question is never turned into an order (2026-10-06g: "where's the closest
    // contact? can you patch it in?" became "attack the nearest" for the last pilot who answered).
    bool questionLike = false;
    {
        // "Three, what's your status?" is the status order, not a question (2026-10-10: it did nothing)
        const std::wstring bt = JoinWords(body);
        if (bt == L"whats status" || bt == L"what is status" || bt == L"whats the status" || bt == L"what is the status" ||
            bt == L"hows status" || bt == L"how is status")
            body = SplitWords(L"status");
    }
    if (!body.empty())
        for (auto *qw : {L"where", L"wheres", L"what", L"whats", L"who", L"whos", L"which", L"how", L"hows", L"why"})
            questionLike |= body[0] == qw;
    // No callsign, and the last thing that happened was one pilot answering your question (build
    // 2026-10-06a): the order is for that pilot (within 60 s; an order to someone since wins).
    {
        const int la = g_lastAnswerer;
        const ULONGLONG lam = g_lastAnswerMs;
        if (!questionLike && who.empty() && !(g_cfg.Listen == 2 && pttHeld) && la > 0 && GetTickCount64() - lam < 60000 && lam > g_lastWhoMs)
        {
            std::vector<Cmd> probe;
            std::wstring pd;
            std::vector<std::wstring> b = body;
            while (!b.empty() && (b[0] == L"then" || b[0] == L"and" || b[0] == L"also" || b[0] == L"ok" || b[0] == L"okay"))
                b.erase(b.begin());
            if (!b.empty() && ParseOrder(b, probe, pd))
            {
                who.push_back(la);
                body = b;
                fromContext = true;
            }
        }
    }
    // NO CALLSIGN, NEVER THE LANCE (2026-10-16, user): a follow-up only carries on to single pilots, not to an order
    // that went to the whole lance ("Not two, stay put" went to all this way).
    if (!questionLike && who.empty() && !(g_cfg.Listen == 2 && pttHeld) && GetTickCount64() - g_lastWhoMs < 10000 && !g_lastWho.empty() &&
        std::find(g_lastWho.begin(), g_lastWho.end(), 0) == g_lastWho.end())
    {
        std::vector<Cmd> probe;
        std::wstring pd;
        std::vector<std::wstring> b = body;
        while (!b.empty() && (b[0] == L"then" || b[0] == L"and" || b[0] == L"also"))
            b.erase(b.begin());
        if (!b.empty() && ParseOrder(b, probe, pd))
        {
            who = g_lastWho;
            body = b;
            fromContext = true;
        }
    }
    // "shadow it" / "get eyes on it" with no callsign: whoever last reported a unit (2 min).
    if (!questionLike && who.empty() && g_lastReporter > 0 && GetTickCount64() - g_lastReporterMs < 120000)
    {
        std::vector<Cmd> probe;
        std::wstring pd;
        if (ParseOrder(body, probe, pd) && !probe.empty() &&
            std::all_of(probe.begin(), probe.end(), [](const Cmd &c) { return c.Type == C_Shadow; }))
        {
            who.push_back(g_lastReporter);
            fromContext = true;
        }
    }
    if (who.empty())
    {
        // NO CALLSIGN, NEVER THE LANCE (2026-10-16, user; was 2026-10-10: no callsign = the whole lance). An order
        // with no callsign and no context is not sent to anyone; the log says it was an order without a callsign.
        std::vector<Cmd> probe;
        std::wstring pd;
        if (!questionLike && !body.empty() && ParseOrder(body, probe, pd))
            verdict = L"an order with no callsign - not sent (start with all / lance / two / three / four or a name)";
        else
            verdict = L"no callsign - not an order (start with all / lance / two / three / four)";
    }
    if (verdict.empty() && body.empty())
        verdict = L"a callsign but no order";
    if (verdict.empty() && questionLike)
        verdict = L"sounds like a question I don't know - not taken as an order";
    std::vector<std::wstring> tail = body;
    {
        // Short forms of whole orders.
        static const struct
        {
            const wchar_t *From, *To;
        } kAlias[] = {{L"hold", L"hold position"}, {L"stay", L"hold here"},
                      {L"stay here", L"hold here"}, {L"stay there", L"hold here"}, {L"wait here", L"hold here"},
                      {L"wait there", L"hold here"}, {L"attack", L"attack my target"},
                      {L"engage", L"engage my target"}, {L"attack target", L"attack my target"},
                      {L"engage target", L"engage my target"}, {L"indirect", L"indirect fire only"},
                      {L"indirect fire", L"indirect fire only"}, {L"alpha", L"alpha strike"}, {L"reset", L"reset fire orders"},
                      {L"to my position", L"form on me"}, {L"all guns", L"all weapons"},                // 2026-10-14 full audit (user)
                      {L"target of opportunity", L"engage at will"}}; // ("targets" arrives as "target")
        const std::wstring t = JoinWords(tail);
        for (auto &k : kAlias)
            if (t == k.From)
                tail = SplitWords(k.To);
    }
    // The order parser first: the parts of an order, in any word order.
    if (verdict.empty())
    {
        std::vector<Cmd> parsed;
        std::wstring what;
        const bool parsedOk = ParseOrder(tail, parsed, what);
        if (parsedOk && !g_attackUnheard.empty() &&
            std::any_of(parsed.begin(), parsed.end(), [](const Cmd &c) { return c.Type == C_Attack; }))
        {
            // 2026-10-10 (user): an attack on a name we did not catch - the pilot(s) ask to say again
            const bool all = std::find(who.begin(), who.end(), 0) != who.end();
            verdict = L"attack on a unit I don't know (\"" + g_attackUnheard + L"\") - asked to say again";
            SpokenAction a;
            a.SayAgain = true;
            a.C.Who = all ? 0 : who[0];
            std::lock_guard<std::mutex> lk(g_amx);
            g_actions.push_back(a);
        }
        else if (parsedOk)
        {
            const bool all = std::find(who.begin(), who.end(), 0) != who.end();
            std::wstring names;
            std::vector<int> done;
            std::lock_guard<std::mutex> lk(g_amx);
            for (int s : who)
            {
                const int slot = all ? 0 : s;
                if (std::find(done.begin(), done.end(), slot) != done.end())
                    continue;
                done.push_back(slot);
                names += (names.empty() ? L"" : L" + ") + std::wstring(slot ? kNum[slot] : L"all");
                for (const Cmd &pc : parsed)
                {
                    SpokenAction a;
                    a.C = pc;
                    a.C.Who = slot;
                    if (pc.Type == C_AttackNamed || pc.Type == C_Shadow || pc.Type == C_CalledShot || pc.Type == C_Flank || pc.Type == C_Pincer)
                        a.QueryName = g_parsedName;
                    PincerSlot(a.C, who, slot);
                    g_actions.push_back(a);
                }
            }
            verdict = L"-> " + names + L": " + what + L" (order parser" + (fromContext ? L", same lancemate as before)" : L")");
            RememberOrders(who, parsed, g_parsedName); // CORRECTIONS
        }
    }
    if (verdict.empty())
    {
        double best = -1.0, other = -1.0;
        const OrderWords *top = nullptr;
        for (auto &o : g_orders)
        {
            const double s = OrderScore(tail, 0, o.W);
            if (s > best)
            {
                if (top && !SameOrder(top->C, o.C))
                    other = std::max(other, best);
                best = s;
                top = &o;
            }
            else if (top && !SameOrder(top->C, o.C) && s > other)
                other = s;
        }
        swprintf_s(sc, L" (match %.2f, next %.2f)", best, other < 0 ? 0.0 : other);
        const bool all = std::find(who.begin(), who.end(), 0) != who.end();
        if (top && ((best >= 0.999 && other < 0.999) || (best >= 0.8 && best - other >= 0.1)))
        {
            std::wstring names;
            std::lock_guard<std::mutex> lk(g_amx);
            std::vector<int> done;
            for (int s : who)
            {
                const int slot = all ? 0 : s;
                if (std::find(done.begin(), done.end(), slot) != done.end())
                    continue;
                done.push_back(slot);
                names += (names.empty() ? L"" : L" + ") + std::wstring(slot ? kNum[slot] : L"all");
                SpokenAction a;
                a.C = top->C;
                a.C.Who = slot;
                g_actions.push_back(a);
            }
            verdict = L"-> " + names + L": " + JoinWords(top->W);
            RememberOrders(who, std::vector<Cmd>(1, top->C), std::wstring()); // CORRECTIONS
        }
        else if (top && best >= 0.5)
        {
            verdict = L"unsure between orders - asked to say again (closest: " + JoinWords(top->W) + L")";
            SpokenAction a;
            a.SayAgain = true;
            a.C.Who = all ? 0 : who[0];
            std::lock_guard<std::mutex> lk(g_amx);
            g_actions.push_back(a);
        }
        else
            verdict = L"no order sounds like that";
    }
    Log(L"whisper heard \"%ls\" [%ls] %ls%ls (%.1f s)", text.c_str(), JoinWords(w).c_str(), verdict.c_str(), sc, secs);
    wchar_t tt[32];
    swprintf_s(tt, L"  (heard in %.2f s)", secs);
    HeardLog(L"\"" + text + L"\"  [" + JoinWords(w) + L"]  " + verdict + sc + tt);
}

void AnswerQuestion(int q, int who, const std::wstring &name); // QUESTIONS, below

// Called by the main loop: carries out what Whisper's thread queued.
void RunSpokenActions()
{
    std::vector<SpokenAction> a;
    {
        std::lock_guard<std::mutex> lk(g_amx);
        a.swap(g_actions);
    }
    if (a.empty())
        return;
    // You spoke: the answers and replies go ahead of the chatter, and the first of them cuts off
    // whoever is talking - your transmission stepped on them; they pick up again afterwards if it is
    // still worth saying (build 2026-10-06a).
    g_sayPriority = true;
    g_cutArmed = true;
    // CORRECTIONS (2026-10-07a): before the new batch - whatever the last batch changed in the fire settings of
    // those corrected is put back (not for a partial one: only its move changes), and their replies to it that
    // are not on the air yet are dropped.
    const uint32_t lastBatch = g_orderBatch;
    bool orders = false;
    auto slotsOf = [](int who) {
        std::vector<int> out;
        VanillaExportBlock::TacMember m[VanillaExportBlock::kTacMaxMembers] = {};
        const int n = g_blk ? ReadLance(m) : 0;
        for (int i = 0; i < n; ++i)
            if (m[i].Valid && !m[i].IsPlayer && !m[i].Dead && m[i].Slot >= 1 && m[i].Slot <= 8 && (who == 0 || m[i].Slot == who))
                out.push_back(m[i].Slot);
        return out;
    };
    for (auto &x : a)
    {
        if (x.Query || x.SayAgain)
            continue;
        if (x.Cancel || x.C.Type != C_Status)
            orders = true; // (2026-10-07e: a status request is not a batch a correction could mean)
        if (!(x.Correct || x.Cancel))
            continue;
        for (int s : slotsOf(x.C.Who))
        {
            RadioDropReplies(s);
            if (x.Partial || !lastBatch || g_firePrev[s].Batch != lastBatch || g_fireBaseOn[s])
                continue;
            const FirePrev &fp = g_firePrev[s];
            const float v[3] = {(float)fp.Disc, (float)fp.Mask, (float)fp.Heat};
            Send(VanillaExportBlock::TCO_FireSettings, s, v);
            g_fireBaseOn[s] = true;
            g_fireBase[s][0] = fp.Disc;
            g_fireBase[s][1] = fp.Mask;
            g_fireBase[s][2] = fp.Heat;
            Log(L"correction: %ls's fire settings put back", Callsign(s).c_str());
        }
    }
    if (orders)
        ++g_orderBatch;
    for (auto &x : a)
    {
        if (x.Cancel)
        {
            // "belay that": a move / hold / attack from the last batch is called off (back on you); fire settings were put back above
            const std::vector<int> ss = slotsOf(x.C.Who);
            for (int s : ss)
                if (g_blk && lastBatch && g_moveBatch[s] == lastBatch)
                    Send(VanillaExportBlock::TCO_Follow, s, nullptr);
            if (!ss.empty())
                Say(ss[0], Line(ss.size() > 1 ? L"reply_cancel_lance" : L"reply_cancel", ss[0]));
            continue;
        }
        if (!x.ReplyGroup.empty())
            Say(x.C.Who ? x.C.Who : 2, Line(x.ReplyGroup.c_str(), x.C.Who ? x.C.Who : 2));
        else if (x.SayAgain)
            Say(x.C.Who, Line(L"say_again", x.C.Who));
        else if (x.Query)
        {
            std::vector<int> got;
            g_sayCollect = &got;
            AnswerQuestion(x.Query, x.C.Who, x.QueryName);
            g_sayCollect = nullptr;
            std::sort(got.begin(), got.end());
            got.erase(std::unique(got.begin(), got.end()), got.end());
            got.erase(std::remove(got.begin(), got.end(), 0), got.end());
            if (got.size() == 1) // one pilot answered: an order with no callsign goes to it
            {
                g_lastAnswerer = got[0];
                g_lastAnswerMs = GetTickCount64();
            }
        }
        else if (!g_blk)
            Say(0, Line(L"problem_no_link", 0));
        else
        {
            g_execName = x.QueryName;
            g_execCorrection = x.Correct;
            Execute(x.C);
            g_execCorrection = false;
        }
    }
    g_sayPriority = false;
    g_cutArmed = false;
    for (auto &f : g_fireBaseOn) // the feed catches up from here on
        f = false;
}

// ---- CALLOUTS (stage 2): lancemates speak up on their own -----------------------------------
// From the VanillaHud plugin's lance status feed and event list (MW5_LanceStatus, read only).
// Fixed lines, in each lancemate's own voice. Kept from turning into chatter:
//   - each kind of callout per lancemate has its own quiet time (kCall[].CoolMs);
//   - one line at a time: a new line is only started when the voice has finished the last;
//   - lines wait at most 4 s for their turn (stale news is dropped), most urgent first.
// callouts = 0 in VoiceCommand.ini switches them off.
HANDLE            g_lsmap = nullptr;
LanceStatusBlock *g_ls = nullptr;
uint32_t          g_evNext = 0; // the next event number (from 0) to read
bool              g_evSynced = false;

bool LsConnect()
{
    if (g_ls && g_ls->Version == LANCE_VERSION)
        return true;
    if (g_ls)
    {
        UnmapViewOfFile(g_ls);
        CloseHandle(g_lsmap);
        g_ls = nullptr;
        g_lsmap = nullptr;
    }
    g_lsmap = OpenFileMappingW(FILE_MAP_READ, FALSE, LANCE_MMF_NAME);
    if (!g_lsmap)
        return false;
    g_ls = static_cast<LanceStatusBlock *>(MapViewOfFile(g_lsmap, FILE_MAP_READ, 0, 0, sizeof(LanceStatusBlock)));
    if (!g_ls || g_ls->Version != LANCE_VERSION)
    {
        if (g_ls)
            UnmapViewOfFile(g_ls);
        CloseHandle(g_lsmap);
        g_ls = nullptr;
        g_lsmap = nullptr;
        return false;
    }
    g_evSynced = false; // start from now: no replay of what happened before we connected
    Log(L"connected to the lance status feed - callouts %ls", g_cfg.Callouts ? L"on" : L"off (switched off in GlobalSettings > VOICE COMMAND)");
    return true;
}

// 2026-10-16: a LIVE feed - LanceControl publishes TickMs ~4x/s only while you are in a mech and the world ticks.
// Ejected / dead / paused: the block stays mapped and Enabled, but frozen (everyone 'alive') - don't trust it.
static bool LsFresh(ULONGLONG maxAgeMs = 3000)
{
    if (!LsConnect() || !g_ls->Enabled)
        return false;
    const ULONGLONG t = g_ls->TickMs, now = GetTickCount64();
    return t && (t >= now || now - t <= maxAgeMs); // (t >= now: published since we read the clock)
}

// The status entries (seqlock: an even, unchanged StatusSeq around the copy).
int LsReadSlots(LsSlot *out, LsCondition *cond = nullptr, LsAmmoList *ammo = nullptr)
{
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_ls->StatusSeq;
        if (a & 1)
        {
            Sleep(1);
            continue;
        }
        MemoryBarrier();
        uint32_t n = g_ls->SlotCount;
        if (n > (uint32_t)kLsMaxSlots)
            n = kLsMaxSlots;
        std::memcpy(out, (const void *)g_ls->Slots, sizeof(LsSlot) * n);
        if (cond)
            std::memcpy(cond, (const void *)g_ls->Cond, sizeof(LsCondition) * n);
        if (ammo)
            std::memcpy(ammo, (const void *)g_ls->Ammo, sizeof(LsAmmoList) * n);
        MemoryBarrier();
        if (g_ls->StatusSeq == a)
            return (int)n;
    }
    return 0;
}

// v6 (lancecontrol-17): the game's own pilot name in a lance number (its V-comms panel), "" unknown.
// It stays with the PILOT when pilots change mechs.
std::wstring LsPilotName(int slot)
{
    if (slot < 1 || slot > kLsMaxSlots || !LsConnect())
        return L"";
    LsPilot p = {};
    bool ok = false;
    for (int tries = 0; tries < 8 && !ok; ++tries)
    {
        const uint32_t a = g_ls->StatusSeq;
        if (a & 1)
        {
            Sleep(1);
            continue;
        }
        MemoryBarrier();
        std::memcpy(&p, (const void *)&g_ls->Pilots[slot - 1], sizeof(p));
        MemoryBarrier();
        ok = g_ls->StatusSeq == a;
    }
    if (!ok || !p.Valid || !p.Name[0])
        return L"";
    p.Name[sizeof(p.Name) - 1] = 0;
    std::wstring w;
    const int n = MultiByteToWideChar(CP_UTF8, 0, p.Name, -1, nullptr, 0);
    if (n > 1)
    {
        w.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, p.Name, -1, &w[0], n);
        w.resize(n - 1);
    }
    const size_t a = w.find_first_not_of(L' '), b = w.find_last_not_of(L' ');
    return a == std::wstring::npos ? L"" : w.substr(a, b - a + 1);
}

// v4: every hostile's position (lancecontrol-9), any range. Seqlock like the slots.
// v5: who has been firing rounds at the player (lancecontrol-14). Seqlock like the units.
int LsReadShooters(std::vector<LsShooter> &out)
{
    out.clear();
    if (!LsConnect())
        return 0;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_ls->ShotSeq;
        if (a & 1)
        {
            Sleep(1);
            continue;
        }
        MemoryBarrier();
        uint32_t n = g_ls->ShooterCount;
        if (n > (uint32_t)kLsMaxShooters)
            n = kLsMaxShooters;
        out.resize(n);
        if (n)
            std::memcpy(out.data(), (const void *)g_ls->Shooters, sizeof(LsShooter) * n);
        MemoryBarrier();
        if (g_ls->ShotSeq == a)
            return (int)n;
    }
    out.clear();
    return 0;
}

int LsReadUnits(std::vector<LsUnit> &out)
{
    out.clear();
    if (!LsConnect())
        return 0;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_ls->UnitSeq;
        if (a & 1)
        {
            Sleep(1);
            continue;
        }
        MemoryBarrier();
        uint32_t n = g_ls->UnitCount;
        if (n > (uint32_t)kLsMaxUnits)
            n = kLsMaxUnits;
        out.resize(n);
        if (n)
            std::memcpy(out.data(), (const void *)g_ls->Units, sizeof(LsUnit) * n);
        MemoryBarrier();
        if (g_ls->UnitSeq == a)
            return (int)n;
    }
    out.clear();
    return 0;
}

// Where every hostile was LAST seen (the units list, twice a second): "last known position".
struct SeenAt
{
    float     X = 0, Y = 0, Z = 0; // where it is (the game's own position, any range)
    ULONGLONG Ms = 0;
    bool      Dead = false;
    bool      Seen = false;        // the lance can see it now
    uint8_t   Flags = 0;           // LSU_* (lancecontrol-11): LSU_TargetsPlayer
    uint8_t   SeenBy = 0;          // which lancemates see it: bit (slot - 1) - their own line-of-sight flag (lancecontrol-12)
    ULONGLONG TgtMs = 0;           // when it last had the player as its target (the AI switches often: kept 5 s)
    float     LX = 0, LY = 0;      // where it was last SEEN - "last known position"
    ULONGLONG LMs = 0;
};
std::map<uint64_t, SeenAt> g_seen;
ULONGLONG                  g_seenMs = 0;
ULONGLONG                  g_seenDataMs = 0; // the units list last had anything in it

void SeenTick()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_seenMs < 500)
        return;
    g_seenMs = now;
    std::vector<LsUnit> us;
    LsReadUnits(us);
    if (!us.empty())
        g_seenDataMs = now;
    for (auto &u : us)
    {
        SeenAt &s = g_seen[((uint64_t)u.ActorHi << 32) | u.ActorLo];
        s.X = u.Loc[0];
        s.Y = u.Loc[1];
        s.Z = u.Loc[2];
        s.Ms = now;
        s.Dead = u.Dead != 0;
        s.Seen = u.Seen != 0;
        s.Flags = u.Flags;
        s.SeenBy = u.SeenBy;
        if (u.Flags & LSU_TargetsPlayer)
            s.TgtMs = now;
        if (s.Seen)
        {
            s.LX = s.X;
            s.LY = s.Y;
            s.LMs = now;
        }
    }
    if (g_seen.size() > 512) // old missions' units
        for (auto it = g_seen.begin(); it != g_seen.end();)
            it = now - it->second.Ms > 600000 ? g_seen.erase(it) : std::next(it);
}

// What a weapon category is called out loud.
const wchar_t *CatWord(uint8_t cat)
{
    switch (cat)
    {
    case kFcLaser: return L"laser";
    case kFcPPC: return L"P P C";
    case kFcBallistic: return L"ballistic";
    case kFcLockMissile:
    case kFcDumbMissile: return L"missile";
    case kFcMGFlamer: return L"machine gun";
    case kFcTAG: return L"tag";
    case kFcNARC: return L"narc";
    default: return L"weapon";
    }
}

// SITUATION callouts (build 2026-10-05i) - VoiceCommand's own kinds, from game state rather than
// one plugin event (numbers above the lance status event types).
enum SitKind : uint16_t
{
    K_HitHard = 100, K_Shredded, K_PartLost, K_Critical, K_ContactHeavy, K_Air, K_MissilesOnYou, K_TargetPart,
    K_LastEnemy, K_AreaClear, K_Outnumbered, K_AllyTrouble, K_Turrets,
    K_Found, K_ShadowLost // SHADOW: "found it" / "no sign of it" - answers to your order, so always said
};
struct CallKind
{
    uint16_t  Type;
    int       Prio;    // higher first; 6 and up are not held back by the chatter gap
    ULONGLONG CoolMs;  // per lancemate (per lance if Global)
    int       MinChat; // the chatter level it needs (ini: chatter)
    bool      Global;
};
const CallKind kCall[] = {
    {LSE_UnitDestroyed, 9, 0, 0, false},       {LSE_Overheat, 7, 20000, 1, false},     {LSE_AmmoEmpty, 6, 15000, 1, false},
    {LSE_WeaponDestroyed, 6, 10000, 1, false}, {LSE_FriendlyInLine, 5, 25000, 1, false}, {LSE_Kill, 5, 5000, 1, false},
    {LSE_AmmoLow, 4, 30000, 2, false},         {LSE_Spotted, 1, 120000, 3, false},     {LSE_TookDamage, 3, 20000, 2, false},
    {LSE_Arrived, 6, 10000, 1, false},         {LSE_UavLaunched, 8, 0, 1, false},
    {LSE_PlayerHit, 3, 45000, 2, true},        {LSE_EnemyOnPlayer, 7, 25000, 3, true},
    // urgency: hit hard < shredded < a part lost / critical
    {K_HitHard, 6, 15000, 2, false},           {K_Shredded, 8, 10000, 1, false},       {K_PartLost, 9, 3000, 0, false},
    {K_Critical, 9, 30000, 0, false},          {K_Turrets, 6, 30000, 2, false},        {K_AllyTrouble, 7, 60000, 1, false},
    {K_ContactHeavy, 6, 20000, 2, true}, /* 2026-10-19 (user): 20 s, was 8 */        {K_Air, 6, 60000, 2, true},             {K_MissilesOnYou, 9, 15000, 0, true},
    {K_TargetPart, 5, 8000, 2, true},          {K_LastEnemy, 6, 0, 1, true},           {K_AreaClear, 4, 0, 1, true},
    {K_Outnumbered, 5, 180000, 2, true},
    {LSE_Flanked, 8, 25000, 1, false}, // lancecontrol-21: an enemy close behind a lancemate (2026-10-07j)
    {LSE_AttackDropped, 6, 8000, 1, false}, // lancecontrol-34: its attack order ended (at most every 8 s per pilot - drops can chain)
    {K_Found, 8, 0, 0, false},                 {K_ShadowLost, 6, 0, 0, false},
};
// CALLOUT SWITCHES (2026-10-07h): one tick box per kind of callout in GlobalSettings (say_<name> in [voice]),
// replacing the chatter level's choice of what is said; "callout_gap" (seconds) replaces its pacing (the gap
// between non-urgent callouts; cooldowns scale with it, x1 at the old Normal of 6 s). Missing keys are written
// once from the old chatter level, so nothing changes until a box is ticked. Order answers (K_Found,
// K_ShadowLost) are always said.
struct CallSwitch
{
    uint16_t       Type;
    const wchar_t *Key;
};
const CallSwitch kCallSwitch[] = {
    {LSE_UnitDestroyed, L"say_lancemate_down"}, {K_PartLost, L"say_part_lost"},       {K_Critical, L"say_critical"},
    {K_Shredded, L"say_shredded"},              {K_HitHard, L"say_hit_hard"},         {LSE_TookDamage, L"say_taking_fire"},
    {K_AllyTrouble, L"say_ally_trouble"},       {LSE_Overheat, L"say_overheat"},      {LSE_AmmoLow, L"say_ammo_low"},
    {LSE_AmmoEmpty, L"say_ammo_out"},           {LSE_WeaponDestroyed, L"say_weapon_lost"}, {LSE_Arrived, L"say_in_position"},
    {LSE_FriendlyInLine, L"say_line_of_fire"},  {LSE_Kill, L"say_kills"},             {LSE_Spotted, L"say_spotted"},
    {K_ContactHeavy, L"say_heavy_contact"},     {K_Air, L"say_air"},                  {K_Turrets, L"say_turrets"},
    {K_MissilesOnYou, L"say_missiles"},         {LSE_PlayerHit, L"say_you_hit"},      {K_TargetPart, L"say_target_damage"},
    {K_Outnumbered, L"say_outnumbered"},        {K_LastEnemy, L"say_last_enemy"},     {K_AreaClear, L"say_area_clear"},
    {LSE_UavLaunched, L"say_uav"},             {LSE_Flanked, L"say_flanked"},
};
std::map<uint16_t, bool> g_callOn;           // kind -> on (a kind not listed: on)
bool      g_sayKillTally = true;             // "... that's 3" after a kill (was chatter >= 2)
bool      g_sayMissileShooter = false;       // "... from the Catapult" (was chatter >= 3)
ULONGLONG g_calloutGapMs = 6000;             // the gap between non-urgent callouts (was kGap[chatter])
double    g_coolMult = 1.0;                  // cooldowns x this (x1 at 6 s)

bool CalloutOn(uint16_t t)
{
    auto it = g_callOn.find(t);
    return it == g_callOn.end() || it->second;
}

const CallKind *FindKind(uint16_t t);

void LoadCallouts(bool writeMissing)
{
    const int ch = std::max(0, std::min(3, gs::Int(L"voice", L"chatter", 2))); // the old level: defaults only
    auto get = [&](const wchar_t *key, bool def) {
        if (!gs::Has(L"voice", key))
        {
            if (writeMissing)
                gs::SetInt(L"voice", key, def ? 1 : 0);
            return def;
        }
        return gs::Int(L"voice", key, def ? 1 : 0) != 0;
    };
    for (auto &c : kCallSwitch)
    {
        const CallKind *k = FindKind(c.Type);
        g_callOn[c.Type] = get(c.Key, k ? ch >= k->MinChat : true);
    }
    g_sayKillTally = get(L"say_kill_tally", ch >= 2);
    g_sayMissileShooter = get(L"say_missile_shooter", ch >= 3);
    static const int kOldGap[4] = {60, 20, 6, 2};
    if (!gs::Has(L"voice", L"callout_gap") && writeMissing)
        gs::SetInt(L"voice", L"callout_gap", kOldGap[ch]);
    const double gap = std::max(0.0, std::min(120.0, gs::Num(L"voice", L"callout_gap", kOldGap[ch])));
    g_calloutGapMs = (ULONGLONG)(gap * 1000.0);
    g_coolMult = std::max(0.5, std::min(2.0, gap / 6.0));
}

const CallKind *FindKind(uint16_t t)
{
    for (auto &k : kCall)
        if (k.Type == t)
            return &k;
    return nullptr;
}

// STEP-ON (build 2026-10-05o): the lines urgent enough to break in on someone else's transmission.
bool StepsOn(uint16_t t)
{
    switch (t)
    {
    case LSE_UnitDestroyed: case K_PartLost: case K_Critical: case K_Shredded: // (missiles on you / enemy on you: not urgent - user, 2026-10-06h)
    case LSE_Flanked: // 2026-10-07j
        return true;
    default:
        return false;
    }
}

// How long after it happened a line is still worth saying (a queued line, or the rest of a
// stepped-on one, is dropped after this). Replies and reports: 30 s.
ULONGLONG StaleMs(uint16_t t)
{
    switch (t)
    {
    case LSE_TookDamage: return 2000;
    case LSE_PlayerHit: case K_MissilesOnYou: return 3000;
    case LSE_EnemyOnPlayer: case K_HitHard: case K_Shredded: case LSE_FriendlyInLine: case LSE_Flanked: return 4000;
    case LSE_Spotted: case LSE_Overheat: case K_TargetPart: case LSE_AttackDropped: return 6000;
    case K_ContactHeavy: case K_AllyTrouble: return 8000;
    case LSE_Kill: case K_Air: case K_Turrets: case K_Found: case K_Position: case LSE_UavLaunched: return 10000;
    case K_PartLost: case K_Critical: case LSE_UnitDestroyed: case K_LastEnemy: case K_Outnumbered: case LSE_Arrived: return 15000;
    case LSE_WeaponDestroyed: case LSE_AmmoEmpty: case K_AreaClear: case K_ShadowLost: return 20000;
    default: return 30000; // ammo low, replies, status reports, answers
    }
}

struct Pending
{
    int          Slot;  // whose voice
    int          Prio;
    ULONGLONG    Ms;
    std::wstring Text;
    uint16_t     Type;
    ULONGLONG    Ready; // not before (a plain "taking fire" waits a moment: a worse report may replace it)
};
std::vector<Pending> g_pending;
std::map<std::pair<int, int>, ULONGLONG> g_lastCall; // (slot, type) -> last time spoken

bool Queue(int slot, uint16_t type, const std::wstring &text, int prio = -1) // prio: -1 = the kind's own; true = queued
{
    const CallKind *k = FindKind(type);
    if (!k || !CalloutOn(type) || text.empty()) // 2026-10-07h: the tick boxes, not the chatter level
        return false;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG cool = (ULONGLONG)(k->CoolMs * g_coolMult);
    auto key = std::make_pair(k->Global ? 0 : slot, (int)type);
    auto it = g_lastCall.find(key);
    if (it != g_lastCall.end() && now - it->second < cool)
        return false;
    g_lastCall[key] = now;
    // 2026-10-07k: urgent lines (the ones that step on others - a lancemate down, flanked...) end with "!" so Kokoro
    // says them with more emphasis (the 07j speed-up is gone, at the user's word)
    std::wstring t = text;
    if (StepsOn(type))
    {
        while (!t.empty() && (t.back() == L' ' || t.back() == L'.'))
            t.pop_back();
        if (!t.empty() && t.back() != L'!' && t.back() != L'?')
            t += L'!';
    }
    g_pending.push_back({slot, prio >= 0 ? prio : k->Prio, now, t, type, now + (type == LSE_TookDamage ? 900 : 0)});
    return true;
}

// 2026-10-16: would Queue() take this kind from this slot now? (the same checks, nothing stamped) - so a line is
// only built (and counted by the in-game test) when it can be said
bool CalloutReady(int slot, uint16_t type)
{
    const CallKind *k = FindKind(type);
    if (!k || !CalloutOn(type))
        return false;
    const ULONGLONG cool = (ULONGLONG)(k->CoolMs * g_coolMult);
    auto it = g_lastCall.find(std::make_pair(k->Global ? 0 : slot, (int)type));
    return it == g_lastCall.end() || GetTickCount64() - it->second >= cool;
}


// ---- SITUATION CALLOUTS (build 2026-10-05i): per-lancemate memory ---------------------------
struct SitSlot
{
    uint32_t  Lo = 0, Hi = 0;                       // the unit
    std::deque<std::pair<ULONGLONG, float>> Hist;   // (ms, health 0..1) - the last 6 s
    float     St[TMP_COUNT] = {};
    bool      HaveSt = false, Critical = false;
    uint32_t  AtkLo = 0, AtkHi = 0;                 // who hit it last (lancecontrol-8)
    ULONGLONG AtkMs = 0;
    int       Kills = 0;
};
SitSlot g_sit[9];
std::wstring EnemyNameOf(uint32_t lo, uint32_t hi, TerrainMechInfo *info = nullptr); // below

// INTEL: the unit each pilot last named in a callout, for "where?" / "what is it?" (QUESTIONS).
struct IntelNote
{
    int          Slot = 0;
    uint32_t     Lo = 0, Hi = 0;   // 0 = no unit named ("taking heavy fire")
    ULONGLONG    Ms = 0;
    std::wstring Name;             // as said ("Atlas")
    float        X = 0, Y = 0;     // where it was when named (for "lost it, last at your 3 o'clock")
    bool         HavePos = false;
};
IntelNote g_intelLast, g_intelBySlot[9];
std::map<std::wstring, IntelNote> g_intelByName; // lower-case name -> the last one reported
void NoteIntel(int slot, uint32_t lo, uint32_t hi, const std::wstring &name); // below
std::wstring LockedEnemyName();                                                     // below
bool ContactBearing(uint16_t id, std::wstring &clock, std::wstring &range);         // below

void SitAttacker(int s, uint32_t lo, uint32_t hi)
{
    if (s >= 1 && s <= 8 && (lo || hi))
    {
        g_sit[s].AtkLo = lo;
        g_sit[s].AtkHi = hi;
        g_sit[s].AtkMs = GetTickCount64();
    }
}

// SUPPORT (2026-10-13, user): what lancemate t is fighting - its own combat target, else whoever hit it in the last
// 30 s, else the nearest enemy it sees, else the nearest within 600 m (attacker = true) - and where that unit is. 0 found, 1 t is not in the field, 2 nothing to shoot at.
bool UnitDead(uint32_t lo, uint32_t hi);                                                      // below
bool UnitPosNow(uint32_t lo, uint32_t hi, float &x, float &y, bool &dead, bool needSight);      // below
int SupportTarget(int t, bool &attacker, float p[3], std::wstring &enemy)
{
    if (!LsFresh()) // 2026-10-16: a frozen feed (ejected / dead / paused) - no attack on stale data
        return 3;
    LsSlot sl[kLsMaxSlots];
    const int ns = LsConnect() ? LsReadSlots(sl) : 0;
    const LsSlot *mate = nullptr;
    for (int k = 0; k < ns; ++k)
        if (sl[k].Valid && sl[k].Slot == t)
            mate = &sl[k];
    if (!mate || !mate->Alive)
        return 1;
    uint32_t lo = mate->TargetLo, hi = mate->TargetHi;
    attacker = false;
    if ((!lo && !hi) || UnitDead(lo, hi))
    {
        lo = hi = 0;
        if (g_sit[t].AtkMs && GetTickCount64() - g_sit[t].AtkMs < 30000 && !UnitDead(g_sit[t].AtkLo, g_sit[t].AtkHi))
        {
            lo = g_sit[t].AtkLo;
            hi = g_sit[t].AtkHi;
            attacker = true;
        }
    }
    std::vector<LsUnit> us;
    LsReadUnits(us);
    // 2026-10-13 (user): no target and nobody hit it - the nearest enemy it can SEE, else the nearest one within 600 m
    if (!lo && !hi)
    {
        float best = 1e30f, bestSeen = 1e30f;
        uint32_t nlo = 0, nhi = 0, slo = 0, shi = 0;
        for (auto &un : us)
        {
            if (un.Dead)
                continue;
            const float dx = un.Loc[0] - mate->Loc[0], dy = un.Loc[1] - mate->Loc[1];
            const float d = dx * dx + dy * dy;
            if (t >= 1 && t <= 8 && (un.SeenBy & (1u << (t - 1))) && d < bestSeen)
            {
                bestSeen = d;
                slo = un.ActorLo;
                shi = un.ActorHi;
            }
            if (d < best && d <= 60000.0f * 60000.0f)
            {
                best = d;
                nlo = un.ActorLo;
                nhi = un.ActorHi;
            }
        }
        const bool seen = slo || shi;
        lo = seen ? slo : nlo;
        hi = seen ? shi : nhi;
        attacker = lo || hi;
    }
    if (!lo && !hi)
        return 2;
    enemy = EnemyNameOf(lo, hi);
    p[0] = p[1] = 0.0f;
    p[2] = mate->Loc[2];
    for (auto &un : us)
        if (un.ActorLo == lo && un.ActorHi == hi && !un.Dead)
        {
            std::memcpy(p, un.Loc, 3 * sizeof(float));
            return 0;
        }
    bool dead = false;
    if (UnitPosNow(lo, hi, p[0], p[1], dead, false) && !dead)
        return 0;
    return 2;
}

// A lancemate's event -> its line (none for the ones it does not speak about).
void PatchLockEvent(uint32_t lo, uint32_t hi); // PATCH-THROUGH, below

// ---- TACOPS ORDERS ANSWERED (2026-10-19, user) ---------------------------------------------------------------------
// An order given on HUDDisplay's TacOps page (or anything else that is not us) is answered on the radio like a voice
// order: reply_single, or reply_lance by the first of several lancemates given the same order within 400 ms. Skipped:
// our own orders (answered already), orders with nothing to say (torso, fire settings, nav marks...), and the same
// order to the same lancemate within 15 s (a route's next leg, a re-sent move).
struct TacAck
{
    int            Slot;
    uint8_t        Order;
    ULONGLONG      Ms;
    const wchar_t *Grp; // the reply line group
};
std::vector<TacAck> g_tacAck;
VcSent g_tacAcked[16];
const wchar_t *TacAckGroup(int o)
{
    switch (o)
    {
    case VanillaExportBlock::TCO_Move:
    case VanillaExportBlock::TCO_MoveFire: return L"order_move";
    case VanillaExportBlock::TCO_Attack:
    case VanillaExportBlock::TCO_AttackMyTarget:
    case VanillaExportBlock::TCO_CalledShot: return L"order_attack";
    case VanillaExportBlock::TCO_Follow: return L"order_follow";
    case VanillaExportBlock::TCO_HoldHere: return L"order_hold";
    case VanillaExportBlock::TCO_Fallback: return L"order_fall_back";
    case VanillaExportBlock::TCO_IndirectCover: return L"order_indirect";
    default: return nullptr;
    }
}
void TacAckEvent(const LsEvent &e)
{
    const int s = e.Slot;
    if (s < 1 || s >= 16 || e.Value < 0.0f)
        return;
    const int o = (int)(e.Value + 0.5f);
    if (!TacAckGroup(o))
        return;
    const ULONGLONG now = GetTickCount64();
    if (g_vcSent[s].Order == o && now - g_vcSent[s].Ms < 3000)
        return; // our own voice order - Execute answered it
    if (g_tacAcked[s].Order == o && now - g_tacAcked[s].Ms < 15000)
        return; // the same order again (a route's next leg)
    g_tacAcked[s].Order = (uint8_t)o;
    g_tacAcked[s].Ms = now;
    g_tacAck.push_back({s, (uint8_t)o, now, TacAckGroup(o)});
}
void TacAckFlush(ULONGLONG now)
{
    if (g_tacAck.empty() || now - g_tacAck.front().Ms < 400)
        return;
    std::vector<TacAck> b;
    b.swap(g_tacAck);
    std::vector<char> done(b.size(), 0);
    for (size_t i = 0; i < b.size(); ++i)
    {
        if (done[i])
            continue;
        int same = 0;
        for (size_t j = i; j < b.size(); ++j)
            if (!done[j] && std::wcscmp(b[j].Grp, b[i].Grp) == 0)
            {
                done[j] = 1;
                ++same;
            }
        const int s = b[i].Slot;
        const wchar_t *g = b[i].Grp;
        Log(L"tacops order answered: slot %d %ls%ls", s, g, same > 1 ? L" (lance)" : L"");
        Say(s, Line(same > 1 ? L"reply_lance" : L"reply_single", s, {{L"reply", Line(g, s, {}, false)}}));
    }
}

void FireNavWatchTick(ULONGLONG now); // below NavSnapshot

void OnEvent(const LsEvent &e, const LsSlot *slots, int n)
{
    if (e.Type == LSE_OrderReceived)
    {
        TacAckEvent(e); // TACOPS ORDERS ANSWERED (2026-10-19)
        return;
    }
    if (e.Type == LSE_PlayerLock)
        PatchLockEvent(e.ALo, e.AHi);
    // UAV LAUNCHED: the player's event, answered by a lancemate still standing (picked at random).
    if (e.Type == LSE_UavLaunched)
    {
        std::vector<int> up;
        for (int i = 0; i < n; ++i)
            if (slots[i].Valid && !slots[i].IsPlayer && slots[i].Alive)
                up.push_back(slots[i].Slot);
        const int by = up.empty() ? 0 : up[(size_t)rand() % up.size()];
        std::wstring line = Line(L"uav_launched", by);
        if (e.Value < 0.5f)
            AddPiece(line, Line(L"uav_last", by));
        Queue(by, e.Type, line);
        return;
    }
    const LsSlot *me = nullptr, *player = nullptr;
    for (int i = 0; i < n; ++i)
    {
        if (!slots[i].Valid)
            continue;
        if (slots[i].IsPlayer)
            player = &slots[i];
        if (slots[i].Slot == e.Slot)
            me = &slots[i];
    }
    // You hit an enemy / an enemy turned on you (lancecontrol-8): said by a lancemate.
    if (e.Type == LSE_EnemyOnPlayer)
        return; // "that Bane is on you" removed entirely (user, 2026-10-06h): the AI's target flag is too fickle
    if (e.Type == LSE_PlayerHit)
    {
        TerrainMechInfo m = {};
        const std::wstring en = EnemyNameOf(e.ALo, e.AHi, &m);
        if (en.empty() || m.Kind >= TUK_Generator)
            return;
        if (e.Type == LSE_EnemyOnPlayer && !(m.Kind == TUK_Mech && (g_cfg.Chatter >= 3 || (m.WeightClass >= 2 && m.WeightClass != 0xFF))))
            return; // a heavy or an assault (any mech when busy)
        std::vector<int> up;
        for (int i = 0; i < n; ++i)
            if (slots[i].Valid && !slots[i].IsPlayer && slots[i].Alive)
                up.push_back(slots[i].Slot);
        if (up.empty())
            return;
        const int by = up[(size_t)rand() % up.size()];
        std::wstring clock, range;
        bool said;
        if (e.Type == LSE_EnemyOnPlayer && ContactBearing(m.Id, clock, range))
            said = Queue(by, e.Type, Line(L"callout_enemy_on_you_at", by, {{L"enemy", en}, {L"clock", clock}, {L"range", range}}));
        else
            said = Queue(by, e.Type, Line(e.Type == LSE_PlayerHit ? L"callout_player_hit" : L"callout_enemy_on_you", by, {{L"enemy", en}}));
        if (said)
            NoteIntel(by, e.ALo, e.AHi, en);
        return;
    }
    if (me && me->IsPlayer)
        return; // your own mech does not report to you
    const int s = e.Slot;
    auto wcat = [&](void) -> std::wstring {
        const int w = (int)e.Value;
        return (me && w >= 0 && w < me->WeaponCount && w < kLsMaxWeapons) ? CatWord(me->Weapons[w].Category) : L"weapon";
    };
    switch (e.Type)
    {
    case LSE_UnitDestroyed:
    {
        // Said by someone else still standing.
        int by = 0;
        for (int i = 0; i < n && !by; ++i)
            if (slots[i].Valid && !slots[i].IsPlayer && slots[i].Alive && slots[i].Slot != s)
                by = slots[i].Slot;
        Queue(by, LSE_UnitDestroyed, Line(L"callout_down", by, {{L"callsign", Callsign(s)}}));
        break;
    }
    case LSE_Kill:
    {
        // One "target down" per kill (build 2026-10-05h): lancemates firing at the same target
        // each get the kill event - the first one says it.
        static std::map<uint64_t, ULONGLONG> said;
        const ULONGLONG now = GetTickCount64();
        const uint64_t key = ((uint64_t)e.AHi << 32) | e.ALo;
        if (key)
        {
            auto it = said.find(key);
            if (it != said.end() && now - it->second < 30000)
                break;
            said[key] = now;
            if (said.size() > 200)
                said.clear();
        }
        // By name when LidarExport knows the unit (it holds a dead one for 5 s); a tally now and then.
        TerrainMechInfo m = {};
        const std::wstring en = key ? EnemyNameOf(e.ALo, e.AHi, &m) : L"";
        std::wstring line = en.empty() ? Line(L"callout_kill", s)
                                       : Line(m.Kind == TUK_Mech ? L"callout_kill_named" : L"callout_kill_vehicle", s, {{L"enemy", en}});
        if (s >= 1 && s <= 8)
        {
            const int k = ++g_sit[s].Kills;
            if (g_sayKillTally && (k == 3 || k == 5 || k == 7 || k == 10))
                AddPiece(line, Line(L"callout_kill_tally", s, {{L"count", std::to_wstring(k)}}));
        }
        Queue(s, e.Type, line);
        break;
    }
    case LSE_Overheat: Queue(s, e.Type, Line(L"callout_overheat", s)); break;
    case LSE_AmmoEmpty: Queue(s, e.Type, Line(L"callout_ammo_empty", s, {{L"weapon", wcat()}})); break;
    case LSE_AmmoLow: Queue(s, e.Type, Line(L"callout_ammo_low", s, {{L"weapon", wcat()}})); break;
    case LSE_WeaponDestroyed: Queue(s, e.Type, Line(L"callout_weapon_lost", s, {{L"weapon", wcat()}})); break;
    case LSE_Spotted:
        // Only into a quiet radio (build 2026-10-05h): nothing said for 2 minutes. "Taking fire"
        // is the line that matters in a fight.
        if (GetTickCount64() - g_lastSayMs >= 120000)
            Queue(s, e.Type, Line(L"callout_spotted", s));
        break;
    case LSE_TookDamage:
    {
        // A = who hit it (lancecontrol-8). How BAD it is is SituationTick's job (hit hard / shredded
        // / a part lost); this is the light "taking fire", which waits a moment so a worse one wins.
        SitAttacker(s, e.ALo, e.AHi);
        const std::wstring en = (e.ALo || e.AHi) ? EnemyNameOf(e.ALo, e.AHi) : L"";
        if (Queue(s, e.Type, en.empty() ? Line(L"callout_taking_fire", s) : Line(L"callout_taking_fire_from", s, {{L"enemy", en}})))
            NoteIntel(s, en.empty() ? 0 : e.ALo, en.empty() ? 0 : e.AHi, en);
        break;
    }
    case LSE_Arrived: Queue(s, e.Type, Line(L"callout_in_position", s)); break;
    case LSE_AttackDropped: // lancecontrol-34: its attack order ended - 2 lost sight, 3 never seen
    {
        const std::wstring en = (e.ALo || e.AHi) ? EnemyNameOf(e.ALo, e.AHi) : L"";
        const int why = (int)(e.Value + 0.5f);
        Queue(s, e.Type, Line(why == 3 ? L"callout_attack_dropped_never" : L"callout_attack_dropped", s,
                              {{L"enemy", en.empty() ? std::wstring(L"target") : en}}));
        break;
    }
    case LSE_Flanked: // an enemy close behind it (lancecontrol-21, 2026-10-07j)
    {
        SitAttacker(s, e.ALo, e.AHi);
        TerrainMechInfo m = {};
        const std::wstring en = (e.ALo || e.AHi) ? EnemyNameOf(e.ALo, e.AHi, &m) : L"";
        if (Queue(s, e.Type, en.empty() ? Line(L"callout_flanked_unknown", s) : Line(L"callout_flanked", s, {{L"enemy", en}})))
            NoteIntel(s, en.empty() ? 0 : e.ALo, en.empty() ? 0 : e.AHi, en);
        break;
    }
    case LSE_FriendlyInLine:
    {
        int ally = 0; // a lancemate by name
        for (int i = 0; i < n; ++i)
            if (slots[i].Valid && !slots[i].IsPlayer && slots[i].ActorLo == e.ALo && slots[i].ActorHi == e.AHi)
                ally = slots[i].Slot;
        if (player && e.ALo == player->ActorLo && e.AHi == player->ActorHi)
            Queue(s, e.Type, Line(L"callout_you_in_line", s));
        else if (ally)
            Queue(s, e.Type, Line(L"callout_friendly_in_line_named", s, {{L"ally", Callsign(ally)}}));
        else
            Queue(s, e.Type, Line(L"callout_friendly_in_line", s));
        break;
    }
    default: break;
    }
}

// "Two, taking fire!" -> "Taking fire!" (when the line in front already says who it is).
std::wstring StripCallsign(int slot, std::wstring t)
{
    const std::wstring cs = Callsign(slot);
    if (t.size() > cs.size() && _wcsnicmp(t.c_str(), cs.c_str(), cs.size()) == 0 &&
        (t[cs.size()] == L',' || t[cs.size()] == L' ' || t[cs.size()] == L'.'))
    {
        size_t k = cs.size();
        while (k < t.size() && (t[k] == L',' || t[k] == L' ' || t[k] == L'.'))
            ++k;
        t = t.substr(k);
        if (!t.empty())
            t[0] = (wchar_t)towupper(t[0]);
    }
    return t;
}

// ---- PATCH-THROUGH (2026-10-06g) ---------------------------------------------------------------
// Your lock set to a unit (LanceControl TCO_LockTarget, the exact unit), said done only once the game
// shows it: the lock event for that unit within 1.5 s (or LidarExport's Locked flag on it then), else
// "no joy". Not sent at all when the unit is off your scope (your tracker would not take it).
struct PatchWait
{
    int          Slot = 0;
    uint32_t     Lo = 0, Hi = 0;
    std::wstring Name;
    ULONGLONG    Ms = 0;
};
PatchWait g_patchWait;

// An answer to you, said as one: ahead of the chatter, cutting in on whoever talks.
void SayPriority(int slot, const std::wstring &text)
{
    g_sayPriority = true;
    g_cutArmed = true;
    Say(slot, text);
    g_sayPriority = false;
    g_cutArmed = false;
}

bool MechInfoOf(uint32_t lo, uint32_t hi, TerrainMechInfo &out, bool needCondition); // below

bool PatchLockedNow(uint32_t lo, uint32_t hi)
{
    TerrainMechInfo m = {};
    return MechInfoOf(lo, hi, m, false) && m.Locked;
}

void StartPatch(int s, uint32_t lo, uint32_t hi, const std::wstring &name)
{
    const ULONGLONG now = GetTickCount64();
    auto it = g_seen.find(((uint64_t)hi << 32) | lo);
    if (it == g_seen.end() || now - it->second.Ms > 3000)
    {
        Say(s, Line(L"answer_patch_lost", s));
        return;
    }
    if (it->second.Dead)
    {
        Say(s, Line(L"answer_down_yes", s, {{L"enemy", name}}));
        return;
    }
    if (!(it->second.Flags & LSU_OnPlayerRadar))
    {
        Say(s, Line(L"answer_patch_offscope", s, {{L"enemy", name}}));
        return;
    }
    if (PatchLockedNow(lo, hi))
    {
        Say(s, Line(L"answer_patch_already", s, {{L"enemy", name}}));
        return;
    }
    if (!g_blk)
    {
        Say(s, Line(L"problem_no_link", s));
        return;
    }
    float loc[3] = {0.0f, 0.0f, VanillaExportBlock::kTacLocActor}; // the exact unit (lancecontrol-13)
    std::memcpy(&loc[0], &lo, 4);
    std::memcpy(&loc[1], &hi, 4);
    Send(VanillaExportBlock::TCO_LockTarget, s, loc);
    g_patchWait.Slot = s;
    g_patchWait.Lo = lo;
    g_patchWait.Hi = hi;
    g_patchWait.Name = name;
    g_patchWait.Ms = now;
    Log(L"patch-through: the %ls sent - waiting for your lock", name.c_str());
}

void PatchLockEvent(uint32_t lo, uint32_t hi)
{
    if (!g_patchWait.Ms || lo != g_patchWait.Lo || hi != g_patchWait.Hi)
        return;
    const PatchWait w = g_patchWait;
    g_patchWait = PatchWait();
    SayPriority(w.Slot, Line(L"answer_patch_done", w.Slot, {{L"enemy", w.Name}}));
}

void PatchTick()
{
    if (!g_patchWait.Ms || GetTickCount64() - g_patchWait.Ms < 1500)
        return;
    const PatchWait w = g_patchWait;
    g_patchWait = PatchWait();
    const bool ok = PatchLockedNow(w.Lo, w.Hi);
    Log(L"patch-through: the %ls - %ls", w.Name.c_str(), ok ? L"locked (no event seen)" : L"NOT locked after 1.5 s");
    SayPriority(w.Slot, Line(ok ? L"answer_patch_done" : L"answer_patch_failed", w.Slot, {{L"enemy", w.Name}}));
}

// STEP-ON: the stepped-on pilot picks up again once the channel is clear - only what was not heard,
// and only while it is still worth saying ("Two, continuing. ..." / "Two, I say again. ...").
void ResumeTick(ULONGLONG now)
{
    const bool busy = !g_sayQ.empty() || RadioBusy();
    ResumeNote r;
    {
        std::lock_guard<std::mutex> lk(g_resumeMx);
        if (!g_resume.Have)
            return;
        if (now - g_resume.Ms > StaleMs(g_resume.Type))
        {
            Log(L"voice: %ls does not pick up again - no longer worth saying", Callsign(g_resume.Slot).c_str());
            g_resume.Have = false;
            return;
        }
        if (busy)
            return;
        r = g_resume;
        g_resume.Have = false;
    }
    std::wstring rest;
    for (auto &x : r.Rest)
        rest += (rest.empty() ? L"" : L" ") + x;
    rest = StripCallsign(r.Slot, rest);
    if (rest.empty())
        return;
    const std::wstring lead = Line(r.Mid ? L"resume_say_again" : L"resume_continue", r.Slot);
    Say(r.Slot, lead + L" " + rest, r.Type, r.Ms, false, (int)SplitSentences(lead).size());
}

// Every loop turn: new events in, one line out when the voice is free.
void CalloutTick()
{
    PatchTick(); // a patch-through's answer (2026-10-06g)
    if (!g_cfg.Callouts || !LsConnect())
        return;
    const uint32_t head = g_ls->EventHead;
    if (!g_evSynced || head < g_evNext)
    {
        g_evNext = head; // first look, or the plugin restarted: from now on
        g_evSynced = true;
    }
    if (head - g_evNext > (uint32_t)kLsEventRing)
        g_evNext = head - kLsEventRing; // fell more than a ring behind
    if (head != g_evNext)
    {
        LsSlot slots[kLsMaxSlots];
        const int n = LsReadSlots(slots);
        for (; g_evNext != head; ++g_evNext)
        {
            const LsEvent e = g_ls->Events[g_evNext % kLsEventRing];
            if (e.Seq != g_evNext + 1)
                break; // not written yet: next turn
            OnEvent(e, slots, n);
        }
    }
    const ULONGLONG now = GetTickCount64();
    FireNavWatchTick(now); // fire settings and nav marks from TacOps (2026-10-19)
    TacAckFlush(now); // TACOPS ORDERS ANSWERED (2026-10-19)
    ResumeTick(now);
    if (g_pending.empty())
        return;
    g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                   [&](const Pending &p) { return now - p.Ms > std::min<ULONGLONG>(4000, StaleMs(p.Type)); }),
                    g_pending.end());
    if (g_pending.empty())
        return;
    if (g_voice && g_cfg.Speak)
    {
        SPVOICESTATUS st = {};
        if (SUCCEEDED(g_voice->GetStatus(&st, nullptr)) && st.dwRunningState == SPRS_IS_SPEAKING)
            return; // one line at a time
    }
    // An answer still waiting to be spoken, or the radio still talking: only an urgent line goes now,
    // stepping on what is on the air (not on another urgent one) - STEP-ON.
    bool stepOn = false;
    if (!g_sayQ.empty() || RadioBusy())
    {
        if (!RadioActive() || !g_sayQ.empty() || !RadioKeyed() || RadioUrgentOnAir())
            return;
        stepOn = true;
    }
    auto best = g_pending.end();
    for (auto i = g_pending.begin(); i != g_pending.end(); ++i)
        if (now >= i->Ready && (!stepOn || StepsOn(i->Type)) &&
            (best == g_pending.end() || i->Prio > best->Prio || (i->Prio == best->Prio && i->Ms < best->Ms)))
            best = i;
    if (best == g_pending.end())
        return;
    // CHATTER: the lesser lines keep a gap after anything said (orders and replies included).
    if (best->Prio <= 5 && now - g_lastSayMs < g_calloutGapMs) // 2026-10-07h: GlobalSettings' "Time between callouts"
        return; // it waits (and expires after 4 s)
    const Pending p = *best;
    g_pending.erase(best);
    if (stepOn && p.Slot == RadioAirSlot())
    {
        // The pilot on the air has the urgent news itself (2026-10-06g): no "Break, break" - it cuts itself
        // short, a beat, "Wait... you have missiles locked on you!" - and does not pick up the old line again.
        Log(L"voice: %ls cuts itself short", Callsign(p.Slot).c_str());
        std::wstring rest = StripCallsign(p.Slot, p.Text);
        const std::wstring pre = Line(L"callout_self_correct", p.Slot);
        if (!pre.empty() && pre.back() == L',' && !rest.empty())
            rest[0] = (wchar_t)towlower(rest[0]);
        Say(p.Slot, pre + L" " + rest, p.Type, p.Ms, true);
    }
    else if (stepOn)
    {
        Log(L"voice: %ls breaks in", Callsign(p.Slot).c_str());
        Say(p.Slot, Line(L"callout_break", p.Slot) + L" " + p.Text, p.Type, p.Ms, true);
    }
    else
        Say(p.Slot, p.Text, p.Type, p.Ms);
}

// ---- STATUS REPORT: "two, status" - damage, ammo, heat, lost weapons, in the pilot's own voice ----
// Damage comes from the lance status feed's condition (v2: LanceControl reads it itself, at any
// distance), falling back to LidarExport's mech condition reads (MW5_TerrainExport, joined on the
// actor address, inside its 1000 m scan); ammo, heat and weapons from the lance status feed. Two or three phrasings a level,
// picked at random, so it does not sound canned.
HANDLE        g_trmap = nullptr;
TerrainBlock *g_tr    = nullptr;

bool TerrainConnect()
{
    if (g_tr && g_tr->H.Version == TERRAIN_VERSION)
        return true;
    if (g_tr)
    {
        UnmapViewOfFile(g_tr);
        CloseHandle(g_trmap);
        g_tr = nullptr;
        g_trmap = nullptr;
    }
    g_trmap = OpenFileMappingW(FILE_MAP_READ, FALSE, TERRAIN_MMF_NAME);
    if (!g_trmap)
        return false;
    // Only the header is read (the mech list lives in it), not the terrain tiles.
    g_tr = static_cast<TerrainBlock *>(MapViewOfFile(g_trmap, FILE_MAP_READ, 0, 0, sizeof(TerrainHeader)));
    if (!g_tr || g_tr->H.Version != TERRAIN_VERSION)
    {
        if (g_tr)
            UnmapViewOfFile(g_tr);
        CloseHandle(g_trmap);
        g_tr = nullptr;
        g_trmap = nullptr;
        return false;
    }
    return true;
}

// A lancemate's condition from LidarExport (seqlock: FrameCounter == FrameCounterEnd around it).
// A unit's record from LidarExport (identity and condition), or false when it is not in its scan.
bool MechInfoOf(uint32_t lo, uint32_t hi, TerrainMechInfo &out, bool needCondition)
{
    if ((!lo && !hi) || !TerrainConnect())
        return false;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_tr->H.FrameCounter;
        MemoryBarrier();
        if (g_tr->H.FrameCounterEnd != a)
        {
            Sleep(1);
            continue;
        }
        bool found = false;
        uint32_t n = g_tr->H.MechInfoCount;
        if (n > (uint32_t)TERR_MAX_MECHS)
            n = TERR_MAX_MECHS;
        for (uint32_t i = 0; i < n && !found; ++i)
            if (g_tr->H.Mechs[i].Valid && g_tr->H.Mechs[i].ActorLo == lo && g_tr->H.Mechs[i].ActorHi == hi)
            {
                std::memcpy(&out, (const void *)&g_tr->H.Mechs[i], sizeof(out));
                found = true;
            }
        MemoryBarrier();
        if (g_tr->H.FrameCounter == a && g_tr->H.FrameCounterEnd == a)
            return found && (!needCondition || out.HaveCondition);
    }
    return false;
}

bool MechConditionOf(uint32_t lo, uint32_t hi, TerrainMechInfo &out) { return MechInfoOf(lo, hi, out, true); }

// ---- SITUATION CALLOUTS (build 2026-10-05i) ------------------------------------------------------
// What the lance says about the fight, from game state, every half second:
//   - a lancemate's damage, by URGENCY: how much of its mech (armour + structure, all parts) went in
//     the last ~5 s - 4 % "that Hunchback's hitting me hard", 10 % "that Atlas is ripping me apart",
//     a part destroyed "lost my arm to that Atlas", critical "I'm going down" (+ another lancemate:
//     "Sledge is in trouble"). Light hits stay the plain "taking fire" (the event, with the attacker).
//     The attacker is the took-damage event's unit (lancecontrol-8, the game's RecentDamageTarget),
//     named when LidarExport knows it (inside its 1000 m scan); a turret gets "take out that turret".
//   - new heavy / assault mechs and VTOLs on LidarExport's scan, with a clock bearing from your cockpit;
//   - your locked target losing a part / its centre torso nearly gone;
//   - last enemy / area clear (within the scan) after a fight; outnumbered;
//   - a missile lock on you (LidarExport's lock check - it runs while HUDDisplay asks for it).
// How much of it is said: chatter in VoiceCommand.ini (Queue / CalloutTick).

// A unit as the pilots name it: the chassis for a mech ("Atlas"), else its kind ("turret", "VTOL").
std::wstring UnitWord(const TerrainMechInfo &m)
{
    auto clean = [](const char *c, size_t max) {
        std::wstring w;
        for (size_t i = 0; i < max && c[i]; ++i)
            w += c[i] == '_' ? L' ' : (wchar_t)(unsigned char)c[i];
        while (!w.empty() && w.back() == L' ')
            w.pop_back();
        if (!w.empty())
            w[0] = (wchar_t)towupper(w[0]);
        return w;
    };
    switch (m.Kind)
    {
    case TUK_Mech:
    {
        std::wstring w = clean(m.Chassis, sizeof(m.Chassis));
        // ALL CAPS ("MAN O' WAR") is read as letters - Title Case it ("Man O' War").
        bool lower = false;
        for (wchar_t c : w)
            lower |= iswlower(c) != 0;
        if (!lower && w.size() > 3)
            for (size_t i = 0; i < w.size(); ++i)
                w[i] = (i == 0 || w[i - 1] == L' ' || w[i - 1] == L'-') ? w[i] : (wchar_t)towlower(w[i]);
        return w.empty() ? L"mech" : w;
    }
    case TUK_Turret: return L"turret";
    case TUK_Vtol:
    case TUK_VtolDualEngine: return L"VTOL";
    case TUK_Artillery: return L"artillery piece";
    case TUK_Generator: return L"generator";
    case TUK_RadarTower: return L"radar tower";
    case TUK_SatelliteDish: return L"satellite dish";
    case TUK_BattleArmor: return L"Elemental"; // a battle armour suit's record (LidarExport ba4, 2026-10-17)
    default:
    {
        // a vehicle: its art key ("manticore") when it reads like a name, else "tank"
        std::wstring w = clean(m.ArtKey[0] ? m.ArtKey : m.Chassis, 32);
        bool ok = !w.empty() && w.size() <= 14;
        for (wchar_t c : w)
            ok &= iswalpha(c) || c == L' ';
        return ok ? w : L"tank";
    }
    }
}

std::wstring EnemyNameOf(uint32_t lo, uint32_t hi, TerrainMechInfo *info)
{
    TerrainMechInfo m = {};
    if (!MechInfoOf(lo, hi, m, false))
        return L"";
    if (info)
        *info = m;
    return UnitWord(m);
}

// Every unit LidarExport describes (its 1000 m scan).
int MechInfosAll(std::vector<TerrainMechInfo> &out)
{
    out.clear();
    if (!TerrainConnect())
        return 0;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_tr->H.FrameCounter;
        MemoryBarrier();
        if (g_tr->H.FrameCounterEnd != a)
        {
            Sleep(1);
            continue;
        }
        uint32_t n = g_tr->H.MechInfoCount;
        if (n > (uint32_t)TERR_MAX_MECHS)
            n = TERR_MAX_MECHS;
        out.resize(n);
        if (n)
            std::memcpy(out.data(), (const void *)g_tr->H.Mechs, n * sizeof(TerrainMechInfo));
        MemoryBarrier();
        if (g_tr->H.FrameCounter == a && g_tr->H.FrameCounterEnd == a)
        {
            out.erase(std::remove_if(out.begin(), out.end(), [](const TerrainMechInfo &m) { return !m.Valid; }), out.end());
            return (int)out.size();
        }
    }
    out.clear();
    return 0;
}

// Your locked target, as the pilots name it ("" when none / unknown).
std::wstring LockedEnemyName()
{
    std::vector<TerrainMechInfo> ms;
    MechInfosAll(ms);
    for (auto &m : ms)
        if (m.Locked && m.Hostile && !m.Dead)
            return UnitWord(m);
    return L"";
}

// "5 o'clock" / "500 metres" from your cockpit, for a contact on LidarExport's scan.
bool ContactBearing(uint16_t id, std::wstring &clockOut, std::wstring &rangeOut)
{
    LockedCount();
    if (!g_lid || g_lid->Magic != LIDAR_MAGIC)
        return false;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_lid->FrameCounterEnd; // bug sweep 2026-10-17: END first (the writer bumps FrameCounter first)
        MemoryBarrier();
        const float px = g_lid->PlayerX, py = g_lid->PlayerY, yaw = g_lid->PlayerYaw + g_lid->PlayerTorsoYaw;
        uint32_t n = g_lid->ContactCount;
        if (n > (uint32_t)LIDAR_MAX_CONTACTS)
            n = LIDAR_MAX_CONTACTS;
        bool found = false;
        float x = 0, y = 0;
        for (uint32_t i = 0; i < n && !found; ++i)
            if (g_lid->Contacts[i].Id == id)
            {
                x = g_lid->Contacts[i].X;
                y = g_lid->Contacts[i].Y;
                found = true;
            }
        MemoryBarrier();
        if (g_lid->FrameCounterEnd != a || g_lid->FrameCounter != a)
        {
            Sleep(1);
            continue;
        }
        if (!found)
            return false;
        const float dx = x - px, dy = y - py;
        float rel = std::atan2(dy, dx) * 57.29578f - yaw; // Unreal: yaw grows to the right
        rel = std::fmod(std::fmod(rel, 360.0f) + 360.0f, 360.0f);
        int clock = (int)std::lround(rel / 30.0f) % 12;
        if (clock == 0)
            clock = 12;
        const int metres = (int)std::lround(std::sqrt(dx * dx + dy * dy) / 100.0f / 50.0f) * 50;
        clockOut = std::to_wstring(clock) + L" o'clock";
        rangeOut = std::to_wstring(std::max(50, metres)) + L" metres";
        return true;
    }
    return false;
}

std::wstring StatusDamage(const float *armour, const float *structure, int slot); // STATUS REPORT, below
bool UnitPosNow(uint32_t lo, uint32_t hi, float &x, float &y, bool &dead, bool needSight = true); // below
std::wstring LowerW(std::wstring s);                                                // below

// A contact's position on LidarExport's scan (its ~3 s ping), or false.
bool ContactXY(uint16_t id, float &x, float &y)
{
    LockedCount();
    if (!g_lid || g_lid->Magic != LIDAR_MAGIC)
        return false;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_lid->FrameCounterEnd; // bug sweep 2026-10-17: END first (the writer bumps FrameCounter first)
        MemoryBarrier();
        uint32_t n = g_lid->ContactCount;
        if (n > (uint32_t)LIDAR_MAX_CONTACTS)
            n = LIDAR_MAX_CONTACTS;
        bool found = false;
        for (uint32_t i = 0; i < n && !found; ++i)
            if (g_lid->Contacts[i].Id == id)
            {
                x = g_lid->Contacts[i].X;
                y = g_lid->Contacts[i].Y;
                found = true;
            }
        MemoryBarrier();
        if (g_lid->FrameCounterEnd == a && g_lid->FrameCounter == a)
            return found;
        Sleep(1);
    }
    return false;
}

// A world point as "5 o'clock" / "500 metres" from your cockpit now; metres out too.
bool BearingTo(float x, float y, std::wstring &clockOut, std::wstring &rangeOut, int *metresOut = nullptr)
{
    LockedCount();
    if (!g_lid || g_lid->Magic != LIDAR_MAGIC)
        return false;
    const float px = g_lid->PlayerX, py = g_lid->PlayerY, yaw = g_lid->PlayerYaw + g_lid->PlayerTorsoYaw;
    const float dx = x - px, dy = y - py;
    float rel = std::atan2(dy, dx) * 57.29578f - yaw;
    rel = std::fmod(std::fmod(rel, 360.0f) + 360.0f, 360.0f);
    int clock = (int)std::lround(rel / 30.0f) % 12;
    if (clock == 0)
        clock = 12;
    const int metres = (int)std::lround(std::sqrt(dx * dx + dy * dy) / 100.0f / 50.0f) * 50;
    clockOut = std::to_wstring(clock) + L" o'clock";
    if (metres >= 1000)
    {
        wchar_t km[32];
        swprintf_s(km, L"%.1f kilometres", metres / 1000.0);
        rangeOut = km; // "1.2 kilometres"
    }
    else
        rangeOut = std::to_wstring(std::max(50, metres)) + L" metres";
    if (metresOut)
        *metresOut = (int)(std::sqrt(dx * dx + dy * dy) / 100.0f);
    return true;
}

// ---- HIGH GROUND (2026-10-14, user): "get up on that hill" -> "which hill, Commander?" -----------------------------
// HillFind (pure, unit-tested on Linux): hilltops on the terrain lattice (TERR_SPACING_CM) 150-800 m from the player,
// all around: a local high point (nothing higher within 48 m), at least 10 m above the lowest ground 112 m around it
// (12 of 16 directions known), with a way up (2 of 8 directions drop at most 14 m in 32 m); the nearest first
// (2026-10-19, user: was ranked by that rise plus half its height above you, which picked tall, far hills); at least 200 m apart and 200 m from every nav point already there (the mission's and
// yours - so a hill already picked, now a nav, is never offered again). Returned nearest first.
struct HillCand
{
    float X, Y, Z;  // world cm (Z = the ground there)
    float Score;
};
std::vector<HillCand> HillFind(const std::function<float(float, float)> &h, float px, float py,
                               const std::vector<std::pair<float, float>> &avoid, int maxN, float minD = 15000.0f,
                               float maxD = 80000.0f)
{
    const float SP = TERR_SPACING_CM;
    const int   R = (int)(maxD / SP) + 9; // the band + the 112 m ring
    const int   W = 2 * R + 1;
    const int   ci = (int)std::floor(px / SP), cj = (int)std::floor(py / SP);
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> g((size_t)W * W, kNan);
    for (int j = 0; j < W; ++j)
        for (int i = 0; i < W; ++i)
        {
            const float v = h((ci - R + i + 0.5f) * SP, (cj - R + j + 0.5f) * SP);
            if (v > TERR_NO_HEIGHT * 0.5f)
                g[(size_t)j * W + i] = v;
        }
    auto at = [&](int i, int j) -> float { return (i < 0 || j < 0 || i >= W || j >= W) ? kNan : g[(size_t)j * W + i]; };
    float hp = h(px, py);
    if (!(hp > TERR_NO_HEIGHT * 0.5f))
        hp = at(R, R);
    std::vector<HillCand> all;
    for (int j = 0; j < W; ++j)
        for (int i = 0; i < W; ++i)
        {
            const float v = at(i, j);
            if (std::isnan(v))
                continue;
            const float x = (ci - R + i + 0.5f) * SP, y = (cj - R + j + 0.5f) * SP;
            const float d = std::sqrt((x - px) * (x - px) + (y - py) * (y - py));
            if (d < minD || d > maxD)
                continue;
            bool top = true;
            for (int b = -3; b <= 3 && top; ++b)
                for (int a = -3; a <= 3 && top; ++a)
                {
                    const float o = at(i + a, j + b);
                    top = std::isnan(o) || o <= v;
                }
            if (!top)
                continue;
            int   cnt = 0;
            float mn = 1e30f;
            for (int k = 0; k < 16; ++k)
            {
                const float an = k * 0.3926991f;
                const float o = at(i + (int)std::lround(7.0f * std::cos(an)), j + (int)std::lround(7.0f * std::sin(an)));
                if (std::isnan(o))
                    continue;
                ++cnt;
                mn = std::min(mn, o);
            }
            if (cnt < 12 || v - mn < 1000.0f) // 2026-10-19 (user): 10 m rise, was 15
                continue;
            static const int kD[8][2] = {{2, 0}, {2, 2}, {0, 2}, {-2, 2}, {-2, 0}, {-2, -2}, {0, -2}, {2, -2}};
            int way = 0;
            for (auto &dd : kD)
            {
                const float o = at(i + dd[0], j + dd[1]);
                way += !std::isnan(o) && v - o <= 1400.0f;
            }
            if (way < 2)
                continue;
            const float score = (v - mn) + 0.5f * (std::isnan(hp) ? 0.0f : std::max(0.0f, v - hp));
            all.push_back({x, y, v, score});
        }
    // 2026-10-19 (user): the NEAREST hills first (was the biggest rise + height above you - it picked tall, far hills).
    std::sort(all.begin(), all.end(), [&](const HillCand &a, const HillCand &b) {
        return (a.X - px) * (a.X - px) + (a.Y - py) * (a.Y - py) < (b.X - px) * (b.X - px) + (b.Y - py) * (b.Y - py);
    });
    std::vector<HillCand> out;
    const float kApart = 20000.0f * 20000.0f;
    for (auto &c : all)
    {
        if ((int)out.size() >= maxN)
            break;
        bool ok = true;
        for (auto &o : out)
            ok &= (o.X - c.X) * (o.X - c.X) + (o.Y - c.Y) * (o.Y - c.Y) >= kApart;
        for (auto &a : avoid)
            ok &= (a.first - c.X) * (a.first - c.X) + (a.second - c.Y) * (a.second - c.Y) >= kApart;
        if (ok)
            out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [&](const HillCand &a, const HillCand &b) {
        return (a.X - px) * (a.X - px) + (a.Y - py) * (a.Y - py) < (b.X - px) * (b.X - px) + (b.Y - py) * (b.Y - py);
    });
    return out;
}

// HillFind on LidarExport's terrain (the whole block mapped for the search only: the tiles are ~2 MB).
bool HillSearch(const std::vector<std::pair<float, float>> &avoid, int maxN, std::vector<HillCand> &out,
                const float *centre = nullptr, float minD = 15000.0f, float maxD = 80000.0f)
{
    out.clear();
    if (!TerrainConnect())
        return false;
    const auto *tb = static_cast<const TerrainBlock *>(MapViewOfFile(g_trmap, FILE_MAP_READ, 0, 0, sizeof(TerrainBlock)));
    if (!tb)
        return false;
    std::map<std::pair<int, int>, int> idx;
    uint32_t gen = 0;
    float    px = 0, py = 0;
    bool     ok = false;
    for (int tries = 0; tries < 8 && !ok; ++tries)
    {
        const uint32_t e = tb->H.FrameCounterEnd;
        MemoryBarrier();
        idx.clear();
        gen = tb->H.Generation;
        px = tb->H.PlayerX;
        py = tb->H.PlayerY;
        for (int k = 0; k < TERR_MAX_TILES; ++k)
        {
            const TerrainSlotInfo si = tb->H.Slots[k];
            if (si.State == TTS_Complete && si.Hits > 0)
                idx[{si.TileX, si.TileY}] = k;
        }
        MemoryBarrier();
        ok = tb->H.FrameCounter == e;
        if (!ok)
            Sleep(1);
    }
    if (ok && !idx.empty())
    {
        auto height = [&](float x, float y) -> float {
            const int tx = (int)std::floor(x / TERR_TILE_CM), ty = (int)std::floor(y / TERR_TILE_CM);
            const auto it = idx.find({tx, ty});
            if (it == idx.end())
                return TERR_NO_HEIGHT;
            const int col = std::max(0, std::min(TERR_SAMPLES - 1, (int)std::floor((x - tx * TERR_TILE_CM) / TERR_SPACING_CM)));
            const int row = std::max(0, std::min(TERR_SAMPLES - 1, (int)std::floor((y - ty * TERR_TILE_CM) / TERR_SPACING_CM)));
            return tb->Tiles[it->second].Height[row * TERR_SAMPLES + col];
        };
        if (centre)
        {
            px = centre[0];
            py = centre[1];
        }
        out = HillFind(height, px, py, avoid, maxN, minD, maxD);
        if (tb->H.Generation != gen) // the cache was dropped under us
        {
            out.clear();
            ok = false;
        }
    }
    else
        ok = false;
    UnmapViewOfFile(tb);
    return ok;
}

// The nav points now (seqlock copy, as NavLoc).
int NavSnapshot(VanillaExportBlock::NavPoint *out)
{
    if (!g_blk)
        return 0;
    for (int tries = 0; tries < 8; ++tries)
    {
        const uint32_t a = g_blk->NavSeq;
        if (a & 1)
        {
            Sleep(1);
            continue;
        }
        MemoryBarrier();
        const uint32_t n = std::min<uint32_t>(g_blk->NavCount, (uint32_t)VanillaExportBlock::kNavMax);
        std::memcpy(out, (const void *)g_blk->Navs, sizeof(VanillaExportBlock::NavPoint) * n);
        MemoryBarrier();
        if (g_blk->NavSeq == a)
            return (int)n;
    }
    return 0;
}

// ---- FIRE SETTINGS AND NAV MARKS ANSWERED (2026-10-19, user) -------------------------------------------------------
// LanceControl applies TacOps fire settings and nav marks itself (no LSE_OrderReceived), so they are watched instead:
// each lancemate's fire settings in the TacOps table (4 Hz) and your own nav points. A change VoiceCommand did not
// make (its own: g_vcSent / g_vcNavMs, 3 s) is answered - fire settings with the reply the voice order gets
// (grouped with TacAckFlush: one answer for the lance), nav marks by the lowest-numbered lancemate standing. A new
// unit in a slot, or a new mission's nav set, is taken as the starting point silently. Reads only (no TacWantMs).
struct FsSeen
{
    bool     On = false;
    uint8_t  D = 0, M = 0, H = 0;
    uint32_t Lo = 0, Hi = 0;
};
FsSeen g_fsSeen[16];
bool g_navSeenOn = false;
int  g_navSeenN = 0;
VanillaExportBlock::NavPoint g_navSeen[VanillaExportBlock::kNavMax];
void FireNavWatchTick(ULONGLONG now)
{
    static ULONGLONG last = 0;
    if (!g_blk || now - last < 250)
        return;
    last = now;
    VanillaExportBlock::TacMember m[VanillaExportBlock::kTacMaxMembers] = {};
    const int n = ReadLance(m);
    int speaker = 0;
    for (int i = 0; i < n; ++i)
    {
        const auto &u = m[i];
        if (!u.Valid || u.IsPlayer || u.Dead || u.Slot < 1 || u.Slot >= 16)
            continue;
        if (!speaker || u.Slot < speaker)
            speaker = u.Slot;
        FsSeen &f = g_fsSeen[u.Slot];
        const bool fresh = !f.On || f.Lo != u.ActorLo || f.Hi != u.ActorHi;
        const uint8_t od = f.D, om = f.M, oh = f.H;
        const bool same = od == u.FireDisc && om == u.FireMask && oh == u.FireHeat;
        f.On = true;
        f.D = u.FireDisc;
        f.M = u.FireMask;
        f.H = u.FireHeat;
        f.Lo = u.ActorLo;
        f.Hi = u.ActorHi;
        if (fresh || same)
            continue;
        if (g_vcSent[u.Slot].Order == VanillaExportBlock::TCO_FireSettings && now - g_vcSent[u.Slot].Ms < 3000)
            continue; // a voice order - Execute answered it
        const wchar_t *g;
        if (u.FireDisc == 0 && u.FireMask == kFcAll && u.FireHeat == 0)
            g = L"fire_reset";
        else if (u.FireDisc != od)
            g = u.FireDisc == 0 ? L"fire_weapons_free" : (u.FireDisc == 1 ? L"fire_return_fire" : L"fire_hold");
        else if (u.FireHeat != oh)
            g = u.FireHeat == 0 ? L"heat_normal" : (u.FireHeat == 1 ? L"heat_cool" : (u.FireHeat == 2 ? L"heat_hot" : L"heat_alpha"));
        else if (u.FireMask == kFcAll)
            g = L"weapons_all";
        else
            g = (u.FireMask & ~om) == 0 ? L"weapons_hold" : L"weapons_only";
        g_tacAck.push_back({(int)u.Slot, VanillaExportBlock::TCO_FireSettings, now, g});
    }

    VanillaExportBlock::NavPoint nv[VanillaExportBlock::kNavMax];
    const int nn = NavSnapshot(nv);
    bool newSet = !g_navSeenOn; // the game's own nav points changed: a new mission - start over, say nothing
    int gameNow = 0, gameSeen = 0;
    for (int k = 0; k < nn; ++k)
        gameNow += !nv[k].Mine;
    for (int k = 0; k < g_navSeenN; ++k)
        gameSeen += !g_navSeen[k].Mine;
    newSet |= gameNow != gameSeen;
    for (int k = 0; k < nn && !newSet; ++k)
        if (!nv[k].Mine)
        {
            bool found = false;
            for (int q = 0; q < g_navSeenN && !found; ++q)
                found = !g_navSeen[q].Mine && g_navSeen[q].Letter == nv[k].Letter;
            newSet |= !found;
        }
    if (!newSet && speaker && now - g_vcNavMs >= 3000)
    {
        auto has = [](const VanillaExportBlock::NavPoint *a, int na, const VanillaExportBlock::NavPoint &p) {
            for (int q = 0; q < na; ++q)
                if (a[q].Mine && a[q].Letter == p.Letter)
                    return true;
            return false;
        };
        for (int k = 0; k < nn; ++k)
            if (nv[k].Mine && !has(g_navSeen, g_navSeenN, nv[k]))
            {
                Log(L"tacops nav answered: Nav %ls marked (slot %d)", VhudNavName(nv[k].Letter), speaker);
                Say(speaker, Line(L"nav_marked", speaker, {{L"nav", NavSaid(nv[k].Letter)}}));
            }
        for (int k = 0; k < g_navSeenN; ++k)
            if (g_navSeen[k].Mine && !has(nv, nn, g_navSeen[k]))
            {
                Log(L"tacops nav answered: Nav %ls cleared (slot %d)", VhudNavName(g_navSeen[k].Letter), speaker);
                Say(speaker, Line(L"nav_cleared", speaker, {{L"nav", NavSaid(g_navSeen[k].Letter)}}));
            }
    }
    std::memcpy(g_navSeen, nv, sizeof(VanillaExportBlock::NavPoint) * (size_t)nn);
    g_navSeenN = nn;
    g_navSeenOn = true;
}

// The open offer (main thread only). Marked with TCO_NavMark (LanceControl gives the letters - the lowest free, never
// two navs with one letter); asked once the letters show (HillTick); 30 s for the answer, then the marks go.
struct HillOffer
{
    bool                  On = false, Asked = false;
    ULONGLONG             MarkMs = 0, AskMs = 0;
    int                   Speaker = 0;
    std::vector<int>      Who;    // the slots to send (0 = the lance)
    std::vector<HillCand> C;
    std::vector<int>      Letter; // per candidate, -1 until LanceControl lists it
    bool                  NavOnly = false; // 2026-10-14: no hill near the nav named - the question is "move there anyway?"
};
HillOffer g_hill;

void HillClear(int keep = -1) // the candidates' marks removed (but `keep`'s), the offer closed
{
    for (int i = 0; i < (int)g_hill.C.size() && !g_hill.NavOnly; ++i) // NavOnly: the mission's nav, never unmarked
        if (i != keep)
        {
            const float p[3] = {g_hill.C[i].X, g_hill.C[i].Y, g_hill.C[i].Z};
            Send(VanillaExportBlock::TCO_NavUnmark, g_hill.Speaker >= 1 ? g_hill.Speaker : 1, p); // yours nearest, within 60 m
        }
    g_hill = HillOffer();
    g_hillMs = 0;
    g_hillLetters = 0;
}

void HillStart(int who, int speaker, int nearNav)
{
    const ULONGLONG now = GetTickCount64();
    if (g_hill.On && !g_hill.Asked && now - g_hill.MarkMs < 1500) // the same sentence, another addressee ("Two and Three, ...")
    {
        if (who == 0)
            g_hill.Who.assign(1, 0);
        else if (std::find(g_hill.Who.begin(), g_hill.Who.end(), 0) == g_hill.Who.end() &&
                 std::find(g_hill.Who.begin(), g_hill.Who.end(), who) == g_hill.Who.end())
            g_hill.Who.push_back(who);
        return;
    }
    std::vector<std::pair<float, float>> old;
    for (auto &c : g_hill.C)
        old.push_back({c.X, c.Y});
    if (g_hill.On)
        Log(L"hill: a new request replaces the open offer");
    HillClear(); // a new request replaces an open offer (its marks go first - LanceControl runs the ring in order)
    VanillaExportBlock::NavPoint nv[VanillaExportBlock::kNavMax];
    const int nn = NavSnapshot(nv);
    float navP[3] = {};
    if (nearNav >= 0 && !NavLoc(nearNav, navP))
    {
        Say(speaker, Line(L"problem_no_nav", speaker, {{L"nav", NavSaid(nearNav)}}));
        return;
    }
    std::vector<std::pair<float, float>> avoid;
    int inUse = 0;
    for (int k = 0; k < nn; ++k)
    {
        if (nearNav >= 0 && nv[k].Letter == nearNav)
        {
            ++inUse; // the nav named: hills right by it may be offered (kept 100 m clear below)
            continue;
        }
        bool leaving = false; // one of the old offer's, being removed
        for (auto &o : old)
            leaving |= nv[k].Mine && std::fabs(o.first - nv[k].Loc[0]) < 100.0f && std::fabs(o.second - nv[k].Loc[1]) < 100.0f;
        if (!leaving)
            avoid.push_back({nv[k].Loc[0], nv[k].Loc[1]});
    }
    const int maxN = std::min(4, VanillaExportBlock::kNavMax - (int)avoid.size() - inUse);
    if (maxN <= 0)
    {
        Log(L"hill: no free nav points (%d in use)", (int)avoid.size());
        Say(speaker, Line(L"hill_full", speaker));
        return;
    }
    std::vector<HillCand> cs;
    const bool read = nearNav >= 0 ? HillSearch(avoid, maxN, cs, navP, 0.0f, 40000.0f) : HillSearch(avoid, maxN, cs);
    if (nearNav >= 0)
        for (size_t i = cs.size(); i-- > 0;) // not on top of the nav itself (a second mark there)
            if ((cs[i].X - navP[0]) * (cs[i].X - navP[0]) + (cs[i].Y - navP[1]) * (cs[i].Y - navP[1]) < 10000.0f * 10000.0f)
                cs.erase(cs.begin() + (long)i);
    if (cs.empty() && nearNav >= 0 && read)
    {
        // 2026-10-14 (user): no hill near that nav - offer to just go there ("yes" / the nav's name = a move-fire to it)
        g_hill.On = g_hill.Asked = g_hill.NavOnly = true;
        g_hill.MarkMs = g_hill.AskMs = now;
        g_hill.Speaker = speaker;
        g_hill.Who.assign(1, who);
        g_hill.C.assign(1, HillCand{navP[0], navP[1], navP[2], 0.0f});
        g_hill.Letter.assign(1, nearNav);
        g_hillLetters = 1u << nearNav;
        g_hillMs = now;
        Log(L"hill: no hilltop within 400 m of %ls - offered to move there", NavSaid(nearNav).c_str());
        Say(speaker, Line(L"hill_none_nav", speaker, {{L"nav", NavSaid(nearNav)}}));
        return;
    }
    if (cs.empty())
    {
        Log(L"hill: %ls", read ? L"no hilltop qualifies 150-800 m around you" : L"no terrain from LidarExport");
        Say(speaker, Line(L"hill_none", speaker));
        return;
    }
    g_hill.On = true;
    g_hill.MarkMs = now;
    g_hill.Speaker = speaker;
    g_hill.Who.assign(1, who);
    g_hill.C = cs;
    g_hill.Letter.assign(cs.size(), -1);
    for (auto &c : cs)
    {
        const float p[3] = {c.X, c.Y, c.Z};
        Send(VanillaExportBlock::TCO_NavMark, speaker, p);
    }
    Log(L"hill: %zu hilltops marked for %ls", cs.size(), who ? Callsign(who).c_str() : L"the lance");
}

// "move to Nav Foxtrot" while the offer is open, or a picked answer: that hill kept, the others removed.
void HillPickLetter(int letter)
{
    if (!g_hill.On || !g_hill.Asked)
        return;
    for (int i = 0; i < (int)g_hill.C.size(); ++i)
        if (g_hill.Letter[i] == letter)
        {
            Log(L"hill: Nav %ls picked", VhudNavName(letter));
            HillClear(i);
            return;
        }
}

void HillAnswer(int code) // main thread: Q_HillPick / Q_HillCancel
{
    if (!g_hill.On || !g_hill.Asked)
        return;
    const int sp = g_hill.Speaker, n = (int)g_hill.C.size();
    if (code == -2)
    {
        Say(sp, Line(L"hill_cancel", sp));
        HillClear();
        return;
    }
    int i = -1;
    if (code >= 100 && code < 200)
    {
        for (int k = 0; k < n; ++k)
            if (g_hill.Letter[k] == code - 100)
                i = k;
    }
    else if (code == 200)
    {
        i = 0;
        for (int k = 1; k < n; ++k)
            if (g_hill.C[k].Z > g_hill.C[i].Z)
                i = k;
    }
    else if (code == 201)
        i = 0; // nearest first
    else if (code == 202)
        i = n - 1;
    else if (code == 300)
        i = g_hill.NavOnly ? 0 : -1; // "yes" answers "move there anyway?", not "which hill?"
    else if (code >= 0 && code < n)
        i = code;
    if (i < 0 || i >= n || g_hill.Letter[i] < 0)
    {
        Say(sp, Line(L"say_again", sp));
        return;
    }
    const int letter = g_hill.Letter[i];
    const std::vector<int> who = g_hill.Who;
    for (int s : who) // a move to that nav, firing on the move - the first one closes the offer (HillPickLetter)
    {
        Cmd c{};
        c.Who = s;
        c.Type = C_Nav;
        c.A = letter;
        c.C = 0;
        Execute(c);
    }
}

void HillTick()
{
    if (!g_hill.On)
        return;
    const ULONGLONG now = GetTickCount64();
    if (!g_hill.Asked)
    {
        VanillaExportBlock::NavPoint nv[VanillaExportBlock::kNavMax];
        const int nn = NavSnapshot(nv);
        int found = 0;
        for (size_t i = 0; i < g_hill.C.size(); ++i)
        {
            g_hill.Letter[i] = -1;
            for (int k = 0; k < nn; ++k)
                if (nv[k].Mine && std::fabs(nv[k].Loc[0] - g_hill.C[i].X) < 100.0f && std::fabs(nv[k].Loc[1] - g_hill.C[i].Y) < 100.0f)
                    g_hill.Letter[i] = nv[k].Letter;
            found += g_hill.Letter[i] >= 0;
        }
        // LanceControl lists them on its next game tick - none while the game is paused (2026-10-14 test: given up
        // after 2.5 s in the pause menu, the plugin marked all four a second later and they were left behind)
        if (found < (int)g_hill.C.size() && now - g_hill.MarkMs < (found ? 2500u : 15000u))
            return;
        const int sp = g_hill.Speaker;
        if (!found)
        {
            Log(L"hill: the marks never showed in 15 s (LanceControl not running?) - removed");
            Say(sp, Line(L"hill_none", sp));
            HillClear(); // any the plugin takes later go again
            return;
        }
        for (size_t i = g_hill.C.size(); i-- > 0;) // any the plugin did not take
            if (g_hill.Letter[i] < 0)
            {
                g_hill.C.erase(g_hill.C.begin() + (long)i);
                g_hill.Letter.erase(g_hill.Letter.begin() + (long)i);
            }
        int hi = 0;
        for (int i = 1; i < (int)g_hill.C.size(); ++i)
            if (g_hill.C[i].Z > g_hill.C[hi].Z)
                hi = i;
        std::wstring list;
        for (int i = 0; i < (int)g_hill.C.size(); ++i)
        {
            std::wstring clock, range;
            BearingTo(g_hill.C[i].X, g_hill.C[i].Y, clock, range);
            list += (list.empty() ? L"" : L". ") + std::wstring(VhudNavName(g_hill.Letter[i])) + L", " + clock + L", " + range +
                    (i == hi && g_hill.C.size() > 1 ? L", the highest" : L"");
        }
        static const wchar_t *kCount[] = {L"no", L"one", L"two", L"three", L"four"};
        Say(sp, Line(L"hill_ask", sp, {{L"count", kCount[std::min<size_t>(4, g_hill.C.size())]}, {L"list", list}}));
        g_hill.Asked = true;
        g_hill.AskMs = now;
        {
            uint32_t bits = 0;
            for (int l : g_hill.Letter)
                if (l >= 0 && l < 32)
                    bits |= 1u << l;
            g_hillLetters = bits;
        }
        g_hillMs = now;
        Log(L"hill: asked - %ls", list.c_str());
        return;
    }
    if (now - g_hill.AskMs > 30000)
    {
        Log(L"hill: no answer in 30 s - %ls", g_hill.NavOnly ? L"the move-there question dropped" : L"marks removed");
        if (!g_hill.NavOnly)
            Say(g_hill.Speaker, Line(L"hill_expired", g_hill.Speaker));
        HillClear();
    }
}

// Where a unit is NOW: the units list (any range), else LidarExport's scan. dead: a wreck.
// needSight: only while the lance can see it (seen in the last 3 s) - "where?" answers what a
// pilot could know; an attack order may use the game's own position.
// A unit's facing (LidarExport's contact yaw, degrees) and position, while it is on the scan (2026-10-11).
bool UnitYawNow(uint32_t lo, uint32_t hi, float &yaw, float &x, float &y)
{
    TerrainMechInfo m = {};
    if (!MechInfoOf(lo, hi, m, false) || !g_lid || g_lid->Magic != LIDAR_MAGIC)
        return false;
    const uint32_t n = std::min<uint32_t>(g_lid->ContactCount, (uint32_t)LIDAR_MAX_CONTACTS);
    for (uint32_t i = 0; i < n; ++i)
        if (g_lid->Contacts[i].Id == m.Id)
        {
            yaw = g_lid->Contacts[i].Yaw;
            x = g_lid->Contacts[i].X;
            y = g_lid->Contacts[i].Y;
            return true;
        }
    return false;
}
bool UnitPosNow(uint32_t lo, uint32_t hi, float &x, float &y, bool &dead, bool needSight)
{
    dead = false;
    const uint64_t key = ((uint64_t)hi << 32) | lo;
    auto it = g_seen.find(key);
    const ULONGLONG now = GetTickCount64();
    if (it != g_seen.end() && now - it->second.Ms < 1500)
    {
        dead = it->second.Dead;
        if (needSight && !dead && !it->second.Seen && now - it->second.LMs > 3000)
            return false; // out of sight: the caller says "last known"
        x = it->second.X;
        y = it->second.Y;
        return true;
    }
    TerrainMechInfo m = {};
    if (MechInfoOf(lo, hi, m, false))
    {
        dead = m.Dead != 0;
        return ContactXY(m.Id, x, y) || dead;
    }
    return false;
}

std::wstring LowerW(std::wstring s)
{
    for (auto &c : s)
        c = (wchar_t)towlower(c);
    return s;
}

void NoteIntel(int slot, uint32_t lo, uint32_t hi, const std::wstring &name)
{
    if (slot < 1 || slot > 8)
        return;
    IntelNote n;
    n.Slot = slot;
    n.Lo = lo;
    n.Hi = hi;
    n.Ms = GetTickCount64();
    n.Name = name;
    bool dead = false;
    if (lo || hi)
        n.HavePos = UnitPosNow(lo, hi, n.X, n.Y, dead, false);
    g_intelLast = n;
    g_intelBySlot[slot] = n;
    g_lastReporter = slot;
    g_lastReporterMs = n.Ms;
    if (!name.empty())
    {
        g_intelByName[LowerW(name)] = n;
        if (g_intelByName.size() > 64)
            g_intelByName.clear();
    }
}

// The answer about one unit: where it is now (or dead / lost / never seen).
std::wstring WhereLine(int by, uint32_t lo, uint32_t hi, const std::wstring &name, const IntelNote *note)
{
    float x = 0, y = 0;
    bool dead = false;
    std::wstring clock, range;
    if ((lo || hi) && UnitPosNow(lo, hi, x, y, dead))
    {
        if (dead)
            return Line(L"answer_where_dead", by, {{L"enemy", name}});
        if (BearingTo(x, y, clock, range))
            return Line(L"answer_where", by, {{L"enemy", name}, {L"clock", clock}, {L"range", range}});
    }
    // out of sight (or gone): its LAST KNOWN position - where the lance last saw it, else where it was when named
    const ULONGLONG now = GetTickCount64();
    auto it = g_seen.find(((uint64_t)hi << 32) | lo);
    float lx = 0, ly = 0;
    ULONGLONG at = 0;
    if (it != g_seen.end())
    {
        if (it->second.Dead)
            return Line(L"answer_where_dead", by, {{L"enemy", name}});
        if (it->second.LMs)
        {
            lx = it->second.LX; // last SEEN
            ly = it->second.LY;
            at = it->second.LMs;
        }
    }
    if (!at && note && note->HavePos)
    {
        lx = note->X;
        ly = note->Y;
        at = note->Ms;
    }
    if (at && BearingTo(lx, ly, clock, range))
    {
        const int secs = (int)((now - at) / 1000);
        const std::wstring ago = secs < 10 ? L"a few seconds ago" : secs < 90 ? std::to_wstring(secs / 5 * 5) + L" seconds ago"
                                                                               : std::to_wstring(secs / 60) + L" minutes ago";
        return Line(L"answer_where_last", by, {{L"enemy", name}, {L"clock", clock}, {L"range", range}, {L"ago", ago}});
    }
    return Line(L"answer_where_unknown", by, {{L"enemy", name}});
}

// Same name, as said or heard: case, spaces and a plural "s" do not matter ("Dire Wolf" = "direwolves").
// 2026-10-10: numerals in their common form ("Atlas II" = "atlas two" = "Atlas 2").
bool SameUnitName(const std::wstring &a, const std::wstring &b)
{
    auto squash = [](const std::wstring &z) {
        std::wstring o;
        for (auto &t : NameTokens(z))
            o += t;
        if (o.size() > 3 && o.back() == L's')
            o.pop_back();
        return o;
    };
    return squash(a) == squash(b);
}
// A unit answers to a name: the same name, or (base) the name without its numeral when the name said had none.
bool UnitAnswersTo(const std::wstring &unit, const std::wstring &said, bool base)
{
    if (!base)
        return SameUnitName(unit, said);
    const std::wstring b = BaseUnitName(unit);
    // "Mad Cat Mark" with no number is the base name too (2026-10-10)
    std::vector<std::wstring> st = NameTokens(said);
    while (st.size() > 1 && st.back() == L"mk")
        st.pop_back();
    const std::wstring s2 = JoinWords(st);
    return !b.empty() && !HasNumeral(s2) && SameUnitName(b, s2);
}

// "attack that Bane": the last reported one of that name (5 min, still standing), else the nearest
// one LidarExport names that way. pos = where it is now; said = its name as the pilots say it.
// lo / hi (2026-10-16, lancecontrol-61): the unit's address, for an order on that exact unit (0 / 0 when not known).
bool FindNamedUnit(const std::wstring &name, float pos[3], std::wstring &said, uint32_t *lo, uint32_t *hi)
{
    if (lo)
        *lo = 0;
    if (hi)
        *hi = 0;
    const ULONGLONG now = GetTickCount64();
    const IntelNote *note = nullptr;
    for (int pass = 0; pass < 2 && !note; ++pass) // 2026-10-10: the exact name, then a numbered one ("Atlas" -> Atlas II)
        for (auto &kv : g_intelByName)
            if (UnitAnswersTo(kv.first, name, pass == 1) && now - kv.second.Ms < 300000 && (!note || kv.second.Ms > note->Ms))
                note = &kv.second;
    float x = 0, y = 0;
    bool dead = false;
    auto zOf = [&](uint32_t lo, uint32_t hi) {
        auto it = g_seen.find(((uint64_t)hi << 32) | lo);
        return it != g_seen.end() ? it->second.Z : (g_lid ? g_lid->PlayerZ : 0.0f);
    };
    if (note && (note->Lo || note->Hi) && UnitPosNow(note->Lo, note->Hi, x, y, dead, false) && !dead)
    {
        pos[0] = x;
        pos[1] = y;
        pos[2] = zOf(note->Lo, note->Hi);
        said = note->Name;
        if (lo)
            *lo = note->Lo;
        if (hi)
            *hi = note->Hi;
        return true;
    }
    std::vector<TerrainMechInfo> ms;
    MechInfosAll(ms);
    float best = 1e30f;
    bool found = false;
    LockedCount();
    for (int pass = 0; pass < 2 && !found; ++pass) // 2026-10-10: the exact name, then a numbered one
    for (auto &m : ms)
    {
        if (!m.Hostile || m.Dead || !UnitAnswersTo(UnitWord(m), name, pass == 1))
            continue;
        if (!UnitPosNow(m.ActorLo, m.ActorHi, x, y, dead, false) || dead || !g_lid)
            continue;
        const float d = (x - g_lid->PlayerX) * (x - g_lid->PlayerX) + (y - g_lid->PlayerY) * (y - g_lid->PlayerY);
        if (d < best)
        {
            best = d;
            pos[0] = x;
            pos[1] = y;
            pos[2] = zOf(m.ActorLo, m.ActorHi);
            said = UnitWord(m);
            found = true;
            if (lo)
                *lo = m.ActorLo;
            if (hi)
                *hi = m.ActorHi;
        }
    }
    return found;
}

// ---- SHADOW (build 2026-10-05n): "shadow it" / "get eyes on the Bane" / "follow that mech" -----
// The lancemate goes to where the lance last SAW the unit (lance status v4's Seen flag: a lancemate's
// own visible list, or within 500 m of you). When the lance sees it again: "found it, your 3 o'clock,
// 300 metres". Gives up after 90 s ("no sign of it"); "never mind, it's down" if it dies first.
struct ShadowJob
{
    uint32_t     Lo = 0, Hi = 0;
    std::wstring Name;
    ULONGLONG    Ms = 0; // 0 = none
};
ShadowJob g_shadow[9];

bool ShadowTarget(int slot, const std::wstring &name, uint32_t &lo, uint32_t &hi, std::wstring &said)
{
    const ULONGLONG now = GetTickCount64();
    lo = hi = 0;
    if (!name.empty())
    {
        const IntelNote *note = nullptr;
        for (int pass = 0; pass < 2 && !note; ++pass) // 2026-10-10: the exact name, then a numbered one
            for (auto &kv : g_intelByName)
                if (UnitAnswersTo(kv.first, name, pass == 1) && now - kv.second.Ms < 300000 && (!note || kv.second.Ms > note->Ms))
                    note = &kv.second;
        if (note && (note->Lo || note->Hi))
        {
            lo = note->Lo;
            hi = note->Hi;
            said = note->Name;
            return true;
        }
        std::vector<TerrainMechInfo> ms;
        MechInfosAll(ms);
        for (int pass = 0; pass < 2; ++pass) // 2026-10-10: the exact name, then a numbered one
        for (auto &m : ms)
            if (m.Hostile && !m.Dead && UnitAnswersTo(UnitWord(m), name, pass == 1))
            {
                lo = m.ActorLo;
                hi = m.ActorHi;
                said = UnitWord(m);
                return true;
            }
        return false;
    }
    const IntelNote *note = (slot >= 1 && slot <= 8 && g_intelBySlot[slot].Slot && now - g_intelBySlot[slot].Ms < 120000)
                                ? &g_intelBySlot[slot]
                                : (g_intelLast.Slot && now - g_intelLast.Ms < 120000 ? &g_intelLast : nullptr);
    if (note && (note->Lo || note->Hi))
    {
        lo = note->Lo;
        hi = note->Hi;
        said = note->Name;
        return true;
    }
    if (note && g_sit[note->Slot].AtkMs && now - g_sit[note->Slot].AtkMs < 15000)
    {
        said = EnemyNameOf(g_sit[note->Slot].AtkLo, g_sit[note->Slot].AtkHi);
        if (!said.empty())
        {
            lo = g_sit[note->Slot].AtkLo;
            hi = g_sit[note->Slot].AtkHi;
            return true;
        }
    }
    return false;
}

// 0 = no position known, 1 = out of sight: go to p (where it was last seen), 2 = in sight now, -1 = dead
int ShadowStart(int slot, uint32_t lo, uint32_t hi, const std::wstring &name, float p[3])
{
    const ULONGLONG now = GetTickCount64();
    auto it = g_seen.find(((uint64_t)hi << 32) | lo);
    if (it == g_seen.end() || now - it->second.Ms > 3000)
    {
        float x = 0, y = 0;
        bool dead = false;
        if (UnitPosNow(lo, hi, x, y, dead, true))
            return dead ? -1 : 2; // LidarExport's scan has it
        return 0;
    }
    if (it->second.Dead)
        return -1;
    if (it->second.Seen || (it->second.LMs && now - it->second.LMs < 3000))
        return 2;
    if (!it->second.LMs)
        return 0;
    p[0] = it->second.LX;
    p[1] = it->second.LY;
    p[2] = it->second.Z;
    if (slot >= 1 && slot <= 8)
    {
        g_shadow[slot].Lo = lo;
        g_shadow[slot].Hi = hi;
        g_shadow[slot].Name = name;
        g_shadow[slot].Ms = now;
    }
    return 1;
}

std::wstring WhereSeenLine(int by, uint32_t lo, uint32_t hi, const std::wstring &name)
{
    float x = 0, y = 0;
    bool dead = false;
    std::wstring clock, range;
    if (UnitPosNow(lo, hi, x, y, dead, true) && !dead && BearingTo(x, y, clock, range))
        return Line(L"answer_shadow_in_sight", by, {{L"enemy", name}, {L"clock", clock}, {L"range", range}});
    return Line(L"answer_where_unknown", by, {{L"enemy", name}});
}

// From the main loop, twice a second: a shadowing lancemate reports when the lance sees it again.
void ShadowTick()
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 500)
        return;
    last = now;
    for (int s = 1; s <= 8; ++s)
    {
        ShadowJob &j = g_shadow[s];
        if (!j.Ms)
            continue;
        auto it = g_seen.find(((uint64_t)j.Hi << 32) | j.Lo);
        const bool fresh = it != g_seen.end() && now - it->second.Ms < 3000;
        if (fresh && it->second.Dead)
        {
            Queue(s, K_ShadowLost, Line(L"callout_shadow_dead", s, {{L"enemy", j.Name}}));
            j = ShadowJob();
        }
        else if (fresh && it->second.Seen && now - j.Ms > 1500)
        {
            std::wstring clock, range;
            if (BearingTo(it->second.X, it->second.Y, clock, range))
                Queue(s, K_Found, Line(L"callout_found", s, {{L"enemy", j.Name}, {L"clock", clock}, {L"range", range}}));
            NoteIntel(s, j.Lo, j.Hi, j.Name); // so "where?" / "attack it" follow on
            j = ShadowJob();
        }
        else if (now - j.Ms > 90000)
        {
            Queue(s, K_ShadowLost, Line(L"callout_shadow_failed", s, {{L"enemy", j.Name}}));
            j = ShadowJob();
        }
    }
}

// ---- MORE QUESTIONS (build 2026-10-06a) -------------------------------------------------------
// how many / what are we facing / what's closest / who's shooting at you / what's your target /
// can you see it / is it down / ammo / heat / anyone hurt / sound off / how's the Atlas / any air /
// who's closest to it / say again. Answered from the lance status feed, the units list (any range)
// and LidarExport's scan (names, weight classes, condition: within 1000 m).
const wchar_t *PartWord(int p); // below

uint64_t UnitKey(uint32_t lo, uint32_t hi) { return ((uint64_t)hi << 32) | lo; }

std::wstring RangeWords(float metres)
{
    const int m = (int)std::lround(metres / 50.0f) * 50;
    if (m >= 1000)
    {
        wchar_t km[32];
        swprintf_s(km, L"%.1f kilometres", m / 1000.0);
        return km;
    }
    return std::to_wstring(std::max(50, m)) + L" metres";
}

std::wstring AgoWords(ULONGLONG ms)
{
    const int secs = (int)(ms / 1000);
    return secs < 10 ? L"a few seconds ago" : secs < 90 ? std::to_wstring(secs / 5 * 5) + L" seconds ago" : std::to_wstring(secs / 60) + L" minutes ago";
}

std::wstring NumWord(int n)
{
    static const wchar_t *kW[] = {L"no", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight", L"nine", L"ten", L"eleven", L"twelve"};
    return n >= 0 && n <= 12 ? kW[n] : std::to_wstring(n);
}

// "a heavy", "three mediums"
std::wstring CountPhrase(int n, const wchar_t *one, const wchar_t *many) { return (n == 1 ? std::wstring(L"a") : NumWord(n)) + L" " + (n == 1 ? one : many); }

// "a, b and c"
std::wstring JoinList(const std::vector<std::wstring> &v)
{
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i)
            s += i + 1 == v.size() ? L" and " : L", ";
        s += v[i];
    }
    return s;
}

// Mean armour + structure left (0..1) over the parts read, -1 = nothing read; lost = a side torso /
// arm / leg destroyed (-1 none).
float CondHealth(const float *arm, const float *st, int *lost)
{
    double sum = 0.0;
    int parts = 0;
    if (lost)
        *lost = -1;
    for (int p = 0; p < TMP_COUNT; ++p)
    {
        if (arm[p] < 0.0f && st[p] < 0.0f)
            continue;
        sum += std::max(0.0f, std::min(1.0f, arm[p])) + std::max(0.0f, std::min(1.0f, st[p]));
        ++parts;
        if (lost && *lost < 0 && p >= 2 && st[p] >= 0.0f && st[p] <= 0.01f)
            *lost = p;
    }
    return parts ? (float)(sum / (2.0 * parts)) : -1.0f;
}

// A wreck (the units list says so, or it dropped off a list that is still live), or LidarExport saw it die.
bool UnitDead(uint32_t lo, uint32_t hi)
{
    const ULONGLONG now = GetTickCount64();
    auto it = g_seen.find(UnitKey(lo, hi));
    if (it != g_seen.end())
        return it->second.Dead || (now - it->second.Ms > 5000 && now - g_seenDataMs < 2000);
    TerrainMechInfo m = {};
    return MechInfoOf(lo, hi, m, false) && m.Dead;
}

// The unit a question is about: "my target" (your lock), "your target" (the pilot's), a name (the last
// one reported, else the nearest so named), or what the pilot (else anyone) last reported.
struct QUnit
{
    uint32_t         Lo = 0, Hi = 0;
    std::wstring     Name;
    const IntelNote *Note = nullptr;
};
bool QuestionUnit(int by, const std::wstring &qname, const LsSlot *const *bySlot, QUnit &u)
{
    const ULONGLONG now = GetTickCount64();
    const std::wstring want = LowerW(qname);
    if (want == L"my target" || want == L"our target")
    {
        std::vector<TerrainMechInfo> ms;
        MechInfosAll(ms);
        for (auto &m : ms)
            if (m.Locked && m.Hostile)
            {
                u.Lo = m.ActorLo;
                u.Hi = m.ActorHi;
                u.Name = UnitWord(m);
                return true;
            }
        return false;
    }
    if (want == L"your target")
    {
        if (by < 1 || by > 8 || !bySlot[by] || (!bySlot[by]->TargetLo && !bySlot[by]->TargetHi))
            return false;
        u.Lo = bySlot[by]->TargetLo;
        u.Hi = bySlot[by]->TargetHi;
        u.Name = EnemyNameOf(u.Lo, u.Hi);
        if (u.Name.empty())
            u.Name = L"target";
        return true;
    }
    if (!want.empty())
    {
        auto squash = [](std::wstring z) {
            z = LowerW(z);
            z.erase(std::remove(z.begin(), z.end(), L' '), z.end());
            if (z.size() > 3 && z.back() == L's')
                z.pop_back();
            return z;
        };
        const std::wstring key = squash(want);
        for (auto &kv : g_intelByName)
            if (squash(kv.first) == key && now - kv.second.Ms < 300000 && (kv.second.Lo || kv.second.Hi) && (!u.Note || kv.second.Ms > u.Note->Ms))
                u.Note = &kv.second;
        if (u.Note)
        {
            u.Lo = u.Note->Lo;
            u.Hi = u.Note->Hi;
            u.Name = u.Note->Name;
            return true;
        }
        std::vector<TerrainMechInfo> ms;
        MechInfosAll(ms);
        float bestD = 1e30f;
        for (auto &m : ms)
        {
            if (!m.Hostile || squash(UnitWord(m)) != key || !g_lid)
                continue;
            float x = 0, y = 0;
            bool dd = false;
            if (!UnitPosNow(m.ActorLo, m.ActorHi, x, y, dd, false))
                continue;
            float d = (x - g_lid->PlayerX) * (x - g_lid->PlayerX) + (y - g_lid->PlayerY) * (y - g_lid->PlayerY);
            if (dd || m.Dead)
                d += 1e18f; // a live one first
            if (d < bestD)
            {
                bestD = d;
                u.Lo = m.ActorLo;
                u.Hi = m.ActorHi;
                u.Name = UnitWord(m);
            }
        }
        return u.Lo || u.Hi;
    }
    const IntelNote *note = nullptr;
    if (by >= 1 && by <= 8 && g_intelBySlot[by].Slot && now - g_intelBySlot[by].Ms < 90000)
        note = &g_intelBySlot[by];
    else if (g_intelLast.Slot && now - g_intelLast.Ms < 90000)
        note = &g_intelLast;
    if (!note)
        return false;
    u.Note = note;
    u.Lo = note->Lo;
    u.Hi = note->Hi;
    u.Name = note->Name;
    const int ns = note->Slot;
    if (!u.Lo && !u.Hi && ns >= 1 && ns <= 8 && g_sit[ns].AtkMs && now - g_sit[ns].AtkMs < 10000)
    {
        u.Lo = g_sit[ns].AtkLo; // it did not say who - the game knows who hit it last
        u.Hi = g_sit[ns].AtkHi;
        u.Name = EnemyNameOf(u.Lo, u.Hi);
    }
    if (u.Name.empty())
        u.Name = L"contact";
    return u.Lo || u.Hi;
}

void AnswerMore(int q, int by, const std::wstring &qname, const std::vector<int> &alive, const LsSlot *const *bySlot,
                const LsCondition *const *condOf, const LsSlot *slots, int n)
{
    const ULONGLONG now = GetTickCount64();
    auto anyone = [&]() { return alive[(size_t)rand() % alive.size()]; };
    auto isAlive = [&](int s) { return s >= 1 && s <= 8 && bySlot[s]; };
    auto health = [&](int s, int *lost) -> float {
        const LsCondition *c = condOf[s];
        if (c && c->Have)
            return CondHealth(c->Armour, c->Structure, lost);
        TerrainMechInfo m = {};
        if (bySlot[s] && MechConditionOf(bySlot[s]->ActorLo, bySlot[s]->ActorHi, m))
            return CondHealth(m.Armour, m.Structure, lost);
        if (lost)
            *lost = -1;
        return -1.0f;
    };
    auto where = [&](uint32_t lo, uint32_t hi, std::wstring &clock, std::wstring &range) {
        float x = 0, y = 0;
        bool dd = false;
        return UnitPosNow(lo, hi, x, y, dd, false) && BearingTo(x, y, clock, range);
    };
    // The nearest hostile the lance can see now (units list, any range), else on LidarExport's scan.
    auto closest = [&](uint32_t &blo, uint32_t &bhi, std::wstring &bc, std::wstring &br) {
        int bestM = 1 << 30;
        for (auto &kv : g_seen)
        {
            const SeenAt &a = kv.second;
            if (a.Dead || !a.Seen || now - a.LMs > 3000 || now - a.Ms > 3000)
                continue;
            std::wstring c, r;
            int m = 0;
            if (!BearingTo(a.X, a.Y, c, r, &m) || m >= bestM)
                continue;
            bestM = m;
            bc = c;
            br = r;
            blo = (uint32_t)kv.first;
            bhi = (uint32_t)(kv.first >> 32);
        }
        if (blo || bhi)
            return true;
        std::vector<TerrainMechInfo> ms;
        MechInfosAll(ms);
        for (auto &m : ms)
        {
            if (!m.Hostile || m.Dead)
                continue;
            float x = 0, y = 0;
            bool dd = false;
            std::wstring c, r;
            int mm = 0;
            if (!UnitPosNow(m.ActorLo, m.ActorHi, x, y, dd) || dd || !BearingTo(x, y, c, r, &mm) || mm >= bestM)
                continue;
            bestM = mm;
            bc = c;
            br = r;
            blo = m.ActorLo;
            bhi = m.ActorHi;
        }
        return blo || bhi;
    };
    // A question about a unit goes to the pilot asked, else whoever reported it, else anyone.
    auto reporter = [&](const QUnit &u) { return by ? by : (u.Note && isAlive(u.Note->Slot) ? u.Note->Slot : anyone()); };
    switch (q)
    {
    case Q_HowMany:
    case Q_Facing:
    {
        const int s = by ? by : anyone();
        std::vector<TerrainMechInfo> ms;
        MechInfosAll(ms);
        int cls[4] = {}, mechs = 0, veh = 0, tur = 0, air = 0, ba = 0;
        std::vector<std::wstring> assaults;
        for (auto &m : ms)
        {
            if (!m.Hostile || m.Dead)
                continue;
            if (m.Kind == TUK_Mech)
            {
                ++mechs;
                if (m.WeightClass <= 3)
                    ++cls[m.WeightClass];
                if (m.WeightClass == 3)
                    assaults.push_back(UnitWord(m));
            }
            else if (m.Kind == TUK_Tank || m.Kind == TUK_Artillery)
                ++veh;
            else if (m.Kind == TUK_Turret)
                ++tur;
            else if (m.Kind == TUK_Vtol || m.Kind == TUK_VtolDualEngine)
                ++air;
        }
        int total = 0;
        {
            std::vector<LsUnit> us;
            LsReadUnits(us);
            for (auto &x : us)
                if (!x.Dead)
                    ++total;
        }
        {
            // 2026-10-17 (user): battle armour suits - LidarExport's contacts (a suit has no mech record)
            std::vector<LidarContact> bas;
            if (ScanKind(8, bas))
                ba = (int)bas.size();
        }
        const int known = mechs + veh + tur + air + ba;
        if (q == Q_HowMany)
        {
            if (total < known)
                total = known;
            if (!total)
            {
                Say(s, Line(L"answer_how_many_none", s));
                return;
            }
            if (!known)
            {
                Say(s, Line(L"answer_how_many_far", s, {{L"count", NumWord(total)}}));
                return;
            }
            std::vector<std::wstring> l;
            if (mechs)
                l.push_back(CountPhrase(mechs, L"mech", L"mechs"));
            if (veh)
                l.push_back(CountPhrase(veh, L"vehicle", L"vehicles"));
            if (tur)
                l.push_back(CountPhrase(tur, L"turret", L"turrets"));
            if (air)
                l.push_back(CountPhrase(air, L"VTOL", L"VTOLs"));
            if (ba)
                l.push_back(CountPhrase(ba, L"battle armour suit", L"battle armour suits"));
            if (total > known)
                l.push_back(NumWord(total - known) + L" more out past the scan");
            std::wstring t = Line(total == 1 ? L"answer_how_many_one" : L"answer_how_many", s, {{L"count", NumWord(total)}, {L"list", JoinList(l)}});
            // ... and the nearest one in sight (build 2026-10-06b)
            uint32_t blo = 0, bhi = 0;
            std::wstring bc, br;
            if (closest(blo, bhi, bc, br))
            {
                std::wstring name = EnemyNameOf(blo, bhi);
                if (name.empty())
                    name = L"contact";
                AddPiece(t, Line(L"answer_how_many_nearest", s, {{L"enemy", name}, {L"clock", bc}, {L"range", br}}));
                NoteIntel(s, blo, bhi, name);
            }
            Say(s, t, K_Position);
            return;
        }
        if (!known)
        {
            Say(s, Line(L"answer_facing_none", s));
            return;
        }
        std::vector<std::wstring> l;
        if (!assaults.empty() && assaults.size() <= 2)
            for (auto &x : assaults)
                l.push_back(L"a " + x);
        else if (cls[3])
            l.push_back(CountPhrase(cls[3], L"assault", L"assaults"));
        if (cls[2])
            l.push_back(CountPhrase(cls[2], L"heavy", L"heavies"));
        if (cls[1])
            l.push_back(CountPhrase(cls[1], L"medium", L"mediums"));
        if (cls[0])
            l.push_back(CountPhrase(cls[0], L"light", L"lights"));
        const int other = mechs - cls[0] - cls[1] - cls[2] - cls[3];
        if (other > 0)
            l.push_back(CountPhrase(other, L"mech", L"mechs"));
        if (veh)
            l.push_back(CountPhrase(veh, L"vehicle", L"vehicles"));
        if (tur)
            l.push_back(CountPhrase(tur, L"turret", L"turrets"));
        if (air)
            l.push_back(CountPhrase(air, L"VTOL", L"VTOLs"));
        if (ba)
            l.push_back(CountPhrase(ba, L"battle armour suit", L"battle armour suits"));
        Say(s, Line(cls[3] || cls[2] ? L"answer_facing_heavy" : L"answer_facing", s, {{L"list", JoinList(l)}}));
        return;
    }
    case Q_Class: // "any heavies?" (2026-10-07j): how many of that kind on the scan, and the nearest one
    {
        const int s = by ? by : anyone();
        if (qname == L"battlearmor")
        {
            // 2026-10-17 (user): battle armour suits - from LidarExport's contacts (a suit has no mech record), the nearest
            // by clock bearing
            std::vector<LidarContact> cs;
            ScanKind(8, cs);
            int bm = 1 << 30;
            std::wstring bc, br;
            for (auto &c : cs)
            {
                std::wstring c1, r1;
                int mm = 0;
                if (BearingTo(c.X, c.Y, c1, r1, &mm) && mm < bm)
                {
                    bm = mm;
                    bc = c1;
                    br = r1;
                }
            }
            if (cs.empty())
            {
                Say(s, Line(L"answer_class_none", s, {{L"class", L"battle armour"}}));
                return;
            }
            std::wstring t = Line(L"answer_class", s, {{L"list", CountPhrase((int)cs.size(), L"battle armour suit", L"battle armour suits")}});
            if (!bc.empty())
                AddPiece(t, Line(L"answer_how_many_nearest", s, {{L"enemy", L"battle armour suit"}, {L"clock", bc}, {L"range", br}}));
            Say(s, t, K_Position);
            return;
        }
        std::vector<TerrainMechInfo> ms;
        MechInfosAll(ms);
        int count = 0, bestM = 1 << 30;
        const TerrainMechInfo *best = nullptr;
        std::wstring bc, br;
        for (auto &m : ms)
        {
            if (!m.Hostile || m.Dead)
                continue;
            bool is = false;
            if (qname == L"turret")
                is = m.Kind == TUK_Turret;
            else if (qname == L"vehicle")
                is = m.Kind == TUK_Tank || m.Kind == TUK_Artillery;
            else if (qname == L"mech")
                is = m.Kind == TUK_Mech;
            else
            {
                const int want = qname == L"light" ? 0 : qname == L"medium" ? 1 : qname == L"heavy" ? 2 : 3;
                is = m.Kind == TUK_Mech && m.WeightClass == want;
            }
            if (!is)
                continue;
            ++count;
            float x = 0, y = 0;
            bool dd = false;
            std::wstring c, r;
            int mm = 0;
            if (UnitPosNow(m.ActorLo, m.ActorHi, x, y, dd) && !dd && BearingTo(x, y, c, r, &mm) && mm < bestM)
            {
                bestM = mm;
                best = &m;
                bc = c;
                br = r;
            }
        }
        const std::wstring one = qname == L"heavy" ? L"heavy" : qname == L"assault" ? L"assault" : qname == L"medium" ? L"medium"
                               : qname == L"light" ? L"light" : qname == L"turret" ? L"turret" : qname == L"vehicle" ? L"vehicle" : L"mech";
        const std::wstring many = qname == L"heavy" ? L"heavies" : one + L"s";
        if (!count)
        {
            Say(s, Line(L"answer_class_none", s, {{L"class", many}}));
            return;
        }
        std::wstring t = Line(L"answer_class", s, {{L"list", CountPhrase(count, one.c_str(), many.c_str())}});
        if (best)
        {
            AddPiece(t, Line(L"answer_how_many_nearest", s, {{L"enemy", UnitWord(*best)}, {L"clock", bc}, {L"range", br}}));
            NoteIntel(s, best->ActorLo, best->ActorHi, UnitWord(*best));
        }
        Say(s, t, K_Position);
        return;
    }
    case Q_Closest:
    {
        const int s = by ? by : anyone();
        uint32_t blo = 0, bhi = 0;
        std::wstring bc, br;
        closest(blo, bhi, bc, br);
        if (!blo && !bhi)
        {
            Say(s, Line(L"answer_closest_none", s));
            return;
        }
        std::wstring name = EnemyNameOf(blo, bhi);
        if (name.empty())
            name = L"contact";
        Say(s, Line(L"answer_closest", s, {{L"enemy", name}, {L"clock", bc}, {L"range", br}}), K_Position);
        NoteIntel(s, blo, bhi, name);
        return;
    }
    case Q_WhoHits:
    {
        int s = by;
        if (!s) // whoever was hit last
        {
            ULONGLONG best = 0;
            for (int x : alive)
                if (g_sit[x].AtkMs && now - g_sit[x].AtkMs < 15000 && g_sit[x].AtkMs > best)
                {
                    best = g_sit[x].AtkMs;
                    s = x;
                }
        }
        if (!s)
            s = anyone();
        const SitSlot &S = g_sit[s];
        if (!S.AtkMs || now - S.AtkMs > 15000 || (!S.AtkLo && !S.AtkHi))
        {
            Say(s, Line(L"answer_who_hits_none", s));
            return;
        }
        std::wstring name = EnemyNameOf(S.AtkLo, S.AtkHi);
        if (name.empty())
            name = L"contact";
        std::wstring clock, range;
        if (where(S.AtkLo, S.AtkHi, clock, range))
            Say(s, Line(L"answer_who_hits", s, {{L"enemy", name}, {L"clock", clock}, {L"range", range}}), K_Position);
        else
            Say(s, Line(L"answer_who_hits_noloc", s, {{L"enemy", name}}));
        NoteIntel(s, S.AtkLo, S.AtkHi, name);
        return;
    }
    case Q_Target:
    {
        std::vector<int> them;
        if (by)
            them.push_back(by);
        else
            them = alive;
        std::sort(them.begin(), them.end());
        for (int s : them)
        {
            const LsSlot *u = bySlot[s];
            if (!u->TargetLo && !u->TargetHi)
            {
                Say(s, Line(L"answer_target_none", s));
                continue;
            }
            std::wstring name = EnemyNameOf(u->TargetLo, u->TargetHi);
            if (name.empty())
                name = L"contact";
            std::wstring clock, range;
            if (where(u->TargetLo, u->TargetHi, clock, range))
                Say(s, Line(L"answer_target", s, {{L"enemy", name}, {L"clock", clock}, {L"range", range}}), K_Position);
            else
                Say(s, Line(L"answer_target_noloc", s, {{L"enemy", name}}));
            if (them.size() == 1)
                NoteIntel(s, u->TargetLo, u->TargetHi, name);
        }
        return;
    }
    case Q_CanSee:
    case Q_IsDown:
    case Q_UnitHow:
    case Q_WhoClosest:
    case Q_Heading:
    {
        QUnit u;
        if (!QuestionUnit(by, qname, bySlot, u))
        {
            const int s = by ? by : anyone();
            const std::wstring lw = LowerW(qname);
            std::wstring nm = qname;
            if (!nm.empty())
                nm[0] = (wchar_t)towupper(nm[0]);
            if (lw == L"my target" || lw == L"our target")
                Say(s, Line(L"answer_mytarget_none", s));
            else if (lw == L"your target")
                Say(s, Line(L"answer_target_none", s));
            else if (!lw.empty())
                Say(s, Line(L"answer_where_named_none", s, {{L"enemy", nm}}));
            else
                Say(s, Line(L"answer_where_none", s));
            return;
        }
        const bool dead = UnitDead(u.Lo, u.Hi);
        if (q == Q_WhoClosest)
        {
            float x = 0, y = 0;
            bool dd = false;
            if (dead || !UnitPosNow(u.Lo, u.Hi, x, y, dd, false))
            {
                const int s = reporter(u);
                Say(s, Line(dead ? L"answer_down_yes" : L"answer_where_unknown", s, {{L"enemy", u.Name}}));
                return;
            }
            int c = 0;
            float cd = 1e30f;
            for (int s : alive)
            {
                const float dx = bySlot[s]->Loc[0] - x, dy = bySlot[s]->Loc[1] - y, d = std::sqrt(dx * dx + dy * dy);
                if (d < cd)
                {
                    cd = d;
                    c = s;
                }
            }
            Say(c, Line(L"answer_closest_me", c, {{L"enemy", u.Name}, {L"range", RangeWords(cd / 100.0f)}}));
            NoteIntel(c, u.Lo, u.Hi, u.Name);
            return;
        }
        const int s = reporter(u);
        TerrainMechInfo m = {};
        int lost = -1;
        if (q == Q_IsDown)
        {
            if (dead)
            {
                Say(s, Line(L"answer_down_yes", s, {{L"enemy", u.Name}}));
                return;
            }
            const float h = MechConditionOf(u.Lo, u.Hi, m) ? CondHealth(m.Armour, m.Structure, &lost) : -1.0f;
            Say(s, Line(h >= 0.0f && h < 0.4f ? L"answer_down_no_hurt" : L"answer_down_no", s, {{L"enemy", u.Name}}));
            return;
        }
        if (q == Q_Heading) // "which way is the Shadow Hawk facing?" (2026-10-11)
        {
            float yaw = 0, ex = 0, ey = 0;
            if (dead)
            {
                Say(s, Line(L"answer_where_dead", s, {{L"enemy", u.Name}}));
                return;
            }
            if (!UnitYawNow(u.Lo, u.Hi, yaw, ex, ey) || !g_lid)
            {
                Say(s, Line(L"answer_where_unknown", s, {{L"enemy", u.Name}}));
                return;
            }
            auto diff = [](float a, float b) {
                float d = std::fmod(a - b, 360.0f);
                if (d > 180.0f)
                    d -= 360.0f;
                if (d < -180.0f)
                    d += 360.0f;
                return std::fabs(d);
            };
            const float toMe = std::atan2(g_lid->PlayerY - ey, g_lid->PlayerX - ex) * 57.29578f;
            const float dMe = diff(yaw, toMe);
            if (dMe <= 35.0f)
                Say(s, Line(L"answer_heading_you", s, {{L"enemy", u.Name}}));
            else
            {
                int at = 0;
                float best = 25.0f;
                for (int sl : alive)
                {
                    const float b = std::atan2(bySlot[sl]->Loc[1] - ey, bySlot[sl]->Loc[0] - ex) * 57.29578f;
                    if (diff(yaw, b) <= best)
                    {
                        best = diff(yaw, b);
                        at = sl;
                    }
                }
                std::wstring clock, range;
                if (at)
                    Say(s, Line(L"answer_heading_lancemate", s, {{L"enemy", u.Name}, {L"who", at == s ? std::wstring(L"me") : NumberName(at)}}));
                else if (dMe >= 135.0f)
                    Say(s, Line(L"answer_heading_away", s, {{L"enemy", u.Name}}));
                else if (BearingTo(g_lid->PlayerX + std::cos(yaw / 57.29578f) * 100000.0f, g_lid->PlayerY + std::sin(yaw / 57.29578f) * 100000.0f,
                                   clock, range))
                    Say(s, Line(L"answer_heading_side", s, {{L"enemy", u.Name}, {L"clock", clock}}));
                else
                    Say(s, Line(L"answer_heading_away", s, {{L"enemy", u.Name}}));
            }
            NoteIntel(s, u.Lo, u.Hi, u.Name);
            return;
        }
        if (q == Q_CanSee)
        {
            if (dead)
            {
                Say(s, Line(L"answer_where_dead", s, {{L"enemy", u.Name}}));
                return;
            }
            auto it = g_seen.find(UnitKey(u.Lo, u.Hi));
            std::wstring clock, range;
            if (it != g_seen.end() && it->second.Seen && now - it->second.LMs < 3000 && BearingTo(it->second.X, it->second.Y, clock, range))
                Say(s, Line(L"answer_see_yes", s, {{L"enemy", u.Name}, {L"clock", clock}, {L"range", range}}), K_Position);
            else if (it != g_seen.end() && it->second.LMs)
                Say(s, Line(L"answer_see_no", s, {{L"enemy", u.Name}, {L"ago", AgoWords(now - it->second.LMs)}}));
            else
                Say(s, Line(L"answer_see_never", s, {{L"enemy", u.Name}}));
            NoteIntel(s, u.Lo, u.Hi, u.Name);
            return;
        }
        // Q_UnitHow
        if (dead)
        {
            Say(s, Line(L"answer_down_yes", s, {{L"enemy", u.Name}}));
            return;
        }
        const float h = MechConditionOf(u.Lo, u.Hi, m) ? CondHealth(m.Armour, m.Structure, &lost) : -1.0f;
        if (h < 0.0f)
        {
            Say(s, Line(L"answer_unit_how_unknown", s, {{L"enemy", u.Name}}));
            return;
        }
        std::wstring t = Line(h >= 0.85f ? L"answer_unit_how_fresh" : h >= 0.55f ? L"answer_unit_how_damaged"
                                                                    : h >= 0.25f ? L"answer_unit_how_bad"
                                                                                 : L"answer_unit_how_dying",
                              s, {{L"enemy", u.Name}});
        if (lost >= 0 && m.Kind == TUK_Mech)
            AddPiece(t, Line(L"answer_unit_how_lost", s, {{L"part", PartWord(lost)}}));
        Say(s, t);
        NoteIntel(s, u.Lo, u.Hi, u.Name);
        return;
    }
    case Q_Ammo:
    case Q_Heat:
    {
        std::vector<int> them;
        if (by)
            them.push_back(by);
        else
            them = alive;
        std::sort(them.begin(), them.end());
        for (int s : them)
            Say(s, StatusLine(s, q == Q_Ammo ? ST_Ammo : ST_HeatAlways));
        return;
    }
    case Q_Hurt:
    {
        float hs[9] = {};
        int lostP[9] = {};
        for (int x : alive)
            hs[x] = health(x, &lostP[x]);
        int s = by;
        if (!s) // the one in the best shape answers
        {
            float best = -2.0f;
            for (int x : alive)
                if (hs[x] > best)
                {
                    best = hs[x];
                    s = x;
                }
        }
        std::vector<std::wstring> pieces;
        bool anyFine = false;
        for (int i = 0; i < n; ++i)
            if (slots[i].Valid && !slots[i].IsPlayer && !slots[i].Alive && slots[i].Slot >= 1 && slots[i].Slot <= 8)
                pieces.push_back(Line(L"hurt_down", s, {{L"who", Callsign(slots[i].Slot)}}, false));
        std::vector<int> them = alive;
        std::sort(them.begin(), them.end());
        for (int x : them)
        {
            const bool me = x == s;
            const float h = hs[x];
            if (h < 0.0f)
                continue; // nothing read
            if (lostP[x] >= 0)
                pieces.push_back(Line(me ? L"hurt_self_lost" : L"hurt_lost", s, {{L"who", Callsign(x)}, {L"part", PartWord(lostP[x])}}, false));
            else if (h < 0.5f)
                pieces.push_back(Line(me ? L"hurt_self_bad" : L"hurt_bad", s, {{L"who", Callsign(x)}}, false));
            else if (h < 0.8f)
                pieces.push_back(Line(me ? L"hurt_self_some" : L"hurt_some", s, {{L"who", Callsign(x)}}, false));
            else
                anyFine = true;
        }
        if (pieces.empty())
        {
            Say(s, Line(L"answer_hurt_none", s));
            return;
        }
        std::wstring t = Line(L"answer_hurt_intro", s);
        for (auto p : pieces)
        {
            while (!p.empty() && (p.back() == L'.' || p.back() == L' ' || p.back() == L'!'))
                p.pop_back();
            if (p.empty())
                continue;
            p[0] = (wchar_t)towupper(p[0]);
            AddPiece(t, p + L".");
        }
        if (anyFine)
            AddPiece(t, Line(L"hurt_rest_fine", s));
        Say(s, t);
        return;
    }
    case Q_RadioCheck: // 2026-10-13: everyone in turn (or the one asked) - "Two, loud and clear"
    {
        std::vector<int> them = by > 0 ? std::vector<int>{by} : alive;
        std::sort(them.begin(), them.end());
        for (int s : them)
            Say(s, Line(L"checkin_radio", s));
        return;
    }
    case Q_SoundOff:
    {
        // "Two, report in": only that pilot (2026-10-11, user); no callsign / "all" / "lance": everyone in turn
        std::vector<int> them = by > 0 ? std::vector<int>{by} : alive;
        std::sort(them.begin(), them.end());
        for (int s : them)
        {
            const float h = health(s, nullptr);
            Say(s, Line(h < 0.0f || h >= 0.8f ? L"checkin_green" : h >= 0.5f ? L"checkin_light" : h >= 0.25f ? L"checkin_hurt" : L"checkin_bad", s));
        }
        return;
    }
    case Q_Air:
    {
        const int s = by ? by : anyone();
        std::vector<TerrainMechInfo> ms;
        MechInfosAll(ms);
        int cnt = 0, bestM = 1 << 30;
        uint32_t blo = 0, bhi = 0;
        std::wstring bc, br, bn;
        for (auto &m : ms)
        {
            if (!m.Hostile || m.Dead || (m.Kind != TUK_Vtol && m.Kind != TUK_VtolDualEngine))
                continue;
            ++cnt;
            float x = 0, y = 0;
            bool dd = false;
            std::wstring c, r;
            int mm = 0;
            if (UnitPosNow(m.ActorLo, m.ActorHi, x, y, dd, false) && !dd && BearingTo(x, y, c, r, &mm) && mm < bestM)
            {
                bestM = mm;
                bc = c;
                br = r;
                blo = m.ActorLo;
                bhi = m.ActorHi;
                bn = UnitWord(m);
            }
        }
        if (!cnt)
        {
            Say(s, Line(L"answer_air_none", s));
            return;
        }
        const std::wstring count = CountPhrase(cnt, L"VTOL", L"VTOLs");
        if (blo || bhi)
        {
            Say(s, Line(L"answer_air", s, {{L"count", count}, {L"clock", bc}, {L"range", br}}), K_Position);
            NoteIntel(s, blo, bhi, bn);
        }
        else
            Say(s, Line(L"answer_air_noloc", s, {{L"count", count}}));
        return;
    }
    case Q_WhoFiring:
    {
        // Who has you as its target (the units feed, lancecontrol-11), the weapon said matched against
        // the loadouts LidarExport reads (within its 1000 m scan; unknown beyond), one a lancemate can
        // see first, then the nearest.
        static const struct
        {
            const wchar_t *K, *Say;
            const wchar_t *Match[8];
        } kWpn[] = {{L"lrm", L"LRM fire", {L"lrm"}},
                    {L"srm", L"SRM fire", {L"srm", L"streak"}},
                    {L"arrow", L"Arrow IV fire", {L"arrow"}},
                    {L"artillery", L"artillery", {L"arrow", L"artillery", L"long tom", L"sniper", L"thumper"}},
                    {L"missile", L"missile fire", {L"lrm", L"srm", L"mrm", L"atm", L"arrow", L"streak", L"rocket", L"missile"}},
                    {L"ppc", L"PPC fire", {L"ppc"}},
                    {L"laser", L"laser fire", {L"laser"}},
                    {L"gauss", L"Gauss fire", {L"gauss"}},
                    {L"ac", L"autocannon fire", {L"ac", L"autocannon"}},
                    {L"mg", L"machine gun fire", {L"machine gun", L"mg"}}};
        const auto *wp = std::find_if(std::begin(kWpn), std::end(kWpn), [&](const decltype(kWpn[0]) &k) { return qname == k.K; });
        const bool haveW = wp != std::end(kWpn);
        auto wpnScore = [&](uint32_t lo, uint32_t hi) -> int { // 2 carries it, 1 unknown, 0 does not
            if (!haveW)
                return 1;
            TerrainMechInfo m = {};
            if (!MechInfoOf(lo, hi, m, false) || m.WeaponCount == 0)
                return 1;
            for (int i = 0; i < m.WeaponCount && i < TERR_MAX_WEAPONS; ++i)
            {
                std::wstring nm;
                for (size_t c = 0; c < sizeof(m.Weapons[i].Name) && m.Weapons[i].Name[c]; ++c)
                    nm += (wchar_t)towlower((unsigned char)m.Weapons[i].Name[c]);
                for (auto *x : wp->Match)
                    if (x && nm.find(x) != std::wstring::npos)
                        return 2;
            }
            return 0;
        };
        struct Cand
        {
            uint32_t Lo, Hi;
            int      Score, Metres;
            uint8_t  SeenBy;
            bool     Radar; // on a lancemate's sensors
            bool     Mine;  // on YOUR sensors: only then can it be patched through (your tracker must know it)
            std::wstring Clock, Range;
            int      Rounds = 0; // TRACED: rounds of the asked kind it fired at you in its current spell
        };
        std::vector<Cand> cs;
        // 1. TRACED (lancecontrol-14): the units whose rounds actually came at you - most rounds of the kind
        //    asked first. Lasers are beams, not rounds: never traced - the guess below answers those.
        bool traced = false;
        {
            int want[LST_Count] = {1, 1, 1, 1}; // no weapon named: every kind
            if (haveW)
            {
                const std::wstring k = wp->K;
                want[LST_Missile] = k == L"lrm" || k == L"srm" || k == L"missile";
                want[LST_Arrow] = k == L"arrow" || k == L"artillery" || k == L"missile";
                want[LST_Ballistic] = k == L"ac" || k == L"gauss" || k == L"mg";
                want[LST_PPC] = k == L"ppc";
            }
            std::vector<LsShooter> sh;
            LsReadShooters(sh);
            const uint32_t now32 = (uint32_t)now;
            for (auto &x : sh)
            {
                if (now32 - x.LastMs > 8000)
                    continue;
                int n = 0;
                for (int t = 0; t < LST_Count; ++t)
                    n += want[t] ? x.Rounds[t] : 0;
                if (!n)
                    continue;
                auto it = g_seen.find(UnitKey(x.ActorLo, x.ActorHi));
                if (it == g_seen.end() || it->second.Dead)
                    continue;
                Cand c;
                c.Lo = x.ActorLo;
                c.Hi = x.ActorHi;
                c.Score = 3;
                c.SeenBy = it->second.SeenBy;
                c.Radar = (it->second.Flags & LSU_OnLanceRadar) != 0;
                c.Mine = (it->second.Flags & LSU_OnPlayerRadar) != 0;
                c.Metres = 0;
                c.Rounds = n;
                if (!BearingTo(it->second.X, it->second.Y, c.Clock, c.Range, &c.Metres))
                    continue;
                cs.push_back(c);
            }
            std::sort(cs.begin(), cs.end(), [](const Cand &a, const Cand &b) { return a.Rounds > b.Rounds; });
            traced = !cs.empty();
        }
        // 2. Else the guess: units that had you as their target (a weapon named: carrying it).
        for (auto &kv : g_seen)
        {
            if (traced)
                break;
            const SeenAt &a = kv.second;
            if (a.Dead || now - a.Ms > 3000 || !a.TgtMs || now - a.TgtMs > 5000)
                continue;
            Cand c;
            c.Lo = (uint32_t)kv.first;
            c.Hi = (uint32_t)(kv.first >> 32);
            c.Score = wpnScore(c.Lo, c.Hi);
            c.SeenBy = a.SeenBy;
            c.Radar = (a.Flags & LSU_OnLanceRadar) != 0;
            c.Mine = (a.Flags & LSU_OnPlayerRadar) != 0;
            c.Metres = 0;
            if (!BearingTo(a.X, a.Y, c.Clock, c.Range, &c.Metres))
                continue;
            cs.push_back(c);
        }
        // A weapon was named: never a unit known NOT to carry it (2026-10-06e - a Gladiator was named for Arrow IV);
        // units whose loadout is unknown (past the scan) only when no known carrier is on you.
        if (haveW && !traced)
        {
            const bool known = std::any_of(cs.begin(), cs.end(), [](const Cand &c) { return c.Score == 2; });
            cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const Cand &c) { return c.Score == 0 || (known && c.Score == 1); }),
                     cs.end());
        }
        if (!traced)
        std::sort(cs.begin(), cs.end(), [](const Cand &a, const Cand &b) {
            if (a.Score != b.Score)
                return a.Score > b.Score;
            if ((a.SeenBy != 0) != (b.SeenBy != 0))
                return a.SeenBy != 0;
            if (a.Radar != b.Radar)
                return a.Radar;
            return a.Metres < b.Metres;
        });
        const std::wstring wsay = haveW ? wp->Say : L"";
        if (cs.empty())
        {
            const int s = by ? by : anyone();
            Say(s, Line(haveW ? L"answer_firing_none_wpn" : L"answer_firing_none", s, {{L"weapon", wsay}}));
            return;
        }
        const Cand &c = cs[0];
        const bool guess = haveW && c.Score == 1; // nobody known to carry it: a best guess, said as one
        auto offer = [&](int sp, std::wstring &txt, const std::wstring &name) {
            NoteIntel(sp, c.Lo, c.Hi, name);
            if (!c.Mine)
                return; // not on your scope: your tracker would not take it
            AddPiece(txt, Line(L"answer_firing_offer", sp));
            g_offer.Slot = sp;
            g_offer.Lo = c.Lo;
            g_offer.Hi = c.Hi;
            g_offer.Name = name;
            g_offerMs = now;
        };
        int s = 0;
        if (by && (c.SeenBy & (1u << (by - 1))))
            s = by;
        for (int x = 1; x <= 8 && !s; ++x)
            if ((c.SeenBy & (1u << (x - 1))) && isAlive(x))
                s = x;
        std::wstring t;
        if (s) // a lancemate sees it: what it is, where, and the offer
        {
            std::wstring name = EnemyNameOf(c.Lo, c.Hi);
            if (name.empty())
                name = L"contact";
            t = Line(guess ? L"answer_firing_guess" : haveW ? L"answer_firing_seen_wpn" : L"answer_firing_seen", s,
                     {{L"enemy", name}, {L"clock", c.Clock}, {L"range", c.Range}, {L"weapon", wsay}});
            offer(s, t, name);
        }
        else if (c.Radar) // nobody sees it, but it is on the lance's radar: that, and the offer (lancecontrol-12)
        {
            s = by ? by : anyone();
            std::wstring name = EnemyNameOf(c.Lo, c.Hi);
            if (name.empty())
                name = L"contact";
            t = Line(guess ? L"answer_firing_guess" : haveW ? L"answer_firing_radar_wpn" : L"answer_firing_radar", s,
                     {{L"enemy", name}, {L"clock", c.Clock}, {L"range", c.Range}, {L"weapon", wsay}});
            offer(s, t, name);
        }
        else // nobody sees it and it is not on radar: only where the fire comes from
        {
            s = by ? by : anyone();
            t = Line(haveW ? L"answer_firing_unseen_wpn" : L"answer_firing_unseen", s, {{L"clock", c.Clock}, {L"weapon", wsay}});
        }
        // A second shooter of the same kind, near as busy (traced only): its bearing too. The offer stays on the first.
        if (traced && cs.size() > 1 && cs[1].Rounds * 2 >= cs[0].Rounds)
        {
            std::wstring n2 = EnemyNameOf(cs[1].Lo, cs[1].Hi);
            AddPiece(t, Line(n2.empty() ? L"answer_firing_second_anon" : L"answer_firing_second", s,
                             {{L"enemy", n2}, {L"clock", cs[1].Clock}, {L"range", cs[1].Range}}));
        }
        Say(s, t, K_Position);
        return;
    }
    case Q_HillPick: // HIGH GROUND (2026-10-14)
        HillAnswer(_wtoi(qname.c_str()));
        return;
    case Q_HillCancel:
        HillAnswer(-2);
        return;
    case Q_PatchYes:
    case Q_PatchNo:
    {
        const int s = isAlive(g_offer.Slot) ? g_offer.Slot : (by ? by : anyone());
        const bool live = g_offerMs != 0;
        g_offerMs = 0;
        if (q == Q_PatchNo || !live)
        {
            Say(s, Line(L"answer_patch_cancel", s));
            return;
        }
        StartPatch(s, g_offer.Lo, g_offer.Hi, g_offer.Name); // said done only once the game shows the lock
        return;
    }
    case Q_PatchCmd:
    {
        // "patch it through" (the offer's unit, else the last reported) / "patch in the Atlas" / "... the closest contact"
        uint32_t lo = 0, hi = 0;
        int s = by;
        if (qname == L"closest")
        {
            int best = 1 << 30;
            for (auto &kv : g_seen)
            {
                const SeenAt &a = kv.second;
                if (a.Dead || now - a.Ms > 3000 || !(a.Flags & LSU_OnPlayerRadar))
                    continue;
                std::wstring c, r;
                int m = 0;
                if (BearingTo(a.X, a.Y, c, r, &m) && m < best)
                {
                    best = m;
                    lo = (uint32_t)kv.first;
                    hi = (uint32_t)(kv.first >> 32);
                }
            }
        }
        else if (qname.empty() && g_offerMs && now - g_offerMs < 60000)
        {
            lo = g_offer.Lo;
            hi = g_offer.Hi;
            if (!s && isAlive(g_offer.Slot))
                s = g_offer.Slot;
        }
        else
        {
            QUnit u;
            if (QuestionUnit(by, qname, bySlot, u))
            {
                lo = u.Lo;
                hi = u.Hi;
                if (!s && u.Note && isAlive(u.Note->Slot))
                    s = u.Note->Slot;
            }
        }
        if (!s)
            s = anyone();
        g_offerMs = 0;
        if (!lo && !hi)
        {
            Say(s, Line(L"answer_patch_nothing", s));
            return;
        }
        std::wstring name = EnemyNameOf(lo, hi);
        if (name.empty())
            name = L"contact";
        StartPatch(s, lo, hi, name);
        return;
    }
    case Q_Repeat:
    {
        int s = by ? by : (g_lastSpeaker && now - g_lastSpeakMs < 120000 ? g_lastSpeaker : 0);
        if (!s || !isAlive(s) || g_lastLine[s].empty())
        {
            const int x = s && isAlive(s) ? s : anyone();
            Say(x, Line(L"answer_repeat_none", x));
            return;
        }
        const std::wstring keep = g_lastLine[s];
        const std::wstring lead = Line(L"resume_say_again", s);
        Say(s, lead + L" " + StripCallsign(s, keep), 0, 0, false, (int)SplitSentences(lead).size());
        g_lastLine[s] = keep; // "say again" twice repeats the report, not "I say again"
        return;
    }
    default:
        return;
    }
}

// Main thread, from RunSpokenActions.
void AnswerQuestion(int q, int who, const std::wstring &qname)
{
    const ULONGLONG now = GetTickCount64();
    LsSlot slots[kLsMaxSlots];
    LsCondition cond[kLsMaxSlots];
    const int n = LsConnect() ? LsReadSlots(slots, cond, nullptr) : 0;
    std::vector<int> alive;
    const LsSlot *bySlot[9] = {};
    const LsCondition *condOf[9] = {};
    for (int i = 0; i < n; ++i)
        if (slots[i].Valid && !slots[i].IsPlayer && slots[i].Alive && slots[i].Slot >= 1 && slots[i].Slot <= 8)
        {
            alive.push_back(slots[i].Slot);
            bySlot[slots[i].Slot] = &slots[i];
            condOf[slots[i].Slot] = &cond[i];
        }
    if (alive.empty())
    {
        Say(0, Line(L"problem_no_lancemates", 0));
        return;
    }
    auto isAlive = [&](int s) { return s >= 1 && s <= 8 && bySlot[s]; };
    // 2026-10-12 (user): a callsign that is not a lancemate in the field ("Six, report in" with three) - the same
    // answer as an order to it, not the whole lance answering for it.
    if (who > 0 && !isAlive(who))
    {
        bool isMe = false;
        for (int i = 0; i < n; ++i)
            isMe = isMe || (slots[i].Valid && slots[i].IsPlayer && slots[i].Slot == who);
        Say(0, isMe ? Line(L"problem_is_you", 0) : Line(L"problem_no_answer", 0, {{L"callsign", NumberName(who)}}));
        return;
    }
    // Who answers: the one named; else whoever reported last (within 90 s); else...
    const IntelNote *last = (g_intelLast.Slot && now - g_intelLast.Ms < 90000) ? &g_intelLast : nullptr;
    int by = who > 0 && isAlive(who) ? who : 0;
    // No callsign: a question to "you" goes to the pilot you were just talking to (build 2026-10-06a).
    if (who < 0 && !by)
    {
        const int la = g_lastAnswerer;
        const ULONGLONG lam = g_lastAnswerMs;
        const bool recent = la > 0 && isAlive(la) && now - lam < 60000;
        const bool newer = !last || lam > g_intelLast.Ms; // nobody reported anything since
        if (recent && (q == Q_WhereYou || q == Q_Target || q == Q_Ammo || q == Q_Heat || q == Q_WhoHits ||
                       ((q == Q_HowBad || q == Q_CanSee || q == Q_IsDown || q == Q_UnitHow) && newer)))
            by = la;
    }
    if (q >= Q_HowMany)
    {
        AnswerMore(q, by, qname, alive, bySlot, condOf, slots, n);
        return;
    }
    if (q == Q_WhereYou || q == Q_HowBad)
    {
        // "where are you" / "how bad" to nobody in particular (or the lance): everyone answers.
        std::vector<int> them;
        if (by)
            them.push_back(by);
        else if (q == Q_HowBad && last && isAlive(last->Slot))
            them.push_back(last->Slot);
        else
            them = alive;
        for (int s : them)
        {
            if (q == Q_HowBad)
            {
                const LsCondition *c = condOf[s];
                std::wstring dmg = (c && c->Have) ? StatusDamage(c->Armour, c->Structure, s) : Line(L"status_no_damage_read", s);
                if (bySlot[s]->Heat >= 0.7f)
                    AddPiece(dmg, Line(L"status_running_hot", s));
                Say(s, Line(L"answer_how_bad", s, {{L"damage", dmg}}));
                continue;
            }
            std::wstring clock, range;
            int metres = 0;
            if (BearingTo(bySlot[s]->Loc[0], bySlot[s]->Loc[1], clock, range, &metres))
                Say(s, metres < 60 ? Line(L"answer_where_me_close", s) : Line(L"answer_where_me", s, {{L"clock", clock}, {L"range", range}}), K_Position);
            else
                Say(s, Line(L"problem_no_position", s));
        }
        return;
    }
    if (q == Q_WhereNamed && qname.compare(0, 4, L"nav ") == 0) // NAV POINTS (2026-10-07k): "nav <letter number>"
    {
        const int nl = _wtoi(qname.c_str() + 4);
        const int s = by ? by : alive[(size_t)rand() % alive.size()];
        float p[3];
        std::wstring clock, range;
        if (NavLoc(nl, p) && BearingTo(p[0], p[1], clock, range))
            Say(s, Line(L"answer_nav_where", s, {{L"nav", NavSaid(nl)}, {L"clock", clock}, {L"range", range}}), K_Position);
        else
            Say(s, Line(L"answer_nav_none", s, {{L"nav", NavSaid(nl)}}));
        return;
    }
    if (q == Q_WhereNamed)
    {
        if (LowerW(qname) == L"my target") // "where's my target"
        {
            QUnit u;
            const int s = by ? by : alive[(size_t)rand() % alive.size()];
            if (QuestionUnit(s, qname, bySlot, u))
                Say(s, WhereLine(s, u.Lo, u.Hi, u.Name, nullptr), K_Position);
            else
                Say(s, Line(L"answer_mytarget_none", s));
            return;
        }
        // The last one of that name reported; else the nearest one LidarExport can see.
        const std::wstring want = LowerW(qname);
        auto same = [&](const std::wstring &a) {
            std::wstring x = LowerW(a), y = want;
            auto squash = [](std::wstring z) {
                z.erase(std::remove(z.begin(), z.end(), L' '), z.end());
                if (z.size() > 3 && z.back() == L's')
                    z.pop_back();
                return z;
            };
            return squash(x) == squash(y);
        };
        const IntelNote *note = nullptr;
        for (auto &kv : g_intelByName)
            if (same(kv.first) && now - kv.second.Ms < 300000 && (!note || kv.second.Ms > note->Ms))
                note = &kv.second;
        std::wstring nameSaid = qname;
        if (!nameSaid.empty())
            nameSaid[0] = (wchar_t)towupper(nameSaid[0]);
        if (note)
        {
            const int s = by ? by : (isAlive(note->Slot) ? note->Slot : alive[(size_t)rand() % alive.size()]);
            Say(s, WhereLine(s, note->Lo, note->Hi, note->Name, note), K_Position);
            return;
        }
        std::vector<TerrainMechInfo> ms;
        MechInfosAll(ms);
        const TerrainMechInfo *best = nullptr;
        float bestD = 1e30f;
        for (auto &m : ms)
        {
            if (!m.Hostile || m.Dead || !same(UnitWord(m)))
                continue;
            float x = 0, y = 0;
            bool dd = false;
            if (!UnitPosNow(m.ActorLo, m.ActorHi, x, y, dd) || dd || !g_lid)
                continue;
            const float d = (x - g_lid->PlayerX) * (x - g_lid->PlayerX) + (y - g_lid->PlayerY) * (y - g_lid->PlayerY);
            if (d < bestD)
            {
                bestD = d;
                best = &m;
            }
        }
        const int s = by ? by : alive[(size_t)rand() % alive.size()];
        if (best)
            Say(s, WhereLine(s, best->ActorLo, best->ActorHi, UnitWord(*best), nullptr), K_Position);
        else
            Say(s, Line(L"answer_where_named_none", s, {{L"enemy", nameSaid}}));
        return;
    }
    // "where?" / "what is it?" - about what that pilot (or whoever reported last) named.
    const IntelNote *note = by ? ((g_intelBySlot[by].Slot && now - g_intelBySlot[by].Ms < 90000) ? &g_intelBySlot[by] : nullptr) : last;
    const int s = by ? by : (note && isAlive(note->Slot) ? note->Slot : 0);
    if (!note || !s)
    {
        Say(s, Line(L"answer_where_none", s));
        return;
    }
    uint32_t lo = note->Lo, hi = note->Hi;
    std::wstring name = note->Name;
    if (!lo && !hi && g_sit[s].AtkMs && now - g_sit[s].AtkMs < 20000) // 2026-10-07j: 20 s (was 10)
    {
        // it did not say who - but the game knows who hit it last
        name = EnemyNameOf(g_sit[s].AtkLo, g_sit[s].AtkHi);
        if (!name.empty())
        {
            lo = g_sit[s].AtkLo;
            hi = g_sit[s].AtkHi;
        }
    }
    if (!lo && !hi)
    {
        Say(s, Line(L"answer_where_swarm", s));
        return;
    }
    if (q == Q_What)
    {
        TerrainMechInfo m = {};
        if (!MechInfoOf(lo, hi, m, false))
        {
            Say(s, Line(L"answer_where_unknown", s, {{L"enemy", name}}));
            return;
        }
        static const wchar_t *kClass[] = {L"light", L"medium", L"heavy", L"assault"};
        if (m.Kind == TUK_Mech && m.WeightClass <= 3)
            Say(s, Line(L"answer_what", s, {{L"enemy", UnitWord(m)}, {L"class", kClass[m.WeightClass]}}));
        else
            Say(s, Line(L"answer_what_vehicle", s, {{L"enemy", UnitWord(m)}}));
        return;
    }
    Say(s, WhereLine(s, lo, hi, name, note), K_Position);
}

const wchar_t *PartWord(int p)
{
    static const wchar_t *k[TMP_COUNT] = {L"head", L"centre torso", L"left torso", L"right torso",
                                          L"left arm", L"right arm", L"left leg", L"right leg"};
    return (p >= 0 && p < TMP_COUNT) ? k[p] : L"armour";
}

uint32_t  g_sitPlayerLo = 0, g_sitPlayerHi = 0;
std::set<uint64_t> g_sitAnnounced; // heavies / VTOLs already called out (this mission)
std::map<uint64_t, std::array<float, TMP_COUNT>> g_sitTgtSt; // locked targets' structure, last look
bool      g_sitEngaged = false, g_sitThreat = false;
int       g_sitPrevHostiles = 0;
ULONGLONG g_sitClearMs = 0, g_sitMs = 0;

void SituationTick()
{
    if (!g_cfg.Callouts || !LsConnect())
        return;
    if (!LsFresh()) // 2026-10-16: frozen data = game paused / out of the mech (ejected, dead) - no callouts on it
        return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_sitMs < 500)
        return;
    g_sitMs = now;
    LsSlot slots[kLsMaxSlots];
    LsCondition cond[kLsMaxSlots];
    const int n = LsReadSlots(slots, cond, nullptr);
    // A new mission (your mech changed): forget it all.
    for (int i = 0; i < n; ++i)
        if (slots[i].Valid && slots[i].IsPlayer && (slots[i].ActorLo != g_sitPlayerLo || slots[i].ActorHi != g_sitPlayerHi))
        {
            g_sitPlayerLo = slots[i].ActorLo;
            g_sitPlayerHi = slots[i].ActorHi;
            for (auto &x : g_sit)
                x = SitSlot();
            g_sitAnnounced.clear();
            g_sitTgtSt.clear();
            g_sitEngaged = g_sitThreat = false;
            g_sitPrevHostiles = 0;
            g_sitClearMs = 0;
        }
    std::vector<int> alive; // lancemates who can speak
    int lanceAlive = 0;     // you included
    for (int i = 0; i < n; ++i)
        if (slots[i].Valid && slots[i].Alive)
        {
            ++lanceAlive;
            if (!slots[i].IsPlayer && slots[i].Slot >= 1 && slots[i].Slot <= 8)
                alive.push_back(slots[i].Slot);
        }
    auto speaker = [&](int notThis) -> int {
        std::vector<int> c;
        for (int v : alive)
            if (v != notThis)
                c.push_back(v);
        return c.empty() ? 0 : c[(size_t)rand() % c.size()];
    };
    auto say = [&](int by, uint16_t kind, const std::wstring &text) -> bool { return by && Queue(by, kind, text); };

    // 1. Each lancemate's damage, by urgency.
    for (int i = 0; i < n; ++i)
    {
        const LsSlot &u = slots[i];
        const int sl = u.Slot;
        if (!u.Valid || u.IsPlayer || !u.Alive || sl < 1 || sl > 8)
            continue;
        SitSlot &S = g_sit[sl];
        if (S.Lo != u.ActorLo || S.Hi != u.ActorHi)
        {
            const int kills = S.Lo == 0 && S.Hi == 0 ? S.Kills : 0; // kills counted before its first look stay
            S = SitSlot();
            S.Lo = u.ActorLo;
            S.Hi = u.ActorHi;
            S.Kills = kills;
        }
        const LsCondition &c = cond[i];
        if (!c.Have)
            continue;
        double sum = 0.0;
        int parts = 0;
        for (int p = 0; p < TMP_COUNT; ++p)
            if (c.Armour[p] >= 0.0f || c.Structure[p] >= 0.0f)
            {
                sum += std::max(0.0f, std::min(1.0f, c.Armour[p])) + std::max(0.0f, std::min(1.0f, c.Structure[p]));
                ++parts;
            }
        if (!parts)
            continue;
        const float h = (float)(sum / (2.0 * parts));
        S.Hist.push_back(std::make_pair(now, h));
        while (!S.Hist.empty() && now - S.Hist.front().first > 6000)
            S.Hist.pop_front();
        const float dmg = S.Hist.front().second - h; // lost in the last ~5-6 s
        int partLost = -1;
        for (int p : {TMP_LeftArm, TMP_RightArm, TMP_LeftTorso, TMP_RightTorso, TMP_LeftLeg, TMP_RightLeg})
            if (S.HaveSt && S.St[p] > 0.01f && c.Structure[p] >= 0.0f && c.Structure[p] <= 0.01f && partLost < 0)
                partLost = p;
        auto st = [&](int p) { return c.Structure[p] < 0.0f ? 1.0f : c.Structure[p]; };
        const float legs = std::min(st(TMP_LeftLeg), st(TMP_RightLeg));
        const bool crit = st(TMP_CentreTorso) < 0.4f || st(TMP_Head) < 0.4f || (legs > 0.01f && legs < 0.3f);
        const bool critRise = crit && !S.Critical && S.HaveSt;
        for (int p = 0; p < TMP_COUNT; ++p)
            S.St[p] = c.Structure[p];
        S.HaveSt = true;
        S.Critical = crit;
        // the attacker, if it hit it in the last 8 s and LidarExport can name it
        TerrainMechInfo am = {};
        const std::wstring en = (S.AtkMs && now - S.AtkMs < 8000) ? EnemyNameOf(S.AtkLo, S.AtkHi, &am) : L"";
        int lvl = 0;
        bool said = false;
        if (partLost >= 0)
        {
            lvl = 3;
            said = Queue(sl, K_PartLost, en.empty() ? Line(L"callout_part_lost_anon", sl, {{L"part", PartWord(partLost)}})
                                             : Line(L"callout_part_lost", sl, {{L"part", PartWord(partLost)}, {L"enemy", en}}));
        }
        else if (critRise)
        {
            lvl = 3;
            said = Queue(sl, K_Critical, en.empty() ? Line(L"callout_critical_anon", sl) : Line(L"callout_critical", sl, {{L"enemy", en}}));
            const int by = speaker(sl);
            say(by, K_AllyTrouble, Line(L"callout_ally_trouble", by, {{L"ally", Callsign(sl)}}));
        }
        else if (dmg >= 0.10f)
        {
            lvl = 2;
            said = Queue(sl, K_Shredded, en.empty() ? Line(L"callout_shredded_anon", sl) : Line(L"callout_shredded", sl, {{L"enemy", en}}));
        }
        else if (dmg >= 0.04f)
        {
            lvl = 1;
            if (!en.empty() && am.Kind == TUK_Turret)
                said = Queue(sl, K_Turrets, Line(L"callout_turrets", sl, {{L"enemy", en}}));
            else
                said = Queue(sl, K_HitHard, en.empty() ? Line(L"callout_hit_hard_anon", sl) : Line(L"callout_hit_hard", sl, {{L"enemy", en}}));
        }
        if (said)
            NoteIntel(sl, en.empty() ? 0 : S.AtkLo, en.empty() ? 0 : S.AtkHi, en); // "where?" -> that attacker
        if (lvl)
        {
            // the worse report replaces the plain "taking fire" (waiting, or for its cool-down)
            g_lastCall[std::make_pair(sl, (int)LSE_TookDamage)] = now;
            g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                           [&](const Pending &p) { return p.Slot == sl && p.Type == LSE_TookDamage; }),
                            g_pending.end());
        }
    }

    // 2. The units LidarExport describes: contacts, your target, how many are left.
    std::vector<TerrainMechInfo> ms;
    if (MechInfosAll(ms) > 0)
    {
        // the names the parser ("attack that Bane") and Whisper's prompt should know
        {
            std::vector<std::wstring> nn;
            for (auto &m : ms)
                if (m.Hostile && !m.Dead)
                {
                    const std::wstring u = UnitWord(m);
                    bool have = false;
                    for (auto &x : nn)
                        have |= SameUnitName(x, u);
                    if (!have && nn.size() < 24)
                        nn.push_back(u);
                }
            for (auto &kv : g_intelByName)
            {
                bool have = false;
                for (auto &x : nn)
                    have |= SameUnitName(x, kv.second.Name);
                if (!have && !kv.second.Name.empty() && nn.size() < 32)
                    nn.push_back(kv.second.Name);
            }
            std::lock_guard<std::mutex> lk(g_namesMx);
            g_knownNames.swap(nn);
        }
        int hostiles = 0, hostileMechs = 0;
        const TerrainMechInfo *lastOne = nullptr, *newAir = nullptr;
        std::vector<const TerrainMechInfo *> newHeavy;
        for (auto &m : ms)
        {
            if (!m.Hostile || m.Dead)
                continue;
            const uint64_t key = ((uint64_t)m.ActorHi << 32) | m.ActorLo;
            const bool combat = m.Kind == TUK_Mech || m.Kind == TUK_Tank || m.Kind == TUK_Vtol || m.Kind == TUK_VtolDualEngine ||
                                m.Kind == TUK_Artillery;
            if (combat)
            {
                ++hostiles;
                lastOne = &m;
            }
            if (m.Kind == TUK_Mech)
                ++hostileMechs;
            if (m.Kind == TUK_Mech && m.WeightClass >= 2 && m.WeightClass != 0xFF && !g_sitAnnounced.count(key))
            {
                g_sitAnnounced.insert(key);
                newHeavy.push_back(&m);
            }
            if ((m.Kind == TUK_Vtol || m.Kind == TUK_VtolDualEngine) && !g_sitAnnounced.count(key))
            {
                g_sitAnnounced.insert(key);
                newAir = &m;
            }
            // your locked target: a part off, the centre torso nearly gone
            if (m.Locked && m.Kind == TUK_Mech && m.HaveCondition)
            {
                auto it = g_sitTgtSt.find(key);
                if (it != g_sitTgtSt.end())
                {
                    const std::wstring en = UnitWord(m);
                    for (int p : {TMP_LeftArm, TMP_RightArm, TMP_LeftTorso, TMP_RightTorso, TMP_LeftLeg, TMP_RightLeg})
                        if (it->second[p] > 0.01f && m.Structure[p] >= 0.0f && m.Structure[p] <= 0.01f)
                        {
                            const int by = speaker(0);
                            if (say(by, K_TargetPart, Line(L"callout_target_part", by, {{L"enemy", en}, {L"part", PartWord(p)}})))
                                NoteIntel(by, m.ActorLo, m.ActorHi, en);
                            break;
                        }
                    if (it->second[TMP_CentreTorso] >= 0.35f && m.Structure[TMP_CentreTorso] >= 0.0f && m.Structure[TMP_CentreTorso] < 0.35f)
                    {
                        const int by = speaker(0);
                        if (say(by, K_TargetPart, Line(L"callout_target_ct", by, {{L"enemy", en}})))
                            NoteIntel(by, m.ActorLo, m.ActorHi, en);
                    }
                }
                std::array<float, TMP_COUNT> a;
                for (int p = 0; p < TMP_COUNT; ++p)
                    a[p] = m.Structure[p];
                g_sitTgtSt[key] = a;
                if (g_sitTgtSt.size() > 64)
                    g_sitTgtSt.clear();
            }
        }
        if (!newHeavy.empty())
        {
            // VOLUME (build 2026-10-05j): the more heavies called out in the last 3 minutes, the
            // lower the priority (so the chatter gap holds them back), the terser the line ("another
            // heavy, 3 o'clock"), and past 5 only assaults are still called.
            static std::deque<ULONGLONG> called;
            while (!called.empty() && now - called.front() > 180000)
                called.pop_front();
            const int recent = (int)called.size();
            const int prio = std::max(2, 6 - recent);
            const int by = speaker(0);
            if (newHeavy.size() == 1)
            {
                const TerrainMechInfo &h = *newHeavy[0];
                if (recent < 6 || h.WeightClass == 3)
                {
                    std::wstring clock, range;
                    const bool at = ContactBearing(h.Id, clock, range);
                    const wchar_t *g = !at ? L"callout_contact_heavy_noloc" : (recent >= 2 ? L"callout_contact_more" : L"callout_contact_heavy");
                    if (by && Queue(by, K_ContactHeavy, Line(g, by, {{L"enemy", UnitWord(h)}, {L"class", h.WeightClass == 3 ? L"assault" : L"heavy"},
                                                                     {L"clock", clock}, {L"range", range}}), prio))
                        NoteIntel(by, h.ActorLo, h.ActorHi, UnitWord(h));
                    called.push_back(now);
                }
            }
            else
            {
                if (by)
                    Queue(by, K_ContactHeavy, Line(L"callout_contact_heavies", by, {{L"count", std::to_wstring(newHeavy.size())}}), prio);
                called.push_back(now);
            }
        }
        if (newAir)
        {
            const int by = speaker(0);
            if (say(by, K_Air, Line(L"callout_air_contact", by, {{L"enemy", UnitWord(*newAir)}})))
                NoteIntel(by, newAir->ActorLo, newAir->ActorHi, UnitWord(*newAir));
        }
        // a fight: two or more at once; then one left, then none for 10 s
        if (hostiles >= 2)
        {
            g_sitEngaged = true;
            g_sitClearMs = 0;
        }
        if (g_sitEngaged && hostiles == 1 && g_sitPrevHostiles >= 2 && lastOne)
        {
            const int by = speaker(0);
            if (say(by, K_LastEnemy, Line(L"callout_last_enemy", by, {{L"enemy", UnitWord(*lastOne)}})))
                NoteIntel(by, lastOne->ActorLo, lastOne->ActorHi, UnitWord(*lastOne));
        }
        if (g_sitEngaged && hostiles == 0)
        {
            if (!g_sitClearMs)
                g_sitClearMs = now;
            else if (now - g_sitClearMs > 10000)
            {
                const int by = speaker(0);
                say(by, K_AreaClear, Line(L"callout_area_clear", by));
                g_sitEngaged = false;
                g_sitClearMs = 0;
            }
        }
        else if (hostiles > 0)
            g_sitClearMs = 0;
        g_sitPrevHostiles = hostiles;
        if (lanceAlive > 0 && hostileMechs >= lanceAlive + 2)
        {
            const int by = speaker(0);
            if (by && CalloutReady(by, K_Outnumbered)) // 2026-10-16: build the line only when its cooldown allows
                say(by, K_Outnumbered, Line(L"callout_outnumbered", by, {{L"count", std::to_wstring(hostileMechs)}}));
        }
    }

    // 3. A missile lock on you (LidarExport's lock check; it runs while HUDDisplay asks for it).
    LockedCount();
    if (g_lid && g_lid->Magic == LIDAR_MAGIC)
    {
        const bool t = (g_lid->ThreatFlags & LTHREAT_LOCKED) != 0;
        if (t && !g_sitThreat)
        {
            const int by = speaker(0);
            // Chatter 3 (busy): who launched, from the traced rounds (lancecontrol-14) - the most recent missile /
            // Arrow IV shooter of the last 6 s (2026-10-06g).
            std::wstring named;
            if (g_sayMissileShooter)
            {
                std::vector<LsShooter> sh;
                LsReadShooters(sh);
                const uint32_t now32 = (uint32_t)GetTickCount64();
                for (auto &x : sh) // most recent first
                {
                    if (now32 - x.LastMs > 6000 || !(x.Rounds[LST_Missile] + x.Rounds[LST_Arrow]))
                        continue;
                    auto it = g_seen.find(((uint64_t)x.ActorHi << 32) | x.ActorLo);
                    std::wstring clock, range;
                    if (it == g_seen.end() || !BearingTo(it->second.X, it->second.Y, clock, range))
                        continue;
                    std::wstring nm = EnemyNameOf(x.ActorLo, x.ActorHi);
                    named = Line(nm.empty() ? L"callout_missiles_from_anon" : L"callout_missiles_from", by,
                                 {{L"enemy", nm}, {L"clock", clock}, {L"range", range}});
                    break;
                }
            }
            say(by, K_MissilesOnYou, named.empty() ? Line(L"callout_missiles_on_you", by) : named);
        }
        g_sitThreat = t;
    }
}

// ---- PILOT NAMES: lancemates called by their pilots' callsigns ("Sledge") ------------------------
// The name is the one the game gives the pilot (MWPersonaComponent's Callsign), which LidarExport
// already reads for every friendly unit in its 1000 m scan (TerrainMechInfo::Callsign) - joined on
// the actor address the lance status feed carries. Learned every 2 s and KEPT for the unit once
// seen, so a lancemate that later walks out of the scan keeps its name (the lance starts together,
// so names are normally known within seconds of the drop). Until then: the lance number.
// Used for speech ({callsign}), for orders by name in Whisper mode ("Sledge, hold position"), for
// per-pilot lines ([Sledge:group]) and in Whisper's vocabulary prompt.
struct PilotEntry
{
    uint32_t     Lo = 0, Hi = 0; // the unit it was learned on
    std::wstring Name;
};
PilotEntry g_pilot[9];
std::mutex g_pilotMx; // the parser (Whisper's thread) reads the names too
ULONGLONG  g_pilotMs = 0;

std::wstring PilotNameOf(int slot)
{
    if (slot < 1 || slot > 8)
        return L"";
    std::lock_guard<std::mutex> lk(g_pilotMx);
    return g_pilot[slot].Name;
}

// From the main loop, every 2 s.
std::wstring PilotVoiceOf(const std::wstring &name, int slot); // PILOT VOICES, beside KokoroVoiceFor

void PilotNamesTick()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_pilotMs < 2000)
        return;
    g_pilotMs = now;
    if (!LsConnect())
        return;
    LsSlot all[kLsMaxSlots];
    const int n = LsReadSlots(all);
    for (int i = 0; i < n; ++i)
    {
        const LsSlot &u = all[i];
        if (!u.Valid || u.IsPlayer || u.Slot < 1 || u.Slot > 8)
            continue;
        // 2026-10-07c: the game's own pilot name first (status feed v6 - it follows the pilot through a
        // takeover); the mech's persona callsign from LidarExport only when the game gives none.
        std::wstring learned = LsPilotName(u.Slot);
        const bool fromGame = !learned.empty();
        // 2026-10-07e: a number whose old mech is still in the lance (someone else's now, or yours) only
        // changed mechs - a takeover, the same pilot: its name is kept. The persona callsign (the MECH's)
        // is only learned while the number has no name yet, so a takeover cannot hand it another pilot's.
        bool swapped = false, haveName = false;
        {
            std::lock_guard<std::mutex> lk(g_pilotMx);
            const PilotEntry &e = g_pilot[u.Slot];
            haveName = !e.Name.empty();
            if ((e.Lo || e.Hi) && !(e.Lo == u.ActorLo && e.Hi == u.ActorHi))
                for (int k = 0; k < n && !swapped; ++k)
                    swapped = all[k].Valid && all[k].ActorLo == e.Lo && all[k].ActorHi == e.Hi;
        }
        TerrainMechInfo m;
        if (!fromGame && !haveName && MechInfoOf(u.ActorLo, u.ActorHi, m, false) && m.Callsign[0])
        {
            char nm[sizeof(m.Callsign) + 1] = {};
            std::memcpy(nm, m.Callsign, sizeof(m.Callsign));
            for (const char *c = nm; *c; ++c)
                learned += (wchar_t)(unsigned char)*c;
            const size_t a = learned.find_first_not_of(L' '), b = learned.find_last_not_of(L' ');
            learned = a == std::wstring::npos ? L"" : learned.substr(a, b - a + 1);
        }
        std::lock_guard<std::mutex> lk(g_pilotMx);
        PilotEntry &e = g_pilot[u.Slot];
        const bool sameUnit = e.Lo == u.ActorLo && e.Hi == u.ActorHi;
        if (!sameUnit)
        {
            e.Lo = u.ActorLo;
            e.Hi = u.ActorHi;
            if (!fromGame && !swapped)
                e.Name.clear(); // another unit in this number, its old one gone (a new mission): forget the old name
        }
        if (!learned.empty() && learned != e.Name)
        {
            e.Name = learned;
            Log(L"lancemate %d is %ls (callsign %ls)", (int)u.Slot, learned.c_str(), CallName(learned).c_str());
        }
    }
    // Voices once the whole lance's names are in (2026-10-07e): a new pilot avoids every voice the others
    // already have (2026-10-07b: the pilot keeps the voice it first got).
    for (int s = 1; s <= 8; ++s)
    {
        const std::wstring nm = PilotNameOf(s);
        if (!nm.empty())
            PilotVoiceOf(nm, s);
    }
}

// For the parser: the names known now, as words (lower case), with their lance numbers.
std::vector<std::pair<std::vector<std::wstring>, int>> PilotNameWords()
{
    std::vector<std::pair<std::vector<std::wstring>, int>> out;
    std::lock_guard<std::mutex> lk(g_pilotMx);
    for (int s = 1; s <= 8; ++s)
        if (!g_pilot[s].Name.empty())
        {
            std::vector<std::wstring> w = SplitWords(g_pilot[s].Name);
            if (!w.empty())
                out.push_back(std::make_pair(w, s)); // the whole name first ("jacob hale")
            std::vector<std::wstring> l = SplitWords(CallName(g_pilot[s].Name));
            if (!l.empty() && l != w)
                out.push_back(std::make_pair(l, s)); // then the callsign alone ("hale")
        }
    return out;
}


// armour / structure: fractions per part (TerrainMechPart order), -1 = not read.
static_assert(kLsParts == TMP_COUNT, "lance status condition parts must match TerrainMechPart");
std::wstring StatusDamage(const float *armour, const float *structure, int slot)
{
    static const wchar_t *kPart[TMP_COUNT] = {L"head", L"centre torso", L"left torso", L"right torso",
                                              L"left arm", L"right arm", L"left leg", L"right leg"};
    std::wstring lost;
    int lostN = 0;
    double armSum = 0.0;
    int worst = -1;
    float worstSt = 2.0f;
    int armN = 0, armMinPart = -1;
    float armMin = 1.0f;
    for (int p = 0; p < TMP_COUNT; ++p)
    {
        if (structure[p] < 0.0f && armour[p] < 0.0f)
            continue; // not read
        const float a = std::max(0.0f, std::min(1.0f, armour[p])), st = structure[p] < 0.0f ? 1.0f : std::min(1.0f, structure[p]);
        if (armour[p] >= 0.0f)
        {
            armSum += a;
            ++armN;
            // a destroyed part has no armour, of course - "... armour's gone" is for parts still on
            if (st > 0.01f && (armMinPart < 0 || a < armMin))
            {
                armMin = a;
                armMinPart = p; // the part with the least armour (for "... armour's gone")
            }
        }
        if (st <= 0.01f)
        {
            if (p == TMP_LeftTorso || p == TMP_RightTorso || p == TMP_LeftArm || p == TMP_RightArm || p == TMP_LeftLeg || p == TMP_RightLeg)
            {
                lost += (lost.empty() ? L"" : L" and ") + std::wstring(kPart[p]);
                ++lostN;
            }
            continue;
        }
        if (st < worstSt)
        {
            worstSt = st;
            worst = p;
        }
    }
    const double armAvg = armN ? armSum / armN : 1.0;
    auto st = [&](int p) { return structure[p] < 0.0f ? 1.0f : structure[p]; }; // unread counts as whole
    const float ct = st(TMP_CentreTorso), head = st(TMP_Head);
    const float legs = std::min(st(TMP_LeftLeg), st(TMP_RightLeg));
    std::wstring s;
    if (ct < 0.4f || head < 0.4f || (legs > 0.01f && legs < 0.3f))
    {
        s = Line(L"status_damage_critical", slot);
        AddPiece(s, Line(ct < 0.4f ? L"status_critical_centre_torso" : (head < 0.4f ? L"status_critical_head" : L"status_critical_leg"), slot));
    }
    else if (worst >= 0 && worstSt < 0.6f)
        s = Line(L"status_damage_heavy", slot, {{L"part", kPart[worst]}});
    else if (lostN)
    {
        // "Lost my left arm" (below) says more than "a bit of plating stripped" would.
        if (armMinPart >= 0 && armMin <= 0.01f)
            AddPiece(s, Line(L"status_armour_gone", slot, {{L"part", kPart[armMinPart]}}));
    }
    else if (armAvg < 0.85 || armMin < 0.6f)
    {
        s = Line(L"status_damage_some", slot);
        if (armMinPart >= 0 && armMin <= 0.01f)
            AddPiece(s, Line(L"status_armour_gone", slot, {{L"part", kPart[armMinPart]}}));
    }
    else
        s = Line(L"status_damage_none", slot);
    if (lostN)
        AddPiece(s, Line(L"status_lost_parts", slot, {{L"part", lost}}));
    return s;
}

// An ammo type's spoken name, from its bin's tag (lance status v3): "Ammo.Missile.LRM.Clan" ->
// "L R M", "Ammo.Ballistic.AutoCannon.Ultra.AC5.Clan" -> "Ultra A C 5", "Ammo.Missile.SRM.Streak.Clan"
// -> "Streak S R M". The family word (Missile / Ballistic / Energy) and a generic "AutoCannon" /
// "Standard" are dropped, the weapon code goes last and is spelled out letter by letter; Clan is
// said only when the mech carries both a Clan and an Inner Sphere kind of the same ammo.
std::wstring SpellCode(const std::wstring &w)
{
    // "AC10" -> "AC 10", "LRM" -> "LRM", "LBX10" -> "LBX 10"; anything with lower case is a word.
    // The letters stay joined (build 2026-10-05f): Kokoro reads "L R M" as three stressed letters
    // with gaps and "A C" as "uh see", but "LRM" / "AC" as natural acronyms. TAG is said as a word.
    for (wchar_t ch : w)
        if (iswlower(ch))
            return w;
    if (w == L"TAG")
        return L"Tag";
    std::wstring o;
    for (size_t i = 0; i < w.size(); ++i)
    {
        if (!iswalnum(w[i]))
            continue;
        if (!o.empty() && i > 0 && (iswdigit(w[i]) != 0) != (iswdigit(w[i - 1]) != 0))
            o += L' ';
        o += w[i];
    }
    return o;
}

// ---- PRONUNCIATIONS (build 2026-10-05k) --------------------------------------------------------
// Pronunciations.txt beside the exe: words Kokoro says wrong, respelled the way they sound (Kokoro
// reads "mech" as "metch" - its espeak fallback wins over the dictionary). No rules live in the code:
// a missing file is written with its instructions only. A copy of the user's rules is kept in
// src/voice_command/Pronunciations.txt (a clean rebuild can wipe the build folder).
// One rule per line, "word = sounds like", whole words, any case (a capital stays a capital);
// "*mech = mek" also matches inside words (Mechs, BattleMech, MechWarrior).
// Saved changes are picked up within a second, like VoiceLines.txt.
struct PronRule
{
    std::wstring From, To; // From in lower case
    bool         Inside;
};
std::vector<PronRule> g_pron;
std::mutex            g_pronMx; // the speech worker reads them
FILETIME              g_pronTime = {};
ULONGLONG             g_pronCheckMs = 0;

std::wstring PronPath() { return ExeDir() + L"Pronunciations.txt"; }

void PronTick()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_pronCheckMs < 1000)
        return;
    g_pronCheckMs = now;
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(PronPath().c_str(), GetFileExInfoStandard, &fa))
    {
        WriteFileUtf8(PronPath(),
                      L"# Pronunciations.txt - words Kokoro says wrong, respelled the way they should sound.\r\n"
                      L"# One per line:  word = sounds like   (whole words, any case; a capital stays a capital)\r\n"
                      L"# A * in front also matches inside longer words:  *mech = mek  covers Mechs, BattleMech, MechWarrior.\r\n"
                      L"# Saved changes are used within a second. Try spellings like a stranger would read them aloud.\r\n"
                      L"\r\n");
        if (!GetFileAttributesExW(PronPath().c_str(), GetFileExInfoStandard, &fa))
            return;
    }
    if (CompareFileTime(&fa.ftLastWriteTime, &g_pronTime) == 0)
        return;
    g_pronTime = fa.ftLastWriteTime;
    std::vector<PronRule> rules;
    FILE *f = nullptr;
    if (_wfopen_s(&f, PronPath().c_str(), L"rb") == 0 && f)
    {
        std::string raw;
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            raw.append(buf, got);
        fclose(f);
        if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0)
            raw.erase(0, 3);
        std::wstring w;
        const int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
        if (n > 0)
        {
            w.resize((size_t)n);
            MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &w[0], n);
        }
        size_t i = 0;
        while (i < w.size())
        {
            size_t j = w.find(L'\n', i);
            if (j == std::wstring::npos)
                j = w.size();
            std::wstring l = w.substr(i, j - i);
            i = j + 1;
            const size_t eq = l.find(L'=');
            if (l.empty() || l[0] == L'#' || eq == std::wstring::npos)
                continue;
            auto trim = [](std::wstring x) {
                const size_t a = x.find_first_not_of(L" \t\r"), b = x.find_last_not_of(L" \t\r");
                return a == std::wstring::npos ? std::wstring() : x.substr(a, b - a + 1);
            };
            PronRule r;
            r.From = trim(l.substr(0, eq));
            r.To = trim(l.substr(eq + 1));
            r.Inside = !r.From.empty() && r.From[0] == L'*';
            if (r.Inside)
                r.From = trim(r.From.substr(1));
            for (auto &c : r.From)
                c = (wchar_t)towlower(c);
            if (!r.From.empty() && !r.To.empty())
                rules.push_back(r);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_pronMx);
        g_pron.swap(rules);
    }
    Log(L"pronunciations: %zu rule(s) from Pronunciations.txt", g_pron.size());
}

std::wstring ApplyPron(const std::wstring &in)
{
    std::vector<PronRule> rules;
    {
        std::lock_guard<std::mutex> lk(g_pronMx);
        rules = g_pron;
    }
    std::wstring t = in;
    for (auto &r : rules)
    {
        std::wstring low = t;
        for (auto &c : low)
            c = (wchar_t)towlower(c);
        std::wstring out;
        size_t i = 0;
        while (i < t.size())
        {
            const bool hit = low.compare(i, r.From.size(), r.From) == 0 &&
                             (r.Inside || ((i == 0 || !iswalnum(t[i - 1])) &&
                                           (i + r.From.size() >= t.size() || !iswalnum(t[i + r.From.size()]))));
            if (!hit)
            {
                out += t[i++];
                continue;
            }
            std::wstring to = r.To;
            if (iswupper(t[i]) && !to.empty())
                to[0] = (wchar_t)towupper(to[0]);
            else if (iswlower(t[i]) && !to.empty() && r.Inside)
                to[0] = (wchar_t)towlower(to[0]);
            out += to;
            i += r.From.size();
        }
        t.swap(out);
    }
    return t;
}

// Text going to Kokoro: acronyms as Kokoro says them naturally, wherever they come from (code,
// VoiceLines.txt, game names). "L R M" -> "LRM", "AC/5" -> "AC 5", "TAG" -> "Tag". Then the
// pronunciations above.
// 2026-10-10: Roman numerals in unit names said as numbers - "Atlas III" -> "Atlas Three", "Hunchback IIC"
// -> "Hunchback Two C", "Mad Cat Mk II" -> "Mad Cat Mark Two". Only a numeral word written in capitals that
// follows another word (so a lone "I" or an "II" opening a line is left alone); "Mk" before a word -> "Mark".
std::wstring SayNumerals(const std::wstring &in)
{
    static const struct
    {
        const wchar_t *From, *To;
    } kNum[] = {{L"II", L"Two"}, {L"III", L"Three"}, {L"IV", L"Four"}, {L"IIC", L"Two C"}, {L"IIIC", L"Three C"}};
    std::wstring out;
    const size_t n = in.size();
    size_t i = 0;
    while (i < n)
    {
        if (!iswalpha(in[i]) || (i > 0 && iswalnum(in[i - 1])))
        {
            out += in[i++];
            continue;
        }
        size_t j = i;
        while (j < n && iswalnum(in[j]))
            ++j;
        const std::wstring w = in.substr(i, j - i);
        // the word before: a space, then a letter (or a digit / closing bracket of a name)
        const bool afterWord = i >= 2 && in[i - 1] == L' ' &&
                               (iswalnum(in[i - 2]) || in[i - 2] == L')' || (out.size() >= 5 && out.compare(out.size() - 5, 5, L"Mark ") == 0));
        const wchar_t *rep = nullptr;
        if (afterWord)
            for (auto &k : kNum)
                if (w == k.From)
                    rep = k.To;
        if (!rep && (w == L"Mk" || w == L"MK") && j < n && (in[j] == L' ' || (in[j] == L'.' && j + 1 < n && in[j + 1] == L' ')))
        {
            out += L"Mark";
            i = (in[j] == L'.') ? j + 1 : j;
            continue;
        }
        out += rep ? std::wstring(rep) : w;
        i = j;
    }
    return out;
}

std::wstring KokoroText(const std::wstring &in0)
{
    const std::wstring in = ApplyPron(SayNumerals(in0));
    std::wstring t;
    const size_t n = in.size();
    auto isCap = [&](size_t i) { return i < n && iswupper(in[i]); };
    auto wordEdge = [&](size_t i) { return i >= n || !iswalnum(in[i]); };
    for (size_t i = 0; i < n; ++i)
    {
        const bool startWord = i == 0 || !iswalnum(in[i - 1]);
        // a run of single capitals separated by single spaces: "L R M"
        if (startWord && isCap(i) && wordEdge(i + 1) && i + 2 < n && in[i + 1] == L' ' && isCap(i + 2) && wordEdge(i + 3))
        {
            size_t j = i;
            while (isCap(j) && wordEdge(j + 1))
            {
                t += in[j];
                if (j + 2 < n && in[j + 1] == L' ' && isCap(j + 2) && wordEdge(j + 3))
                    j += 2;
                else
                {
                    ++j;
                    break;
                }
            }
            i = j - 1;
            continue;
        }
        // "AC/5" -> "AC 5"
        if (in[i] == L'/' && i > 0 && iswupper(in[i - 1]) && i + 1 < n && iswdigit(in[i + 1]))
        {
            t += L' ';
            continue;
        }
        // "TAG" as a word
        if (startWord && in.compare(i, 3, L"TAG") == 0 && wordEdge(i + 3))
        {
            t += L"Tag";
            i += 2;
            continue;
        }
        t += in[i];
    }
    return t;
}

std::wstring AmmoName(const char *tag, bool withClan)
{
    std::vector<std::wstring> seg;
    std::wstring cur;
    for (const char *p = tag; ; ++p)
    {
        if (*p == '.' || *p == 0)
        {
            if (!cur.empty())
                seg.push_back(cur);
            cur.clear();
            if (!*p)
                break;
        }
        else
            cur += (wchar_t)(unsigned char)*p;
    }
    if (!seg.empty() && seg[0] == L"Ammo")
        seg.erase(seg.begin());
    if (!seg.empty() && (seg[0] == L"Missile" || seg[0] == L"Ballistic" || seg[0] == L"Energy" || seg[0] == L"Artillery"))
        seg.erase(seg.begin());
    bool clan = false;
    std::wstring code;
    std::vector<std::wstring> mods;
    for (auto &x : seg)
    {
        if (x == L"Clan")
            clan = true;
        else if (x == L"IS" || x == L"Standard" || x == L"InnerSphere")
            continue;
        else if (x == L"AutoCannon" && seg.size() > 1)
            continue; // the code (AC5 / UAC10 ...) says it
        else
        {
            bool isCode = !x.empty(), digits = false;
            for (wchar_t ch : x)
            {
                isCode &= !iswlower(ch);
                digits |= iswdigit(ch) != 0;
            }
            // The weapon code: one with a number wins ("LBX.LBX10" -> LBX10, "AC5"), and a code that
            // only repeats the start of it ("LBX") is dropped.
            if (isCode && (code.empty() || (digits && x.compare(0, code.size(), code) == 0)))
                code = x;
            else if (isCode && code.compare(0, x.size(), x) == 0)
                continue;
            else
            {
                // "MachineGun" -> "Machine Gun"
                std::wstring w;
                for (size_t i = 0; i < x.size(); ++i)
                {
                    if (i && iswupper(x[i]) && iswlower(x[i - 1]))
                        w += L' ';
                    w += x[i];
                }
                mods.push_back(w);
            }
        }
    }
    if (code.compare(0, 3, L"UAC") == 0) // "Ultra.UAC10": the code already says Ultra
        mods.erase(std::remove(mods.begin(), mods.end(), std::wstring(L"Ultra")), mods.end());
    std::wstring o = withClan && clan ? L"Clan" : L"";
    for (auto &m : mods)
        o += (o.empty() ? L"" : L" ") + m;
    if (!code.empty())
        o += (o.empty() ? L"" : L" ") + SpellCode(code);
    if (o.empty())
        o = L"ammo";
    return o;
}

// From the bins (lance status v3): each ammo type by its weapon's name.
std::wstring StatusAmmoBins(const LsAmmoList &a, int slot)
{
    std::wstring low, out;
    for (int i = 0; i < a.Count && i < kLsAmmoTypes; ++i)
    {
        const LsAmmoBin &b = a.Bins[i];
        if (b.Max <= 0)
            continue;
        // Clan named only if another bin has the same name without it.
        bool clash = false;
        const std::wstring plain = AmmoName(b.Tag, false);
        for (int j = 0; j < a.Count && j < kLsAmmoTypes; ++j)
            clash |= j != i && AmmoName(a.Bins[j].Tag, false) == plain;
        const std::wstring name = AmmoName(b.Tag, clash);
        const int pct = (int)(100.0 * b.Count / b.Max + 0.5);
        if (b.Count <= 0)
            AddPiece(out, Line(L"status_ammo_out", slot, {{L"weapon", name}}));
        else if (pct < 50)
            AddPiece(low, Line(L"status_ammo_low", slot, {{L"weapon", name}, {L"percent", std::to_wstring(std::max(10, (pct + 5) / 10 * 10))}}));
    }
    if (low.empty() && out.empty())
        return Line(L"status_ammo_good", slot);
    AddPiece(low, out);
    return low;
}

// The fallback when the bins could not be read: by weapon category, from the weapons' own counts.
std::wstring StatusAmmo(const LsSlot &u, int slot)
{
    static const struct
    {
        uint8_t        Cat;
        const wchar_t *Word;
    } kAmmo[] = {{kFcLockMissile, L"L R M"}, {kFcDumbMissile, L"S R M"}, {kFcBallistic, L"ballistic"},
                 {kFcMGFlamer, L"machine gun"}, {kFcNARC, L"narc"}, {kFcPPC, L"P P C"}, {kFcLaser, L"laser"}, {kFcTAG, L"tag"}};
    std::wstring low, out;
    int users = 0;
    for (auto &k : kAmmo)
    {
        long have = 0, cap = 0;
        for (int w = 0; w < u.WeaponCount && w < kLsMaxWeapons; ++w)
            if (u.Weapons[w].Category == k.Cat && u.Weapons[w].AmmoMax > 0 && u.Weapons[w].Ammo >= 0)
            {
                have += u.Weapons[w].Ammo; // bins fed to several weapons count once per weapon: the ratio holds
                cap += u.Weapons[w].AmmoMax;
            }
        if (cap <= 0)
            continue;
        ++users;
        const int pct = (int)(100.0 * have / cap + 0.5);
        if (have <= 0)
            AddPiece(out, Line(L"status_ammo_out", slot, {{L"weapon", k.Word}}));
        else if (pct < 50)
            AddPiece(low, Line(L"status_ammo_low", slot, {{L"weapon", k.Word}, {L"percent", std::to_wstring(std::max(10, (pct + 5) / 10 * 10))}}));
    }
    if (!users)
        return Line(L"status_all_energy", slot);
    if (low.empty() && out.empty())
        return Line(L"status_ammo_good", slot);
    AddPiece(low, out);
    return low;
}

// Which part a weapon sits in, from its class name ("CTF_Torso_Left_EH1_Laser",
// "Weapon_Mech_AS7_Forearm_Right_EH1_Laser_C"): TerrainMechPart, -1 unknown.
int WeaponPartOf(const char *name)
{
    std::string n;
    for (const char *c = name; *c && n.size() < 64; ++c)
        n += (char)tolower((unsigned char)*c);
    const bool left = n.find("left") != std::string::npos, right = n.find("right") != std::string::npos;
    if (n.find("head") != std::string::npos)
        return TMP_Head;
    if (n.find("arm") != std::string::npos || n.find("shoulder") != std::string::npos || n.find("hand") != std::string::npos)
        return left ? TMP_LeftArm : right ? TMP_RightArm : -1;
    if (n.find("leg") != std::string::npos)
        return left ? TMP_LeftLeg : right ? TMP_RightLeg : -1;
    if (n.find("torso") != std::string::npos)
        return left ? TMP_LeftTorso : right ? TMP_RightTorso : TMP_CentreTorso;
    return -1;
}

// What the lancemate in this lance number says to "status".
std::wstring StatusLine(int slot, int what)
{
    std::wstring s = Line(L"status_intro", slot);
    if (!LsConnect())
    {
        AddPiece(s, Line(L"status_no_systems", slot));
        return s;
    }
    LsSlot all[kLsMaxSlots];
    LsCondition cond[kLsMaxSlots];
    LsAmmoList ammo[kLsMaxSlots];
    const int n = LsReadSlots(all, cond, ammo);
    const LsSlot *u = nullptr;
    const LsCondition *c = nullptr;
    const LsAmmoList *am = nullptr;
    for (int i = 0; i < n; ++i)
        if (all[i].Valid && !all[i].IsPlayer && all[i].Slot == slot)
        {
            u = &all[i];
            c = &cond[i];
            am = &ammo[i];
        }
    if (!u)
    {
        AddPiece(s, Line(L"status_no_systems", slot));
        return s;
    }
    // Damage: LanceControl's own read (lance status v2, any distance); LidarExport's within 1000 m
    // as the fallback.
    TerrainMechInfo m = {};
    const float *stc = nullptr; // the structure read used, for the lost-weapon count below
    if (!(what & ST_Damage))
        ; // only part of the report was asked for ("how's your ammo")
    else if (c && c->Have)
    {
        AddPiece(s, StatusDamage(c->Armour, c->Structure, slot));
        stc = c->Structure;
    }
    else if (MechConditionOf(u->ActorLo, u->ActorHi, m))
    {
        AddPiece(s, StatusDamage(m.Armour, m.Structure, slot));
        stc = m.Structure;
    }
    else
        AddPiece(s, Line(L"status_no_damage_read", slot));
    // Ammo by type from the bins (v3); the per-weapon category count as the fallback.
    if (what & ST_Ammo)
        AddPiece(s, am && am->Count ? StatusAmmoBins(*am, slot) : StatusAmmo(*u, slot));
    if ((what & ST_HeatAlways) && u->Heat >= 0.0f && u->Heat < 0.7f)
        AddPiece(s, Line(u->Heat < 0.4f ? L"status_heat_ok" : L"status_heat_level", slot,
                         {{L"percent", std::to_wstring((int)std::lround(u->Heat * 100.0f))}}));
    else if ((what & (ST_Heat | ST_HeatAlways)) && u->Heat >= 0.7f)
        AddPiece(s, Line(L"status_running_hot", slot));
    // Lost weapons - not the ones that went with a lost part (build 2026-10-05h): "lost my left arm"
    // already says its lasers are gone.
    int gone = 0;
    for (int w = 0; (what & ST_Weapons) && w < u->WeaponCount && w < kLsMaxWeapons; ++w)
        if (u->Weapons[w].State == LSW_Destroyed)
        {
            const int part = WeaponPartOf(u->Weapons[w].Name);
            if (!(stc && part >= 0 && stc[part] >= 0.0f && stc[part] <= 0.01f))
                ++gone;
        }
    if (gone == 1)
        AddPiece(s, Line(L"status_lost_weapon", slot));
    else if (gone > 1)
        AddPiece(s, Line(L"status_lost_weapons", slot, {{L"count", std::to_wstring(gone)}}));
    return s;
}

// ---- KOKORO VOICES + RADIO -------------------------------------------------------------------
// The pilots' lines can be spoken by Kokoro (sherpa-onnx, on the CPU) instead of the Windows voices,
// and either engine's speech can go through a RADIO filter: band-limited to a military handset
// (high-pass ~350 Hz, low-pass ~3 kHz, 4th order each), driven into soft saturation, a little
// sample-and-hold grit, a bed of band-limited static, random crackle pops, and a squelch click +
// static burst at key-up and key-down. The band-limiting and noise hide most of what makes
// synthetic speech sound synthetic, and it sounds like comms in a cockpit.
// Everything runs on one worker thread (synthesis, filter, playback through waveOut) at below-
// normal priority, one line at a time; the main loop only hands it the next line when it is free.
// VoiceCommand.ini:
//   tts            = kokoro (default when built in and the model is found) | windows
//   kokoro_dir     = the Kokoro model folder (model.onnx, voices.bin, tokens.txt, espeak-ng-data)
//   kokoro_threads = CPU threads for Kokoro (default 2)
//   kokoro_speed   = speaking rate (default 1.0; exclamations a little faster)
//   kokoro_voice_N / kokoro_voice_<PilotName> = a Kokoro voice name (am_michael, bf_emma, ...) or id
//   radio          = 1 radio filter on (default), 0 clean
//   radio_static   = static level, 0 .. 1 (default 0.35 = mild)
//   radio_crackle  = crackle pops per second (default 1.5)
//   radio_volume   = output volume, 0 .. 1 (default 0.9)
//   radio_start    = the cue that opens a transmission: beep (default - two quick tones, rising),
//                    chirp (a short digital sync burst), squelch (an FM "kerchunk"), file
//                    (radio_start.wav beside the exe - any recording you like), none
//   radio_end      = the cue that closes it: beep (default - the same two tones, falling), squelch
//                    (the FM tail), chirp, file (radio_end.wav), none
//   radio_cue_volume = loudness of those cues, 0 .. 1 (default 0.6)
//   radio_gap      = silence after each transmission, seconds (default 0.35), so one line's closing
//                    tones and the next line's opening tones do not run together
//   radio_fry      = fine "frying" crackle under the voice, 0 .. 1 (default 0.5)
//   radio_wash     = static washes that swell over the voice, 0 .. 1 (default 0.5)
//   radio_whistle  = faint drifting heterodyne whistle, 0 .. 1 (default 0.3)
//   radio_hum      = alternator buzz, 0 .. 1 (default 0 - the reference recordings have none)
//   radio_band_low / radio_band_high = the radio's band on the whole mix, Hz (default 250 / 3600)
//   radio_presence = lift around 1.5 kHz, dB (default 4)
//   radio_limit    = peak limiter threshold, 0.1 .. 1 (default 0.45; lower = denser, 1 = off)
//   radio_dropout  = brief voice dropouts per second (default 0.4, 0 = off)
//   (the hiss bed itself also flutters now, like a weak carrier)
// Two threads: one synthesises (the next line is made while the current one plays - no gap
// for synthesis between lines), one plays.
std::wstring PilotNameOf(int slot);

struct RadioCfg
{
    int          Tts = 1; // 1 kokoro, 0 windows
    std::wstring KokoroDir;
    int          KokoroThreads = 2;
    float        KokoroSpeed = 1.0f;
    int          Radio = 1;
    float        Static = 0.35f, Crackle = 1.5f, Volume = 0.9f, CueVolume = 0.6f, Gap = 0.35f;
    float        Fry = 0.5f, Wash = 0.5f, Whistle = 0.3f, Hum = 0.0f, Dropout = 0.4f;
    float        BandLow = 250.0f, BandHigh = 3600.0f, Presence = 4.0f, Limit = 0.45f;
    std::wstring StartCue = L"beep", EndCue = L"beep";
} g_rc;

const wchar_t *const kKokoroNames[] = {
    L"af_alloy", L"af_aoede", L"af_bella", L"af_heart", L"af_jessica", L"af_kore", L"af_nicole", L"af_nova", L"af_river",
    L"af_sarah", L"af_sky", L"am_adam", L"am_echo", L"am_eric", L"am_fenrir", L"am_liam", L"am_michael", L"am_onyx",
    L"am_puck", L"am_santa", L"bf_alice", L"bf_emma", L"bf_isabella", L"bf_lily", L"bm_daniel", L"bm_fable", L"bm_george",
    L"bm_lewis"}; // ids 0..27 of kokoro-multi-lang-v1_0 (the English ones)
// Defaults per lance number: distinct, the better-rated voices first. 0 = the lance voice.
const wchar_t *const kKokoroDefault[9] = {L"bm_george", L"am_adam", L"am_michael", L"af_heart", L"bm_lewis",
                                          L"am_fenrir", L"bf_emma", L"am_puck", L"af_bella"};

int KokoroSid(const std::wstring &v)
{
    if (v.empty())
        return -1;
    if (iswdigit(v[0]))
        return _wtoi(v.c_str());
    for (int i = 0; i < (int)(sizeof(kKokoroNames) / sizeof(kKokoroNames[0])); ++i)
        if (_wcsicmp(v.c_str(), kKokoroNames[i]) == 0)
            return i;
    return -1;
}

std::wstring IniStr(const wchar_t *key, const wchar_t *def)
{
    wchar_t b[512] = L"";
    GetPrivateProfileStringW(L"voice", key, def, b, 512, VcIni().c_str());
    return b;
}

void LoadRadioConfig()
{
    const std::wstring ini = VcIni();
    if (GetPrivateProfileIntW(L"voice", L"radio", -1, ini.c_str()) == (UINT)-1)
    {
        // written once, so the settings can be found
        WritePrivateProfileStringW(L"voice", L"tts", L"kokoro", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"kokoro_dir", gs::kDefKokoroDir, ini.c_str()); // release: beside the exe
        WritePrivateProfileStringW(L"voice", L"kokoro_threads", L"2", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"kokoro_speed", L"1.0", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio", L"1", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_static", L"0.35", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_crackle", L"1.5", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_volume", L"0.9", ini.c_str());
        for (int i = 0; i <= 4; ++i)
            WritePrivateProfileStringW(L"voice", (L"kokoro_voice_" + std::to_wstring(i)).c_str(), kKokoroDefault[i], ini.c_str());
    }
    g_rc.Tts = _wcsicmp(IniStr(L"tts", L"kokoro").c_str(), L"windows") == 0 ? 0 : 1;
    g_rc.KokoroDir = gs::Resolve(IniStr(L"kokoro_dir", gs::kDefKokoroDir)); // release: a relative path is beside the exe
    g_rc.KokoroThreads = std::max(1, _wtoi(IniStr(L"kokoro_threads", L"2").c_str()));
    g_rc.KokoroSpeed = (float)_wtof(IniStr(L"kokoro_speed", L"1.0").c_str());
    if (g_rc.KokoroSpeed < 0.5f || g_rc.KokoroSpeed > 2.0f)
        g_rc.KokoroSpeed = 1.0f;
    g_rc.Radio = _wtoi(IniStr(L"radio", L"1").c_str());
    g_rc.Static = std::max(0.0f, std::min(1.0f, (float)_wtof(IniStr(L"radio_static", L"0.35").c_str())));
    g_rc.Crackle = std::max(0.0f, std::min(20.0f, (float)_wtof(IniStr(L"radio_crackle", L"1.5").c_str())));
    g_rc.Volume = std::max(0.0f, std::min(1.0f, (float)_wtof(IniStr(L"radio_volume", L"0.9").c_str())));
    g_rc.CueVolume = std::max(0.0f, std::min(1.0f, (float)_wtof(IniStr(L"radio_cue_volume", L"0.6").c_str())));
    g_rc.Gap = std::max(0.0f, std::min(3.0f, (float)_wtof(IniStr(L"radio_gap", L"0.35").c_str())));
    g_rc.StartCue = IniStr(L"radio_start", L"beep");
    g_rc.EndCue = IniStr(L"radio_end", L"beep");
    if (IniStr(L"radio_start", L"\x01") == L"\x01") // written once, so the settings can be found
    {
        WritePrivateProfileStringW(L"voice", L"radio_start", L"beep", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_end", L"beep", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_cue_volume", L"0.6", ini.c_str());
    }
    auto knob = [&](const wchar_t *k, const wchar_t *d, float hi) {
        return std::max(0.0f, std::min(hi, (float)_wtof(IniStr(k, d).c_str())));
    };
    g_rc.Fry = knob(L"radio_fry", L"0.5", 1.0f);
    g_rc.Wash = knob(L"radio_wash", L"0.5", 1.0f);
    g_rc.Whistle = knob(L"radio_whistle", L"0.3", 1.0f);
    g_rc.Hum = knob(L"radio_hum", L"0", 1.0f);
    g_rc.BandLow = std::max(50.0f, knob(L"radio_band_low", L"250", 1000.0f));
    g_rc.BandHigh = std::max(1500.0f, knob(L"radio_band_high", L"3600", 8000.0f));
    g_rc.Presence = knob(L"radio_presence", L"4", 12.0f);
    g_rc.Limit = std::max(0.1f, knob(L"radio_limit", L"0.45", 1.0f));
    g_rc.Dropout = knob(L"radio_dropout", L"0.4", 5.0f);
    if (IniStr(L"radio_fry", L"\x01") == L"\x01") // written once, so the settings can be found
    {
        WritePrivateProfileStringW(L"voice", L"radio_fry", L"0.5", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_wash", L"0.5", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_whistle", L"0.3", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_hum", L"0", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_dropout", L"0.4", ini.c_str());
    }
    if (IniStr(L"radio_band_high", L"\x01") == L"\x01") // written once, so the settings can be found
    {
        WritePrivateProfileStringW(L"voice", L"radio_band_low", L"250", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_band_high", L"3600", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_presence", L"4", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_limit", L"0.45", ini.c_str());
        WritePrivateProfileStringW(L"voice", L"radio_hum", L"0", ini.c_str()); // hum is off now
    }
}

// ---- PILOT VOICES (2026-10-07b): a voice belongs to the PILOT, not the lance number ----------------
// Lance numbers can change (a lancemate dies, you take over a mech, the next mission's lance order), so
// a voice picked by number jumped to another pilot. Now: the first time a pilot's name is seen, it gets
// the voice its lance number has at that moment (kokoro_voice_N / the default) - or, if a pilot in the
// lance already speaks with that one, the next free voice - and keeps it, saved in PilotVoices.txt
// beside the exe ("Name = voice"; edited in GlobalSettings too - re-read when it changes, VcSettingsTick).
// kokoro_voice_<Name> in the ini still wins over the file.
std::mutex g_pvMx;
std::map<std::wstring, std::wstring> g_pilotVoice; // pilot name -> Kokoro voice name
bool g_pvLoaded = false;

std::wstring PilotVoicesPath() { return ExeDir() + L"PilotVoices.txt"; }

static std::wstring PvTrim(const std::wstring &t)
{
    const size_t a = t.find_first_not_of(L" \t\r\n"), b = t.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : t.substr(a, b - a + 1);
}

static std::string PvU8(const std::wstring &w)
{
    std::string u;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n > 0)
    {
        u.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
    }
    return u;
}

static std::wstring PvW(const std::string &raw)
{
    std::wstring w;
    const int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
    if (n > 0)
    {
        w.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &w[0], n);
    }
    return w;
}

void PilotVoicesSave() // g_pvMx held
{
    FILE *f = nullptr;
    if (_wfopen_s(&f, PilotVoicesPath().c_str(), L"wb") != 0 || !f)
        return;
    std::string out = "# PilotVoices.txt - which voice each pilot speaks with, kept across missions.\r\n"
                      "# Written by VoiceCommand the first time it hears of a pilot. Edit only with VoiceCommand closed.\r\n"
                      "# Name = Kokoro voice (af_*, am_*, bf_*, bm_* - see kokoro_voice_N in VoiceCommand.ini). Delete a line\r\n"
                      "# to let that pilot be given a voice again.\r\n";
    for (auto &e : g_pilotVoice)
        out += PvU8(e.first + L" = " + e.second) + "\r\n";
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

void PilotVoicesLoad() // g_pvMx held
{
    g_pvLoaded = true;
    FILE *f = nullptr;
    if (_wfopen_s(&f, PilotVoicesPath().c_str(), L"rb") != 0 || !f)
        return;
    std::string raw;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        raw.append(buf, n);
    fclose(f);
    const std::wstring all = PvW(raw);
    size_t pos = 0;
    while (pos <= all.size())
    {
        size_t e = all.find(L'\n', pos);
        if (e == std::wstring::npos)
            e = all.size();
        const std::wstring line = PvTrim(all.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == L'#')
            continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;
        const std::wstring k = PvTrim(line.substr(0, eq)), v = PvTrim(line.substr(eq + 1));
        if (!k.empty() && KokoroSid(v) >= 0)
            g_pilotVoice[k] = v;
    }
    Log(L"pilot voices: %zu from PilotVoices.txt", g_pilotVoice.size());
}

// A voice set for this pilot in VoiceCommand.ini (kokoro_voice_<Name> / kokoro_voice_<Callsign>), "" none.
std::wstring PilotIniVoice(const std::wstring &name)
{
    if (name.empty())
        return L"";
    std::wstring v = IniStr((L"kokoro_voice_" + name).c_str(), L"");
    if (KokoroSid(v) < 0 && CallName(name) != name)
        v = IniStr((L"kokoro_voice_" + CallName(name)).c_str(), L"");
    return KokoroSid(v) >= 0 ? v : L"";
}

std::wstring PilotVoiceOf(const std::wstring &name, int slot)
{
    if (name.empty())
        return L"";
    {
        const std::wstring iv = PilotIniVoice(name); // the ini wins - nothing given, nothing saved
        if (!iv.empty())
            return iv;
    }
    const int s = (slot < 0 || slot > 8) ? 0 : slot;
    // the other pilots in the lance now (read before taking g_pvMx: lock order g_pvMx -> g_pilotMx never nests)
    std::wstring others[9];
    for (int i = 1; i <= 8; ++i)
        if (i != s)
            others[i] = PilotNameOf(i);
    std::lock_guard<std::mutex> lk(g_pvMx);
    if (!g_pvLoaded)
        PilotVoicesLoad();
    auto it = g_pilotVoice.find(name);
    if (it != g_pilotVoice.end())
        return it->second;
    std::set<int> used;
    used.insert(KokoroSid(IniStr(L"kokoro_voice_0", kKokoroDefault[0]))); // the lance voice
    for (int i = 1; i <= 8; ++i)
        if (!others[i].empty())
        {
            const std::wstring iv = PilotIniVoice(others[i]); // (2026-10-07e: an ini voice is taken too)
            auto o = g_pilotVoice.find(others[i]);
            if (!iv.empty())
                used.insert(KokoroSid(iv));
            else if (o != g_pilotVoice.end())
                used.insert(KokoroSid(o->second));
        }
    std::vector<std::wstring> cand;
    cand.push_back(IniStr((L"kokoro_voice_" + std::to_wstring(s)).c_str(), kKokoroDefault[s]));
    for (int i = 1; i <= 8; ++i)
        cand.push_back(IniStr((L"kokoro_voice_" + std::to_wstring(i)).c_str(), kKokoroDefault[i]));
    for (auto *k : kKokoroNames)
        cand.push_back(k);
    std::wstring pick;
    for (auto &c : cand)
        if (KokoroSid(c) >= 0 && !used.count(KokoroSid(c)))
        {
            pick = c;
            break;
        }
    if (pick.empty())
        pick = cand[0];
    g_pilotVoice[name] = pick;
    PilotVoicesSave();
    Log(L"pilot voices: %ls speaks with %ls from now on (saved in PilotVoices.txt)", name.c_str(), pick.c_str());
    return pick;
}

// ---- GLOBAL SETTINGS, LIVE (2026-10-07f) ---------------------------------------------------------
// GlobalSettings.ini is watched twice a second; what can change while running is re-read at once -
// numbers only (other threads read them; strings like model paths are start-only). The start-only
// ones are logged when changed ("used when VoiceCommand restarts"). PilotVoices.txt is re-read when
// it changes. ([lance] is read by LanceControl itself since 2026-10-07h - nothing is relayed any more.)
gs::Watch     g_gsWatch{gs::Path()};
gs::Watch     g_pvWatch{gs::Dir() + L"PilotVoices.txt"};
void LoadCallouts(bool writeMissing); // CALLOUT SWITCHES, beside kCall
std::map<std::wstring, std::wstring> g_gsAtStart; // the start-only settings, as first read

int KokoroVoiceFor(int slot); // below

void VcSettingsTick()
{
    const ULONGLONG now = GetTickCount64();
    if (g_gsWatch.Changed(now))
    {
        const bool first = g_gsAtStart.empty();
        g_cfg.Speak = gs::Int(L"voice", L"speak", 1);
        g_cfg.Callouts = gs::Int(L"voice", L"callouts", 1);
        g_cfg.Chatter = std::max(0, std::min(3, gs::Int(L"voice", L"chatter", 2)));
        g_cfg.MinConf = gs::Int(L"voice", L"min_confidence", 1);
        g_cfg.PttVk = gs::Int(L"voice", L"ptt_vk", 0);
        g_cfg.WhisperFast = gs::Int(L"voice", L"whisper_trim", 1);
        const std::wstring lv = gs::Str(L"voice", L"listen", L"ptt");
        int listen = _wcsicmp(lv.c_str(), L"always") == 0 ? 0 : (_wcsicmp(lv.c_str(), L"both") == 0 ? 2 : 1);
        // 2026-10-09: push-to-talk without a key hears NOTHING (it used to fall back to always listening,
        // which defeats push-to-talk as the default); "both" without a key is simply "always".
        if (listen == 2 && !g_cfg.PttVk)
            listen = 0;
        if (listen == 1 && !g_cfg.PttVk && !first)
            Log(L"settings: push-to-talk has no key yet - set one in GlobalSettings (VOICE COMMAND > Push-to-talk key); nothing is heard until then");
        g_cfg.Listen = listen;
        auto num = [](const wchar_t *k, double d, double lo, double hi) {
            return (float)std::max(lo, std::min(hi, gs::Num(L"voice", k, d)));
        };
        const float sp = (float)gs::Num(L"voice", L"kokoro_speed", 1.0);
        g_rc.KokoroSpeed = (sp < 0.5f || sp > 2.0f) ? 1.0f : sp;
        g_rc.Radio = gs::Int(L"voice", L"radio", 1);
        g_rc.Static = num(L"radio_static", 0.35, 0, 1);
        g_rc.Crackle = num(L"radio_crackle", 1.5, 0, 20);
        g_rc.Volume = num(L"radio_volume", 0.9, 0, 1);
        g_rc.CueVolume = num(L"radio_cue_volume", 0.6, 0, 1);
        g_rc.Gap = num(L"radio_gap", 0.35, 0, 3);
        g_rc.Fry = num(L"radio_fry", 0.5, 0, 1);
        g_rc.Wash = num(L"radio_wash", 0.5, 0, 1);
        g_rc.Whistle = num(L"radio_whistle", 0.3, 0, 1);
        g_rc.Hum = num(L"radio_hum", 0, 0, 1);
        g_rc.BandLow = std::max(50.0f, num(L"radio_band_low", 250, 0, 1000));
        g_rc.BandHigh = std::max(1500.0f, num(L"radio_band_high", 3600, 0, 8000));
        g_rc.Presence = num(L"radio_presence", 4, 0, 12);
        g_rc.Limit = std::max(0.1f, num(L"radio_limit", 0.45, 0, 1));
        g_rc.Dropout = num(L"radio_dropout", 0.4, 0, 5);
        LoadCallouts(first);
        static const wchar_t *const kAtStart[] = {L"recognizer", L"whisper_model", L"whisper_threads", L"whisper_flash_attn", L"whisper_gpu",
                                                  L"english_only", L"tts", L"kokoro_dir", L"kokoro_threads", L"radio_start",
                                                  L"radio_end", L"rate", L"voice_0", L"voice_1", L"voice_2", L"voice_3", L"voice_4"};
        for (auto *k : kAtStart)
        {
            const std::wstring v = gs::Str(L"voice", k, L"");
            auto it = g_gsAtStart.find(k);
            if (first || it == g_gsAtStart.end())
                g_gsAtStart[k] = v;
            else if (it->second != v)
            {
                it->second = v;
                Log(L"settings: %ls changed - used when VoiceCommand restarts", k);
            }
        }
        // GlobalSettings' TEST buttons (2026-10-07g): [test] seq changes -> say [test] text in [test] voice (or the
        // voice lance number [test] slot has). Said by "the lance" (slot 0), so no pilot's last line or turn changes.
        static std::wstring lastSeq;
        const std::wstring seq = gs::Str(L"test", L"seq", L"");
        const bool test = !first && !seq.empty() && seq != lastSeq;
        lastSeq = seq;
        if (test)
        {
            std::wstring voice = gs::Str(L"test", L"voice", L"");
            if (voice.empty())
                voice = std::to_wstring(KokoroVoiceFor(std::max(0, std::min(8, gs::Int(L"test", L"slot", 0)))));
            const std::wstring text = gs::Str(L"test", L"text", L"Radio check.");
            Log(L"settings: voice test (%ls)", voice.c_str());
            g_sayVoice = voice;
            Say(0, text);
            g_sayVoice.clear();
        }
        else if (!first)
            Log(L"settings: GlobalSettings.ini changed - applied");
    }
    if (g_pvWatch.Changed(now))
    {
        std::lock_guard<std::mutex> lk(g_pvMx);
        g_pilotVoice.clear(); // read again on next use
        g_pvLoaded = false;
    }
}

// The Kokoro voice for this lance number: the pilot's own (kokoro_voice_<Name>, then PilotVoices.txt),
// the number's, the default.
int KokoroVoiceFor(int slot)
{
    const int s = (slot < 0 || slot > 8) ? 0 : slot;
    const std::wstring pn = PilotNameOf(s);
    int sid = -1;
    if (!pn.empty())
        sid = KokoroSid(IniStr((L"kokoro_voice_" + pn).c_str(), L""));
    if (sid < 0 && !pn.empty() && CallName(pn) != pn)
        sid = KokoroSid(IniStr((L"kokoro_voice_" + CallName(pn)).c_str(), L"")); // kokoro_voice_Hale
    if (sid < 0 && !pn.empty())
        sid = KokoroSid(PilotVoiceOf(pn, s)); // 2026-10-07b: the voice follows the pilot
    if (sid < 0)
        sid = KokoroSid(IniStr((L"kokoro_voice_" + std::to_wstring(s)).c_str(), L""));
    if (sid < 0)
        sid = KokoroSid(kKokoroDefault[s]);
    return sid;
}

// ---- the radio filter ----
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void Set(bool high, double f, double sr, double q = 0.7071)
    {
        const double w = 2.0 * 3.14159265358979 * f / sr, c = std::cos(w), al = std::sin(w) / (2.0 * q), a0 = 1.0 + al;
        if (high)
        {
            b0 = (1.0 + c) / 2.0 / a0;
            b1 = -(1.0 + c) / a0;
        }
        else
        {
            b0 = (1.0 - c) / 2.0 / a0;
            b1 = (1.0 - c) / a0;
        }
        b2 = b0;
        a1 = -2.0 * c / a0;
        a2 = (1.0 - al) / a0;
        z1 = z2 = 0;
    }
    double Run(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct BandPass // 4th-order high-pass + 4th-order low-pass (two biquads each)
{
    Biquad h1, h2, l1, l2;
    void Set(double lo, double hi, double sr)
    {
        h1.Set(true, lo, sr, 0.54);
        h2.Set(true, lo, sr, 1.31);
        l1.Set(false, hi, sr, 0.54);
        l2.Set(false, hi, sr, 1.31);
    }
    double Run(double x) { return l2.Run(l1.Run(h2.Run(h1.Run(x)))); }
};
// Peaking EQ (the radio's presence lift)
struct Peak
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void Set(double f, double sr, double db, double q)
    {
        const double A = std::pow(10.0, db / 40.0), w = 2.0 * 3.14159265358979 * f / sr, al = std::sin(w) / (2.0 * q),
                     c = std::cos(w), a0 = 1.0 + al / A;
        b0 = (1.0 + al * A) / a0;
        b1 = -2.0 * c / a0;
        b2 = (1.0 - al * A) / a0;
        a1 = -2.0 * c / a0;
        a2 = (1.0 - al / A) / a0;
        z1 = z2 = 0;
    }
    double Run(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

uint32_t g_rng = 0x1234567u;
inline float Rnd() // -1 .. 1
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)((g_rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

// Speech in (mono floats, any rate) -> a radio transmission out (16-bit), with key-up and key-down.
// ---- transmission cues: what tells you a transmission is starting (and, optionally, ending) ----
// Built from clean tones / shaped noise (the earlier noise-burst squelch sounded wrong), or a WAV
// of your own. Each is band-limited like the voice so it sits in the same radio.
std::vector<float> LoadWavMono(const std::wstring &path, int sr)
{
    std::vector<float> out;
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return out;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        d.insert(d.end(), buf, buf + got);
    fclose(f);
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return out;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    size_t i = 12;
    const uint8_t *data = nullptr;
    size_t dataLen = 0;
    while (i + 8 <= d.size())
    {
        uint32_t sz;
        std::memcpy(&sz, d.data() + i + 4, 4);
        if (std::memcmp(d.data() + i, "fmt ", 4) == 0 && i + 24 <= d.size())
        {
            uint16_t v16;
            uint32_t v32;
            std::memcpy(&v16, d.data() + i + 8, 2);
            fmt = v16;
            std::memcpy(&v16, d.data() + i + 10, 2);
            ch = v16;
            std::memcpy(&v32, d.data() + i + 12, 4);
            rate = (int)v32;
            std::memcpy(&v16, d.data() + i + 22, 2);
            bits = v16;
        }
        else if (std::memcmp(d.data() + i, "data", 4) == 0)
        {
            data = d.data() + i + 8;
            dataLen = std::min((size_t)sz, d.size() - i - 8);
        }
        i += 8 + sz + (sz & 1);
    }
    if (!data || ch < 1 || rate < 4000 || !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32) || (fmt == 0xFFFE && (bits == 16 || bits == 32))))
        return out;
    const bool isFloat = fmt == 3 || (fmt == 0xFFFE && bits == 32);
    const size_t frames = dataLen / (size_t)(ch * bits / 8);
    std::vector<float> mono(frames);
    for (size_t k = 0; k < frames; ++k)
    {
        float v = 0.0f;
        for (int c = 0; c < ch; ++c)
        {
            const uint8_t *p = data + (k * ch + c) * (bits / 8);
            if (isFloat)
            {
                float fv;
                std::memcpy(&fv, p, 4);
                v += fv;
            }
            else
            {
                int16_t iv;
                std::memcpy(&iv, p, 2);
                v += iv / 32768.0f;
            }
        }
        mono[k] = v / ch;
    }
    const double step = (double)rate / sr; // linear resample to the speech rate
    for (double pos = 0.0; pos + 1.0 < (double)frames; pos += step)
    {
        const size_t k = (size_t)pos;
        const float fr = (float)(pos - k);
        out.push_back(mono[k] * (1.0f - fr) + mono[k + 1] * fr);
    }
    return out;
}

std::vector<float> MakeCue(const std::wstring &style, bool start, int sr)
{
    const float fs = (float)sr;
    std::vector<float> c;
    auto tone = [&](float hz, float len) {
        const size_t n = (size_t)(fs * len), edge = (size_t)(fs * 0.004f);
        for (size_t i = 0; i < n; ++i)
        {
            const float e = i < edge ? 0.5f - 0.5f * std::cos(3.14159265f * i / edge)
                                     : (i > n - edge ? 0.5f - 0.5f * std::cos(3.14159265f * (n - i) / edge) : 1.0f);
            c.push_back(0.45f * e * std::sin(2.0f * 3.14159265f * hz * i / fs));
        }
    };
    auto gap = [&](float len) { c.insert(c.end(), (size_t)(fs * len), 0.0f); };
    if (_wcsicmp(style.c_str(), L"beep") == 0)
    {
        // two quick tones: rising to open the transmission, the same two falling to close it
        tone(start ? 1050.0f : 1400.0f, 0.045f);
        gap(0.025f);
        tone(start ? 1400.0f : 1050.0f, 0.045f);
    }
    else if (_wcsicmp(style.c_str(), L"chirp") == 0)
    {
        // A digital sync burst: phase-continuous FSK, 1200 / 2400 Hz at 600 baud, with a short
        // lead tone - the "brrp" of an encrypted set synchronising.
        tone(1800.0f, 0.03f);
        const size_t n = (size_t)(fs * (start ? 0.13f : 0.08f)), bit = (size_t)(fs / 600.0f), edge = (size_t)(fs * 0.004f);
        float ph = 0.0f, hz = 1200.0f;
        for (size_t i = 0; i < n; ++i)
        {
            if (i % bit == 0)
                hz = Rnd() > 0.0f ? 2400.0f : 1200.0f;
            ph += 2.0f * 3.14159265f * hz / fs;
            const float e = i < edge ? (float)i / edge : (i > n - edge ? (float)(n - i) / edge : 1.0f);
            c.push_back(0.38f * e * std::sin(ph));
        }
    }
    else if (_wcsicmp(style.c_str(), L"squelch") == 0)
    {
        // FM receiver noise: rising (differentiated) spectrum, on at once, held, cut off sharply -
        // a short "kerchunk" to open, a longer tail to close.
        const size_t n = (size_t)(fs * (start ? 0.07f : 0.18f)), cut = (size_t)(fs * 0.003f);
        float prev = 0.0f;
        for (size_t i = 0; i < n; ++i)
        {
            const float w = Rnd(), d = w - prev;
            prev = w;
            const float e = i < (size_t)(fs * 0.002f) ? (float)i / (fs * 0.002f) : (i > n - cut ? (float)(n - i) / cut : 1.0f);
            c.push_back(0.30f * e * d);
        }
    }
    else if (_wcsicmp(style.c_str(), L"file") == 0)
        c = LoadWavMono(ExeDir() + (start ? L"radio_start.wav" : L"radio_end.wav"), sr);
    // band-limited like the radio (a little wider, so tones stay clean)
    if (!c.empty())
    {
        BandPass bp;
        bp.Set(250.0, 4000.0, sr);
        for (auto &v : c)
            v = (float)bp.Run(v) * g_rc.CueVolume;
    }
    return c;
}

// Sound design: voice 300 Hz - 3.2 kHz (4th order each way), a fast compressor, soft saturation, a
// little sample-and-hold grit, slow slight fading (QSB); band-limited static under it (faded in
// and out at the ends) with occasional short filtered crackle bursts. Transmission cues
// (radio_start / radio_end, MakeCue) replace the synthetic squelch bursts and PTT clicks (tried
// twice, removed at the user's request).
// first / last: this piece opens / closes the transmission (cues, hiss fades and the gap after it
// only there - the sentences in between join up seamlessly).
std::vector<int16_t> RadioProcess(const std::vector<float> &in, int sr, bool first = true, bool last = true)
{
    const bool radio = g_rc.Radio != 0;
    const float fs = (float)sr;
    // The cues (radio_start / radio_end), with a short gap before / after the voice.
    const std::vector<float> cueIn = radio && first ? MakeCue(g_rc.StartCue, true, sr) : std::vector<float>();
    const std::vector<float> cueOut = radio && last ? MakeCue(g_rc.EndCue, false, sr) : std::vector<float>();
    const int pre = radio && first ? (int)(fs * 0.06f) + (int)cueIn.size() : 0;
    const int post = radio && last ? (int)(fs * 0.10f) + (int)cueOut.size() : 0;
    const size_t gapAfter = last ? (size_t)(fs * g_rc.Gap) : 0; // silence after a transmission
    std::vector<float> x((size_t)pre, 0.0f);
    float peak = 0.0f;
    for (float v : in)
        peak = std::max(peak, std::fabs(v));
    const float norm = peak > 0.001f ? 0.8f / peak : 1.0f;
    for (float v : in)
        x.push_back(v * norm);
    x.insert(x.end(), (size_t)post, 0.0f);
    const size_t n = x.size(), speechEnd = n - (size_t)post;
    std::vector<int16_t> out(n);
    if (!radio)
    {
        for (size_t i = 0; i < n; ++i)
            out[i] = (int16_t)std::max(-32767.0f, std::min(32767.0f, x[i] * g_rc.Volume * 32767.0f));
        out.insert(out.end(), gapAfter, (int16_t)0);
        return out;
    }
    BandPass voice, hiss, crk, fryF, washF, humF;
    voice.Set(450.0, 3000.0, sr);
    // Matched to the BattleChatter mod's radio recordings (build 2026-10-05d): presence lift, a steep
    // band on the whole mix (they have almost nothing above ~3.6 kHz or below ~250 Hz) and a peak
    // limiter (their level is dense and flat) - no distortion.
    Peak pk;
    pk.Set(1500.0, sr, g_rc.Presence, 0.9);
    const double bandHi = std::min((double)g_rc.BandHigh, 0.45 * sr);
    BandPass fin1, fin2;
    fin1.Set(g_rc.BandLow, bandHi, sr);
    fin2.Set(g_rc.BandLow, bandHi, sr);
    float lim = 0.0f;
    const float limRel = std::exp(-1.0f / (0.060f * fs));
    hiss.Set(450.0, 3800.0, sr);
    crk.Set(700.0, 4500.0, sr);
    fryF.Set(1800.0, std::min(5500.0, 0.45 * sr), sr); // fine "frying" crackle, bright
    washF.Set(250.0, 2500.0, sr);                         // static washes, darker
    humF.Set(150.0, 1800.0, sr);                          // alternator buzz
    const float stat = 0.11f * g_rc.Static;
    const double popRate = g_rc.Crackle / fs;
    const int hold = std::max(1, sr / 11025);
    const double drive = 2.2, norm2 = std::tanh(drive);
    // the other kinds of static (build 2026-10-05c) - each has its own ini knob, 0 = off
    const float fryLvl = 0.06f * g_rc.Fry, washLvl = 0.16f * g_rc.Wash, whistleLvl = 0.010f * g_rc.Whistle,
                humLvl = 0.030f * g_rc.Hum;
    const double washRate = 0.45 / fs, dropRate = g_rc.Dropout / fs, twoPi = 6.283185307179586;
    // compressor
    const float atk = std::exp(-1.0f / (0.004f * fs)), rel = std::exp(-1.0f / (0.090f * fs));
    float env = 0.0f, held = 0.0f, qsb = 1.0f, qsbT = 1.0f, crackle = 0.0f;
    int crackleLeft = 0;
    double flPh = 0.0, flHz = 8.0, whPh = 0.0, whHz = 1300.0, whHzT = 1300.0, humPh = 0.0;
    float whAmp = 0.0f, whAmpT = 0.0f, fryEnv = 0.6f, fryEnvT = 0.6f, dropG = 1.0f, wash = 0.0f, washPeak = 0.0f;
    int washLen = 0, washPos = 0, dropLeft = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const float t = (float)i / fs;
        const bool keyed = i > (size_t)pre && i < speechEnd;
        // --- slow random drifts (every ~80 ms)
        if (i % (size_t)std::max(1, sr / 12) == 0)
        {
            flHz = 5.0 + 8.0 * std::fabs(Rnd());                       // flutter rate 5..13 Hz
            fryEnvT = 0.25f + 0.75f * std::fabs(Rnd());                 // fry density swells
            if (Rnd() > 0.85f)
                whHzT = 900.0 + 800.0 * std::fabs(Rnd());              // the whistle wanders
            if (Rnd() > 0.92f)
                whAmpT = Rnd() > 0.0f ? 1.0f : 0.0f;                    // ... and comes and goes
        }
        fryEnv += (fryEnvT - fryEnv) * 0.002f;
        whHz += (whHzT - whHz) * 0.0002;
        whAmp += (whAmpT - whAmp) * 0.0003f;
        // --- dropouts: the voice dips for a moment, the static stays
        if (keyed && dropLeft == 0 && (double)(Rnd() * 0.5f + 0.5f) < dropRate)
            dropLeft = (int)(fs * (0.02f + 0.06f * std::fabs(Rnd())));
        const float dropT = dropLeft > 0 ? 0.12f : 1.0f;
        if (dropLeft > 0)
            --dropLeft;
        dropG += (dropT - dropG) * 0.01f;
        // --- static washes: a swell of noise that rises and falls, the voice sinks under it
        if (keyed && washLen == 0 && washLvl > 0.0f && (double)(Rnd() * 0.5f + 0.5f) < washRate)
        {
            washLen = (int)(fs * (0.25f + 0.6f * std::fabs(Rnd())));
            washPos = 0;
            washPeak = 0.5f + 0.5f * std::fabs(Rnd());
        }
        wash = 0.0f;
        if (washLen > 0)
        {
            const float w = std::sin(3.14159265f * (float)washPos / (float)washLen);
            wash = washPeak * w * w;
            if (++washPos >= washLen)
                washLen = 0;
        }
        // --- voice
        double v = pk.Run(voice.Run(x[i]));
        const float a = (float)std::fabs(v);
        env = a > env ? atk * env + (1.0f - atk) * a : rel * env + (1.0f - rel) * a;
        const float gain = env > 0.08f ? std::pow(0.08f / env, 0.75f) : 1.0f; // ~4:1 above the knee
        v = std::tanh(drive * v * gain * 1.6) / norm2;
        if (i % (size_t)hold == 0)
            held = (float)v;
        if (i % 2000 == 0)
            qsbT = 0.9f + 0.1f * Rnd(); // slow fading target
        qsb += (qsbT - qsb) * 0.0005f;
        float sOut = held * qsb * dropG * (1.0f - 0.35f * wash);
        // --- static, faded in and out at the ends of the transmission
        {
            const float fin = first ? std::min(1.0f, (float)i / (float)std::max(1, pre)) : 1.0f;
            const float fout = last ? std::min(1.0f, (float)(n - i) / (float)std::max(1, post)) : 1.0f;
            // the static breathes: it rises between words, like a receiver's AGC
            const float key = fin * fout * (1.0f + 1.5f * std::max(0.0f, 1.0f - env / 0.12f));
            // hiss bed, fluttering (AM at 5..13 Hz) like a weak carrier
            flPh += twoPi * flHz / fs;
            const float flutter = 1.0f + 0.45f * (float)std::sin(flPh) * (0.5f + 0.5f * Rnd());
            sOut += stat * key * flutter * (1.0f + 0.6f * (1.0f - dropG)) * (float)hiss.Run(Rnd());
            // frying: dense tiny ticks
            float tick = 0.0f;
            if ((double)(Rnd() * 0.5f + 0.5f) < 400.0 * fryEnv / fs)
                tick = Rnd() * (0.4f + 0.6f * std::fabs(Rnd())) * 6.0f;
            sOut += fryLvl * key * (float)fryF.Run(tick);
            // washes
            sOut += washLvl * key * wash * (float)washF.Run(Rnd());
            // heterodyne whistle, faint and drifting
            whPh += twoPi * whHz / fs;
            sOut += whistleLvl * key * whAmp * (float)std::sin(whPh);
            // alternator buzz
            humPh += twoPi * 117.0 / fs;
            sOut += humLvl * key * (float)humF.Run(std::tanh(5.0 * std::sin(humPh)));
        }
        // --- crackle: short filtered bursts while keyed
        if (keyed && crackleLeft == 0 && (double)(Rnd() * 0.5f + 0.5f) < popRate)
        {
            crackleLeft = (int)(fs * (0.002f + 0.004f * std::fabs(Rnd())));
            crackle = 0.10f + 0.12f * std::fabs(Rnd());
        }
        if (crackleLeft > 0)
        {
            sOut += crackle * (float)crk.Run(Rnd());
            crackle *= 0.97f;
            --crackleLeft;
        }
        (void)t;
        // --- the radio's band on the whole mix, then the peak limiter
        sOut = (float)fin2.Run(fin1.Run(sOut));
        if (g_rc.Limit < 1.0f)
        {
            const float pa = std::fabs(sOut);
            lim = pa > lim ? pa : limRel * lim + (1.0f - limRel) * pa;
            if (lim > g_rc.Limit)
                sOut *= g_rc.Limit / lim;
            sOut *= 0.675f / g_rc.Limit; // make-up gain (1.5x at the default 0.45)
        }
        // the cues: at the very start / end of the transmission
        if (i < cueIn.size())
            sOut += cueIn[i];
        if (i >= n - cueOut.size())
            sOut += cueOut[i - (n - cueOut.size())];
        out[i] = (int16_t)std::max(-32767.0f, std::min(32767.0f, sOut * 0.8f * g_rc.Volume * 32767.0f));
    }
    out.insert(out.end(), gapAfter, (int16_t)0);
    return out;
}

// Plays and waits (the worker thread's job).
void PlayPcm(const std::vector<int16_t> &pcm, int sr)
{
    if (pcm.empty())
        return;
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = (DWORD)sr;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = (DWORD)sr * 2;
    HWAVEOUT wo = nullptr;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return;
    WAVEHDR h = {};
    h.lpData = (LPSTR)pcm.data();
    h.dwBufferLength = (DWORD)(pcm.size() * 2);
    if (waveOutPrepareHeader(wo, &h, sizeof(h)) == MMSYSERR_NOERROR)
    {
        if (waveOutWrite(wo, &h, sizeof(h)) == MMSYSERR_NOERROR)
            while (!(h.dwFlags & WHDR_DONE))
                Sleep(10);
        waveOutUnprepareHeader(wo, &h, sizeof(h));
    }
    waveOutClose(wo);
}

// ---- the speech worker ----
std::mutex              g_spkMx;
std::condition_variable g_spkCv;
// One line to speak - one transmission. Shared by the synthesis and the playback threads; STEP-ON
// (build 2026-10-05o) marks it dropped when an urgent line breaks in on it.
struct SpeakJob
{
    int                       Slot = 0;
    std::wstring              Text;
    uint16_t                  Type = 0;
    ULONGLONG                 Ms = 0;
    bool                      Urgent = false;
    int                       Lead = 0;
    std::wstring              Voice;              // a voice instead of the slot's (GlobalSettings' TEST)
    std::vector<std::wstring> Parts;              // its sentences (set before its first part is queued)
    std::atomic<bool>         Dropped{false};     // stepped on / stale: play no more of it
    std::atomic<bool>         UrgentOpen{false};  // counted in g_urgentLive
    ULONGLONG                 SynthMs = 0;        // making its first sentence took this long (not waiting - 2026-10-06e)
    ULONGLONG                 PlayMs = 0;         // when it went on the air
};
typedef std::shared_ptr<SpeakJob> JobPtr;
std::deque<JobPtr>      g_spkQ;
bool                    g_spkBusy = false;  // (g_spkMx) a line is being synthesised
std::atomic<int>        g_urgentLive{0};    // urgent lines handed over and not yet played out
std::atomic<bool>       g_keyed{false};     // a transmission is on the air
std::atomic<int>        g_airSlot{-1};      // whose (2026-10-06g)
int RadioAirSlot() { return g_keyed ? (int)g_airSlot : -1; }
std::atomic<bool>       g_cutReq{false};    // you spoke: cut off whoever is talking (build 2026-10-06a)
std::atomic<int>        g_cutNoResume{0};   // ... unless it is the one answering you, it picks up again after
std::mutex              g_playMx;
std::condition_variable g_playCv;
// One rendered part of a transmission. A line is one transmission; each sentence is a part, and
// the static keeps running between parts (build 2026-10-05e) until the part marked Last.
struct PlayItem
{
    std::vector<int16_t> Pcm; // empty = an end marker (nothing to play)
    int                  Sr = 0;
    bool                 Last = true;
    JobPtr               Job;
    int                  Part = 0;
};
std::deque<PlayItem> g_playQ;           // rendered transmissions, in order (urgent ones first)
bool                 g_playBusy = false; // (g_playMx) an item taken and not yet played out
bool                    g_spkStarted = false;
#ifdef VC_HAVE_TTS
const SherpaOnnxOfflineTts *g_kokoro = nullptr;
#endif
volatile bool g_kokoroReady = false;

bool RadioActive() { return g_spkStarted && (g_rc.Radio || (g_rc.Tts == 1 && g_kokoroReady)); }
bool RadioBusy()
{
    {
        std::lock_guard<std::mutex> lk(g_spkMx);
        if (g_spkBusy || !g_spkQ.empty())
            return true;
    }
    std::lock_guard<std::mutex> lk(g_playMx);
    return g_playBusy || !g_playQ.empty();
}
bool RadioKeyed() { return g_keyed; }
void UrgentDone(const JobPtr &j); // below

// CORRECTIONS: a lancemate's replies (type 0) still waiting or being made are dropped - its answer to an order
// you just corrected. The one on the air is cut by RadioCut.
void RadioDropReplies(int slot)
{
    if (!g_spkStarted)
        return;
    std::vector<JobPtr> gone;
    {
        std::lock_guard<std::mutex> lk(g_spkMx);
        for (auto it = g_spkQ.begin(); it != g_spkQ.end();)
            if ((*it)->Slot == slot && (*it)->Type == 0)
            {
                gone.push_back(*it);
                it = g_spkQ.erase(it);
            }
            else
                ++it;
    }
    for (auto &j : gone)
        UrgentDone(j);
    std::lock_guard<std::mutex> lk(g_playMx);
    for (auto &it : g_playQ)
        if (it.Job && it.Job->Slot == slot && it.Job->Type == 0)
            it.Job->Dropped = true;
}

void RadioCut(int noResumeSlot)
{
    if (!g_spkStarted)
        return;
    g_cutNoResume = noResumeSlot;
    g_cutReq = true;
}
bool RadioUrgentOnAir() { return g_urgentLive > 0; }
void UrgentDone(const JobPtr &j)
{
    if (j && j->UrgentOpen.exchange(false))
        --g_urgentLive;
}

void RadioSpeak(const SayJob &s)
{
    JobPtr j = std::make_shared<SpeakJob>();
    j->Slot = s.Slot;
    j->Text = s.Text;
    j->Type = s.Type;
    j->Ms = s.Ms;
    j->Urgent = s.Urgent;
    j->Lead = s.Lead;
    j->Voice = s.Voice;
    {
        std::lock_guard<std::mutex> lk(g_spkMx);
        if (j->Urgent)
        {
            j->UrgentOpen = true;
            ++g_urgentLive;
            auto at = g_spkQ.begin(); // ahead of every lesser line
            while (at != g_spkQ.end() && (*at)->Urgent)
                ++at;
            g_spkQ.insert(at, j);
        }
        else
            g_spkQ.push_back(j);
    }
    g_spkCv.notify_one();
}

std::string Utf8(const std::wstring &w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// The Windows voice of this lance number, rendered into memory (for the radio filter).
bool SapiRender(ISpVoice *v, int slot, const std::wstring &text, std::vector<float> &out, int &sr)
{
    sr = 22050;
    ISpStream *ss = nullptr;
    IStream *mem = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) ||
        FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, (void **)&ss)))
    {
        if (mem)
            mem->Release();
        return false;
    }
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = (DWORD)sr;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = (DWORD)sr * 2;
    bool ok = false;
    if (SUCCEEDED(ss->SetBaseStream(mem, SPDFID_WaveFormatEx, &wf)) && SUCCEEDED(v->SetOutput(ss, TRUE)))
    {
        const int s = (slot < 0 || slot > 8) ? 0 : slot;
        if (g_slotVoice[s])
            v->SetVoice(g_slotVoice[s]);
        if (SUCCEEDED(v->Speak(text.c_str(), SPF_DEFAULT, nullptr)))
        {
            HGLOBAL hg = nullptr;
            STATSTG st = {};
            if (SUCCEEDED(GetHGlobalFromStream(mem, &hg)) && SUCCEEDED(mem->Stat(&st, STATFLAG_NONAME)))
            {
                const int16_t *p = (const int16_t *)GlobalLock(hg);
                const size_t n = (size_t)(st.cbSize.QuadPart / 2);
                if (p)
                {
                    out.resize(n);
                    for (size_t i = 0; i < n; ++i)
                        out[i] = p[i] / 32768.0f;
                    GlobalUnlock(hg);
                    ok = n > 0;
                }
            }
        }
        v->SetOutput(nullptr, TRUE);
    }
    ss->Release();
    mem->Release();
    return ok;
}

// Hands a rendered part to the playback thread (an urgent line's parts ahead of the lesser ones).
void PushPlay(PlayItem &&it)
{
    {
        std::lock_guard<std::mutex> lk(g_playMx);
        if (it.Job && it.Job->Urgent)
        {
            auto at = g_playQ.begin();
            while (at != g_playQ.end() && at->Job && at->Job->Urgent)
                ++at;
            g_playQ.insert(at, std::move(it));
        }
        else
            g_playQ.push_back(std::move(it));
    }
    g_playCv.notify_one();
}

JobPtr TakeUrgent()
{
    std::lock_guard<std::mutex> lk(g_spkMx);
    if (g_spkQ.empty() || !g_spkQ.front()->Urgent)
        return nullptr;
    JobPtr j = g_spkQ.front();
    g_spkQ.pop_front();
    return j;
}

// Synthesises one line sentence by sentence: the first sentence starts playing while the rest are
// made, so a long answer ("Two here. Armour's good. ...") starts much sooner. The sentences of one
// line are one transmission: the start cue on the first, the end cue and the gap after it on the
// last. STEP-ON: an urgent line waiting is made first, between two sentences of a lesser one.
void RenderJob(const JobPtr &job, ISpVoice *sv, bool canYield)
{
    if (!job->Urgent && GetTickCount64() - job->Ms > StaleMs(job->Type))
    {
        job->Dropped = true;
        Log(L"voice: dropped, no longer worth saying: %ls", job->Text.c_str());
        return;
    }
    job->Parts = SplitSentences(job->Text);
    const std::vector<std::wstring> &parts = job->Parts;
    for (size_t pi = 0; pi < parts.size(); ++pi)
    {
        if (canYield && pi > 0 && !job->Urgent) // 2026-10-07c: urgent never steps on urgent (a "lance, status" answer stays whole)
            while (JobPtr u = TakeUrgent())
                RenderJob(u, sv, false);
        if (job->Dropped)
        {
            // bug sweep 13 (2026-10-18): a dropped urgent line ("belay that" while it was being made) never queues its
            // last sentence, and only the last sentence released the urgent count - urgent callouts could then never
            // break in again. Release it here (UrgentDone is once-only).
            if (job->Urgent)
                UrgentDone(job);
            return; // stepped on: what is left is the resume's business
        }
        std::vector<float> pcm;
        int sr = 0;
        bool have = false;
        const ULONGLONG t0 = GetTickCount64();
#ifdef VC_HAVE_TTS
        if (g_kokoroReady && g_rc.Tts == 1)
        {
            SherpaOnnxGenerationConfig gc;
            std::memset(&gc, 0, sizeof(gc));
            int sid = job->Voice.empty() ? -1 : KokoroSid(job->Voice); // GlobalSettings' TEST
            if (sid < 0)
                sid = KokoroVoiceFor(job->Slot);
            gc.sid = std::max(0, sid);
            // 2026-10-07h: each voice its own speed (kokoro_speed_<voice>, GlobalSettings' VOICES), else kokoro_speed
            float spd = g_rc.KokoroSpeed;
            if (gc.sid >= 0 && gc.sid < (int)(sizeof(kKokoroNames) / sizeof(kKokoroNames[0])))
            {
                const std::wstring vs = IniStr((std::wstring(L"kokoro_speed_") + kKokoroNames[gc.sid]).c_str(), L"");
                const float f = vs.empty() ? 0.0f : (float)_wtof(vs.c_str());
                if (f >= 0.5f && f <= 2.0f)
                    spd = f;
            }
            gc.speed = spd * (parts[pi].find(L'!') != std::wstring::npos ? 1.08f : 1.0f);
            gc.silence_scale = 0.2f;
            const std::string t = Utf8(KokoroText(parts[pi]));
            const SherpaOnnxGeneratedAudio *ga = SherpaOnnxOfflineTtsGenerateWithConfig(g_kokoro, t.c_str(), &gc, nullptr, nullptr);
            if (ga && ga->n > 0)
            {
                pcm.assign(ga->samples, ga->samples + ga->n);
                sr = ga->sample_rate;
                have = true;
            }
            if (ga)
                SherpaOnnxDestroyOfflineTtsGeneratedAudio(ga);
        }
#endif
        if (!have && sv)
            have = SapiRender(sv, job->Slot, parts[pi], pcm, sr);
        if (!have)
        {
            if (pi + 1 == parts.size()) // still close the transmission (stops the static)
            {
                PlayItem end;
                end.Job = job;
                end.Part = (int)pi;
                PushPlay(std::move(end));
            }
            continue;
        }
        if (pi == 0)
        {
            job->SynthMs = GetTickCount64() - t0;
            Log(L"voice: first sentence ready in %.2f s", job->SynthMs / 1000.0);
        }
        // The engines pad with silence at both ends: trimmed (a little more kept between sentences).
        size_t a = 0, b = pcm.size();
        while (a < b && std::fabs(pcm[a]) < 0.01f)
            ++a;
        while (b > a && std::fabs(pcm[b - 1]) < 0.01f)
            --b;
        const size_t keep = (size_t)sr * 3 / 100, between = (size_t)sr * 12 / 100;
        a = a > keep ? a - keep : 0;
        b = std::min(pcm.size(), b + keep);
        std::vector<float> t(pcm.begin() + (long)a, pcm.begin() + (long)b);
        if (pi + 1 < parts.size())
            t.insert(t.end(), between, 0.0f);
        PlayItem it;
        it.Pcm = RadioProcess(t, sr, pi == 0, pi + 1 == parts.size());
        it.Sr = sr;
        it.Last = pi + 1 == parts.size();
        it.Job = job;
        it.Part = (int)pi;
        PushPlay(std::move(it));
    }
}

// Kokoro loaded (2026-10-10: its own function - the speech worker and VoiceCommand --roundtrip use it).
void KokoroLoad()
{
#ifdef VC_HAVE_TTS
    if (g_rc.Tts == 1)
    {
        const std::wstring d = g_rc.KokoroDir + L"\\";
        const std::string model = Utf8(d + L"model.onnx"), voices = Utf8(d + L"voices.bin"), tokens = Utf8(d + L"tokens.txt"),
                          data = Utf8(d + L"espeak-ng-data"), lex = Utf8(d + L"lexicon-us-en.txt");
        if (GetFileAttributesW((d + L"model.onnx").c_str()) == INVALID_FILE_ATTRIBUTES)
            Log(L"voices: Kokoro model not found in %ls (kokoro_dir) - the Windows voices speak", g_rc.KokoroDir.c_str());
        else
        {
            SherpaOnnxOfflineTtsConfig c;
            std::memset(&c, 0, sizeof(c));
            c.model.kokoro.model = model.c_str();
            c.model.kokoro.voices = voices.c_str();
            c.model.kokoro.tokens = tokens.c_str();
            c.model.kokoro.data_dir = data.c_str();
            c.model.kokoro.lexicon = lex.c_str();
            c.model.kokoro.length_scale = 1.0f;
            c.model.num_threads = g_rc.KokoroThreads;
            c.model.provider = "cpu";
            c.max_num_sentences = 1;
            const ULONGLONG t0 = GetTickCount64();
            g_kokoro = SherpaOnnxCreateOfflineTts(&c);
            if (g_kokoro)
            {
                g_kokoroReady = true;
                Log(L"voices: Kokoro loaded in %.1f s (%d threads) - the pilots speak with Kokoro%ls", (GetTickCount64() - t0) / 1000.0,
                    g_rc.KokoroThreads, g_rc.Radio ? L", over the radio" : L"");
            }
            else
                Log(L"voices: Kokoro failed to load from %ls - the Windows voices speak", g_rc.KokoroDir.c_str());
        }
    }
#else
    if (g_rc.Tts == 1)
        Log(L"voices: Kokoro not built in (sherpa-onnx was not found when building) - the Windows voices speak");
#endif
}

void SpeechWorker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ISpVoice *sv = nullptr; // this thread's own Windows voice (rendering into memory)
    CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, (void **)&sv);
    if (sv)
        sv->SetRate(g_cfg.Rate);
    KokoroLoad();
    for (;;)
    {
        JobPtr job;
        {
            std::unique_lock<std::mutex> lk(g_spkMx);
            g_spkCv.wait(lk, [] { return !g_spkQ.empty(); });
            job = g_spkQ.front();
            g_spkQ.pop_front();
            g_spkBusy = true;
        }
        RenderJob(job, sv, true);
        {
            std::lock_guard<std::mutex> lk(g_spkMx);
            g_spkBusy = false;
        }
    }
}

// Plays the rendered transmissions one after another (the synthesis thread is already on the next).
// One waveOut stream per transmission: the parts are queued back to back with no gap, and while
// the next sentence is still being synthesised the radio keeps hissing (static-only filler) - the
// channel stays keyed until the last part has played.
struct PlayBuf
{
    WAVEHDR              H;
    std::vector<int16_t> Pcm;
    uint64_t             Start = 0; // sample offset in the stream
    JobPtr               Job;
    int                  Part = 0;
    bool                 Fill = false; // static between parts (after Part)
};

// Static only, n samples (the same chain with no voice: it breathes up, as between words).
std::vector<int16_t> RadioFill(int sr, size_t n)
{
    const size_t warm = (size_t)sr / 50; // 20 ms for the filters to settle, dropped
    std::vector<int16_t> o = RadioProcess(std::vector<float>(n + warm, 0.0f), sr, false, false);
    o.erase(o.begin(), o.begin() + (long)std::min(warm, o.size()));
    return o;
}

// STEP-ON (build 2026-10-05o): the urgent transmission starts over the one on the air - that one
// carries on ducked for half a second under a burst of static (two transmitters keyed at once),
// then is gone. What it had not said yet is left for ResumeTick.
std::vector<int16_t> StepOnMix(std::vector<int16_t> &&urgent, const std::vector<int16_t> &old, int sr)
{
    const size_t ov = (size_t)sr / 2, ramp = (size_t)sr / 25;
    if (urgent.size() < ov)
        urgent.resize(ov, 0);
    static uint32_t rng = 0x1234567u;
    const float dt = 1.0f / (float)sr, rcH = 1.0f / (6.2831853f * 350.0f), rcL = 1.0f / (6.2831853f * 3400.0f);
    const float aH = rcH / (rcH + dt), aL = dt / (rcL + dt);
    float hp = 0.0f, lp = 0.0f, px = 0.0f;
    for (size_t i = 0; i < ov; ++i)
    {
        const float duck = i < ramp ? 1.0f - 0.65f * (float)i / (float)ramp : 0.35f;
        const float fade = i + ramp > ov ? (float)(ov - i) / (float)ramp : 1.0f;
        const float o = i < old.size() ? (float)old[i] * duck * fade : 0.0f;
        rng = rng * 1664525u + 1013904223u;
        const float x = (float)(rng >> 8) / 8388608.0f - 1.0f;
        hp = aH * (hp + x - px);
        px = x;
        lp += aL * (hp - lp);
        const float env = std::pow(std::sin(3.14159265f * (float)i / (float)ov), 0.7f);
        const float v = (float)urgent[i] + o + lp * env * 0.4f * 32767.0f;
        urgent[i] = (int16_t)std::max(-32767.0f, std::min(32767.0f, v));
    }
    return std::move(urgent);
}

void PlaybackWorker()
{
    HWAVEOUT wo = nullptr;
    int woSr = 0;
    uint64_t submitted = 0; // samples written since the stream opened
    std::deque<std::unique_ptr<PlayBuf>> fl; // buffers in flight, oldest first
    JobPtr cur;       // the transmission on the air
    int curPart = -1; // its last part handed to waveOut
    auto reap = [&](bool wait) {
        while (!fl.empty())
        {
            PlayBuf *b = fl.front().get();
            if (!(b->H.dwFlags & WHDR_DONE))
            {
                if (!wait)
                    return;
                Sleep(5);
                continue;
            }
            waveOutUnprepareHeader(wo, &b->H, sizeof(b->H));
            fl.pop_front();
        }
    };
    auto submit = [&](std::vector<int16_t> &&pcm, const JobPtr &job, int part, bool fill) {
        std::unique_ptr<PlayBuf> b(new PlayBuf());
        b->Pcm = std::move(pcm);
        b->Start = submitted;
        b->Job = job;
        b->Part = part;
        b->Fill = fill;
        std::memset(&b->H, 0, sizeof(b->H));
        b->H.lpData = (LPSTR)b->Pcm.data();
        b->H.dwBufferLength = (DWORD)(b->Pcm.size() * 2);
        if (!b->Pcm.empty() && waveOutPrepareHeader(wo, &b->H, sizeof(b->H)) == MMSYSERR_NOERROR)
        {
            if (waveOutWrite(wo, &b->H, sizeof(b->H)) == MMSYSERR_NOERROR)
            {
                submitted += b->Pcm.size();
                fl.push_back(std::move(b));
                return;
            }
            waveOutUnprepareHeader(wo, &b->H, sizeof(b->H));
        }
    };
    auto closeStream = [&]() {
        if (wo)
        {
            reap(true);
            waveOutClose(wo);
        }
        wo = nullptr;
        woSr = 0;
        g_keyed = false;
        g_airSlot = -1;
        ViSpeakingOff(); // VOICE INTENT (2026-10-19)
        UrgentDone(cur);
        cur.reset();
        curPart = -1;
    };
    auto playedNow = [&]() -> uint64_t {
        MMTIME mt = {};
        mt.wType = TIME_SAMPLES;
        waveOutGetPosition(wo, &mt, sizeof(mt));
        return mt.wType == TIME_SAMPLES ? mt.u.sample : 0;
    };
    // Where the transmission on the air is cut now: the sentence to pick up from, whether it was cut
    // mid-sentence, and up to maxOld samples of what was still to come. Stops the stream (waveOutReset).
    auto cutPoint = [&](int &from, bool &mid, std::vector<int16_t> &old, size_t maxOld) {
        const int sr = woSr;
        const uint64_t at = playedNow();
        from = curPart + 1;
        mid = false;
        old.clear();
        for (auto &bp : fl)
        {
            const PlayBuf &b = *bp;
            const uint64_t s0 = b.Start, e0 = b.Start + b.Pcm.size();
            if (e0 <= at)
                continue;
            if (s0 <= at && b.Job == cur)
            {
                if (b.Fill)
                    from = b.Part + 1;
                else if (e0 - at > (uint64_t)sr * 6 / 10)
                {
                    from = b.Part;
                    mid = true;
                }
                else
                    from = b.Part + 1;
            }
            for (uint64_t i = std::max(s0, at); i < e0 && old.size() < maxOld; ++i)
                old.push_back(b.Pcm[(size_t)(i - s0)]);
        }
        waveOutReset(wo); // everything queued comes back at once
        reap(true);
        submitted = 0;    // (the position starts again from 0)
        cur->Dropped = true;
        from = std::max(from, cur->Lead); // a resume's own "continuing" is not said again
    };
    auto recordResume = [&](int from, bool mid) {
        if (from >= (int)cur->Parts.size())
            return;
        std::lock_guard<std::mutex> lk(g_resumeMx);
        g_resume.Have = true;
        g_resume.Slot = cur->Slot;
        g_resume.Type = cur->Type;
        // the resume's age: what it waited before it went on the air, then the time since the cut
        {
            const ULONGLONG nw = GetTickCount64(), played = cur->PlayMs ? cur->PlayMs : nw;
            const ULONGLONG waited = played > cur->Ms + cur->SynthMs ? played - cur->Ms - cur->SynthMs : 0;
            g_resume.Ms = nw - waited;
        }
        g_resume.Mid = mid;
        g_resume.Rest.assign(cur->Parts.begin() + from, cur->Parts.end());
    };
    // You spoke over it: the one on the air stops (a 40 ms fade, no burst - you hear yourself, not it).
    auto doCut = [&]() {
        g_cutReq = false;
        if (!wo || !cur)
            return;
        int from = 0;
        bool mid = false;
        std::vector<int16_t> tail;
        cutPoint(from, mid, tail, (size_t)woSr * 4 / 100);
        for (size_t i = 0; i < tail.size(); ++i)
            tail[i] = (int16_t)((float)tail[i] * (float)(tail.size() - i) / (float)tail.size());
        const bool resume = cur->Slot != g_cutNoResume;
        Log(L"voice: you cut %ls off%ls", Callsign(cur->Slot).c_str(), resume ? L"" : L" (it answers you instead)");
        if (resume)
            recordResume(from, mid);
        if (!tail.empty())
            submit(std::move(tail), cur, curPart, true);
        closeStream();
    };
    // VOICE INTENT (2026-10-19): the pilot on the air as a PEAK WAVEFORM for HUDDisplay's V-LNC - for every
    // 11 ms of audio that has been heard, the lowest and highest sample (radio static included), as an audio
    // editor draws its overview. Driven by the play position, not the clock, so a late wake-up fills in every
    // column it missed. Whose voice: the playing buffer's job, not `cur` (the next one can be queued behind it).
    uint64_t lvlAt = 0; // stream position (samples) up to which columns are written
    auto levelTick = [&]() {
        if (!g_vi || !wo || woSr <= 0)
            return;
        const uint64_t at = playedNow();
        const uint64_t col = (uint64_t)woSr * 11 / 1000;
        if (at < lvlAt || at - lvlAt > col * kViWave) // a new stream (the position starts at 0), or far behind
            lvlAt = at > col * 8 ? at - col * 8 : 0;
        for (int n = 0; n < kViWave && lvlAt + col <= at; ++n, lvlAt += col)
        {
            const uint64_t a = lvlAt, e = lvlAt + col;
            int sl = cur ? cur->Slot : -1;
            int lo = 0, hi = 0;
            for (auto &bp : fl)
            {
                const PlayBuf &b = *bp;
                const uint64_t s0 = b.Start, e0 = b.Start + b.Pcm.size();
                if (e0 <= a || s0 >= e)
                    continue;
                if (b.Job && !b.Fill)
                    sl = b.Job->Slot;
                for (uint64_t i = std::max(s0, a); i < std::min(e0, e); ++i)
                {
                    const int v = b.Pcm[(size_t)(i - s0)];
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
            }
            if (sl < 1 || sl >= kViSlots)
                continue;
            ViSlot &vs = g_vi->Slots[sl];
            if (!vs.Speaking)
            {
                std::memset(vs.WaveLo, 0, sizeof(vs.WaveLo)); // a new transmission: last time's trace goes
                std::memset(vs.WaveHi, 0, sizeof(vs.WaveHi));
            }
            for (int s = 0; s < kViSlots; ++s)
                g_vi->Slots[s].Speaking = s == sl ? 1 : 0;
            vs.WaveLo[vs.WaveHead % kViWave] = (int8_t)std::max(-127, lo * 127 / 32767);
            vs.WaveHi[vs.WaveHead % kViWave] = (int8_t)std::min(127, hi * 127 / 32767);
            vs.WaveHead = (uint8_t)((vs.WaveHead + 1) % kViWave);
        }
    };
    // (under g_playMx) an urgent line is next, and may step on what is on the air
    auto urgentNext = [&]() -> bool {
        if (!cur || cur->Urgent || g_playQ.empty())
            return false;
        const PlayItem &f = g_playQ.front();
        return f.Job && f.Job->Urgent && f.Job != cur && f.Part == 0 && !f.Pcm.empty();
    };
    for (;;)
    {
        PlayItem item;
        bool got = false;
        if (!wo)
        {
            std::unique_lock<std::mutex> lk(g_playMx);
            if (g_playQ.empty())
                g_playBusy = false;
            g_playCv.wait(lk, [] { return !g_playQ.empty(); });
            item = std::move(g_playQ.front());
            g_playQ.pop_front();
            g_playBusy = true;
            g_cutReq = false; // nothing was on the air to cut
            got = true;
        }
        else
        {
            // Keyed, waiting for the next part: keep ~80 ms of static queued behind what is playing.
            const ULONGLONG waitStart = GetTickCount64();
            for (;;)
            {
                if (g_cutReq)
                {
                    doCut();
                    break;
                }
                {
                    std::unique_lock<std::mutex> lk(g_playMx);
                    if (g_playCv.wait_for(lk, std::chrono::milliseconds(10), [] { return !g_playQ.empty(); }))
                    {
                        item = std::move(g_playQ.front());
                        g_playQ.pop_front();
                        got = true;
                        break;
                    }
                }
                reap(false);
                levelTick(); // VOICE INTENT (2026-10-19): the waveform on V-LNC
                if (GetTickCount64() - waitStart > 15000) // the next part never came: unkey
                {
                    closeStream();
                    break;
                }
                if (g_rc.Radio && submitted < playedNow() + (uint64_t)woSr * 8 / 100)
                    submit(RadioFill(woSr, (size_t)woSr * 6 / 100), cur, curPart, true);
            }
            if (!got)
                continue;
        }
        const JobPtr job = item.Job;
        if (job && job->Dropped) // stepped on, or stale
        {
            if (item.Last)
                UrgentDone(job);
            continue;
        }
        // Still worth saying? Only the time it WAITED counts - not the time it took to make (2026-10-06e).
        if (job && !job->Urgent && item.Part == 0 && job != cur &&
            GetTickCount64() - job->Ms > StaleMs(job->Type) + job->SynthMs)
        {
            job->Dropped = true;
            Log(L"voice: dropped, no longer worth saying: %ls", job->Text.c_str());
            continue;
        }
        if (item.Pcm.empty()) // an end marker (the last sentence could not be made)
        {
            if (item.Last)
            {
                if (!cur || job == cur)
                    closeStream();
                UrgentDone(job);
            }
            continue;
        }
        if (wo && woSr != item.Sr)
            closeStream();
        const bool stepOn = wo && cur && job && job->Urgent && item.Part == 0 && job != cur && !cur->Urgent;
        if (stepOn && job->Slot == cur->Slot)
        {
            // The same pilot cuts itself short (2026-10-06g): its line fades out, a beat of open mic, then the
            // urgent one - one voice, so no second-transmitter burst - and the old line is not picked up again.
            int from = 0;
            bool mid = false;
            std::vector<int16_t> tail;
            cutPoint(from, mid, tail, (size_t)woSr * 6 / 100);
            for (size_t i = 0; i < tail.size(); ++i)
                tail[i] = (int16_t)((float)tail[i] * (float)(tail.size() - i) / (float)tail.size());
            Log(L"voice: %ls cuts itself short", Callsign(job->Slot).c_str());
            if (!tail.empty())
                submit(std::move(tail), cur, curPart, true);
            if (g_rc.Radio)
                submit(RadioFill(woSr, (size_t)woSr * 25 / 100), cur, curPart, true);
            submit(std::move(item.Pcm), job, 0, false);
        }
        else if (stepOn)
        {
            // Where the one on the air was cut, and the half second of it still to come.
            const int sr = woSr;
            int from = 0;
            bool mid = false;
            std::vector<int16_t> old;
            cutPoint(from, mid, old, (size_t)sr / 2);
            const int total = (int)cur->Parts.size();
            Log(L"voice: %ls steps on %ls (%ls sentence %d of %d)", Callsign(job->Slot).c_str(), Callsign(cur->Slot).c_str(),
                mid ? L"cut in" : L"after", mid ? from + 1 : from, total);
            recordResume(from, mid);
            submit(StepOnMix(std::move(item.Pcm), old, sr), job, 0, false);
        }
        else
        {
            if (!wo)
            {
                WAVEFORMATEX wf = {};
                wf.wFormatTag = WAVE_FORMAT_PCM;
                wf.nChannels = 1;
                wf.nSamplesPerSec = (DWORD)item.Sr;
                wf.wBitsPerSample = 16;
                wf.nBlockAlign = 2;
                wf.nAvgBytesPerSec = (DWORD)item.Sr * 2;
                if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
                {
                    wo = nullptr;
                    if (item.Last)
                        UrgentDone(job);
                    continue;
                }
                woSr = item.Sr;
                submitted = 0;
                g_keyed = true;
            }
            submit(std::move(item.Pcm), job, item.Part, false);
        }
        cur = job;
        curPart = item.Part;
        if (job)
            g_airSlot = job->Slot;
        if (job && item.Part == 0 && !job->PlayMs)
            job->PlayMs = GetTickCount64();
        if (item.Last)
        {
            // The last part is playing: let it play out - unless an urgent line comes to step on it.
            bool out = false;
            for (;;)
            {
                if (g_cutReq)
                {
                    doCut(); // (closes the stream)
                    break;
                }
                reap(false);
                levelTick(); // VOICE INTENT: a one-sentence callout ("that mech is down") is all last part
                if (fl.empty())
                {
                    out = true;
                    break;
                }
                {
                    std::lock_guard<std::mutex> lk(g_playMx);
                    if (urgentNext())
                        break;
                }
                Sleep(5);
            }
            if (out)
                closeStream();
        }
    }
}

void StartSpeechWorker()
{
    LoadRadioConfig();
    if (!g_cfg.Speak || (!g_rc.Radio && g_rc.Tts == 0))
        return; // the plain Windows voices, spoken directly (SayTick) as before
    g_spkStarted = true;
    std::thread(SpeechWorker).detach();
    std::thread(PlaybackWorker).detach();
}

// ---- FREE SPEECH (stage 3, part 1): Whisper for what the fixed phrases do not cover ---------
// SAPI still listens for the fixed phrases (instant, offline). It also keeps the audio of every
// utterance (SPAO_RETAIN_AUDIO, 16 kHz 16-bit mono); an utterance that matched no phrase comes back
// as a FALSE RECOGNITION with that audio, and goes to whisper.cpp (base.en by default, on the CPU,
// on a worker thread so listening never stops). For now the words are only printed - turning them
// into orders comes with the local AI model.
// VoiceCommand.ini: whisper_model = path to a ggml model ("" = free speech off),
//                   whisper_threads = CPU threads for it (default 4).
#ifdef VC_HAVE_WHISPER

whisper_context         *g_wctx = nullptr;
std::mutex               g_wmx;
std::condition_variable  g_wcv;
struct WJob
{
    std::vector<float> Pcm;     // 16 kHz mono
    ULONGLONG          HeardMs = 0;
    bool               AsCommand = false; // recognizer = whisper: its words become orders
    bool               Ptt = false;       // the push-to-talk key was held
};
std::deque<WJob> g_wq;
bool                     g_wbusy = false;

std::string Narrow(const std::wstring &w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// A crash inside whisper.cpp must not take VoiceCommand with it: its calls run under a structured
// exception guard, and a fault turns free speech off with the fault's code and module logged.
DWORD g_wExcCode = 0;
void *g_wExcAddr = nullptr;
int WhisperExcFilter(EXCEPTION_POINTERS *ep)
{
    g_wExcCode = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    g_wExcAddr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}
std::wstring WhisperExcText()
{
    wchar_t mod[MAX_PATH] = L"?";
    HMODULE hm = nullptr;
    if (g_wExcAddr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCWSTR>(g_wExcAddr), &hm))
        GetModuleFileNameW(hm, mod, MAX_PATH);
    wchar_t b[MAX_PATH + 96];
    swprintf_s(b, L"exception 0x%08lX at %p (%ls, +0x%llX)", (unsigned long)g_wExcCode, g_wExcAddr, mod,
               (unsigned long long)((uintptr_t)g_wExcAddr - (uintptr_t)hm));
    return b;
}
#ifdef _MSC_VER
whisper_context *WhisperInitGuarded(const char *path, whisper_context_params cp, bool &faulted)
{
    faulted = false;
    __try
    {
        return whisper_init_from_file_with_params(path, cp);
    }
    __except (WhisperExcFilter(GetExceptionInformation()))
    {
        faulted = true;
        return nullptr;
    }
}
int WhisperFullGuarded(whisper_context *c, const whisper_full_params *p, const float *pcm, int n, bool &faulted)
{
    faulted = false;
    __try
    {
        return whisper_full(c, *p, pcm, n);
    }
    __except (WhisperExcFilter(GetExceptionInformation()))
    {
        faulted = true;
        return -1;
    }
}
#else
whisper_context *WhisperInitGuarded(const char *path, whisper_context_params cp, bool &faulted)
{
    faulted = false;
    return whisper_init_from_file_with_params(path, cp);
}
int WhisperFullGuarded(whisper_context *c, const whisper_full_params *p, const float *pcm, int n, bool &faulted)
{
    faulted = false;
    return whisper_full(c, *p, pcm, n);
}
#endif

// One transcription of 16 kHz mono audio, with the lance's words as the prompt (2026-10-10: its own
// function - the worker and VoiceCommand --roundtrip use it). noise: only noise was heard.
std::wstring WhisperTranscribe(const std::vector<float> &pcm, bool &noise, bool &faulted)
{
        whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        p.n_threads = g_cfg.WhisperThreads > 0 ? g_cfg.WhisperThreads : 4;
        p.language = "en";
        p.no_timestamps = true;
        p.single_segment = true;
        p.max_tokens = 48; // an order is a few words: no runaway repetition
        p.no_context = true;
        p.print_progress = false;
        p.print_realtime = false;
        p.print_timestamps = false;
        p.print_special = false;
        // The words a MechWarrior lance uses, to steer it away from near-misses.
        // The pilots' names go in front, so Whisper spells them the way the game does.
        std::string prompt;
        for (auto &nm : PilotNameWords())
        {
            std::wstring full;
            {
                std::lock_guard<std::mutex> lk(g_pilotMx);
                full = g_pilot[nm.second].Name;
            }
            prompt += Narrow(full) + ", ";
        }
        {
            // the enemies on the scan by name, so "Bane" is not heard as "beam" (build 2026-10-05m)
            std::lock_guard<std::mutex> lk(g_namesMx);
            for (auto &nm : g_knownNames)
                prompt += Narrow(nm) + ", ";
        }
        prompt += "Where is it? Where's the Atlas? Attack that Bane. What is it? How bad? Where are you? ";
        prompt += "How many left? What are we facing? What's closest? Who's shooting at you? What's your target? Can you see it? "
                  "Is it down? How's your ammo? Heat check. Anyone hurt? Sound off. Any air? Who's closest to it? Say again. ";
        prompt += "I'm taking fire, can anyone see where it's coming from? Who's firing those LRMs? Patch it through. ";
        prompt += "Two, move to Nav Alpha. Lance, hold at Nav Bravo. Fall back to Nav Charlie. Where is Nav Delta? "; // NATO (2026-10-14)
        prompt += "Three, advance to Nav Delta. Lance, push up. Two, advance three hundred north. Three, make a run for it! Two, get out of there! Two, go give Four some support. "; // 2026-10-13
        prompt += "Two, get up on that hill. Foxtrot. Lance, take out the turrets. Three, go for the VTOLs. Nav Echo, Nav Golf, Nav Hotel. "; // 2026-10-14
        prompt += "Lance, take out the elementals. Two, hit the battle armor. "; // 2026-10-17 battle armour
        prompt += "Lance, form on me. Lance, move forward five hundred. Two, status report. Two, attack my target. Three, hold position. Four, indirect fire only. "
                  "All, fall back two hundred metres north. Weapons free. Hold fire. Return fire. Run cool. "
                  "Run hot. Alpha strike. PPCs only. Lasers only. LRMs. SRMs. TAG. NARC. Machine guns. "
                  "Reset fire orders.";
        p.initial_prompt = prompt.c_str();
        // Only as much of the 30 s window as was said (50 encoder frames a second, plus a margin).
        if (g_cfg.WhisperFast)
            p.audio_ctx = std::max(512, std::min(1500, (int)(pcm.size() / 16000.0 * 50.0 * 1.5) + 256));
        std::wstring text;
        faulted = false;
        noise = true;
        const int rc = WhisperFullGuarded(g_wctx, &p, pcm.data(), (int)pcm.size(), faulted);
        if (faulted)
            return L"";
        if (rc == 0)
            for (int i = 0; i < whisper_full_n_segments(g_wctx); ++i)
                text += Widen(whisper_full_get_segment_text(g_wctx, i));
        // Trim, and drop what Whisper says about noise ("[BLANK_AUDIO]", "(wind blowing)").
        size_t a = text.find_first_not_of(L" \t\r\n"), b = text.find_last_not_of(L" \t\r\n");
        text = a == std::wstring::npos ? L"" : text.substr(a, b - a + 1);
        noise = text.empty() || text[0] == L'[' || text[0] == L'(' || text[0] == L'*';
        return text;
}

void WhisperWorker()
{
    for (;;)
    {
        WJob job;
        {
            std::unique_lock<std::mutex> lk(g_wmx);
            g_wcv.wait(lk, [] { return !g_wq.empty(); });
            job = std::move(g_wq.front());
            g_wq.pop_front();
            g_wbusy = true;
        }
        const ULONGLONG t0 = GetTickCount64();
        bool faulted = false, noise = true;
        const std::wstring text = WhisperTranscribe(job.Pcm, noise, faulted);
        if (faulted)
        {
            Log(L"free speech: Whisper crashed transcribing (%ls) - free speech off until VoiceCommand restarts",
                WhisperExcText().c_str());
            std::lock_guard<std::mutex> lk(g_wmx);
            g_wbusy = false;
            g_wctx = nullptr; // (its state is not to be trusted: left, not freed)
            return;
        }
        const double took = (GetTickCount64() - t0) / 1000.0;
        if (job.AsCommand && !noise)
            HandleSpoken(text, job.Ptt, took);
        else
            Log(L"%ls \"%ls\" (%.1f s of speech, transcribed in %.2f s)", noise ? L"whisper: only noise -" : L"you said:",
                text.c_str(), job.Pcm.size() / 16000.0, took);
        std::lock_guard<std::mutex> lk(g_wmx);
        g_wbusy = false;
    }
}

void WhisperInit()
{
    if (g_cfg.WhisperModel.empty())
    {
        Log(L"free speech: off (no whisper_model in VoiceCommand.ini)");
        return;
    }
    if (GetFileAttributesW(g_cfg.WhisperModel.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Log(L"free speech: off - model not found: %ls", g_cfg.WhisperModel.c_str());
        return;
    }
    whisper_context_params cp = whisper_context_default_params();
    // 2026-10-14 (user): [voice] whisper_gpu = Whisper on the graphics card (Vulkan) - off by default, the card is the
    // game's. Off (or no Vulkan driver, or a CPU-only build): GGML_DISABLE_VULKAN before ggml's backend registry starts
    // (the first Whisper call), so no Vulkan instance is made and vulkan-1.dll (delay-loaded) is never touched.
    bool gpu = g_cfg.WhisperGpu != 0;
#if defined(VC_HAVE_WHISPER_GPU)
    if (gpu && !LoadLibraryW(L"vulkan-1.dll"))
    {
        Log(L"free speech: whisper_gpu=1 but no Vulkan driver (vulkan-1.dll) - Whisper on the CPU");
        gpu = false;
    }
#else
    if (gpu)
        Log(L"free speech: whisper_gpu=1 but this build has no GPU support (built without the Vulkan SDK) - Whisper on the CPU");
    gpu = false;
#endif
    if (!gpu)
        _putenv_s("GGML_DISABLE_VULKAN", "1");
    cp.use_gpu = gpu;
    Log(L"free speech: Whisper on the %ls", gpu ? L"graphics card (Vulkan)" : L"CPU");
    cp.flash_attn = g_cfg.WhisperFlashAttn != 0;
    const ULONGLONG t0 = GetTickCount64();
    const std::string path = Narrow(g_cfg.WhisperModel);
    bool faulted = false;
    g_wctx = WhisperInitGuarded(path.c_str(), cp, faulted);
    if (faulted)
    {
        Log(L"free speech: off - Whisper crashed loading the model (%ls, flash attention %ls)", WhisperExcText().c_str(),
            cp.flash_attn ? L"on" : L"off");
        g_wctx = nullptr;
        return;
    }
    if (!g_wctx)
    {
        Log(L"free speech: off - the model did not load: %ls", g_cfg.WhisperModel.c_str());
        return;
    }
    Log(L"free speech: on - %ls loaded in %.1f s, %d CPU threads, flash attention %ls", g_cfg.WhisperModel.c_str(),
        (GetTickCount64() - t0) / 1000.0, g_cfg.WhisperThreads, cp.flash_attn ? L"on" : L"off");
    std::thread(WhisperWorker).detach();
}

// The retained audio of a result, as 16 kHz mono floats (16-bit PCM at any rate / channel count).
bool ResultAudio(ISpRecoResult *r, std::vector<float> &out)
{
    ISpStreamFormat *st = nullptr;
    if (FAILED(r->GetAudio(0, 0, &st)) || !st)
        return false;
    GUID fmt = {};
    WAVEFORMATEX *wfx = nullptr;
    bool ok = false;
    STATSTG ss = {};
    if (SUCCEEDED(st->GetFormat(&fmt, &wfx)) && wfx && wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16 &&
        wfx->nChannels >= 1 && wfx->nSamplesPerSec >= 8000 && SUCCEEDED(st->Stat(&ss, STATFLAG_NONAME)))
    {
        const ULONGLONG bytes = ss.cbSize.QuadPart;
        if (bytes > 0 && bytes < 16000ull * 2 * 60 * 4) // under a minute or so
        {
            std::vector<int16_t> pcm((size_t)(bytes / 2));
            LARGE_INTEGER z = {};
            ULONG got = 0;
            st->Seek(z, STREAM_SEEK_SET, nullptr);
            if (SUCCEEDED(st->Read(pcm.data(), (ULONG)(pcm.size() * 2), &got)))
            {
                const size_t ch = wfx->nChannels, frames = got / 2 / ch;
                const double step = wfx->nSamplesPerSec / 16000.0;
                out.clear();
                for (double pos = 0.0; (size_t)pos < frames; pos += step)
                {
                    const size_t i = (size_t)pos;
                    float v = 0.0f;
                    for (size_t c = 0; c < ch; ++c)
                        v += pcm[i * ch + c];
                    out.push_back(v / (32768.0f * ch));
                }
                ok = out.size() > 16000 / 3; // a third of a second at least
            }
        }
    }
    if (wfx)
        CoTaskMemFree(wfx);
    st->Release();
    return ok;
}

bool WhisperReady() { return g_wctx != nullptr; }

void WhisperQueue(ISpRecoResult *r, bool asCommand, bool ptt)
{
    if (!g_wctx)
        return;
    WJob j;
    if (!ResultAudio(r, j.Pcm))
        return;
    j.HeardMs = GetTickCount64();
    j.AsCommand = asCommand;
    j.Ptt = ptt;
    std::lock_guard<std::mutex> lk(g_wmx);
    if (g_wq.size() >= 3)
        g_wq.pop_front(); // keep up: the oldest goes
    g_wq.push_back(std::move(j));
    g_wcv.notify_one();
}
#else
void WhisperInit()
{
    Log(L"free speech: not built in (whisper.cpp was not found when building)");
}
bool WhisperReady() { return false; }
void WhisperQueue(ISpRecoResult *, bool, bool) {}
#endif

// ---- ROUND TRIP (2026-10-10): VoiceCommand.exe --roundtrip [names file] ----------------------------
// No game, no microphone: Kokoro says each unit name inside an order and a question ("Attack the
// Hatamoto-Chi." / "Where's the Hatamoto-Chi?"), with our pronunciations, in a few voices; Whisper
// listens with the same prompt as in a mission; the words go through the same clean-up (HeardAs.txt)
// and parser. Every miss is written to RoundTrip_report.tsv with what Whisper wrote, and a suggested
// fix to HeardAs.suggested.txt - suggestions only: nothing is added to HeardAs.txt by itself.
// The names: RoundTripNames.txt beside the exe (one per line, # comments), or the file given.
// 2026-10-19: outside the round-trip #if - the game test (GtLoad) uses it in every build, so a build
// without whisper.cpp / sherpa-onnx failed to compile.
// The lines of a text file beside the exe (UTF-8, # comments and blank lines left out).
static bool RtReadLines(const std::wstring &path, std::vector<std::wstring> &out)
{
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return false;
    std::string raw;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        raw.append(buf, got);
    fclose(f);
    if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0)
        raw.erase(0, 3);
    // (its own UTF-8 conversion: Widen() exists only in Whisper builds)
    std::wstring w;
    if (!raw.empty())
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
        w.assign((size_t)(n > 0 ? n : 0), L'\0');
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &w[0], n);
    }
    size_t i = 0;
    while (i <= w.size())
    {
        size_t j = w.find(L'\n', i);
        if (j == std::wstring::npos)
            j = w.size();
        std::wstring l = w.substr(i, j - i);
        i = j + 1;
        while (!l.empty() && (l.back() == L'\r' || l.back() == L' ' || l.back() == L'\t'))
            l.pop_back();
        while (!l.empty() && (l[0] == L' ' || l[0] == L'\t'))
            l.erase(0, 1);
        if (!l.empty() && l[0] != L'#')
            out.push_back(l);
    }
    return true;
}
#if defined(VC_HAVE_WHISPER) && defined(VC_HAVE_TTS)
int RoundTrip(const std::wstring &listArg)
{
    SetConsoleTitleW(L"MW5 Voice Command - round trip");
    LoadConfig();
    BuildPhrases();
    BuildOrderList();
    LinesTick(true);
    g_pronCheckMs = 0;
    PronTick();
    g_heardCheckMs = 0;
    HeardAsTick();
    LoadRadioConfig();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (g_rc.Tts != 1)
        Log(L"round trip: GlobalSettings has Windows voices - Kokoro is used for the round trip anyway");
    g_rc.Tts = 1;
    KokoroLoad();
    WhisperInit();
    if (!g_kokoroReady || !g_wctx)
    {
        Log(L"round trip: needs Kokoro and Whisper (see above) - nothing done");
        return 1;
    }
    const std::wstring list = listArg.empty() ? ExeDir() + L"RoundTripNames.txt" : listArg;
    std::vector<std::wstring> names;
    {
        FILE *f = nullptr;
        if (_wfopen_s(&f, list.c_str(), L"rb") != 0 || !f)
        {
            Log(L"round trip: no names file %ls", list.c_str());
            return 1;
        }
        std::string raw;
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            raw.append(buf, got);
        fclose(f);
        if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0)
            raw.erase(0, 3);
        std::wstring w = Widen(raw.c_str());
        size_t i = 0;
        while (i <= w.size())
        {
            size_t j = w.find(L'\n', i);
            if (j == std::wstring::npos)
                j = w.size();
            std::wstring l = w.substr(i, j - i);
            i = j + 1;
            while (!l.empty() && (l.back() == L'\r' || l.back() == L' ' || l.back() == L'\t'))
                l.pop_back();
            while (!l.empty() && (l[0] == L' ' || l[0] == L'\t'))
                l.erase(0, 1);
            if (!l.empty() && l[0] != L'#')
                names.push_back(l);
        }
    }
    static const wchar_t *kVoices[] = {L"am_adam", L"bf_emma", L"af_heart"};
    std::vector<int> sids;
    for (auto *v : kVoices)
        if (KokoroSid(v) >= 0)
            sids.push_back(KokoroSid(v));
    if (sids.empty())
        sids.push_back(0);
    Log(L"round trip: %zu names x 2 sentences x %zu voices - this takes a while", names.size(), sids.size());

    FILE *rep = nullptr;
    _wfopen_s(&rep, (ExeDir() + L"RoundTrip_report.tsv").c_str(), L"wb");
    auto put = [&](const std::wstring &line) {
        if (!rep)
            return;
        const std::string u = Utf8(line + L"\r\n");
        fwrite(u.data(), 1, u.size(), rep);
    };
    if (rep)
        fwrite("\xEF\xBB\xBF", 1, 3, rep);
    put(L"name\tvoice\tsentence\twhisper wrote\tafter clean-up\tunderstood\tsuggested HeardAs line");
    std::map<std::wstring, int> sugg; // "heard = meant" -> how often
    int tests = 0, passed = 0;
    const ULONGLONG t0 = GetTickCount64();
    for (size_t ni = 0; ni < names.size(); ++ni)
    {
        const std::wstring &name = names[ni];
        {
            std::lock_guard<std::mutex> lk(g_namesMx); // as in a mission: this one and a few others on the scan
            g_knownNames = {name};
            for (const wchar_t *d : {L"Atlas", L"Bane", L"Hunchback", L"Jenner"})
                if (!SameUnitName(d, name))
                    g_knownNames.push_back(d);
        }
        for (int kind = 0; kind < 2; ++kind)
        {
            const std::wstring sentence = kind == 0 ? L"Attack the " + name + L"." : L"Where's the " + name + L"?";
            for (int sid : sids)
            {
                SherpaOnnxGenerationConfig gc;
                std::memset(&gc, 0, sizeof(gc));
                gc.sid = sid;
                gc.speed = 1.0f;
                gc.silence_scale = 0.2f;
                const std::string t = Utf8(KokoroText(sentence));
                const SherpaOnnxGeneratedAudio *ga = SherpaOnnxOfflineTtsGenerateWithConfig(g_kokoro, t.c_str(), &gc, nullptr, nullptr);
                std::vector<float> pcm;
                if (ga && ga->n > 0 && ga->sample_rate > 0)
                {
                    const double step = ga->sample_rate / 16000.0; // to Whisper's 16 kHz
                    for (double pos = 0.0; (size_t)pos + 1 < (size_t)ga->n; pos += step)
                    {
                        const size_t i0 = (size_t)pos;
                        const float fr = (float)(pos - i0);
                        pcm.push_back(ga->samples[i0] * (1.0f - fr) + ga->samples[i0 + 1] * fr);
                    }
                    pcm.insert(pcm.begin(), 4000, 0.0f); // a quarter second either side, as the recogniser cuts it
                    pcm.insert(pcm.end(), 4000, 0.0f);
                }
                if (ga)
                    SherpaOnnxDestroyOfflineTtsGeneratedAudio(ga);
                if (pcm.empty())
                    continue;
                bool noise = true, faulted = false;
                const std::wstring heard = WhisperTranscribe(pcm, noise, faulted);
                if (faulted)
                {
                    Log(L"round trip: Whisper crashed - stopping");
                    ni = names.size();
                    break;
                }
                std::vector<std::wstring> w = CollapseRepeats(NormalizeHeard(heard));
                bool ok = false;
                if (kind == 0)
                {
                    std::vector<Cmd> out;
                    std::wstring desc;
                    g_parsedName.clear();
                    if (ParseOrder(w, out, desc))
                        for (auto &c : out)
                            ok = ok || (c.Type == C_AttackNamed && (SameUnitName(g_parsedName, name) || UnitAnswersTo(name, g_parsedName, true)));
                }
                else
                {
                    std::wstring qn;
                    ok = ParseQuestion(w, qn) == Q_WhereNamed && (SameUnitName(qn, name) || UnitAnswersTo(name, qn, true));
                }
                ++tests;
                passed += ok ? 1 : 0;
                // what was heard for the name: the words after "attack the" / "where's the"
                std::wstring fix;
                if (!ok)
                {
                    std::vector<std::wstring> r = w;
                    static const wchar_t *kDrop[] = {L"attack", L"wheres", L"where", L"is", L"the", L"that", L"a"};
                    bool cut = true;
                    while (cut && !r.empty())
                    {
                        cut = false;
                        for (auto *d : kDrop)
                            if (!r.empty() && r.front() == d)
                            {
                                r.erase(r.begin());
                                cut = true;
                            }
                    }
                    const std::wstring h = JoinWords(r), m = JoinWords(SplitWords(name));
                    if (!h.empty() && h != m && r.size() <= 4)
                    {
                        fix = h + L" = " + m;
                        ++sugg[fix];
                    }
                }
                wchar_t vn[16];
                swprintf(vn, 16, L"%d", sid);
                put(name + L"\t" + std::wstring(sid >= 0 && sid < (int)(sizeof(kKokoroNames) / sizeof(kKokoroNames[0])) ? kKokoroNames[sid] : vn) +
                    L"\t" + sentence + L"\t" + heard + L"\t" + JoinWords(w) + L"\t" + (ok ? L"yes" : L"NO") + L"\t" + fix);
            }
        }
        if ((ni + 1) % 10 == 0 || ni + 1 == names.size())
            Log(L"round trip: %zu / %zu names, %d of %d understood (%.0f s)", ni + 1, names.size(), passed, tests,
                (GetTickCount64() - t0) / 1000.0);
    }
    if (rep)
        fclose(rep);
    FILE *sf = nullptr;
    if (_wfopen_s(&sf, (ExeDir() + L"HeardAs.suggested.txt").c_str(), L"wb") == 0 && sf)
    {
        std::wstring o = L"# HeardAs.suggested.txt - written by VoiceCommand --roundtrip: what Whisper wrote for a unit name it did not\r\n"
                         L"# understand, and the name. Copy the lines you agree with into HeardAs.txt (number = how many voices / sentences).\r\n";
        for (auto &kv : sugg)
            o += kv.first + L"   # " + std::to_wstring(kv.second) + L"\r\n";
        const std::string u = Utf8(o);
        fwrite("\xEF\xBB\xBF", 1, 3, sf);
        fwrite(u.data(), 1, u.size(), sf);
        fclose(sf);
    }
    Log(L"round trip: done - %d of %d understood; RoundTrip_report.tsv and HeardAs.suggested.txt (%zu suggestions) beside the exe",
        passed, tests, sugg.size());
    return 0;
}
// ---- ROUND TRIP WITH SENTENCES (2026-10-10): VoiceCommand.exe --roundtrip2 [sentences file] ----------------
// As --roundtrip, but whole sentences the way they are said in a mission (callsigns, other verbs, questions),
// each put through HandleSpoken itself - the same path as the microphone, push-to-talk key held - and judged by
// what it hands the main loop (an order of the right kind / a question of the right kind, the right unit).
// RoundTripSentences.txt beside the exe:  voices = am_x, bf_y ...  /  names = <file>  /  <expect> | <sentence>
// ({X} = the unit name; a sentence without {X} is said once per voice). expect: attack, where, unithow, isdown,
// cansee, patch, whofiring, howmany, howbad, status, move, fallback. Report: RoundTrip2_report.tsv.
struct RtSentence
{
    std::wstring Expect, Text;
};
static bool RtJudge(const std::wstring &ex, const std::vector<SpokenAction> &acts, const std::wstring &name, std::wstring &got)
{
    static const wchar_t *kQn[] = {L"none", L"where", L"where-named", L"where-you", L"what", L"howbad", L"howmany", L"facing",
                                   L"closest", L"whohits", L"target", L"cansee", L"isdown", L"ammo", L"heat", L"hurt",
                                   L"soundoff", L"unithow", L"air", L"whoclosest", L"repeat", L"whofiring", L"patchyes",
                                   L"patchno", L"patch", L"class", L"heading", L"radiocheck",
                                   L"hillpick", L"hillcancel"};
    bool ok = false;
    for (auto &a : acts)
    {
        std::wstring g;
        if (!a.ReplyGroup.empty())
            g = L"reply " + a.ReplyGroup;
        else if (a.SayAgain)
            g = L"say again";
        else if (a.Query > 0)
            g = std::wstring(a.Query < (int)(sizeof(kQn) / sizeof(kQn[0])) ? kQn[a.Query] : L"?") + (a.QueryName.empty() ? L"" : L"(" + a.QueryName + L")");
        else
        {
            static const wchar_t *kCn[] = {L"follow", L"attack my target", L"hold", L"indirect", L"fall back", L"move", L"discipline",
                                           L"heat", L"weapons only", L"hold weapons", L"all weapons", L"reset fire", L"fall back move",
                                           L"status", L"attack nearest", L"fire settings", L"attack named", L"shadow", L"nav",
                                           L"vision", L"called shot", L"support", L"hill",
                                           L"attack class", L"flank", L"pincer"};
            const int ct = (int)a.C.Type;
            g = std::wstring(ct >= 0 && ct < (int)(sizeof(kCn) / sizeof(kCn[0])) ? kCn[ct] : L"order ?") + L" -> " +
                (a.C.Who ? std::to_wstring(a.C.Who) : std::wstring(L"lance")) + (a.QueryName.empty() ? L"" : L" (" + a.QueryName + L")") +
                ((a.C.Type == C_Move || a.C.Type == C_FallMove || a.C.Type == C_CalledShot)
                     ? L" [A=" + std::to_wstring(a.C.A) + L" B=" + std::to_wstring(a.C.B) + L" C=" + std::to_wstring(a.C.C) + L"]"
                     : std::wstring());
        }
        got += (got.empty() ? L"" : L"; ") + g;
        const bool nameOk = name.empty() || SameUnitName(a.QueryName, name) || UnitAnswersTo(name, a.QueryName, true);
        if (ex == L"attack")
            ok |= a.Query == 0 && a.C.Type == C_AttackNamed && nameOk;
        else if (ex == L"where")
            ok |= a.Query == Q_WhereNamed && nameOk;
        else if (ex == L"unithow")
            ok |= a.Query == Q_UnitHow && nameOk;
        else if (ex == L"isdown")
            ok |= a.Query == Q_IsDown && nameOk;
        else if (ex == L"cansee")
            ok |= a.Query == Q_CanSee && nameOk;
        else if (ex == L"patch")
            ok |= a.Query == Q_PatchCmd && nameOk;
        else if (ex == L"whofiring")
            ok |= a.Query == Q_WhoFiring;
        else if (ex == L"howmany")
            ok |= a.Query == Q_HowMany;
        else if (ex == L"howbad")
            ok |= a.Query == Q_HowBad;
        else if (ex == L"status")
            ok |= (a.Query == 0 && a.C.Type == C_Status) || a.Query == Q_HowBad;
        else if (ex == L"move")
            ok |= a.Query == 0 && a.C.Type == C_Move;
        else if (ex == L"fallback")
            ok |= a.Query == 0 && a.C.Type == C_Fallback;
    }
    if (got.empty())
        got = L"nothing";
    return ok;
}
int RoundTrip2(const std::wstring &listArg)
{
    SetConsoleTitleW(L"MW5 Voice Command - round trip (sentences)");
    LoadConfig();
    BuildPhrases();
    BuildOrderList();
    LinesTick(true);
    g_pronCheckMs = 0;
    PronTick();
    g_heardCheckMs = 0;
    HeardAsTick();
    LoadRadioConfig();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (g_rc.Tts != 1)
        Log(L"round trip: GlobalSettings has Windows voices - Kokoro is used for the round trip anyway");
    g_rc.Tts = 1;
    KokoroLoad();
    WhisperInit();
    if (!g_kokoroReady || !g_wctx)
    {
        Log(L"round trip: needs Kokoro and Whisper (see above) - nothing done");
        return 1;
    }
    const std::wstring list = listArg.empty() ? ExeDir() + L"RoundTripSentences.txt" : listArg;
    std::vector<std::wstring> lines;
    if (!RtReadLines(list, lines))
    {
        Log(L"round trip: no sentences file %ls", list.c_str());
        return 1;
    }
    std::vector<int> sids;
    std::vector<std::wstring> unitNames;
    std::vector<RtSentence> sents;
    for (auto &l : lines)
    {
        const size_t eq = l.find(L'='), bar = l.find(L'|');
        auto trim = [](std::wstring t) {
            while (!t.empty() && (t.back() == L' ' || t.back() == L'\t'))
                t.pop_back();
            while (!t.empty() && (t[0] == L' ' || t[0] == L'\t'))
                t.erase(0, 1);
            return t;
        };
        if (bar != std::wstring::npos)
            sents.push_back({trim(l.substr(0, bar)), trim(l.substr(bar + 1))});
        else if (eq != std::wstring::npos && trim(l.substr(0, eq)) == L"voices")
        {
            std::wstring v = l.substr(eq + 1);
            for (auto &c : v)
                if (c == L',' || c == L'\t')
                    c = L' ';
            std::vector<std::wstring> vs;
            for (size_t p0 = 0; p0 < v.size();)
            {
                size_t p1 = v.find(L' ', p0);
                if (p1 == std::wstring::npos)
                    p1 = v.size();
                if (p1 > p0)
                    vs.push_back(v.substr(p0, p1 - p0));
                p0 = p1 + 1;
            }
            for (auto &t : vs)
                if (KokoroSid(t) >= 0)
                    sids.push_back(KokoroSid(t));
                else
                    Log(L"round trip: no Kokoro voice %ls", t.c_str());
        }
        else if (eq != std::wstring::npos && trim(l.substr(0, eq)) == L"names")
        {
            const std::wstring nf = ExeDir() + trim(l.substr(eq + 1));
            if (!RtReadLines(nf, unitNames))
                Log(L"round trip: no names file %ls", nf.c_str());
        }
    }
    if (sids.empty())
        sids.push_back(KokoroSid(L"am_adam"));
    if (sents.empty() || unitNames.empty())
    {
        Log(L"round trip: %ls needs 'voices =', 'names =' and '<expect> | <sentence>' lines - nothing done", list.c_str());
        return 1;
    }
    size_t total = 0;
    for (auto &st : sents)
        total += (st.Text.find(L"{X}") != std::wstring::npos ? unitNames.size() : 1) * sids.size();
    Log(L"round trip: %zu sentences, %zu names, %zu voices = %zu tries - this takes a while", sents.size(), unitNames.size(),
        sids.size(), total);
    FILE *rep = nullptr;
    _wfopen_s(&rep, (ExeDir() + L"RoundTrip2_report.tsv").c_str(), L"wb");
    auto put = [&](const std::wstring &line) {
        if (!rep)
            return;
        const std::string u = Utf8(line + L"\r\n");
        fwrite(u.data(), 1, u.size(), rep);
    };
    if (rep)
        fwrite("\xEF\xBB\xBF", 1, 3, rep);
    put(L"expect\tname\tvoice\tsentence\twhisper wrote\tafter clean-up\tunderstood\twhat it became");
    int tests = 0, passed = 0;
    const ULONGLONG t0 = GetTickCount64();
    for (size_t si = 0; si < sents.size(); ++si)
    {
        const RtSentence &st = sents[si];
        const bool named = st.Text.find(L"{X}") != std::wstring::npos;
        const size_t nn = named ? unitNames.size() : 1;
        for (size_t ni = 0; ni < nn; ++ni)
        {
            const std::wstring name = named ? unitNames[ni] : L"";
            std::wstring sentence = st.Text;
            if (named)
                sentence.replace(sentence.find(L"{X}"), 3, name);
            {
                std::lock_guard<std::mutex> lk(g_namesMx);
                g_knownNames.clear();
                if (named)
                    g_knownNames.push_back(name);
                for (const wchar_t *d : {L"Atlas", L"Bane", L"Hunchback", L"Jenner"})
                    if (!named || !SameUnitName(d, name))
                        g_knownNames.push_back(d);
            }
            for (int sid : sids)
            {
                SherpaOnnxGenerationConfig gc;
                std::memset(&gc, 0, sizeof(gc));
                gc.sid = sid;
                gc.speed = 1.0f;
                gc.silence_scale = 0.2f;
                const std::string t = Utf8(KokoroText(sentence));
                const SherpaOnnxGeneratedAudio *ga = SherpaOnnxOfflineTtsGenerateWithConfig(g_kokoro, t.c_str(), &gc, nullptr, nullptr);
                std::vector<float> pcm;
                if (ga && ga->n > 0 && ga->sample_rate > 0)
                {
                    const double step = ga->sample_rate / 16000.0;
                    for (double pos = 0.0; (size_t)pos + 1 < (size_t)ga->n; pos += step)
                    {
                        const size_t i0 = (size_t)pos;
                        const float fr = (float)(pos - i0);
                        pcm.push_back(ga->samples[i0] * (1.0f - fr) + ga->samples[i0 + 1] * fr);
                    }
                    pcm.insert(pcm.begin(), 4000, 0.0f);
                    pcm.insert(pcm.end(), 4000, 0.0f);
                }
                if (ga)
                    SherpaOnnxDestroyOfflineTtsGeneratedAudio(ga);
                if (pcm.empty())
                    continue;
                bool noise = true, faulted = false;
                const std::wstring heard = WhisperTranscribe(pcm, noise, faulted);
                if (faulted)
                {
                    Log(L"round trip: Whisper crashed - stopping");
                    si = sents.size();
                    ni = nn;
                    break;
                }
                // a fresh start for every sentence: no earlier addressee, answerer, offer or correction target
                g_lastWho.clear();
                g_lastWhoMs = 0;
                g_lastAnswerer = 0;
                g_lastAnswerMs = 0;
                g_offerMs = 0;
                g_lastReporter = 0;
                g_lastReporterMs = 0;
                g_corrWho.clear();
                g_corrWhoMs = 0;
                {
                    std::lock_guard<std::mutex> lk(g_amx);
                    g_actions.clear();
                }
                HandleSpoken(heard, true, 0.0);
                std::vector<SpokenAction> acts;
                {
                    std::lock_guard<std::mutex> lk(g_amx);
                    acts.swap(g_actions);
                }
                std::wstring got;
                const bool ok = RtJudge(st.Expect, acts, name, got);
                ++tests;
                passed += ok ? 1 : 0;
                wchar_t vn[16];
                swprintf(vn, 16, L"%d", sid);
                put(st.Expect + L"\t" + (named ? name : L"-") + L"\t" +
                    std::wstring(sid >= 0 && sid < (int)(sizeof(kKokoroNames) / sizeof(kKokoroNames[0])) ? kKokoroNames[sid] : vn) +
                    L"\t" + sentence + L"\t" + heard + L"\t" + JoinWords(CollapseRepeats(NormalizeHeard(heard))) + L"\t" +
                    (ok ? L"yes" : L"NO") + L"\t" + got);
            }
        }
        Log(L"round trip: sentence %zu / %zu (%ls), %d of %d understood (%.0f s)", si + 1, sents.size(), st.Text.c_str(), passed,
            tests, (GetTickCount64() - t0) / 1000.0);
    }
    if (rep)
        fclose(rep);
    Log(L"round trip: done - %d of %d understood; RoundTrip2_report.tsv beside the exe", passed, tests);
    return 0;
}
// ---- ROUND TRIP LOOP (2026-10-10, user): VoiceCommand.exe --roundtrip3 [minutes] (RoundTrip3.bat) ---------------------
// For an hour (or the minutes given, or until RoundTrip3.stop appears beside the exe): a random voice (any of the 28
// English Kokoro voices) says a random sentence from RoundTripSentences.txt with a random name from RoundTripNames.txt;
// it goes through HandleSpoken and is judged as in --roundtrip2. A miss whose sentence was heard right around the name:
//  - the heard name has a word that is not a common English word (CommonWords.txt) -> "heard = name" is ADDED to
//    HeardAs.txt at once (used from the next try on) and listed in HeardAs.auto.txt;
//  - only common words (or a clash with an existing HeardAs line) -> HeardAs.undecided.txt with a count, for the user;
//  - the rest of the sentence misheard, or the name heard right but not understood -> HeardAs.undecided.txt too (frame /
//    parser). Heard names listed in HeardAs.ignore.txt (e.g. alpha) are not recorded at all.
// Every try -> RoundTrip3_log.tsv (appended).
static std::wstring RtLower(std::wstring t)
{
    for (auto &c : t)
        c = (wchar_t)towlower(c);
    return t;
}
// A fixed command phrase as said: "two move three hundred metres north" -> "Two, move three hundred metres north."
static std::wstring RtSpoken(const std::wstring &phrase)
{
    std::wstring o = phrase;
    const size_t sp = o.find(L' ');
    if (!o.empty())
        o[0] = (wchar_t)towupper(o[0]);
    if (sp != std::wstring::npos)
        o.insert(sp, L",");
    return o + L".";
}
// The order that phrase stands for, for the same lancemate (fire settings may come back merged as C_FireSet).
static bool RtJudgeCmd(const Cmd &want, const std::vector<SpokenAction> &acts)
{
    for (auto &a : acts)
    {
        if (a.Query || a.SayAgain || a.C.Who != want.Who)
            continue;
        if (SameOrder(a.C, want))
            return true;
        const bool fire = want.Type == C_Disc || want.Type == C_Heat || want.Type == C_CatOnly || want.Type == C_CatHold ||
                          want.Type == C_AllWeapons;
        if (fire && a.C.Type == C_FireSet)
            return true;
    }
    return false;
}
// ---- SENTENCE GENERATOR (2026-10-11, user): the round-trip loop's lines built from parts - any callsign, number,
// direction, bearing, part, name, phrasing - instead of fixed templates. Every category equally likely. A unit name
// is left as {X} (the loop fills it and learns its mishearings). Judged "same": understood as the typed line is.
static const wchar_t *RtPick(std::mt19937 &r, std::initializer_list<const wchar_t *> l)
{
    return *(l.begin() + (size_t)(r() % l.size()));
}
static std::wstring RtCallsign(std::mt19937 &r, bool allowNone)
{
    static const wchar_t *k[] = {L"Two, ", L"Three, ", L"Four, ", L"One, ", L"Lance, ", L"Everyone, ", L"Two and Three, ",
                                 L"Guys, ", L"All units, ", L""};
    const size_t n = sizeof(k) / sizeof(k[0]) - (allowNone ? 0 : 1);
    return k[r() % n];
}
static std::wstring RtDistance(std::mt19937 &r)
{
    const int kind = (int)(r() % 6);
    wchar_t b[48];
    if (kind == 0) // klicks
    {
        const int d = 1 + (int)(r() % 2), t = (int)(r() % 10);
        if (t && r() % 2)
            swprintf_s(b, L"%d.%d k", d, t);
        else
            swprintf_s(b, L"%d %ls", d, RtPick(r, {L"klicks", L"kilometres", L"k"}));
        return b;
    }
    const int m = 25 + (int)(r() % 196) * 10; // 25 .. 1975
    swprintf_s(b, kind == 1 ? L"%dm" : kind <= 3 ? L"%d metres" : L"%d", m);
    return b;
}
static std::wstring RtCap(std::wstring s)
{
    if (!s.empty())
        s[0] = (wchar_t)towupper(s[0]);
    return s;
}
// One generated line; cat = which category it came from (for the log).
static std::wstring RtGenerate(std::mt19937 &r, std::wstring &cat)
{
    const int c = (int)(r() % 10);
    std::wstring cs = RtCallsign(r, false);
    auto dir = [&]() -> std::wstring {
        return RtPick(r, {L"north", L"south", L"east", L"west", L"north east", L"north west", L"south east", L"south west",
                          L"forward", L"back", L"left", L"right"});
    };
    auto digits3 = [&](int v) {
        wchar_t b[16];
        swprintf_s(b, r() % 2 ? L"%d-%d-%d" : L"%03d", v / 100, (v / 10) % 10, v % 10);
        if (b[1] != L'-')
            swprintf_s(b, L"%03d", v);
        return std::wstring(b);
    };
    switch (c)
    {
    case 0: // moves
    {
        cat = L"move";
        const std::wstring verb = RtPick(r, {L"move", L"go", L"head", L"relocate", L"push", L"move up"});
        const std::wstring d = RtDistance(r), dr = dir();
        if (r() % 2)
            return cs + RtCap(verb) + L" " + d + L" " + dr + L".";
        return cs + RtCap(verb) + L" " + dr + L" " + d + L".";
    }
    case 1: // bearings and clock
    {
        cat = L"bearing";
        const std::wstring d = RtDistance(r);
        const int h = 1 + (int)(r() % 12);
        wchar_t b[96];
        switch (r() % 5)
        {
        case 0: swprintf_s(b, L"Move to my %d o'clock, %ls.", h, d.c_str()); break;
        case 1: swprintf_s(b, L"Move to your %d o'clock, %ls.", h, d.c_str()); break;
        case 2: swprintf_s(b, L"Set heading %ls, move %ls.", digits3((int)(r() % 360)).c_str(), d.c_str()); break;
        case 3: swprintf_s(b, L"Bearing %ls, mark %ls.", digits3((int)(r() % 360)).c_str(), d.c_str()); break;
        default: swprintf_s(b, L"Heading %ls, %ls, and hold.", digits3((int)(r() % 360)).c_str(), d.c_str()); break;
        }
        return cs + b;
    }
    case 2: // fall back / hold / follow
    {
        cat = L"fallback-hold-follow";
        switch (r() % 4)
        {
        case 0: return cs + RtCap(RtPick(r, {L"fall back", L"fall back to me", L"pull back", L"withdraw", L"bug out", L"break off"})) + L".";
        case 1: return cs + L"Fall back " + RtDistance(r) + L" " + dir() + L".";
        case 2: return cs + RtCap(RtPick(r, {L"hold position", L"hold here", L"stay there", L"dig in", L"stand by", L"stay back",
                                              L"hold here and don't push", L"wait here"})) + L".";
        default: return cs + RtCap(RtPick(r, {L"form on me", L"follow me", L"stay close", L"stick with me", L"tighten up",
                                               L"regroup", L"on me", L"get back here", L"close up"})) + L".";
        }
    }
    case 3: // attacks
    {
        cat = L"attack";
        switch (r() % 4)
        {
        case 0: return cs + RtCap(RtPick(r, {L"attack my target", L"engage my target", L"focus fire on my target", L"take it down",
                                              L"hit my target", L"take out my target"})) + L".";
        case 1: return cs + RtCap(RtPick(r, {L"attack", L"engage", L"take down", L"take out", L"go after", L"focus fire on", L"kill",
                                              L"destroy", L"hit"})) + L" " + RtPick(r, {L"the", L"that"}) + L" {X}.";
        case 2: return cs + RtCap(RtPick(r, {L"attack the closest contact", L"engage the nearest enemy", L"engage at will",
                                              L"attack at will", L"pick your targets"})) + L".";
        default:
        {
            // "keep" always takes "busy", the others never (2026-10-11: "engage the heavies busy" is no sentence)
            const std::wstring v = RtPick(r, {L"go for", L"keep", L"engage", L"go after", L"deal with"});
            return cs + RtCap(v) + L" the " + RtPick(r, {L"lights", L"mediums", L"heavies", L"assaults"}) + (v == L"keep" ? L" busy." : L".");
        }
        }
    }
    case 4: // called shots
    {
        cat = L"called shot";
        const std::wstring part = RtPick(r, {L"legs", L"left leg", L"right leg", L"arms", L"left arm", L"right arm", L"side torsos",
                                              L"left torso", L"right torso", L"center torso", L"cockpit", L"head"});
        const std::wstring verb = RtPick(r, {L"get", L"take out", L"aim for", L"go for", L"shoot", L"hit", L"knock out", L"target"});
        switch (r() % 4)
        {
        case 0: return cs + RtCap(verb) + L" his " + part + L".";
        case 1: return cs + RtCap(verb) + L" the {X}'s " + part + L".";
        case 2: return cs + RtCap(part) + L" on the {X}.";
        default: return cs + RtCap(RtPick(r, {L"cancel called shot", L"pick your shots", L"aim anywhere"})) + L".";
        }
    }
    case 5: // fire settings
    {
        cat = L"fire settings";
        switch (r() % 4)
        {
        case 0: return cs + RtCap(RtPick(r, {L"weapons free", L"open fire", L"fire at will", L"return fire", L"hold fire", L"cease firing",
                                              L"weapons hold"})) + L".";
        case 1: return cs + RtCap(RtPick(r, {L"run cool", L"watch your heat", L"run hot", L"alpha strike", L"everything you've got",
                                              L"normal heat"})) + L".";
        case 2: return cs + RtCap(RtPick(r, {L"lasers", L"PPCs", L"ballistics", L"missiles", L"LRMs", L"SRMs", L"machine guns",
                                              L"flamers"})) + L" only.";
        default: return cs + RtCap(RtPick(r, {L"all weapons", L"reset fire orders", L"hold lasers", L"hold missiles",
                                               L"weapons free and run cool", L"indirect fire only", L"artillery only"})) + L".";
        }
    }
    case 6: // questions about a unit
    {
        cat = L"unit question";
        return RtCallsign(r, true) + RtCap(RtPick(r, {L"where's the {X}?", L"where is the {X}?", L"can you see the {X}?",
                                                      L"can anyone see the {X}?", L"which way is the {X} facing?", L"is the {X} down?",
                                                      L"status on the {X}.", L"how's the {X}?", L"who's closest to the {X}?",
                                                      L"patch in the {X}.", L"find the {X}."}));
    }
    case 7: // other questions
    {
        cat = L"question";
        return RtCallsign(r, true) + RtCap(RtPick(r, {L"status", L"sitrep", L"what's your status?", L"how are you holding up?",
                                                      L"how's your heat?", L"how's your ammo?", L"anyone low on ammo?", L"is anyone hurt?",
                                                      L"how bad is it?", L"sound off", L"where are you?", L"what do you see?",
                                                      L"how many contacts?", L"any heavies?", L"any air contacts?", L"who's closest?",
                                                      L"what are you targeting?", L"who's hitting you?", L"who's firing on me?",
                                                      L"say again"}));
    }
    case 8: // remarks, lancemate status, nav, vision
    {
        cat = L"remark-nav-misc";
        switch (r() % 3) // (enhanced imaging is switched at once, no action to compare - left out)
        {
        case 0: return RtPick(r, {L"Good kill, ", L"Nice shot, ", L"Check fire, ", L"Watch your fire, ", L"How's "}) +
                       std::wstring(RtPick(r, {L"Two", L"Three", L"Four"})) + L".";
        case 1: return cs + RtCap(RtPick(r, {L"move to", L"hold at", L"fall back to", L"advance to", L"indirect fire at"})) + L" Nav " +
                       RtPick(r, {L"Alpha", L"Bravo", L"Charlie", L"Delta"}) /* NATO (2026-10-14, user) */ + L".";
        default: return cs + RtCap(RtPick(r, {L"check fire", L"watch your fire", L"you're hitting me"})) + L".";
        }
    }
    default: // 9: the old fixed templates are kept in the mix by the caller
        cat = L"";
        return L"";
    }
}
int RoundTrip3(const std::wstring &minArg, const std::wstring &instArg = L"", int threads = 0, const std::wstring &onlyFile = L"",
               const std::wstring &shareArg = L"", const std::wstring &optArg = L"")
{
    SetConsoleTitleW((L"MW5 Voice Command - round trip loop" + (instArg.empty() ? std::wstring() : L" " + instArg)).c_str());
    LoadConfig();
    // 2026-10-11: several loops side by side - each its own share of the CPU and its own log / undecided file
    const std::wstring inst = instArg.empty() ? std::wstring() : L"_" + instArg;
    if (threads > 0)
        g_cfg.WhisperThreads = threads;
    g_cfg.WhisperGpu = 1; // 2026-10-14 (user): the round trip always uses the graphics card (CPU if it can't)
    // 2026-10-14 (user, A/B): 8th argument "s=1.25" = only these sweep speeds ("s=1.0+1.25" several), "g=1" / "g=0" =
    // callsign guess on / off for this loop (else GlobalSettings' callsign_guess)
    std::vector<float> optSpeeds;
    {
        std::wstring o = optArg + L",";
        size_t st = 0;
        for (size_t i = 0; i < o.size(); ++i)
            if (o[i] == L',')
            {
                const std::wstring kv = o.substr(st, i - st);
                st = i + 1;
                if (kv.size() > 2 && kv.compare(0, 2, L"s=") == 0)
                {
                    std::wstring v = kv.substr(2) + L"+";
                    size_t s0 = 0;
                    for (size_t j = 0; j < v.size(); ++j)
                        if (v[j] == L'+')
                        {
                            const float f = (float)_wtof(v.substr(s0, j - s0).c_str());
                            if (f > 0.3f && f < 3.0f)
                                optSpeeds.push_back(f);
                            s0 = j + 1;
                        }
                }
                else if (kv == L"g=1" || kv == L"g=0")
                    g_cfg.CallsignGuess = kv == L"g=1";
            }
    }
    Log(L"round trip loop: callsign guess %ls", g_cfg.CallsignGuess ? L"ON" : L"off");
    BuildPhrases();
    BuildOrderList();
    LinesTick(true);
    g_pronCheckMs = 0;
    PronTick();
    g_heardCheckMs = 0;
    HeardAsTick();
    LoadRadioConfig();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (g_rc.Tts != 1)
        Log(L"round trip: GlobalSettings has Windows voices - Kokoro is used for the round trip anyway");
    g_rc.Tts = 1;
    if (threads > 0)
        g_rc.KokoroThreads = std::max(1, threads / 2);
    Log(L"round trip loop%ls: %u hardware threads; Whisper %d threads, Kokoro %d", inst.c_str(), std::thread::hardware_concurrency(),
        g_cfg.WhisperThreads, g_rc.KokoroThreads);
    KokoroLoad();
    WhisperInit();
    if (!g_kokoroReady || !g_wctx)
    {
        Log(L"round trip: needs Kokoro and Whisper (see above) - nothing done");
        return 1;
    }
    const double minutes = minArg.empty() ? 60.0 : std::max(1.0, _wtof(minArg.c_str()));
    std::vector<std::wstring> lines, unitNames, commonList, ignoreList;
    // 2026-10-14 (user): a sentences file as the 5th argument = ONLY its sentences (no fixed list, no generated ones)
    const bool onlyMode = !onlyFile.empty();
    if (!RtReadLines(onlyMode ? ExeDir() + onlyFile : ExeDir() + L"RoundTripSentences.txt", lines) ||
        !RtReadLines(ExeDir() + L"RoundTripNames.txt", unitNames) ||
        !RtReadLines(ExeDir() + L"CommonWords.txt", commonList))
    {
        Log(L"round trip loop: needs RoundTripSentences.txt, RoundTripNames.txt and CommonWords.txt beside the exe - nothing done");
        return 1;
    }
    RtReadLines(ExeDir() + L"HeardAs.ignore.txt", ignoreList);
    std::set<std::wstring> common, ignore;
    for (auto &c : commonList)
        common.insert(RtLower(c));
    for (auto &c : ignoreList)
        ignore.insert(JoinWords(SplitWords(c)));
    // 2026-10-12 (user): every unit name, as heard words - a mishearing that IS another unit's name ("gun" for the
    // Garm) is never added: it would send every order for that unit to the other one.
    std::set<std::wstring> unitWords;
    for (auto &c : unitNames)
        unitWords.insert(RtLower(JoinWords(SplitWords(c))));
    std::vector<RtSentence> sents;
    // 2026-10-14 (user test): words that are a REAL part of an order - every word of the sentences file and every nav
    // letter. A misheard stretch with one of them ("epsilon" heard for "Upsilon") is never a sure rule: it would turn
    // every "nav Epsilon" into Upsilon. Left for review instead.
    std::set<std::wstring> vocab;
    for (int l = 0; l < kVhudNavLetters; ++l)
        vocab.insert(NavWord(l));
    for (auto &l : lines)
    {
        const size_t bar = l.find(L'|');
        if (bar != std::wstring::npos)
            for (auto &x : CollapseRepeats(NormalizeHeard(l.substr(bar + 1))))
                vocab.insert(x);
    }
    for (auto &l : lines)
    {
        const size_t bar = l.find(L'|');
        if (bar == std::wstring::npos)
            continue;
        std::wstring e = l.substr(0, bar), t = l.substr(bar + 1);
        while (!e.empty() && e.back() == L' ')
            e.pop_back();
        while (!t.empty() && t[0] == L' ')
            t.erase(0, 1);
        sents.push_back({e, t});
    }
    const int nVoices = (int)(sizeof(kKokoroNames) / sizeof(kKokoroNames[0]));
    if (sents.empty() || unitNames.empty())
        return 1;
    // 2026-10-10 (user): half the tries come from the full fixed command list (every callsign x order, BuildPhrases)
    std::vector<std::pair<std::wstring, Cmd>> cmds(g_phrases.begin(), g_phrases.end());
    // what HeardAs.txt already has (heard -> meant), so a clash is not added
    // 2026-10-12: re-read before every add (loadHave below) - the loops run side by side and share HeardAs.txt, so
    // one loop's adds (and the user's own edits / removals) must be seen by the other, or rules get added twice and
    // removed ones come back.
    std::map<std::wstring, std::wstring> have;
    auto loadHave = [&have]() {
        have.clear();
        std::vector<std::wstring> hl;
        RtReadLines(HeardAsPath(), hl);
        for (auto &l : hl)
        {
            const size_t eq = l.find(L'=');
            if (eq != std::wstring::npos)
                have[JoinWords(SplitWords(l.substr(0, eq)))] = JoinWords(SplitWords(l.substr(eq + 1)));
        }
    };
    loadHave();
    auto appendUtf8 = [](const std::wstring &path, const std::wstring &text) {
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f)
        {
            const std::string u = Utf8(text);
            fwrite(u.data(), 1, u.size(), f);
            fclose(f);
        }
    };
    std::map<std::wstring, std::pair<int, std::wstring>> undecided; // "heard = meant" -> count, reason
    // 2026-10-14 (user): the sure ones are written to HeardAs.txt every 30 minutes (and at the end), not at once -
    // until then they wait here (and count as known, so they are not queued twice).
    std::vector<std::pair<std::wstring, std::wstring>> pending; // heard -> meant
    ULONGLONG lastFlush = GetTickCount64();
    auto flushPending = [&]() {
        if (pending.empty())
            return;
        loadHave(); // another loop (or the user) may have written the same rule meanwhile
        int n = 0;
        for (auto &pr : pending)
        {
            auto hv = have.find(pr.first);
            if (hv != have.end())
                continue;
            const std::wstring line = pr.first + L" = " + pr.second;
            appendUtf8(HeardAsPath(), L"\r\n" + line); // no comment on the line: HeardAs.txt has no inline comments
            appendUtf8(ExeDir() + L"HeardAs.auto.txt", line + L"\r\n");
            have[pr.first] = pr.second;
            ++n;
        }
        pending.clear();
        g_heardCheckMs = 0;
        g_heardTime = FILETIME{};
        HeardAsTick();
        Log(L"round trip loop: %d rule(s) written to HeardAs.txt (30-minute batch)", n);
    };
    auto writeUndecided = [&]() {
        std::wstring o = L"# HeardAs.undecided.txt - written by VoiceCommand --roundtrip3: mishearings NOT added to HeardAs.txt\r\n"
                         L"# (common words, a clash, or the rest of the sentence misheard). Copy the ones you want into HeardAs.txt.\r\n";
        std::vector<std::pair<std::wstring, std::pair<int, std::wstring>>> v(undecided.begin(), undecided.end());
        std::sort(v.begin(), v.end(), [](const std::pair<std::wstring, std::pair<int, std::wstring>> &x,
                                         const std::pair<std::wstring, std::pair<int, std::wstring>> &y) {
            return x.second.first > y.second.first;
        });
        for (auto &kv : v)
            o += kv.first + L"   # " + std::to_wstring(kv.second.first) + L"x, " + kv.second.second + L"\r\n";
        WriteFileUtf8(ExeDir() + L"HeardAs.undecided" + inst + L".txt", o);
    };
    {
        const std::wstring logPath = ExeDir() + L"RoundTrip3_log" + inst + L".tsv";
        if (GetFileAttributesW(logPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            appendUtf8(logPath, L"\xFEFFtime\tvoice\tsentence\twhisper wrote\tafter clean-up\tunderstood\twhat it became\tdone\r\n");
        appendUtf8(ExeDir() + L"HeardAs.auto.txt", L"# --roundtrip3 run started\r\n");
    }
    std::mt19937 rng((unsigned)GetTickCount64() ^ (unsigned)(std::hash<std::wstring>()(inst) * 2654435761u));
    Log(L"round trip loop: %.0f minutes, %zu sentences x %zu names x %d voices at random - RoundTrip3.stop beside the exe ends it",
        minutes, sents.size(), unitNames.size(), nVoices);
    // 2026-10-14 (user): with a sentences file, EVERY sentence x voice x speed once (Kokoro + greedy Whisper give the same
    // result for the same input - random picks mostly repeated). The same shuffle (fixed seed) in every loop; the 7th
    // argument "k/n" gives loop k every n-th one, so n loops split the list without overlap. Done = the list is finished.
    struct RtCombo
    {
        int   S, V;
        float Sp;
    };
    std::vector<RtCombo> sweep;
    size_t sweepAt = 0;
    if (onlyMode)
    {
        int shK = 1, shN = 1;
        if (swscanf_s(shareArg.c_str(), L"%d/%d", &shK, &shN) != 2 || shN < 1 || shK < 1 || shK > shN)
            shK = shN = 1;
        static const float kSpeeds[] = {1.0f, 1.15f, 1.25f}; // 2026-10-14 (user): 0.85 dropped, 1.25 added
        const std::vector<float> speeds = optSpeeds.empty() ? std::vector<float>(std::begin(kSpeeds), std::end(kSpeeds)) : optSpeeds;
        std::vector<RtCombo> all;
        for (int si = 0; si < (int)sents.size(); ++si)
            for (int v = 0; v < nVoices; ++v)
                for (float sp : speeds)
                    all.push_back(RtCombo{si, v, sp});
        std::mt19937 fixedRng(20261014u);
        std::shuffle(all.begin(), all.end(), fixedRng);
        for (size_t i = 0; i < all.size(); ++i)
            if ((int)(i % (size_t)shN) == shK - 1)
                sweep.push_back(all[i]);
        Log(L"round trip loop: every sentence x voice x speed once - %zu in all, this loop %d/%d takes %zu", all.size(), shK, shN,
            sweep.size());
    }
    const ULONGLONG t0 = GetTickCount64();
    int tests = 0, passed = 0, added = 0, undec = 0;
    while (GetTickCount64() - t0 < (ULONGLONG)(minutes * 60000.0))
    {
        if (onlyMode && sweepAt >= sweep.size())
        {
            Log(L"round trip loop: the whole list is done");
            break;
        }
        if (GetFileAttributesW((ExeDir() + L"RoundTrip3.stop").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            Log(L"round trip loop: RoundTrip3.stop found - stopping");
            break;
        }
        if (GetTickCount64() - lastFlush >= 30ull * 60000ull)
        {
            flushPending();
            lastFlush = GetTickCount64();
        }
        // 2026-10-11 (user): 11 equal parts - the fixed command list, the old templates, and the 9 generated categories
        const int part = onlyMode ? 1 : (int)(rng() % 11); // only mode: the file's sentences, every try
        const bool cmdRound = !cmds.empty() && part == 0;
        const std::pair<std::wstring, Cmd> &cp = cmds[cmds.empty() ? 0 : rng() % cmds.size()];
        RtSentence st = sents[rng() % sents.size()];
        float spd = 1.0f;
        int sweepV = -1;
        if (onlyMode)
        {
            const RtCombo &cb = sweep[sweepAt++];
            st = sents[(size_t)cb.S];
            spd = cb.Sp;
            sweepV = cb.V;
        }
        std::wstring genCat = part == 0 ? L"fixed list" : part == 1 ? L"templates" : L"";
        if (part >= 2)
        {
            std::wstring g;
            do
                g = RtGenerate(rng, genCat);
            while (g.empty());
            st = RtSentence{L"same", g};
        }
        const size_t xp = cmdRound ? std::wstring::npos : st.Text.find(L"{X}");
        const bool named = xp != std::wstring::npos;
        const std::wstring name = named ? unitNames[rng() % unitNames.size()] : L"";
        const int sid = sweepV >= 0 ? sweepV : (int)(rng() % (unsigned)nVoices);
        std::wstring sentence = cmdRound ? RtSpoken(cp.first) : st.Text;
        if (named)
            sentence.replace(xp, 3, name);
        {
            std::lock_guard<std::mutex> lk(g_namesMx);
            g_knownNames.clear();
            if (named)
                g_knownNames.push_back(name);
            for (const wchar_t *d : {L"Atlas", L"Bane", L"Hunchback", L"Jenner"})
                if (!named || !SameUnitName(d, name))
                    g_knownNames.push_back(d);
        }
        // expect "same" (2026-10-11): what the TYPED sentence becomes is the answer - any order, any question
        auto freshContext = [&]() {
            g_lastWho.clear();
            g_lastWhoMs = 0;
            g_lastAnswerer = 0;
            g_lastAnswerMs = 0;
            g_offerMs = 0;
            g_lastReporter = 0;
            g_lastReporterMs = 0;
            g_corrWho.clear();
            g_corrWhoMs = 0;
            std::lock_guard<std::mutex> lk(g_amx);
            g_actions.clear();
        };
        HeardAsTick(); // hand edits to HeardAs.txt are used from the next try (2026-10-11; was: only after an auto-add)
        const bool sameRound = !cmdRound && st.Expect == L"same";
        if (sameRound && (sentence.find(L"nhanced imaging") != std::wstring::npos))
            continue; // a cockpit switch, no lancemate action to compare (2026-10-11)
        std::vector<SpokenAction> want;
        if (sameRound)
        {
            freshContext();
            HandleSpoken(sentence, true, 0.0);
            std::lock_guard<std::mutex> lk(g_amx);
            want.swap(g_actions);
            if (want.empty())
            {
                // not understood even typed: a parser gap, not a mishearing - kept for review
                appendUtf8(ExeDir() + L"RoundTrip3_typedfail" + inst + L".tsv", genCat + L"\t" + sentence + L"\r\n");
                continue;
            }
        }
        SherpaOnnxGenerationConfig gc;
        std::memset(&gc, 0, sizeof(gc));
        gc.sid = sid;
        gc.speed = spd; // 2026-10-14: 1.0 / 1.15 / 1.25 in the sweep (real speech varies)
        gc.silence_scale = 0.2f;
        const std::string t = Utf8(KokoroText(sentence));
        const SherpaOnnxGeneratedAudio *ga = SherpaOnnxOfflineTtsGenerateWithConfig(g_kokoro, t.c_str(), &gc, nullptr, nullptr);
        std::vector<float> pcm;
        if (ga && ga->n > 0 && ga->sample_rate > 0)
        {
            const double step = ga->sample_rate / 16000.0;
            for (double pos = 0.0; (size_t)pos + 1 < (size_t)ga->n; pos += step)
            {
                const size_t i0 = (size_t)pos;
                const float fr = (float)(pos - i0);
                pcm.push_back(ga->samples[i0] * (1.0f - fr) + ga->samples[i0 + 1] * fr);
            }
            pcm.insert(pcm.begin(), 4000, 0.0f);
            pcm.insert(pcm.end(), 4000, 0.0f);
        }
        if (ga)
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(ga);
        if (pcm.empty())
            continue;
        bool noise = true, faulted = false;
        const std::wstring heard = WhisperTranscribe(pcm, noise, faulted);
        if (faulted)
        {
            Log(L"round trip loop: Whisper crashed - stopping");
            break;
        }
        g_lastWho.clear();
        g_lastWhoMs = 0;
        g_lastAnswerer = 0;
        g_lastAnswerMs = 0;
        g_offerMs = 0;
        g_lastReporter = 0;
        g_lastReporterMs = 0;
        g_corrWho.clear();
        g_corrWhoMs = 0;
        {
            std::lock_guard<std::mutex> lk(g_amx);
            g_actions.clear();
        }
        HandleSpoken(heard, true, 0.0);
        std::vector<SpokenAction> acts;
        {
            std::lock_guard<std::mutex> lk(g_amx);
            acts.swap(g_actions);
        }
        std::wstring got;
        bool ok = sameRound ? false : RtJudge(cmdRound ? std::wstring() : st.Expect, acts, name, got);
        if (cmdRound)
            ok = RtJudgeCmd(cp.second, acts);
        if (sameRound)
        {
            ok = acts.size() == want.size();
            for (size_t k = 0; ok && k < acts.size(); ++k)
            {
                const SpokenAction &a = acts[k], &b = want[k];
                ok = a.SayAgain == b.SayAgain && a.Query == b.Query && a.C.Type == b.C.Type && a.C.Who == b.C.Who && a.C.A == b.C.A && a.C.B == b.C.B &&
                     a.C.C == b.C.C && a.C.D == b.C.D && a.ReplyGroup == b.ReplyGroup &&
                     (a.QueryName.empty() == b.QueryName.empty()) && (a.QueryName.empty() || SameUnitName(a.QueryName, b.QueryName));
            }
            got = std::to_wstring(acts.size()) + L" action(s)" + (ok ? L", as typed" : L", NOT as typed");
        }
        ++tests;
        passed += ok ? 1 : 0;
        const std::vector<std::wstring> w = CollapseRepeats(NormalizeHeard(heard));
        std::wstring done;
        if (!ok && named)
        {
            const std::wstring meant = RtLower(JoinWords(SplitWords(name)));
            const std::vector<std::wstring> pre = CollapseRepeats(NormalizeHeard(st.Text.substr(0, xp)));
            const std::vector<std::wstring> suf = CollapseRepeats(NormalizeHeard(st.Text.substr(xp + 3)));
            std::wstring mid, reason;
            if (w.size() > pre.size() + suf.size() && std::equal(pre.begin(), pre.end(), w.begin()) &&
                std::equal(suf.begin(), suf.end(), w.end() - (long)suf.size()))
                mid = JoinWords(std::vector<std::wstring>(w.begin() + (long)pre.size(), w.end() - (long)suf.size()));
            else
            {
                mid = JoinWords(w);
                reason = L"the rest of the sentence misheard (" + st.Text + L")";
            }
            if (reason.empty() && (SameUnitName(mid, name) || UnitAnswersTo(name, mid, true)))
                reason = L"name heard right, not understood (" + st.Text + L")";
            bool allCommon = true;
            for (auto &x : SplitWords(mid))
                allCommon = allCommon && (x.size() <= 2 || common.count(x) > 0);
            if (reason.empty() && allCommon)
                reason = L"common words";
            if (reason.empty() && mid != meant && unitWords.count(mid))
                reason = L"another unit's name";
            loadHave(); // 2026-10-12: the file as it is now, not as it was at the start
            auto hv = have.find(mid);
            if (reason.empty() && hv != have.end() && hv->second != meant)
                reason = L"clash: HeardAs has " + mid + L" = " + hv->second;
            // 2026-10-15 (user): nothing is added to HeardAs.txt by the loop any more - the sure ones go to the
            // undecided file too (marked "sure"), for the user to approve.
            if (reason.empty() && hv == have.end())
                reason = L"sure - not added: every mishearing is for review (2026-10-15, user)";
            if (ignore.count(mid))
                done = L"ignored";
            else if (reason.empty() && hv == have.end() &&
                     std::none_of(pending.begin(), pending.end(), [&](const std::pair<std::wstring, std::wstring> &q) { return q.first == mid; }))
            {
                pending.push_back({mid, meant}); // written with the next 30-minute batch
                ++added;
                done = L"ADDED " + mid + L" = " + meant + L" (next batch)";
            }
            else if (!reason.empty())
            {
                auto &u = undecided[mid + L" = " + meant];
                if (u.first == 0)
                    ++undec;
                ++u.first;
                u.second = reason;
                writeUndecided();
                done = L"undecided: " + reason;
            }
            else
                done = L"already in HeardAs";
        }
        else if (!ok)
            done = cmdRound ? L"undecided (command)" : L"undecided (no name in the sentence)";
        // 2026-10-14 (user): a sentence WITHOUT a unit name, misheard in ONE stretch (1-3 words, the rest word for word):
        // "heard = meant" for that stretch is sure - queued like a name - when the heard words are not all common
        // English, not another unit's name, and clash with nothing in HeardAs.txt; otherwise it is left for review.
        bool wordDone = false;
        if (!ok && !named && !cmdRound)
        {
            const std::vector<std::wstring> tw = CollapseRepeats(NormalizeHeard(sentence));
            size_t a = 0;
            while (a < tw.size() && a < w.size() && tw[a] == w[a])
                ++a;
            size_t bt = tw.size(), bh = w.size();
            while (bt > a && bh > a && tw[bt - 1] == w[bh - 1])
            {
                --bt;
                --bh;
            }
            const size_t nt = bt - a, nh = bh - a;
            if (nt >= 1 && nt <= 3 && nh >= 1 && nh <= 3)
            {
                wordDone = true;
                const std::wstring hmid = JoinWords(std::vector<std::wstring>(w.begin() + (long)a, w.begin() + (long)bh));
                const std::wstring tmid = JoinWords(std::vector<std::wstring>(tw.begin() + (long)a, tw.begin() + (long)bt));
                bool anyCommon = false, anyVocab = false;
                for (auto &x : SplitWords(hmid))
                {
                    anyCommon = anyCommon || x.size() <= 2 || common.count(x) > 0;
                    anyVocab = anyVocab || vocab.count(x) > 0;
                }
                loadHave();
                auto hv = have.find(hmid);
                std::wstring reason;
                if (anyVocab)
                    reason = L"a real order word (another nav letter / order word)";
                else if (anyCommon)
                    reason = L"common words";
                else if (unitWords.count(hmid))
                    reason = L"another unit's name";
                else if (hv != have.end() && hv->second != tmid)
                    reason = L"clash: HeardAs has " + hmid + L" = " + hv->second;
                if (reason.empty() && hv == have.end()) // 2026-10-15 (user): for review, not added
                    reason = L"sure - not added: every mishearing is for review (2026-10-15, user)";
                if (ignore.count(hmid))
                    done = L"ignored";
                else if (reason.empty() && hv == have.end() &&
                         std::none_of(pending.begin(), pending.end(), [&](const std::pair<std::wstring, std::wstring> &q) { return q.first == hmid; }))
                {
                    pending.push_back({hmid, tmid});
                    ++added;
                    done = L"ADDED " + hmid + L" = " + tmid + L" (next batch)";
                }
                else if (reason.empty())
                    done = L"already in HeardAs / queued";
                else
                {
                    auto &u = undecided[hmid + L" = " + tmid];
                    if (u.first == 0)
                        ++undec;
                    ++u.first;
                    u.second = reason + L" (" + sentence + L")";
                    writeUndecided();
                    done = L"undecided: " + reason;
                }
            }
        }
        if (!ok && !named && !wordDone)
        {
            auto &u = undecided[JoinWords(w) + L" = (" + (cmdRound ? cp.first : st.Text) + L")"];
            if (u.first == 0)
                ++undec;
            ++u.first;
            u.second = cmdRound ? L"command not understood as said: " + got : L"a sentence without a name misheard";
            writeUndecided();
        }
        SYSTEMTIME tm;
        GetLocalTime(&tm);
        wchar_t ts[16];
        swprintf_s(ts, L"%02d:%02d:%02d", tm.wHour, tm.wMinute, tm.wSecond);
        appendUtf8(ExeDir() + L"RoundTrip3_log" + inst + L".tsv", std::wstring(ts) + L"\t" + kKokoroNames[sid] +
                                (spd != 1.0f ? (spd < 1.2f ? L"@1.15" : L"@1.25") : L"") + L"\t" + genCat + L": " + sentence + L"\t" + heard +
                                                        L"\t" + JoinWords(w) + L"\t" + (ok ? L"yes" : L"NO") + L"\t" + got + L"\t" +
                                                        done + L"\r\n");
        if (!done.empty() && done.compare(0, 5, L"ADDED") == 0)
            Log(L"round trip loop: %ls", done.c_str());
        if (tests % 50 == 0)
            Log(L"round trip loop: %d tries, %d understood (%.1f%%), %d added to HeardAs, %d undecided (%.0f min)", tests, passed,
                100.0 * passed / tests, added, undec, (GetTickCount64() - t0) / 60000.0);
    }
    flushPending();
    Log(L"round trip loop: done - %d tries, %d understood (%.1f%%), %d added to HeardAs (HeardAs.auto.txt), %d undecided "
        L"(HeardAs.undecided.txt)",
        tests, passed, tests ? 100.0 * passed / tests : 0.0, added, undec);
    return 0;
}
// ---- PARSE TEST (2026-10-10): VoiceCommand.exe --parsetest <file> -------------------------------------------------------
// No audio: every line's first column (TSV) goes straight into HandleSpoken as if Whisper wrote it (push-to-talk held,
// fresh context each line, a few enemies on scan: Atlas, Hunchback, Jenner, Catapult, Locust, Raven, Marauder, Shadow Hawk).
// ParseTest_report.tsv: utterance, the other columns, after clean-up, what it became.
int ParseTest(const std::wstring &file, const std::wstring &opt)
{
    LoadConfig();
    if (opt == L"g=1" || opt == L"g=0") // 2026-10-14: callsign guess on / off for this run
        g_cfg.CallsignGuess = opt == L"g=1";
    BuildPhrases();
    BuildOrderList();
    g_heardCheckMs = 0;
    HeardAsTick();
    std::vector<std::wstring> lines;
    if (!RtReadLines(file.empty() ? ExeDir() + L"PlayerCommands.tsv" : file, lines))
    {
        Log(L"parse test: no file");
        return 1;
    }
    {
        std::lock_guard<std::mutex> lk(g_namesMx);
        g_knownNames = {L"Atlas", L"Hunchback", L"Jenner", L"Catapult", L"Locust", L"Raven", L"Marauder", L"Shadow Hawk",
                        L"Atlas III", L"Mongoose II", L"Wolfhound II", L"Dragon II", L"Marauder II",
                        L"Hunchback IIC", L"Jenner IIC", L"Shadow Hawk IIC", L"Rifleman IIC"}; // + numbered (2026-10-11)
    }
    std::wstring o = L"\xFEFFutterance\texpected\tcategory\tafter clean-up\twhat it became\tfull\r\n";
    int n = 0, none = 0;
    const bool big = lines.size() > 2000; // the full audit: quiet, with progress
    g_quiet = big;
    for (auto &l : lines)
    {
        const size_t tab = l.find(L'\t');
        const std::wstring text = tab == std::wstring::npos ? l : l.substr(0, tab);
        const std::wstring rest = tab == std::wstring::npos ? L"" : l.substr(tab + 1);
        if (text == L"utterance")
            continue;
        g_lastWho.clear();
        g_lastWhoMs = 0;
        g_lastAnswerer = 0;
        g_lastAnswerMs = 0;
        g_offerMs = 0;
        g_lastReporter = 0;
        g_lastReporterMs = 0;
        g_corrWho.clear();
        g_corrWhoMs = 0;
        {
            std::lock_guard<std::mutex> lk(g_amx);
            g_actions.clear();
        }
        HandleSpoken(text, true, 0.0);
        std::vector<SpokenAction> acts;
        {
            std::lock_guard<std::mutex> lk(g_amx);
            acts.swap(g_actions);
        }
        std::wstring got;
        RtJudge(L"", acts, L"", got);
        ++n;
        none += acts.empty() ? 1 : 0;
        // every field of every action (2026-10-14, the full audit): T type, W who, A B C D, Q query, N name, R reply, flags
        std::wstring full;
        for (auto &a : acts)
        {
            wchar_t b[160];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"%lsT%d W%d A%d B%d C%d D%d Q%d%ls%ls%ls", full.empty() ? L"" : L" | ", a.C.Type,
                         a.C.Who, a.C.A, a.C.B, a.C.C, a.C.D, a.Query, a.SayAgain ? L" SAYAGAIN" : L"", a.Correct ? L" CORR" : L"",
                         a.Cancel ? L" CANCEL" : L"");
            full += b;
            if (!a.QueryName.empty())
                full += L" N=" + a.QueryName;
            if (!a.ReplyGroup.empty())
                full += L" R=" + a.ReplyGroup;
        }
        o += text + L"\t" + rest + L"\t" + JoinWords(CollapseRepeats(NormalizeHeard(text))) + L"\t" + got + L"\t" + full + L"\r\n";
        if (big && n % 5000 == 0)
        {
            wprintf(L"parse test: %d / %zu\n", n, lines.size());
            fflush(stdout);
        }
    }
    g_quiet = false;
    WriteFileUtf8(ExeDir() + L"ParseTest_report.tsv", o);
    Log(L"parse test: %d lines, %d did nothing - ParseTest_report.tsv beside the exe", n, none);
    return 0;
}
#else
int RoundTrip(const std::wstring &)
{
    Log(L"round trip: needs Whisper and Kokoro built in");
    return 1;
}
int RoundTrip2(const std::wstring &)
{
    Log(L"round trip: needs Whisper and Kokoro built in");
    return 1;
}
int RoundTrip3(const std::wstring &, const std::wstring & = L"", int = 0, const std::wstring & = L"", const std::wstring & = L"",
               const std::wstring & = L"")
{
    Log(L"round trip: needs Whisper and Kokoro built in");
    return 1;
}
int ParseTest(const std::wstring &, const std::wstring &)
{
    Log(L"parse test: needs the round-trip build");
    return 1;
}
#endif

} // namespace

namespace // the IN-GAME TEST: in the same (anonymous) namespace as Line(), which it hooks
{
// ---- IN-GAME TEST (2026-10-10, user): "VoiceCommand.exe --gametest [GameTest.tsv]" --------------------------------
// The normal VoiceCommand (game link, voices, replies), but the orders come from the file - typed, as Whisper would
// write them - and each one is CHECKED against what the game publishes: MW5_LanceStatus (order events, mode flags,
// fire settings, targets), the TacOps members (move points, positions, facing) and the game's log.txt (called-shot
// and aim lines). The microphone is ignored while it runs. Results: GameTest_report.tsv beside the exe.
// File (tab separated): who, utterance, checks, timeout s. who: a slot, "2,3", "all" (every live lancemate) or "-"
// (no lancemate - remarks, questions to anyone). who "!" = a directive: "pilot <TestPilot command>", "wait <s>",
// "lockwait <s>" (until you have a lock). Lines starting with # are notes. Checks, space separated, all must hold:
//   order=<move|beeline|attack|follow|fallback|hold|indirect|lock>  each slot got that order (LSE_OrderReceived;
//                              2026-10-13: move = fire on the move, beeline / run = the straight TCO_Move)
//   noorder                    no order reached any lancemate          mode=<hold|fallback|indirect|attack|movefire|flank|pincer|free>
//   (order=flank / order=pincer: TCO_Flank, 2026-10-20)
//   disc=<0..2> heat=<0..3> mask=<0..255>   the slot's fire settings  target=lock | target=<name> | class=<0..3>
//   move=<metres>@<my|your|c><degrees>   the slot's new move point, measured from you (my: 0 = where you look,
//                              c = compass) or from the lancemate (your: 0 = where it faces); +-20 deg, +-30 %
//   walk          it walks toward its move point (or is there)   face   while falling back it faces its target
//   reply=<group prefix>       a pilot said a line of that group      log=<text>  log.txt got it ({s} = the slot)
//   aim=<text>    most of the slot's direct-fire volleys aim at it      lock   (needs your lock - TestPilot is sent for one, else SKIP)
//   vision=<0|1>  Enhanced Imaging vision mode
struct GtTest
{
    std::wstring Who, Text, Checks;
    int          Timeout = 15;
    int          Row = 0;
};
struct GtNote
{
    std::wstring Group;
    int          Slot;
};
bool                g_gtOn = false;
std::wstring        g_gtFile;
std::vector<GtNote> g_gtNotes;
std::mutex          g_gtMx;
void GtNoteLine(const wchar_t *group, int slot)
{
    if (!g_gtOn || !group)
        return;
    std::lock_guard<std::mutex> lk(g_gtMx);
    if (g_gtNotes.size() < 4000)
        g_gtNotes.push_back({group, slot});
}

struct GtSlotSnap
{
    int      Slot = 0;
    float    Loc[3] = {};
    float    Yaw = 0;
    uint32_t MoveSeq = 0;
    float    WalkD0 = -1; // distance to the new move point when first seen
    bool     WalkOk = false;
    int      FaceN = 0, FaceOk = 0;
    uint16_t ModeSeen = 0; // 2026-10-16: every mode bit seen at any poll (a short hold / fall back still counts)
};
struct GtRun
{
    std::vector<GtTest>     T;
    size_t                  I = 0;
    int                     Phase = 0; // 0 next, 1 checking, 2 directive wait
    ULONGLONG               StartMs = 0, WaitUntil = 0, PollMs = 0;
    uint32_t                EvHead = 0;
    uint64_t                LogOff = 0;
    std::string             Log;          // log.txt since the order
    std::vector<GtSlotSnap> S;
    float                   MeLoc[3] = {}, MeYaw = 0;
    bool                    HaveMe = false;
    uint32_t                LockLo = 0, LockHi = 0;
    int                     LockWait = 0;
    size_t                  LockTry = (size_t)-1; // 2026-10-16: the line TestPilot was last sent after a lock for
    int                     Pass = 0, Fail = 0, Skip = 0;
    std::wstring            CurText, CurChecks; // {enemy} filled in
    FILE                   *Rep = nullptr;
    bool                    Started = false;
    // DEATH PAUSE (2026-10-16, user: long phrasing test, restart the mission on any death): a death pauses the test;
    // once the whole lance is back and alive for 20 s it replays the directives of the section and goes on from the
    // line that was interrupted. GameTest_status.txt beside the exe says RUNNING / PAUSED / DONE.
    bool                    Paused = false;
    size_t                  ResumeI = 0, ReplayTo = 0;
    int                     MateCount = -1, Restarts = 0;
    ULONGLONG               AliveSince = 0, StatusMs = 0;
} g_gt;
int g_gtStartRow = 0; // --gametest <file> <row>: start at that row (its section's directives first)

static std::wstring GtLogPath()
{
    wchar_t ad[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    return std::wstring(ad) + L"\\UnrealVRMod\\MechWarrior-Win64-Shipping\\log.txt";
}
static uint64_t GtLogSize()
{
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(GtLogPath().c_str(), GetFileExInfoStandard, &fa))
        return 0;
    return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}
static void GtReadLog()
{
    HANDLE h = CreateFileW(GtLogPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    if ((uint64_t)sz.QuadPart < g_gt.LogOff)
        g_gt.LogOff = 0; // a new game: log.txt started again
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)g_gt.LogOff;
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got)
    {
        g_gt.Log.append(buf, got);
        g_gt.LogOff += got;
        if (g_gt.Log.size() > 8u * 1024 * 1024)
            g_gt.Log.erase(0, g_gt.Log.size() - 4u * 1024 * 1024);
    }
    CloseHandle(h);
}
static std::string GtNarrow(const std::wstring &w)
{
    std::string s;
    for (wchar_t c : w)
        s += (c < 128) ? (char)c : '?';
    return s;
}
static std::wstring GtLower(std::wstring s)
{
    for (auto &c : s)
        c = (wchar_t)towlower(c);
    return s;
}
static bool GtLsSlot(int slot, LsSlot &out)
{
    if (!g_ls || !g_ls->Enabled)
        return false;
    const uint32_t n = std::min<uint32_t>(g_ls->SlotCount, kLsMaxSlots);
    for (uint32_t i = 0; i < n; ++i)
        if (g_ls->Slots[i].Valid && g_ls->Slots[i].Slot == slot)
        {
            out = g_ls->Slots[i];
            return true;
        }
    return false;
}
static bool GtMember(int slot, VanillaExportBlock::TacMember &out)
{
    if (!g_blk)
        return false;
    VanillaExportBlock::TacMember m[VanillaExportBlock::kTacMaxMembers] = {};
    const int n = ReadLance(m);
    for (int i = 0; i < n; ++i)
        if (m[i].Valid && m[i].Slot == slot)
        {
            out = m[i];
            return true;
        }
    return false;
}
static bool GtUnitXY(uint32_t lo, uint32_t hi, float xy[2])
{
    TerrainMechInfo m = {};
    if (!MechInfoOf(lo, hi, m, false) || !g_lid || g_lid->Magic != LIDAR_MAGIC)
        return false;
    uint32_t n = std::min<uint32_t>(g_lid->ContactCount, (uint32_t)LIDAR_MAX_CONTACTS);
    for (uint32_t i = 0; i < n; ++i)
        if (g_lid->Contacts[i].Id == m.Id)
        {
            xy[0] = g_lid->Contacts[i].X;
            xy[1] = g_lid->Contacts[i].Y;
            return true;
        }
    return false;
}
static float GtAngDiff(float a, float b)
{
    float d = std::fmod(a - b, 360.0f);
    if (d > 180.0f)
        d -= 360.0f;
    if (d < -180.0f)
        d += 360.0f;
    return std::fabs(d);
}
static int GtOrderCode(const std::wstring &n)
{
    if (n == L"move") return VanillaExportBlock::TCO_MoveFire; // 2026-10-13: moves fire on the move
    if (n == L"beeline" || n == L"run") return VanillaExportBlock::TCO_Move;
    if (n == L"attack") return VanillaExportBlock::TCO_Attack;
    if (n == L"follow") return VanillaExportBlock::TCO_Follow;
    if (n == L"fallback") return VanillaExportBlock::TCO_Fallback;
    if (n == L"hold") return VanillaExportBlock::TCO_HoldHere;
    if (n == L"advance" || n == L"movefire") return VanillaExportBlock::TCO_MoveFire; // 2026-10-13: advance = fire on the move
    if (n == L"indirect") return VanillaExportBlock::TCO_IndirectCover;
    if (n == L"lock") return VanillaExportBlock::TCO_LockTarget;
    if (n == L"attackmy") return VanillaExportBlock::TCO_AttackMyTarget;
    if (n == L"flank" || n == L"pincer") return VanillaExportBlock::TCO_Flank; // 2026-10-20 (lancecontrol-75)
    return -1;
}
static std::vector<std::wstring> GtSplit(const std::wstring &s, wchar_t sep)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s)
        if (c == sep)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    out.push_back(cur);
    return out;
}
static std::wstring GtTrim(std::wstring s)
{
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\r' || s.back() == L'\n' || s.back() == L'\t'))
        s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == L' ' || s[b] == 0xFEFF))
        ++b;
    return s.substr(b);
}

// One check token. 1 = holds, 0 = not (yet), -1 = can never hold now. final: the time is up (walk / face / aim /
// noorder are judged then). why: what was seen.
static int GtCheckOne(const std::wstring &tok, bool final, std::wstring &why)
{
    const size_t eq = tok.find(L'=');
    const std::wstring k = eq == std::wstring::npos ? tok : tok.substr(0, eq), v = eq == std::wstring::npos ? L"" : tok.substr(eq + 1);
    wchar_t b[256];
    if (k == L"lock")
        return 1; // checked before the order
    if (k == L"reply")
    {
        std::lock_guard<std::mutex> lk(g_gtMx);
        std::vector<std::pair<std::wstring, int>> seen; // 2026-10-16: each group once, with a count (31 outnumbered
        for (auto &n : g_gtNotes)                       // callouts hid every later reply)
        {
            for (auto &alt : GtSplit(v, L'|')) // reply=a|b: either group prefix
                if (!alt.empty() && n.Group.compare(0, alt.size(), alt) == 0)
                    return 1;
            if (!n.Group.compare(0, 4, L"pool"))
                continue;
            bool dup = false;
            for (auto &g : seen)
                if (g.first == n.Group)
                {
                    ++g.second;
                    dup = true;
                }
            if (!dup)
                seen.push_back(std::make_pair(n.Group, 1));
        }
        std::wstring got;
        for (auto &g : seen)
            got += (got.empty() ? L"" : L",") + g.first + (g.second > 1 ? L" x" + std::to_wstring(g.second) : std::wstring());
        why = L"replies: " + (got.empty() ? std::wstring(L"none") : got);
        return 0;
    }
    if (k == L"vision")
    {
        const bool on = g_blk && g_blk->EiVisionOn;
        why = on ? L"vision on" : L"vision off";
        return (on ? L"1" : L"0") == v ? 1 : 0;
    }
    if (k == L"log" || k == L"aim")
    {
        int all = 1;
        std::wstring acc; // 2026-10-16: every slot's text, not just the last one's
        for (auto &sn : g_gt.S.empty() ? std::vector<GtSlotSnap>(1) : g_gt.S)
        {
            std::wstring t = v;
            const size_t p = t.find(L"{s}");
            if (p != std::wstring::npos)
                t.replace(p, 3, std::to_wstring(sn.Slot));
            if (k == L"log") // checks are split on spaces: "_" stands for one in a log text
                for (auto &ch : t)
                    if (ch == L'_')
                        ch = L' ';
            const std::string nt = GtNarrow(t);
            if (k == L"log")
            {
                if (g_gt.Log.find(nt) == std::string::npos)
                {
                    const std::wstring miss = L"log has no \"" + t + L"\"";
                    if (acc.find(miss) == std::wstring::npos)
                        acc += (acc.empty() ? L"" : L", ") + miss;
                    all = 0;
                }
                continue;
            }
            // aim: the slot's direct-fire volleys
            const std::string key = "direct fire, slot " + std::to_string(sn.Slot) + " - STARTED";
            int n = 0, hit = 0;
            size_t at = 0;
            while ((at = g_gt.Log.find(key, at)) != std::string::npos)
            {
                const size_t e = g_gt.Log.find('\n', at);
                const std::string line = g_gt.Log.substr(at, (e == std::string::npos ? g_gt.Log.size() : e) - at);
                ++n;
                hit += line.find(nt) != std::string::npos;
                at = e == std::string::npos ? g_gt.Log.size() : e;
            }
            swprintf_s(b, L"slot %d: %d of %d volleys at \"%ls\"", sn.Slot, hit, n, t.c_str());
            acc += (acc.empty() ? L"" : L", ") + std::wstring(b);
            if (!final)
                all = 0;
            else if (n == 0 || hit * 10 < n * 6)
                all = -1;
        }
        if (!acc.empty())
            why = acc;
        return all;
    }
    if (k == L"noorder")
    {
        if (g_ls)
            for (uint32_t n = g_gt.EvHead; n != g_ls->EventHead; ++n)
            {
                const LsEvent &e = g_ls->Events[n % kLsEventRing];
                if (e.Seq == n + 1 && e.Type == LSE_OrderReceived)
                {
                    swprintf_s(b, L"slot %d got order %d", (int)e.Slot, (int)e.Value);
                    why = b;
                    return -1;
                }
            }
        return final ? 1 : 0;
    }
    if (g_gt.S.empty())
    {
        why = L"no lancemate to check";
        return -1;
    }
    int all = 1;
    std::wstring bad; // 2026-10-16: every failing slot is reported (was: the last one only)
    for (auto &sn : g_gt.S)
    {
        const int s = sn.Slot;
        LsSlot ls = {};
        const bool haveLs = GtLsSlot(s, ls);
        VanillaExportBlock::TacMember m = {};
        const bool haveM = GtMember(s, m);
        int r = 0;
        if (k == L"order")
        {
            const int code = GtOrderCode(v);
            std::wstring got;
            if (g_ls)
                for (uint32_t n = g_gt.EvHead; n != g_ls->EventHead; ++n)
                {
                    const LsEvent &e = g_ls->Events[n % kLsEventRing];
                    if (e.Seq != n + 1 || e.Type != LSE_OrderReceived || e.Slot != s)
                        continue;
                    if ((int)e.Value == code)
                        r = 1;
                    got += L" " + std::to_wstring((int)e.Value);
                }
            swprintf_s(b, L"slot %d orders seen:%ls", s, got.empty() ? L" none" : got.c_str());
        }
        else if (k == L"mode")
        {
            const uint16_t f = haveLs ? ls.Mode : 0;
            const uint16_t want = v == L"hold" ? LSM_Hold : v == L"fallback" ? LSM_FallBack : v == L"indirect" ? LSM_IndirectHold
                                : v == L"attack" ? LSM_AttackOrder : v == L"movefire" ? LSM_MoveFire
                                : v == L"flank" ? LSM_Flank : v == L"pincer" ? LSM_Pincer : 0; // movefire: 2026-10-13; flank / pincer: 2026-10-20
            sn.ModeSeen |= f; // 2026-10-16: latched - a mode that came and went between polls still passes ('free': live)
            r = want ? ((sn.ModeSeen & want) != 0) : ((f & (LSM_Hold | LSM_FallBack | LSM_IndirectHold)) == 0);
            swprintf_s(b, L"slot %d mode 0x%03x (seen 0x%03x)", s, (unsigned)f, (unsigned)sn.ModeSeen);
        }
        else if (k == L"disc" || k == L"heat" || k == L"mask")
        {
            const int got = !haveLs ? -1 : k == L"disc" ? ls.FireDisc : k == L"heat" ? ls.FireHeat : ls.FireMask;
            r = got == _wtoi(v.c_str());
            swprintf_s(b, L"slot %d %ls %d", s, k.c_str(), got);
        }
        else if (k == L"target" || k == L"class")
        {
            TerrainMechInfo t = {};
            const bool tk = haveLs && (ls.TargetLo || ls.TargetHi) && MechInfoOf(ls.TargetLo, ls.TargetHi, t, false);
            const std::wstring nm = tk ? UnitWord(t) : std::wstring(L"none");
            if (k == L"class")
                r = tk && t.WeightClass == _wtoi(v.c_str());
            else if (v == L"lock")
                r = haveLs && (g_gt.LockLo || g_gt.LockHi) && ls.TargetLo == g_gt.LockLo && ls.TargetHi == g_gt.LockHi;
            else
                r = tk && GtLower(nm).find(GtLower(v)) != std::wstring::npos;
            swprintf_s(b, L"slot %d target %ls (class %d)", s, nm.c_str(), tk ? (int)t.WeightClass : -1);
        }
        else if (k == L"move" || k == L"walk")
        {
            if (!haveM || m.LastMoveSeq == sn.MoveSeq || !m.LastMoveSeq)
                swprintf_s(b, L"slot %d: no new move point", s);
            else
            {
                const float dx = m.LastMove[0] - m.Loc[0], dy = m.LastMove[1] - m.Loc[1];
                const float dNow = std::sqrt(dx * dx + dy * dy);
                if (sn.WalkD0 < 0)
                    sn.WalkD0 = dNow;
                if (dNow < 4000.0f || dNow < sn.WalkD0 - 1500.0f)
                    sn.WalkOk = true;
                if (k == L"walk")
                {
                    r = sn.WalkOk ? 1 : 0;
                    swprintf_s(b, L"slot %d: %.0f m to its point (was %.0f m)", s, dNow / 100.0f, sn.WalkD0 / 100.0f);
                }
                else
                {
                    const size_t at = v.find(L'@');
                    const float wantM = (float)_wtof(v.substr(0, at).c_str());
                    std::wstring ref = at == std::wstring::npos ? L"c0" : v.substr(at + 1);
                    const bool your = ref.compare(0, 4, L"your") == 0, my = ref.compare(0, 2, L"my") == 0;
                    const float deg = (float)_wtof(ref.substr(your ? 4 : my ? 2 : 1).c_str());
                    const float *o = your ? sn.Loc : g_gt.MeLoc;
                    const float base = your ? sn.Yaw : my ? CockpitYaw(g_gt.MeYaw) : 0.0f;
                    const float px = m.LastMove[0] - o[0], py = m.LastMove[1] - o[1];
                    const float gotM = std::sqrt(px * px + py * py) / 100.0f;
                    const float gotA = std::atan2(py, px) * 57.29578f;
                    const float wantA = base + deg;
                    r = GtAngDiff(gotA, wantA) <= 20.0f && gotM >= wantM * 0.7f && gotM <= wantM * 1.3f + 30.0f;
                    swprintf_s(b, L"slot %d: point %.0f m at %.0f deg (wanted %.0f m at %.0f deg)", s, gotM,
                               std::fmod(gotA + 720.0f, 360.0f), wantM, std::fmod(wantA + 720.0f, 360.0f));
                }
            }
        }
        else if (k == L"face")
        {
            float xy[2];
            // 2026-10-16: sampled only while it is falling back (before / after, it may face anywhere)
            if (haveM && haveLs && (ls.Mode & LSM_FallBack) && (ls.TargetLo || ls.TargetHi) && GtUnitXY(ls.TargetLo, ls.TargetHi, xy) &&
                GetTickCount64() - g_gt.StartMs > 3000)
            {
                const float brg = std::atan2(xy[1] - m.Loc[1], xy[0] - m.Loc[0]) * 57.29578f;
                ++sn.FaceN;
                sn.FaceOk += GtAngDiff(m.Yaw, brg) <= 75.0f;
            }
            if (!final)
                r = 0;
            else
                r = sn.FaceN < 4 ? 1 : (sn.FaceOk * 10 >= sn.FaceN * 6 ? 1 : -1);
            swprintf_s(b, L"slot %d faced its target %d of %d samples%ls", s, sn.FaceOk, sn.FaceN,
                       sn.FaceN < 4 ? L" (no target to face - not judged)" : L"");
        }
        else
        {
            swprintf_s(b, L"unknown check %ls", k.c_str());
            r = -1;
        }
        if (r != 1)
        {
            bad += (bad.empty() ? L"" : L", ") + std::wstring(b);
            all = r < 0 ? -1 : (all < 0 ? -1 : 0);
        }
        else if (why.empty())
            why = b;
    }
    if (!bad.empty())
        why = bad;
    return all;
}

static void GtReport(const GtTest &t, const wchar_t *res, const std::wstring &why)
{
    const double secs = g_gt.StartMs ? (GetTickCount64() - g_gt.StartMs) / 1000.0 : 0.0;
    const std::wstring &tx = g_gt.CurText.empty() ? t.Text : g_gt.CurText;
    Log(L"game test %d: %ls - \"%ls\" [%ls] %ls", t.Row, res, tx.c_str(), t.Checks.c_str(), why.c_str());
    if (g_gt.Rep)
    {
        fwprintf(g_gt.Rep, L"%d\t%ls\t%ls\t%ls\t%ls\t%.1f\t%ls\n", t.Row, t.Who.c_str(), tx.c_str(), t.Checks.c_str(), res, secs,
                 why.c_str());
        fflush(g_gt.Rep);
    }
}

static bool GtLoad()
{
    const std::wstring f = g_gtFile.empty() ? ExeDir() + L"GameTest.tsv" : g_gtFile;
    std::vector<std::wstring> lines;
    if (!RtReadLines(f, lines))
    {
        Log(L"game test: cannot read %ls", f.c_str());
        return false;
    }
    int row = 0;
    for (auto &l : lines)
    {
        ++row;
        const std::wstring t = GtTrim(l);
        if (t.empty() || t[0] == L'#' || t.compare(0, 3, L"who") == 0)
            continue;
        const auto c = GtSplit(t, L'\t');
        GtTest x;
        x.Row = row;
        x.Who = GtTrim(c[0]);
        x.Text = c.size() > 1 ? GtTrim(c[1]) : L"";
        x.Checks = c.size() > 2 ? GtTrim(c[2]) : L"";
        x.Timeout = c.size() > 3 && !GtTrim(c[3]).empty() ? _wtoi(GtTrim(c[3]).c_str()) : 15;
        g_gt.T.push_back(x);
    }
    const std::wstring rp = ExeDir() + L"GameTest_report.tsv";
    g_gt.Rep = _wfopen(rp.c_str(), L"w, ccs=UTF-8");
    if (g_gt.Rep)
        fwprintf(g_gt.Rep, L"row\twho\tutterance\tchecks\tresult\tseconds\tdetails\n");
    Log(L"game test: %zu lines from %ls - report %ls", g_gt.T.size(), f.c_str(), rp.c_str());
    return true;
}

static bool GtLockNow(uint32_t &lo, uint32_t &hi)
{
    std::vector<TerrainMechInfo> ms;
    MechInfosAll(ms);
    for (auto &m : ms)
        if (m.Locked && !m.Dead)
        {
            lo = m.ActorLo;
            hi = m.ActorHi;
            return true;
        }
    lo = hi = 0;
    return false;
}

static void GtStatus(const wchar_t *state, const std::wstring &more)
{
    const std::wstring f = ExeDir() + L"GameTest_status.txt";
    FILE *o = _wfopen(f.c_str(), L"w, ccs=UTF-8");
    if (!o)
        return;
    const int row = g_gt.I < g_gt.T.size() ? g_gt.T[g_gt.I].Row : -1;
    fwprintf(o, L"%ls row %d (line %zu of %zu) pass %d fail %d skip %d restarts %d %ls\n", state, row, g_gt.I, g_gt.T.size(),
             g_gt.Pass, g_gt.Fail, g_gt.Skip, g_gt.Restarts, more.c_str());
    fclose(o);
}

// Who is down: you or a lancemate dead, or a lancemate missing (fewer than at the start). Empty = everyone up.
static std::wstring GtDown(int *mates = nullptr)
{
    VanillaExportBlock::TacMember m[VanillaExportBlock::kTacMaxMembers] = {};
    const int n = ReadLance(m);
    int cnt = 0;
    bool me = false;
    std::wstring down;
    for (int i = 0; i < n; ++i)
    {
        if (!m[i].Valid)
            continue;
        if (m[i].IsPlayer)
        {
            me = true;
            if (m[i].Dead)
                down += L" you";
            continue;
        }
        ++cnt;
        if (m[i].Dead)
            down += L" " + std::to_wstring(m[i].Slot);
    }
    // 2026-10-16: TacMember.Dead stays 0 for a lancemate whose wreck is still there (three dead Adders went
    // unnoticed) - the lance status feed's Alive is the truth when it is up.
    {
        LsSlot ls[kLsMaxSlots];
        const int nl = LsConnect() ? LsReadSlots(ls) : 0;
        for (int i = 0; i < nl; ++i)
            if (ls[i].Valid && !ls[i].Alive)
            {
                const std::wstring who = ls[i].IsPlayer ? L" you" : L" " + std::to_wstring(ls[i].Slot);
                if (down.find(who) == std::wstring::npos)
                    down += who + L" (dead)";
            }
    }
    if (mates)
        *mates = cnt;
    if (!me)
        down += L" (no player in the lance table)";
    if (g_gt.MateCount > 0 && cnt < g_gt.MateCount)
        down += L" (lancemates " + std::to_wstring(cnt) + L" of " + std::to_wstring(g_gt.MateCount) + L")";
    return down;
}

// 2026-10-16: the lance status feed must be LIVE - ejected / dead / paused, LanceControl stops publishing and the
// block freezes with everyone 'alive' (GtDown then sees nobody down). Empty = live.
static std::wstring GtStale(ULONGLONG now)
{
    if (LsFresh(4000))
        return L"";
    if (!g_ls)
        return L" (no lance status feed)";
    const ULONGLONG t = g_ls->TickMs; // read once (the plugin writes it ~4x/s)
    if (!g_ls->Enabled || !t)
        return L" (lance status feed not live)";
    wchar_t b[96];
    swprintf_s(b, L" (lance status feed frozen %.0f s - out of the mech or paused)", now > t ? (now - t) / 1000.0 : 0.0);
    return b;
}

// A command for TestPilot (its TestPilot.cmd) - the "! pilot" directive, and the lock retry (2026-10-16).
static bool GtPilotCmd(const std::wstring &c)
{
    const std::wstring cmd = ExeDir() + L"..\\..\\test_pilot\\TestPilot.cmd";
    FILE *f = _wfopen(cmd.c_str(), L"wb");
    const bool ok = f != nullptr;
    if (f)
    {
        const std::string s = GtNarrow(c) + "\r\n";
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
    Log(L"game test: TestPilot <- %ls%ls", c.c_str(), ok ? L"" : L" (could not write TestPilot.cmd)");
    return ok;
}

// The section the line belongs to starts at the last "! section" before it.
static size_t GtSectionStart(size_t i)
{
    for (size_t k = i + 1; k-- > 0;)
        if (k < g_gt.T.size() && g_gt.T[k].Who == L"!" && g_gt.T[k].Text.compare(0, 7, L"section") == 0)
            return k;
    return 0;
}

void GameTestTick()
{
    if (!g_gtOn)
        return;
    const ULONGLONG now = GetTickCount64();
    if (!g_gt.Started)
    {
        g_gt.Started = true;
        if (!GtLoad())
        {
            g_gtOn = false;
            return;
        }
        g_gt.WaitUntil = now + 3000; // let the links come up
        g_gt.Phase = 2;
        if (g_gtStartRow > 0)
        {
            size_t k = 0;
            while (k < g_gt.T.size() && g_gt.T[k].Row < g_gtStartRow)
                ++k;
            g_gt.ReplayTo = k;
            g_gt.I = GtSectionStart(k);
            Log(L"game test: starting at row %d (directives from row %d first)", g_gtStartRow,
                g_gt.I < g_gt.T.size() ? g_gt.T[g_gt.I].Row : -1);
        }
    }
    // DEATH PAUSE (2026-10-16)
    if (g_gt.Paused)
    {
        int mates = 0;
        // 2026-10-16: + a live feed (a frozen one reads everyone alive)
        const std::wstring d = (g_blk && g_ls && g_ls->Enabled) ? GtDown(&mates) + GtStale(now) : std::wstring(L" (no game link)");
        if (!d.empty() || mates <= 0)
        {
            g_gt.AliveSince = 0;
            if (now - g_gt.StatusMs > 10000)
            {
                g_gt.StatusMs = now;
                GtStatus(L"PAUSED", L"- waiting for a restarted mission:" + d);
            }
            return;
        }
        if (!g_gt.AliveSince)
            g_gt.AliveSince = now;
        if (now - g_gt.AliveSince < 20000)
            return;
        g_gt.Paused = false;
        ++g_gt.Restarts;
        g_gt.ReplayTo = g_gt.ResumeI;
        g_gt.I = GtSectionStart(g_gt.ResumeI);
        g_gt.Phase = 0;
        g_gt.LockTry = (size_t)-1; // a replayed line may ask TestPilot for a lock again
        Log(L"game test: lance back (restart %d) - section directives from row %d, then row %d", g_gt.Restarts,
            g_gt.I < g_gt.T.size() ? g_gt.T[g_gt.I].Row : -1, g_gt.ResumeI < g_gt.T.size() ? g_gt.T[g_gt.ResumeI].Row : -1);
        GtStatus(L"RUNNING", L"- resumed");
        return;
    }
    // 2026-10-16: also during a lock wait (the 90 s approach) - a death there was reported as SKIP
    if ((g_gt.Phase != 2 || g_gt.LockWait) && g_blk && g_ls && g_ls->Enabled && g_gt.I < g_gt.T.size() && g_gt.MateCount > 0)
    {
        const std::wstring d = GtDown() + GtStale(now); // 2026-10-16: a frozen feed (ejected / dead) pauses too
        if (!d.empty())
        {
            g_gt.Paused = true;
            g_gt.LockWait = 0;
            g_gt.ResumeI = g_gt.I; // a line being checked is run again
            g_gt.AliveSince = 0;
            g_gt.StatusMs = now;
            g_gt.Phase = 0;
            Log(L"game test PAUSED at row %d - down:%ls - restart the mission", g_gt.T[g_gt.I].Row, d.c_str());
            GtReport(g_gt.T[g_gt.I], L"PAUSE", L"down:" + d + L" - mission restarted, line run again");
            GtStatus(L"PAUSED", L"- down:" + d);
            return;
        }
    }
    if (g_gt.Phase == 2)
    {
        if (g_gt.LockWait && now < g_gt.WaitUntil)
        {
            uint32_t lo, hi;
            if (!GtLockNow(lo, hi))
                return;
            Log(L"game test: lock acquired");
            g_gt.LockWait = 0;
        }
        else if (now < g_gt.WaitUntil)
            return;
        if (g_gt.LockWait)
            Log(L"game test: no lock after the wait");
        g_gt.LockWait = 0;
        g_gt.Phase = 0;
    }
    if (g_gt.Phase == 0)
    {
        if (!g_blk || !g_ls || !g_ls->Enabled)
        {
            if (now - g_gt.PollMs > 5000)
            {
                g_gt.PollMs = now;
                Log(L"game test: waiting for the game (VanillaHud / LanceControl links)");
            }
            return;
        }
        if (g_gt.I >= g_gt.T.size())
        {
            Log(L"game test DONE: %d passed, %d failed, %d skipped - GameTest_report.tsv", g_gt.Pass, g_gt.Fail, g_gt.Skip);
            GtStatus(L"DONE", L"");
            if (g_gt.Rep)
            {
                fwprintf(g_gt.Rep, L"\t\tTOTAL\t\t%d passed, %d failed, %d skipped\t\t\n", g_gt.Pass, g_gt.Fail, g_gt.Skip);
                fclose(g_gt.Rep);
                g_gt.Rep = nullptr;
            }
            g_gtOn = false;
            return;
        }
        GtTest &t = g_gt.T[g_gt.I];
        if (g_gt.I < g_gt.ReplayTo && t.Who != L"!") // replaying a section's directives after a restart
        {
            ++g_gt.I;
            return;
        }
        if (g_gt.MateCount < 0 && t.Who != L"!")
        {
            // 2026-10-16: the first test line waits for a live mission (a frozen feed: nothing would be checked)
            const std::wstring st = GtStale(now);
            if (!st.empty())
            {
                if (now - g_gt.PollMs > 5000)
                {
                    g_gt.PollMs = now;
                    Log(L"game test: waiting for a live mission before row %d -%ls", t.Row, st.c_str());
                }
                return;
            }
            int mates = 0;
            if (GtDown(&mates).empty() && mates > 0)
            {
                g_gt.MateCount = mates;
                Log(L"game test: %d lancemates - any death pauses the test", mates);
            }
        }
        if (now - g_gt.StatusMs > 5000)
        {
            g_gt.StatusMs = now;
            GtStatus(L"RUNNING", L"");
        }
        if (t.Who == L"!")
        {
            ++g_gt.I;
            const auto w = GtSplit(t.Text, L' ');
            if (w[0] == L"pilot" && t.Text.size() > 6)
            {
                GtPilotCmd(t.Text.substr(6));
                g_gt.WaitUntil = now + 1500;
            }
            else if (w[0] == L"wait" && w.size() > 1)
                g_gt.WaitUntil = now + (ULONGLONG)(_wtof(w[1].c_str()) * 1000.0);
            else if (w[0] == L"lockwait" && w.size() > 1)
            {
                g_gt.WaitUntil = now + (ULONGLONG)(_wtof(w[1].c_str()) * 1000.0);
                g_gt.LockWait = 1;
            }
            else if (w[0] == L"section")
                Log(L"game test: ==== %ls", t.Text.c_str());
            g_gt.Phase = 2;
            return;
        }
        // the lancemates it is for, as they are now
        g_gt.S.clear();
        g_gt.HaveMe = false;
        VanillaExportBlock::TacMember m[VanillaExportBlock::kTacMaxMembers] = {};
        const int n = ReadLance(m);
        for (int i = 0; i < n; ++i)
            if (m[i].Valid && m[i].IsPlayer)
            {
                std::memcpy(g_gt.MeLoc, m[i].Loc, sizeof(g_gt.MeLoc));
                g_gt.MeYaw = m[i].Yaw;
                g_gt.HaveMe = true;
            }
        std::wstring missing;
        if (t.Who != L"-")
            for (int i = 0; i < n; ++i)
            {
                if (!m[i].Valid || m[i].IsPlayer)
                    continue;
                bool want = t.Who == L"all";
                for (auto &x : GtSplit(t.Who, L','))
                    want |= _wtoi(x.c_str()) == m[i].Slot;
                if (!want)
                    continue;
                if (m[i].Dead)
                {
                    missing += L" " + std::to_wstring(m[i].Slot);
                    continue;
                }
                GtSlotSnap sn;
                sn.Slot = m[i].Slot;
                std::memcpy(sn.Loc, m[i].Loc, sizeof(sn.Loc));
                sn.Yaw = m[i].Yaw;
                sn.MoveSeq = m[i].LastMoveSeq;
                g_gt.S.push_back(sn);
            }
        GtLockNow(g_gt.LockLo, g_gt.LockHi);
        g_gt.StartMs = now;
        g_gt.CurText.clear();
        if (!missing.empty() || (t.Who != L"-" && g_gt.S.empty()))
        {
            ++g_gt.Skip;
            ++g_gt.I;
            GtReport(t, L"SKIP", L"lancemate(s) down or missing:" + (missing.empty() ? std::wstring(L" all") : missing));
            return;
        }
        // "lock" / "target=lock" with no lock: 2026-10-16 - once per line TestPilot goes for a mech (up to 90 s for the
        // lock, then the line again); SKIP only when there is still none
        auto lockRetry = [&](const wchar_t *why) -> bool {
            if (g_gt.LockTry == g_gt.I)
                return false;
            g_gt.LockTry = g_gt.I;
            Log(L"game test %d: %ls - TestPilot approaches a mech, up to 90 s for a lock", t.Row, why);
            GtPilotCmd(L"approach 250 mech");
            g_gt.LockWait = 1;
            g_gt.WaitUntil = now + 90000;
            g_gt.Phase = 2;
            return true;
        };
        if (t.Checks.find(L"lock") != std::wstring::npos && !g_gt.LockLo && !g_gt.LockHi) // (target=lock included)
        {
            if (lockRetry(L"needs your lock"))
                return;
            ++g_gt.Skip;
            ++g_gt.I;
            GtReport(t, L"SKIP", L"needs your lock - none");
            return;
        }
        // {enemy}: the nearest hostile mech's name, now
        g_gt.CurText = t.Text;
        g_gt.CurChecks = t.Checks;
        if (t.Text.find(L"{enemy}") != std::wstring::npos || t.Checks.find(L"{enemy}") != std::wstring::npos)
        {
            std::vector<TerrainMechInfo> ms;
            MechInfosAll(ms);
            std::wstring best;
            float bd = 1e30f;
            for (auto &mi : ms)
            {
                float xy[2];
                if (!mi.Hostile || mi.Dead || mi.Kind != TUK_Mech || !GtUnitXY(mi.ActorLo, mi.ActorHi, xy)) // mechs only (2026-10-11)
                    continue;
                const float d = (xy[0] - g_gt.MeLoc[0]) * (xy[0] - g_gt.MeLoc[0]) + (xy[1] - g_gt.MeLoc[1]) * (xy[1] - g_gt.MeLoc[1]);
                if (d < bd)
                {
                    bd = d;
                    best = UnitWord(mi);
                }
            }
            if (best.empty())
            {
                if (lockRetry(L"no hostile mech known for {enemy}"))
                    return;
                ++g_gt.Skip;
                ++g_gt.I;
                GtReport(t, L"SKIP", L"no hostile mech known for {enemy}");
                return;
            }
            // 2026-10-16: in the CHECKS the name's spaces become no-break spaces - checks split on spaces, and
            // "target=Shadow Cat" was "target=Shadow" + "unknown check Cat"; put back before each check runs.
            std::wstring bestTok = best;
            std::replace(bestTok.begin(), bestTok.end(), L' ', L'\u00A0');
            for (size_t p; (p = g_gt.CurText.find(L"{enemy}")) != std::wstring::npos;)
                g_gt.CurText.replace(p, 7, best);
            for (size_t p; (p = g_gt.CurChecks.find(L"{enemy}")) != std::wstring::npos;)
                g_gt.CurChecks.replace(p, 7, bestTok);
        }
        {
            std::lock_guard<std::mutex> lk(g_gtMx);
            g_gtNotes.clear();
        }
        g_gt.EvHead = g_ls->EventHead;
        g_gt.LogOff = GtLogSize();
        g_gt.Log.clear();
        Log(L"game test %d: \"%ls\"", t.Row, g_gt.CurText.c_str());
        HandleSpoken(g_gt.CurText, false, 0.0);
        g_gt.Phase = 1;
        g_gt.PollMs = 0;
        return;
    }
    // checking
    if (now - g_gt.PollMs < 300)
        return;
    g_gt.PollMs = now;
    GtReadLog();
    GtTest &t = g_gt.T[g_gt.I];
    const bool final = now - g_gt.StartMs >= (ULONGLONG)t.Timeout * 1000;
    int res = 1;
    std::wstring why, fails;
    for (auto &tok : GtSplit(g_gt.CurChecks, L' '))
    {
        if (GtTrim(tok).empty())
            continue;
        std::wstring w, tk = GtTrim(tok);
        std::replace(tk.begin(), tk.end(), L'\u00A0', L' '); // a two-word {enemy} (above)
        const int r = GtCheckOne(tk, final, w);
        if (r != 1)
        {
            fails += (fails.empty() ? L"" : L"; ") + tok + L": " + w;
            res = r < 0 ? -1 : (res < 0 ? -1 : 0);
        }
        else
            why += (why.empty() ? L"" : L"; ") + w;
    }
    if (res == 1)
    {
        ++g_gt.Pass;
        ++g_gt.I;
        GtReport(t, L"PASS", why);
        g_gt.Phase = 2;
        g_gt.WaitUntil = now + 1500; // the reply finishes, the next order is not stepped on
    }
    else if (res < 0 || final)
    {
        ++g_gt.Fail;
        ++g_gt.I;
        GtReport(t, L"FAIL", fails + (why.empty() ? L"" : L" | ok: " + why));
        g_gt.Phase = 2;
        g_gt.WaitUntil = now + 1500;
    }
}

} // namespace (IN-GAME TEST)

// VOICE INTENT (2026-10-19): the open questions and the lines waiting to be spoken, every 100 ms.
void ViTick()
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!g_vi || now - last < 100)
        return;
    last = now;
    uint8_t pend[kViSlots] = {};
    std::wstring ptxt[kViSlots];
    if (g_hill.On)
    {
        std::wstring t;
        if (!g_hill.Asked)
            t = L"marking hilltops for you to pick";
        else if (g_hill.NavOnly)
            t = L"no hill there - move to " + NavSaid(g_hill.Letter.empty() ? 0 : g_hill.Letter[0]) + L" anyway? (yes / no)";
        else
        {
            std::vector<std::wstring> names;
            for (int l : g_hill.Letter)
                if (l >= 0)
                    names.push_back(VhudNavName(l));
            t = L"waiting for your hill choice: Nav ";
            for (size_t i = 0; i < names.size(); ++i)
                t += (i == 0 ? L"" : (i + 1 == names.size() ? L" or " : L", ")) + names[i];
        }
        bool all = false;
        for (int w : g_hill.Who)
            all = all || w == 0;
        for (int s = 1; s < kViSlots; ++s)
        {
            bool mine = all || s == g_hill.Speaker;
            for (int w : g_hill.Who)
                mine = mine || w == s;
            if (mine)
            {
                pend[s] = VIP_Hill;
                ptxt[s] = t;
            }
        }
    }
    const ULONGLONG off = g_offerMs;
    if (off && now - off < 60000 && g_offer.Slot >= 1 && g_offer.Slot < kViSlots)
    {
        pend[g_offer.Slot] = VIP_Patch;
        ptxt[g_offer.Slot] = L"has a lock on the " + g_offer.Name + L" - patch it through? (yes / no)";
    }
    uint8_t q[kViSlots] = {};
    for (const auto &j : g_sayQ)
        if (j.Slot >= 1 && j.Slot < kViSlots && q[j.Slot] < 255)
            ++q[j.Slot];
    {
        std::lock_guard<std::mutex> lk(g_spkMx);
        for (const auto &j : g_spkQ)
            if (j && j->Slot >= 1 && j->Slot < kViSlots && q[j->Slot] < 255)
                ++q[j->Slot];
    }
    volatile uint32_t &seq = g_vi->Seq;
    seq = seq + 1;
    MemoryBarrier();
    for (int s = 1; s < kViSlots; ++s)
    {
        ViSlot &v = g_vi->Slots[s];
        if (pend[s] != v.Pending)
            v.PendingMs = pend[s] ? (uint32_t)now : 0;
        v.Pending = pend[s];
        ViCopy(v.PendingText, sizeof(v.PendingText), ptxt[s]);
        v.Queued = q[s];
    }
    g_vi->TickMs = now;
    MemoryBarrier();
    seq = seq + 1;
}

int wmain(int argc, wchar_t **argv)
{
    // VoiceCommand.exe --roundtrip [names file]: Kokoro says, Whisper hears - no game needed (2026-10-10).
    if (argc >= 2 && _wcsicmp(argv[1], L"--parsetest") == 0) // text only, no audio (2026-10-10)
    {
        const int rc = ParseTest(argc >= 3 ? argv[2] : L"", argc >= 4 ? argv[3] : L""); // [file] [g=1 / g=0]
        wprintf(L"\nPress Enter to close.");
        getwchar();
        return rc;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--gametest") == 0) // IN-GAME TEST: then the normal start (2026-10-10)
    {
        g_gtOn = true;
        g_gtFile = argc >= 3 ? argv[2] : L"";
        g_gtStartRow = argc >= 4 ? _wtoi(argv[3]) : 0; // 2026-10-16: start row (after a VoiceCommand restart)
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--roundtrip3") == 0) // the random loop (2026-10-10)
    {
        const int rc = RoundTrip3(argc >= 3 ? argv[2] : L"", argc >= 4 ? argv[3] : L"", argc >= 5 ? _wtoi(argv[4]) : 0,
                                 argc >= 6 ? argv[5] : L"", argc >= 7 ? argv[6] : L"",
                                 argc >= 8 ? argv[7] : L""); // [minutes] [instance] [threads] [sentences file] [k/n] [s=1.25,g=1]
        wprintf(L"\nPress Enter to close.");
        getwchar();
        return rc;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--roundtrip2") == 0) // whole sentences (2026-10-10)
    {
        const int rc = RoundTrip2(argc >= 3 ? argv[2] : L"");
        wprintf(L"\nPress Enter to close.");
        getwchar();
        return rc;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--roundtrip") == 0)
    {
        const int rc = RoundTrip(argc >= 3 ? argv[2] : L"");
        wprintf(L"\nPress Enter to close.");
        getwchar();
        return rc;
    }
    // VoiceCommand.exe --preview <group|all> [count] [lance number]: random lines, no game needed.
    if (argc >= 3 && _wcsicmp(argv[1], L"--preview") == 0)
    {
        LinesTick(true);
        const std::wstring grp = argv[2];
        const int n = argc > 3 ? std::max(1, _wtoi(argv[3])) : 20;
        const int slot = argc > 4 ? _wtoi(argv[4]) : 2;
        if (_wcsicmp(grp.c_str(), L"all") == 0)
        {
            for (auto &g : LineGroups())
                wprintf(L"%-30ls %12.0f\n", g.Key, CountGroup(g.Key, slot));
            return 0;
        }
        const bool wrap = grp.compare(0, 6, L"order_") == 0 || grp.compare(0, 5, L"fire_") == 0 ||
                          grp.compare(0, 5, L"heat_") == 0 || grp.compare(0, 8, L"weapons_") == 0;
        wprintf(L"%ls: about %.0f different lines%ls\n\n", grp.c_str(), CountGroup(grp, slot),
                wrap ? L" (each also wrapped by reply_single / reply_lance)" : L"");
        for (int i = 0; i < n; ++i)
        {
            std::wstring t;
            if (wrap)
                t = Line(L"reply_single", slot, {{L"reply", Line(grp.c_str(), slot, {}, false)}});
            else
                t = Line(grp.c_str(), slot, {{L"part", L"left arm"}, {L"weapon", L"LRM"}, {L"percent", L"30"}, {L"count", L"2"}});
            wprintf(L"  %ls\n", t.c_str());
        }
        return 0;
    }
    SetConsoleTitleW(L"MW5 Voice Command (prototype)");
    LoadConfig();
    LoadCallouts(true); // the callout tick boxes (writes the missing ones once, from the old chatter level)
    if (g_gsMigrated)
        Log(L"settings: copied from VoiceCommand.ini into GlobalSettings.ini - VoiceCommand.ini is not read any more");
    else
        Log(L"settings: GlobalSettings.ini (change them with GlobalSettings.exe)");
    BuildPhrases();
    BuildOrderList();
    LinesTick(true); // VoiceLines.txt: written if missing, then read
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        Log(L"COM failed to start");
        return 1;
    }
    InitVoices();
    StartSpeechWorker(); // Kokoro and the radio filter (VoiceCommand.ini: tts, radio, ...)
    WhisperInit();

    // The in-process recogniser on the default microphone (not the shared Windows Speech
    // Recognition, which would also type and run its own commands).
    ISpRecognizer *rec = nullptr;
    ISpRecoContext *ctx = nullptr;
    ISpRecoGrammar *gram = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_ISpRecognizer, (void **)&rec);
    if (SUCCEEDED(hr))
    {
        ISpObjectTokenCategory *cat = nullptr;
        wchar_t *id = nullptr;
        ISpObjectToken *mic = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void **)&cat)) &&
            SUCCEEDED(cat->SetId(SPCAT_AUDIOIN, FALSE)) && SUCCEEDED(cat->GetDefaultTokenId(&id)) &&
            SUCCEEDED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken, (void **)&mic)) &&
            SUCCEEDED(mic->SetId(nullptr, id, FALSE)))
            hr = rec->SetInput(mic, TRUE);
        else
            hr = E_FAIL;
        if (id)
            CoTaskMemFree(id);
        if (cat)
            cat->Release();
        if (mic)
            mic->Release();
    }
    if (SUCCEEDED(hr))
        hr = rec->CreateRecoContext(&ctx);
    if (SUCCEEDED(hr))
        hr = ctx->SetNotifyWin32Event();
    if (SUCCEEDED(hr))
        hr = ctx->SetInterest(SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_FALSE_RECOGNITION),
                              SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_FALSE_RECOGNITION));
    if (SUCCEEDED(hr))
    {
        // Keep each utterance's audio (16 kHz 16-bit mono) for Whisper; not fatal if refused.
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = 16000;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = 32000;
        if (FAILED(ctx->SetAudioOptions(SPAO_RETAIN_AUDIO, &SPDFID_WaveFormatEx, &wf)))
            Log(L"free speech: the recogniser will not keep audio - Whisper gets nothing");
    }
    if (SUCCEEDED(hr))
        hr = ctx->CreateGrammar(1, &gram);
    SPSTATEHANDLE rule = nullptr;
    if (SUCCEEDED(hr))
        hr = gram->GetRule(L"orders", 1, SPRAF_TopLevel | SPRAF_Active, TRUE, &rule);
    if (SUCCEEDED(hr))
        for (auto &p : g_phrases)
            gram->AddWordTransition(rule, nullptr, p.first.c_str(), L" ", SPWT_LEXICAL, 1.0f, nullptr);
    if (SUCCEEDED(hr))
        hr = gram->Commit(0);
    if (SUCCEEDED(hr))
        hr = gram->SetRuleState(nullptr, nullptr, SPRS_ACTIVE);
    if (FAILED(hr))
    {
        Log(L"speech recognition could not start (0x%08lx) - is a microphone connected, and a Windows speech "
            L"recogniser installed for your language (Settings > Time & language > Speech)?", (unsigned long)hr);
        return 1;
    }
    if (g_cfg.Listen == 2 && !g_cfg.PttVk)
        g_cfg.Listen = 0; // "both" without a key: always
    if (g_cfg.Listen == 1 && !g_cfg.PttVk)
        Log(L"push-to-talk has no key yet - set one in GlobalSettings (VOICE COMMAND > Push-to-talk key). Nothing is heard until then.");
    if (g_cfg.Recognizer == 1 && !WhisperReady())
        Log(L"recognizer = whisper, but Whisper is not running - the Windows recogniser (fixed phrases) is used");
    Log(L"recognizer: %ls - %zu orders. Say e.g. \"two form on me\", \"lance attack my target\", \"three PPCs only\".",
        g_cfg.Recognizer == 1 && WhisperReady() ? L"Whisper" : L"Windows (fixed phrases)", g_orders.size());
    // VoiceCommand_heard.log keeps every session (appended); each one starts with a marker.
    // 2026-10-19 (user): the heard log starts fresh at every VoiceCommand start (no backup) - it had grown to 114 MB.
    if (!g_quiet)
    {
        FILE *hf = nullptr;
        if (_wfopen_s(&hf, (ExeDir() + L"VoiceCommand_heard.log").c_str(), L"wb") == 0 && hf)
            fclose(hf);
    }
    HeardLog(std::wstring(L"==================== VoiceCommand started - recognizer ") +
             (g_cfg.Recognizer == 1 && WhisperReady() ? L"Whisper" : L"Windows") + L", listen " +
             (g_cfg.Listen == 0 ? L"always" : (g_cfg.Listen == 1 ? L"ptt" : L"both")) + L" ====================");
    Log(L"listening: %ls", g_cfg.Listen == 0 ? L"always" : (g_cfg.Listen == 1 ? L"only while the push-to-talk key is held"
                                                                              : L"always; with the push-to-talk key held the callsign may be left out"));

    const HANDLE ev = ctx->GetNotifyEventHandle();
    ULONGLONG lastTry = 0, pttUp = 0;
    bool pttWas = false;
    // GlobalSettings shows "VoiceCommand: running" (and lets its TEST buttons work) while this exists - made
    // only now, with voices and recognition loaded, so a TEST is never sent to a VoiceCommand still starting.
    static HANDLE s_running = CreateMutexW(nullptr, FALSE, L"Local\\MW5_VoiceCommand");
    (void)s_running;
    for (;;)
    {
        const ULONGLONG now = GetTickCount64();
        if (!g_blk && now - lastTry > 1000)
        {
            lastTry = now;
            Connect();
        }
        else if (g_blk && g_blk->Version != VHUD_VERSION)
            Connect();
        if (g_cfg.PttVk)
        {
            const bool down = (GetAsyncKeyState(g_cfg.PttVk) & 0x8000) != 0;
            if (pttWas && !down)
                pttUp = now;
            pttWas = down;
        }
        SeenTick();         // where every hostile was last seen (lance status v4)
        CalloutTick();
        ShadowTick();       // "found it" for a lancemate sent to shadow a unit
        HillTick();         // HIGH GROUND: "which hill?" once the marks show, and its 30 s
        ViOpen();           // VOICE INTENT (2026-10-19): MW5_VoiceIntent for V-LNC (once)
        ViTick();
        SweepTick();        // TARGET CLASSES: the next turret / tank / VTOL
        SituationTick();    // game-state callouts (who hit whom, contacts, targets) - build 2026-10-05i
        RunSpokenActions(); // what Whisper understood
        GameTestTick();     // --gametest: the next test order, and the checks of the last (2026-10-10)
        SayTick();          // the next line to speak, when the last one is done
        LinesTick();        // VoiceLines.txt re-read when saved
        PronTick();         // Pronunciations.txt, the same way
        HeardAsTick();      // HeardAs.txt (what Whisper mishears), the same way
        PilotNamesTick();   // the lancemates' pilot names, as LidarExport sees them
        VcSettingsTick();   // GlobalSettings.ini / PilotVoices.txt changes, and the lance switches to the plugin
        if (WaitForSingleObject(ev, 50) != WAIT_OBJECT_0)
            continue;
        SPEVENT e;
        ULONG got = 0;
        while (ctx->GetEvents(1, &e, &got) == S_OK && got)
        {
            if (g_gtOn) // --gametest: the orders come from the file, not the microphone (the event is still freed)
            {
                if (e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam)
                    reinterpret_cast<IUnknown *>(e.lParam)->Release();
                else if ((e.elParamType == SPET_LPARAM_IS_POINTER || e.elParamType == SPET_LPARAM_IS_STRING) && e.lParam)
                    CoTaskMemFree(reinterpret_cast<void *>(e.lParam));
                continue;
            }
            // The push-to-talk key held (or just let go) for this utterance, and whether the listen
            // mode lets it through at all.
            const bool pttHeld = g_cfg.PttVk && (pttWas || GetTickCount64() - pttUp < 600);
            const bool listenOk = g_cfg.Listen != 1 || pttHeld;
            // recognizer = whisper: Windows only cuts the speech into utterances - every one of them,
            // matched or not, goes to Whisper, and its words become the order.
            const bool viaWhisper = g_cfg.Recognizer == 1 && WhisperReady();
            if (viaWhisper && (e.eEventId == SPEI_FALSE_RECOGNITION || e.eEventId == SPEI_RECOGNITION) &&
                e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam)
            {
                if (listenOk)
                    WhisperQueue(reinterpret_cast<ISpRecoResult *>(e.lParam), true, pttHeld);
            }
            // ---- WINDOWS RECOGNISER (the fallback: recognizer = windows, or Whisper not running) ----
            else if (e.eEventId == SPEI_FALSE_RECOGNITION && e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam)
            {
                // Speech that matched no fixed phrase: to Whisper, printed only.
                if (listenOk)
                    WhisperQueue(reinterpret_cast<ISpRecoResult *>(e.lParam), false, pttHeld);
            }
            else if (e.eEventId == SPEI_RECOGNITION && e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam)
            {
                ISpRecoResult *r = reinterpret_cast<ISpRecoResult *>(e.lParam);
                SPPHRASE *ph = nullptr;
                wchar_t *text = nullptr;
                if (SUCCEEDED(r->GetPhrase(&ph)) && ph && SUCCEEDED(r->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &text, nullptr)) && text)
                {
                    const int conf = ph->Rule.Confidence; // -1 low, 0 normal, 1 high
                    std::wstring t(text);
                    for (auto &ch : t)
                        ch = (wchar_t)towlower(ch);
                    const bool pttOk = listenOk;
                    auto it = g_phrases.end();
                    for (auto i = g_phrases.begin(); i != g_phrases.end(); ++i)
                        if (_wcsicmp(i->first.c_str(), t.c_str()) == 0)
                            it = i;
                    Log(L"heard \"%ls\" (confidence %d)%ls", text, conf, !pttOk ? L" - push-to-talk not held, ignored"
                                                                                : (conf + 1 < g_cfg.MinConf ? L" - too unsure, ignored" : L""));
                    if (pttOk && conf + 1 >= g_cfg.MinConf && it != g_phrases.end())
                    {
                        if (!g_blk)
                            Say(0, Line(L"problem_no_link", 0));
                        else
                            Execute(it->second);
                    }
                }
                if (text)
                    CoTaskMemFree(text);
                if (ph)
                    CoTaskMemFree(ph);
            }
            if (e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam)
                reinterpret_cast<IUnknown *>(e.lParam)->Release();
            else if ((e.elParamType == SPET_LPARAM_IS_POINTER || e.elParamType == SPET_LPARAM_IS_STRING) && e.lParam)
                CoTaskMemFree(reinterpret_cast<void *>(e.lParam));
        }
    }
}
