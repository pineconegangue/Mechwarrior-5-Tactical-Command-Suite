// The folder the art lives in (paperdolls\, assets\), for a host that does not run beside it.
// HUDDisplay leaves it empty and the art is found by walking up from its exe, as always.
// EnhancedImaging.dll (inside the game, 2026-10-08) sets it to the apps folder (GsPointer.h).
#pragma once
#include <string>

inline std::wstring &AssetDirOverride()
{
    static std::wstring dir;
    return dir;
}
