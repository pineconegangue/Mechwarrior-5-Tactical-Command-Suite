// Where GlobalSettings.ini lives, for code running inside the game (plugins): every app of ours writes
// its full path into %APPDATA%\UnrealVRMod\MechWarrior-Win64-Shipping\GlobalSettingsPath.txt (gs::PublishPath).
// The apps folder is that file's folder - paperdolls\ and assets\ sit in it. Header-only, no dependencies
// beyond <windows.h>. (2026-10-08, EnhancedImaging.dll / cameracapture-14)
#pragma once
#include <windows.h>
#include <string>

namespace gsp
{
// bug sweep 22 (2026-10-18): the cached path is shared by every thread of a plugin (CameraCapture reads it on the game
// thread AND the render thread) - it is now read and refilled under a lock. Unlocked, a copy could meet the assignment
// (a torn string, a crash in the game), or see the fresh time stamp with the path still empty (defaults for 10 s).
inline SRWLOCK &IniPathLock()
{
    static SRWLOCK lock = SRWLOCK_INIT;
    return lock;
}
inline std::wstring IniPathLocked(); // below
inline std::wstring IniPath()
{
    AcquireSRWLockExclusive(&IniPathLock());
    std::wstring r = IniPathLocked();
    ReleaseSRWLockExclusive(&IniPathLock());
    return r;
}

// The ini's full path, re-read at most every 10 s; empty when no app has run yet. Call IniPath(), not this.
inline std::wstring IniPathLocked()
{
    static std::wstring path;
    static ULONGLONG    readMs = 0;
    const ULONGLONG     now = GetTickCount64();
    if (readMs && now - readMs < 10000)
        return path;
    readMs = now;
    wchar_t ad[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return path;
    const std::wstring f = std::wstring(ad) + L"\\UnrealVRMod\\MechWarrior-Win64-Shipping\\GlobalSettingsPath.txt";
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return path;
    char b[2048];
    DWORD got = 0;
    if (ReadFile(h, b, sizeof(b) - 1, &got, nullptr) && got)
    {
        const int k = MultiByteToWideChar(CP_UTF8, 0, b, (int)got, nullptr, 0);
        if (k > 0)
        {
            std::wstring p((size_t)k, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, b, (int)got, &p[0], k);
            while (!p.empty() && (p.back() == L'\n' || p.back() == L'\r' || p.back() == L' '))
                p.pop_back();
            path = p;
        }
    }
    CloseHandle(h);
    return path;
}

// The apps folder (the ini's folder), or empty.
inline std::wstring AppsDir()
{
    const std::wstring p = IniPath();
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : p.substr(0, k);
}

inline int Int(const wchar_t *sec, const wchar_t *key, int def)
{
    const std::wstring p = IniPath();
    return p.empty() ? def : (int)GetPrivateProfileIntW(sec, key, def, p.c_str());
}

inline double Num(const wchar_t *sec, const wchar_t *key, double def)
{
    const std::wstring p = IniPath();
    if (p.empty())
        return def;
    wchar_t b[64] = L"";
    GetPrivateProfileStringW(sec, key, L"", b, 64, p.c_str());
    return b[0] ? _wtof(b) : def;
}
inline std::wstring Str(const wchar_t *sec, const wchar_t *key, const wchar_t *def)
{
    const std::wstring p = IniPath();
    if (p.empty())
        return def;
    wchar_t b[1024] = L"";
    GetPrivateProfileStringW(sec, key, def, b, 1024, p.c_str());
    return b;
}
// 2026-10-18 (release prep): the periodic profile / status lines (present profile, lstick / iatick / mechinfo
// profiles, LIDAR scan and projectile summaries, insane-allies "kept" status, units feed, callback rate) only when
// GlobalSettings [debug] verbose_log = 1. Off by default - a long session's log stays small. Read every 5 s.
inline bool VerboseLog()
{
    static ULONGLONG ms = 0;
    static bool      on = false;
    const ULONGLONG  now = GetTickCount64();
    if (!ms || now - ms > 5000)
    {
        ms = now;
        on = Int(L"debug", L"verbose_log", 0) != 0;
    }
    return on;
}
} // namespace gsp
