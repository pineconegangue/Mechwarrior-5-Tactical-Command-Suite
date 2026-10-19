// PaperdollExtract.h - the paperdoll masks, read from the player's OWN game files (2026-10-17, user).
//
// C++ port of tools_paperdoll_extract.py ("extract" + "canon"): reads the UE4 .pak index, finds every
// paperdoll texture (mech Target folders with <part>_armor / _internal masks, vehicle Paperdoll_ bodies),
// inflates and decodes it, and writes the same greyscale PNG masks into the same layout the shipped
// paperdolls\ folder used: <out>\<chassis>\<part>_<layer>.png, <chassis>\body.png, _generic\<kind>.png,
// manifest.json. Nothing of the game is shipped - each player extracts from their own install
// (GlobalSettings > Paperdolls, only when they press the button).
//
// Portable (Windows + Linux, so it can be checked against the Python tool on Linux). zlib by miniz.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pde
{
struct Result
{
    int                      Paks = 0;       // paks read
    int                      Chassis = 0;    // chassis folders written
    int                      Files = 0;      // masks written
    int                      Bodies = 0;     // body.png made by Canonicalise
    std::vector<std::string> Failures;       // "<pak>: ..." / "<chassis>/<asset>: ..."
};

// Progress callback: (done, total, what) - total = paperdoll assets found in all paks; called from the
// extracting thread. Return false to cancel.
using ProgressFn = std::function<bool(int done, int total, const std::string &what)>;

// Extract every paperdoll mask from the paks (in order; an earlier pak wins a name clash, as the Python
// tool) into outDir, then make body.png per one-body unit and _generic\. outDir is created; existing
// files are overwritten (a rebuild).
bool Extract(const std::vector<std::filesystem::path> &paks, const std::filesystem::path &outDir, Result &res,
             const ProgressFn &progress = nullptr);

// The step "canon" of the Python tool on its own (Extract already calls it).
int Canonicalise(const std::filesystem::path &outDir);

// The paks of an install: <game>\MW5Mercs\Content\Paks\*.pak (base game + DLC), then optionally every
// mod's Paks\*.pak under <game>\MW5Mercs\Mods and the Steam workshop folder of app 784080.
std::vector<std::filesystem::path> FindPaks(const std::filesystem::path &gameDir, bool includeMods);

// Fingerprint of a pak list (path, size, last write per pak) - stored beside the masks after an extraction, so
// GlobalSettings can tell "game or mods changed since" without reading any pak.
// The decoder's version leads the signature (2026-10-19): a set made by an older decoder no longer matches, so
// GlobalSettings asks for a re-extract. Bump kDecoderVersion whenever the pictures an extraction makes change.
// 1 = luminance of DXT5 colour (wrong: blocky dolls), 2 = DXT5 alpha (the real line art).
constexpr int kDecoderVersion = 2;
std::string DecoderTag(); // "decoder 2\n" - the first line of source.txt
std::string PakSignature(const std::vector<std::filesystem::path> &paks);

// True when dir holds MW5Mercs\Content\Paks\MW5Mercs-WindowsNoEditor.pak (the game's main pak).
bool IsGameDir(const std::filesystem::path &dir);

#ifdef _WIN32
// The per-user cache the displays read: %LOCALAPPDATA%\MW5 Tactical Command Suite\paperdolls.
std::filesystem::path CacheDir();
// The game's install folder found through Steam (registry SteamPath + libraryfolders.vdf); empty if not found.
std::filesystem::path FindGameDir();
#endif
} // namespace pde
