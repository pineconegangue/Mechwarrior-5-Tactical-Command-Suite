#pragma once
// =====================================================================================
// GLOBAL SETTINGS (2026-10-07f). One file, GlobalSettings.ini beside the exes (the build folder, with
// VoiceLines.txt and Pronunciations.txt), for everything that is the same for every window and app:
//   [lance] - LanceControl's switches (insane allies, the game's lancemate voices)
//   [voice] - every VoiceCommand setting (moved here from VoiceCommand.ini, copied over once)
// Edited by GlobalSettings.exe (its UI), HUDDisplay's Esc menu (the two lance switches) or by hand. A
// change is written at once and every program notices within half a second (Watch below) - no SAVE.
// Per-window settings (layout, keys, pages, LIDAR, maps) stay in each HUDDisplay profile's .cfg.
// LanceControl (in the game, where this folder is unknown) finds it through GlobalSettingsPath.txt
// (PublishPath below) and reads [lance] itself (2026-10-07h; the OptFlags relay before that is gone).
// Header-only, C++14 (VoiceCommand), no dependencies beyond Windows.
// =====================================================================================
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace gs
{
// The folder of the running exe (all our exes sit together in the build folder).
inline std::wstring Dir()
{
    wchar_t p[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : s.substr(0, k + 1);
}

inline std::wstring Path() { return Dir() + L"GlobalSettings.ini"; }

// RELEASE (2026-10-08): model paths may be RELATIVE to the exe's folder ("models\ggml-base.en.bin"), so the package
// works wherever it is unpacked; an absolute path (C:\..., \\server\...) is used as it is.
inline std::wstring Resolve(const std::wstring &p)
{
    if (p.empty() || (p.size() > 1 && p[1] == L':') || (p.size() > 1 && p[0] == L'\\' && p[1] == L'\\'))
        return p;
    return Dir() + p;
}
// The defaults the release ships with (beside the exes).
static constexpr const wchar_t *kDefWhisperModel = L"models\\ggml-base.en.bin";
static constexpr const wchar_t *kDefKokoroDir = L"kokoro-82m\\kokoro-multi-lang-v1_0";

inline std::wstring Str(const wchar_t *sec, const wchar_t *key, const wchar_t *def)
{
    wchar_t b[1024] = L"";
    GetPrivateProfileStringW(sec, key, def, b, 1024, Path().c_str());
    return b;
}

inline bool Has(const wchar_t *sec, const wchar_t *key) { return Str(sec, key, L"\x01") != L"\x01"; }

inline int Int(const wchar_t *sec, const wchar_t *key, int def)
{
    const std::wstring v = Str(sec, key, L"");
    return v.empty() ? def : _wtoi(v.c_str());
}

inline double Num(const wchar_t *sec, const wchar_t *key, double def)
{
    const std::wstring v = Str(sec, key, L"");
    return v.empty() ? def : _wtof(v.c_str());
}

inline void Set(const wchar_t *sec, const wchar_t *key, const std::wstring &v)
{
    WritePrivateProfileStringW(sec, key, v.c_str(), Path().c_str());
}

inline void SetInt(const wchar_t *sec, const wchar_t *key, int v) { Set(sec, key, std::to_wstring(v)); }

// A number written as people would type it: 0.35, 1.5, 250 (no trailing zeros).
inline void SetNum(const wchar_t *sec, const wchar_t *key, double v, int decimals)
{
    wchar_t b[64];
    swprintf(b, 64, L"%.*f", decimals < 0 ? 0 : decimals, v);
    std::wstring s = b;
    if (s.find(L'.') != std::wstring::npos)
    {
        while (!s.empty() && s.back() == L'0')
            s.pop_back();
        if (!s.empty() && s.back() == L'.')
            s.pop_back();
    }
    if (s == L"-0")
        s = L"0";
    Set(sec, key, s);
}

// Last write time + size of a file, 0 when there is none: what Watch compares.
inline ULONGLONG FileStamp(const std::wstring &file)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &d))
        return 0;
    const ULONGLONG t = ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    return t ^ ((ULONGLONG)d.nFileSizeLow * 2654435761ull);
}

