// v4_cli ORIG.wav KARAOKE.wav OUT.wav [--level off|on|auto] [--kvol X] [--extraction]
//
// Runs the Utagoe Rip 4 separation (src/engine/v4.cpp) on 16-bit WAV files and prints
// what the alignment found. eval/run.py uses it as engine "v4-cpp" to score the C++
// port against the Python prototype.
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

#include "engine/v4.hpp"
#include "engine/wav.hpp"

using namespace utagoe;

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: v4_cli ORIG.wav KARAOKE.wav OUT.wav [--level off|on|auto] [--kvol X] [--extraction]\n");
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
        }
    }
    Audio orig, kar;
    if (!read_wav(argv[1], &orig) || !read_wav(argv[2], &kar)) {
        std::fprintf(stderr, "cannot read input (16-bit PCM WAV)\n");
        return 2;
    }
    if (orig.channels != kar.channels || orig.rate != kar.rate) {
        std::fprintf(stderr, "sample rate and channel count must match\n");
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
    std::printf("lag %ld sign %d drift_line %d lag_start %.3f lag_end %.3f level_gain %.3f level_applied %d\n",
                rep.lag, rep.sign, (int)rep.drift_line, rep.lag_start, rep.lag_end, rep.level_gain,
                (int)rep.level_applied);
    Audio out;
    out.rate = orig.rate;
    out.channels = orig.channels;
    out.data.resize(orig.data.size());
    for (size_t i = 0; i < orig.frames(); i++)
        for (int c = 0; c < orig.channels; c++)
            out.data[i * orig.channels + c] = (int16_t)std::clamp(std::lround(y[c][i] * 32768.0), -32768L, 32767L);
    return write_wav(argv[3], out) ? 0 : 2;
}
