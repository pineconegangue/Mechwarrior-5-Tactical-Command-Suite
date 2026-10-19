#pragma once
// =====================================================================================
// DROPSHIP OUTLINES (2026-10-08, TERRAIN v14). The hull shapes HUDDisplay draws under a dropship's turrets:
// assets/dropships/<kind>.txt, made from the game's own meshes by tools_dropship_outline.py. Each line, cm in
// the dropship actor's frame (X forward, Y right, Z up):
//     top   x y x y ...        seen from above - the LIDAR scope
//     slice z x y x y ...      the outline at height z - the 3D map / TacOps
//     side  x z x z ...        the centreline profile - the 3D map / TacOps
// Loaded once, on first use; the folder is found by walking up from the exe (as paperdolls\ is).
// =====================================================================================
#include <windows.h>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include "AssetRoot.h" // EnhancedImaging.dll: the art folder when the host is not beside it

struct DropshipOutline
{
    bool Loaded = false;
    std::vector<std::vector<float>> Top;  // x, y pairs
    struct Slice
    {
        float              Z;
        std::vector<float> Pts;           // x, y pairs
    };
    std::vector<Slice>              Slices;
    std::vector<std::vector<float>> Side; // x, z pairs
};

// kind: TerrainDropshipKind (1 Leopard, 2 Union destructible, 3 Union takeoff, 4 Broadsword, 5 Union base game);
// nullptr for none / not found.
inline const DropshipOutline *DropshipOutlineFor(int kind)
{
    static DropshipOutline outl[6];
    static bool tried[6] = {};
    if (kind < 1 || kind > 5)
        return nullptr;
    if (!tried[kind])
    {
        tried[kind] = true;
        static const wchar_t *kFile[6] = {L"",           L"leopard.txt",    L"union_destructible.txt",
                                          L"union_takeoff.txt", L"broadsword.txt", L"union.txt"};
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        // A host not beside the art (EnhancedImaging.dll, 2026-10-08): its folder first - "x" stands in for
        // the file name the loop strips on its first pass.
        if (!AssetDirOverride().empty())
            dir = AssetDirOverride() + L"\\x";
        for (int up = 0; up < 7; ++up)
        {
            const size_t k = dir.find_last_of(L"\\/");
            if (k == std::wstring::npos)
                break;
            dir = dir.substr(0, k);
            FILE *fp = nullptr; // (a wide path: MSVC's ifstream takes one, mingw's does not)
            if (_wfopen_s(&fp, (dir + L"\\assets\\dropships\\" + kFile[kind]).c_str(), L"rb") != 0 || !fp)
                continue;
            std::string all;
            char buf[4096];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), fp)) > 0)
                all.append(buf, got);
            fclose(fp);
            std::istringstream f(all);
            DropshipOutline &o = outl[kind];
            std::string line;
            while (std::getline(f, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream ss(line);
                std::string tag;
                ss >> tag;
                DropshipOutline::Slice sl{};
                if (tag == "slice")
                    ss >> sl.Z;
                std::vector<float> pts;
                float v;
                while (ss >> v)
                    pts.push_back(v);
                if (pts.size() < 6)
                    continue;
                if (tag == "top")
                    o.Top.push_back(pts);
                else if (tag == "side")
                    o.Side.push_back(pts);
                else if (tag == "slice")
                {
                    sl.Pts = pts;
                    o.Slices.push_back(sl);
                }
            }
            o.Loaded = !o.Top.empty();
            break;
        }
    }
    return outl[kind].Loaded ? &outl[kind] : nullptr;
}
