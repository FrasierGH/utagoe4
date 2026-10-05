#include "engine.hpp"

#include "v4.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "dsp.hpp"

namespace utagoe {

namespace {

const double PREROLL_GAIN = 0.95;  // level used for the intro / pre-roll blocks
const int SOFT_KNEE = 0x7999;      // 31129: soft clipping starts here

typedef std::vector<int32_t> IVec;

// Sequential reader with mmioRead semantics: fills the *front* of the buffer
// and leaves the rest as it was (the original never clears its buffers).
struct Source {
    const int16_t* data;
    long long frames;
    int ch;
    long long pos = 0;
    explicit Source(const Audio& a) : data(a.data.data()), frames((long long)a.frames()), ch(a.channels) {}
    void seek(long long p) { pos = p; }
    long long tell() const { return pos; }
    int read(std::vector<int16_t>* buf, long long count = -1) {
        long long n = count < 0 ? (long long)buf->size() / ch : count;
        long long p = pos;
        if (p >= frames) return 0;
        long long got = std::min(n, frames - p);
        int16_t* out = buf->data();
        if (p < 0) {  // before the start of the file: silence
            long long lead = std::min(-p, got);
            std::fill(out, out + lead * ch, (int16_t)0);
            std::copy(data, data + (got - lead) * ch, out + lead * ch);
        } else {
            std::copy(data + p * ch, data + (p + got) * ch, out);
        }
        pos = p + got;
        return (int)got;
    }
};

// TVocalFunc: block buffers and per-block primitives.
struct Work {
    int ch, rate, os, blk, iblk, sign = 1;
    std::vector<int16_t> raw_o, raw_i;
    IVec o[2], in[2], od, id, ios[2], ids;

