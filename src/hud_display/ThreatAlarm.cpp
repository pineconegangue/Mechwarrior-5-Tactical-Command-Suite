// ThreatAlarm.cpp - see ThreatAlarm.h.
//
// Audio design notes, because the previous attempts got this wrong three times:
//
//  * PlaySound is process-global and single-channel. Two instances, or one instance and
//    anything else in the process making noise, fought over it. A dedicated waveOut handle
//    is ours alone.
//  * The device is opened ONCE and kept for the session. Opening it per alarm episode put
//    a waveOutOpen on the critical path at the exact moment the warning had to be audible.
//  * Volume is waveOutSetVolume, NOT re-synthesis. An earlier version rebuilt and restarted
//    the sound on every slider tick, which is where a use-after-free lived. The PCM buffer
//    is built once per episode, so dragging the volume slider touches no memory at all.
//  * The repeat is driven by a QUEUE OF BUFFERS WE REFILL, not by WinMM's loop flags.
//    WHDR_BEGINLOOP/WHDR_ENDLOOP with dwLoops=0xFFFFFFFF is supposed to repeat forever;
//    in practice it stopped after roughly one cycle, and because nothing re-checked, the
//    alarm went permanently silent while still believing it was sounding. One short beep
//    per salvo. Queueing several buffers and re-writing each as it completes depends on no
//    driver looping behaviour at all.
//  * The buffer is freed only after waveOutReset has returned and every header has been
//    unprepared. The device can still be reading it right up until then.

#include "ThreatAlarm.h"

#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib") // MSVC; the CMake target links winmm for other toolchains

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr int   kSampleRate = 22050;
constexpr int   kBeepMs     = 110;    // tone burst length
// A5 cuts through engine rumble without being shrill. ONE pitch for everything: the two
// states are distinguished by cadence, so nothing has to be identified by ear.
constexpr float kToneHz = 880.0f;

// The alarm has exactly TWO cadences and no others, selected by the nearest bound round's
// range against kCloseCm. Nothing else - not the salvo count, not the round type - can
// change the sound.
//
// A four-entry salvo-count table used to live here. It was removed because the count is
// jittery by nature: launchers that fire as a stream produce rounds over many publishes, so
// the number of tracked paths genuinely rises and falls mid-flight. Mapping that onto
// cadence made the tone wander for a reason the pilot could not act on. Range is the only
// input that changes what you should DO about an inbound missile, so it is the only input
// that changes what you hear.
constexpr int kPeriodFarMs   = 560; // outside kCloseCm
constexpr int kPeriodCloseMs = 180; // terminal range - reads as a different state instantly
} // namespace

ThreatAlarm::~ThreatAlarm()
{
    Shutdown();
}

void ThreatAlarm::SetVolume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    VolumePct = percent;
    ApplyVolume();
}

void ThreatAlarm::ApplyVolume()
{
    if (!Wave) return;
    // Both channels in one DWORD (low word left, high word right).
    const DWORD lvl = static_cast<DWORD>((VolumePct * 0xFFFF) / 100);
    waveOutSetVolume(reinterpret_cast<HWAVEOUT>(Wave), (lvl << 16) | lvl);
}

// Open the output device once and keep it for the session.
bool ThreatAlarm::EnsureDevice()
{
    if (Wave) return true;
    if (DeviceFailed) return false;

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = kSampleRate;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    HWAVEOUT hwo = nullptr;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        DeviceFailed = true; // latch: retrying every frame would stall the render loop
        return false;
    }
    Wave = hwo;
    ApplyVolume();
    return true;
}

void ThreatAlarm::SetSound(int sound)
{
    if (sound < 0 || sound > 4)
        sound = 0;
    if (sound == Sound)
        return;
    Sound = sound;
    if (Sounding && PeriodMs > 0)
        Begin(PeriodMs, Near); // heard at once, same cadence
}

void ThreatAlarm::Preview(bool isNear)
{
    if (Begin(isNear ? kPeriodCloseMs : kPeriodFarMs, isNear))
        Near = isNear;
}

void ThreatAlarm::PreviewTick()
{
    if (Sounding)
        Pump();
}

void ThreatAlarm::PreviewStop()
{
    if (Sounding)
        End();
    Near = false;
}

