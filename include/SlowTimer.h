#pragma once
// =====================================================================================
// SLOW-STEP TIMER (2026-10-08). Hitch hunting: our plugins' per-frame work, step by step, on the game thread.
// Begin() at the top of a tick, Step("name") after each piece; a piece taking more than kSlowMs is reported
// at once ("slow: <plugin> <step> 4.12 ms", at most 5 lines a second), and every 60 s a summary of the steps
// that went over kNoteMs (how often, the worst). Cost: one QueryPerformanceCounter per step (~20 ns) - nothing
// is logged while everything is fast.
// =====================================================================================
#include <windows.h>
#include <GsPointer.h>
#include <cstdio>
#include <cstring>

struct SlowTimer
{
    static constexpr double kSlowMs = 2.0; // reported at once
    static constexpr double kNoteMs = 1.0; // counted for the minute's summary
    static constexpr double kHitchMs = 50.0; // logged at once even with the detailed log off
    struct Stat
    {
        const char *Name;
        int         Over;  // times over kNoteMs this minute
        double      Worst; // ms, this minute
    };
    const char   *Plugin;
    Stat          S[40] = {};
    int           N = 0;
    LARGE_INTEGER T0{}, Freq{};
    ULONGLONG     SecMs = 0, SumMs = 0;
    int           SecLines = 0;
    char          Pending[8][96] = {};
    double        PendMs[8] = {};
    int           PendN = 0;

    explicit SlowTimer(const char *plugin) : Plugin(plugin) { QueryPerformanceFrequency(&Freq); }

    void Begin() { QueryPerformanceCounter(&T0); }

    void Step(const char *name)
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        const double ms = (double)(t.QuadPart - T0.QuadPart) * 1000.0 / (double)Freq.QuadPart;
        T0 = t;
        if (ms < kNoteMs)
            return;
        int k = 0;
        while (k < N && S[k].Name != name)
            ++k;
        if (k == N && N < 40)
            S[N++] = Stat{name, 0, 0.0};
        if (k < N)
        {
            ++S[k].Over;
            if (ms > S[k].Worst)
                S[k].Worst = ms;
        }
        if (ms >= kSlowMs && PendN < 8)
        {
            PendMs[PendN] = ms;
            std::snprintf(Pending[PendN++], sizeof(Pending[0]), "slow: %s %s %.2f ms", Plugin, name, ms);
        }
    }

    // Once a tick, after the steps: hands finished lines to log(const char *).
    template <class F> void Flush(F log)
    {
        const ULONGLONG now = GetTickCount64();
        if (now - SecMs >= 1000)
        {
            SecMs = now;
            SecLines = 0;
        }
        // 2026-10-18 (release prep, user): the one-by-one lines only with GlobalSettings [debug] verbose_log = 1 - except a
        // real hitch (kHitchMs and over), which is always logged. The 60 s summary below is always written.
        const bool each = PendN && gsp::VerboseLog();
        for (int i = 0; i < PendN; ++i)
            if (SecLines < 5 && (each || PendMs[i] >= kHitchMs))
            {
                ++SecLines;
                log(Pending[i]);
            }
        PendN = 0;
        if (!SumMs)
            SumMs = now;
        if (now - SumMs >= 60000)
        {
            SumMs = now;
            char line[600];
            int len = std::snprintf(line, sizeof(line), "slow: %s, last 60 s, steps over %.0f ms:", Plugin, kNoteMs);
            bool any = false;
            for (int k = 0; k < N && len < (int)sizeof(line) - 60; ++k)
                if (S[k].Over)
                {
                    any = true;
                    len += std::snprintf(line + len, sizeof(line) - len, " %s %dx (worst %.1f ms);", S[k].Name, S[k].Over, S[k].Worst);
                    S[k].Over = 0;
                    S[k].Worst = 0.0;
                }
            if (any)
                log(line);
        }
    }
};