// "Has the file changed since I last looked?" - looked at no more than every everyMs.
struct Watch
{
    std::wstring File;
    ULONGLONG    Stamp = ~0ull; // the first look always counts as a change
    ULONGLONG    NextMs = 0;
    explicit Watch(const std::wstring &f = Path()) : File(f) {}
    bool Changed(ULONGLONG nowMs, ULONGLONG everyMs = 500)
    {
        if (nowMs < NextMs)
            return false;
        NextMs = nowMs + everyMs;
        const ULONGLONG s = FileStamp(File);
        if (s == Stamp)
            return false;
        Stamp = s;
        return true;
    }
};

// A whole section copied from an older file once (VoiceCommand.ini -> [voice]) when this file has none.
inline bool MigrateSection(const wchar_t *sec, const std::wstring &oldFile)
{
    std::vector<wchar_t> buf(65536, 0);
    if (GetPrivateProfileSectionW(sec, buf.data(), (DWORD)buf.size(), Path().c_str()) > 0)
        return false; // already here
    if (GetFileAttributesW(oldFile.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD n = GetPrivateProfileSectionW(sec, buf.data(), (DWORD)buf.size(), oldFile.c_str());
    if (n == 0)
        return false;
    return WritePrivateProfileSectionW(sec, buf.data(), Path().c_str()) != FALSE;
}

// Where the game's plugins find this file (2026-10-07h): LanceControl runs inside the game and cannot know the
// build folder, so every app of ours writes the full path of GlobalSettings.ini into
// %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\GlobalSettingsPath.txt (UTF-8, one line) when it starts.
// LanceControl reads [lance] straight from that file - no app has to be running.
inline std::wstring PointerFile()
{
    wchar_t b[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return std::wstring();
    return std::wstring(b) + L"\\UnrealVRMod\\MechWarrior-Win64-Shipping\\GlobalSettingsPath.txt";
}

inline void PublishPath()
{
    const std::wstring f = PointerFile();
    if (f.empty())
        return;
    const std::wstring dir = f.substr(0, f.find_last_of(L'\\'));
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // UEVR's game folder is not there: nothing to tell
    const std::wstring p = Path();
    const int k = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), (int)p.size(), nullptr, 0, nullptr, nullptr);
    if (k <= 0)
        return;
    std::string u((size_t)k, '\0');
    WideCharToMultiByte(CP_UTF8, 0, p.c_str(), (int)p.size(), &u[0], k, nullptr, nullptr);
    std::string cur;
    if (HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr))
        if (h != INVALID_HANDLE_VALUE)
        {
            char b[2048];
            DWORD got = 0;
            if (ReadFile(h, b, sizeof(b), &got, nullptr))
                cur.assign(b, got);
            CloseHandle(h);
        }
    if (cur == u)
        return;
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD put = 0;
    WriteFile(h, u.data(), (DWORD)u.size(), &put, nullptr);
    CloseHandle(h);
}

// [lance]: LanceControl's switches. Bits as VanillaHudShared.h's OptFlags (kVhudOptInsaneAllies = 1,
// kVhudOptGameVoices = 2: the game's lancemate voices PLAY - unset means muted).
struct LanceOpts
{
    bool InsaneAllies = false; // any mode but vanilla
    int  AllyMode = 0;         // [lance] insane_allies: 0 vanilla, 1 insane, 2 impossibly strong (2026-10-09)
    bool MuteGameVoices = true;
    void Load()
    {
        AllyMode = Int(L"lance", L"insane_allies", 0);
        AllyMode = AllyMode < 0 ? 0 : (AllyMode > 2 ? 2 : AllyMode);
        InsaneAllies = AllyMode != 0;
        MuteGameVoices = Int(L"lance", L"mute_game_voices", 1) != 0;
    }
    uint32_t Bits() const { return (InsaneAllies ? 1u : 0u) | (MuteGameVoices ? 0u : 2u); }
};

