// TestPilotFire.h - the test pilot's own shared block between TestPilot.exe (requests) and TestPilot.dll (the
// in-game part, fires the player's weapons). Nothing else of the suite maps it.
#pragma once
#include <cstdint>

#define TP_FIRE_MMF_NAME L"MW5_TestPilotFire"
static constexpr uint32_t TP_FIRE_MAGIC   = 0x46505054u; // 'TPPF'
static constexpr uint32_t TP_FIRE_VERSION = 2; // v2 (2026-10-17): the aim point below

#pragma pack(push, 4)
struct TpFireBlock
{
    uint32_t Magic;
    uint32_t Version;
    // TestPilot.exe writes: ReqSeq odd while writing, then even (+2 per request). Secs 0 = cease fire.
    uint32_t ReqSeq;
    float    ReqSecs;      // fire for this long (max 10 s)
    float    ReqHeatCap;   // 0..1 (0 = 0.85)
    float    ReqMaxAimDeg; // aim gate (0 = 4 deg)
    // TestPilot.dll writes:
    uint32_t AckSeq;       // the last request it took
    uint32_t PluginAlive;  // GetTickCount64() / 1000 every frame
    uint32_t State;        // 0 idle, 1 firing, 2 holding (Last says why)
    uint32_t Shots;        // weapon starts, all time
    float    AimErrDeg, DistM, Heat;
    char     Last[160];
    // v2 (testpilot-plugin-3, user 2026-10-17): WHERE THE TORSO SHOULD POINT - the lock's aim point, led by the target's
    // velocity and raised for drop, for the in-range projectile weapon needing the biggest correction (the aim point
    // itself when none). TestPilot.dll writes it every 250 ms while the game has a lock; TestPilot.exe aims the torso at it.
    uint32_t AimSeq;   // +1 per write
    uint32_t AimOn;    // 1 = AimPt is valid (a live lock)
    float    AimPt[3]; // world cm
    float    AimCorrM; // lead + drop correction applied, m
    char     AimWho[48];
};
#pragma pack(pop)