// One burst of the selected sound into out[]; the rest of the cycle is the caller's silence.
int ThreatAlarm::SynthBurst(short *out, int maxSamples, bool isNear) const
{
    int n = 0;
    auto ms = [](int m) { return (kSampleRate * m) / 1000; };
    // a sine pip with short ramps (no click per pulse)
    auto pip = [&](float hz, int lenMs) {
        const int len = ms(lenMs), ramp = ms(3) > 0 ? ms(3) : 1;
        for (int i = 0; i < len && n < maxSamples; ++i, ++n)
        {
            float env = 1.0f;
            if (i < ramp) env = (float)i / (float)ramp;
            else if (i >= len - ramp) env = (float)(len - i) / (float)ramp;
            out[n] = (short)(std::sin(6.2831853f * hz * (float)i / (float)kSampleRate) * env * 0.6f * 32767.0f);
        }
    };
    auto gap = [&](int lenMs) { n += ms(lenMs); if (n > maxSamples) n = maxSamples; };
    // a two-tone warble: the frequency flips between fc-dev and fc+dev rate times a second, phase kept continuous
    auto warble = [&](float fc, float dev, float rate, int lenMs) {
        const int len = ms(lenMs), ramp = ms(3) > 0 ? ms(3) : 1;
        float ph = 0.0f;
        for (int i = 0; i < len && n < maxSamples; ++i, ++n)
        {
            const float t = (float)i / (float)kSampleRate;
            const float f = fc + (std::sin(6.2831853f * rate * t) >= 0.0f ? dev : -dev);
            ph += 6.2831853f * f / (float)kSampleRate;
            float env = 1.0f;
            if (i < ramp) env = (float)i / (float)ramp;
            else if (i >= len - ramp) env = (float)(len - i) / (float)ramp;
            out[n] = (short)(std::sin(ph) * env * 0.6f * 32767.0f);
        }
    };
    switch (Sound)
    {
    case 1: // E - double pip
        pip(1200.0f, 45); gap(35); pip(1200.0f, 45);
        break;
    case 2: // J - triple pip, two when close (it has to fit the 180 ms close cycle)
        pip(1600.0f, 35); gap(25); pip(1600.0f, 35);
        if (!isNear) { gap(25); pip(1600.0f, 35); }
        break;
    case 3: // N - fast warble
        warble(1450.0f, 250.0f, 30.0f, 140);
        break;
    case 4: // the original: one 880 Hz sine pip (kToneHz, kBeepMs)
        pip(kToneHz, kBeepMs);
        break;
    default: // B - warble: 55 ms at 1000 Hz, 55 ms at 1400 Hz
        pip(1000.0f, 55); pip(1400.0f, 55);
        break;
    }
    return n;
}