    Work(int ch_, int rate_, double block_sec, int os_ = 1) : ch(ch_), rate(rate_), os(os_) {
        blk = (int)(rate * block_sec);              // original block      (0x1a8)
        iblk = (int)(rate * block_sec * 1.5);       // instrumental window (0x1ac)
        raw_o.assign((size_t)blk * ch, 0);
        raw_i.assign((size_t)iblk * ch, 0);
        for (int c = 0; c < 2; c++) o[c].assign(blk, 0), in[c].assign(iblk, 0);
        od.assign(blk, 0);
        id.assign(iblk, 0);
    }
    void clear() {
        std::fill(raw_o.begin(), raw_o.end(), (int16_t)0);
        std::fill(raw_i.begin(), raw_i.end(), (int16_t)0);
        for (int c = 0; c < 2; c++) std::fill(o[c].begin(), o[c].end(), 0), std::fill(in[c].begin(), in[c].end(), 0);
        std::fill(od.begin(), od.end(), 0);
        std::fill(id.begin(), id.end(), 0);
    }
    void deinterleave() {  // 0x40e668
        for (int k = 0; k < blk; k++) {
            o[0][k] = raw_o[(size_t)k * ch];
            o[1][k] = ch >= 2 ? raw_o[(size_t)k * ch + 1] : 0;
        }
        for (int k = 0; k < iblk; k++) {
            in[0][k] = raw_i[(size_t)k * ch] * sign;
            in[1][k] = ch >= 2 ? raw_i[(size_t)k * ch + 1] * sign : 0;
        }
    }
    void make_diff() {  // 0x40e820
        for (int k = 0; k < blk; k++) od[k] = o[0][k] - o[1][k];
        for (int k = 0; k < iblk; k++) id[k] = in[0][k] - in[1][k];
    }
    void make_mono() {  // 0x40e87c
        for (int k = 0; k < blk; k++) od[k] = o[0][k] + o[1][k];
        for (int k = 0; k < iblk; k++) id[k] = in[0][k] + in[1][k];
    }
    static IVec interp(const IVec& x, int os) {
        IVec out((x.size() - 1) * os);
        for (size_t j = 0; j + 1 < x.size(); j++) {
            double step = (double)(x[j + 1] - x[j]), base = (double)x[j];
            for (int k = 0; k < os; k++) out[j * os + k] = (int32_t)std::trunc(step * ((double)k / os) + base);
        }
        return out;
    }
    void oversample_inst() { ios[0] = interp(in[0], os), ios[1] = interp(in[1], os); }  // 0x40e8d8
    void oversample_id() { ids = interp(id, os); }                                      // 0x40e980
};

struct Chans {
    const IVec* v[2];
    int n;
};

Chans chans(const IVec* a, const IVec* b, int n) { return Chans{{a, b}, n}; }

// Best alignment (0x40eaf0 / 0x40ed88 / 0x40e9f4).  Cost per candidate:
// sum|r| + |r0| + sum|r[i-1]-r[i]| (smooth) or sum|r|, r = orig[i] - inst[ofs + i*stride].
// Candidates centre-out, first minimum wins.
int search_offset(const Chans& orig, const Chans& inst, int center, int rng, int length, int step = 1,
                  int stride = 1, bool smooth = true) {
    long long best_cost = std::numeric_limits<long long>::max();
    int best = center;
    auto evaluate = [&](int ofs) {
        long long cost = 0;
        int32_t prev[2] = {0, 0};
        for (int i = 0; i < length; i++) {
            long long idx = (long long)ofs + (long long)i * stride;
            for (int c = 0; c < orig.n; c++) {
                const IVec& in = *inst.v[c];
                long long j = std::min<long long>(std::max<long long>(idx, 0), (long long)in.size() - 1);
                int32_t r = (*orig.v[c])[i] - in[(size_t)j];
                cost += std::llabs(r);
                if (smooth) {
                    cost += std::llabs((long long)prev[c] - r);
                    prev[c] = r;
                }
            }
            if (cost > best_cost) return;
        }
        if (cost < best_cost) best_cost = cost, best = ofs;
    };
    for (int d = 0; d <= rng; d += step) {
        evaluate(center + d);
        if (d && center - d >= 0) evaluate(center - d);
    }
    return best;
}

// Instrumental level minimising sum|trunc(orig - g*inst)| (0x40f008 / 0x40f22c).
double search_level(const Chans& orig, const Chans& inst, int ofs, int length, double center, double rng,
                    double step, int stride, bool ascending) {
    std::vector<double> cands;
    if (ascending) {
        double hi = center + rng;
        for (double g = center - rng; g <= hi; g += step) cands.push_back(g);
    } else {
        for (double d = 0.0; d <= rng; d += step) {
            cands.push_back(center + d);
            if (d != 0.0) cands.push_back(center - d);
        }
    }
    double best_cost = std::numeric_limits<double>::infinity(), best = cands.empty() ? center : cands[0];
    for (double g : cands) {
        double cost = 0.0;
        bool over = false;
        for (int i = 0; i < length && !over; i++) {
            size_t idx = (size_t)ofs + (size_t)i * stride;
            for (int c = 0; c < orig.n; c++) {
                const IVec& in = *inst.v[c];
                double s = (double)in[std::min(idx, in.size() - 1)];
                cost += std::fabs(std::trunc((double)(*orig.v[c])[i] - g * s));
            }
            if (cost > best_cost) over = true;
        }
        if (!over && cost < best_cost) best_cost = cost, best = g;
    }
    return best;
}

double coarse_fine_level(const Chans& o, const Chans& i, int ofs, int length, int stride = 1, bool ascending = false) {
    double lvl = search_level(o, i, ofs, length, 1.0, 0.2, 0.02, stride, ascending);
    return search_level(o, i, ofs, length, lvl, 0.03, 0.002, stride, ascending);
}

// Time-domain subtraction over the whole block (0x40f3a0 / 0x40f410 / 0x40f498 / 0x40f514).
void subtract(const Work& w, int ofs, double g, bool mono, int stride, IVec out[2]) {
    const IVec* src = stride == 1 ? w.in : w.ios;
    out[0].resize(w.blk);
    out[1].resize(w.blk);
    for (int k = 0; k < w.blk; k++) {
        size_t idx = (size_t)ofs + (size_t)k * stride;
        if (mono) {
            double s = (double)(src[0][idx] + src[1][idx]);
            int32_t v = (int32_t)std::trunc(((double)(w.o[0][k] + w.o[1][k]) - s * g) * 0.5);
            out[0][k] = out[1][k] = v;
        } else {
            for (int c = 0; c < 2; c++) out[c][k] = (int32_t)std::trunc((double)w.o[c][k] - (double)src[c][idx] * g);
        }
    }
}

int16_t soft_clip(long long x) {  // 0x40e728
    if (x > SOFT_KNEE) x = (x - SOFT_KNEE) / 2 + SOFT_KNEE;
    if (x < -SOFT_KNEE) x = (x + SOFT_KNEE) / 2 - SOFT_KNEE;
    return (int16_t)std::max<long long>(-32768, std::min<long long>(32767, x));
}

// floor(a / b) for the original's x87 extended-precision division, made exact.
long long exact_floor_div(double a, double b) {
    long long q = (long long)std::floor(a / b);
    while (std::fma(-(double)q, b, a) < 0) q--;
    while (std::fma(-(double)(q + 1), b, a) >= 0) q++;
    return q;
}

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, f);
    vswprintf(buf, 512, f, ap);
    va_end(ap);
    return buf;
}

}  // namespace

