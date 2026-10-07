// Engine tests and a small processing tool for development.
//
//   engine_test                       self-test on a synthetic song (used by ctest)
//   engine_test ORIG INST OUT [key=value ...] [--original-timing]
//                                     process files and print the analysis log;
//                                     keys are the [V30_Option] INI keys
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "engine/audio_io.hpp"
#include "engine/dsp.hpp"
#include "engine/engine.hpp"

using namespace utagoe;

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) failures++;
}

// Deterministic synthetic material: a centre-panned "vocal" with vibrato and
// syllables over chords, bass and noise bursts panned around the stereo field.
struct Song {
    Audio orig, inst;
    std::vector<int16_t> vocal;  // mono
};

Song make_song(double seconds, int lead_in, bool lead_in_orig = false, int rate = 44100) {
    const double PI = 3.14159265358979323846;
    size_t n = (size_t)(seconds * rate);
    std::vector<double> il(n), ir(n), voc(n);
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (double)(seed >> 8) / 16777216.0; };
    const double tones[5][3] = {{110.0, 0.5, 0.18}, {277.2, 0.15, 0.10}, {330.0, 0.85, 0.10}, {660.0, 0.3, 0.06},
                                {523.25, 0.7, 0.05}};
    for (const auto& tn : tones) {
        double ph[6], eph = rnd() * 6.28;
        for (int hh = 1; hh <= 5; hh++) ph[hh] = rnd() * 6.28;
        for (size_t i = 0; i < n; i++) {
            double t = (double)i / rate, v = 0;
            for (int hh = 1; hh <= 5; hh++) v += std::sin(2 * PI * tn[0] * hh * t + ph[hh]) / hh;
            v *= tn[2] * (0.6 + 0.4 * std::sin(2 * PI * 0.25 * t + eph));
            il[i] += v * (1 - tn[1]);
            ir[i] += v * tn[1];
        }
    }
    for (size_t start = 0; start < n; start += rate / 4) {
        for (size_t i = start; i < std::min(n, start + 3000); i++) {
            double e = std::exp(-(double)(i - start) / 600.0) * (rnd() * 2 - 1) * 1.7;
            il[i] += 0.08 * e;
            if (i + 7 < n) ir[i + 7] += 0.06 * e;
        }
    }
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / rate;
        double f = 300 + 80 * std::sin(2 * PI * 0.1 * t) + 6 * std::sin(2 * PI * 5.5 * t);
        ph += 2 * PI * f / rate;
        double v = 0;
        for (int hh = 1; hh < 10; hh++) v += std::sin(hh * ph) * std::pow(0.7, hh);
        double syl = std::sin(2 * PI * 1.5 * t);
        voc[i] = t < 2.0 ? 0.0 : 0.25 * v * std::sqrt(syl > 0 ? syl : 0.0);
    }
    double peak = 1e-9;
    for (size_t i = 0; i < n; i++) peak = std::max(peak, std::max(std::fabs(il[i] + voc[i]), std::fabs(ir[i] + voc[i])));
    double scale = 32767 * 0.8 / peak;
    Song s;
    s.vocal.resize(n);
    std::vector<int16_t> inst(2 * n);
    for (size_t i = 0; i < n; i++) {
        s.vocal[i] = (int16_t)std::lround(voc[i] * scale);
        inst[2 * i] = (int16_t)std::lround(il[i] * scale);
        inst[2 * i + 1] = (int16_t)std::lround(ir[i] * scale);
    }
    std::vector<int16_t> orig(2 * n);
    for (size_t i = 0; i < 2 * n; i++) orig[i] = (int16_t)std::max(-32768, std::min(32767, inst[i] + s.vocal[i / 2]));
    s.orig.rate = s.inst.rate = rate;
    s.orig.channels = s.inst.channels = 2;
    if (lead_in_orig) {
        s.orig.data.assign(2 * (size_t)lead_in, 0);
        s.orig.data.insert(s.orig.data.end(), orig.begin(), orig.end());
        s.inst.data = inst;
        s.vocal.insert(s.vocal.begin(), (size_t)lead_in, 0);
    } else {
        s.orig.data = orig;
        s.inst.data.assign(2 * (size_t)lead_in, 0);
        s.inst.data.insert(s.inst.data.end(), inst.begin(), inst.end());
    }
    return s;
}