// [hud] compass_*: HUDDisplay's heading compass (2026-10-12) - the strip over the 3D / TacOps pages (in place of
// the game's captured strip), the compass rings on the ground, clock positions, the LIDAR scope's compass ring,
// and the bearing dots. All on by default; each window's CMPS bezel button hides the lot on that window.
struct CompassOpts
{
    bool TacStrip = true, TacRings = true, TacClock = true, LidarRing = true, LidarClock = true, Dots = true;
    void Load()
    {
        TacStrip = Int(L"hud", L"compass_strip", 1) != 0;
        TacRings = Int(L"hud", L"compass_rings", 1) != 0;
        TacClock = Int(L"hud", L"compass_clock", 1) != 0;
        LidarRing = Int(L"hud", L"compass_lidar", 1) != 0;
        LidarClock = Int(L"hud", L"compass_lidar_clock", 1) != 0;
        Dots = Int(L"hud", L"compass_dots", 1) != 0;
    }
};

// [hud] radar_mode (2026-10-12): which hostiles HUDDisplay's LIDAR scope, 3D / TacOps maps and compass dots show.
// 0 = Full LIDAR (everything inside LidarExport's 1000 m scan - the behaviour before this setting existed),
// 1 = Vanilla (only what the game's own detection has: your radar incl. what the team relays, a lancemate's
// radar or line of sight - LanceControl's MW5_LanceStatus Units[] flags). Display only; the game is untouched.
// 2 = Vanilla, 5 km (2026-10-15, user): Vanilla's rules, with HUDDisplay's views and LidarExport's ranges out to 5 km.
struct RadarOpts
{
    int Mode = 0;
    void Load()
    {
        const int m = Int(L"hud", L"radar_mode", 0);
        Mode = (m == 1 || m == 2) ? m : 0;
    }
    bool Vanilla() const { return Mode == 1 || Mode == 2; } // the game's detection rules
    bool Far() const { return Mode == 2; }                  // the 5 km ranges
};
// 2026-10-13 (user): the incoming-missile alarm, moved out of HUDDisplay's per-profile LIDAR settings - one switch and
// one volume for every HUDDisplay window. [hud] alarm_on (1), alarm_vol (0-100, 70).
struct AlarmOpts
{
    bool On = true;
    int  Vol = 70;
    int  Sound = 0; // [hud] alarm_sound: 0 warble, 1 double pip, 2 triple pip, 3 fast warble (ThreatAlarm::SetSound)
    void Load()
    {
        On = Int(L"hud", L"alarm_on", 1) != 0;
        Vol = Int(L"hud", L"alarm_vol", 70);
        if (Vol < 0 || Vol > 100)
            Vol = 70;
        Sound = Int(L"hud", L"alarm_sound", 0);
        if (Sound < 0 || Sound > 4)
            Sound = 0;
    }
};
// 2026-10-13 (user): HUDDisplay's LIDAR scope layers and projectile smoothing, moved out of each window's profile -
// one set for every window. Key i belongs to LidarView layer bit i (kLidarLayerKeys[9] is the alarm: AlarmOpts).
// The scope's RANGE stays a per-window zoom (ZM+ / ZM-).
static const wchar_t *const kLidarLayerKeys[11] = {
    L"lidar_rings", L"lidar_ping", L"lidar_aim", L"lidar_mechs", L"lidar_vehicles", L"lidar_weight",
    L"lidar_target", L"lidar_projectiles", L"lidar_missiles", nullptr /* alarm */, L"lidar_terrain"};
static const int kLidarLayerDefaults[11] = {1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1};
struct LidarOpts
{
    uint32_t Layers = 0; // without the alarm bit
    bool     Smooth = true;
    void Load()
    {
        Layers = 0;
        for (int i = 0; i < 11; ++i)
            if (kLidarLayerKeys[i] && Int(L"hud", kLidarLayerKeys[i], kLidarLayerDefaults[i]) != 0)
                Layers |= 1u << i;
        Smooth = Int(L"hud", L"lidar_smooth", 1) != 0;
    }
};
} // namespace gs