// ==========================================================================

UtagoeRip::UtagoeRip(const Audio& orig, const Audio* inst, const Settings& cfg, Callbacks cb, bool original_timing,
                     AnalysisCache* cache, const std::wstring& orig_key, const std::wstring& inst_key)
    : orig_(orig), inst_(inst), cfg_(cfg), cb_(std::move(cb)), original_timing_(original_timing), cache_(cache),
      orig_key_(orig_key), inst_key_(inst_key), ch_(orig.channels), rate_(orig.rate) {
    if (ch_ < 2) {  // 0x40fbb4: mono input forces normal mode, no centre focus
        cfg_.proc_mode = PROC_NORMAL;
        cfg_.cntr_flag = false;
    }
}

void UtagoeRip::check() {
    if (cb_.cancel && cb_.cancel()) {
        cancelled_ = true;
        throw Cancelled();
    }
}

// ---------------------------------------------------------------- 0x410570: trial run

TrialResult UtagoeRip::trial(int ofs0, int base, bool level_search, int phase, int max_sec, int rng) {
    Work w(ch_, rate_, cfg_.block_sec() * 2.0);
    w.sign = phase ? -1 : 1;
    Source so(orig_), si(*inst_);
    long long max_blocks = max_sec ? exact_floor_div(max_sec, cfg_.block_sec()) : -1;
    int level_every = level_search ? 30 : 0x7FFFFFFF;

    if (ofs0 < 0) so.seek(-ofs0 + rng), si.seek(rng);
    else so.seek(rng), si.seek(ofs0 + rng);
    int q = w.blk / 4, h = w.blk / 2;
    long long p_o = so.tell(), p_i = si.tell();
    struct Rec { int ofs; long long voc, org; };
    std::vector<Rec> recs;
    double lvl_sum = 0;
    int lvl_n = 0, cnt = 0;
    int mode = cfg_.proc_mode;
    IVec out[2];
    for (;;) {
        p_i -= rng;
        si.seek(p_i);
        if (!so.read(&w.raw_o) || !si.read(&w.raw_i)) break;
        if (max_blocks >= 0 && (long long)recs.size() >= max_blocks) break;
        check();
        w.deinterleave();
        Chans o, in;
        if (mode == PROC_NORMAL) {
            o = chans(&w.o[0], &w.o[1], ch_ >= 2 ? 2 : 1);
            in = chans(&w.in[0], &w.in[1], ch_ >= 2 ? 2 : 1);
        } else {
            mode == PROC_LR_DIFF ? w.make_diff() : w.make_mono();
            o = chans(&w.od, nullptr, 1);
            in = chans(&w.id, nullptr, 1);
        }
        int found = search_offset(o, in, rng, rng, w.blk);
        int c = cnt++;
        if (c >= level_every) {
            lvl_sum += coarse_fine_level(o, in, found, w.blk);
            lvl_n++;
            cnt = 0;
        }
        subtract(w, found, 1.0, mode == PROC_MONO, 1, out);
        long long voc = 0, org = 0;
        for (int cc = 0; cc < 2; cc++)
            for (int k = q; k < q + h; k++) voc += std::llabs(out[cc][k]), org += std::llabs(w.o[cc][k]);
        recs.push_back({found - rng, voc, org});
        p_i += found + base + h;
        p_o += h;
        so.seek(p_o);
    }
    TrialResult t;
    size_t n = recs.size();
    if (!n) {
        t.voc = std::numeric_limits<double>::infinity();
        return t;
    }
    long long so_sum = 0;
    double voc = 0, org = 0;
    for (const Rec& r : recs) so_sum += r.ofs, voc += (double)r.voc, org += (double)r.org;
    t.ofs = (double)so_sum / n;
    t.voc = voc / n;
    t.org = org / n;
    double dev = 0;
    for (const Rec& r : recs) dev += std::fabs(r.ofs - t.ofs);
    t.bnsn = dev / n;
    t.vol = lvl_n ? lvl_sum / lvl_n : 1.0;
    return t;
}

