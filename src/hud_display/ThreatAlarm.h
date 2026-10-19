// ThreatAlarm.h
//
// Incoming-missile warning for HUDDisplay: the audible half of the MWMissilePath lock
// probe. Deliberately self-contained - this whole feature is provisional, and if it does
// not earn its place in the cockpit it should come out by deleting these two files, the
// three threat fields in LidarShared.h, the probe block in LidarExport.cpp, and the
// handful of call sites that mention ThreatAlarm. Nothing else should learn about it.
//
// What it warns about: a homing salvo that is guiding on YOU, right now. Not your own
// outgoing missiles, not salvos tracking a lancemate, and never dumbfire SRMs - those
// carry no lock, so the plugin cannot see them and this cannot warn about them.
//
// All hold and debounce lives HERE, not in the plugin. The plugin publishes instantaneous
// truth; a stalled plugin must never be able to pin the alarm on, which is precisely how
// the previous implementation got stuck sounding forever.

#pragma once

#include <cstdint>

class ThreatAlarm
{
  public:
    ThreatAlarm() = default;
    ~ThreatAlarm();

    ThreatAlarm(const ThreatAlarm &) = delete;
    ThreatAlarm &operator=(const ThreatAlarm &) = delete;

    // Feed every frame, whichever MFD page is showing.
    //   locked    - the caller's sustain signal: true while a bound round of a salvo
    //               tracking us is still alive
    //   nearestCm - range to the NEAREST bound round, in centimetres; negative when there
    //               is none to measure. This is the only input that selects the cadence.
    //               Below kCloseCm the alarm pulses fast, at or above it slow, and it moves
    //               between the two the frame the range crosses - both directions, no
    //               latch, no band. A negative value holds whichever state is current.
    //   dataFresh - false if the publisher has stalled; forces silence immediately
    //   enabled   - the user's layer toggle, and whether this instance owns the alarm
    //   nowMs     - high-resolution clock, shared with the caller
    //
    // A second salvo launching mid-episode deliberately changes NOTHING here. It does not
    // restart the tone and does not shift its phase - a warning that re-cues for an event
    // the pilot is already reacting to reads as a new threat and pulls attention at the
    // wrong moment. The salvo keeps the alarm alive through the caller's `locked`, and if
    // it closes inside kCloseCm the range transition speaks for it.
    void Update(bool locked, float nearestCm, bool dataFresh, bool enabled, double nowMs);

    // Terminal-range threshold, centimetres. Lives here rather than in the caller because
    // the two-state rule is this class's contract: one number, one comparison, no second
    // opinion about what "close" means anywhere else in the program.
    static constexpr float kCloseCm = 25000.0f; // 250 m

    // Pulse cycle currently sounding, ms; 0 when silent.
    int GetPulseMs() const { return PeriodMs; }

    // True while the tone burst itself is sounding, as opposed to the gap after it.
    //
    // Phase comes from waveOutGetPosition - the DEVICE's own playback position, which
    // waveOutReset zeroes at the start of each episode. So this is the actual clock
    // generating the sound rather than an estimate running alongside it: no drift, and the
    // scope's flash lands on the beep instead of near it. Falls back to a free-running
    // wall-clock cycle only if the driver refuses the query.
    bool IsBeeping(double nowMs) const;

    // 0..100. Applied via waveOutSetVolume, so it takes effect on the next buffer with no
    // re-synthesis and nothing to free - a slider drag mid-combat cannot fault.
    void SetVolume(int percent);
    int  GetVolume() const { return VolumePct; }

    // SOUND (2026-10-13, user's pick of the mockups in docs\alarm_mockups): 0 warble (B, 1000/1400 Hz),
    // 1 double pip (E, 1200 Hz), 2 triple pip (J, 1600 Hz; two pips when close), 3 fast warble (N, 1200/1700 Hz at
    // 30 Hz), 4 single beep (the original 880 Hz sine, 2026-10-13). The far / close cadence is the same for all -
    // only the burst changes. Takes effect at once.
    void SetSound(int sound);

    // PREVIEW (GlobalSettings' TEST button, 2026-10-13): sound the far or close cycle now, keep it going with
    // PreviewTick() (every ~50 ms), stop with PreviewStop(). Not for the live alarm - that is Update().
    void Preview(bool isNear);
    void PreviewTick();
    void PreviewStop();
    int  GetSound() const { return Sound; }

    // True while the tone is actually sounding (for an on-screen cue).
    bool IsSounding() const { return Sounding; }

    // Release the device. Safe to call twice; safe if never started.
    void Shutdown();

  private:
    bool EnsureDevice(); // opens waveOut once and keeps it open for the session
    bool Begin(int periodMs, bool isNear);
    int  SynthBurst(short *out, int maxSamples, bool isNear) const; // the selected sound's burst; returns its length
    void Pump();         // re-queue drained buffers; this is what keeps the tone going
    void End();
    void ApplyVolume();

    // The repeat is a queue we refill, NOT WinMM's loop flags - those stopped after about
    // one cycle while the object still believed it was sounding, which is how the alarm
    // became a single short beep per salvo. Four cycles is over a second of buffered audio,
    // so the render loop has ample slack to top it up even through a bad frame hitch.
    static constexpr int kQueueDepth = 4;

    void *Wave        = nullptr;  // HWAVEOUT, opaque so callers need no <windows.h>
    void *Headers[kQueueDepth] = {}; // WAVEHDR*, all pointing at Samples; queue slots
    short *Samples    = nullptr;  // owned PCM buffer, freed only after waveOutReset
    unsigned long SampleBytes = 0;
    int    PeriodMs   = 0;        // pulse cycle currently synthesised (0 = nothing playing)
    int    VolumePct  = 70;
    int    Sound      = 0;        // SetSound
    int    BurstMs    = 110;      // length of the burst now playing (IsBeeping's flash window)
    bool   Sounding   = false;
    bool   DeviceFailed = false;  // waveOutOpen failed once; do not retry every frame
    bool   Near         = false;  // which of the two cadences is playing; cleared at End()

    // Release hold. Arming is immediate - a warning that hesitates is worse than useless.
    //
    // This is now ONLY the fallback for when the caller has no bound rounds to reason
    // about: no projectile layer, or the scope is not the active view. It is deliberately
    // short. Bridging a salvo's whole flight with a timer was the wrong idea - flight time
    // depends on range, and no constant is right for both a 200 m Streak and a 900 m LRM
    // arc. When the array is available the caller holds `locked` true for as long as the
    // salvo's actual rounds are alive, and this debounce never comes into play.
    double LockedSinceMs = 0.0;
    double ClearSinceMs  = 0.0;
    static constexpr double kReleaseMs = 100.0;
};