double snr(const Audio& out, const std::vector<int16_t>& voc) {
    double sig = 0, err = 0;
    for (size_t i = 0; i < voc.size() && i < out.frames(); i++)
        for (int c = 0; c < out.channels; c++) {
            double r = voc[i], e = (double)out.data[i * out.channels + c] - r;
            sig += r * r, err += e * e;
        }
    return 10 * std::log10(sig / std::max(err, 1e-9));
}

struct Run {
    Audio out;
    std::vector<std::wstring> log;
};

// this harness tests (and, from the command line, runs) the 3.0 engine unless asked otherwise
Settings settings_30() {
    Settings c;
    c.v4 = false;
    return c;
}

Run run(const Song& s, Settings cfg, bool original_timing = false, bool single = false) {
    Run r;
    Callbacks cb;
    cb.log = [&](const std::wstring& line) { r.log.push_back(line); };
    UtagoeRip rip(s.orig, single ? nullptr : &s.inst, cfg, cb, original_timing);
    r.out = rip.run();
    return r;
}

bool logged(const Run& r, const wchar_t* text) {
    for (const auto& l : r.log)
        if (l.find(text) != std::wstring::npos) return true;
    return false;
}

// A WAV file of `frames` x `channels` samples from f(frame, channel) in [-1, 1]: 24-bit
// integer (tag 1), 32-bit float (tag 3) or, with more than two channels, extensible.
void write_test_wav(const std::wstring& path, int rate, int channels, int tag, int bits, size_t frames,
                    double (*f)(size_t, int, int)) {
    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return;
    uint32_t data_len = (uint32_t)(frames * channels * (bits / 8));
    bool ext = channels > 2;
    uint32_t fmt_len = ext ? 40 : 16;
    auto put = [&](uint32_t v, int n) { for (int i = 0; i < n; i++) fputc((int)(v >> (8 * i)) & 255, fp); };
    fwrite("RIFF", 1, 4, fp), put(4 + 8 + fmt_len + 8 + data_len, 4), fwrite("WAVEfmt ", 1, 8, fp), put(fmt_len, 4);
    put(ext ? 0xFFFE : tag, 2), put(channels, 2), put(rate, 4), put(rate * channels * bits / 8, 4);
    put(channels * bits / 8, 2), put(bits, 2);
    if (ext) {  // cbSize, valid bits, channel mask, sub-format GUID (KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT)
        put(22, 2), put(bits, 2), put(0x3F, 4), put(tag, 2);
        const unsigned char rest[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
        fwrite(rest, 1, 14, fp);
    }
    fwrite("data", 1, 4, fp), put(data_len, 4);
    for (size_t i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) {
            double v = f(i, c, rate);
            if (tag == 3) {
                float x = (float)v;
                fwrite(&x, 4, 1, fp);
            } else {
                put((uint32_t)(int32_t)std::lround(v * 8388607.0), 3);
            }
        }
    fclose(fp);
}

// the input formats other than 16-bit WAV (4.2): sample formats, resampling, channels
void audio_io_test() {
    std::printf("audio input\n");
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring base = std::wstring(dir) + L"utagoe_audio_test_";
    const double PI = 3.14159265358979323846;
    auto tone = [](size_t i, int c, int rate) { return 0.5 * std::sin(2 * 3.14159265358979323846 * (440.0 + 220 * c) * i / rate); };
    auto check = [&](const Audio& a, int rate, double (*f)(size_t, int, int), size_t skip) {
        double sig = 0, err = 0;
        for (size_t i = skip; i + skip < a.frames(); i++)
            for (int c = 0; c < a.channels; c++) {
                double r = f(i, c, rate) * 32768.0, e = a.data[i * a.channels + c] - r;
                sig += r * r, err += e * e;
            }
        return 10 * std::log10(sig / std::max(err, 1e-9));
    };
    Audio a;
    std::wstring p24 = base + L"24.wav", pf = base + L"f.wav", p48 = base + L"48.wav", p6 = base + L"6.wav";
    write_test_wav(p24, 44100, 2, 1, 24, 44100, tone);
    expect(load_audio(p24, 0, 0, &a) && a.rate == 44100 && a.channels == 2 && check(a, 44100, tone, 0) > 80,
           "24-bit WAV read");
    write_test_wav(pf, 44100, 2, 3, 32, 44100, tone);
    expect(load_audio(pf, 0, 0, &a) && check(a, 44100, tone, 0) > 80, "32-bit float WAV read");
    write_test_wav(p48, 48000, 2, 1, 24, 96000, tone);
    bool ok = load_audio(p48, 44100, 2, &a);
    double snr48 = ok ? check(a, 44100, tone, 2000) : 0;
    std::printf("        48 -> 44.1 kHz: SNR %.1f dB\n", snr48);
    expect(ok && a.rate == 44100 && std::labs((long)a.frames() - 88200) <= 1 && snr48 > 70, "48 kHz resampled to 44.1 kHz");
    ok = load_audio(p24, 0, 1, &a) && a.channels == 1;
    double snr_mono = ok ? check(a, 44100, [](size_t i, int, int rate) {
        return 0.25 * (std::sin(2 * 3.14159265358979323846 * 440.0 * i / rate) +
                       std::sin(2 * 3.14159265358979323846 * 660.0 * i / rate));
    }, 0) : 0;
    std::printf("        stereo -> mono: SNR %.1f dB\n", snr_mono);
    expect(ok && snr_mono > 80, "stereo to mono");
    write_test_wav(p6, 44100, 6, 1, 24, 4410, [](size_t i, int c, int) { return c == 0 ? 0.1 : c == 1 ? -0.1 : c == 2 ? 0.2 : 0.0; });
    ok = load_audio(p6, 0, 0, &a) && a.channels == 2;
    // L = FL + 0.707 FC, R = FR + 0.707 FC
    expect(ok && std::abs(a.data[0] - std::lround((0.1 + 0.70710678 * 0.2) * 32768)) <= 1 &&
           std::abs(a.data[1] - std::lround((-0.1 + 0.70710678 * 0.2) * 32768)) <= 1, "5.1 mixed down to stereo");
    for (const std::wstring& p : {p24, pf, p48, p6}) DeleteFileW(p.c_str());
    (void)PI;
}

int self_test() {
    std::printf("synthetic song (8 s, instrumental offset 12345 samples)\n");
    Song s = make_song(8.0, 12345);
    {
        Run r = run(s, settings_30());
        expect(logged(r, L"Initial offset:12345 phase:0"), "initial offset found");
        expect(r.out.frames() == s.orig.frames(), "output length = original length");
        double v = snr(r.out, s.vocal);
        std::printf("        frequency method SNR %.1f dB\n", v);
        expect(v > 25, "frequency method recovers the vocal");
    }
    struct Case { const char* name; void (*set)(Settings&); double min_snr; };
    const Case cases[] = {
        {"waveform method", [](Settings& c) { c.merge_mode = MERGE_WAVEFORM; }, 25},
        {"L/R difference", [](Settings& c) { c.proc_mode = PROC_LR_DIFF; }, 15},
        {"mono", [](Settings& c) { c.proc_mode = PROC_MONO; }, 15},
        {"extraction priority", [](Settings& c) { c.sound_qty = EXTRACTION_PRIORITY; }, 15},
        {"adaptive level", [](Settings& c) { c.merge_mode = MERGE_WAVEFORM; c.level_adpt = LEVEL_AUTO_ADAPTIVE; }, 15},
        {"oversampling x8", [](Settings& c) { c.merge_mode = MERGE_WAVEFORM; c.ovsp_flag = true; c.ovsp_mx = 8; }, 15},
        {"oversampling mono", [](Settings& c) { c.merge_mode = MERGE_WAVEFORM; c.ovsp_flag = true; c.ovsp_mx = 8;
                                                 c.proc_mode = PROC_MONO; }, 15},
        {"manual everything", [](Settings& c) { c.intro_mode = INTRO_DETAILED; c.adpt_mode = ADPT_MANUAL;
                                                 c.level_adpt = LEVEL_MANUAL; c.krk_phase = PHASE_POSITIVE; }, 15},
        {"normal intro", [](Settings& c) { c.intro_mode = INTRO_NORMAL; }, 15},
    };
    for (const Case& k : cases) {
        Settings cfg = settings_30();
        k.set(cfg);
        Run r = run(s, cfg);
        double v = snr(r.out, s.vocal);
        std::printf("        %-20s SNR %.1f dB\n", k.name, v);
        expect(v > k.min_snr, k.name);
    }
    {
        Settings cfg = settings_30();
        cfg.cntr_flag = cfg.lpf_flag = cfg.hpf_flag = true;
        Run r = run(s, cfg);
        expect(r.out.frames() == s.orig.frames(), "filters + centralization");
    }
    {
        Run r = run(s, settings_30(), true);
        std::vector<int16_t> shifted(s.vocal.size() + LATENCY, 0);
        std::copy(s.vocal.begin(), s.vocal.end(), shifted.begin() + LATENCY);
        shifted.resize(s.vocal.size());
        expect(snr(r.out, shifted) > 25, "original timing has the 7168-sample delay");
    }
    {
        Song inv = s;
        for (auto& x : inv.inst.data) x = (int16_t)std::max(-32768, std::min(32767, -(int)x));
        Run r = run(inv, settings_30());
        expect(logged(r, L"phase:1"), "inverted instrumental detected");
        expect(snr(r.out, s.vocal) > 25, "inverted instrumental extracted");
    }
    {
        Song lead = make_song(8.0, 5000, true);
        Run r = run(lead, settings_30());
        expect(logged(r, L"Initial offset:-5000"), "original with longer lead-in");
        expect(snr(r.out, lead.vocal) > 25, "original with longer lead-in extracted");
    }
    {
        Run r = run(s, settings_30(), false, true);
        expect(r.out.data == s.orig.data, "original only, nothing enabled: passes through");
    }
    std::printf("Utagoe Rip 4 separation\n");
    {
        Settings v = settings_30();
        v.v4 = true;
        Run r = run(s, v);
        expect(logged(r, L"v4 lag:12345 sign:1"), "v4: offset found");
        expect(r.out.frames() == s.orig.frames(), "v4: output length = original length");
        double x = snr(r.out, s.vocal);
        std::printf("        v4 SNR %.1f dB\n", x);
        expect(x > 50, "v4 recovers the vocal (an exact match subtracts, keeping the vocal whole)");
        Song inv = s;
        for (auto& q : inv.inst.data) q = (int16_t)std::max(-32768, std::min(32767, -(int)q));
        Run ri = run(inv, v);
        expect(logged(ri, L"sign:-1"), "v4: inverted instrumental detected");
        expect(snr(ri.out, s.vocal) > 25, "v4: inverted instrumental extracted");
        Song lead = make_song(8.0, 5000, true);
        Run rl = run(lead, v);
        expect(logged(rl, L"v4 lag:-5000"), "v4: original with longer lead-in");
        expect(snr(rl.out, lead.vocal) > 25, "v4: original with longer lead-in extracted");
        // an instrumental that plays 300 ppm slow (clock drift): the lag moves 2.5 samples
        // across a frame, so v4 resamples it along the lag curve
        Song dr = s;
        const double PI = 3.14159265358979323846, ratio = 1.0 / (1.0 + 300e-6);
        const int half = 16;
        size_t frames = s.inst.frames();
        for (size_t i = 0; i < frames; i++) {
            double pos = (double)i * ratio, fl = std::floor(pos);
            for (int c = 0; c < 2; c++) {
                double acc = 0;
                for (int j = -half + 1; j <= half; j++) {
                    long long idx = (long long)fl + j;
                    if (idx < 0 || idx >= (long long)frames) continue;
                    double u = pos - (double)idx;
                    double w = u == 0 ? 1.0 : half * std::sin(PI * u) * std::sin(PI * u / half) / (PI * PI * u * u);
                    acc += s.inst.data[(size_t)idx * 2 + c] * w;
                }
                dr.inst.data[i * 2 + c] = (int16_t)std::lround(std::max(-32768.0, std::min(32767.0, acc)));
            }
        }
        Run rd = run(dr, v), r30 = run(dr, settings_30());
        double xd = snr(rd.out, s.vocal), x30 = snr(r30.out, s.vocal);
        std::printf("        drift 300 ppm: v4 SNR %.1f dB, 3.0 %.1f dB\n", xd, x30);
        expect(logged(rd, L"(resampled)"), "v4: drifting instrumental resampled");
        expect(xd > 20 && xd > x30, "v4: drifting instrumental extracted");
        Song late = make_song(16.0, 529200);  // the karaoke starts 12 s later
        Run rf = run(late, v);
        expect(logged(rf, L"v4 lag:529200 sign:1"), "v4: offset over 10 s found");
        Song low = make_song(10.0, 1234, false, 8000);  // 8 kHz: band edges below Nyquist
        Run rlow = run(low, v);
        expect(logged(rlow, L"v4 lag:1234 sign:1") && snr(rlow.out, low.vocal) > 20, "v4: 8 kHz file");
        Song brief = make_song(5.0, 100);
        Run rb = run(brief, v);
        expect(logged(rb, L"under 8 s") && snr(rb.out, brief.vocal) > 20, "v4: under 8 s, 3.0's processing");
        v.cntr_flag = v.lpf_flag = v.hpf_flag = true;
        expect(run(s, v).out.frames() == s.orig.frames(), "v4: filters + centralization");
    }
    {
        Settings d;
        expect(d.v4, "Utagoe Rip 4 separation is the default");
        expect(std::fabs(d.kvol() - 1.2f) < 1e-6 && d.lpf_hz() == 10000 && d.hpf_hz() == 230 &&
               d.cntr_strength() == 2.0f && d.adpt_range() == 3, "settings conversions");
    }
    audio_io_test();
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return self_test();
    Audio orig, inst;
    // any supported format; the instrumental at the original's rate and channel count
    if (!load_audio(argv[1], 0, 0, &orig) || !load_audio(argv[2], orig.rate, orig.channels, &inst)) {
        std::fprintf(stderr, "cannot read input\n");
        return 2;
    }
    Settings cfg = settings_30();   // V4Engine=1 on the command line runs v4
    bool original_timing = false;
    std::wstring ini = L"[V30_Option]\r\n";
    for (int i = 4; i < argc; i++) {
        if (!wcscmp(argv[i], L"--original-timing")) original_timing = true;
        else ini += std::wstring(argv[i]) + L"\r\n";
    }
    if (argc > 4) {  // reuse the INI parser for key=value options
        wchar_t tmp[MAX_PATH], path[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        GetTempFileNameW(tmp, L"utg", 0, path);
        FILE* f = _wfopen(path, L"wb");
        int n = WideCharToMultiByte(932, 0, ini.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string b(n, '\0');
        WideCharToMultiByte(932, 0, ini.c_str(), -1, &b[0], n, nullptr, nullptr);
        fwrite(b.data(), 1, b.size() - 1, f);
        fclose(f);
        cfg.load_ini(path);
        DeleteFileW(path);
    }
    Callbacks cb;
    cb.log = [](const std::wstring& l) { std::wprintf(L"%ls\n", l.c_str()); };
    UtagoeRip rip(orig, &inst, cfg, cb, original_timing);
    Audio out = rip.run();
    return write_wav(argv[3], out) ? 0 : 2;
}
