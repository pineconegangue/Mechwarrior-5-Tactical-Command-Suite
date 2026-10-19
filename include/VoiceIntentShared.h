// VoiceIntentShared.h - MW5_VoiceIntent (2026-10-19, user): what VoiceCommand told each lancemate, the
// question a lancemate is waiting on you to answer, and who is on the radio (a loudness trace for a
// waveform). VoiceCommand writes it; HUDDisplay's V-LNC page reads it. A block of its own: nothing in
// MW5_VanillaHUDExport / MW5_LanceStatus changed.
//
// TWO WRITERS, separate fields:
//  - VoiceCommand's main thread: TickMs, and per slot IntentMs / Intent / PendingMs / Pending / PendingText /
//    Queued, all under the seqlock Seq (odd while writing).
//  - VoiceCommand's playback thread: per slot Speaking / WaveLo / WaveHi / WaveHead only, outside the seqlock (single
//    bytes - a torn read is at worst one stale sample of a picture).
#pragma once
#include <cstdint>

#define VOICE_INTENT_MMF_NAME L"MW5_VoiceIntent"
static constexpr uint32_t VOICE_INTENT_VERSION = 3; // v3: a peak waveform - min / max sample per 11 ms column (was an RMS envelope)
static constexpr int kViSlots = 9;  // index = the lance number (1..8); 0 unused
static constexpr int kViWave = 144; // waveform columns, one per 11 ms of audio (1.6 s)

enum ViPendingKind : uint8_t
{
    VIP_None = 0,
    VIP_Hill,   // "which hill?" (or "move there anyway?") - waiting for your pick
    VIP_Patch,  // offered to patch its lock through to you - waiting for yes / no
    VIP_Other,
};

struct ViSlot
{
    uint32_t IntentMs;        // low 32 bits of GetTickCount64 when the order was given; 0 = none
    uint32_t PendingMs;       // when the question was asked; 0 = none
    uint8_t  Pending;         // ViPendingKind
    uint8_t  Queued;          // its lines waiting to be spoken
    uint8_t  Speaking;        // playback thread: 1 = its transmission is on the air
    uint8_t  WaveHead;        // playback thread: the next column's index in WaveLo / WaveHi
    int8_t   WaveLo[kViWave]; // playback thread: the lowest sample of the column, -127..0 (a ring)
    int8_t   WaveHi[kViWave]; // playback thread: the highest, 0..127
    char     Intent[96];      // UTF-8: "to Nav Alpha (fire on the move)", "called shot: left torso on the Atlas"
    char     PendingText[96]; // UTF-8: "waiting for your hill choice: Nav B, C or D"
};

struct VoiceIntentBlock
{
    uint32_t Version;  // VOICE_INTENT_VERSION
    uint32_t Seq;      // seqlock over the main-thread fields
    uint64_t TickMs;   // GetTickCount64 at the last main-thread update (stale after ~3 s = VoiceCommand gone)
    ViSlot   Slots[kViSlots];
};
static_assert(sizeof(ViSlot) == 12 + kViWave * 2 + 96 * 2, "ViSlot layout changed");