void UtagoeRip::log_trial(int rng, const TrialResult& t) {
    log(fmt(L"range:%d vol:%f voc:%f org:%f ofs:%f bnsn:%f", rng, t.vol, t.voc, t.org, t.ofs, t.bnsn));
}

// ---------------------------------------------------------------- 0x411a7c: "normal" intro analysis

int UtagoeRip::intro_simple(int phase) {
    Work w(ch_, rate_, 1.0);
    w.sign = phase ? -1 : 1;
    Source so(orig_), si(*inst_);
    auto onset = [&](Source& src, std::vector<int16_t>* raw, const IVec& arr, int n) {
        long long count = 0;
        while (src.read(raw)) {
            check();
            w.deinterleave();
            w.make_mono();
            for (int k = 0; k < n; k++)
                if (arr[k] >= 0x80) return count + k;
            count += n;
        }
        return count;
    };
    long long o_on = onset(so, &w.raw_o, w.od, w.blk);
    long long i_on = onset(si, &w.raw_i, w.id, w.iblk);
    long long i_start = std::max<long long>(0, i_on - w.blk / 4);
    so.seek(o_on);
    si.seek(i_start);
    so.read(&w.raw_o);
    si.read(&w.raw_i);
    w.deinterleave();
    w.make_mono();
    int found = search_offset(chans(&w.od, nullptr, 1), chans(&w.id, nullptr, 1), w.blk / 4, w.blk / 4, w.blk / 5);
    return (int)(found + i_start - o_on);
}

// ---------------------------------------------------------------- 0x411cd4: "detailed" intro analysis

int UtagoeRip::intro_detailed(bool mono, int phase) {
    const int seg = (int)(rate_ * 0.02), n_o = 3000, n_i = 6000;
    Work w(ch_, rate_, 0.2);
    w.sign = phase ? -1 : 1;
    Source so(orig_), si(*inst_);
    auto derive = [&] { mono ? w.make_mono() : w.make_diff(); };
    IVec env_o(n_o), b(n_o / 2 + n_i + n_o, 0);
    for (int k = 0; k < n_o; k++) {
        if (k % 100 == 0) check();
        so.read(&w.raw_o, seg);
        w.deinterleave();
        derive();
        long long s = 0;
        for (int j = 0; j < seg; j++) s += std::llabs(w.od[j]);
        env_o[k] = (int32_t)(s / seg);
    }
    for (int k = 0; k < n_i; k++) {
        if (k % 100 == 0) check();
        si.read(&w.raw_i, seg);
        w.deinterleave();
        derive();
        long long s = 0;
        for (int j = 0; j < seg; j++) s += std::llabs(w.id[j]);
        b[n_o / 2 + k] = (int32_t)(s / seg);
    }
    w.clear();
    int coarse = (search_offset(chans(&env_o, nullptr, 1), chans(&b, nullptr, 1), n_o / 2, n_o / 2, n_o, 1, 1, false)
                  - n_o / 2) * seg;
    int idx = n_o;
    for (int k = 0; k < n_o; k++)
        if (env_o[k] > 0x100) { idx = k; break; }
    long long start = (long long)idx * seg;
    if (coarse >= 0) so.seek(start), si.seek(start + coarse - 2 * seg);
    else so.seek(start - coarse), si.seek(start - 2 * seg);
    so.read(&w.raw_o);
    si.read(&w.raw_i);
    w.deinterleave();
    w.make_mono();
    int found = search_offset(chans(&w.od, nullptr, 1), chans(&w.id, nullptr, 1), 2 * seg, 2 * seg, w.blk);
    return found + coarse - 2 * seg;
}

// ---------------------------------------------------------------- 0x4102c4: initial offset + phase

