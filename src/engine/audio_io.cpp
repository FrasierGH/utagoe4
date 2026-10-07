#include "audio_io.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace utagoe {

const wchar_t* const AUDIO_EXTENSIONS[] = {L".WAV", L".WAVE", L".FLAC", L".MP3", L".M4A", L".AAC", L".MP4", L".WMA",
                                           L".OGG", L".OGA", L".OPUS", nullptr};

bool is_audio_extension(const std::wstring& upper_ext) {
    for (const wchar_t* const* e = AUDIO_EXTENSIONS; *e; e++)
        if (upper_ext == *e) return true;
    return false;
}

namespace {

const double PI = 3.14159265358979323846;

// interleaved float samples at the file's own rate and channel count
struct Pcm {
    int rate = 0, channels = 0;
    std::vector<float> data;
    size_t frames() const { return channels ? data.size() / (size_t)channels : 0; }
};

// ---------------------------------------------------------------- WAV

struct File {
    FILE* f;
    explicit File(const std::wstring& path) : f(_wfopen(path.c_str(), L"rb")) {}
    ~File() { if (f) fclose(f); }
};

uint32_t u32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }

struct WavFmt {
    int tag = 0, channels = 0, rate = 0, bits = 0, block = 0;
    long long data_pos = 0, data_len = 0;
};

// The fmt and data chunks of a RIFF WAVE file; tag is 1 (integer PCM) or 3 (float),
// WAVE_FORMAT_EXTENSIBLE resolved to its sub-format. 0 or a WAV_* code.
int wav_scan(FILE* f, WavFmt* w) {
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return WAV_BAD_FILE;
    bool have_fmt = false;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t len = u32(ck + 4);
        long long body = _ftelli64(f);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fmt[40] = {};
            size_t got = fread(fmt, 1, std::min<uint32_t>(len, 40), f);
            if (got < 16) return WAV_BAD_FILE;
            w->tag = u16(fmt);
            w->channels = u16(fmt + 2);
            w->rate = (int)u32(fmt + 4);
            w->block = u16(fmt + 12);
            w->bits = u16(fmt + 14);
            if (w->tag == 0xFFFE && got >= 26) w->tag = u16(fmt + 24);  // the sub-format GUID's first word
            have_fmt = true;
        } else if (!memcmp(ck, "data", 4)) {
            if (!have_fmt) return WAV_BAD_FILE;
            w->data_pos = body;
            w->data_len = len;
            bool ok = w->channels >= 1 && w->rate > 0 && w->block == w->channels * ((w->bits + 7) / 8) &&
                      ((w->tag == 1 && (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)) ||
                       (w->tag == 3 && (w->bits == 32 || w->bits == 64)));
            return ok ? WAV_OK : WAV_BAD_FORMAT;
        }
        if (_fseeki64(f, body + (long long)len + (long long)(len & 1), SEEK_SET)) break;
    }
    return WAV_BAD_FILE;
}

bool wav_read(FILE* f, const WavFmt& w, Pcm* out) {
    _fseeki64(f, 0, SEEK_END);
    long long end = _ftelli64(f);
    long long len = std::min(w.data_len, end - w.data_pos);  // a truncated file: take what is there
    size_t frames = (size_t)(len / w.block);
    std::vector<unsigned char> raw(frames * (size_t)w.block);
    _fseeki64(f, w.data_pos, SEEK_SET);
    if (fread(raw.data(), 1, raw.size(), f) != raw.size()) return false;
    out->rate = w.rate;
    out->channels = w.channels;
    out->data.resize(frames * (size_t)w.channels);
    const unsigned char* p = raw.data();
    for (float& v : out->data) {
        if (w.tag == 3 && w.bits == 32) {
            float x;
            memcpy(&x, p, 4);
            v = x;
        } else if (w.tag == 3) {
            double x;
            memcpy(&x, p, 8);
            v = (float)x;
        } else if (w.bits == 8) {
            v = (float)((int)p[0] - 128) / 128.0f;
        } else if (w.bits == 16) {
            v = (float)(int16_t)u16(p) / 32768.0f;
        } else if (w.bits == 24) {
            int32_t x = (int32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            v = (float)((double)x / 8388608.0);
        } else {
            v = (float)((double)(int32_t)u32(p) / 2147483648.0);
        }
        p += w.bits / 8;
    }
    return true;
}

// ---------------------------------------------------------------- Media Foundation

struct ComInit {
    bool ok;
    ComInit() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ok = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: already initialised otherwise, which is fine
    }
    ~ComInit() { if (ok) CoUninitialize(); }
};