// Synthesise one beep-plus-gap cycle and queue it kQueueDepth times. Pump() re-queues each
// buffer as it drains, so the repeat continues for as long as the episode lasts.
bool ThreatAlarm::Begin(int periodMs, bool isNear)
{
    if (!EnsureDevice()) return false;
    End(); // clear any previous buffers, but leave the device open

    const int total = (kSampleRate * periodMs) / 1000;
    if (total <= 0) return false;

    Samples = static_cast<short *>(std::calloc(static_cast<size_t>(total), sizeof(short)));
    if (!Samples) return false;
    SampleBytes = static_cast<unsigned long>(total) * sizeof(short);

    // The selected sound's burst (each part has short ramps - a hard edge clicks every pulse); the rest of the cycle
    // stays zeroed by calloc - that is the gap.
    const int burst = SynthBurst(Samples, total, isNear);
    BurstMs = burst > 0 ? (burst * 1000) / kSampleRate : kBeepMs;

    HWAVEOUT hwo = reinterpret_cast<HWAVEOUT>(Wave);

    // Every header points at the SAME sample buffer - they are just queue slots. Preparing
    // the same memory more than once is fine; it page-locks the region per header.
    for (int i = 0; i < kQueueDepth; ++i)
    {
        WAVEHDR *hdr = static_cast<WAVEHDR *>(std::calloc(1, sizeof(WAVEHDR)));
        if (!hdr) { End(); return false; }
        hdr->lpData         = reinterpret_cast<LPSTR>(Samples);
        hdr->dwBufferLength = SampleBytes;
        Headers[i]          = hdr;

        if (waveOutPrepareHeader(hwo, hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
            waveOutWrite(hwo, hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        {
            End();
            return false;
        }
    }

    PeriodMs = periodMs;
    Sounding = true;
    return true;
}

// Re-queue every buffer the device has finished with. Called every frame while sounding.
// With kQueueDepth cycles buffered there is over a second of slack, so even a bad frame
// hitch cannot open a gap in the tone - the queue drains far slower than we refill it.
void ThreatAlarm::Pump()
{
    if (!Sounding || !Wave) return;
    HWAVEOUT hwo = reinterpret_cast<HWAVEOUT>(Wave);

    for (int i = 0; i < kQueueDepth; ++i)
    {
        WAVEHDR *hdr = static_cast<WAVEHDR *>(Headers[i]);
        if (!hdr || !(hdr->dwFlags & WHDR_DONE)) continue;
        // Clear DONE before re-writing: the header stays prepared, and writing it again
        // with the flag still set is rejected by some drivers.
        hdr->dwFlags &= ~WHDR_DONE;
        waveOutWrite(hwo, hdr, sizeof(WAVEHDR));
    }
}

// Stop the tone and release the BUFFERS, but leave the device open for the next episode.
// Teardown order is the whole point: reset (device stops reading), unprepare (driver lets
// go of each header), only then free. Getting this backwards is a use-after-free in the
// audio thread, which shows up as an intermittent crash under fire and nowhere else.
void ThreatAlarm::End()
{
    HWAVEOUT hwo = reinterpret_cast<HWAVEOUT>(Wave);
    if (hwo) waveOutReset(hwo);

    for (int i = 0; i < kQueueDepth; ++i)
    {
        WAVEHDR *hdr = static_cast<WAVEHDR *>(Headers[i]);
        if (!hdr) continue;
        if (hwo) waveOutUnprepareHeader(hwo, hdr, sizeof(WAVEHDR));
        std::free(hdr);
        Headers[i] = nullptr;
    }
    if (Samples)
    {
        std::free(Samples);
        Samples = nullptr;
    }
    SampleBytes = 0;
    PeriodMs    = 0;
    Sounding    = false;
}

// Full teardown, including the device. Only on shutdown - Update() never closes it.
void ThreatAlarm::Shutdown()
{
    End();
    if (Wave)
    {
        waveOutClose(reinterpret_cast<HWAVEOUT>(Wave));
        Wave = nullptr;
    }
    DeviceFailed  = false;
    Near          = false;
    LockedSinceMs = 0.0;
    ClearSinceMs  = 0.0;
}

bool ThreatAlarm::IsBeeping(double nowMs) const
{
    if (!Sounding || PeriodMs <= 0) return false;

    if (Wave)
    {
        MMTIME mt{};
        mt.wType = TIME_MS;
        if (waveOutGetPosition(reinterpret_cast<HWAVEOUT>(Wave), &mt, sizeof(mt)) ==
                MMSYSERR_NOERROR)
        {
            long posMs = -1;
            if (mt.wType == TIME_MS)
                posMs = (long)mt.u.ms;
            else if (mt.wType == TIME_SAMPLES)
                posMs = (long)((mt.u.sample * 1000ULL) / kSampleRate);
            else if (mt.wType == TIME_BYTES)
                posMs = (long)((mt.u.cb * 1000ULL) / (kSampleRate * 2));
            if (posMs >= 0)
                return (posMs % PeriodMs) < BurstMs;
        }
    }
    // Driver would not report a position - same rate, free phase.
    return std::fmod(nowMs, (double)PeriodMs) < (double)BurstMs;
}

void ThreatAlarm::Update(bool locked, float nearestCm, bool dataFresh, bool enabled,
                         double nowMs)
{
    // Any of these is an unconditional, immediate silence. In particular a stalled
    // publisher must silence the tone rather than latch it: dying, escaping to the menu,
    // ending the mission and closing the game all stop the plugin publishing, and every
    // one of those is a moment when a stuck alarm would be intolerable.
    if (!enabled || !dataFresh)
    {
        if (Sounding) End();
        LockedSinceMs = 0.0;
        ClearSinceMs  = 0.0;
        return;
    }

    bool shouldSound;
    if (locked)
    {
        ClearSinceMs = 0.0;
        if (LockedSinceMs == 0.0) LockedSinceMs = nowMs;
        shouldSound = true;
    }
    else
    {
        if (ClearSinceMs == 0.0) ClearSinceMs = nowMs;
        // Hold through a dropped probe sample, then release for real. The hold only ever
        // EXTENDS a tone that is already sounding - it must never start one, or a single
        // stale sample would beep at a pilot nobody is shooting at.
        shouldSound = Sounding && (nowMs - ClearSinceMs < kReleaseMs);
        if (!shouldSound) LockedSinceMs = 0.0;
    }

    if (!shouldSound)
    {
        if (Sounding) End();
        Near = false;
        return;
    }

    // TWO STATES, and the transition between them is instant in both directions.
    //
    //   nearest <  kCloseCm  ->  kPeriodCloseMs
    //   nearest >= kCloseCm  ->  kPeriodFarMs
    //
    // There is no hysteresis band and no one-way latch. Both were considered and both were
    // solving a problem that does not occur: a closing round's range is monotone and moves
    // several metres per publish, so it crosses the threshold exactly once. The case that
    // does drive the state back to far - the close salvo impacting while a distant second
    // salvo is still bound - is a discontinuous jump of hundreds of metres, nowhere near
    // the boundary.
    //
    // Note what is NOT here: a second salvo launching does not touch the cadence or the
    // phase. It hits neither branch below and falls through to Pump(), so the tone runs on
    // uninterrupted. Only crossing the range threshold re-cues.
    //
    // A range of exactly kCloseCm is far; nearestCm < 0 means "no bound round to measure",
    // which HOLDS the current state rather than forcing far. That only arises during the
    // short release tail after the last round dies, and stepping the cadence down for the
    // final few frames of an episode would be a rate change carrying no information.
    //
    // `isNear`, not `near`: <windows.h> still #defines `near` as an empty legacy segment
    // keyword, so a local of that name silently becomes a syntax error.
    const bool isNear = (nearestCm < 0.0f) ? Near : (nearestCm < kCloseCm);

    if (!Sounding)
    {
        Begin(isNear ? kPeriodCloseMs : kPeriodFarMs, isNear);
        Near = isNear;
    }
    else if (isNear != Near)
    {
        // Re-synthesis restarts the phase. That is deliberate: a cadence change is a cue in
        // its own right, and the moment it happens is exactly when the ear should catch it.
        Begin(isNear ? kPeriodCloseMs : kPeriodFarMs, isNear);
        Near = isNear;
    }
    else
        Pump(); // keep the queue topped up - this is what makes the tone continuous
}