void UtagoeRip::initial_offset(int* ofs_out, int* phase_out) {
    double best_ratio = std::numeric_limits<double>::infinity();
    int result = 0, phase_found = 0;
    static const wchar_t* labels[3] = {L"Simple analysis        ", L"Detailed analysis LR   ", L"Detailed analysis mono "};
    for (int phase = 0; phase < 2; phase++) {
        for (int m = 0; m < 3; m++) {
            int ofs = m == 0 ? intro_simple(phase) : intro_detailed(m == 2, phase);
            TrialResult t = trial(ofs, 0, false, phase, 30, 20);
            log(fmt(L"%lsVoc:%f Org:%f ofs:%d phase:%d", labels[m], t.voc, t.org, ofs, phase));
            if (t.org != 0) {
                double ratio = t.voc / t.org;
                if (ratio < 0.8) {
                    log(fmt(L"Initial offset:%d phase:%d", ofs, phase));
                    *ofs_out = ofs, *phase_out = phase;
                    return;
                }
                if (ratio < best_ratio) best_ratio = ratio, result = ofs, phase_found = phase;
            }
        }
    }
    log(fmt(L"Initial offset:%d phase:%d", result, phase_found));
    *ofs_out = result, *phase_out = phase_found;
}

// ---------------------------------------------------------------- 0x40fdc4: automatic analysis

bool UtagoeRip::auto_analysis() {
    if (cache_ && cache_->valid && cache_->orig == orig_key_ && cache_->inst == inst_key_ &&
        cache_->proc_mode == cfg_.proc_mode && cache_->block_sec == cfg_.block_sec()) {
        analysis_ = cache_->analysis;
        return true;
    }
    if (cfg_.intro_mode && cfg_.adpt_mode && cfg_.level_adpt && cfg_.krk_phase) return false;  // nothing automatic
    status(STATUS_ANALYZING);
    try {
        do_auto_analysis();
    } catch (Cancelled&) {
        if (cache_) cache_->valid = false;
        status(STATUS_NONE);
        throw;
    }
    status(STATUS_NONE);
    if (cache_) {
        cache_->valid = true;
        cache_->orig = orig_key_, cache_->inst = inst_key_;
        cache_->proc_mode = cfg_.proc_mode, cache_->block_sec = cfg_.block_sec();
        cache_->analysis = analysis_;
    }
    return true;
}

void UtagoeRip::do_auto_analysis() {
    Analysis a;
    initial_offset(&a.ofs0, &a.phase);
    double best_voc = std::numeric_limits<double>::infinity();
    bool found = false;
    int spread = 0, rng = 20;
    {
        TrialResult t = trial(a.ofs0, 0, false, a.phase, 120, rng);
        log_trial(rng, t);
        if (t.voc < t.org) {
            a.base = (int)t.ofs;
            spread = (int)(std::fabs(t.ofs - a.base) + t.bnsn + 0.5);
            found = true;
        } else if (t.voc < best_voc) {
            best_voc = t.voc;
            a.base = (int)t.ofs;
            spread = (int)(std::fabs(t.ofs - a.base) + t.bnsn + 0.5);
        }
    }
    if (found) {
        int hi = spread + 8, best_r = 0;
        double best = std::numeric_limits<double>::infinity(), prev_voc = 0;
        bool have_prev = false;
        rng = spread;
        for (; rng <= hi; rng++) {
            TrialResult t = trial(a.ofs0, a.base, false, a.phase, 120, rng);
            log_trial(rng, t);
            if (t.voc < t.org) {
                if (have_prev) {
                    if (prev_voc == 0.0) { rng--; break; }
                    double ratio = t.voc / prev_voc;
                    if (0.997 < ratio && ratio < 1.003) { rng--; break; }
                }
                have_prev = true, prev_voc = t.voc;
            }
            if (t.voc < best) best = t.voc, best_r = rng;
        }
        if (rng > hi) rng = best_r;
    } else {
        rng = spread + 1;
    }
    TrialResult t = trial(a.ofs0, a.base, true, a.phase, 0, rng);
    log_trial(rng, t);
    a.range = rng;
    a.vol = t.vol;
    analysis_ = a;
    log(fmt(L"Selected BaseOfs:%d Range:%d", a.base, a.range));
    if (cb_.timestamp) cb_.timestamp();
}

// ---------------------------------------------------------------- 0x410cfc: main processing