struct MfInit {
    bool ok;
    MfInit() : ok(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {}
    ~MfInit() { if (ok) MFShutdown(); }
};

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// Media Foundation's audio sub-types are format tags in the first field of the GUID
// (older SDK headers lack some of the named constants)
enum : uint32_t { TAG_PCM = 0x1, TAG_FLOAT = 0x3, TAG_MPEG = 0x50, TAG_MP3 = 0x55, TAG_WMA8 = 0x161, TAG_WMA9 = 0x162,
                  TAG_WMA_LOSSLESS = 0x163, TAG_AAC = 0x1610, TAG_ALAC = 0x6C61, TAG_OPUS = 0x704F, TAG_FLAC = 0xF1AC };

std::wstring codec_name(uint32_t tag) {
    switch (tag) {
    case TAG_MP3: case TAG_MPEG: return L"MP3";
    case TAG_AAC: return L"AAC";
    case TAG_FLAC: return L"FLAC";
    case TAG_ALAC: return L"ALAC";
    case TAG_OPUS: return L"Opus";
    case TAG_WMA8: case TAG_WMA9: case TAG_WMA_LOSSLESS: return L"WMA";
    case TAG_PCM: return L"PCM";
    case TAG_FLOAT: return L"Float";
    default: return L"Audio";
    }
}

// Opens `path` with a source reader set to deliver 32-bit float PCM from the first audio
// stream; fills info from the stream's native and decoded formats.
bool mf_open(const std::wstring& path, IMFSourceReader** reader, AudioInfo* info) {
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader))) return false;
    IMFSourceReader* r = *reader;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) return false;
    Com<IMFMediaType> native;
    if (SUCCEEDED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native))) {
        GUID sub = GUID_NULL;
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        uint32_t tag = (uint32_t)sub.Data1;
        info->codec = codec_name(tag);
        UINT32 bits = 0;
        bool lossless = tag == TAG_FLAC || tag == TAG_ALAC || tag == TAG_PCM || tag == TAG_FLOAT || tag == TAG_WMA_LOSSLESS;
        if (lossless && SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits))) info->bits = (int)bits;
    }
    Com<IMFMediaType> want;
    if (FAILED(MFCreateMediaType(&want))) return false;
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want.p))) return false;
    Com<IMFMediaType> got;
    if (FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) return false;
    UINT32 ch = 0, rate = 0, bits = 0;
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    got->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    if (!ch || !rate || bits != 32) return false;
    info->channels = (int)ch;
    info->rate = (int)rate;
    return true;
}

bool mf_read(const std::wstring& path, Pcm* out) {
    ComInit com;
    MfInit mf;
    if (!mf.ok) return false;
    Com<IMFSourceReader> r;
    AudioInfo info;
    if (!mf_open(path, &r, &info)) return false;
    out->rate = info.rate;
    out->channels = info.channels;
    out->data.clear();
    for (;;) {
        DWORD flags = 0;
        Com<IMFSample> sample;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample)))
            return false;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {  // still float at the same rate and layout?
            Com<IMFMediaType> t;
            UINT32 ch = 0, rate = 0, bits = 0;
            if (FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &t))) return false;
            t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            if ((int)ch != out->channels || (int)rate != out->rate || bits != 32) return false;
        }
        if (sample.p) {
            Com<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
            BYTE* p = nullptr;
            DWORD len = 0;
            if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
            size_t n = len / sizeof(float);
            size_t at = out->data.size();
            out->data.resize(at + n);
            memcpy(out->data.data() + at, p, n * sizeof(float));
            buf->Unlock();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    out->data.resize(out->frames() * (size_t)out->channels);
    return true;
}

// ---------------------------------------------------------------- conversion

// More than two channels to stereo (WAVE order: FL FR FC LFE BL BR SL SR ...): the front
// pair, plus the centre and the surrounds at -3 dB on their sides; the LFE is left out.
void downmix_to_stereo(Pcm* a) {
    if (a->channels <= 2) return;
    const size_t n = a->frames(), C = (size_t)a->channels;
    std::vector<float> out(n * 2);
    const float k = 0.70710678f;
    for (size_t i = 0; i < n; i++) {
        const float* s = &a->data[i * C];
        float l = s[0], r = s[1];
        if (C >= 3) l += k * s[2], r += k * s[2];
        for (size_t c = 4; c < C; c++) (c % 2 == 0 ? l : r) += k * s[c];
        out[2 * i] = l, out[2 * i + 1] = r;
    }
    a->data.swap(out);
    a->channels = 2;
}

void to_channels(Pcm* a, int channels) {
    if (a->channels == channels) return;
    const size_t n = a->frames();
    std::vector<float> out(n * (size_t)channels);
    if (channels == 2)  // mono -> stereo
        for (size_t i = 0; i < n; i++) out[2 * i] = out[2 * i + 1] = a->data[i];
    else  // stereo -> mono
        for (size_t i = 0; i < n; i++) out[i] = 0.5f * (a->data[2 * i] + a->data[2 * i + 1]);
    a->data.swap(out);
    a->channels = channels;
}

