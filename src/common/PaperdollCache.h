// PaperdollCache.h - where the paperdoll masks live (2026-10-17): extracted from the player's own game files by
// GlobalSettings > PAPERDOLLS into %LOCALAPPDATA%\MW5 Tactical Command Suite\paperdolls. Header-only, so the readers
// (HUDDisplay / Enhanced Imaging through Topo3DView) need neither the extractor nor miniz.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <string>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

inline std::wstring PaperdollCacheDir()
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p)
        out = std::wstring(p) + L"\\MW5 Tactical Command Suite\\paperdolls";
    if (p)
        CoTaskMemFree(p);
    return out;
}

// The last write of <cache>\source.txt (written last by every extraction) - 0 when there is no extracted set.
inline ULONGLONG PaperdollCacheStamp(const std::wstring &dir)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (dir.empty() || !GetFileAttributesExW((dir + L"\\source.txt").c_str(), GetFileExInfoStandard, &d))
        return 0;
    return ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}