Audio UtagoeRip::run() {
    if (!inst_) return run_original_only();
    if (cfg_.v4) return run_v4();
    Audio empty;
    empty.rate = rate_, empty.channels = ch_;
    const Settings& cfg = cfg_;
    bool analysed;
    try {
        analysed = auto_analysis();
    } catch (Cancelled&) {
        return empty;  // the output file was already created; it stays empty
    }
    const Analysis& a = analysis_;

    bool wave_merge = cfg.merge_mode == MERGE_WAVEFORM;
    bool oversampled = wave_merge && cfg.ovsp_flag;
    int os = oversampled ? cfg.oversample() : 1;
    bool adaptive = false;
    double gain;
    if (!wave_merge) gain = analysed ? a.vol : PREROLL_GAIN;
    else if (cfg.level_adpt == LEVEL_AUTO_AVERAGED) gain = a.vol;
    else if (cfg.level_adpt == LEVEL_AUTO_ADAPTIVE) adaptive = true, gain = 1.0;
    else if (cfg.level_adpt == LEVEL_MANUAL) gain = cfg.klvl();
    else gain = 1.0;

    int base = 0, rng = 0;
    if (cfg.adpt_mode == ADPT_AUTO) base = a.base, rng = a.range;
    else if (cfg.adpt_mode == ADPT_MANUAL) rng = cfg.adpt_range();

    int phase = cfg.krk_phase == PHASE_AUTO ? a.phase : cfg.krk_phase == PHASE_INVERTED ? 1 : 0;

    int ofs0 = 0;
    try {
        if (cfg.intro_mode == INTRO_AUTO) ofs0 = a.ofs0;
        else if (cfg.intro_mode == INTRO_NORMAL) ofs0 = intro_simple(phase);
        else if (cfg.intro_mode == INTRO_DETAILED) {
            status(STATUS_PREPARING);
            ofs0 = intro_detailed(false, phase);
            status(STATUS_NONE);
        }
    } catch (Cancelled&) {
        status(STATUS_NONE);
        return empty;
    }

    Work w(ch_, rate_, cfg.block_sec() * 2.0, os);
    w.sign = phase ? -1 : 1;
    Source so(orig_), si(*inst_);
    bool mono = cfg.proc_mode == PROC_MONO;
    int q = w.blk / 4, h = w.blk / 2;

    // Waveform: output samples.  Frequency: per-channel input streams of the
    // FFT stage and the level in effect for each sample.
    std::vector<double> outs[2], ostr[2], istr[2], gstr;
    IVec tmp[2];
    auto emit_fft = [&](int from, int count, const IVec* src_o, const IVec* src_i, int i_from, double g) {
        for (int k = 0; k < count; k++) {
            if (mono) {
                int32_t so_ = src_o[0][from + k] + src_o[1][from + k];
                int32_t si_ = src_i[0][i_from + k] + src_i[1][i_from + k];
                ostr[0].push_back((double)(so_ / 2));
                istr[0].push_back((double)(si_ / 2));
            } else {
                for (int c = 0; c < 2; c++) {
                    ostr[c].push_back((double)src_o[c][from + k]);
                    istr[c].push_back((double)src_i[c][i_from + k]);
                }
            }
            gstr.push_back(g);
        }
    };
    auto emit_wave = [&](const IVec out[2], int from, int count) {
        for (int c = 0; c < 2; c++)
            for (int k = 0; k < count; k++) outs[c].push_back((double)out[c][from + k]);
    };
    auto preroll = [&](int count, bool read_inst) {  // 0x41211c
        w.clear();
        for (int remaining = count; remaining > 0; remaining -= w.blk) {
            int n = std::min(remaining, w.blk);
            so.read(&w.raw_o, n);
            if (read_inst) si.read(&w.raw_i, n);
            w.deinterleave();
            if (wave_merge) {
                subtract(w, 0, PREROLL_GAIN, mono, 1, tmp);
                emit_wave(tmp, 0, n);
            } else {
                emit_fft(0, n, w.o, w.in, 0, PREROLL_GAIN);
            }
        }
    };

    so.seek(0);
    if (ofs0 < 0) si.seek(rng), preroll(rng - ofs0, false);
    else si.seek(ofs0 + rng), preroll(rng, false);
    long long p_o = so.tell(), p_i = si.tell();
    preroll(q, true);
    so.seek(p_o);
    si.seek(p_i);

    long long total = std::min<long long>((long long)orig_.frames(), (long long)inst_->frames());
    long long written = (ofs0 < 0 ? rng - ofs0 : rng) + q;
    int tick = 0, mode = cfg.proc_mode;
    for (;;) {
        p_i -= rng;
        si.seek(p_i);
        if (!so.read(&w.raw_o) || !si.read(&w.raw_i)) break;
        w.deinterleave();
        Chans o, in;
        if (mode == PROC_NORMAL) {
            o = chans(&w.o[0], &w.o[1], ch_ >= 2 ? 2 : 1);
            in = chans(&w.in[0], &w.in[1], ch_ >= 2 ? 2 : 1);
        } else {
            mode == PROC_LR_DIFF ? w.make_diff() : w.make_mono();
            o = chans(&w.od, nullptr, 1);
            in = chans(&w.id, nullptr, 1);
        }
        int found;
        if (!oversampled) {
            found = search_offset(o, in, rng, rng, w.blk);
            double g = adaptive ? coarse_fine_level(o, in, found, w.blk) : gain;
            if (wave_merge) {
                subtract(w, found, g, mono, 1, tmp);
                emit_wave(tmp, q, h);
            } else {
                emit_fft(q, h, w.o, w.in, found + q, g);
            }
        } else {
            w.oversample_inst();
            Chans s_in;
            if (mode == PROC_NORMAL) {
                s_in = chans(&w.ios[0], &w.ios[1], o.n);
            } else {
                w.oversample_id();
                s_in = chans(&w.ids, nullptr, 1);
            }
            found = search_offset(o, s_in, rng * os, rng * os, w.blk, os, os);
            found = search_offset(o, s_in, found, os - 1, w.blk, 1, os);
            double g = adaptive ? coarse_fine_level(o, s_in, found, w.blk, os, true) : gain;
            subtract(w, found, g, mono, os, tmp);
            emit_wave(tmp, q, h);
            found /= os;
        }
        p_i += found + base + h;
        p_o += h;
        so.seek(p_o);
        written += h;
        if (tick++ > 10) {  // the original refreshes the progress display every 12 blocks
            progress(std::min(1.0, (double)written / (double)std::max<long long>(1, total)));
            tick = 0;
        }
        if (cb_.cancel && cb_.cancel()) {
            cancelled_ = true;
            break;
        }
    }

    if (wave_merge) return finish(outs);

    // FFT stage: one ThVocalFFT stream per channel (the original ran them in threads too)
    size_t pad = original_timing_ ? 0 : LATENCY;
    int nch = mono ? 1 : std::min(2, ch_);
    size_t len = gstr.size();
    double last_g = len ? gstr.back() : PREROLL_GAIN;
    gstr.resize(len + pad, last_g);
    std::vector<double> res[2];
    std::thread workers[2];
    for (int c = 0; c < nch; c++) {
        ostr[c].resize(len + pad, 0.0);
        istr[c].resize(len + pad, 0.0);
        workers[c] = std::thread([&, c] {
            std::vector<double> y = vocal_fft(ostr[c].data(), istr[c].data(), gstr.data(), len + pad, cfg.kvol(),
                                              cfg.sound_qty == QUALITY_PRIORITY);
            res[c].assign(len, 0.0);
            for (size_t i = 0; i < len; i++) res[c][i] = std::trunc(y[i + pad]);
        });
    }
    for (int c = 0; c < nch; c++) workers[c].join();
    if (nch == 1) res[1] = mono ? res[0] : std::vector<double>(len, 0.0);
    return finish(res);
}