double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

// Windowed-sinc resampling (Kaiser, beta 10): the low-pass at 0.98 of the lower Nyquist,
// 48 zero crossings of it on each side, taps from a table of 512 phases per crossing
// (linearly interpolated). 44.1 -> 48 -> 44.1 kHz comes back within about -38 dB.
void resample(Pcm* a, int rate) {
    if (a->rate == rate || a->data.empty()) {
        a->rate = rate;
        return;
    }
    const double ratio = (double)rate / a->rate;           // output samples per input sample
    const double fc = 0.98 * std::min(1.0, ratio);           // cutoff, fraction of the input Nyquist
    const int zc = 48, phases = 512;
    const double half = zc / fc;                             // kernel half-width, input samples
    const double beta = 10.0, i0b = bessel_i0(beta);
    std::vector<double> table((size_t)zc * phases + 2);      // kernel at t = j / (phases * fc)
    for (size_t j = 0; j < table.size(); j++) {
        double t = (double)j / phases;                       // in zero crossings of the cutoff
        double u = PI * t;
        double sinc = t == 0 ? 1.0 : std::sin(u) / u;
        double r = t / zc;
        table[j] = r >= 1 ? 0.0 : sinc * bessel_i0(beta * std::sqrt(1 - r * r)) / i0b;
    }
    auto kern = [&](double dt) {                             // dt in input samples
        double t = std::fabs(dt) * fc * phases;
        size_t j = (size_t)t;
        if (j + 1 >= table.size()) return 0.0;
        double f = t - (double)j;
        return table[j] + f * (table[j + 1] - table[j]);
    };
    const size_t n_in = a->frames(), C = (size_t)a->channels;
    const size_t n_out = (size_t)std::floor((double)n_in * ratio + 0.5);
    std::vector<float> out(n_out * C);
    auto run = [&](size_t i0, size_t i1) {
        std::vector<double> acc(C);
        for (size_t i = i0; i < i1; i++) {
            double pos = (double)i / ratio;
            long long lo = (long long)std::ceil(pos - half), hi = (long long)std::floor(pos + half);
            std::fill(acc.begin(), acc.end(), 0.0);
            for (long long k = std::max(0LL, lo); k <= hi && k < (long long)n_in; k++) {
                double w = kern(pos - (double)k);
                const float* s = &a->data[(size_t)k * C];
                for (size_t c = 0; c < C; c++) acc[c] += w * s[c];
            }
            for (size_t c = 0; c < C; c++) out[i * C + c] = (float)(acc[c] * fc);
        }
    };
    // each output sample on its own: the threads only split the range
    size_t T = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (size_t t = 0; t < T; t++) th.emplace_back(run, n_out * t / T, n_out * (t + 1) / T);
    for (auto& x : th) x.join();
    a->data.swap(out);
    a->rate = rate;
}

bool decode(const std::wstring& path, Pcm* out) {
    {
        File f(path);
        if (!f.f) return false;
        WavFmt w;
        int err = wav_scan(f.f, &w);
        if (err == WAV_OK) return wav_read(f.f, w, out);
    }
    return mf_read(path, out);  // other formats, and WAVs in a coding we do not read (ADPCM, ...)
}

}  // namespace

int audio_info(const std::wstring& path, AudioInfo* info) {
    bool bad_wav = false;
    {
        File f(path);
        if (!f.f) return WAV_OPEN_FAILED;
        WavFmt w;
        int err = wav_scan(f.f, &w);
        if (err == WAV_OK) {
            info->codec = w.tag == 3 ? L"Float" : L"PCM";
            info->rate = w.rate;
            info->channels = w.channels;
            info->bits = w.bits;
            return 0;
        }
        bad_wav = err == WAV_BAD_FORMAT;
    }
    ComInit com;
    MfInit mf;
    Com<IMFSourceReader> r;
    *info = AudioInfo();
    if (mf.ok && mf_open(path, &r, info)) return 0;
    return bad_wav ? WAV_BAD_FORMAT : WAV_BAD_FILE;  // a WAV coding nothing here reads
}

bool load_audio(const std::wstring& path, int rate, int channels, Audio* out) {
    Pcm a;
    if (!decode(path, &a) || a.channels < 1 || a.rate < 1) return false;
    downmix_to_stereo(&a);
    if (channels) to_channels(&a, channels);
    if (rate) resample(&a, rate);
    out->rate = a.rate;
    out->channels = a.channels;
    out->data.resize(a.data.size());
    for (size_t i = 0; i < a.data.size(); i++)
        out->data[i] = (int16_t)std::clamp(std::lround((double)a.data[i] * 32768.0), -32768L, 32767L);
    return true;
}

}  // namespace utagoe
