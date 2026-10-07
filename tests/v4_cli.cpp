// v4_cli ORIG.wav KARAOKE.wav OUT.wav [--level off|on|auto] [--kvol X] [--extraction] [--hard]
//
// Runs the Utagoe Rip 4 separation (src/engine/v4.cpp) on two audio files (any format
// the program reads) and prints what the alignment found. eval/run.py uses it as engine
// "v4-cpp" to score the C++ port against the Python prototype.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

#include "engine/audio_io.hpp"
#include "engine/v4.hpp"
#include "engine/wav.hpp"

using namespace utagoe;

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: v4_cli ORIG.wav KARAOKE.wav OUT.wav [--level off|on|auto] [--kvol X] [--extraction] [--hard]\n");
        return 1;
    }
    v4::Options opt;
    for (int i = 4; i < argc; i++) {
        if (!wcscmp(argv[i], L"--level") && i + 1 < argc) {
            const wchar_t* v = argv[++i];
            opt.level_track = !wcscmp(v, L"off") ? 0 : !wcscmp(v, L"on") ? 1 : 2;
        } else if (!wcscmp(argv[i], L"--kvol") && i + 1 < argc) {
            opt.kvol = _wtof(argv[++i]);
        } else if (!wcscmp(argv[i], L"--extraction")) {
            opt.quality = false;
        } else if (!wcscmp(argv[i], L"--hard")) {  // Utagoe Rip 4.0: 4.0's EQ passes and 3.0's rule
            opt.huber = opt.soft = false;
            opt.const_lag = 0.0;
        }
    }
    Audio orig, kar;
    // any supported format; the karaoke at the original's rate and channel count
    if (!load_audio(argv[1], 0, 0, &orig) || !load_audio(argv[2], orig.rate, orig.channels, &kar)) {
        std::fprintf(stderr, "cannot read input\n");
        return 2;
    }
    auto planar = [](const Audio& a) {
        std::vector<std::vector<double>> p(a.channels, std::vector<double>(a.frames()));
        for (size_t i = 0; i < a.frames(); i++)
            for (int c = 0; c < a.channels; c++) p[c][i] = a.data[i * a.channels + c] / 32768.0;
        return p;
    };
    v4::Report rep;
    auto y = v4::separate(planar(orig), planar(kar), orig.rate, opt, &rep);
    std::printf("lag %ld sign %d drift_line %d lag_start %.3f lag_end %.3f stretch %.3f resampled %d "
                "level_gain %.3f level_applied %d soft_q %.2f\n",
                rep.lag, rep.sign, (int)rep.drift_line, rep.lag_start, rep.lag_end, rep.stretch, (int)rep.resampled,
                rep.level_gain, (int)rep.level_applied, rep.soft_q);
    Audio out;
    out.rate = orig.rate;
    out.channels = orig.channels;
    out.data.resize(orig.data.size());
    for (size_t i = 0; i < orig.frames(); i++)
        for (int c = 0; c < orig.channels; c++)
            out.data[i * orig.channels + c] = (int16_t)std::clamp(std::lround(y[c][i] * 32768.0), -32768L, 32767L);
    return write_wav(argv[3], out) ? 0 : 2;
}