// ---------------------------------------------------------------- 0x411818: original only

Audio UtagoeRip::run_original_only() {
    Work w(ch_, rate_, cfg_.block_sec() * 2.0);
    Source so(orig_);
    std::vector<double> chunks[2];
    int tick = 0;
    while (so.read(&w.raw_o)) {
        w.deinterleave();
        for (int c = 0; c < 2; c++) chunks[c].insert(chunks[c].end(), w.o[c].begin(), w.o[c].end());
        if (tick++ > 10) {
            progress(std::min(1.0, (double)so.tell() / (double)std::max<long long>(1, (long long)orig_.frames())));
            tick = 0;
        }
        if (cb_.cancel && cb_.cancel()) {
            cancelled_ = true;
            break;
        }
    }
    return finish(chunks);
}

// ---------------------------------------------------------------- Utagoe Rip 4

// The v4 separation aligns, matches levels and EQ and decides per bin on its own, so
// the 3.0 analysis and its settings (intro, time shift, level, phase, processing mode,
// waveform method, oversampling, block length) don't apply. Extractable Level and
// Accuracy Priority do, and so does the post-processing (centralization, filters).
Audio UtagoeRip::run_v4() {
    auto planar = [](const Audio& a) {
        std::vector<std::vector<double>> p(a.channels, std::vector<double>(a.frames()));
        for (size_t i = 0; i < a.frames(); i++)
            for (int c = 0; c < a.channels; c++) p[c][i] = a.data[i * a.channels + c] / 32768.0;
        return p;
    };
    v4::Options opt;
    opt.kvol = cfg_.kvol();
    opt.quality = cfg_.sound_qty == QUALITY_PRIORITY;
    v4::Report rep;
    status(STATUS_ANALYZING);
    bool analysing = true;
    auto y = v4::separate(planar(orig_), planar(*inst_), rate_, opt, &rep, [&](int pct) {
        if (analysing && pct >= 30) {
            analysing = false;
            status(STATUS_NONE);
        }
        progress(pct / 100.0);
        return !(cb_.cancel && cb_.cancel());
    });
    status(STATUS_NONE);
    if (y.empty()) {
        cancelled_ = true;
        Audio empty;
        empty.rate = rate_, empty.channels = ch_;
        return empty;
    }
    wchar_t line[256];
    swprintf(line, 256, L"v4 lag:%ld sign:%d drift:%ls start:%.2f end:%.2f level:%ls gain:%.3f", rep.lag,
             rep.sign, rep.drift_line ? L"line" : L"tracked", rep.lag_start, rep.lag_end,
             rep.level_applied ? L"tracked" : L"fixed", rep.level_gain);
    log(line);
    std::vector<double> x[2];
    for (int c = 0; c < ch_; c++) {
        x[c].resize(y[c].size());
        for (size_t i = 0; i < y[c].size(); i++) x[c][i] = y[c][i] * 32768.0;
    }
    return finish(x);
}

