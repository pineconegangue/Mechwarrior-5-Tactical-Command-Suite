// =====================================================================================
// HudProfiles.h - HUDDisplay window profiles, shared by HUDDisplay (edit mode: SAVE AS) and
// GlobalSettings (the HUD PROFILES page). 2026-10-20, user.
//
// A profile is MFDDisplay_<name>.cfg beside HUDDisplay.exe; HUDDisplay.exe profile=<name> runs it
// (window title "MW5 MFD Display (<name>)", named mutex Local\MW5_MFDDisplay_<name>). Each profile
// gets a shortcut "HUDDisplay - <name>.lnk" beside the exe. Nothing is ever deleted: a removed
// profile's files go to the _to_delete folder beside the exe.
// =====================================================================================
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

namespace hudprof
{
static constexpr size_t kMaxName = 24;

inline bool NameChar(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
}

// The same rule as HUDDisplay's SetProfile: letters, digits, '-' and '_', at most 24.
inline std::wstring Clean(const std::wstring &s)
{
    std::wstring o;
    for (wchar_t c : s)
    {
        if (c == L' ')
            c = L'_';
        if (NameChar(c) && o.size() < kMaxName)
            o.push_back(c);
    }
    return o;
}

inline std::wstring ExeDir()
{
    wchar_t p[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : s.substr(0, k + 1);
}

inline std::wstring CfgPath(const std::wstring &dir, const std::wstring &name) { return dir + L"MFDDisplay_" + name + L".cfg"; }
inline std::wstring LnkPath(const std::wstring &dir, const std::wstring &name) { return dir + L"HUDDisplay - " + name + L".lnk"; }
inline bool Exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Same name, any case (the file system ignores case).
inline bool SameName(const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// A HUDDisplay window is running that profile (its single-instance mutex exists).
inline bool Running(const std::wstring &name)
{
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, (L"Local\\MW5_MFDDisplay_" + name).c_str());
    if (!m)
        return false;
    CloseHandle(m);
    return true;
}

// Every profile with a config file, sorted ("None" is the pre-profile file, not a profile).
inline std::vector<std::wstring> List(const std::wstring &dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"MFDDisplay_*.cfg").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    do
    {
        std::wstring n = fd.cFileName;
        if (n.size() <= 15)
            continue;
        n = n.substr(11, n.size() - 15); // MFDDisplay_ ... .cfg
        if (!n.empty() && !SameName(n, L"None") && Clean(n) == n)
            out.push_back(n);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

// "HUDDisplay - <name>.lnk" beside the exe: HUDDisplay.exe profile=<name>. Replaces an old one.
inline bool MakeShortcut(const std::wstring &dir, const std::wstring &name)
{
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW *sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl)) && sl)
    {
        const std::wstring exe = dir + L"HUDDisplay.exe";
        sl->SetPath(exe.c_str());
        sl->SetArguments((L"profile=" + name).c_str());
        sl->SetWorkingDirectory(dir.c_str());
        sl->SetDescription((L"MW5 MFD Display - profile " + name).c_str());
        sl->SetIconLocation(exe.c_str(), 0);
        IPersistFile *pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void **)&pf)) && pf)
        {
            ok = SUCCEEDED(pf->Save(LnkPath(dir, name).c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(ci))
        CoUninitialize();
    return ok;
}

inline bool Launch(const std::wstring &dir, const std::wstring &name, HWND owner = nullptr)
{
    const std::wstring exe = dir + L"HUDDisplay.exe", args = L"profile=" + name;
    return (INT_PTR)ShellExecuteW(owner, L"open", exe.c_str(), args.c_str(), dir.c_str(), SW_SHOWNORMAL) > 32;
}

// Moves a file into <dir>_to_delete\ (a numbered name when one is already there). Missing file = done.
inline bool MoveAside(const std::wstring &dir, const std::wstring &path)
{
    if (!Exists(path))
        return true;
    const std::wstring bin = dir + L"_to_delete\\";
    CreateDirectoryW(bin.c_str(), nullptr);
    const size_t k = path.find_last_of(L"\\/");
    const std::wstring file = k == std::wstring::npos ? path : path.substr(k + 1);
    std::wstring to = bin + file;
    for (int i = 2; Exists(to) && i < 1000; ++i)
        to = bin + std::to_wstring(i) + L"_" + file;
    return MoveFileExW(path.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED) != 0;
}
} // namespace hudprof
