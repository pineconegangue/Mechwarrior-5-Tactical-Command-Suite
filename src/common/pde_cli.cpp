// Test harness for PaperdollExtract on Linux:  pde_cli <outdir> <pak> [more.pak ...]
#include "PaperdollExtract.h"
#include <cstdio>
#include <ctime>

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::printf("usage: pde_cli <outdir> <pak> [more.pak ...]\n");
        return 2;
    }
    std::vector<std::filesystem::path> paks;
    for (int i = 2; i < argc; ++i)
        paks.push_back(argv[i]);
    pde::Result r;
    const time_t t0 = time(nullptr);
    int lastPct = -1;
    pde::Extract(paks, argv[1], r, [&](int d, int t, const std::string &) {
        const int pct = t ? d * 100 / t : 100;
        if (pct / 10 != lastPct / 10) { std::printf("%d%% (%d/%d)\n", pct, d, t); std::fflush(stdout); lastPct = pct; }
        return true;
    });
    std::printf("paks %d, chassis %d, files %d, bodies %d, failures %zu, %ld s\n", r.Paks, r.Chassis, r.Files, r.Bodies,
                r.Failures.size(), (long)(time(nullptr) - t0));
    for (size_t i = 0; i < r.Failures.size() && i < 40; ++i)
        std::printf("   %s\n", r.Failures[i].c_str());
    return 0;
}