// ---------------------------------------------------------------- post-processing

Audio UtagoeRip::finish(std::vector<double> x[2]) {
    const Settings& cfg = cfg_;
    size_t len = x[0].size();
    x[1].resize(len, 0.0);
    if (cfg.cntr_flag && ch_ >= 2) {
        size_t pad = original_timing_ ? 0 : LATENCY;
        x[0].resize(len + pad, 0.0);
        x[1].resize(len + pad, 0.0);
        std::vector<double> l, r;
        center_focus(x[0].data(), x[1].data(), len + pad, cfg.cntr_strength(), &l, &r);
        for (size_t i = 0; i < len; i++) x[0][i] = std::trunc(l[i + pad]), x[1][i] = std::trunc(r[i + pad]);
        x[0].resize(len);
        x[1].resize(len);
    }
    const struct { bool on; FIRFilter::Kind kind; int hz; } filters[2] = {
        {cfg.lpf_flag, FIRFilter::LOWPASS, cfg.lpf_hz()}, {cfg.hpf_flag, FIRFilter::HIGHPASS, cfg.hpf_hz()}};
    for (const auto& f : filters) {
        if (!f.on) continue;
        FIRFilter fir(rate_, f.kind, f.hz);
        size_t pad = original_timing_ ? 0 : fir.delay();
        x[0].resize(len + pad, 0.0);
        x[1].resize(len + pad, 0.0);
        fir.apply(&x[0], &x[1]);
        for (int c = 0; c < 2; c++) {
            x[c].erase(x[c].begin(), x[c].begin() + pad);
            x[c].resize(len);
        }
    }
    Audio out;
    out.rate = rate_;
    out.channels = ch_;
    size_t n = orig_.frames();
    out.data.assign(n * ch_, 0);
    for (size_t i = 0; i < n && i < len; i++)
        for (int c = 0; c < ch_; c++) out.data[i * ch_ + c] = soft_clip((long long)x[c][i]);
    return out;
}

}  // namespace utagoe
